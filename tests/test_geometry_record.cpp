#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "geometry.h"
#include "test_support.h"

using namespace HNwd::detail;
using testdata::Bytes;
using testdata::ByteWriter;

namespace {

// ---------------------------------------------------------------------------
// Byggstenar för posterna. Kodaren för packade värden är skriven för sig, så
// att den inte delar fel med avkodaren.

Bytes ints(std::initializer_list<int32_t> v) {
    ByteWriter w;
    for (const int32_t x : v) w.i32(x);
    return w.data;
}

Bytes hx(const char* s) {
    ByteWriter w;
    w.hex(s);
    return w.data;
}

Bytes cat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const Bytes& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

// `n` byte med värdet `v`, följda av byten i `tail`.
Bytes run_of(uint8_t v, size_t n, const char* tail) {
    ByteWriter w;
    for (size_t i = 0; i < n; ++i) w.u8(v);
    w.hex(tail);
    return w.data;
}

Bytes word(uint32_t v) { return ints({static_cast<int32_t>(v)}); }

Bytes floats(size_t n) {
    ByteWriter w;
    for (size_t i = 0; i < n; ++i) w.f32(0.5f);
    return w.data;
}

// PACKED(count, width): värde k ligger på bitarna k*width .., mest signifikanta
// biten först, i little-endian-ord.
Bytes packed_words(const std::vector<uint32_t>& values, unsigned width) {
    const uint64_t bits = static_cast<uint64_t>(values.size()) * width;
    std::vector<uint32_t> words(static_cast<size_t>((bits + 31) / 32), 0);
    for (size_t k = 0; k < values.size(); ++k)
        for (unsigned b = 0; b < width; ++b) {
            if (((values[k] >> (width - 1 - b)) & 1u) == 0) continue;
            const uint64_t at = static_cast<uint64_t>(k) * width + b;
            words[static_cast<size_t>(at / 32)] |= 1u << (31 - at % 32);
        }
    ByteWriter w;
    for (const uint32_t x : words) w.i32(static_cast<int32_t>(x));
    return w.data;
}

Bytes head(int32_t cls, int32_t p1, const std::vector<float>& pos) {
    ByteWriter w;
    w.i32(100);
    w.i32(cls);
    w.i32(101);
    w.i32(60);
    w.i32(p1);
    w.i32(static_cast<int32_t>(pos.size()));
    for (const float f : pos) w.f32(f);
    return w.data;
}

// P: [n][bits][n] och längderna.
Bytes prim(const std::vector<uint32_t>& lengths, int32_t bits) {
    const int32_t n = static_cast<int32_t>(lengths.size());
    return cat({ints({n, bits, n}), packed_words(lengths, static_cast<unsigned>(bits))});
}

Bytes norm_slot(int32_t id, int32_t neg_p1, int32_t pal, const Bytes& words, const Bytes& tok) {
    return cat({ints({id, 100, neg_p1, pal}), words, tok});
}

Bytes colour_slot(int32_t id, int32_t cls, int32_t flag, int32_t neg_p1, int32_t cpal,
                  const Bytes& entries, const Bytes& tok) {
    return cat({ints({id, cls, flag, neg_p1, cpal}), entries, tok});
}

Bytes uv_raw(int32_t id, int32_t p1, int32_t fc, size_t floats_written, const Bytes& tok) {
    return cat({ints({id, 61, p1, fc}), floats(floats_written), tok});
}

Bytes uv_quant(int32_t id, int32_t neg_p1, int32_t w, int32_t m, const Bytes& words,
               const Bytes& tok) {
    return cat({ints({id, 61, neg_p1}), floats(4), ints({w, m}), words, tok});
}

// Rå UV med givna tal: fc = antalet tal.
Bytes uv_raw_values(int32_t id, int32_t p1, const std::vector<float>& values, const Bytes& tok) {
    ByteWriter w;
    w.raw(ints({id, 61, p1, static_cast<int32_t>(values.size())}));
    for (const float v : values) w.f32(v);
    return cat({w.data, tok});
}

// Kvantiserad UV med intervallet (umin, vmin, umax, vmax).
Bytes uv_quant_range(int32_t id, int32_t neg_p1, const float range[4], int32_t w, int32_t m,
                     const Bytes& words, const Bytes& tok) {
    ByteWriter b;
    b.raw(ints({id, 61, neg_p1}));
    for (int k = 0; k < 4; ++k) b.f32(range[k]);
    return cat({b.data, ints({w, m}), words, tok});
}

// En post i delar, så att ett test kan byta ut en del. bytes() fyller ut till 8.
struct Rec {
    Bytes head, vmap, uv, normals, colors, prim, sfield;
    Bytes bytes() const {
        ByteWriter w;
        for (const Bytes* p : {&head, &vmap, &uv, &normals, &colors, &prim, &sfield}) w.raw(*p);
        w.pad(8);
        return w.data;
    }
};

const std::vector<float> kQuad = {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0};

// R1: en strip om fyra hörn, alla normaler +Z (axelkod). 120 byte.
Rec r1(int32_t normal_id = 102) {
    Rec r;
    r.head = head(94, 4, kQuad);
    r.uv = ints({0});
    r.normals = cat({ints({normal_id, 100, -4, 0}), hx("D4 D4 D4 D4")});
    r.colors = ints({0});
    r.prim = prim({4}, 4);
    r.sfield = ints({0});
    return r;
}

// R5: som R1 men med en palettpost, giltig bara med åtta bitar. 128 byte.
Rec r5() {
    Rec r = r1();
    r.normals = cat({ints({102, 100, -4, 1}), word(0x7F810000), hx("00 82 00 00")});
    return r;
}

// R16: som R1 men med en palettpost, giltig bara med sexton bitar. 128 byte.
Rec r16() {
    Rec r = r1();
    r.normals = cat({ints({102, 100, -4, 1}), word(0x7FFF0000), word(0), hx("00 82 00 00")});
    return r;
}

// L1: ett linjesegment med färgen 0xFF0000FF. 104 byte.
Rec l1() {
    Rec r;
    r.head = head(95, 2, {0, 0, 0, 10, 0, 0});
    r.uv = ints({0});
    r.normals = ints({0});
    r.colors = colour_slot(102, 58, 5634, -2, 2, hx("FF0000FF FF0000FF"), {});
    r.prim = prim({2}, 2);
    return r;
}

// L1 utan färgplats: linjer får sakna färg. 80 byte.
Rec lines_plain() {
    Rec r = l1();
    r.colors = ints({0});
    return r;
}

// Linjer med två polylinjer och en färg som mappas med tokens.
Rec lines_multi() {
    Rec r;
    r.head = head(95, 5, {0, 0, 0, 1, 0, 0, 2, 0, 0, 3, 0, 0, 4, 0, 0});
    r.uv = ints({0});
    r.normals = ints({0});
    r.colors = colour_slot(102, 58, 5634, -5, 1, hx("FF00FF00"), hx("00 83 00 00"));
    r.prim = prim({2, 3}, 2);
    return r;
}

// En post med allt: VMAP, kvantiserade UV med tokens, normalpalett med axelkoder
// och S > 0. Tre lagrade positioner och fem render-vertexar.
Rec rich() {
    Rec r;
    r.head = head(94, 5, {0, 0, 0, 1, 0, 0, 0, 1, 0});
    r.vmap = hx("02 42 80 00");
    r.uv = uv_quant(102, -5, 0x0404, 3, word(0x12345600), hx("02 42 80 00"));
    r.normals = norm_slot(103, -5, 2, packed_words({0, 127, 0, 0x81, 0, 0}, 8), hx("01 D4 D0 44"));
    r.colors = ints({0});
    r.prim = prim({3, 4}, 3);
    r.sfield = cat({ints({7}), hx("04 44 44 00")});
    return r;
}

// Fyra hörn, en strip om fem hörn via S = 5.
Rec strip5() {
    Rec r = r1();
    r.prim = prim({5}, 3);
    r.sfield = cat({ints({5}), hx("03 42 00 00")});
    return r;
}

struct Parsed {
    ErrorCode code;
    std::string detail;
    RecordData data;
};

Parsed run(const Bytes& b, int bits) {
    Parsed p;
    p.code = parse_record(b.data(), b.size(), bits, p.data, &p.detail);
    return p;
}

bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// Posten ska ge `want` och `detail` ska nämna regeln.
void expect_fail(int& fails, int line, const Bytes& b, int bits, const char* rule,
                 ErrorCode want = ErrorCode::CorruptGeometry) {
    const Parsed p = run(b, bits);
    const bool good = p.code == want && contains(p.detail, rule) &&
                      p.detail.rfind("record at byte ", 0) == 0 && p.data.positions.empty() &&
                      p.data.indices.empty();
    if (!good) {
        std::printf("  FAIL %s:%d — expected '%s', got code %d, detail '%s'\n", __FILE__, line,
                    rule, static_cast<int>(p.code), p.detail.c_str());
        ++fails;
    }
}
#define EXPECT_FAIL(b, bits, rule) expect_fail(fails, __LINE__, (b), (bits), (rule))
#define EXPECT_UNSUPPORTED(b, bits, rule) \
    expect_fail(fails, __LINE__, (b), (bits), (rule), ErrorCode::Unsupported)

bool near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

bool near_all(const std::vector<float>& a, std::initializer_list<float> b) {
    if (a.size() != b.size()) return false;
    size_t i = 0;
    for (const float x : b)
        if (!near(a[i++], x)) return false;
    return true;
}

bool same(const std::vector<uint32_t>& a, std::initializer_list<uint32_t> b) {
    return std::vector<uint32_t>(b) == a;
}

// Det en lyckad tolkning ska uppfylla oavsett indata.
bool sane(const RecordData& d) {
    const size_t nv = d.positions.size() / 3;
    if (d.positions.size() % 3 != 0 || nv == 0) return false;
    for (const uint32_t i : d.indices)
        if (i >= nv) return false;
    if (d.indices.size() != (d.lines ? 2u : 3u) * d.primitives) return false;
    if (d.lines) return (d.colors.empty() || d.colors.size() == nv) && d.normals.empty();
    return d.normals.size() == d.positions.size() && d.colors.empty();
}

// Ändrar ett heltal i en färdig post.
Bytes with_i32(Bytes b, size_t at, int32_t v) {
    const uint32_t u = static_cast<uint32_t>(v);
    if (b.size() < 4 || at > b.size() - 4) return b;   // ett test som träffar utanför ändrar inget
    for (int i = 0; i < 4; ++i) b[at + static_cast<size_t>(i)] = static_cast<uint8_t>(u >> (8 * i));
    return b;
}

struct Tok {
    ErrorCode code;
    std::vector<int32_t> out;
    size_t pos;
    std::string detail;
};

Tok tok(const Bytes& data, uint32_t count, uint32_t fresh, bool axis, size_t start = 0) {
    Tok t;
    t.pos = start;
    t.out = {99, 98};   // ska ersättas
    t.code = decode_tokens(data.data(), data.size(), t.pos, count, fresh, axis, t.out, &t.detail);
    return t;
}

bool tok_ok(const Tok& t, std::initializer_list<int32_t> want, size_t end_pos) {
    return t.code == ErrorCode::Ok && t.out == std::vector<int32_t>(want) && t.pos == end_pos;
}

bool tok_fail(const Tok& t, const char* rule, size_t start = 0) {
    return t.code == ErrorCode::CorruptGeometry && contains(t.detail, rule) &&
           t.detail.rfind("record at byte ", 0) == 0 && t.pos == start && t.out.empty();
}

struct Pk {
    ErrorCode code;
    std::vector<uint32_t> out;
    size_t pos;
    std::string detail;
};

Pk pk(const Bytes& data, uint32_t count, uint32_t width, size_t start = 0) {
    Pk p;
    p.pos = start;
    p.out = {7};
    p.code = decode_packed(data.data(), data.size(), p.pos, count, width, p.out, &p.detail);
    return p;
}

bool pk_fail(const Pk& p, const char* rule, size_t start = 0) {
    return p.code == ErrorCode::CorruptGeometry && contains(p.detail, rule) && p.pos == start &&
           p.out.empty();
}

Bytes toc_bytes(const std::vector<int32_t>& chunks, const std::vector<int32_t>& lengths) {
    ByteWriter w;
    w.i32(static_cast<int32_t>(lengths.size()));
    w.i32(static_cast<int32_t>(chunks.size()));
    for (const int32_t c : chunks) w.i32(c);
    for (const int32_t l : lengths) w.i32(l);
    w.pad(8);
    return w.data;
}

bool toc_fail(const Bytes& b, const char* rule) {
    GeometryToc t;
    std::string detail;
    const ErrorCode c = parse_geometry_toc(b.data(), b.size(), t, &detail);
    return c == ErrorCode::CorruptGeometry && contains(detail, rule) &&
           detail.rfind("geometry TOC at byte ", 0) == 0 && t.chunk_sizes.empty() &&
           t.record_lengths.empty();
}

// ---------------------------------------------------------------------------

void test_tokens(int& fails) {
    // Exemplen ur formatbeskrivningen.
    CHECK(tok_ok(tok(hx("04 80"), 6, 5, false), {0, 1, 2, 3, 4, 4}, 2));
    CHECK(tok_ok(tok(hx("02 42"), 4, 3, false), {0, 1, 2, 1}, 2));
    CHECK(tok_ok(tok(hx("D0 D1 D5"), 3, 0, true), {-1, -2, -6}, 3));

    // Alla ledbytetyper.
    {
        // Körning: 00 ger ett värde, 3F ger 64.
        const Tok t = tok(hx("00 3F"), 65, 65, false);
        CHECK(t.code == ErrorCode::Ok && t.out.size() == 65 && t.pos == 2);
        bool seq = t.out.size() == 65;
        for (size_t i = 0; seq && i < t.out.size(); ++i) seq = t.out[i] == static_cast<int32_t>(i);
        CHECK(seq);
        // Körningen fortsätter från next, inte från förra värdet.
        CHECK(tok_ok(tok(hx("01 41 01"), 5, 4, false), {0, 1, 1, 2, 3}, 3));
        // Kopia: 41 är avstånd 1, 7F är avstånd 63.
        CHECK(tok_ok(tok(hx("02 41 43"), 5, 3, false), {0, 1, 2, 2, 1}, 3));
        const Tok c = tok(run_of(0x3E, 1, "7F"), 64, 63, false);   // 63 värden, sedan avstånd 63
        CHECK(c.code == ErrorCode::Ok && c.out.size() == 64 && c.out.back() == 0);
        // Upprepning: 80 en gång, BF 64 gånger.
        CHECK(tok_ok(tok(hx("00 80"), 2, 1, false), {0, 0}, 2));
        const Tok rep = tok(hx("00 BF"), 65, 1, false);
        CHECK(rep.code == ErrorCode::Ok && rep.out.size() == 65);
        bool all0 = rep.out.size() == 65;
        for (const int32_t v : rep.out) all0 = all0 && v == 0;
        CHECK(all0);
        // Alla sex axelkoder.
        CHECK(tok_ok(tok(hx("D0 D1 D2 D3 D4 D5"), 6, 0, true), {-1, -2, -3, -4, -5, -6}, 6));
        // Axelkoder blandade med vanliga värden.
        CHECK(tok_ok(tok(hx("00 D4 41"), 3, 1, true), {0, -5, -5}, 3));
        // Lång kopia: 4096 värden, sedan avstånd 4095 och 256.
        const Bytes big = run_of(0x3F, 64, "EF FF E1 00");
        const Tok e = tok(big, 4098, 4096, false);
        CHECK(e.code == ErrorCode::Ok && e.out.size() == 4098 && e.pos == big.size());
        if (e.out.size() == 4098) {
            CHECK(e.out[4096] == 1);     // out[4096 - 4095]
            CHECK(e.out[4097] == 3841);  // out[4097 - 256]
        }
        CHECK(tok_ok(tok(hx("03 E0 02"), 5, 4, false), {0, 1, 2, 3, 2}, 3));
        // Literal: strikt under next.
        CHECK(tok_ok(tok(hx("04 F0 00 03"), 6, 5, false), {0, 1, 2, 3, 4, 3}, 4));
        CHECK(tok_ok(tok(hx("00 F0 00 00"), 2, 1, false), {0, 0}, 4));
        const Bytes lit = run_of(0x3F, 5, "F0 01 00");   // 320 värden, sedan literalen 256
        const Tok l = tok(lit, 321, 320, false);
        CHECK(l.code == ErrorCode::Ok && l.out.size() == 321 && l.out.back() == 256);
    }

    // Start mitt i en buffert; pos flyttas bara förbi tokens, inte förbi utfyllnaden.
    {
        const Tok t = tok(hx("EE EE EE EE 02 42 00 00"), 4, 3, false, 4);
        CHECK(tok_ok(t, {0, 1, 2, 1}, 6));
    }
    // Noll värden.
    CHECK(tok_ok(tok({}, 0, 0, false), {}, 0));
    CHECK(tok_fail(tok({}, 0, 1, false), "next 0 != fresh 1"));

    // Felen.
    CHECK(tok_fail(tok(hx("40"), 1, 0, false), "back-reference"));         // avstånd 0
    CHECK(tok_fail(tok(hx("41"), 1, 0, false), "back-reference"));         // före början
    CHECK(tok_fail(tok(hx("00 42"), 2, 1, false), "back-reference"));      // för långt
    CHECK(tok_fail(tok(hx("00 E0 00"), 2, 1, false), "back-reference"));   // avstånd 0, lång form
    CHECK(tok_fail(tok(hx("00 E0 05"), 2, 1, false), "back-reference"));
    CHECK(tok_fail(tok(hx("00 F0 00 01"), 2, 1, false), "literal"));       // literal == next
    CHECK(tok_fail(tok(hx("00 F0 00 07"), 2, 1, false), "literal"));
    CHECK(tok_fail(tok(hx("F0 00 00"), 1, 0, false), "literal"));          // next = 0
    CHECK(tok_fail(tok(hx("80"), 1, 0, false), "repeat has no previous"));
    CHECK(tok_fail(tok(hx("D0"), 1, 0, false), "lead byte is undefined"));  // axel utan axis
    CHECK(tok_fail(tok(hx("D5"), 1, 0, false), "lead byte is undefined"));
    for (const char* undefined : {"C0", "CF", "D6", "DF", "F1", "FF"}) {
        CHECK(tok_fail(tok(hx(undefined), 1, 0, true), "lead byte is undefined"));
    }
    CHECK(tok_fail(tok(hx("02"), 4, 3, false), "unexpected end"));         // slut på data
    CHECK(tok_fail(tok(hx("00 E0"), 2, 1, false), "unexpected end"));
    CHECK(tok_fail(tok(hx("00 F0"), 2, 1, false), "unexpected end"));
    CHECK(tok_fail(tok(hx("00 F0 00"), 2, 1, false), "unexpected end"));
    CHECK(tok_fail(tok(hx("02"), 3, 2, false), "next 3 != fresh 2"));      // för många nya
    CHECK(tok_fail(tok(hx("01"), 2, 3, false), "next 2 != fresh 3"));      // för få nya
    CHECK(tok_fail(tok(hx("03 42 42 42"), 7, 3, false), "next 4 != fresh 3"));
    CHECK(tok_fail(tok(hx("05"), 3, 6, false), "token runs past the count"));
    CHECK(tok_fail(tok(hx("00 85"), 3, 1, false), "token runs past the count"));
    CHECK(tok_ok(tok(hx("04 80 80"), 7, 5, false), {0, 1, 2, 3, 4, 4, 4}, 3));
    // Ett antal som data inte kan koda ger fel utan allokering.
    CHECK(tok_fail(tok(hx("00 00"), 1000, 1000, false), "token count"));
    CHECK(tok_fail(tok(hx("00 00"), 0xFFFFFFFFu, 0xFFFFFFFFu, false), "token count"));
    CHECK(tok_fail(tok(Bytes(8, 0xBF), 0x80000000u, 0, false), "token count"));
    // En position förbi slutet.
    {
        Bytes b = hx("00");
        size_t pos = 5;
        std::vector<int32_t> out = {1};
        std::string detail;
        CHECK(decode_tokens(b.data(), b.size(), pos, 1, 1, false, out, &detail) ==
              ErrorCode::CorruptGeometry);
        CHECK(pos == 5 && out.empty());
    }
    // Utan detail går det också.
    {
        Bytes b = hx("FF");
        size_t pos = 0;
        std::vector<int32_t> out;
        CHECK(decode_tokens(b.data(), b.size(), pos, 1, 0, false, out) ==
              ErrorCode::CorruptGeometry);
    }
}

void test_packed(int& fails) {
    // Bredd 1, 8, 16, 32 med givna ord.
    {
        const Pk a = pk(word(0xA8000000), 5, 1);
        CHECK(a.code == ErrorCode::Ok && a.out == std::vector<uint32_t>({1, 0, 1, 0, 1}) &&
              a.pos == 4);
        const Pk b = pk(cat({word(0x01020304), word(0x05000000)}), 5, 8);
        CHECK(b.code == ErrorCode::Ok && b.out == std::vector<uint32_t>({1, 2, 3, 4, 5}) &&
              b.pos == 8);
        const Pk c = pk(cat({word(0x00010002), word(0x00030000)}), 3, 16);
        CHECK(c.code == ErrorCode::Ok && c.out == std::vector<uint32_t>({1, 2, 3}) && c.pos == 8);
        const Pk d = pk(cat({word(0xDEADBEEF), word(0x12345678)}), 2, 32);
        CHECK(d.code == ErrorCode::Ok && d.out == std::vector<uint32_t>({0xDEADBEEF, 0x12345678}) &&
              d.pos == 8);
        // Bredd 31: värden över ordgränsen. 62 bitar i två ord.
        const Pk e = pk(packed_words({0x7FFFFFFF, 1}, 31), 2, 31);
        CHECK(e.code == ErrorCode::Ok && e.out == std::vector<uint32_t>({0x7FFFFFFF, 1}));
        // Ord 0 = 0xFFFFFFFE, ord 1 = 0x00000004, räknat för hand.
        const Pk f = pk(cat({word(0xFFFFFFFE), word(0x00000004)}), 2, 31);
        CHECK(f.code == ErrorCode::Ok && f.out == std::vector<uint32_t>({0x7FFFFFFF, 1}));
    }
    // Rundturer för alla bredder och många antal, med värden över ordgränser.
    {
        uint64_t state = 12345;
        bool all = true;
        for (unsigned width = 1; width <= 32; ++width) {
            for (uint32_t count = 0; count <= 70; ++count) {
                std::vector<uint32_t> values(count);
                for (uint32_t& v : values) {
                    state = state * 6364136223846793005ull + 1442695040888963407ull;
                    const uint32_t r = static_cast<uint32_t>(state >> 32);
                    v = width == 32 ? r : r & ((1u << width) - 1);
                }
                const Bytes data = packed_words(values, width);
                const Pk p = pk(cat({hx("AA BB CC DD"), data}), count, width, 4);
                if (p.code != ErrorCode::Ok || p.out != values || p.pos != 4 + data.size())
                    all = false;
            }
        }
        CHECK(all);
    }
    // Noll värden.
    {
        const Pk p = pk({}, 0, 8);
        CHECK(p.code == ErrorCode::Ok && p.out.empty() && p.pos == 0);
    }
    // Oanvända bitar som inte är 0.
    CHECK(pk_fail(pk(word(0xA8000001), 5, 1), "unused packed bit"));
    CHECK(pk_fail(pk(word(0xA8400000), 5, 1), "unused packed bit"));
    CHECK(pk_fail(pk(cat({word(0x01020304), word(0x05000001)}), 5, 8), "unused packed bit"));
    CHECK(pk_fail(pk(cat({word(0xFFFFFFFF), word(0x00000007)}), 2, 31), "unused packed bit"));
    CHECK(pk_fail(pk(cat({word(0xFFFFFFFE), word(0x00000005)}), 2, 31), "unused packed bit"));
    // De använda bitarna får vara vilka som helst.
    {
        const Pk p = pk(word(0xA8000000), 5, 1);
        CHECK(p.code == ErrorCode::Ok);
    }
    // För lite data, och en bredd utanför 1..32.
    CHECK(pk_fail(pk(hx("01 02 03 04"), 5, 8), "packed words pass the end"));
    CHECK(pk_fail(pk({}, 1, 8), "packed words pass the end"));
    CHECK(pk_fail(pk(hx("01 02 03"), 1, 8), "packed words pass the end"));
    CHECK(pk_fail(pk(hx("00 00 00 00"), 1, 0), "packed width"));
    CHECK(pk_fail(pk(hx("00 00 00 00"), 1, 33), "packed width"));
    // Ett antal som inte ryms i datan ger fel utan allokering.
    CHECK(pk_fail(pk(word(0), 0xFFFFFFFFu, 32), "packed words pass the end"));
    CHECK(pk_fail(pk(word(0), 0xFFFFFFFFu, 1), "packed words pass the end"));
    // Position förbi slutet.
    {
        const Bytes b = hx("00 00 00 00");
        size_t pos = 9;
        std::vector<uint32_t> out = {1};
        CHECK(decode_packed(b.data(), b.size(), pos, 1, 8, out) == ErrorCode::CorruptGeometry);
        CHECK(pos == 9 && out.empty());
    }
}

void test_palette_normal(int& fails) {
    float n[3];
    palette_normal(127, 0, 0, 8, n);
    CHECK(near(n[0], 1) && near(n[1], 0) && near(n[2], 0));
    palette_normal(0x81, 0, 0, 8, n);   // -127
    CHECK(near(n[0], -1) && near(n[1], 0) && near(n[2], 0));
    palette_normal(0x7F, 0x81, 0, 8, n);
    CHECK(near(n[0], 0.70710678f) && near(n[1], -0.70710678f) && near(n[2], 0));
    palette_normal(0x80, 0, 0, 8, n);   // -128 normeras till -1
    CHECK(near(n[0], -1) && near(n[1], 0) && near(n[2], 0));
    palette_normal(1, 1, 1, 8, n);
    CHECK(near(n[0], 0.57735027f) && near(n[1], 0.57735027f) && near(n[2], 0.57735027f));
    palette_normal(0xFF, 0xFF, 0xFF, 8, n);   // -1, -1, -1
    CHECK(near(n[0], -0.57735027f) && near(n[1], -0.57735027f) && near(n[2], -0.57735027f));
    palette_normal(0, 0, 0, 8, n);
    CHECK(n[0] == 0 && n[1] == 0 && n[2] == 0);

    palette_normal(0x7FFF, 0, 0, 16, n);
    CHECK(near(n[0], 1) && near(n[1], 0) && near(n[2], 0));
    palette_normal(0x8001, 0, 0x7FFF, 16, n);   // -32767, 0, 32767
    CHECK(near(n[0], -0.70710678f) && near(n[1], 0) && near(n[2], 0.70710678f));
    palette_normal(0, 0x8000, 0, 16, n);        // -32768 normeras till -1
    CHECK(near(n[0], 0) && near(n[1], -1) && near(n[2], 0));
    palette_normal(0, 0, 0, 16, n);
    CHECK(n[0] == 0 && n[1] == 0 && n[2] == 0);
    // Samma bitar tolkas olika med de två bredderna.
    palette_normal(0x7F81, 0, 0, 16, n);
    CHECK(near(n[0], 1) && near(n[1], 0) && near(n[2], 0));
    palette_normal(0x00, 0x7F, 0x81, 8, n);
    CHECK(near(n[0], 0) && near(n[1], 0.70710678f) && near(n[2], -0.70710678f));
    // Resultatet är alltid en enhetsvektor eller noll.
    palette_normal(100, 3, 0xF0, 8, n);
    CHECK(near(std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]), 1.0f));
}

