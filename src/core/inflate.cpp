#include "inflate.h"

#include <algorithm>
#include <cstring>

namespace HNwd::detail {

uint32_t adler32(const uint8_t* data, size_t size, uint32_t adler) {
    constexpr uint32_t kMod = 65521;
    // Största antalet byte som kan summeras innan a och b måste reduceras för
    // att inte spilla över 32 bitar.
    constexpr size_t kBlock = 5552;
    uint32_t a = adler & 0xFFFF;
    uint32_t b = adler >> 16;
    while (size > 0) {
        const size_t n = std::min(size, kBlock);
        for (size_t i = 0; i < n; ++i) {
            a += data[i];
            b += a;
        }
        a %= kMod;
        b %= kMod;
        data += n;
        size -= n;
    }
    return (b << 16) | a;
}

// Avkodaren som inflate_zlib och InflateStream delar. Den har ett eget namn i
// stället för en anonym namnrymd, eftersom InflateStream::State bär den.
namespace inflate_impl {

// Koder upp till så här många bitar slås upp direkt i en tabell; längre koder
// tar den kanoniska vägen, en längd i taget.
constexpr int kFastBits = 10;
constexpr int kMaxSymbols = 288;
// Den längsta kopian en symbol kan ge, och det längsta avståndet bakåt.
constexpr uint32_t kMaxMatch = 258;
constexpr size_t kHistory = 32768;

uint32_t reverse_bits(uint32_t v, int count) {
    uint32_t r = 0;
    for (int i = 0; i < count; ++i) {
        r = (r << 1) | (v & 1);
        v >>= 1;
    }
    return r;
}

// Bitar läses från den minst signifikanta änden av varje byte, så som DEFLATE
// packar dem.
struct BitReader {
    const uint8_t* p = nullptr;
    const uint8_t* end = nullptr;
    uint64_t buf = 0;
    int count = 0;
    bool overrun = false;   // något försökte läsa bitar efter strömmens slut

    void refill() {
        while (count <= 56 && p < end) {
            buf |= static_cast<uint64_t>(*p++) << count;
            count += 8;
        }
    }
    uint32_t peek(int n) const {
        return static_cast<uint32_t>(buf) & ((1u << n) - 1);
    }
    void consume(int n) {
        if (n > count) {
            overrun = true;
            buf = 0;
            count = 0;
            return;
        }
        buf >>= n;
        count -= n;
    }
    // Högst 16 bitar åt gången.
    uint32_t take(int n) {
        refill();
        const uint32_t v = peek(n);
        consume(n);
        return v;
    }
    void align_to_byte() {
        consume(count & 7);
    }
};

// En kanonisk Huffman-kod byggd ur kodlängder (RFC 1951 §3.2.2).
struct Huffman {
    uint16_t fast[1 << kFastBits];   // (längd << 9) | symbol, 0 = ingen träff
    uint16_t first_code[17];
    uint16_t first_symbol[17];
    uint32_t max_code[18];           // övre gräns per längd, vänsterställd i 16 bitar
    uint8_t  size[kMaxSymbols];
    uint16_t value[kMaxSymbols];

    // Vad en kod används till avgör om den får vara ofullständig, alltså
    // lämna bitmönster utan symbol.
    enum class Use {
        Fixed,         // de fasta koderna: prövas inte
        CodeLengths,   // koden för kodlängder: ska vara fullständig
        Symbols,       // litteraler/längder och avstånd: fullständig, eller en enda kod på en bit
    };

