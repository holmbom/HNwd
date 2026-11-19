// Hjälp för att bygga syntetiska strömmar och filer i testerna, och för att
// räkna fel.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "inflate.h"

// Räknar ett fel och skriver var, om villkoret inte håller. Kräver en lokal
// `int fails` i testfunktionen.
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("  FAIL %s:%d — %s\n", __FILE__, __LINE__, #cond);     \
            ++fails;                                                           \
        }                                                                      \
    } while (0)

namespace testdata {

using Bytes = std::vector<uint8_t>;

// Skriver little-endian-värden så som formatet lagrar dem.
struct ByteWriter {
    Bytes data;

    void u8(uint8_t v) { data.push_back(v); }
    void i32(int32_t v) {
        const uint32_t u = static_cast<uint32_t>(v);
        for (int i = 0; i < 4; ++i) data.push_back(static_cast<uint8_t>(u >> (8 * i)));
    }
    void i64(int64_t v) {
        const uint64_t u = static_cast<uint64_t>(v);
        for (int i = 0; i < 8; ++i) data.push_back(static_cast<uint8_t>(u >> (8 * i)));
    }
    void raw(const Bytes& b) { data.insert(data.end(), b.begin(), b.end()); }
    void text(const std::string& s) { data.insert(data.end(), s.begin(), s.end()); }
    void zeros(size_t n) { data.insert(data.end(), n, 0); }
    void pad(size_t align) {
        while (data.size() % align) data.push_back(0);
    }
    // [int32 längd][byte][nollor upp till 4]
    void str(const std::string& s) {
        i32(static_cast<int32_t>(s.size()));
        text(s);
        pad(4);
    }
    // Null-sträng: längden -1, inga byte.
    void null_str() { i32(-1); }

    void f32(float v) {
        uint32_t u;
        std::memcpy(&u, &v, sizeof u);
        i32(static_cast<int32_t>(u));
    }
    // 64-bitarsvärden så som en objektström lagrar dem: med en int32 0 före
    // om markören står på 4 mod 8. i64() ovan skriver utan den.
    void i64a(int64_t v) {
        if (data.size() % 8 == 4) i32(0);
        i64(v);
    }
    void f64(double v) {
        int64_t u;
        std::memcpy(&u, &v, sizeof u);
        i64a(u);
    }
    // Byte ur en hexsträng, "D4D4 00FF"; blanksteg hoppas över.
    void hex(const char* s) {
        auto digit = [](char c) {
            return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
        };
        for (; *s; ++s) {
            if (*s == ' ') continue;
            data.push_back(static_cast<uint8_t>(digit(s[0]) * 16 + digit(s[1])));
            ++s;
        }
    }

    // Pekare i en objektström. Ett nytt objekt är [nästa id][klass] och får
    // sitt id innan kroppen skrivs; id räknas från 100.
    int32_t next_id = 100;
    int32_t obj(int32_t cls) {
        i32(next_id);
        i32(cls);
        return next_id++;
    }
    // Den underförstådda roten: ett id utan klass och kropp.
    int32_t implicit_root() {
        i32(next_id);
        return next_id++;
    }
    void ref(int32_t id) {
        i32(1);
        i32(id);
    }
    void null_ptr() { i32(0); }
};

// En zlib-ström med lagrade block om högst `block` byte. Giltig för vilken
// avkodare som helst, och kräver ingen kodare i testerna.
inline Bytes zlib_store(const Bytes& src, size_t block = 65535) {
    Bytes out = {0x78, 0x01};
    size_t pos = 0;
    do {
        const size_t n = src.size() - pos < block ? src.size() - pos : block;
        const bool last = pos + n == src.size();
        out.push_back(last ? 1 : 0);
        out.push_back(static_cast<uint8_t>(n));
        out.push_back(static_cast<uint8_t>(n >> 8));
        out.push_back(static_cast<uint8_t>(~n));
        out.push_back(static_cast<uint8_t>(~n >> 8));
        out.insert(out.end(), src.begin() + static_cast<std::ptrdiff_t>(pos),
                   src.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
    } while (pos < src.size());
    const uint32_t sum = HNwd::detail::adler32(src.data(), src.size());
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<uint8_t>(sum >> shift));
    return out;
}

// Ett komprimerbart mönster på `n` byte. Samma formel gav facit i
// inflate_vectors.h.
inline Bytes pattern(size_t n) {
    Bytes out(n);
    for (size_t i = 0; i < n; ++i)
        out[i] = static_cast<uint8_t>("ABCDEFGH"[(i * 7 + (i >> 4)) % 8] + (i / 97) % 3);
    return out;
}

// ---------------------------------------------------------------------------
// Hela filer.

inline constexpr char kDataHeader[] = "#LcUStream-V1.1 binary double release C lichunk-008\n";
inline constexpr char kDirHeader[] =
    "#LcUStream-V1.1 binary double release utf8 navisworks:448 lizlib\n";

struct Package {
    std::string name;
    int32_t flags;
    int32_t a, b;
    Bytes bytes;   // som de ligger i filen
};

// En katalogström över paketen, lagda tätt efter varandra från `first_offset`.
inline Bytes directory(const std::vector<Package>& packages, uint64_t first_offset) {
    ByteWriter w;
    w.i32(-1);
    w.i32(static_cast<int32_t>(packages.size()));
    uint64_t offset = first_offset;
    for (const Package& p : packages) {
        w.str(p.name);
        w.i32(p.flags);
        w.pad(8);
        w.i64(static_cast<int64_t>(offset));
        w.i64(static_cast<int64_t>(p.bytes.size()));
        w.i32(p.a);
        w.i32(p.b);
        offset += p.bytes.size();
    }
    return w.data;
}

// En hel fil. `gap` byte skjuts in före katalogsektionen utan att katalogen
// vet om dem, och `with_tail` styr om filen slutar med offset och rubrik.
inline Bytes build_file(const std::vector<Package>& packages, bool with_tail, size_t gap = 0) {
    ByteWriter w;
    w.text(kDataHeader);
    const uint64_t first = w.data.size();
    for (const Package& p : packages) w.raw(p.bytes);
    w.zeros(gap);

    const uint64_t dir_section = w.data.size();
    w.text(kDirHeader);
    w.zeros(7);   // rubriken är 65 byte; strömmen börjar 72 byte in i sektionen
    w.raw(zlib_store(directory(packages, first)));
    if (with_tail) {
        w.i64(0x1122334455667788);
        w.i64(static_cast<int64_t>(dir_section));
        w.text(kDataHeader);
    }
    return w.data;
}

// Byten för ett paket i formen (4, b): prefix, strömmarna, och sist
// innehållsförteckningen. Alla packas som lagrade zlib-strömmar. `b` får
// innehållsförteckningens komprimerade längd.
inline Bytes prefixed(int32_t prefix, const std::vector<Bytes>& streams, const Bytes& toc,
                      int32_t& b) {
    ByteWriter w;
    w.i32(prefix);
    for (const Bytes& s : streams) w.raw(zlib_store(s));
    const Bytes z = zlib_store(toc);
    b = static_cast<int32_t>(z.size());
    w.raw(z);
    return w.data;
}

} // namespace testdata