void test_valid_records(int& fails) {
    const Bytes r1b = r1().bytes();
    CHECK(r1b.size() == 120);
    CHECK(r5().bytes().size() == 128);
    CHECK(r16().bytes().size() == 128);
    CHECK(l1().bytes().size() == 104);

    // R1: båda bredderna.
    for (const int bits : {8, 16}) {
        const Parsed p = run(r1b, bits);
        CHECK(p.code == ErrorCode::Ok);
        CHECK(!p.data.lines);
        CHECK(near_all(p.data.positions, {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0}));
        CHECK(near_all(p.data.normals, {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1}));
        CHECK(same(p.data.indices, {0, 1, 2, 2, 1, 3}));
        CHECK(p.data.colors.empty());
        CHECK(p.data.primitives == 2);
        CHECK(p.data.normal_palette == 0);
        CHECK(near_all(std::vector<float>(p.data.bbox, p.data.bbox + 6), {0, 0, 0, 1, 1, 0}));
        CHECK(sane(p.data));
    }

    // R5: bara åtta bitar.
    {
        const Bytes b = r5().bytes();
        const Parsed p = run(b, 8);
        CHECK(p.code == ErrorCode::Ok);
        CHECK(near_all(p.data.normals, {0.70710678f, -0.70710678f, 0, 0.70710678f, -0.70710678f, 0,
                                        0.70710678f, -0.70710678f, 0, 0.70710678f, -0.70710678f, 0}));
        CHECK(same(p.data.indices, {0, 1, 2, 2, 1, 3}));
        CHECK(p.data.normal_palette == 1);
        const Parsed w = run(b, 16);
        CHECK(w.code == ErrorCode::CorruptGeometry && contains(w.detail, "unused packed bit"));
    }
    // R16: bara sexton bitar.
    {
        const Bytes b = r16().bytes();
        const Parsed p = run(b, 16);
        CHECK(p.code == ErrorCode::Ok);
        CHECK(near_all(p.data.normals, {1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0}));
        CHECK(p.data.normal_palette == 1);
        const Parsed w = run(b, 8);
        CHECK(w.code == ErrorCode::CorruptGeometry && contains(w.detail, "next 4 != fresh 1"));
    }
    // L1: båda bredderna.
    for (const int bits : {8, 16}) {
        const Parsed p = run(l1().bytes(), bits);
        CHECK(p.code == ErrorCode::Ok);
        CHECK(p.data.lines);
        CHECK(near_all(p.data.positions, {0, 0, 0, 10, 0, 0}));
        CHECK(p.data.normals.empty());
        CHECK(same(p.data.colors, {0xFF0000FFu, 0xFF0000FFu}));
        CHECK(same(p.data.indices, {0, 1}));
        CHECK(p.data.primitives == 1);
        CHECK(p.data.normal_palette == 0);
        CHECK(near_all(std::vector<float>(p.data.bbox, p.data.bbox + 6), {0, 0, 0, 10, 0, 0}));
        CHECK(sane(p.data));
    }
    // Linjer utan färgplats: giltiga, med tom färglista.
    for (const int bits : {8, 16}) {
        const Bytes b = lines_plain().bytes();
        CHECK(b.size() == 80);
        const Parsed p = run(b, bits);
        CHECK(p.code == ErrorCode::Ok);
        CHECK(p.data.lines && p.data.colors.empty() && p.data.normals.empty());
        CHECK(same(p.data.indices, {0, 1}));
        CHECK(p.data.primitives == 1);
        CHECK(sane(p.data));
    }
    // Färgbyten: [A][B][G][R] är 0xRRGGBBAA.
    {
        Rec r = l1();
        r.colors = colour_slot(102, 58, 5634, -2, 2, hx("FF112233 80445566"), {});
        const Parsed p = run(r.bytes(), 16);
        CHECK(p.code == ErrorCode::Ok);
        CHECK(same(p.data.colors, {0x332211FFu, 0x66554480u}));
    }

    // Alla axlar som normaler.
    {
        Rec r = r1();
        r.normals = cat({ints({102, 100, -4, 0}), hx("D0 D1 D2 D3")});
        Parsed p = run(r.bytes(), 8);
        CHECK(p.code == ErrorCode::Ok);
        CHECK(near_all(p.data.normals, {1, 0, 0, -1, 0, 0, 0, 1, 0, 0, -1, 0}));
        r.normals = cat({ints({102, 100, -4, 0}), hx("D4 D5 D5 D4")});
        p = run(r.bytes(), 16);
        CHECK(p.code == ErrorCode::Ok);
        CHECK(near_all(p.data.normals, {0, 0, 1, 0, 0, -1, 0, 0, -1, 0, 0, 1}));
    }
    // Normalpalett lika stor som p1: ingen NTOK. Åtta och sexton bitar.
    {
        Rec r = r1();
        r.normals = norm_slot(102, -4, 4, packed_words({127, 0, 0, 0, 127, 0, 0, 0, 127, 0x81, 0, 0}, 8), {});
        const Parsed p = run(r.bytes(), 8);
        CHECK(p.code == ErrorCode::Ok);
        CHECK(near_all(p.data.normals, {1, 0, 0, 0, 1, 0, 0, 0, 1, -1, 0, 0}));
        CHECK(p.data.normal_palette == 4);
        CHECK(run(r.bytes(), 16).code == ErrorCode::CorruptGeometry);
        r.normals = norm_slot(102, -4, 4,
                              packed_words({0x7FFF, 0, 0, 0, 0x7FFF, 0, 0, 0, 0x7FFF, 0x8001, 0, 0}, 16), {});
        const Parsed w = run(r.bytes(), 16);
        CHECK(w.code == ErrorCode::Ok);
        CHECK(near_all(w.data.normals, {1, 0, 0, 0, 1, 0, 0, 0, 1, -1, 0, 0}));
        CHECK(run(r.bytes(), 8).code == ErrorCode::CorruptGeometry);
    }

    // VMAP, UV med tokens, normalpalett med axelkoder och S > 0.
    {
        const Bytes b = rich().bytes();
        const Parsed p = run(b, 8);
        CHECK(p.code == ErrorCode::Ok);
        CHECK(near_all(p.data.positions, {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 0, 0, 1, 0, 0}));
        CHECK(near_all(p.data.normals, {0, 1, 0, -1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 1, 0}));
        CHECK(same(p.data.indices, {0, 1, 2, 3, 4, 1, 1, 4, 2}));
        CHECK(p.data.primitives == 3);
        CHECK(p.data.normal_palette == 2);
        // Ramen gäller de lagrade positionerna.
        CHECK(near_all(std::vector<float>(p.data.bbox, p.data.bbox + 6), {0, 0, 0, 1, 1, 0}));
        CHECK(sane(p.data));
        // Palettens ord fyller 48 av 64 bitar, och med sexton bitar blir det fel.
        CHECK(run(b, 16).code == ErrorCode::CorruptGeometry);
    }
    // Strip om fem hörn med S = 5; paritet och hörn via tokens.
    {
        const Parsed p = run(strip5().bytes(), 8);
        CHECK(p.code == ErrorCode::Ok);
        CHECK(same(p.data.indices, {0, 1, 2, 2, 1, 3, 2, 3, 2}));
        CHECK(p.data.primitives == 3);
    }
    // Två strips: paritetsräknaren börjar om i varje. Nio hörn, strips om fyra och fem.
    {
        Rec r = r1();
        r.head = head(94, 9, {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0, 2, 0, 0, 2, 1, 0, 3, 0, 0, 3, 1, 0, 4, 0, 0});
        r.normals = cat({ints({102, 100, -9, 0}), hx("D4 D4 D4 D4 D4 D4 D4 D4 D4 00 00 00")});
        r.prim = prim({4, 5}, 3);
        const Parsed p = run(r.bytes(), 8);
        CHECK(p.code == ErrorCode::Ok);
        CHECK(same(p.data.indices, {0, 1, 2, 2, 1, 3, 4, 5, 6, 6, 5, 7, 6, 7, 8}));
        CHECK(p.data.primitives == 5);
        // Sex hörn i en strip, och sex i två strips om tre.
        r.head = head(94, 6, {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0, 2, 0, 0, 2, 1, 0});
        r.normals = cat({ints({102, 100, -6, 0}), hx("D4 D4 D4 D4 D4 D4 00 00")});
        r.prim = prim({6}, 3);
        const Parsed s = run(r.bytes(), 8);
        CHECK(s.code == ErrorCode::Ok);
        CHECK(same(s.data.indices, {0, 1, 2, 2, 1, 3, 2, 3, 4, 4, 3, 5}));
        CHECK(s.data.primitives == 4);
        r.prim = prim({3, 3}, 3);
        const Parsed t = run(r.bytes(), 8);
        CHECK(t.code == ErrorCode::Ok);
        CHECK(same(t.data.indices, {0, 1, 2, 3, 4, 5}));
        // En strip om två är ingen triangel.
        r.prim = prim({4, 2}, 3);
        CHECK(run(r.bytes(), 8).code == ErrorCode::CorruptGeometry);
    }
    // UV: rå, med och utan tokens; kvantiserad, med och utan tokens.
    {
        Rec r = r1(103);
        r.uv = uv_raw(102, 4, 8, 8, {});
        CHECK(run(r.bytes(), 8).code == ErrorCode::Ok);
        r.uv = uv_raw(102, 4, 6, 6, hx("02 42 00 00"));
        CHECK(run(r.bytes(), 8).code == ErrorCode::Ok);
        r.uv = uv_quant(102, -4, 0x0404, 4, packed_words({1, 2, 3, 4}, 8), {});
        CHECK(run(r.bytes(), 8).code == ErrorCode::Ok);
        r.uv = uv_quant(102, -4, 0x0404, 2, packed_words({0xAB, 0xCD}, 8), hx("01 41 41 00"));
        const Parsed p = run(r.bytes(), 8);
        CHECK(p.code == ErrorCode::Ok);
        CHECK(same(p.data.indices, {0, 1, 2, 2, 1, 3}));
        // Bredder som inte delar 32: 3 * (5 + 7) = 36 bitar, två ord.
        r.uv = uv_quant(102, -4, 0x0705, 3, packed_words({1, 2, 3}, 12), hx("02 80 00 00"));
        CHECK(run(r.bytes(), 8).code == ErrorCode::Ok);
    }
    // UV-värdena ut, ett par per render-vertex.
    {
        Rec r = r1(103);
        // Rå utan tokens: paren som de står.
        r.uv = uv_raw_values(102, 4, {0, 1, 0.25f, -2, 3.5f, 0.5f, 1, 1}, {});
        Parsed p = run(r.bytes(), 8);
        CHECK(p.code == ErrorCode::Ok);
        CHECK(near_all(p.data.uvs, {0, 1, 0.25f, -2, 3.5f, 0.5f, 1, 1}));
        // Rå med tokens: tre par, mappen 0, 1, 2, 1.
        r.uv = uv_raw_values(102, 4, {0, 0, 1, 0, 0, 1}, hx("02 42 00 00"));
        p = run(r.bytes(), 8);
        CHECK(p.code == ErrorCode::Ok);
        CHECK(near_all(p.data.uvs, {0, 0, 1, 0, 0, 1, 1, 0}));
        // Ett par för alla fyra.
        r.uv = uv_raw_values(102, 4, {0.75f, 0.125f}, hx("00 82 00 00"));
        p = run(r.bytes(), 8);
        CHECK(p.code == ErrorCode::Ok);
        CHECK(near_all(p.data.uvs, {0.75f, 0.125f, 0.75f, 0.125f, 0.75f, 0.125f, 0.75f, 0.125f}));
        // De befintliga posterna: alla tal är 0,5.
        r.uv = uv_raw(102, 4, 8, 8, {});
        p = run(r.bytes(), 8);
        CHECK(p.code == ErrorCode::Ok && near_all(p.data.uvs, {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f}));
        r.uv = uv_quant(102, -4, 0x0404, 2, packed_words({0xAB, 0xCD}, 8), hx("01 41 41 00"));
        p = run(r.bytes(), 8);
        CHECK(p.code == ErrorCode::Ok && p.data.uvs.size() == 8);
        for (const float v : p.data.uvs) CHECK(v == 0.5f);   // umin = umax: varje värde är 0,5

        // Kvantiserad: (umin, vmin, umax, vmax) = (0, 10, 1, 20), fyra plus fyra bitar.
        const float range[4] = {0, 10, 1, 20};
        r.uv = uv_quant_range(102, -4, range, 0x0404, 4, packed_words({0x0F, 0xF0, 0x5A, 0xFF}, 8), {});
        p = run(r.bytes(), 8);
        CHECK(p.code == ErrorCode::Ok);
        CHECK(near_all(p.data.uvs, {0, 20, 1, 10, 1.0f / 3, 10 + 10 * 10.0f / 15, 1, 20}));
        // Med tokens: två par, mappen 0, 1, 1, 1.
        r.uv = uv_quant_range(102, -4, range, 0x0404, 2, packed_words({0x0F, 0xF0}, 8),
                              hx("01 41 41 00"));
        p = run(r.bytes(), 8);
        CHECK(p.code == ErrorCode::Ok);
        CHECK(near_all(p.data.uvs, {0, 20, 1, 10, 1, 10, 1, 10}));
        // Bredden 0 för v: v är vmin i alla par.
        r.uv = uv_quant_range(102, -4, range, 0x0008, 4, packed_words({0, 255, 51, 102}, 8), {});
        p = run(r.bytes(), 8);
        CHECK(p.code == ErrorCode::Ok);
        CHECK(near_all(p.data.uvs, {0, 10, 1, 10, 0.2f, 10, 0.4f, 10}));
        // Bredder som inte delar 32: u om 5 bitar, v om 7, par över ordgränsen.
        const float wide[4] = {-1, 0, 1, 127};
        r.uv = uv_quant_range(102, -4, wide, 0x0705, 3,
                              packed_words({(31u << 7) | 127u, (0u << 7) | 1u, (16u << 7) | 64u}, 12),
                              hx("02 80 00 00"));
        p = run(r.bytes(), 8);
        CHECK(p.code == ErrorCode::Ok);
        CHECK(near_all(p.data.uvs, {1, 127, -1, 1, -1 + 2 * 16.0f / 31, 64, -1 + 2 * 16.0f / 31, 64}));
        // 32 bitar för u går.
        r.uv = uv_quant_range(102, -4, range, 0x0020, 1, word(0xFFFFFFFFu), hx("00 82 00 00"));
        p = run(r.bytes(), 8);
        CHECK(p.code == ErrorCode::Ok && near_all(p.data.uvs, {1, 10, 1, 10, 1, 10, 1, 10}));

        // Posten med allt: kvantiserade UV med tokens, ett par per render-vertex.
        const Parsed q = run(rich().bytes(), 8);
        CHECK(q.code == ErrorCode::Ok && q.data.uvs.size() == 10);
        // Utan UV är listan tom.
        CHECK(run(r1().bytes(), 8).data.uvs.empty());
        CHECK(run(l1().bytes(), 8).data.uvs.empty());
    }
    // Färg med tokens, och flera polylinjer.
    {
        Rec r = l1();
        r.colors = colour_slot(102, 58, 5634, -2, 1, hx("FF0000FF"), hx("00 80 00 00"));
        const Parsed p = run(r.bytes(), 8);
        CHECK(p.code == ErrorCode::Ok);
        CHECK(same(p.data.colors, {0xFF0000FFu, 0xFF0000FFu}));

        const Parsed m = run(lines_multi().bytes(), 8);
        CHECK(m.code == ErrorCode::Ok);
        CHECK(same(m.data.indices, {0, 1, 2, 3, 3, 4}));
        CHECK(m.data.primitives == 3);
        CHECK(m.data.colors.size() == 5);
        CHECK(same(m.data.colors, {0x00FF00FFu, 0x00FF00FFu, 0x00FF00FFu, 0x00FF00FFu, 0x00FF00FFu}));
        CHECK(sane(m.data));

        Rec t = l1();
        t.head = head(95, 4, {0, 0, 0, 1, 0, 0, 2, 0, 0, 3, 0, 0});
        t.colors = colour_slot(102, 58, 5634, -4, 2, hx("FF0000FF FF00FF00"), hx("01 41 41 00"));
        t.prim = prim({4}, 3);
        const Parsed q = run(t.bytes(), 8);
        CHECK(q.code == ErrorCode::Ok);
        CHECK(same(q.data.colors, {0xFF0000FFu, 0x00FF00FFu, 0x00FF00FFu, 0x00FF00FFu}));
        CHECK(same(q.data.indices, {0, 1, 1, 2, 2, 3}));
    }
    // P-bredden 31.
    {
        Rec r = r1();
        r.prim = prim({4}, 31);
        CHECK(run(r.bytes(), 8).code == ErrorCode::Ok);
    }
    // Tolkningen ersätter det som fanns i `out`.
    {
        RecordData d;
        d.positions = {1, 2, 3};
        d.primitives = 9;
        d.colors = {1, 2};
        const Bytes b = l1().bytes();
        CHECK(parse_record(b.data(), b.size(), 8, d) == ErrorCode::Ok);
        CHECK(d.positions.size() == 6 && d.primitives == 1);
        const Bytes t = r1().bytes();
        CHECK(parse_record(t.data(), t.size(), 8, d) == ErrorCode::Ok);
        CHECK(d.colors.empty() && !d.lines && d.primitives == 2);
    }
}