    // false om längderna beskriver fler koder än som ryms (överbokad kod),
    // eller färre än användningen tillåter. En kod helt utan symboler byggs:
    // den ger fel först när något försöker avkodas med den.
    bool build(const uint8_t* lengths, int n, Use use) {
        int sizes[17] = {};
        std::memset(fast, 0, sizeof fast);
        std::memset(size, 0, sizeof size);
        for (int i = 0; i < n; ++i) ++sizes[lengths[i]];
        sizes[0] = 0;

        int left = 1;      // lediga bitmönster på den aktuella längden
        int longest = 0;
        for (int i = 1; i < 16; ++i) {
            left = left * 2 - sizes[i];
            if (left < 0) return false;
            if (sizes[i]) longest = i;
        }
        if (left > 0 && longest != 0 && use != Use::Fixed &&
            (use == Use::CodeLengths || longest != 1))
            return false;

        uint32_t next_code[16];
        uint32_t code = 0;
        int symbol = 0;
        for (int i = 1; i < 16; ++i) {
            next_code[i] = code;
            first_code[i] = static_cast<uint16_t>(code);
            first_symbol[i] = static_cast<uint16_t>(symbol);
            code += static_cast<uint32_t>(sizes[i]);
            if (sizes[i] && code - 1 >= (1u << i)) return false;
            max_code[i] = code << (16 - i);
            code <<= 1;
            symbol += sizes[i];
        }
        max_code[16] = 0x10000;
        max_code[17] = 0x10000;

        for (int i = 0; i < n; ++i) {
            const int s = lengths[i];
            if (!s) continue;
            const int c = static_cast<int>(next_code[s]) - first_code[s] + first_symbol[s];
            size[c] = static_cast<uint8_t>(s);
            value[c] = static_cast<uint16_t>(i);
            if (s <= kFastBits) {
                const uint16_t entry = static_cast<uint16_t>((s << 9) | i);
                for (uint32_t j = reverse_bits(next_code[s], s); j < (1u << kFastBits);
                     j += (1u << s))
                    fast[j] = entry;
            }
            ++next_code[s];
        }
        return true;
    }

    // Nästa symbol, eller -1 om bitarna inte är en kod. Bufferten ska vara
    // påfylld.
    int decode(BitReader& b) const {
        const uint32_t bits = b.peek(16);
        if (const uint16_t f = fast[bits & ((1u << kFastBits) - 1)]) {
            b.consume(f >> 9);
            return f & 511;
        }
        const uint32_t k = reverse_bits(bits, 16);
        int s = kFastBits + 1;
        while (k >= max_code[s]) ++s;
        if (s >= 16) return -1;
        const int c = static_cast<int>(k >> (16 - s)) - first_code[s] + first_symbol[s];
        if (c < 0 || c >= kMaxSymbols || size[c] != s) return -1;
        b.consume(s);
        return value[c];
    }
};

constexpr uint16_t kLengthBase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLengthExtra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistExtra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// zlib-huvudet: metod 8, fönster högst 32 KB, kontrollbitarna, ingen
// förinställd ordlista.
bool header_ok(const uint8_t* src) {
    const uint32_t cmf = src[0];
    const uint32_t flg = src[1];
    return (cmf & 0x0F) == 8 && (cmf >> 4) <= 7 && ((cmf << 8) | flg) % 31 == 0 &&
           (flg & 0x20) == 0;
}

// Utdata: `data[0, size)` är skrivet, och avkodaren skriver vidare upp till
// `cap`. De sista min(utdata hittills, 32 KB) byten före `size` ska finnas i
// bufferten, eftersom kopiorna läser bakåt i dem.
struct OutBuf {
    uint8_t* data;
    size_t size;
    size_t cap;
};

enum class Step {
    NeedSpace,   // bufferten är full; fortsätt med mer plats
    Boundary,    // ett block är slut och nästa har inte börjat
    Done,        // det sista blocket är slut och kontrollsumman är läst
    Corrupt,
    TooLarge,
    Next,        // bara internt: fortsätt i nästa fas
};

// DEFLATE-avkodningen med sitt läge mellan anropen, så att den kan stanna när
// bufferten är full och fortsätta där den slutade: mitt i ett lagrat block,
// eller mellan två symboler i ett komprimerat (en kopia som inte ryms sparas
// och skrivs först i nästa anrop).
//
// Taket prövas mot den totala utdatan, inte mot bufferten: en symbol eller ett
// lagrat block som skulle gå över taket ger TooLarge, innan något av det
// skrivs. inflate_zlib() och InflateStream har därmed samma tak.
class Inflater {
public:
    // Börjar vid ett blockhuvud på bit `in_bit` i `src`, med `total_out` byte
    // utdata före.
    void start(const uint8_t* src, size_t size, uint64_t in_bit, uint64_t total_out,
               uint64_t limit) {
        src_ = src;
        b_ = BitReader{};
        b_.p = src + static_cast<size_t>(in_bit / 8);
        b_.end = src + size;
        b_.refill();
        b_.consume(static_cast<int>(in_bit % 8));
        phase_ = Phase::BlockHeader;
        final_ = false;
        boundary_reported_ = false;
        pend_len_ = 0;
        total_ = total_out;
        limit_ = limit;
        reason_ = "";
    }

