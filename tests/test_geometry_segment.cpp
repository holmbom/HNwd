#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "geometry_segment.h"
#include "test_support.h"

using namespace HNwd::detail;
using testdata::Bytes;
using testdata::ByteWriter;

namespace {

// ---------------------------------------------------------------------------
// Poster. R1, R5, R16 och L1 byggs här för sig; de ger samma byte som i
// formatbeskrivningen och tolkas med de bredder som anges.

Bytes ints(std::initializer_list<int32_t> v) {
    ByteWriter w;
    for (const int32_t x : v) w.i32(x);
    return w.data;
}

Bytes cat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const Bytes& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

Bytes hx(const char* s) {
    ByteWriter w;
    w.hex(s);
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

// En strip om fyra hörn med bredd 4: P är [1][4][1] och ordet 0x40000000.
Bytes tri_record(const Bytes& normals) {
    ByteWriter w;
    w.raw(head(94, 4, {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0}));
    w.i32(0);                 // UV saknas
    w.raw(normals);
    w.i32(0);                 // färg saknas
    w.raw(ints({1, 4, 1, 0x40000000}));
    w.i32(0);                 // S = 0
    w.pad(8);
    return w.data;
}

Bytes r1() { return tri_record(cat({ints({102, 100, -4, 0}), hx("D4 D4 D4 D4")})); }

Bytes r5() {
    return tri_record(cat({ints({102, 100, -4, 1, 0x7F810000}), hx("00 82 00 00")}));
}

Bytes r16() {
    return tri_record(cat({ints({102, 100, -4, 1, 0x7FFF0000, 0}), hx("00 82 00 00")}));
}

Bytes l1() {
    ByteWriter w;
    w.raw(head(95, 2, {0, 0, 0, 10, 0, 0}));
    w.i32(0);
    w.i32(0);
    w.raw(ints({102, 58, 5634, -2, 2}));
    w.hex("FF0000FF FF0000FF");
    w.raw(ints({1, 2, 1, static_cast<int32_t>(0x80000000u)}));
    w.pad(8);
    return w.data;
}

// Byte som inte är en giltig post men går att kontrollera byte för byte.
Bytes filler(size_t n, unsigned seed) {
    Bytes b(n);
    for (size_t i = 0; i < n; ++i)
        b[i] = static_cast<uint8_t>(i * 131 + seed * 17 + (i >> 3) * 7 + (i >> 9));
    return b;
}

// ---------------------------------------------------------------------------
// Paketet.

constexpr size_t kP = 65536;   // paketets prefix: chunkstorleken i Fixed

struct Built {
    Bytes file;                      // bara paketets byte
    DbCacheEntry entry;
    std::vector<Bytes> records;      // som de ska komma ut
    std::vector<size_t> chunk_sizes; // uppackad storlek per chunk
    std::vector<size_t> chunk_start;
    size_t total = 0;
};

Bytes toc_bytes(const std::vector<int32_t>& compressed, const std::vector<int32_t>& lengths) {
    ByteWriter w;
    w.i32(static_cast<int32_t>(lengths.size()));
    w.i32(static_cast<int32_t>(compressed.size()));
    for (const int32_t c : compressed) w.i32(c);
    for (const int32_t l : lengths) w.i32(l);
    w.pad(8);
    return w.data;
}

DbCacheEntry entry_for(const Bytes& file, int32_t b) {
    DbCacheEntry e;
    e.name = "model\\LcOpNwdGeometry";
    e.flags = 1;
    e.offset = 0;
    e.length = file.size();
    e.a = 4;
    e.b = b;
    return e;
}

// Chunkar med de givna uppackade storlekarna ur `records` lagda efter varandra.
// Summan av storlekarna får skilja sig från postlängdernas: den som är kortare
// kapar följden, den som är längre fylls ut med nollor.
Built build(const std::vector<Bytes>& records, const std::vector<size_t>& chunk_sizes,
            int32_t prefix = 65536) {
    Built out;
    out.records = records;
    out.chunk_sizes = chunk_sizes;
    Bytes stream;
    std::vector<int32_t> lengths;
    for (const Bytes& r : records) {
        stream.insert(stream.end(), r.begin(), r.end());
        lengths.push_back(static_cast<int32_t>(r.size()));
    }
    out.total = stream.size();

    size_t sum = 0;
    for (const size_t s : chunk_sizes) sum += s;
    if (stream.size() < sum) stream.resize(sum, 0);

    std::vector<Bytes> chunks;
    std::vector<int32_t> compressed;
    size_t at = 0;
    for (const size_t s : chunk_sizes) {
        out.chunk_start.push_back(at);
        const Bytes c(stream.begin() + static_cast<std::ptrdiff_t>(at),
                      stream.begin() + static_cast<std::ptrdiff_t>(at + s));
        compressed.push_back(static_cast<int32_t>(testdata::zlib_store(c).size()));
        chunks.push_back(c);
        at += s;
    }
    int32_t b = 0;
    out.file = testdata::prefixed(prefix, chunks, toc_bytes(compressed, lengths), b);
    out.entry = entry_for(out.file, b);
    return out;
}

// Chunkstorlekarna enligt regeln i Fixed: P byte utom den sista, som tar resten.
std::vector<size_t> fixed_sizes(size_t total) {
    std::vector<size_t> sizes;
    for (size_t at = 0; at < total; at += kP) sizes.push_back(std::min(kP, total - at));
    return sizes;
}

SegmentLimits limits(ChunkLayout layout, size_t cache_bytes, int bits, size_t max_stream) {
    SegmentLimits l;
    l.cache_bytes = cache_bytes;
    l.normal_bits = bits;
    l.max_stream = max_stream;
    l.layout = layout;
    return l;
}

SegmentLimits scan(size_t cache_bytes = size_t(256) << 20, int bits = 0,
                   size_t max_stream = kDefaultMaxStreamBytes) {
    return limits(ChunkLayout::Scan, cache_bytes, bits, max_stream);
}

SegmentLimits fixed(size_t cache_bytes = size_t(256) << 20, int bits = 0,
                    size_t max_stream = kDefaultMaxStreamBytes) {
    return limits(ChunkLayout::Fixed, cache_bytes, bits, max_stream);
}

ErrorCode open_with(GeometrySegment& seg, const Built& b, const SegmentLimits& l,
                    std::string* detail = nullptr) {
    return seg.open(b.file.data(), b.file.size(), b.entry, l, detail);
}

ErrorCode open_with(GeometryLayout& layout, const Built& b, const SegmentLimits& l,
                    std::string* detail = nullptr) {
    return layout.open(b.file.data(), b.file.size(), b.entry, l, detail);
}

bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

bool same_bytes(const uint8_t* p, size_t n, const Bytes& want) {
    return n == want.size() && (n == 0 || std::memcmp(p, want.data(), n) == 0);
}

// Byten i post k ska vara de som byggdes.
template <class Reader>
bool same_record(Reader& r, const Built& b, uint32_t k) {
    const uint8_t* p = nullptr;
    size_t n = 0;
    std::string detail;
    if (r.record_bytes(k, p, n, &detail) != ErrorCode::Ok) {
        std::printf("  record %u: %s\n", k, detail.c_str());
        return false;
    }
    return same_bytes(p, n, b.records[k]);
}

template <class Reader>
bool all_in_order(Reader& r, const Built& b) {
    bool ok = true;
    for (uint32_t k = 0; k < b.records.size(); ++k) ok = same_record(r, b, k) && ok;
    return ok;
}

// Byte i de chunkar post k behöver: de som har något av postens byte.
size_t needed_bytes(const Built& b, uint32_t k) {
    size_t rs = 0;
    for (uint32_t i = 0; i < k; ++i) rs += b.records[i].size();
    const size_t re = rs + b.records[k].size();
    size_t sum = 0;
    for (size_t i = 0; i < b.chunk_sizes.size(); ++i) {
        const size_t cs = b.chunk_start[i], ce = cs + b.chunk_sizes[i];
        if (b.chunk_sizes[i] != 0 && cs < re && ce > rs) sum += b.chunk_sizes[i];
    }
    return sum;
}

std::vector<uint32_t> shuffled(uint32_t n, uint64_t seed) {
    std::vector<uint32_t> v(n);
    for (uint32_t i = 0; i < n; ++i) v[i] = i;
    for (uint32_t i = n; i > 1; --i) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        std::swap(v[i - 1], v[static_cast<size_t>((seed >> 33) % i)]);
    }
    return v;
}

// Post- och chunkgränser: poster om 40, 64, 120, 8, 200, 16, 96, 1000 och 24
// byte, och chunkar där
//   post 1 går över tre chunkar (och slutar på en chunkgräns),
//   chunk 3 är tom,
//   post 2 börjar precis på en chunkgräns (efter den tomma chunken),
//   post 4 går över två och post 7 över fyra.
// Chunkarna följer inte regeln i Fixed.
Built layout() {
    std::vector<Bytes> recs;
    unsigned seed = 1;
    for (const size_t n : {40, 64, 120, 8, 200, 16, 96, 1000, 24}) recs.push_back(filler(n, seed++));
    return build(recs, {50, 30, 24, 0, 200, 300, 300, 300, 364});
}

// 17 poster om 7768 byte och en om 16, sammanlagt 132072 = 2·P + 1000.
// Post 8 (62144..69912) går över gränsen mellan chunk 0 och 1, post 16
// (124288..132056) över den mellan 1 och 2, och post 17 ligger i chunk 2.
Built big(const std::vector<size_t>& chunk_sizes) {
    std::vector<Bytes> recs;
    for (unsigned i = 0; i < 17; ++i) recs.push_back(filler(7768, i + 1));
    recs.push_back(filler(16, 99));
    return build(recs, chunk_sizes);
}

// ---------------------------------------------------------------------------
// Scan: chunkar av vilken storlek som helst.

void test_scan_layout(int& fails) {
    const Built b = layout();
    CHECK(b.total == 1568);

    // open mäter chunkarna; cursorn har inte packat upp något.
    {
        GeometrySegment seg;
        std::string detail;
        CHECK(open_with(seg, b, scan(), &detail) == ErrorCode::Ok);
        CHECK(detail.empty());
        CHECK(seg.record_count() == 9);
        CHECK(seg.chunk_count() == 9);
        CHECK(seg.total_bytes() == 1568);
        CHECK(seg.sized_chunks() == 9);
        CHECK(seg.inflated_total() == 0);
        CHECK(seg.cached_bytes() == 0);
        CHECK(seg.normal_bits() == 0);
        CHECK(seg.decided_by() == -1);
        for (uint32_t i = 0; i < 9; ++i) {
            CHECK(seg.chunk_size(i) == b.chunk_sizes[i]);
            CHECK(seg.chunk_start(i) == b.chunk_start[i]);
        }
        CHECK(seg.chunk_size(9) == 0 && seg.chunk_start(9) == 0);
        CHECK(seg.layout() != nullptr && seg.layout()->layout() == ChunkLayout::Scan);
        // Den tomma chunken 3 håller ingen byte: byte 104 ligger i chunk 4.
        CHECK(seg.layout()->chunk_of(0) == 0 && seg.layout()->chunk_of(49) == 0);
        CHECK(seg.layout()->chunk_of(50) == 1 && seg.layout()->chunk_of(103) == 2);
        CHECK(seg.layout()->chunk_of(104) == 4 && seg.layout()->chunk_of(1567) == 8);

        // Varje post packar upp bara sina egna chunkar.
        CHECK(same_record(seg, b, 0));
        CHECK(seg.inflated_total() == 50);
        CHECK(same_record(seg, b, 2));
        CHECK(seg.inflated_total() == 250);
        CHECK(same_record(seg, b, 8));
        CHECK(seg.inflated_total() == 614);
        CHECK(seg.size_all() == ErrorCode::Ok);
        CHECK(seg.inflated_total() == 614);   // size_all går förbi cachen
    }

    // I ordning, med stor cache: allt packas upp en gång.
    {
        GeometrySegment seg;
        CHECK(open_with(seg, b, scan()) == ErrorCode::Ok);
        CHECK(all_in_order(seg, b));
        CHECK(seg.inflated_total() == 1568);
        CHECK(seg.cached_bytes() == 1568);
        CHECK(all_in_order(seg, b));   // andra varvet ur cachen
        CHECK(seg.inflated_total() == 1568);
    }

    // Den sista posten först.
    {
        GeometrySegment seg;
        CHECK(open_with(seg, b, scan()) == ErrorCode::Ok);
        CHECK(same_record(seg, b, 8));
        CHECK(seg.inflated_total() == 364);
        CHECK(all_in_order(seg, b));
    }

    // Slumpvis ordning ger samma byte, med stor cache och med en liten.
    for (const size_t cache : {size_t(256) << 20, size_t(700), size_t(1)}) {
        GeometrySegment seg;
        CHECK(open_with(seg, b, scan(cache)) == ErrorCode::Ok);
        for (const uint64_t seed : {1, 2, 3}) {
            bool ok = true;
            for (const uint32_t k : shuffled(9, seed)) ok = same_record(seg, b, k) && ok;
            CHECK(ok);
        }
        CHECK(seg.size_all() == ErrorCode::Ok);
    }

    // Cachetak mindre än en chunk: det som hålls är precis det posten behöver.
    {
        GeometrySegment seg;
        CHECK(open_with(seg, b, scan(1)) == ErrorCode::Ok);
        bool exact = true, ok = true;
        for (int round = 0; round < 2; ++round) {
            for (uint32_t k = 0; k < 9; ++k) {
                ok = same_record(seg, b, k) && ok;
                exact = seg.cached_bytes() == needed_bytes(b, k) && exact;
            }
        }
        CHECK(ok);
        CHECK(exact);
        // Omtagningar räknas: mer än en uppackning av varje chunk.
        CHECK(seg.inflated_total() > 1568);
        CHECK(seg.size_all() == ErrorCode::Ok);
    }

    // Samma chunkar går inte att öppna i Fixed.
    {
        GeometrySegment seg;
        std::string detail;
        CHECK(open_with(seg, b, fixed(), &detail) == ErrorCode::CorruptPackage);
        CHECK(contains(detail, "LcOpNwdGeometry at byte 0") &&
              contains(detail, "ChunkLayout::Scan"));
        CHECK(seg.record_count() == 0 && seg.layout() == nullptr);
    }
}

// Cachen kastar den som användes längst sedan.
void test_lru(int& fails) {
    std::vector<Bytes> recs;
    for (unsigned i = 0; i < 4; ++i) recs.push_back(filler(304, i));
    const Built b = build(recs, {304, 304, 304, 304});
    GeometrySegment seg;
    CHECK(open_with(seg, b, scan(608)) == ErrorCode::Ok);

    CHECK(same_record(seg, b, 3));           // bara chunk 3
    CHECK(seg.inflated_total() == 304);
    CHECK(seg.cached_bytes() == 304);
    CHECK(same_record(seg, b, 2));
    CHECK(seg.inflated_total() == 608);
    CHECK(seg.cached_bytes() == 608);        // chunk 2 och 3
    CHECK(same_record(seg, b, 2));           // ur cachen
    CHECK(seg.inflated_total() == 608);
    CHECK(same_record(seg, b, 0));           // kastar chunk 3, inte chunk 2
    CHECK(seg.inflated_total() == 912);
    CHECK(seg.cached_bytes() == 608);
    CHECK(same_record(seg, b, 2));           // fortfarande kvar
    CHECK(seg.inflated_total() == 912);
    CHECK(same_record(seg, b, 3));           // måste tas om
    CHECK(seg.inflated_total() == 1216);
    CHECK(seg.cached_bytes() == 608);
}

// Många små chunkar och poster som går över chunkgränserna.
void test_many_chunks(int& fails) {
    std::vector<Bytes> recs;
    for (unsigned i = 0; i < 4000; ++i) recs.push_back(filler(24, i));
    const Built b = build(recs, std::vector<size_t>(6000, 16));
    CHECK(b.total == 96000);

    for (const size_t cache : {size_t(256) << 20, size_t(100)}) {
        GeometrySegment seg;
        CHECK(open_with(seg, b, scan(cache)) == ErrorCode::Ok);
        CHECK(seg.chunk_count() == 6000 && seg.record_count() == 4000);
        CHECK(seg.chunk_start(5999) == 5999 * 16);
        CHECK(all_in_order(seg, b));
        bool ok = true;
        const std::vector<uint32_t> order = shuffled(4000, 7);
        for (size_t i = 0; i < 800; ++i) ok = same_record(seg, b, order[i]) && ok;
        CHECK(ok);
        CHECK(seg.size_all() == ErrorCode::Ok);
        if (cache == 100) CHECK(seg.cached_bytes() <= 100);
    }
}

// ---------------------------------------------------------------------------
// Fixed: chunk i börjar på i·P.

void test_fixed(int& fails) {
    const Built b = big({kP, kP, 1000});
    CHECK(b.total == 2 * kP + 1000);

    // open packar inte upp något, och positionerna följer av regeln.
    {
        GeometrySegment seg;
        std::string detail;
        CHECK(open_with(seg, b, fixed(), &detail) == ErrorCode::Ok);
        CHECK(detail.empty());
        CHECK(seg.chunk_count() == 3 && seg.record_count() == 18);
        CHECK(seg.sized_chunks() == 3);
        CHECK(seg.chunk_start(0) == 0 && seg.chunk_start(1) == kP && seg.chunk_start(2) == 2 * kP);
        CHECK(seg.chunk_size(0) == kP && seg.chunk_size(1) == kP && seg.chunk_size(2) == 1000);
        CHECK(seg.chunk_size(3) == 0 && seg.chunk_start(3) == 0);
        CHECK(seg.inflated_total() == 0);
        const GeometryLayout* l = seg.layout();
        CHECK(l && l->layout() == ChunkLayout::Fixed);
        CHECK(l->chunk_of(0) == 0 && l->chunk_of(kP - 1) == 0 && l->chunk_of(kP) == 1);
        CHECK(l->chunk_of(2 * kP) == 2 && l->chunk_of(2 * kP + 999) == 2);
        CHECK(l->record_start(8) == 8 * 7768 && l->record_start(18) == b.total);

        // Den sista posten nås direkt: bara den sista chunken packas upp.
        CHECK(same_record(seg, b, 17));
        CHECK(seg.inflated_total() == 1000);
        // Post 8 går över gränsen mellan chunk 0 och 1.
        CHECK(same_record(seg, b, 8));
        CHECK(seg.inflated_total() == 1000 + 2 * kP);
        // Post 16 behöver chunk 1 och 2, som redan är uppackade.
        CHECK(same_record(seg, b, 16));
        CHECK(seg.inflated_total() == 1000 + 2 * kP);
        CHECK(all_in_order(seg, b));
        CHECK(seg.size_all() == ErrorCode::Ok);
    }

    // Slumpvis ordning, med stor cache, en på drygt en chunk och en minimal.
    for (const size_t cache : {size_t(256) << 20, size_t(70000), size_t(1)}) {
        GeometrySegment seg;
        CHECK(open_with(seg, b, fixed(cache)) == ErrorCode::Ok);
        for (const uint64_t seed : {4, 5}) {
            bool ok = true;
            for (const uint32_t k : shuffled(18, seed)) ok = same_record(seg, b, k) && ok;
            CHECK(ok);
        }
        if (cache == 1) {
            bool exact = true;
            for (uint32_t k = 0; k < 18; ++k) {
                same_record(seg, b, k);
                exact = seg.cached_bytes() == needed_bytes(b, k) && exact;
            }
            CHECK(exact);
        }
    }

    // Scan mäter samma storlekar som regeln ger.
    {
        GeometrySegment f, s;
        CHECK(open_with(f, b, fixed()) == ErrorCode::Ok);
        CHECK(open_with(s, b, scan()) == ErrorCode::Ok);
        for (uint32_t i = 0; i < 3; ++i)
            CHECK(f.chunk_start(i) == s.chunk_start(i) && f.chunk_size(i) == s.chunk_size(i));
        CHECK(all_in_order(s, b));
    }

    // En chunk som inte är P byte: totalen går ihop, men uppackningen säger ifrån.
    {
        const Built c = big({kP - 8, kP + 8, 1000});
        GeometrySegment seg;
        std::string detail;
        CHECK(open_with(seg, c, fixed()) == ErrorCode::Ok);
        const uint8_t* p = nullptr;
        size_t n = 0;
        CHECK(seg.record_bytes(0, p, n, &detail) == ErrorCode::CorruptPackage);
        CHECK(contains(detail, "LcOpNwdGeometry chunk 0 at byte") &&
              contains(detail, "different size") && contains(detail, "65528"));
        CHECK(seg.record_bytes(9, p, n, &detail) == ErrorCode::CorruptPackage);
        CHECK(contains(detail, "chunk 1") && contains(detail, "more than 65536"));
        CHECK(same_record(seg, c, 17));   // chunk 2 har rätt storlek och börjar rätt
        CHECK(seg.size_all(&detail) == ErrorCode::CorruptPackage);
        CHECK(contains(detail, "chunk 0"));
        // Scan läser samma paket.
        CHECK(open_with(seg, c, scan()) == ErrorCode::Ok);
        CHECK(all_in_order(seg, c));
    }
    // Den sista chunken ger mer än resten.
    {
        const Built c = big({kP, kP, 1008});
        GeometrySegment seg;
        std::string detail;
        CHECK(open_with(seg, c, fixed()) == ErrorCode::Ok);
        CHECK(same_record(seg, c, 0));
        const uint8_t* p = nullptr;
        size_t n = 0;
        CHECK(seg.record_bytes(17, p, n, &detail) == ErrorCode::CorruptPackage);
        CHECK(contains(detail, "chunk 2"));
        CHECK(open_with(seg, c, scan(), &detail) == ErrorCode::CorruptPackage);
        CHECK(contains(detail, "chunk 2"));
    }

    // Totalen utanför (ch − 1)·P < total ≤ ch·P: fel vid open i Fixed, men Scan går.
    {
        std::string detail;
        GeometrySegment seg;
        // Större än ch·P.
        const Built over = build({filler(kP + 8, 1)}, {kP + 8});
        CHECK(open_with(seg, over, fixed(), &detail) == ErrorCode::CorruptPackage);
        CHECK(contains(detail, "1 chunks of 65536") && contains(detail, "ChunkLayout::Scan"));
        CHECK(open_with(seg, over, scan()) == ErrorCode::Ok);
        CHECK(same_record(seg, over, 0));
        // Inte större än (ch − 1)·P: den sista chunken vore tom.
        const Built under = build({filler(kP, 2)}, {kP, 0});
        CHECK(open_with(seg, under, fixed(), &detail) == ErrorCode::CorruptPackage);
        CHECK(contains(detail, "ChunkLayout::Scan"));
        CHECK(open_with(seg, under, scan()) == ErrorCode::Ok);
        CHECK(same_record(seg, under, 0));
        // Precis ch·P går.
        const Built full = build({filler(kP, 3)}, {kP});
        CHECK(open_with(seg, full, fixed()) == ErrorCode::Ok);
        CHECK(same_record(seg, full, 0));
        // Utan poster och utan chunkar går; tomma chunkar utan poster går inte.
        const Built none = build({}, {});
        CHECK(open_with(seg, none, fixed()) == ErrorCode::Ok);
        CHECK(seg.record_count() == 0 && seg.size_all() == ErrorCode::Ok);
        const Built empty = build({}, {0, 0});
        CHECK(open_with(seg, empty, fixed()) == ErrorCode::CorruptPackage);
        // Poster utan chunkar.
        const Built bare = build({filler(80, 1)}, {});
        CHECK(open_with(seg, bare, fixed()) == ErrorCode::CorruptPackage);
    }

    // Med ett tak under P binder taket: StreamTooLarge.
    {
        const Built c = big({kP, kP, 1000});
        GeometrySegment seg;
        std::string detail;
        CHECK(open_with(seg, c, fixed(1 << 20, 0, kP - 1)) == ErrorCode::Ok);
        const uint8_t* p = nullptr;
        size_t n = 0;
        CHECK(seg.record_bytes(0, p, n, &detail) == ErrorCode::StreamTooLarge);
        CHECK(contains(detail, "chunk 0"));
        CHECK(same_record(seg, c, 17));   // den sista chunken ryms
    }

    // En trasig chunk: kontrollsumman är fel.
    {
        Built c = build({filler(80, 1), filler(80, 2)}, {160});
        c.file[4 + testdata::zlib_store(filler(160, 0)).size() - 1] ^= 0xFF;
        GeometrySegment seg;
        std::string detail;
        CHECK(open_with(seg, c, fixed()) == ErrorCode::Ok);
        const uint8_t* p = nullptr;
        size_t n = 0;
        CHECK(seg.record_bytes(0, p, n, &detail) == ErrorCode::CorruptStream);
        CHECK(contains(detail, "chunk 0"));
        CHECK(seg.size_all() == ErrorCode::CorruptStream);
    }
}

// Två cursorer över samma layout har var sin cache och stör inte varandra.
void test_cursors(int& fails) {
    const Built b = big({kP, kP, 1000});
    GeometryLayout layout;
    CHECK(open_with(layout, b, fixed()) == ErrorCode::Ok);
    GeometryCursor small(layout, b.file.data(), b.file.size(), 1);
    GeometryCursor large(layout, b.file.data(), b.file.size(), size_t(256) << 20);

    // Pekaren från den ena gäller kvar när den andra läser.
    const uint8_t* ps = nullptr;
    const uint8_t* pl = nullptr;
    size_t ns = 0, nl = 0;
    CHECK(small.record_bytes(0, ps, ns) == ErrorCode::Ok);
    CHECK(large.record_bytes(17, pl, nl) == ErrorCode::Ok);
    CHECK(same_bytes(ps, ns, b.records[0]));
    CHECK(same_bytes(pl, nl, b.records[17]));
    CHECK(small.inflated_total() == kP && large.inflated_total() == 1000);
    CHECK(small.cached_bytes() == kP && large.cached_bytes() == 1000);

    // Omväxlande, i slumpvis ordning: båda ger de byte som byggdes.
    bool ok = true;
    for (int round = 0; round < 3; ++round) {
        for (const uint32_t k : shuffled(18, static_cast<uint64_t>(round + 11))) {
            ok = same_record(small, b, k) && ok;
            ok = same_record(large, b, k) && ok;
        }
    }
    CHECK(ok);
    CHECK(large.inflated_total() == b.total);   // varje chunk en gång
    CHECK(small.inflated_total() > b.total);    // omtagningar
    CHECK(small.cached_bytes() <= 2 * kP);
    CHECK(layout.chunk_count() == 3 && layout.normal_bits() == 0);

    // En cursor utan layout har inga poster, och parse kräver en avgjord bredd.
    GeometryCursor none;
    const uint8_t* p = nullptr;
    size_t n = 0;
    CHECK(none.record_bytes(0, p, n) == ErrorCode::OutOfRange);
    RecordData rd;
    std::string detail;
    CHECK(small.parse(0, rd, &detail) == ErrorCode::NotLoaded);
    CHECK(contains(detail, "normal width"));
    CHECK(small.parse(18, rd) == ErrorCode::OutOfRange);
}

// Paketets ram och chunkarnas storlekar.
void test_errors(int& fails) {
    const std::vector<Bytes> two = {filler(80, 1), filler(80, 2)};
    std::string detail;

    // Fel prefix.
    {
        const Built b = build(two, {80, 80}, 65535);
        GeometrySegment seg;
        CHECK(open_with(seg, b, scan(), &detail) == ErrorCode::CorruptPackage);
        CHECK(contains(detail, "LcOpNwdGeometry") && contains(detail, "65536"));
        CHECK(seg.record_count() == 0 && seg.chunk_count() == 0);
    }

    // Paket av fel form, och ett som pekar utanför filen.
    {
        const Built b = build(two, {80, 80});
        GeometrySegment seg;
        DbCacheEntry e = b.entry;
        e.a = 0;
        e.b = 0;
        CHECK(seg.open(b.file.data(), b.file.size(), e, scan(), &detail) == ErrorCode::CorruptPackage);
        CHECK(contains(detail, "LcOpNwdGeometry"));
        e = b.entry;
        e.b = 0;
        CHECK(seg.open(b.file.data(), b.file.size(), e, scan()) == ErrorCode::CorruptPackage);
        e = b.entry;
        e.length = b.file.size() + 1;
        CHECK(seg.open(b.file.data(), b.file.size(), e, scan()) == ErrorCode::CorruptPackage);
        e = b.entry;
        e.offset = ~uint64_t(0) - 2;
        CHECK(seg.open(b.file.data(), b.file.size(), e, scan()) == ErrorCode::CorruptPackage);
        e = b.entry;
        e.b = static_cast<int32_t>(e.length);   // prefixet ryms inte före den sista strömmen
        CHECK(seg.open(b.file.data(), b.file.size(), e, scan()) == ErrorCode::CorruptPackage);
        CHECK(seg.open(nullptr, 0, b.entry, scan()) == ErrorCode::CorruptPackage);
        CHECK(seg.open(b.file.data(), b.file.size(), b.entry, scan(1 << 20, 12), &detail) ==
              ErrorCode::OutOfRange);
        CHECK(contains(detail, "normal_bits"));
        // Efter ett misslyckat open är segmentet tomt.
        CHECK(seg.record_count() == 0);
        const uint8_t* p = nullptr;
        size_t n = 0;
        CHECK(seg.record_bytes(0, p, n) == ErrorCode::OutOfRange);
        CHECK(seg.resolve_normal_bits() == ErrorCode::NotLoaded);
        CHECK(seg.size_all() == ErrorCode::Ok);
        // Likaså en layout.
        GeometryLayout layout;
        CHECK(open_with(layout, b, scan()) == ErrorCode::Ok);
        CHECK(layout.record_count() == 2);
        CHECK(layout.open(nullptr, 0, b.entry, scan()) == ErrorCode::CorruptPackage);
        CHECK(layout.record_count() == 0 && layout.chunk_count() == 0);
    }

    // Innehållsförteckningen bryter grammatiken.
    {
        const Built b = build(two, {80, 80});
        std::vector<Bytes> chunks = {filler(80, 1), filler(80, 2)};
        const int32_t z0 = static_cast<int32_t>(testdata::zlib_store(chunks[0]).size());
        const int32_t z1 = static_cast<int32_t>(testdata::zlib_store(chunks[1]).size());
        int32_t bb = 0;
        // Postlängd 12 är inte delbar med 8.
        Bytes file = testdata::prefixed(65536, chunks, toc_bytes({z0, z1}, {80, 12}), bb);
        GeometrySegment seg;
        CHECK(seg.open(file.data(), file.size(), entry_for(file, bb), scan(), &detail) ==
              ErrorCode::CorruptGeometry);
        CHECK(contains(detail, "LcOpNwdGeometry") && contains(detail, "multiple of 8"));
        // Utfyllnaden är inte noll: en postlängd och två chunkar ger 20 byte och 4 i utfyllnad.
        Bytes toc = toc_bytes({z0, z1}, {160});
        CHECK(toc.size() == 24);
        toc.back() = 1;
        file = testdata::prefixed(65536, chunks, toc, bb);
        CHECK(seg.open(file.data(), file.size(), entry_for(file, bb), scan(), &detail) ==
              ErrorCode::CorruptGeometry);
        CHECK(contains(detail, "pad byte"));
        // Längden stämmer inte med antalen.
        toc = toc_bytes({z0, z1}, {80, 80});
        toc.resize(toc.size() + 8, 0);
        file = testdata::prefixed(65536, chunks, toc, bb);
        CHECK(seg.open(file.data(), file.size(), entry_for(file, bb), scan(), &detail) ==
              ErrorCode::CorruptGeometry);
        // En chunkstorlek 0.
        file = testdata::prefixed(65536, chunks, toc_bytes({z0, 0}, {80, 80}), bb);
        CHECK(seg.open(file.data(), file.size(), entry_for(file, bb), scan()) ==
              ErrorCode::CorruptGeometry);
        // Innehållsförteckningen är en trasig zlib-ström.
        file = b.file;
        file[file.size() - 1] ^= 0xFF;
        CHECK(seg.open(file.data(), file.size(), b.entry, scan(), &detail) ==
              ErrorCode::CorruptStream);
        CHECK(contains(detail, "LcOpNwdGeometry"));
        // Innehållsförteckningen är större än max_stream.
        CHECK(open_with(seg, b, scan(1 << 20, 0, 8)) == ErrorCode::StreamTooLarge);
    }

    // Chunklängderna går inte ihop med paketets längd.
    {
        const std::vector<Bytes> chunks = {filler(80, 1), filler(80, 2)};
        const int32_t z0 = static_cast<int32_t>(testdata::zlib_store(chunks[0]).size());
        const int32_t z1 = static_cast<int32_t>(testdata::zlib_store(chunks[1]).size());
        int32_t bb = 0;
        GeometrySegment seg;
        Bytes file = testdata::prefixed(65536, chunks, toc_bytes({z0 + 1, z1}, {80, 80}), bb);
        CHECK(seg.open(file.data(), file.size(), entry_for(file, bb), scan(), &detail) ==
              ErrorCode::CorruptPackage);
        CHECK(contains(detail, "LcOpNwdGeometry"));
        file = testdata::prefixed(65536, chunks, toc_bytes({z0, z1 - 1}, {80, 80}), bb);
        CHECK(seg.open(file.data(), file.size(), entry_for(file, bb), scan()) ==
              ErrorCode::CorruptPackage);
        // Bara en av två chunkar i förteckningen.
        file = testdata::prefixed(65536, chunks, toc_bytes({z0}, {80, 80}), bb);
        CHECK(seg.open(file.data(), file.size(), entry_for(file, bb), scan()) ==
              ErrorCode::CorruptPackage);
    }

    const uint8_t* p = nullptr;
    size_t n = 0;

    // Scan: chunkarna ger mer än totalen, i den sista...
    {
        const Built b = build(two, {100, 100});   // 200 byte mot 160
        GeometrySegment seg;
        CHECK(open_with(seg, b, scan(), &detail) == ErrorCode::CorruptPackage);
        CHECK(contains(detail, "chunk 1") && contains(detail, "LcOpNwdGeometry"));
        CHECK(seg.record_count() == 0);
        // Två chunkar för 160 byte bryter regeln i Fixed.
        CHECK(open_with(seg, b, fixed(), &detail) == ErrorCode::CorruptPackage);
        CHECK(contains(detail, "ChunkLayout::Scan"));
    }
    // ...eller i den första.
    {
        const Built b = build(two, {200, 10});
        GeometrySegment seg;
        CHECK(open_with(seg, b, scan(), &detail) == ErrorCode::CorruptPackage);
        CHECK(contains(detail, "chunk 0"));
    }

    // Scan: chunkarna ger mindre än totalen.
    {
        const Built b = build(two, {90, 20});   // 110 byte mot 160
        GeometrySegment seg;
        CHECK(open_with(seg, b, scan(), &detail) == ErrorCode::CorruptPackage);
        CHECK(contains(detail, "LcOpNwdGeometry") && contains(detail, "end before"));
    }
    // Inga chunkar alls.
    {
        const Built b = build(two, {});
        GeometrySegment seg;
        CHECK(open_with(seg, b, scan()) == ErrorCode::CorruptPackage);
        // Utan poster och utan chunkar är allt som det ska.
        const Built none = build({}, {});
        CHECK(open_with(seg, none, scan()) == ErrorCode::Ok);
        CHECK(seg.record_count() == 0 && seg.total_bytes() == 0);
        CHECK(seg.size_all() == ErrorCode::Ok);
        CHECK(seg.record_bytes(0, p, n) == ErrorCode::OutOfRange);
    }
    // Chunkar som bara är tomma: ingen post kan ligga i dem.
    {
        const Built b = build({}, {0, 0});
        GeometrySegment seg;
        CHECK(open_with(seg, b, scan()) == ErrorCode::Ok);
        CHECK(seg.size_all() == ErrorCode::Ok);
        CHECK(seg.sized_chunks() == 2 && seg.chunk_size(0) == 0);
    }
    // En tom chunk efter det som posterna behöver är tillåten; en med data är det inte.
    {
        const Built b = build(two, {160, 0});
        GeometrySegment seg;
        CHECK(open_with(seg, b, scan()) == ErrorCode::Ok);
        CHECK(all_in_order(seg, b));
        CHECK(seg.size_all() == ErrorCode::Ok);
        const Built c = build(two, {160, 8});
        CHECK(open_with(seg, c, scan(), &detail) == ErrorCode::CorruptPackage);
        CHECK(contains(detail, "chunk 1"));
    }

    // max_stream för litet ger StreamTooLarge när det är det som binder...
    {
        const std::vector<Bytes> one = {filler(400, 1)};
        const Built b = build(one, {400});
        GeometrySegment seg;
        CHECK(open_with(seg, b, scan(1 << 20, 0, 399), &detail) == ErrorCode::StreamTooLarge);
        CHECK(contains(detail, "LcOpNwdGeometry"));
        CHECK(open_with(seg, b, fixed(1 << 20, 0, 399)) == ErrorCode::Ok);
        CHECK(seg.record_bytes(0, p, n, &detail) == ErrorCode::StreamTooLarge);
        CHECK(seg.size_all() == ErrorCode::StreamTooLarge);
        CHECK(open_with(seg, b, scan(1 << 20, 0, 400)) == ErrorCode::Ok);
        CHECK(same_record(seg, b, 0));
        // ...men CorruptPackage när totalen binder lika hårt: chunken är större än posterna.
        const Built c = build(one, {408});
        CHECK(open_with(seg, c, scan(1 << 20, 0, 400)) == ErrorCode::CorruptPackage);
        CHECK(open_with(seg, c, fixed(1 << 20, 0, 400)) == ErrorCode::Ok);
        CHECK(seg.record_bytes(0, p, n) == ErrorCode::CorruptPackage);
        // Taket gäller det som återstår: den första chunken har 200 byte kvar.
        const Built d = build({filler(200, 1)}, {100, 100});
        CHECK(open_with(seg, d, scan(1 << 20, 0, 99)) == ErrorCode::StreamTooLarge);
    }

    // Post utanför.
    {
        const Built b = build(two, {80, 80});
        GeometrySegment seg;
        CHECK(open_with(seg, b, scan()) == ErrorCode::Ok);
        CHECK(seg.record_bytes(2, p, n, &detail) == ErrorCode::OutOfRange);
        CHECK(contains(detail, "LcOpNwdGeometry"));
        CHECK(seg.record_bytes(0xFFFFFFFFu, p, n) == ErrorCode::OutOfRange);
        RecordData rd;
        CHECK(open_with(seg, b, scan(1 << 20, 16)) == ErrorCode::Ok);
        CHECK(seg.parse(2, rd) == ErrorCode::OutOfRange);
    }

    // Scan: en trasig chunk ger felet redan vid open.
    {
        Built b = build(two, {80, 80});
        const size_t z0 = testdata::zlib_store(filler(80, 1)).size();
        b.file[4 + z0 - 1] ^= 0xFF;
        GeometrySegment seg;
        CHECK(open_with(seg, b, scan(), &detail) == ErrorCode::CorruptStream);
        CHECK(contains(detail, "chunk 0"));
    }

    // En chunk som tas om och ger en annan storlek än layouten säger. Den
    // lagrade strömmen på 64 byte och en med två block om 59 byte är lika långa.
    {
        const Built b = build({filler(64, 1), filler(64, 2)}, {64, 64});
        Built m = b;
        GeometrySegment seg;
        CHECK(open_with(seg, m, scan(1)) == ErrorCode::Ok);
        CHECK(same_record(seg, m, 0));
        CHECK(same_record(seg, m, 1));    // chunk 0 kastas
        CHECK(seg.cached_bytes() == 64);
        const Bytes other = testdata::zlib_store(filler(59, 9), 58);
        CHECK(other.size() == testdata::zlib_store(filler(64, 1)).size());
        std::copy(other.begin(), other.end(), m.file.begin() + 4);
        CHECK(seg.record_bytes(0, p, n, &detail) == ErrorCode::CorruptPackage);
        CHECK(contains(detail, "chunk 0") && contains(detail, "different size"));
        // Chunk 1 var kvar i cachen och påverkas inte.
        CHECK(same_record(seg, m, 1));
    }
}

// ---------------------------------------------------------------------------
// Normalbredden.

// Posterna, lagda i chunkar som skär dem.
Built width_built(const std::vector<Bytes>& recs) {
    size_t total = 0;
    for (const Bytes& r : recs) total += r.size();
    return build(recs, {total / 3, total / 3 + 8, total - total / 3 - total / 3 - 8});
}

bool vec_eq(const std::vector<uint32_t>& a, std::initializer_list<uint32_t> b) {
    return a == std::vector<uint32_t>(b);
}

void test_width(int& fails) {
    std::string detail;
    RecordData rd;

    // Posterna i sig själva.
    CHECK(r1().size() == 120 && r5().size() == 128 && r16().size() == 128 && l1().size() == 104);

    // R1 + R5: R5 avgör med åtta bitar.
    {
        const Built b = width_built({r1(), r5()});
        GeometrySegment seg;
        CHECK(open_with(seg, b, scan()) == ErrorCode::Ok);
        CHECK(seg.normal_bits() == 0 && seg.decided_by() == -1);
        CHECK(seg.resolve_normal_bits() == ErrorCode::Ok);
        CHECK(seg.normal_bits() == 8 && seg.decided_by() == 1);
        CHECK(seg.resolve_normal_bits() == ErrorCode::Ok);   // en gång till ändrar inget
        CHECK(seg.normal_bits() == 8 && seg.decided_by() == 1);
        CHECK(seg.parse(0, rd, &detail) == ErrorCode::Ok);
        CHECK(rd.primitives == 2 && vec_eq(rd.indices, {0, 1, 2, 2, 1, 3}) && !rd.lines);
        CHECK(seg.parse(1, rd, &detail) == ErrorCode::Ok);
        CHECK(rd.normal_palette == 1 && rd.normals.size() == 12);
        CHECK(rd.normals[0] > 0.70f && rd.normals[0] < 0.71f && rd.normals[1] < -0.70f);
        CHECK(seg.size_all() == ErrorCode::Ok);
    }
    // Samma i Fixed, med posterna i en chunk.
    {
        const Built b = build({r1(), r5()}, fixed_sizes(248));
        GeometrySegment seg;
        CHECK(open_with(seg, b, fixed()) == ErrorCode::Ok);
        CHECK(seg.parse(1, rd, &detail) == ErrorCode::Ok);
        CHECK(seg.normal_bits() == 8 && seg.decided_by() == 1);
    }
    // parse avgör bredden först när det inte är gjort.
    {
        const Built b = width_built({r1(), r5()});
        GeometrySegment seg;
        CHECK(open_with(seg, b, scan()) == ErrorCode::Ok);
        CHECK(seg.parse(0, rd, &detail) == ErrorCode::Ok);
        CHECK(seg.normal_bits() == 8 && seg.decided_by() == 1);
    }
    // R16 + R5: R16 avgör med sexton, och R5 går bara med åtta.
    {
        const Built b = width_built({r16(), r5()});
        GeometrySegment seg;
        CHECK(open_with(seg, b, scan()) == ErrorCode::Ok);
        CHECK(seg.resolve_normal_bits() == ErrorCode::Ok);
        CHECK(seg.normal_bits() == 16 && seg.decided_by() == 0);
        CHECK(seg.parse(0, rd, &detail) == ErrorCode::Ok);
        CHECK(rd.normals.size() == 12 && rd.normals[0] > 0.99f);
        CHECK(seg.parse(1, rd, &detail) == ErrorCode::CorruptGeometry);
        CHECK(contains(detail, "parses only under the other normal width"));
        CHECK(contains(detail, "LcOpNwdGeometry") && contains(detail, "record 1"));
        // Den som går med den avgjorda bredden fungerar fortfarande.
        CHECK(seg.parse(0, rd) == ErrorCode::Ok);
    }
    // Samma, men R5 först: åtta avgör, och R16 är den som inte går med.
    {
        const Built b = width_built({r5(), r16()});
        GeometrySegment seg;
        CHECK(open_with(seg, b, scan()) == ErrorCode::Ok);
        CHECK(seg.parse(1, rd, &detail) == ErrorCode::CorruptGeometry);
        CHECK(contains(detail, "parses only under the other normal width"));
        CHECK(seg.normal_bits() == 8 && seg.decided_by() == 0);
    }
    // Bara poster som går med båda: sexton, och ingen avgjorde.
    {
        const Built b = width_built({r1(), l1(), r1()});
        GeometrySegment seg;
        CHECK(open_with(seg, b, scan()) == ErrorCode::Ok);
        CHECK(seg.resolve_normal_bits() == ErrorCode::Ok);
        CHECK(seg.normal_bits() == 16 && seg.decided_by() == -1);
        CHECK(seg.parse(1, rd) == ErrorCode::Ok && rd.lines && rd.primitives == 1);
        CHECK(vec_eq(rd.colors, {0xFF0000FFu, 0xFF0000FFu}));
        const Built one = width_built({r1()});
        CHECK(open_with(seg, one, scan()) == ErrorCode::Ok);
        CHECK(seg.resolve_normal_bits() == ErrorCode::Ok);
        CHECK(seg.normal_bits() == 16 && seg.decided_by() == -1);
        // Ett segment utan poster.
        const Built none = build({}, {});
        CHECK(open_with(seg, none, scan()) == ErrorCode::Ok);
        CHECK(seg.resolve_normal_bits() == ErrorCode::Ok);
        CHECK(seg.normal_bits() == 16 && seg.decided_by() == -1);
    }
    // Tvingad bredd: ingen prövning, och felet är postens eget.
    {
        const Built b = width_built({r1(), r5()});
        GeometrySegment seg;
        CHECK(open_with(seg, b, scan(1 << 20, 16)) == ErrorCode::Ok);
        CHECK(seg.resolve_normal_bits() == ErrorCode::Ok);
        CHECK(seg.normal_bits() == 16 && seg.decided_by() == -1);
        CHECK(seg.parse(0, rd) == ErrorCode::Ok);
        CHECK(seg.parse(1, rd, &detail) == ErrorCode::CorruptGeometry);
        CHECK(contains(detail, "unused packed bit") && contains(detail, "record 1"));
        CHECK(!contains(detail, "other normal width"));

        CHECK(open_with(seg, b, scan(1 << 20, 8)) == ErrorCode::Ok);
        CHECK(seg.parse(0, rd) == ErrorCode::Ok);
        CHECK(seg.parse(1, rd) == ErrorCode::Ok);
        CHECK(seg.normal_bits() == 8 && seg.decided_by() == -1);

        const Built c = width_built({r16()});
        CHECK(open_with(seg, c, scan(1 << 20, 8)) == ErrorCode::Ok);
        CHECK(seg.parse(0, rd, &detail) == ErrorCode::CorruptGeometry);
        CHECK(contains(detail, "next 4 != fresh 1"));
    }
    // En post som inte går med någon bredd hoppas över i avgörandet, och ger
    // sitt fel med postnumret när den efterfrågas.
    {
        Bytes bad = r1();
        bad[4] = 93;   // klassen är varken 94, 95 eller 96
        for (const bool scanned : {true, false}) {
            const Built b = scanned ? width_built({r1(), bad, r5()})
                                    : build({r1(), bad, r5()}, fixed_sizes(368));
            GeometrySegment seg;
            CHECK(open_with(seg, b, scanned ? scan() : fixed()) == ErrorCode::Ok);
            CHECK(seg.resolve_normal_bits(&detail) == ErrorCode::Ok);
            CHECK(seg.normal_bits() == 8 && seg.decided_by() == 2);
            CHECK(seg.parse(0, rd) == ErrorCode::Ok);
            CHECK(seg.parse(1, rd, &detail) == ErrorCode::CorruptGeometry);
            CHECK(contains(detail, "record 1") && contains(detail, "class is not 94, 95, 96"));
            CHECK(seg.parse(2, rd) == ErrorCode::Ok);
        }
        // Bara trasiga poster: ingen avgjorde.
        const Built only = width_built({bad, bad});
        GeometrySegment seg;
        CHECK(open_with(seg, only, scan()) == ErrorCode::Ok);
        CHECK(seg.resolve_normal_bits() == ErrorCode::Ok);
        CHECK(seg.normal_bits() == 16 && seg.decided_by() == -1);
        CHECK(seg.parse(0, rd) == ErrorCode::CorruptGeometry);
    }
    // En post över taket hoppas också över, och ger LimitExceeded när den efterfrågas.
    {
        const Built b = build({r5(), r1()}, fixed_sizes(248));
        SegmentLimits l = fixed();
        l.max_record = 120;
        GeometrySegment seg;
        CHECK(open_with(seg, b, l) == ErrorCode::Ok);
        CHECK(seg.resolve_normal_bits() == ErrorCode::Ok);
        CHECK(seg.normal_bits() == 16 && seg.decided_by() == -1);
        CHECK(seg.parse(0, rd) == ErrorCode::LimitExceeded);
        CHECK(seg.parse(1, rd) == ErrorCode::Ok);
    }
    // En post som inte avkodas (kvantiserade positioner): den avgör ingen bredd
    // och ger Unsupported när den efterfrågas.
    {
        Bytes pts = r1();
        pts[16] = 0xFC;   // antalet render-vertexar -4
        pts[17] = pts[18] = pts[19] = 0xFF;
        const Built b = width_built({pts, r5()});
        GeometrySegment seg;
        CHECK(open_with(seg, b, scan()) == ErrorCode::Ok);
        CHECK(seg.resolve_normal_bits() == ErrorCode::Ok);
        CHECK(seg.normal_bits() == 8 && seg.decided_by() == 1);
        CHECK(seg.parse(0, rd, &detail) == ErrorCode::Unsupported);
        CHECK(contains(detail, "LcOpNwdGeometry") && contains(detail, "record 0"));
        CHECK(seg.parse(1, rd) == ErrorCode::Ok);
        const Built only = width_built({pts});
        CHECK(open_with(seg, only, scan()) == ErrorCode::Ok);
        CHECK(seg.resolve_normal_bits() == ErrorCode::Ok);
        CHECK(seg.normal_bits() == 16 && seg.decided_by() == -1);
        CHECK(seg.parse(0, rd) == ErrorCode::Unsupported);
    }
    // Ett fel i paketet under avgörandet lämnas vidare.
    {
        Built b = build({r1(), r5()}, fixed_sizes(248));
        b.file[4 + testdata::zlib_store(Bytes(248)).size() - 1] ^= 0xFF;
        GeometrySegment seg;
        CHECK(open_with(seg, b, fixed()) == ErrorCode::Ok);
        CHECK(seg.resolve_normal_bits(&detail) == ErrorCode::CorruptStream);
        CHECK(contains(detail, "chunk 0"));
        CHECK(seg.normal_bits() == 0);
        CHECK(seg.parse(1, rd) == ErrorCode::CorruptStream);
    }
}

// Avgörandet med förlopp, och avbrott genom det.
void test_progress(int& fails) {
    const std::vector<Bytes> recs(3000, r1());   // går med båda bredderna: alla prövas
    const Built b = build(recs, fixed_sizes(3000 * 120));
    CHECK(b.chunk_sizes.size() == 6);
    std::vector<std::pair<uint64_t, uint64_t>> calls;
    std::string detail;

    GeometryLayout layout;
    CHECK(open_with(layout, b, fixed()) == ErrorCode::Ok);

    // Avbrutet vid första anropet.
    CHECK(layout.decide_normal_bits(
              [&](uint64_t done, uint64_t total) {
                  calls.emplace_back(done, total);
                  return false;
              },
              &detail) == ErrorCode::Cancelled);
    CHECK(calls.size() == 1 && calls[0] == std::make_pair(uint64_t(0), uint64_t(3000)));
    CHECK(contains(detail, "LcOpNwdGeometry") && contains(detail, "cancelled"));
    CHECK(layout.normal_bits() == 0);

    // Avbrutet mitt i.
    calls.clear();
    CHECK(layout.decide_normal_bits([&](uint64_t done, uint64_t total) {
        calls.emplace_back(done, total);
        return done < 2048;
    }) == ErrorCode::Cancelled);
    CHECK(calls.size() == 3 && calls[1].first == 1024 && calls[2].first == 2048);
    CHECK(layout.normal_bits() == 0);

    // Avbrutet vid sista anropet: bredden är fortfarande inte avgjord.
    calls.clear();
    CHECK(layout.decide_normal_bits([&](uint64_t done, uint64_t total) {
        calls.emplace_back(done, total);
        return done < total;
    }) == ErrorCode::Cancelled);
    CHECK(calls.size() == 4 && calls.back() == std::make_pair(uint64_t(3000), uint64_t(3000)));
    CHECK(layout.normal_bits() == 0);

    // Hela vägen.
    calls.clear();
    CHECK(layout.decide_normal_bits([&](uint64_t done, uint64_t total) {
        calls.emplace_back(done, total);
        return true;
    }) == ErrorCode::Ok);
    CHECK(calls.size() == 4 && calls.back().first == 3000);
    CHECK(layout.normal_bits() == 16 && layout.decided_by() == -1);

    // Avgjord: inga fler anrop.
    calls.clear();
    CHECK(layout.decide_normal_bits([&](uint64_t done, uint64_t total) {
        calls.emplace_back(done, total);
        return false;
    }) == ErrorCode::Ok);
    CHECK(calls.empty());

    // Nu går det att tolka genom en cursor.
    GeometryCursor cursor(layout, b.file.data(), b.file.size(), 0);
    RecordData rd;
    CHECK(cursor.parse(2999, rd) == ErrorCode::Ok && rd.primitives == 2);

    // Utan förlopp, och med tvingad bredd: inga anrop.
    GeometryLayout plain;
    CHECK(open_with(plain, b, fixed()) == ErrorCode::Ok);
    CHECK(plain.decide_normal_bits({}) == ErrorCode::Ok && plain.normal_bits() == 16);
    GeometryLayout forced;
    CHECK(open_with(forced, b, fixed(1 << 20, 8)) == ErrorCode::Ok);
    CHECK(forced.normal_bits() == 0);
    CHECK(forced.decide_normal_bits([&](uint64_t done, uint64_t total) {
        calls.emplace_back(done, total);
        return false;
    }) == ErrorCode::Ok);
    CHECK(calls.empty() && forced.normal_bits() == 8);
}

} // namespace

int test_geometry_segment() {
    int fails = 0;
    test_scan_layout(fails);
    test_lru(fails);
    test_many_chunks(fails);
    test_fixed(fails);
    test_cursors(fails);
    test_errors(fails);
    test_width(fails);
    test_progress(fails);
    std::printf("test_geometry_segment: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