void test_record_rules(int& fails) {
    const Bytes r1b = r1().bytes();

    // Huvudet.
    EXPECT_FAIL(with_i32(r1b, 0, 101), 8, "record id is not 100");
    EXPECT_FAIL(with_i32(r1b, 4, 93), 8, "class is not 94, 95, 96, 103, 104 or 105");
    EXPECT_FAIL(with_i32(r1b, 4, 0), 8, "class is not 94, 95, 96, 103, 104 or 105");
    EXPECT_FAIL(with_i32(r1b, 4, 106), 8, "class is not 94, 95, 96, 103, 104 or 105");
    EXPECT_FAIL(with_i32(r1b, 4, 103), 8, "");   // en triangelpost är ingen text

    // Punkter (klass 96): positioner, valfri färg, ett tal 0 eller 1, slut.
    {
        Rec pt;
        pt.head = head(96, 2, {1, 2, 3, 4, 5, 6});
        pt.uv = ints({0});
        pt.normals = ints({0});
        pt.colors = colour_slot(102, 58, 5634, -2, 1, hx("FF7F7F7F"), hx("00 80 00 00"));
        pt.prim = ints({1});
        for (const int bits : {8, 16}) {
            const Bytes rec = pt.bytes();
            RecordData out;
            std::string why;
            CHECK(parse_record(rec.data(), rec.size(), bits, out, &why) == ErrorCode::Ok);
            CHECK(out.points && !out.lines && out.primitives == 2);
            CHECK(out.indices.empty() && out.normals.empty());
            CHECK(out.positions == (std::vector<float>{1, 2, 3, 4, 5, 6}));
            CHECK(out.colors.size() == 2 && out.colors[0] == 0x7F7F7FFFu && out.colors[1] == 0x7F7F7FFFu);
            CHECK(out.bbox[0] == 1 && out.bbox[5] == 6);
        }
        Rec plain = pt;   // utan färg, och talet 0
        plain.colors = ints({0});
        plain.prim = ints({0});
        {
            const Bytes rec = plain.bytes();
            RecordData out;
            CHECK(parse_record(rec.data(), rec.size(), 8, out) == ErrorCode::Ok);
            CHECK(out.points && out.colors.empty() && out.primitives == 2);
        }
        Rec bad = pt;     // ett annat tal sist
        bad.prim = ints({2});
        EXPECT_FAIL(bad.bytes(), 8, "point record does not end with 0 or 1");
        bad = pt;         // punkter har inga normaler
        bad.normals = cat({ints({102, 100, -2, 0}), hx("D4 D4 00 00")});
        bad.colors = ints({0});
        EXPECT_FAIL(bad.bytes(), 8, "slot set does not match the class");
        bad = pt;         // byte efter slutet
        bad.sfield = ints({0, 0});
        EXPECT_FAIL(bad.bytes(), 8, "record does not end at its length");
    }
    EXPECT_FAIL(with_i32(r1b, 8, 102), 8, "position object id is not 101");
    EXPECT_FAIL(with_i32(r1b, 12, 61), 8, "position class is not 60");
    EXPECT_UNSUPPORTED(with_i32(r1b, 16, -4), 8, "quantised positions");
    EXPECT_FAIL(with_i32(r1b, 16, 0), 8, "render vertex count is zero");
    EXPECT_FAIL(with_i32(r1b, 16, 121), 8, "render vertex count is above the record length");
    EXPECT_FAIL(with_i32(r1b, 16, 0x7FFFFFFF), 8, "render vertex count is above the record length");
    EXPECT_FAIL(with_i32(r1b, 20, 0), 8, "position float count");
    EXPECT_FAIL(with_i32(r1b, 20, -3), 8, "position float count");
    EXPECT_FAIL(with_i32(r1b, 20, 10), 8, "position float count");
    EXPECT_FAIL(with_i32(r1b, 20, 300), 8, "positions pass the end");
    EXPECT_FAIL(with_i32(r1b, 20, 0x7FFFFFFE), 8, "positions pass the end");
    EXPECT_FAIL(with_i32(r1b, 20, -2147483647), 8, "position float count");
    EXPECT_FAIL(with_i32(with_i32(r1b, 16, 3), 20, 12), 8, "below the position count");
    // Byten räknas från postens början.
    {
        const Parsed p = run(with_i32(r1b, 16, -4), 8);
        CHECK(p.detail.rfind("record at byte 16: ", 0) == 0);
        const Parsed q = run(with_i32(r1b, 4, 93), 8);
        CHECK(q.detail.rfind("record at byte 4: ", 0) == 0);
    }
    // En position som inte är ändlig.
    {
        const float bad[3] = {std::nanf(""), HUGE_VALF, -HUGE_VALF};
        for (const float v : bad) {
            Bytes b = r1b;
            uint32_t u;
            std::memcpy(&u, &v, sizeof u);
            b = with_i32(b, 24 + 12 * 2 + 4, static_cast<int32_t>(u));
            EXPECT_FAIL(b, 8, "position is not finite");
        }
        Bytes b = with_i32(r1b, 24, 0x7F800000);   // +inf i första talet
        EXPECT_FAIL(b, 8, "position is not finite");
    }

    // VMAP.
    {
        const Rec q = rich();
        Rec r = q;
        r.vmap = hx("03 42 42 42");   // next 4 != fresh 3
        EXPECT_FAIL(r.bytes(), 8, "next 4 != fresh 3");
        r = q;
        r.vmap = hx("02 42 80 01");   // utfyllnaden är inte noll
        EXPECT_FAIL(r.bytes(), 8, "pad byte is not zero");
        r = q;
        r.vmap = hx("02 D0 D0 D0");   // axelkod i en VMAP
        EXPECT_FAIL(r.bytes(), 8, "lead byte is undefined");
        r = q;
        r.vmap = hx("02 80 80 00");   // två upprepningar ger också fem värden
        CHECK(run(r.bytes(), 8).code == ErrorCode::Ok);
        r.vmap = hx("02 90 00 00");   // 0x90 är 17 upprepningar
        EXPECT_FAIL(r.bytes(), 8, "token runs past the count");
        r.vmap = hx("02 42 80");      // utfyllnaden saknas, så nästa byte tas för den
        EXPECT_FAIL(r.bytes(), 8, "pad byte is not zero");
    }

    // Platsernas id.
    {
        Rec r = r1(103);                 // normaler utan UV ska ha 102
        EXPECT_FAIL(r.bytes(), 8, "slot id is out of order");
        r = r1(102);
        r.uv = uv_raw(103, 4, 8, 8, {});
        EXPECT_FAIL(r.bytes(), 8, "slot id is out of order");
        r = r1(102);                     // UV har 102, så normalerna ska ha 103
        r.uv = uv_raw(102, 4, 8, 8, {});
        EXPECT_FAIL(r.bytes(), 8, "slot id is out of order");
        r = r1(104);
        r.uv = uv_raw(102, 4, 8, 8, {});
        EXPECT_FAIL(r.bytes(), 8, "slot id is out of order");
        r = r1(-1);
        EXPECT_FAIL(r.bytes(), 8, "slot id is out of order");
        r = r1(101);
        EXPECT_FAIL(r.bytes(), 8, "slot id is out of order");
        Rec l = l1();
        l.colors = colour_slot(103, 58, 5634, -2, 2, hx("FF0000FF FF0000FF"), {});
        EXPECT_FAIL(l.bytes(), 8, "slot id is out of order");
    }

    // UV.
    {
        Rec r = r1(103);
        r.uv = cat({ints({102, 62, 4, 8}), floats(8)});
        EXPECT_FAIL(r.bytes(), 8, "UV class is not 61");
        r.uv = uv_raw(102, 7, 8, 8, {});
        EXPECT_FAIL(r.bytes(), 8, "UV count is neither p1 nor -p1");
        r.uv = uv_raw(102, 0, 8, 8, {});
        EXPECT_FAIL(r.bytes(), 8, "UV count is neither p1 nor -p1");
        r.uv = uv_raw(102, 4, 0, 0, {});
        EXPECT_FAIL(r.bytes(), 8, "UV float count is not positive and even");
        r.uv = uv_raw(102, 4, 7, 7, hx("02 42 00 00"));
        EXPECT_FAIL(r.bytes(), 8, "UV float count is not positive and even");
        r.uv = uv_raw(102, 4, -2, 2, {});
        EXPECT_FAIL(r.bytes(), 8, "UV float count is not positive and even");
        r.uv = uv_raw(102, 4, 1000, 8, {});
        EXPECT_FAIL(r.bytes(), 8, "UV floats pass the end");
        r.uv = uv_raw(102, 4, 0x7FFFFFFE, 8, {});
        EXPECT_FAIL(r.bytes(), 8, "UV floats pass the end");
        // fc/2 != p1 kräver tokens, och de prövas.
        r.uv = uv_raw(102, 4, 6, 6, hx("03 42 42 00"));
        EXPECT_FAIL(r.bytes(), 8, "next 4 != fresh 3");
        r.uv = uv_raw(102, 4, 6, 6, hx("02 42 00 01"));
        EXPECT_FAIL(r.bytes(), 8, "pad byte is not zero");
        r.uv = uv_raw(102, 4, 2, 2, hx("00 80 80 80"));    // tokens ger p1 = 4 värden av ett par
        CHECK(run(r.bytes(), 8).code == ErrorCode::Ok);
        r.uv = uv_raw(102, 4, 10, 10, hx("03 00 00 00"));  // fc/2 = 5 > p1: next kan inte nå 5
        EXPECT_FAIL(r.bytes(), 8, "next 4 != fresh 5");

        // Kvantiserad.
        const Bytes w4 = packed_words({1, 2, 3, 4}, 8);
        r.uv = uv_quant(102, -4, 0x10404, 4, w4, {});
        EXPECT_FAIL(r.bytes(), 8, "UV bit widths");
        // En bredd på 0 går: koordinaten är densamma i alla par och tar inga bitar.
        r.uv = uv_quant(102, -4, 0x0008, 4, w4, {});   // bv = 0
        CHECK(run(r.bytes(), 8).code == ErrorCode::Ok);
        r.uv = uv_quant(102, -4, 0x0800, 4, w4, {});   // bu = 0
        CHECK(run(r.bytes(), 8).code == ErrorCode::Ok);
        r.uv = uv_quant(102, -4, 0, 4, {}, {});        // båda 0: inga ord alls
        CHECK(run(r.bytes(), 8).code == ErrorCode::Ok);
        r.uv = uv_quant(102, -4, 0x0004, 4, w4, {});   // fyra bitar per par, men orden har fler
        EXPECT_FAIL(r.bytes(), 8, "unused packed bit is set");
        r.uv = uv_quant(102, -4, static_cast<int32_t>(0x80000404), 4, w4, {});
        EXPECT_FAIL(r.bytes(), 8, "UV bit widths");
        r.uv = uv_quant(102, -4, -1, 4, w4, {});
        EXPECT_FAIL(r.bytes(), 8, "UV bit widths");
        r.uv = uv_quant(102, -4, 0x0404, 0, {}, {});
        EXPECT_FAIL(r.bytes(), 8, "UV value count is not in 1..p1");
        r.uv = uv_quant(102, -4, 0x0404, -1, w4, {});
        EXPECT_FAIL(r.bytes(), 8, "UV value count is not in 1..p1");
        r.uv = uv_quant(102, -4, 0x0404, 5, w4, {});
        EXPECT_FAIL(r.bytes(), 8, "UV value count is not in 1..p1");
        // Oanvända bitar: ett värde om 4 + 4 bitar använder de översta åtta av 32.
        r.uv = uv_quant(102, -4, 0x0404, 1, word(0x00010000), hx("00 82 00 00"));
        EXPECT_FAIL(r.bytes(), 8, "unused packed bit");
        r.uv = uv_quant(102, -4, 0x0404, 1, word(0x00000001), hx("00 82 00 00"));
        EXPECT_FAIL(r.bytes(), 8, "unused packed bit");
        r.uv = uv_quant(102, -4, 0x0404, 1, word(0x12000000), hx("00 82 00 00"));
        CHECK(run(r.bytes(), 8).code == ErrorCode::Ok);
        // Fler bitar än datan har: 4 * (255 + 255) bitar.
        r.uv = uv_quant(102, -4, 0xFFFF, 4, w4, {});
        EXPECT_FAIL(r.bytes(), 8, "packed words pass the end");
        // Tokens med fel antal nya.
        r.uv = uv_quant(102, -4, 0x0404, 2, packed_words({1, 2}, 8), hx("02 42 00 00"));
        EXPECT_FAIL(r.bytes(), 8, "next 3 != fresh 2");

        // Värden och intervall som inte är ändliga.
        r.uv = uv_raw_values(102, 4, {0, 0, 1, std::nanf(""), 0, 1, 1, 1}, {});
        EXPECT_FAIL(r.bytes(), 8, "UV value is not finite");
        r.uv = uv_raw_values(102, 4, {0, 0, 1, 0, 0, 1, HUGE_VALF, 1}, {});
        EXPECT_FAIL(r.bytes(), 8, "UV value is not finite");
        {
            const float bad_range[4] = {0, 0, HUGE_VALF, 1};
            r.uv = uv_quant_range(102, -4, bad_range, 0x0404, 4, w4, {});
            EXPECT_FAIL(r.bytes(), 8, "UV range is not finite");
            const float nan_range[4] = {std::nanf(""), 0, 1, 1};
            r.uv = uv_quant_range(102, -4, nan_range, 0x0404, 4, w4, {});
            EXPECT_FAIL(r.bytes(), 8, "UV range is not finite");
        }
        // En bredd över 32 bitar avkodas inte, när orden väl ryms.
        r.uv = uv_quant(102, -4, 0x0021, 1, cat({word(0), word(0)}), hx("00 82 00 00"));
        EXPECT_UNSUPPORTED(r.bytes(), 8, "quantised UV width above 32 bits");
        r.uv = uv_quant(102, -4, 0x2100, 1, cat({word(0), word(0)}), hx("00 82 00 00"));
        EXPECT_UNSUPPORTED(r.bytes(), 8, "quantised UV width above 32 bits");
    }

    // Normaler.
    {
        Rec r = r1();
        r.normals = cat({ints({102, 99, -4, 0}), hx("D4 D4 D4 D4")});
        EXPECT_FAIL(r.bytes(), 8, "normal class is not 100");
        r.normals = cat({ints({102, 100, -3, 0}), hx("D4 D4 D4 D4")});
        EXPECT_FAIL(r.bytes(), 8, "normal count is not -p1");
        r.normals = cat({ints({102, 100, 4, 0}), hx("D4 D4 D4 D4")});
        EXPECT_FAIL(r.bytes(), 8, "normal count is not -p1");
        r.normals = cat({ints({102, 100, -4, -1}), hx("D4 D4 D4 D4")});
        EXPECT_FAIL(r.bytes(), 8, "normal palette size is out of range");
        r.normals = cat({ints({102, 100, -4, 715827883}), hx("D4 D4 D4 D4")});
        EXPECT_FAIL(r.bytes(), 8, "normal palette size is out of range");
        r.normals = cat({ints({102, 100, -4, 0x7FFFFFFF}), hx("D4 D4 D4 D4")});
        EXPECT_FAIL(r.bytes(), 8, "normal palette size is out of range");
        r.normals = cat({ints({102, 100, -4, 715827882}), hx("D4 D4 D4 D4")});
        EXPECT_FAIL(r.bytes(), 8, "packed words pass the end");
        EXPECT_FAIL(r.bytes(), 16, "packed words pass the end");
        // Oanvänd bit i palettens sista ord: 24 av 32 bitar används med åtta bitar.
        r.normals = cat({ints({102, 100, -4, 1}), word(0x7F810001), hx("00 82 00 00")});
        EXPECT_FAIL(r.bytes(), 8, "unused packed bit");
        // Tokens: för få nya, för många nya, utfyllnad.
        r.normals = cat({ints({102, 100, -4, 1}), word(0x7F810000), hx("00 00 00 00")});
        EXPECT_FAIL(r.bytes(), 8, "next 4 != fresh 1");
        r.normals = cat({ints({102, 100, -4, 5}), packed_words(std::vector<uint32_t>(15, 1), 8),
                         hx("03 00 00 00")});
        EXPECT_FAIL(r.bytes(), 8, "next 4 != fresh 5");   // en palett större än p1 kan inte nås
        r.normals = cat({ints({102, 100, -4, 2}), packed_words(std::vector<uint32_t>(6, 1), 8),
                         hx("01 90 00 00")});
        EXPECT_FAIL(r.bytes(), 8, "token runs past the count");
        r.normals = cat({ints({102, 100, -4, 1}), word(0x7F810000), hx("00 82 00 01")});
        EXPECT_FAIL(r.bytes(), 8, "pad byte is not zero");
        // Axelkod utan palett är giltig; okänd kod är det inte.
        r.normals = cat({ints({102, 100, -4, 0}), hx("D6 D4 D4 D4")});
        EXPECT_FAIL(r.bytes(), 8, "lead byte is undefined");
        // Tokens där alla värden är axelkoder men en körning krävs.
        r.normals = cat({ints({102, 100, -4, 0}), hx("00 D4 D4 D4")});
        EXPECT_FAIL(r.bytes(), 8, "next 1 != fresh 0");
    }

    // Färg.
    {
        Rec l = l1();
        l.colors = colour_slot(102, 59, 5634, -2, 2, hx("FF0000FF FF0000FF"), {});
        EXPECT_FAIL(l.bytes(), 8, "colour class is not 58");
        l.colors = colour_slot(102, 58, 5635, -2, 2, hx("FF0000FF FF0000FF"), {});
        EXPECT_FAIL(l.bytes(), 8, "colour flag is not 5634");
        l.colors = colour_slot(102, 58, 5634, -3, 2, hx("FF0000FF FF0000FF"), {});
        EXPECT_FAIL(l.bytes(), 8, "colour count is not -p1");
        l.colors = colour_slot(102, 58, 5634, 2, 2, hx("FF0000FF FF0000FF"), {});
        EXPECT_FAIL(l.bytes(), 8, "colour count is not -p1");
        l.colors = colour_slot(102, 58, 5634, -2, 0, {}, {});
        EXPECT_FAIL(l.bytes(), 8, "colour palette size is not positive");
        l.colors = colour_slot(102, 58, 5634, -2, -1, hx("FF0000FF FF0000FF"), {});
        EXPECT_FAIL(l.bytes(), 8, "colour palette size is not positive");
        l.colors = colour_slot(102, 58, 5634, -2, 0x1FFFFFFF, hx("FF0000FF FF0000FF"), {});
        EXPECT_FAIL(l.bytes(), 8, "colour entries pass the end");
        l.colors = colour_slot(102, 58, 5634, -2, 0x7FFFFFFF, hx("FF0000FF FF0000FF"), {});
        EXPECT_FAIL(l.bytes(), 8, "colour entries pass the end");
        l.colors = colour_slot(102, 58, 5634, -2, 1, hx("FF0000FF"), hx("00 00 00 00"));
        EXPECT_FAIL(l.bytes(), 8, "next 2 != fresh 1");
        l.colors = colour_slot(102, 58, 5634, -2, 1, hx("FF0000FF"), hx("00 D4 00 00"));
        EXPECT_FAIL(l.bytes(), 8, "lead byte is undefined");   // axelkoder bara för normaler
        l.colors = colour_slot(102, 58, 5634, -2, 3, hx("FF0000FF FF0000FF FF0000FF"), {});
        EXPECT_FAIL(l.bytes(), 8, "next 2 != fresh 3");         // cpal != p1 utan egna tokens
    }

    // Platserna ska passa klassen.
    {
        Rec t = r1();   // trianglar utan normaler
        t.normals = ints({0});
        EXPECT_FAIL(t.bytes(), 8, "slot set does not match the class");
        t.colors = colour_slot(102, 58, 5634, -4, 4, hx("FF0000FF FF0000FF FF0000FF FF0000FF"), {});
        EXPECT_FAIL(t.bytes(), 8, "slot set does not match the class");   // bara färg
        Rec b = r1();   // normaler och färg: trianglar får ha färg per vertex
        b.colors = colour_slot(103, 58, 5634, -4, 4, hx("FF0000FF FF00FF00 FFFF0000 FF112233"), {});
        {
            const Bytes rec = b.bytes();
            RecordData out;
            std::string why;
            CHECK(parse_record(rec.data(), rec.size(), 8, out, &why) == ErrorCode::Ok);
            CHECK(!out.lines && out.primitives == 2 && out.normals.size() == 12);
            CHECK(out.colors.size() == 4);
            if (out.colors.size() == 4) {
                // Paletten lagrar byten A, B, G, R; ut kommer 0xRRGGBBAA.
                CHECK(out.colors[0] == 0xFF0000FFu);
                CHECK(out.colors[1] == 0x00FF00FFu);
                CHECK(out.colors[2] == 0x0000FFFFu);
                CHECK(out.colors[3] == 0x332211FFu);
            }
        }
        Rec u = r1();   // bara UV
        u.normals = ints({0});
        u.uv = uv_raw(102, 4, 8, 8, {});
        EXPECT_FAIL(u.bytes(), 8, "slot set does not match the class");

        Rec n = l1();   // linjer med normaler och färg
        n.normals = norm_slot(102, -2, 0, {}, hx("D4 D4 00 00"));
        n.colors = colour_slot(103, 58, 5634, -2, 2, hx("FF0000FF FF0000FF"), {});
        EXPECT_FAIL(n.bytes(), 8, "slot set does not match the class");
        Rec nn = l1();  // linjer med bara normaler
        nn.normals = norm_slot(102, -2, 0, {}, hx("D4 D4 00 00"));
        nn.colors = ints({0});
        EXPECT_FAIL(nn.bytes(), 8, "slot set does not match the class");
        Rec lu = l1();  // linjer med UV och färg
        lu.uv = uv_raw(102, 2, 4, 4, {});
        lu.colors = colour_slot(103, 58, 5634, -2, 2, hx("FF0000FF FF0000FF"), {});
        EXPECT_FAIL(lu.bytes(), 8, "slot set does not match the class");
    }

    // P.
    {
        Rec r = r1();
        r.prim = ints({0, 4, 0});
        EXPECT_FAIL(r.bytes(), 8, "primitive count is not positive");
        r.prim = ints({-1, 4, -1});
        EXPECT_FAIL(r.bytes(), 8, "primitive count is not positive");
        r.prim = cat({ints({1, 0, 1}), word(0)});
        EXPECT_FAIL(r.bytes(), 8, "primitive length width");
        r.prim = cat({ints({1, 32, 1}), word(4)});
        EXPECT_FAIL(r.bytes(), 8, "primitive length width");
        r.prim = cat({ints({1, -1, 1}), word(4)});
        EXPECT_FAIL(r.bytes(), 8, "primitive length width");
        r.prim = cat({ints({1, 4, 2}), word(0x40000000)});
        EXPECT_FAIL(r.bytes(), 8, "second primitive count differs");
        r.prim = cat({ints({1000, 31, 1000}), word(0)});
        EXPECT_FAIL(r.bytes(), 8, "packed words pass the end");
        r.prim = cat({ints({0x7FFFFFFF, 31, 0x7FFFFFFF}), word(0)});
        EXPECT_FAIL(r.bytes(), 8, "packed words pass the end");
        r.prim = cat({ints({1, 4, 1}), word(0x40000001)});
        EXPECT_FAIL(r.bytes(), 8, "unused packed bit");
        // Trianglar: L >= 3.
        r.prim = prim({2}, 4);
        EXPECT_FAIL(r.bytes(), 8, "primitive length is too short");
        r.prim = prim({3, 1}, 4);
        EXPECT_FAIL(r.bytes(), 8, "primitive length is too short");
        r.prim = prim({4, 0}, 4);
        EXPECT_FAIL(r.bytes(), 8, "primitive length is too short");
        // Summan av längderna ska vara antalet hörn.
        r.prim = prim({3}, 4);
        EXPECT_FAIL(r.bytes(), 8, "primitive lengths do not sum to the corner count");
        r.prim = prim({5}, 4);
        EXPECT_FAIL(r.bytes(), 8, "primitive lengths do not sum to the corner count");
        r.prim = prim({3, 3}, 4);
        EXPECT_FAIL(r.bytes(), 8, "primitive lengths do not sum to the corner count");

        // Linjer: L >= 2 och summan p1.
        Rec l = l1();
        l.prim = prim({1, 1}, 1);
        EXPECT_FAIL(l.bytes(), 8, "primitive length is too short");
        l.prim = prim({0, 2}, 2);
        EXPECT_FAIL(l.bytes(), 8, "primitive length is too short");
        l.prim = prim({3}, 2);
        EXPECT_FAIL(l.bytes(), 8, "primitive lengths do not sum to p1");
        l.prim = prim({1}, 2);
        EXPECT_FAIL(l.bytes(), 8, "primitive length is too short");
        l.prim = prim({2, 2}, 2);
        EXPECT_FAIL(l.bytes(), 8, "primitive lengths do not sum to p1");
        // Linjer har inget S-fält: ett S som står där är extra byte.
        l.prim = prim({2}, 2);
        l.sfield = ints({0, 0});
        EXPECT_FAIL(l.bytes(), 8, "record does not end at its length");
    }

    // S.
    {
        Rec r = r1();
        r.sfield = ints({-1});
        EXPECT_FAIL(r.bytes(), 8, "corner count is negative");
        r.sfield = ints({-2147483647 - 1});
        EXPECT_FAIL(r.bytes(), 8, "corner count is negative");
        r.sfield = cat({ints({1}), hx("00 00 00 00")});
        EXPECT_FAIL(r.bytes(), 8, "corner count is not above p1");
        r.sfield = cat({ints({4}), hx("03 00 00 00")});
        EXPECT_FAIL(r.bytes(), 8, "corner count is not above p1");
        r.sfield = ints({200});
        EXPECT_FAIL(r.bytes(), 8, "corner count is above the record length");
        r.sfield = ints({0x7FFFFFFF});
        EXPECT_FAIL(r.bytes(), 8, "corner count is above the record length");
        // S > p1 men längderna summerar till p1.
        r.sfield = cat({ints({5}), hx("03 42 00 00")});
        EXPECT_FAIL(r.bytes(), 8, "primitive lengths do not sum to the corner count");
        // Tokens: next ska bli p1.
        Rec s = strip5();
        s.sfield = cat({ints({5}), hx("04 42 00 00")});
        EXPECT_FAIL(s.bytes(), 8, "next 5 != fresh 4");
        s.sfield = cat({ints({5}), hx("02 42 41 00")});
        EXPECT_FAIL(s.bytes(), 8, "next 3 != fresh 4");
        s.sfield = cat({ints({5}), hx("03 42 00 01")});
        EXPECT_FAIL(s.bytes(), 8, "pad byte is not zero");
        s.sfield = cat({ints({5}), hx("03 D4 00 00")});
        EXPECT_FAIL(s.bytes(), 8, "lead byte is undefined");   // axelkod är bara för normaler
        s.sfield = cat({ints({5}), hx("03 F0 00 09")});
        EXPECT_FAIL(s.bytes(), 8, "literal is not below the next fresh value");
        s.sfield = ints({5});   // tokens saknas helt
        EXPECT_FAIL(s.bytes(), 8, "token count");
    }

    // Slutet.
    {
        // Utfyllnad som inte är noll: i R5 står den sista int32-nollan som utfyllnad.
        const Bytes b = r5().bytes();
        for (size_t i = 124; i < 128; ++i) {
            Bytes bad = b;
            bad[i] = 1;
            EXPECT_FAIL(bad, 8, "pad byte is not zero");
        }
        const Parsed p = run([&] { Bytes x = b; x[124] = 1; return x; }(), 8);
        CHECK(p.detail.rfind("record at byte 124: ", 0) == 0);
        const Bytes lb = l1().bytes();
        for (size_t i = 100; i < 104; ++i) {
            Bytes bad = lb;
            bad[i] = 0x80;
            EXPECT_FAIL(bad, 8, "pad byte is not zero");
        }
        // En byte extra, och fyra eller åtta nollbyte extra.
        Bytes extra = r1().bytes();
        extra.push_back(0);
        EXPECT_FAIL(extra, 8, "record does not end at its length");
        extra.insert(extra.end(), 7, 0);
        EXPECT_FAIL(extra, 8, "record does not end at its length");
        Bytes four = lb;
        four.insert(four.end(), 4, 0);
        EXPECT_FAIL(four, 8, "record does not end at its length");
        Bytes eight = lb;
        eight.insert(eight.end(), 8, 0);
        EXPECT_FAIL(eight, 8, "record does not end at its length");
        Bytes junk = lb;
        junk.push_back(0x55);
        EXPECT_FAIL(junk, 8, "record does not end at its length");
    }

    // En bredd som inte är 8 eller 16, och en tom post.
    EXPECT_FAIL(r1b, 12, "normal width");
    EXPECT_FAIL(r1b, 0, "normal width");
    {
        RecordData d;
        std::string detail;
        CHECK(parse_record(nullptr, 0, 8, d, &detail) == ErrorCode::CorruptGeometry);
        CHECK(contains(detail, "unexpected end"));
        CHECK(parse_record(r1b.data(), 0, 8, d) == ErrorCode::CorruptGeometry);
    }
}