    // Avkodar in i `out` tills bufferten är full, strömmen är slut eller ett
    // fel. Med `stop_at_boundaries` stannar den också vid varje blockgräns.
    Step run(OutBuf& out, bool stop_at_boundaries) {
        const size_t pos0 = out.size;
        bias_ = total_ - pos0;
        const Step s = run_phases(out, stop_at_boundaries);
        total_ += out.size - pos0;
        return s;
    }

    uint64_t total_out() const { return total_; }
    // Biten där avkodningen står: vid Boundary nästa blockhuvud.
    uint64_t in_bit() const {
        return static_cast<uint64_t>(b_.p - src_) * 8 - static_cast<uint64_t>(b_.count);
    }
    // Efter Done: kontrollsummans plats i `src` och dess värde.
    size_t trailer() const { return trailer_; }
    uint32_t stored_adler() const { return stored_adler_; }
    // Efter Corrupt: vilken regel som bröts.
    const char* reason() const { return reason_; }

private:
    enum class Phase { BlockHeader, Stored, Codes, Done };

    Step corrupt(const char* why) {
        reason_ = why;
        return Step::Corrupt;
    }
    Step too_large() {
        reason_ = "output passes the limit";
        return Step::TooLarge;
    }

    Step run_phases(OutBuf& out, bool stop) {
        for (;;) {
            Step s = Step::Next;
            switch (phase_) {
                case Phase::Done:        return Step::Done;
                case Phase::BlockHeader: s = block_header(out, stop); break;
                case Phase::Stored:      s = stored(out); break;
                case Phase::Codes:       s = codes(out); break;
            }
            if (s != Step::Next) return s;
        }
    }

    Step block_header(OutBuf& out, bool stop) {
        if (final_) return trailer_step();
        if (stop && !boundary_reported_) {
            boundary_reported_ = true;
            return Step::Boundary;
        }
        boundary_reported_ = false;
        final_ = b_.take(1) != 0;
        const uint32_t type = b_.take(2);
        if (b_.overrun) return corrupt("block header passes the end of the data");
        switch (type) {
            case 0:  return begin_stored(out);
            case 1:  return begin_fixed();
            case 2:  return begin_dynamic();
            default: return corrupt("block type 3");
        }
    }

    // Bitar som blev över i den sista byten kastas, och hela byte som redan
    // dragits in i bufferten lämnas tillbaka: kontrollsumman börjar på nästa
    // bytegräns.
    Step trailer_step() {
        if (b_.overrun) return corrupt("data passes the end of the stream");
        b_.align_to_byte();
        const uint8_t* tail = b_.p - b_.count / 8;
        if (b_.end - tail < 4) return corrupt("checksum passes the end of the data");
        stored_adler_ = (static_cast<uint32_t>(tail[0]) << 24) |
                        (static_cast<uint32_t>(tail[1]) << 16) |
                        (static_cast<uint32_t>(tail[2]) << 8) |
                        static_cast<uint32_t>(tail[3]);
        trailer_ = static_cast<size_t>(tail - src_);
        phase_ = Phase::Done;
        return Step::Done;
    }

    Step begin_stored(const OutBuf& out) {
        b_.align_to_byte();
        const uint32_t len = b_.take(16);
        const uint32_t nlen = b_.take(16);
        if (b_.overrun || (len ^ 0xFFFF) != nlen)
            return corrupt("stored block length does not match its complement");
        if (len > limit_ - (out.size + bias_)) return too_large();
        // Byten i bitbufferten (hela byte efter justeringen) och resten av källan.
        const size_t avail = static_cast<size_t>(b_.count / 8) + static_cast<size_t>(b_.end - b_.p);
        if (avail < len) return corrupt("stored block passes the end of the data");
        stored_left_ = len;
        phase_ = Phase::Stored;
        return Step::Next;
    }

