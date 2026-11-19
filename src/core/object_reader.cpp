#include "object_reader.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace HNwd::detail {

namespace {

bool allows(std::initializer_list<int32_t> allowed, int32_t cls) {
    return std::find(allowed.begin(), allowed.end(), cls) != allowed.end();
}

constexpr int32_t kFirstId = 100;
// skip_padded genom ett fönster går fram i steg om så här många byte, så att
// en lång inbäddad fil aldrig behöver ligga i fönstret på en gång.
constexpr size_t kSkipStep = size_t(64) << 10;

} // namespace

ObjectReader::ObjectReader(const uint8_t* data, size_t size, const char* what)
    : reader_(data, size), what_(what ? what : "") {}

ObjectReader::ObjectReader(StreamWindow& window, const char* what)
    : reader_(window), what_(what ? what : "") {}

bool ObjectReader::fail_at(size_t pos, ErrorCode code, const std::string& rule) {
    if (ok_) {
        ok_ = false;
        code_ = code;
        detail_ = std::string(what_) + " at byte " + std::to_string(pos) + ": " + rule;
    }
    reader_.fail();
    return false;
}

bool ObjectReader::fail(const char* rule) {
    return fail_at(reader_.pos(), ErrorCode::CorruptObject, rule);
}

bool ObjectReader::fail_limit(const char* rule) {
    return fail_at(reader_.pos(), ErrorCode::LimitExceeded, rule);
}

bool ObjectReader::short_data(size_t pos, const char* rule) {
    const StreamWindow* w = reader_.window();
    if (w && w->error() != ErrorCode::Ok) return fail_at(pos, w->error(), w->detail());
    return fail_at(pos, ErrorCode::CorruptObject, rule);
}

bool ObjectReader::i32(int32_t& v) {
    if (!ok_) return false;
    if (!reader_.available(4)) return short_data(reader_.pos(), "i32 passes the end of the data");
    return reader_.i32(v);
}

bool ObjectReader::f32(float& v) {
    if (!ok_) return false;
    if (!reader_.available(4)) return short_data(reader_.pos(), "f32 passes the end of the data");
    return reader_.f32(v);
}

bool ObjectReader::i64(int64_t& v) {
    if (!ok_) return false;
    // En 64-bitarsläsning ligger på jämn 8 byte: på 4 mod 8 kommer först en
    // int32 som ska vara 0.
    if (reader_.pos() % 8 == 4) {
        int32_t pad;
        if (!i32(pad)) return false;
        if (pad != 0) return fail("64-bit alignment pad is not zero");
    }
    if (!reader_.available(8))
        return short_data(reader_.pos(), "64-bit value passes the end of the data");
    return reader_.i64(v);
}

bool ObjectReader::f64(double& v) {
    int64_t bits;
    if (!i64(bits)) return false;
    std::memcpy(&v, &bits, sizeof v);
    return true;
}

bool ObjectReader::str(std::string& s, bool* is_null) {
    s.clear();
    int32_t len;
    if (!i32(len)) return false;
    if (is_null) *is_null = (len == -1);
    if (len == -1) return true;
    if (len < 0) return fail("string length is negative");

    const size_t n = static_cast<size_t>(len);
    const size_t padded = (n + 3) & ~size_t(3);
    // padded >= n. Först mot den övre gränsen, så att en orimlig längd inte
    // packar upp något genom ett fönster.
    if (padded > reader_.remaining()) return fail("string passes the end of the data");
    if (!reader_.available(padded))
        return short_data(reader_.pos(), "string passes the end of the data");

    const uint8_t* p = nullptr;
    reader_.bytes(padded, p);
    for (size_t i = n; i < padded; ++i)
        if (p[i] != 0) return fail("string pad is not zero");

    s.assign(reinterpret_cast<const char*>(p), n);
    if (!valid_utf8(p, n)) ++non_utf8_;
    return true;
}