// Varje avhuggen längd av en giltig post ger fel. Varje prefix ligger i en egen
// buffert, så att en läsning utanför den syns i en sanerare.
// ---------------------------------------------------------------------------
// Formerna: cirkel (klass 104) och cylindermantel (klass 105).

struct Vec {
    float x, y, z;
};

Bytes circle(Vec a, Vec b, float r, Vec o = {0, 0, 0}) {
    ByteWriter w;
    w.i32(100);
    w.i32(104);
    w.i32(3);
    for (const Vec v : {o, a, b}) w.f32(v.x), w.f32(v.y), w.f32(v.z);
    w.f32(r);
    w.i32(0);
    return w.data;
}

Bytes cylinder(Vec p, Vec a, Vec b, float r, Vec o = {0, 0, 0}) {
    ByteWriter w;
    w.i32(100);
    w.i32(105);
    w.i32(0x30003);
    for (const Vec v : {o, p, a, b}) w.f32(v.x), w.f32(v.y), w.f32(v.z);
    w.f32(r);
    return w.data;
}

// Varje triangel i en mantel ska vända framsidan ut från axeln, och varje
// normal ska vara en enhetsvektor vinkelrät mot axeln.
bool mantle_faces_out(const RecordData& d, Vec axis) {
    const double len =
        std::sqrt(double(axis.x) * axis.x + double(axis.y) * axis.y + double(axis.z) * axis.z);
    const double ax[3] = {axis.x / len, axis.y / len, axis.z / len};
    for (size_t t = 0; t < d.indices.size() / 3; ++t) {
        const float* p0 = &d.positions[3 * d.indices[3 * t]];
        const float* p1 = &d.positions[3 * d.indices[3 * t + 1]];
        const float* p2 = &d.positions[3 * d.indices[3 * t + 2]];
        const double u[3] = {double(p1[0]) - p0[0], double(p1[1]) - p0[1], double(p1[2]) - p0[2]};
        const double v[3] = {double(p2[0]) - p0[0], double(p2[1]) - p0[1], double(p2[2]) - p0[2]};
        const double n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
                             u[0] * v[1] - u[1] * v[0]};
        // Triangelns mitt, med delen längs axeln borttagen, pekar rakt ut.
        double mid[3];
        for (int k = 0; k < 3; ++k) mid[k] = (double(p0[k]) + p1[k] + p2[k]) / 3.0;
        const double along = mid[0] * ax[0] + mid[1] * ax[1] + mid[2] * ax[2];
        double out = 0;
        for (int k = 0; k < 3; ++k) out += n[k] * (mid[k] - along * ax[k]);
        if (!(out > 0)) return false;
    }
    for (size_t v = 0; v < d.normals.size() / 3; ++v) {
        const float* n = &d.normals[3 * v];
        const double l = double(n[0]) * n[0] + double(n[1]) * n[1] + double(n[2]) * n[2];
        const double along = n[0] * ax[0] + n[1] * ax[1] + n[2] * ax[2];
        if (std::fabs(l - 1) > 1e-5 || std::fabs(along) > 1e-5) return false;
    }
    return true;
}