    Step stored(OutBuf& out) {
        while (stored_left_ > 0) {
            const size_t space = out.cap - out.size;
            if (space == 0) return Step::NeedSpace;
            uint8_t* dst = out.data + out.size;
            const size_t k = std::min<size_t>(space, stored_left_);
            size_t done = 0;
            // Först det som redan ligger i bitbufferten, sedan resten rakt ur källan.
            while (done < k && b_.count >= 8) {
                dst[done++] = static_cast<uint8_t>(b_.peek(8));
                b_.consume(8);
            }
            if (done < k) {
                std::memcpy(dst + done, b_.p, k - done);
                b_.p += k - done;
            }
            out.size += k;
            stored_left_ -= static_cast<uint32_t>(k);
        }
        phase_ = Phase::BlockHeader;
        return Step::Next;
    }

    Step begin_fixed() {
        uint8_t lengths[288];
        int i = 0;
        for (; i < 144; ++i) lengths[i] = 8;
        for (; i < 256; ++i) lengths[i] = 9;
        for (; i < 280; ++i) lengths[i] = 7;
        for (; i < 288; ++i) lengths[i] = 8;
        uint8_t dist_lengths[30];
        std::memset(dist_lengths, 5, sizeof dist_lengths);
        if (!lit_.build(lengths, 288, Huffman::Use::Fixed) ||
            !dist_.build(dist_lengths, 30, Huffman::Use::Fixed))
            return corrupt("fixed codes");
        phase_ = Phase::Codes;
        return Step::Next;
    }

    Step begin_dynamic() {
        static constexpr uint8_t kOrder[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                               11, 4,  12, 3, 13, 2, 14, 1, 15};
        const int hlit = static_cast<int>(b_.take(5)) + 257;
        const int hdist = static_cast<int>(b_.take(5)) + 1;
        const int hclen = static_cast<int>(b_.take(4)) + 4;
        if (b_.overrun || hlit > 286 || hdist > 30)
            return corrupt("dynamic block header is out of range");

        uint8_t code_lengths[19] = {};
        for (int i = 0; i < hclen; ++i)
            code_lengths[kOrder[i]] = static_cast<uint8_t>(b_.take(3));
        Huffman code;
        if (b_.overrun || !code.build(code_lengths, 19, Huffman::Use::CodeLengths))
            return corrupt("code-length code is not a complete code");

        uint8_t lengths[286 + 30] = {};
        const int total = hlit + hdist;
        int n = 0;
        while (n < total) {
            b_.refill();
            const int sym = code.decode(b_);
            if (sym < 0 || b_.overrun) return corrupt("invalid code-length code");
            if (sym < 16) {
                lengths[n++] = static_cast<uint8_t>(sym);
                continue;
            }
            uint8_t fill = 0;
            int repeat;
            if (sym == 16) {
                if (n == 0) return corrupt("code-length repeat without a previous length");
                fill = lengths[n - 1];
                repeat = 3 + static_cast<int>(b_.take(2));
            } else if (sym == 17) {
                repeat = 3 + static_cast<int>(b_.take(3));
            } else {
                repeat = 11 + static_cast<int>(b_.take(7));
            }
            if (b_.overrun || repeat > total - n)
                return corrupt("code-length repeat passes the end of the lengths");
            std::memset(lengths + n, fill, static_cast<size_t>(repeat));
            n += repeat;
        }
        // Utan en kod för blockslut kan blocket aldrig avslutas.
        if (lengths[256] == 0) return corrupt("no code for the end of the block");

        if (!lit_.build(lengths, hlit, Huffman::Use::Symbols) ||
            !dist_.build(lengths + hlit, hdist, Huffman::Use::Symbols))
            return corrupt("literal/length or distance code is not a valid code");
        phase_ = Phase::Codes;
        return Step::Next;
    }

    // Kopierar `length` byte från `distance` bakåt till `dst`.
    static void copy_match(uint8_t* dst, size_t distance, uint32_t length) {
        const uint8_t* from = dst - distance;
        if (distance >= length) {
            std::memcpy(dst, from, length);
        } else {
            // Källan och målet överlappar: kopian ska se det den själv skrev.
            for (uint32_t i = 0; i < length; ++i) dst[i] = from[i];
        }
    }

