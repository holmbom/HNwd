#pragma once
#include "inflate.h"
#include "nwd_error.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace HNwd::detail {

// Ett glidande fönster över en InflateStream: byte i strömmen nås med sin
// absoluta position (räknad från strömmens början), och fönstret packar upp
// mer när något efterfrågas som ännu inte finns.
//
// Fönstret börjar där strömmen står när det skapas (stream.position()), så det
// fungerar också efter InflateStream::resume(). Det som ligger före
// release_before() får kastas; det som ligger efter ligger kvar tills det
// släpps. Fönstret är minst `min_bytes` stort och växer när en enskild läsning
// behöver mer.
class StreamWindow {
public:
    StreamWindow(InflateStream& stream, size_t min_bytes)
        : stream_(stream),
          min_(std::max<size_t>(min_bytes, 1)),
          begin_(stream.position()),
          end_(begin_),
          release_(begin_) {}

    StreamWindow(const StreamWindow&) = delete;
    StreamWindow& operator=(const StreamWindow&) = delete;

    // Byten [pos, pos + n) finns i fönstret. Packar upp mer och flyttar ner det
    // som inte är släppt. false om strömmen slutar före pos + n, om strömmen
    // är trasig (error() säger hur), eller om pos ligger före det som redan
    // släppts (error() blir då OutOfRange).
    bool ensure(uint64_t pos, size_t n) {
        if (pos < begin_) {
            if (error_ == ErrorCode::Ok) {
                error_ = ErrorCode::OutOfRange;
                detail_ = "byte " + std::to_string(pos) + " is before the window at byte " +
                          std::to_string(begin_);
            }
            return false;
        }
        if (pos <= end_ && n <= end_ - pos) return true;
        if (error_ != ErrorCode::Ok || ended_) return false;
        // Längre än taket kan strömmen inte bli. Går begäran förbi taket packas
        // det upp till en byte efter det, så att slutet (eller felet) blir känt
        // utan att mer än begäran läggs i fönstret.
        const uint64_t cap = stream_.max_output();
        uint64_t target;
        if (pos <= cap && n <= cap - pos) {
            target = pos + n;
        } else if (cap < UINT64_MAX && pos <= cap) {
            target = cap + 1;
        } else {
            return false;
        }

        // Det som inte är släppt flyttas ner till buffertens början.
        const uint64_t keep = std::min(release_, pos);
        if (keep > begin_) {
            const size_t drop = static_cast<size_t>(std::min(keep, end_) - begin_);
            std::memmove(buf_.data(), buf_.data() + drop, static_cast<size_t>(end_ - begin_) - drop);
            begin_ += drop;
        }
        const uint64_t need = target - begin_;
        while (end_ - begin_ < need) {
            const size_t have = static_cast<size_t>(end_ - begin_);
            if (have == buf_.size()) {
                // Bufferten fördubblas, men inte förbi det som behövs: ett
                // stort tal ur filen kostar inte mer minne än strömmen har.
                uint64_t grown = std::max<uint64_t>(uint64_t(buf_.size()) * 2, min_);
                grown = std::min<uint64_t>(grown, std::max<uint64_t>(need, min_));
                buf_.resize(static_cast<size_t>(grown));
            }
            size_t got = 0;
            const size_t asked = buf_.size() - have;
            const ErrorCode e = stream_.read(buf_.data() + have, asked, got, &detail_);
            end_ += got;
            if (e != ErrorCode::Ok) {
                // Det som kom före felet går att läsa; felet gäller det som
                // ligger efter.
                error_ = e;
                return pos <= end_ && n <= end_ - pos;
            }
            // InflateStream lämnar färre byte än begärt bara vid slutet, och
            // först när kontrollsumman är prövad.
            if (got < asked) {
                ended_ = true;
                return pos <= end_ && n <= end_ - pos;
            }
        }
        return pos <= end_ && n <= end_ - pos;
    }

    // Byten före `pos` behövs inte längre.
    void release_before(uint64_t pos) {
        if (pos > release_) release_ = pos;
    }

    // Byten på `pos`. Gäller efter ensure(pos, n) och fram till nästa ensure().
    const uint8_t* at(uint64_t pos) const {
        return buf_.data() + static_cast<size_t>(pos - begin_);
    }

    // Strömmen slutar exakt på `pos`: allt fram till pos finns, inget efter,
    // och slutet är prövat. false också vid fel (se error()).
    bool ends_at(uint64_t pos) {
        if (ensure(pos, 1)) return false;
        return error_ == ErrorCode::Ok && ended_ && end_ == pos;
    }

    // Var fönstret började, och hur långt strömmen kan vara: dess längd om
    // slutet är nått, annars taket.
    uint64_t start() const { return start_; }
    uint64_t bound() const { return ended_ ? end_ : stream_.max_output(); }