Bytes cut_to(const Bytes& b, size_t size) {
    return Bytes(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(size));
}

void test_shapes(int& fails) {
    const uint32_t n = kShapeSides;
    CHECK(n == 32);

    // En cirkel i XY-planet med radien 2.
    {
        const Bytes b = circle({1, 0, 0}, {0, 1, 0}, 2);
        CHECK(b.size() == 56);
        const Parsed p = run(b, 8);
        CHECK(p.code == ErrorCode::Ok);
        const RecordData& d = p.data;
        CHECK(d.lines && !d.points && !d.box_exact && d.primitives == n && d.normal_palette == 0);
        CHECK(d.positions.size() == 3 * n && d.normals.empty() && d.colors.empty());
        CHECK(d.indices.size() == 2 * n);
        if (d.positions.size() == 3 * n && d.indices.size() == 2 * n) {
            // Början i riktningen a, sedan mot b; den sista biten sluter cirkeln.
            CHECK(near(d.positions[0], 2) && near(d.positions[1], 0) && near(d.positions[2], 0));
            CHECK(near(d.positions[3 * 8], 0) && near(d.positions[3 * 8 + 1], 2));
            CHECK(near(d.positions[3 * 16], -2) && near(d.positions[3 * 16 + 1], 0));
            CHECK(d.indices[0] == 0 && d.indices[1] == 1);
            CHECK(d.indices[2 * n - 2] == n - 1 && d.indices[2 * n - 1] == 0);
            bool on_circle = true;
            for (uint32_t i = 0; i < n; ++i)
                on_circle = on_circle &&
                            near(std::hypot(d.positions[3 * i], d.positions[3 * i + 1]), 2) &&
                            d.positions[3 * i + 2] == 0;
            CHECK(on_circle);
        }
        CHECK(d.bbox[0] == -2 && d.bbox[1] == -2 && d.bbox[2] == 0);
        CHECK(d.bbox[3] == 2 && d.bbox[4] == 2 && d.bbox[5] == 0);
        // Normalbredden spelar ingen roll.
        CHECK(run(b, 16).code == ErrorCode::Ok);
    }
    // En cirkel i ett lutande plan: lådan är den kring den omslutande kvadraten.
    {
        const float h = 0.70710678f;
        const Parsed p = run(circle({h, h, 0}, {0, 0, 1}, 1), 8);
        CHECK(p.code == ErrorCode::Ok);
        CHECK(near(p.data.bbox[0], -h) && near(p.data.bbox[1], -h) && near(p.data.bbox[2], -1));
        CHECK(near(p.data.bbox[3], h) && near(p.data.bbox[4], h) && near(p.data.bbox[5], 1));
    }

    // En mantel längs +Z: radien 1, höjden 3.
    {
        const Bytes b = cylinder({0, 0, 3}, {1, 0, 0}, {0, 1, 0}, 1);
        CHECK(b.size() == 64);
        const Parsed p = run(b, 8);
        CHECK(p.code == ErrorCode::Ok);
        const RecordData& d = p.data;
        CHECK(!d.lines && !d.points && !d.box_exact && d.primitives == 2 * n);
        CHECK(d.positions.size() == 6 * n && d.normals.size() == 6 * n);
        CHECK(d.indices.size() == 6 * n);
        if (d.positions.size() == 6 * n && d.indices.size() == 6 * n) {
            CHECK(sane(d));
            CHECK(near(d.positions[0], 1) && near(d.positions[2], 0));              // botten
            CHECK(near(d.positions[3 * n], 1) && near(d.positions[3 * n + 2], 3));   // toppen
            CHECK(near(d.normals[0], 1) && near(d.normals[1], 0) && near(d.normals[2], 0));
            CHECK(mantle_faces_out(d, {0, 0, 3}));
        }
        CHECK(d.bbox[0] == -1 && d.bbox[1] == -1 && d.bbox[2] == 0);
        CHECK(d.bbox[3] == 1 && d.bbox[4] == 1 && d.bbox[5] == 3);
    }
    // Axeln åt andra hållet än a × b: framsidan är fortfarande utåt.
    {
        const Parsed p = run(cylinder({0, 0, -3}, {1, 0, 0}, {0, 1, 0}, 1), 8);
        CHECK(p.code == ErrorCode::Ok);
        CHECK(p.data.indices.size() == 6 * n && mantle_faces_out(p.data, {0, 0, -3}));
        CHECK(p.data.bbox[2] == -3 && p.data.bbox[5] == 0);
    }
    // En mantel längs +X.
    {
        const Parsed p = run(cylinder({5, 0, 0}, {0, 1, 0}, {0, 0, 1}, 0.5f), 8);
        CHECK(p.code == ErrorCode::Ok);
        CHECK(p.data.indices.size() == 6 * n && mantle_faces_out(p.data, {5, 0, 0}));
        CHECK(p.data.bbox[0] == 0 && p.data.bbox[3] == 5 && p.data.bbox[1] == -0.5f &&
              p.data.bbox[5] == 0.5f);
    }

    // Reglerna.
    const Bytes c = circle({1, 0, 0}, {0, 1, 0}, 2);
    const Bytes y = cylinder({0, 0, 3}, {1, 0, 0}, {0, 1, 0}, 1);
    EXPECT_FAIL(with_i32(c, 8, 2), 8, "shape header");
    EXPECT_FAIL(with_i32(y, 8, 3), 8, "shape header");
    EXPECT_FAIL(with_i32(c, 52, 1), 8, "circle does not end with zero");
    EXPECT_UNSUPPORTED(circle({1, 0, 0}, {0, 1, 0}, 2, {0, 0, 1}), 8, "non-zero origin");
    EXPECT_UNSUPPORTED(cylinder({0, 0, 3}, {1, 0, 0}, {0, 1, 0}, 1, {1, 0, 0}), 8,
                       "non-zero origin");
    EXPECT_FAIL(circle({2, 0, 0}, {0, 1, 0}, 2), 8, "perpendicular unit vectors");
    EXPECT_FAIL(circle({1, 0, 0}, {0, 0.5f, 0}, 2), 8, "perpendicular unit vectors");
    EXPECT_FAIL(circle({1, 0, 0}, {1, 0, 0}, 2), 8, "perpendicular unit vectors");
    EXPECT_FAIL(circle({1, 0, 0}, {0, 1, 0}, 0), 8, "radius is not positive");
    EXPECT_FAIL(circle({1, 0, 0}, {0, 1, 0}, -1), 8, "radius is not positive");
    EXPECT_FAIL(with_i32(c, 48, 0x7F800000), 8, "not finite");   // radien oändlig
    EXPECT_FAIL(with_i32(c, 24, 0x7FC00000), 8, "not finite");   // NaN i a
    EXPECT_FAIL(cylinder({0, 0, 0}, {1, 0, 0}, {0, 1, 0}, 1), 8, "cylinder axis");
    EXPECT_FAIL(cylinder({1, 0, 3}, {1, 0, 0}, {0, 1, 0}, 1), 8, "cylinder axis");
    EXPECT_FAIL(cylinder({3, 0, 0}, {1, 0, 0}, {0, 1, 0}, 1), 8, "cylinder axis");

    // Längden: för kort, och byte efter slutet.
    for (size_t cut = 4; cut < c.size(); cut += 4)
        CHECK(run(cut_to(c, cut), 8).code == ErrorCode::CorruptGeometry);
    for (size_t cut = 4; cut < y.size(); cut += 4)
        CHECK(run(cut_to(y, cut), 8).code == ErrorCode::CorruptGeometry);
    {
        Bytes longer = c;
        longer.insert(longer.end(), 8, 0);
        EXPECT_FAIL(longer, 8, "record does not end at its length");
        longer = y;
        longer.insert(longer.end(), 8, 0);
        EXPECT_FAIL(longer, 8, "record does not end at its length");
    }
}