    Step codes(OutBuf& out) {
        uint8_t* const data = out.data;
        size_t pos = out.size;
        // Avkodningen skriver fritt fram till `stop`: bufferten eller taket,
        // det som kommer först. Utdatan totalt är pos + bias_.
        const uint64_t limit_end = limit_ - bias_;
        const size_t stop = static_cast<size_t>(std::min<uint64_t>(out.cap, limit_end));

        // En symbol som inte rymdes förra gången.
        if (pend_len_ > 0) {
            if (pend_len_ > out.cap - pos) return Step::NeedSpace;
            if (pend_dist_ == 0) {
                data[pos] = pend_lit_;
            } else {
                copy_match(data + pos, pend_dist_, pend_len_);
            }
            pos += pend_len_;
            pend_len_ = 0;
        }

        for (;;) {
            b_.refill();
            const int sym = lit_.decode(b_);
            if (sym < 0 || b_.overrun) {
                out.size = pos;
                return corrupt("invalid literal/length code");
            }
            if (sym < 256) {
                if (pos >= stop) {
                    out.size = pos;
                    if (pos + bias_ >= limit_) return too_large();
                    pend_len_ = 1;
                    pend_dist_ = 0;
                    pend_lit_ = static_cast<uint8_t>(sym);
                    return Step::NeedSpace;
                }
                data[pos++] = static_cast<uint8_t>(sym);
                continue;
            }
            if (sym == 256) {
                out.size = pos;
                phase_ = Phase::BlockHeader;
                return Step::Next;
            }
            if (sym > 285) {
                out.size = pos;
                return corrupt("length code 286 or 287");
            }

            // Längd och avstånd tar högst 5 + 15 + 13 bitar efter symbolen, och det
            // ryms i bufferten efter påfyllningen ovan.
            const int li = sym - 257;
            const uint32_t length = kLengthBase[li] + b_.peek(kLengthExtra[li]);
            b_.consume(kLengthExtra[li]);

            const int ds = dist_.decode(b_);
            if (ds < 0 || ds > 29) {
                out.size = pos;
                return corrupt("invalid distance code");
            }
            b_.refill();
            const size_t distance = kDistBase[ds] + b_.peek(kDistExtra[ds]);
            b_.consume(kDistExtra[ds]);
            if (b_.overrun) {
                out.size = pos;
                return corrupt("code passes the end of the data");
            }
            if (distance > pos + bias_) {
                out.size = pos;
                return corrupt("distance reaches before the start of the output");
            }

            if (length > stop - std::min(stop, pos)) {
                out.size = pos;
                if (length > limit_ - (pos + bias_)) return too_large();
                pend_len_ = length;
                pend_dist_ = static_cast<uint32_t>(distance);
                return Step::NeedSpace;
            }
            copy_match(data + pos, distance, length);
            pos += length;
        }
    }

    const uint8_t* src_ = nullptr;
    BitReader b_;
    Huffman lit_, dist_;
    Phase phase_ = Phase::BlockHeader;
    uint64_t total_ = 0;      // utdata före det pågående anropet
    uint64_t limit_ = 0;
    uint64_t bias_ = 0;       // total utdata minus positionen i bufferten, under ett anrop
    uint32_t stored_left_ = 0;
    uint32_t pend_len_ = 0;   // en sparad symbol: längd, avstånd (0 = litteral) och byte
    uint32_t pend_dist_ = 0;
    uint8_t pend_lit_ = 0;
    bool final_ = false;
    bool boundary_reported_ = false;
    const char* reason_ = "";
    size_t trailer_ = 0;
    uint32_t stored_adler_ = 0;
};

} // namespace inflate_impl

using inflate_impl::Inflater;
using inflate_impl::OutBuf;
using inflate_impl::Step;
using inflate_impl::kHistory;
using inflate_impl::kMaxMatch;

ErrorCode inflate_zlib(const uint8_t* src, size_t src_size, std::vector<uint8_t>& out,
                       size_t max_output, size_t* consumed) {
    // Två byte huvud och fyra byte kontrollsumma är det minsta en ström kan vara.
    if (!src || src_size < 6) return ErrorCode::CorruptStream;
    if (!inflate_impl::header_ok(src)) return ErrorCode::CorruptStream;

    // Avkodaren är några KB, och anroparens stack räcker gott till den.
    Inflater inf;
    inf.start(src, src_size, 16, 0, max_output);

    out.clear();
    // En första gissning på storleken, så att små strömmar slipper växa. Den
    // är liten och oberoende av hur mycket som följer efter strömmen: en
    // ström som är trasig redan i sitt första block ska inte kosta en stor
    // nollställd buffert.
    out.resize(std::min<size_t>(max_output, std::min<size_t>(
                                                std::max<size_t>(src_size * 4, 4096), 65536)));
    OutBuf ob{out.data(), 0, out.size()};
    for (;;) {
        const Step s = inf.run(ob, false);
        if (s == Step::Done) break;
        if (s == Step::TooLarge) return ErrorCode::StreamTooLarge;
        if (s != Step::NeedSpace) return ErrorCode::CorruptStream;
        // Vektorn växer i steg. Avkodaren har redan prövat taket, så det som
        // behövs ryms under det.
        size_t grown = std::max<size_t>(out.size() * 2, 4096);
        grown = std::min(std::max<size_t>(grown, ob.size + kMaxMatch), max_output);
        if (grown <= out.size()) return ErrorCode::StreamTooLarge;
        out.resize(grown);
        ob.data = out.data();
        ob.cap = out.size();
    }

    out.resize(ob.size);
    if (inf.stored_adler() != adler32(out.data(), out.size())) return ErrorCode::CorruptStream;
    if (consumed) *consumed = inf.trailer() + 4;
    return ErrorCode::Ok;
}

// ---------------------------------------------------------------------------
// InflateStream

struct InflateStream::State {
    // Bufferten: 32 KB historia för kopiorna och plats för det som packas upp
    // i ett svep.
    static constexpr size_t kBuffer = kHistory + (size_t(256) << 10);