    // Fel ur strömmen eller ett läsförsök före fönstret; Ok annars.
    ErrorCode error() const { return error_; }
    const std::string& detail() const { return detail_; }

private:
    InflateStream& stream_;
    size_t min_;
    uint64_t begin_;          // strömposition för buf_[0]
    uint64_t end_;            // strömposition efter den sista uppackade byten
    uint64_t release_;
    uint64_t start_ = begin_;
    std::vector<uint8_t> buf_;
    bool ended_ = false;
    ErrorCode error_ = ErrorCode::Ok;
    std::string detail_;
};

// Läser little-endian-värden ur en buffert, med gränskontroll på varje läsning.
//
// Ett misslyckande fastnar: efter den första läsningen utanför bufferten, eller
// den första utfyllnaden som inte är noll, svarar varje senare anrop false och
// ok() är false. En avkodare kan därför läsa en hel post och fråga en gång.
//
// Positioner räknas från buffertens början, och det är den uppackade strömmens
// början: formatets justering till 4 och 8 byte gäller därifrån.
//
// Över ett StreamWindow är positionen absolut i strömmen, så justeringen gäller
// därifrån också då. Läsaren går bara framåt och släpper det den passerat;
// pekaren från bytes() gäller fram till nästa läsning. size() är då strömmens
// längd om slutet är nått och annars taket, alltså en övre gräns.
class ByteReader {
public:
    ByteReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
    explicit ByteReader(StreamWindow& window)
        : data_(nullptr), size_(0), pos_(static_cast<size_t>(window.start())), window_(&window) {}

    size_t pos() const { return pos_; }
    size_t size() const { return window_ ? static_cast<size_t>(window_->bound()) : size_; }
    size_t remaining() const { return size() - pos_; }
    bool ok() const { return ok_; }
    bool at_end() const {
        if (!window_) return ok_ && pos_ == size_;
        return ok_ && window_->ends_at(pos_);
    }
    // Fönstret läsaren går över, eller nullptr för en buffert.
    StreamWindow* window() const { return window_; }

    // `n` byte till finns från positionen. Sätter inget fel.
    bool available(size_t n) {
        if (!ok_) return false;
        if (!window_) return n <= size_ - pos_;
        window_->release_before(pos_);
        return window_->ensure(pos_, n);
    }

    bool u32(uint32_t& v) {
        if (!need(4)) return false;
        const uint8_t* p = cur();
        v = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
            (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
        pos_ += 4;
        return true;
    }
    bool i32(int32_t& v) {
        uint32_t u;
        if (!u32(u)) return false;
        v = static_cast<int32_t>(u);
        return true;
    }
    bool u64(uint64_t& v) {
        uint32_t lo, hi;
        if (!need(8) || !u32(lo) || !u32(hi)) return false;
        v = static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
        return true;
    }
    bool i64(int64_t& v) {
        uint64_t u;
        if (!u64(u)) return false;
        v = static_cast<int64_t>(u);
        return true;
    }
    bool f32(float& v) {
        uint32_t u;
        if (!u32(u)) return false;
        std::memcpy(&v, &u, sizeof v);
        return true;
    }
    bool f64(double& v) {
        uint64_t u;
        if (!u64(u)) return false;
        std::memcpy(&v, &u, sizeof v);
        return true;
    }

    // `n` råa byte. Pekaren gäller så länge bufferten gör det; över ett
    // fönster fram till nästa läsning.
    bool bytes(size_t n, const uint8_t*& p) {
        if (!need(n)) return false;
        p = cur();
        pos_ += n;
        return true;
    }

    // Sträng: [int32 längd][byte][nollor upp till 4]. Längden -1 är en
    // null-sträng utan byte och utan utfyllnad; `is_null` skiljer den från en
    // tom sträng.
    bool str(std::string& s, bool* is_null = nullptr) {
        int32_t len;
        if (!i32(len)) return false;
        if (is_null) *is_null = (len == -1);
        s.clear();
        if (len == -1) return true;
        if (len < 0) return fail();
        const uint8_t* p;
        if (!bytes(static_cast<size_t>(len), p)) return false;
        s.assign(reinterpret_cast<const char*>(p), static_cast<size_t>(len));
        return zero_pad(4);
    }

    // Nollor fram till nästa multipel av `align`.
    bool zero_pad(size_t align) {
        const size_t rest = pos_ % align;
        if (rest == 0) return ok_;
        const size_t n = align - rest;
        if (!need(n)) return false;
        const uint8_t* p = cur();
        for (size_t i = 0; i < n; ++i)
            if (p[i] != 0) return fail();
        pos_ += n;
        return true;
    }

    // Markerar strömmen som feltolkad. Returnerar alltid false.
    bool fail() {
        ok_ = false;
        return false;
    }

private:
    bool need(size_t n) {
        if (!ok_) return fail();
        if (!window_) {
            if (n > size_ - pos_) return fail();
            return true;
        }
        window_->release_before(pos_);
        if (!window_->ensure(pos_, n)) return fail();
        return true;
    }
    const uint8_t* cur() const { return window_ ? window_->at(pos_) : data_ + pos_; }

    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
    StreamWindow* window_ = nullptr;
    bool ok_ = true;
};

} // namespace HNwd::detail