// En text (klass 103): sträng, 1, 1, vinkel, axel, låda, typsnitt.
Bytes text_record(const std::string& s, float angle, Vec axis, Vec lo, Vec hi, int32_t font,
                  float sx = 1, float sy = 1) {
    ByteWriter w;
    w.i32(100);
    w.i32(103);
    w.str(s);
    w.f32(sx);
    w.f32(sy);
    w.f32(angle);
    for (const Vec v : {axis, lo, hi}) w.f32(v.x), w.f32(v.y), w.f32(v.z);
    w.i32(font);
    w.pad(8);
    return w.data;
}

void test_text(int& fails) {
    {
        const Bytes b = text_record("Hall 12", 0.5f, {0, 0, 1}, {0, -71, 0}, {912, 284, 0}, 79);
        const Parsed p = run(b, 8);
        CHECK(p.code == ErrorCode::Ok);
        const RecordData& d = p.data;
        CHECK(d.text && !d.lines && !d.points && d.text_utf8 == "Hall 12");
        CHECK(d.text_angle == 0.5f && d.text_axis[2] == 1 && d.text_font == 79);
        CHECK(d.bbox[1] == -71 && d.bbox[3] == 912 && d.bbox[4] == 284 && d.bbox[5] == 0);
        CHECK(d.positions.empty() && d.indices.empty() && d.primitives == 0 && d.box_exact);
        CHECK(run(b, 16).code == ErrorCode::Ok);
    }
    {   // En tom text, och en vars längd inte är delbar med fyra eller åtta.
        const Parsed e = run(text_record("", 0, {0, 0, 1}, {0, 0, 0}, {0, 0, 0}, 0), 8);
        CHECK(e.code == ErrorCode::Ok && e.data.text && e.data.text_utf8.empty());
        for (const char* s : {"a", "ab", "abc", "abcd", "abcde", "abcdefgh", "abcdefghi"})
            CHECK(run(text_record(s, 0, {1, 0, 0}, {0, 0, 0}, {1, 1, 1}, 3), 8).data.text_utf8 == s);
    }
    const Bytes t = text_record("abcd", 0, {0, 0, 1}, {0, 0, 0}, {1, 1, 0}, 2);
    EXPECT_FAIL(with_i32(t, 8, -1), 8, "text length");
    EXPECT_FAIL(with_i32(t, 8, 100000), 8, "text length");
    EXPECT_UNSUPPORTED(text_record("abcd", 0, {0, 0, 1}, {0, 0, 0}, {1, 1, 0}, 2, 2, 1), 8,
                       "leading values");
    EXPECT_FAIL(text_record("abcd", 0, {0, 0, 2}, {0, 0, 0}, {1, 1, 0}, 2), 8, "unit vector");
    EXPECT_FAIL(text_record("abcd", 0, {0, 0, 1}, {2, 0, 0}, {1, 1, 0}, 2), 8, "inverted");
    EXPECT_FAIL(text_record("abcd", 0, {0, 0, 1}, {0, 0, 0}, {1, 1, 0}, -1), 8, "font index");
    for (size_t cut = 8; cut < t.size(); cut += 4)
        CHECK(run(cut_to(t, cut), 8).code == ErrorCode::CorruptGeometry);
    Bytes longer = t;
    longer.insert(longer.end(), 8, 0);
    EXPECT_FAIL(longer, 8, "record does not end at its length");
}