    const uint8_t* src = nullptr;
    size_t size = 0;
    uint64_t max_output = 0;
    bool opened = false;

    Inflater inf;
    std::vector<uint8_t> buf;   // buf[i] är utdatabyte base + i
    size_t filled = 0;          // uppackat i bufferten
    size_t rd = 0;              // lämnat till anroparen
    uint64_t base = 0;

    bool sequential = false;    // passet började vid open(): Adler-32 räknas
    uint32_t adler = 1;
    bool done = false;
    size_t consumed = 0;
    ErrorCode error = ErrorCode::CorruptStream;
    std::string detail = "no stream is open";

    uint64_t every = 0;
    std::vector<Checkpoint>* sink = nullptr;
    uint64_t last_checkpoint = 0;

    void fail(ErrorCode e, const std::string& rule) {
        error = e;
        detail = "zlib stream at input byte " + std::to_string(inf.in_bit() / 8) +
                 ", output byte " + std::to_string(inf.total_out()) + ": " + rule;
    }

    // Packar upp mer när allt i bufferten är lämnat. Behåller de sista 32 KB
    // som historia.
    void fill() {
        if (buf.size() - filled < kBuffer - kHistory) {
            const size_t keep = std::min(filled, kHistory);
            std::memmove(buf.data(), buf.data() + (filled - keep), keep);
            base += filled - keep;
            filled = rd = keep;
        }
        OutBuf ob{buf.data(), filled, buf.size()};
        const bool stop = sequential && sink != nullptr;
        for (;;) {
            const size_t before = ob.size;
            const Step s = inf.run(ob, stop);
            if (sequential) adler = adler32(buf.data() + before, ob.size - before, adler);
            filled = ob.size;
            switch (s) {
                case Step::Boundary: {
                    const uint64_t at = inf.total_out();
                    if (at > last_checkpoint && at - last_checkpoint >= every) {
                        const size_t k = static_cast<size_t>(std::min<uint64_t>(at, kHistory));
                        Checkpoint cp;
                        cp.out_pos = at;
                        cp.in_bit = inf.in_bit();
                        cp.window.assign(buf.data() + (filled - k), buf.data() + filled);
                        sink->push_back(std::move(cp));
                        last_checkpoint = at;
                    }
                    continue;
                }
                case Step::NeedSpace:
                    return;
                case Step::Done:
                    done = true;
                    consumed = inf.trailer() + 4;
                    if (sequential && inf.stored_adler() != adler) {
                        done = false;
                        fail(ErrorCode::CorruptStream, "Adler-32 does not match");
                    }
                    return;
                case Step::TooLarge:
                    fail(ErrorCode::StreamTooLarge,
                         "output passes the limit of " + std::to_string(max_output) + " bytes");
                    return;
                case Step::Corrupt:
                case Step::Next:
                    fail(ErrorCode::CorruptStream, inf.reason());
                    return;
            }
        }
    }