bool ObjectReader::skip_padded(int64_t len) {
    if (!ok_) return false;
    if (len < 0) return fail("byte length is negative");

    // Räknat i 64 bitar utan tecken: len är högst INT64_MAX, så + 3 slår inte runt.
    const uint64_t n = static_cast<uint64_t>(len);
    const uint64_t padded = (n + 3) & ~uint64_t(3);
    if (padded > reader_.remaining()) return fail("byte length passes the end of the data");

    const uint8_t* p = nullptr;
    if (!reader_.window()) {
        reader_.bytes(static_cast<size_t>(padded), p);
        for (uint64_t i = n; i < padded; ++i)
            if (p[i] != 0) return fail("pad after the bytes is not zero");
        return true;
    }

    // Genom ett fönster: byten i steg, sedan utfyllnaden. Ett fel om datan
    // inte räcker anges vid början, som med en buffert.
    const size_t start = reader_.pos();
    for (uint64_t left = n; left > 0;) {
        const size_t k = static_cast<size_t>(std::min<uint64_t>(left, kSkipStep));
        if (!reader_.available(k)) return short_data(start, "byte length passes the end of the data");
        reader_.bytes(k, p);
        left -= k;
    }
    const size_t pad = static_cast<size_t>(padded - n);
    if (!reader_.available(pad)) return short_data(start, "byte length passes the end of the data");
    reader_.bytes(pad, p);
    for (size_t i = 0; i < pad; ++i)
        if (p[i] != 0) return fail("pad after the bytes is not zero");
    return true;
}

bool ObjectReader::count(uint32_t min_bytes_per_item, int32_t& n) {
    if (!i32(n)) return false;
    // Produkten räknas i 64 bitar: n är högst 2^31 och min_bytes högst 2^32.
    if (n < 0 || static_cast<uint64_t>(n) * min_bytes_per_item > reader_.remaining())
        return fail("count is negative or exceeds the remaining bytes");
    return true;
}

bool ObjectReader::implicit_root() {
    int32_t id;
    if (!i32(id)) return false;
    if (id != next_id_) return fail("implicit root id is not the next id");
    slots_.push_back({0, 0, true});   // klass 0, värde 0, färdig
    ++next_id_;
    return true;
}

bool ObjectReader::end() {
    if (!ok_) return false;
    const size_t pos = reader_.pos();
    const size_t aligned = (pos + 7) & ~size_t(7);
    if (aligned < pos) return fail("data does not end at the zero pad to a multiple of 8");
    if (StreamWindow* w = reader_.window()) {
        // Byten fram till `aligned` ska finnas och strömmen sluta just där;
        // ends_at() packar upp till slutet och prövar då kontrollsumman.
        if (!reader_.available(aligned - pos) || !w->ends_at(aligned))
            return short_data(pos, "data does not end at the zero pad to a multiple of 8");
    } else if (reader_.size() != aligned) {
        return fail("data does not end at the zero pad to a multiple of 8");
    }
    const uint8_t* p = nullptr;
    reader_.bytes(aligned - pos, p);
    for (size_t i = 0; i < aligned - pos; ++i)
        if (p[i] != 0) return fail("end pad is not zero");
    return true;
}