void test_truncation(int& fails) {
    struct Case {
        Bytes bytes;
        int bits;
    };
    const Case cases[] = {
        {r1().bytes(), 8},          {r5().bytes(), 8},   {r16().bytes(), 16},
        {l1().bytes(), 8},          {rich().bytes(), 8}, {strip5().bytes(), 8},
        {lines_multi().bytes(), 8}, {lines_plain().bytes(), 8},
    };
    for (const Case& c : cases) {
        CHECK(run(c.bytes, c.bits).code == ErrorCode::Ok);
        for (size_t n = 0; n < c.bytes.size(); ++n) {
            const Bytes cut(c.bytes.begin(), c.bytes.begin() + static_cast<std::ptrdiff_t>(n));
            const Parsed p = run(cut, c.bits);
            CHECK(p.code != ErrorCode::Ok);
            CHECK(p.data.positions.empty() && p.data.indices.empty());
        }
    }
}

// Varje byte i en giltig post ändras för sig. Resultatet är fel eller en post som
// uppfyller invarianterna; inget får läsa utanför bufferten eller allokera efter ett tal
// som inte prövats.
void test_mutations(int& fails) {
    struct Case {
        Bytes bytes;
        int bits;
    };
    const Case cases[] = {
        {r1().bytes(), 8},   {r5().bytes(), 8},   {r16().bytes(), 16},
        {l1().bytes(), 8},   {rich().bytes(), 8}, {strip5().bytes(), 8},
        {lines_multi().bytes(), 8}, {lines_plain().bytes(), 8},
    };
    for (const Case& c : cases) {
        for (size_t i = 0; i < c.bytes.size(); ++i) {
            const uint8_t was = c.bytes[i];
            for (const uint8_t v : {static_cast<uint8_t>(0), static_cast<uint8_t>(0xFF),
                                    static_cast<uint8_t>(was ^ 1), static_cast<uint8_t>(was + 1),
                                    static_cast<uint8_t>(0x80)}) {
                Bytes m = c.bytes;
                m[i] = v;
                const Parsed p = run(m, c.bits);
                if (p.code == ErrorCode::Ok) CHECK(sane(p.data));
                else CHECK(!p.detail.empty());
            }
        }
    }
}