    // Lämnar upp till `n` byte; `dst` null hoppar över dem.
    ErrorCode deliver(uint8_t* dst, uint64_t n, uint64_t& got, std::string* out_detail) {
        got = 0;
        while (got < n) {
            if (rd < filled) {
                const size_t k = static_cast<size_t>(std::min<uint64_t>(n - got, filled - rd));
                if (dst) std::memcpy(dst + got, buf.data() + rd, k);
                rd += k;
                got += k;
                continue;
            }
            if (error != ErrorCode::Ok) {
                if (out_detail) *out_detail = detail;
                return error;
            }
            if (done) break;
            fill();
        }
        return ErrorCode::Ok;
    }
};

InflateStream::InflateStream() : s_(std::make_unique<State>()) {}
InflateStream::~InflateStream() = default;

ErrorCode InflateStream::open(const uint8_t* src, size_t size, uint64_t max_output) {
    State& s = *s_;
    s.src = src;
    s.size = size;
    s.max_output = max_output;
    s.opened = false;
    s.filled = s.rd = 0;
    s.base = 0;
    s.sequential = true;
    s.adler = 1;
    s.done = false;
    s.consumed = 0;
    s.error = ErrorCode::Ok;
    s.detail.clear();
    s.every = 0;
    s.sink = nullptr;
    s.last_checkpoint = 0;

    if (!src || size < 6) {
        s.error = ErrorCode::CorruptStream;
        s.detail = "zlib stream is shorter than 6 bytes";
        return s.error;
    }
    if (!inflate_impl::header_ok(src)) {
        s.error = ErrorCode::CorruptStream;
        s.detail = "zlib header is not deflate with valid check bits and no dictionary";
        return s.error;
    }
    if (s.buf.size() != State::kBuffer) s.buf.assign(State::kBuffer, 0);
    s.inf.start(src, size, 16, 0, max_output);
    s.opened = true;
    return ErrorCode::Ok;
}

ErrorCode InflateStream::read(uint8_t* dst, size_t n, size_t& got, std::string* detail) {
    uint64_t g = 0;
    const ErrorCode e = s_->deliver(dst, n, g, detail);
    got = static_cast<size_t>(g);
    return e;
}

uint64_t InflateStream::position() const {
    return s_->base + s_->rd;
}

void InflateStream::record_checkpoints(uint64_t every_bytes, std::vector<Checkpoint>* sink) {
    s_->every = every_bytes;
    s_->sink = sink;
}

ErrorCode InflateStream::resume(const Checkpoint& cp) {
    State& s = *s_;
    if (!s.opened) return ErrorCode::CorruptStream;
    const size_t window = static_cast<size_t>(std::min<uint64_t>(cp.out_pos, kHistory));
    if (cp.in_bit < 16 || cp.in_bit / 8 > s.size || cp.out_pos > s.max_output ||
        cp.window.size() != window)
        return ErrorCode::OutOfRange;

    std::memcpy(s.buf.data(), cp.window.data(), window);
    s.filled = s.rd = window;
    s.base = cp.out_pos - window;
    s.sequential = false;
    s.done = false;
    s.consumed = 0;
    s.error = ErrorCode::Ok;
    s.detail.clear();
    s.inf.start(s.src, s.size, cp.in_bit, cp.out_pos, s.max_output);
    return ErrorCode::Ok;
}

ErrorCode InflateStream::skip(uint64_t n, std::string* detail) {
    uint64_t got = 0;
    const ErrorCode e = s_->deliver(nullptr, n, got, detail);
    if (e != ErrorCode::Ok) return e;
    if (got < n) {
        if (detail)
            *detail = "zlib stream: skip of " + std::to_string(n) + " bytes passes the end at output byte " +
                      std::to_string(position());
        return ErrorCode::OutOfRange;
    }
    return ErrorCode::Ok;
}

bool InflateStream::finished() const {
    return s_->done && s_->error == ErrorCode::Ok;
}

size_t InflateStream::consumed() const {
    return s_->consumed;
}

uint64_t InflateStream::max_output() const {
    return s_->max_output;
}

} // namespace HNwd::detail