bool ObjectReader::ptr(Ptr& out, std::initializer_list<int32_t> allowed, bool allow_null,
                       bool allow_back) {
    out = Ptr{};
    int32_t lead;
    if (!i32(lead)) return false;

    if (lead == 0) {
        if (!allow_null) return fail("null pointer in a slot that needs an object");
        return true;
    }

    if (lead == 1) {
        if (!allow_back) return fail("back-reference in a slot that needs a new object");
        int32_t id;
        if (!i32(id)) return false;
        if (id < kFirstId || id >= next_id_) return fail("back-reference to an unknown id");
        if (id < base_id_) {
            // Före läsarens början: objektet är färdigt, eller okänt.
            int32_t cls = 0;
            uint32_t value = 0;
            if (!resolve_ || !resolve_(id, cls, value)) return fail("back-reference to an unknown id");
            if (!allows(allowed, cls))
                return fail("back-reference to a class that this slot does not allow");
            out.kind = Ptr::Back;
            out.id = id;
            out.cls = cls;
            out.value = value;
            return true;
        }
        const Slot& slot = slots_[static_cast<size_t>(id - base_id_)];
        if (!slot.finished) return fail("back-reference to an unfinished object");
        if (!allows(allowed, static_cast<int32_t>(slot.cls)))
            return fail("back-reference to a class that this slot does not allow");
        out.kind = Ptr::Back;
        out.id = id;
        out.cls = slot.cls;
        out.value = slot.value;
        return true;
    }

    if (lead != next_id_) return fail("pointer is neither 0, 1 nor the next id");

    int32_t cls;
    if (!i32(cls)) return false;
    if (!allows(allowed, cls)) return fail("class that this slot does not allow");
    if (cls < 0 || cls > 255) return fail("class number does not fit in 8 bits");
    if (depth_ >= kMaxNesting) {
        fail("nesting deeper than the limit");
        code_ = ErrorCode::LimitExceeded;
        return false;
    }

    slots_.push_back({0, static_cast<uint8_t>(cls), false});
    ++depth_;
    out.kind = Ptr::New;
    out.id = next_id_++;
    out.cls = cls;
    return true;
}

bool ObjectReader::finish(int32_t id, uint32_t value) {
    if (!ok_) return false;
    if (id < base_id_ || id >= next_id_ || slots_[static_cast<size_t>(id - base_id_)].finished ||
        depth_ <= 0)
        return fail("finish of an unknown or already finished object");
    Slot& slot = slots_[static_cast<size_t>(id - base_id_)];
    slot.finished = true;
    slot.value = value;
    --depth_;
    return true;
}

void ObjectReader::continue_ids(int32_t next_id, Resolver resolve) {
    slots_.clear();
    base_id_ = next_id;
    next_id_ = next_id;
    resolve_ = std::move(resolve);
}

bool valid_utf8(const uint8_t* data, size_t size) {
    size_t i = 0;
    while (i < size) {
        const uint8_t c = data[i];
        size_t extra;
        uint8_t lo = 0x80, hi = 0xBF;   // tillåtet intervall för den första fortsättningsbyten
        if (c < 0x80) {
            ++i;
            continue;
        } else if (c >= 0xC2 && c <= 0xDF) {
            extra = 1;
        } else if (c >= 0xE0 && c <= 0xEF) {
            extra = 2;
            if (c == 0xE0) lo = 0xA0;        // inga överlånga former
            else if (c == 0xED) hi = 0x9F;   // inga surrogat
        } else if (c >= 0xF0 && c <= 0xF4) {
            extra = 3;
            if (c == 0xF0) lo = 0x90;        // inga överlånga former
            else if (c == 0xF4) hi = 0x8F;   // inget över U+10FFFF
        } else {
            return false;
        }
        if (size - i <= extra) return false;
        if (data[i + 1] < lo || data[i + 1] > hi) return false;
        for (size_t k = 2; k <= extra; ++k)
            if ((data[i + k] & 0xC0) != 0x80) return false;
        i += extra + 1;
    }
    return true;
}

bool skip_class185(ObjectReader& r) {
    std::string s;
    int32_t n;
    if (!r.str(s) || !r.str(s) || !r.count(12, n)) return false;
    for (int32_t i = 0; i < n; ++i) {
        int32_t embedded;
        if (!r.str(s) || !r.i32(embedded)) return false;
        if (embedded != 0 && embedded != 1)
            return r.fail("class-185 item flag is neither 0 nor 1");
        if (!r.str(s)) return false;
        if (embedded == 1) {
            int64_t len;
            if (!r.str(s) || !r.i64(len) || !r.skip_padded(len)) return false;
        }
    }
    return true;
}

} // namespace HNwd::detail