void test_toc(int& fails) {
    // Giltig.
    {
        const Bytes b = toc_bytes({100, 200}, {8, 16, 24});
        CHECK(b.size() == 32);
        GeometryToc t;
        std::string detail;
        CHECK(parse_geometry_toc(b.data(), b.size(), t, &detail) == ErrorCode::Ok);
        CHECK(t.chunk_sizes == std::vector<uint32_t>({100, 200}));
        CHECK(t.record_lengths == std::vector<uint32_t>({8, 16, 24}));
    }
    // Utan utfyllnad, och med fyra byte utfyllnad.
    {
        GeometryToc t;
        const Bytes a = toc_bytes({50}, {8});    // 8 + 4 + 4 = 16
        CHECK(a.size() == 16);
        CHECK(parse_geometry_toc(a.data(), a.size(), t) == ErrorCode::Ok);
        const Bytes b = toc_bytes({}, {8});      // 12 -> 16
        CHECK(b.size() == 16);
        CHECK(parse_geometry_toc(b.data(), b.size(), t) == ErrorCode::Ok);
        CHECK(t.chunk_sizes.empty() && t.record_lengths == std::vector<uint32_t>({8}));
        const Bytes c = toc_bytes({7}, {});
        CHECK(parse_geometry_toc(c.data(), c.size(), t) == ErrorCode::Ok);
        CHECK(t.chunk_sizes == std::vector<uint32_t>({7}) && t.record_lengths.empty());
        const Bytes e = toc_bytes({}, {});       // bara huvudet
        CHECK(e.size() == 8);
        CHECK(parse_geometry_toc(e.data(), e.size(), t) == ErrorCode::Ok);
        CHECK(t.chunk_sizes.empty() && t.record_lengths.empty());
        // Största tillåtna värden.
        const Bytes big = toc_bytes({0x7FFFFFFF}, {0x7FFFFFF8});
        CHECK(parse_geometry_toc(big.data(), big.size(), t) == ErrorCode::Ok);
        CHECK(t.chunk_sizes[0] == 0x7FFFFFFFu && t.record_lengths[0] == 0x7FFFFFF8u);
    }
    // Fel längd: för kort, för lång, och inte ens ett huvud.
    {
        const Bytes b = toc_bytes({100, 200}, {8, 16, 24});
        CHECK(toc_fail(Bytes(b.begin(), b.begin() + 24), "length is not"));
        CHECK(toc_fail(Bytes(b.begin(), b.begin() + 28), "length is not"));
        Bytes longer = b;
        longer.insert(longer.end(), 8, 0);
        CHECK(toc_fail(longer, "length is not"));
        Bytes one = b;
        one.push_back(0);
        CHECK(toc_fail(one, "length is not"));
        CHECK(toc_fail(Bytes(), "header passes the end"));
        CHECK(toc_fail(Bytes(7, 0), "header passes the end"));
        for (size_t n = 0; n < b.size(); ++n) {
            GeometryToc t;
            CHECK(parse_geometry_toc(b.data(), n, t) == ErrorCode::CorruptGeometry);
        }
    }
    // Utfyllnad som inte är noll.
    {
        Bytes b = toc_bytes({100}, {8, 16});     // 8 + 4 + 8 = 20 -> 24
        CHECK(b.size() == 24);
        for (size_t i = 20; i < 24; ++i) {
            Bytes bad = b;
            bad[i] = 1;
            CHECK(toc_fail(bad, "pad byte is not zero"));
        }
        GeometryToc t;
        std::string detail;
        Bytes bad = b;
        bad[21] = 1;
        CHECK(parse_geometry_toc(bad.data(), bad.size(), t, &detail) == ErrorCode::CorruptGeometry);
        CHECK(detail.rfind("geometry TOC at byte 21: ", 0) == 0);
    }
    // Negativa antal.
    {
        Bytes b = toc_bytes({100}, {8});
        CHECK(toc_fail(with_i32(b, 0, -1), "record count is negative"));
        CHECK(toc_fail(with_i32(b, 4, -1), "chunk count is negative"));
        CHECK(toc_fail(with_i32(with_i32(b, 0, -2147483647 - 1), 4, -2147483647 - 1), "negative"));
    }
    // Antal som inte ryms i datan ger fel före varje allokering.
    {
        Bytes b = toc_bytes({100}, {8});
        CHECK(toc_fail(with_i32(b, 0, 0x7FFFFFFF), "length is not"));
        CHECK(toc_fail(with_i32(b, 4, 0x7FFFFFFF), "length is not"));
        CHECK(toc_fail(with_i32(with_i32(b, 0, 0x7FFFFFFF), 4, 0x7FFFFFFF), "length is not"));
        CHECK(toc_fail(with_i32(b, 0, 2), "length is not"));
        CHECK(toc_fail(with_i32(b, 4, 2), "length is not"));
    }
    // Chunkstorlek 0 och negativ.
    {
        CHECK(toc_fail(toc_bytes({0}, {8}), "chunk size is not positive"));
        CHECK(toc_fail(toc_bytes({100, -5}, {8}), "chunk size is not positive"));
        CHECK(toc_fail(toc_bytes({-2147483647 - 1}, {8}), "chunk size is not positive"));
        CHECK(toc_fail(toc_bytes({0}, {}), "chunk size is not positive"));
    }
    // Postlängd som inte är positiv eller inte delbar med 8.
    {
        CHECK(toc_fail(toc_bytes({100}, {0}), "record length"));
        CHECK(toc_fail(toc_bytes({100}, {-8}), "record length"));
        CHECK(toc_fail(toc_bytes({100}, {12}), "record length"));
        CHECK(toc_fail(toc_bytes({100}, {7}), "record length"));
        CHECK(toc_fail(toc_bytes({100}, {8, 16, 20}), "record length"));
        CHECK(toc_fail(toc_bytes({100}, {8, 0}), "record length"));
        CHECK(toc_fail(toc_bytes({100}, {-2147483647 - 1}), "record length"));
        // Felets plats: den tredje längden ligger på byte 8 + 4 + 8.
        const Bytes b = toc_bytes({100}, {8, 16, 20});
        GeometryToc t;
        std::string detail;
        CHECK(parse_geometry_toc(b.data(), b.size(), t, &detail) == ErrorCode::CorruptGeometry);
        CHECK(detail.rfind("geometry TOC at byte 20: ", 0) == 0);
    }
    // Utan detail.
    {
        const Bytes b = toc_bytes({0}, {8});
        GeometryToc t;
        CHECK(parse_geometry_toc(b.data(), b.size(), t) == ErrorCode::CorruptGeometry);
    }
    // En tidigare tolkning rensas.
    {
        GeometryToc t;
        t.chunk_sizes = {1, 2, 3};
        t.record_lengths = {8};
        const Bytes b = toc_bytes({0}, {8});
        CHECK(parse_geometry_toc(b.data(), b.size(), t) == ErrorCode::CorruptGeometry);
        CHECK(t.chunk_sizes.empty() && t.record_lengths.empty());
        const Bytes ok = toc_bytes({5}, {16});
        CHECK(parse_geometry_toc(ok.data(), ok.size(), t) == ErrorCode::Ok);
        CHECK(t.chunk_sizes == std::vector<uint32_t>({5}));
        const Bytes ok2 = toc_bytes({9, 9}, {});
        CHECK(parse_geometry_toc(ok2.data(), ok2.size(), t) == ErrorCode::Ok);
        CHECK(t.record_lengths.empty() && t.chunk_sizes.size() == 2);
    }
}

} // namespace

int test_geometry_record() {
    int fails = 0;
    test_tokens(fails);
    test_packed(fails);
    test_palette_normal(fails);
    test_valid_records(fails);
    test_record_rules(fails);
    test_shapes(fails);
    test_text(fails);
    test_truncation(fails);
    test_mutations(fails);
    test_toc(fails);
    std::printf("test_geometry_record: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
