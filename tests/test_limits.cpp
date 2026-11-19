// Tak och gränsfall: det som håller en trasig eller illasinnad fil från att
// kosta obegränsat med tid eller minne, och formatets ytterkanter.

#include <cstdio>
#include <string>
#include <vector>

#include "HNwd.h"
#include "container.h"
#include "fragment_view.h"
#include "fragments.h"
#include "geometry_segment.h"
#include "inflate.h"
#include "path_utf8.h"
#include "test_data.h"
#include "test_support.h"

using namespace HNwd::detail;
using testdata::Bytes;
using testdata::ByteWriter;

namespace {

// Skriver en fil till testernas temporärkatalog. Tom sträng om det inte gick.
std::string write_temp(const char* name, const Bytes& bytes) {
    const std::string path = testdata::tmp_path(name);
#ifdef _WIN32
    std::FILE* f = _wfopen(utf8_to_wide(path).c_str(), L"wb");
#else
    std::FILE* f = std::fopen(path.c_str(), "wb");
#endif
    if (!f) return std::string();
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    return std::fclose(f) == 0 && ok ? path : std::string();
}

// Ett fragment utan pose som pekar bakåt på ett utseende och en geometri.
void shared_fragment(ByteWriter& w, int32_t seq, int32_t look, int32_t geometry) {
    w.obj(13);
    w.f64(1.0);
    w.i32(2);
    w.i32(4);
    w.i32(0);
    w.i32(seq);
    w.null_ptr();
    w.ref(look);
    w.ref(geometry);
}

} // namespace

int test_limits() {
    int fails = 0;

    // En ström som är trasig i sitt första block får inte kosta en buffert i
    // proportion till det som råkar följa efter den i filen.
    {
        Bytes z = {0x78, 0x01, 0x07};   // blocktyp 3 finns inte
        z.resize(4u << 20, 0);
        Bytes out;
        CHECK(inflate_zlib(z.data(), z.size(), out, 16u << 20) == ErrorCode::CorruptStream);
        CHECK(out.capacity() <= 65536);
    }

    // En fil full av rubriker som inte följs av en katalog: varje rubrik
    // prövas och förkastas, och svaret är att katalogen saknas.
    {
        ByteWriter w;
        w.text(testdata::kDataHeader);
        for (int i = 0; i < 4000; ++i) {
            w.text("#LcUStream\n");
            w.u8(0x78);
            w.u8(0x01);
            w.u8(0x07);
        }
        w.zeros(1u << 20);
        Container c;
        CHECK(read_container(w.data.data(), w.data.size(), c) == ErrorCode::NoDirectory);
    }

    // En ofullständig Huffman-kod: längderna lämnar bitmönster utan symbol.
    // Strömmen har dynamiska koder där avståndskoden är ofullständig.
    {
        const Bytes z = {0x78, 0x01, 0x05, 0xc1, 0x01, 0x01, 0x00, 0x00, 0x00, 0x82, 0x20, 0xd3,
                         0xff, 0x9f, 0x0b, 0x9b, 0x31, 0xb6, 0xd1, 0x42, 0xac, 0x84, 0x25, 0x33,
                         0x84, 0xb9, 0xa9, 0x1b, 0x92, 0x2b, 0x60, 0xa0, 0xe4, 0x50, 0x97, 0x89,
                         0xad, 0xa0, 0xca, 0x35, 0x48, 0xd4, 0xed, 0x1c, 0xf5, 0x00, 0x97};
        Bytes out;
        CHECK(inflate_zlib(z.data(), z.size(), out, 1 << 20) == ErrorCode::CorruptStream);
    }

    // Fragment utan pose som delar utseende och geometri är korta: 52 byte
    // eller 56 med utfyllnad. Antalet får inte avvisas för att det inte skulle
    // rymmas.
    for (const int count : {1, 10, 1000}) {
        ByteWriter w;
        w.i32(count);
        w.obj(13);
        w.f64(1.0);
        w.i32(2);
        w.i32(4);
        w.i32(0);
        w.i32(1);
        w.null_ptr();
        const int32_t look = w.obj(55);
        w.i32(0);
        w.obj(54);
        for (int i = 0; i < 14; ++i) w.f32(0.5f);
        const int32_t geometry = w.obj(93);
        for (int i = 0; i < 6; ++i) w.f32(static_cast<float>(i));
        for (int i = 0; i < 3; ++i) w.f64(0.0);
        w.f32(0.0f);
        w.i32(7);
        w.i32(1);
        for (int i = 1; i < count; ++i) shared_fragment(w, i + 1, look, geometry);
        w.pad(8);

        FragmentStore table;
        std::string detail;
        const ErrorCode e = parse_fragments(w.data.data(), w.data.size(), table, nullptr, &detail);
        if (e != ErrorCode::Ok) {
            std::printf("  FAIL — %d fragment: %s %s\n", count, error_message(e), detail.c_str());
            ++fails;
            continue;
        }
        CHECK(table.items.size() == static_cast<size_t>(count));
        CHECK(table.looks.size() == 1);
        CHECK(fragment_view(table, table.items.size() - 1).pose_class == 0);
        CHECK(fragment_view(table, table.items.size() - 1).look == 0);
        CHECK(fragment_view(table, table.items.size() - 1).record == 1);
        CHECK(fragment_view(table, table.items.size() - 1).seq == count);
    }

    // En geometripost längre än taket ger LimitExceeded, och en precis på
    // taket går.
    {
        const Bytes record(80, 0);
        ByteWriter toc;
        toc.i32(1);
        toc.i32(1);
        toc.i32(static_cast<int32_t>(testdata::zlib_store(record).size()));
        toc.i32(80);
        toc.pad(8);
        DbCacheEntry entry;
        entry.name = "LcOpNwdGeometry";
        entry.flags = 1;
        entry.a = 4;
        const Bytes package = testdata::prefixed(65536, {record}, toc.data, entry.b);
        entry.offset = 0;
        entry.length = package.size();

        SegmentLimits limits;
        limits.max_record = 79;
        GeometrySegment seg;
        const uint8_t* data = nullptr;
        size_t size = 0;
        std::string detail;
        CHECK(seg.open(package.data(), package.size(), entry, limits) == ErrorCode::Ok);
        CHECK(seg.record_bytes(0, data, size, &detail) == ErrorCode::LimitExceeded);
        CHECK(!detail.empty());

        limits.max_record = 80;
        CHECK(seg.open(package.data(), package.size(), entry, limits) == ErrorCode::Ok);
        CHECK(seg.record_bytes(0, data, size) == ErrorCode::Ok);
        CHECK(size == 80);
    }

    // Cachen räknar det minne chunkarna faktiskt håller. Många små chunkar
    // under ett litet tak får inte lämna mer än taket och en chunk kvar.
    {
        const size_t chunks = 3000;
        std::vector<Bytes> parts(chunks, Bytes(8, 0x11));
        ByteWriter toc;
        toc.i32(static_cast<int32_t>(chunks));
        toc.i32(static_cast<int32_t>(chunks));
        for (size_t i = 0; i < chunks; ++i)
            toc.i32(static_cast<int32_t>(testdata::zlib_store(parts[i]).size()));
        for (size_t i = 0; i < chunks; ++i) toc.i32(8);
        toc.pad(8);
        DbCacheEntry entry;
        entry.name = "LcOpNwdGeometry";
        entry.flags = 1;
        entry.a = 4;
        const Bytes package = testdata::prefixed(65536, parts, toc.data, entry.b);
        entry.length = package.size();

        // Chunkar om 8 byte följer inte regeln i Fixed: storlekarna mäts.
        SegmentLimits limits;
        limits.cache_bytes = 256;
        limits.layout = ChunkLayout::Scan;
        GeometrySegment seg;
        CHECK(seg.open(package.data(), package.size(), entry, limits) == ErrorCode::Ok);
        const uint8_t* data = nullptr;
        size_t size = 0;
        bool all = true;
        size_t worst = 0;
        // Framlänges och sedan baklänges, så att chunkar kastas och tas om.
        for (int pass = 0; pass < 2; ++pass) {
            for (size_t n = 0; n < chunks; ++n) {
                const uint32_t k = static_cast<uint32_t>(pass == 0 ? n : chunks - 1 - n);
                all = all && seg.record_bytes(k, data, size) == ErrorCode::Ok && size == 8 &&
                      data[0] == 0x11;
                if (seg.cached_bytes() > worst) worst = seg.cached_bytes();
            }
        }
        CHECK(all);
        CHECK(worst <= limits.cache_bytes + 64);
        CHECK(seg.inflated_total() > 8 * chunks);   // något togs om
    }

    // Document: val som inte går, och en fil vars flyttal inte är 64 bitar.
    {
        using HNwd::Document;
        using HNwd::Options;
        using HNwd::Status;

        const std::vector<testdata::Package> packages = {
            {"LcOpNwdGeometryCompress", 3, 0, 0, Bytes(40, 0xAB)},
        };
        const Bytes good = testdata::build_file(packages, true);
        const std::string path = write_temp("hnwd_test_limits.nwd", good);
        CHECK(!path.empty());

        Document doc;
        CHECK(doc.open(path.c_str()) == Status::Ok);
        CHECK(doc.is_open());
        doc.close();

        Options bad;
        bad.normal_bits = 12;
        CHECK(doc.open(path.c_str(), bad) == Status::OutOfRange);
        CHECK(!doc.is_open() && *doc.last_error() != 0);

        // "double" i båda rubrikerna byts mot ett lika långt ord.
        Bytes single = good;
        size_t replaced = 0;
        for (size_t i = 0; i + 6 <= single.size(); ++i) {
            if (std::string(single.begin() + static_cast<std::ptrdiff_t>(i),
                            single.begin() + static_cast<std::ptrdiff_t>(i + 6)) == "double") {
                const char word[] = "single";
                for (size_t j = 0; j < 6; ++j) single[i + j] = static_cast<uint8_t>(word[j]);
                ++replaced;
            }
        }
        CHECK(replaced == 3);   // datasektionen, katalogsektionen och slutrubriken
        const std::string other = write_temp("hnwd_test_limits_single.nwd", single);
        CHECK(!other.empty());
        CHECK(doc.open(other.c_str()) == Status::Unsupported);
        CHECK(!doc.is_open() && *doc.last_error() != 0);

        doc.close();
        testdata::remove_utf8(path);
        testdata::remove_utf8(other);
    }

    // Rotationens riktning mot en riktig modell. Hisschaktet är en rätvinklig
    // låda, och de flesta av dess delar är roterade: bara med rätt riktning på
    // rotationen blir utbredningen i världen schaktets jämna mått. Med den
    // omvända sticker delarna ut flera hundra enheter.
    {
        const char* path = testdata::kSampleNwc[3];
        if (!testdata::exists(path)) {
            std::printf("  SKIP — %s saknas\n", path);
            ++g_skipped;
        } else {
            HNwd::Document doc;
            CHECK(doc.open(path) == HNwd::Status::Ok);
            CHECK(doc.load_fragments() == HNwd::Status::Ok);
            double lo[3] = {1e300, 1e300, 1e300};
            double hi[3] = {-1e300, -1e300, -1e300};
            size_t rotated = 0;
            for (size_t i = 0; i < doc.fragment_count(); ++i) {
                HNwd::Fragment f;
    doc.fragment(i, f);
                if (f.pose.q[3] < 0.999999 && f.pose.q[3] > -0.999999) ++rotated;
                for (int corner = 0; corner < 8; ++corner) {
                    const float v[3] = {corner & 1 ? f.bounds.max[0] : f.bounds.min[0],
                                        corner & 2 ? f.bounds.max[1] : f.bounds.min[1],
                                        corner & 4 ? f.bounds.max[2] : f.bounds.min[2]};
                    double w[3];
                    HNwd::world_point(f.pose, v, w);
                    for (int k = 0; k < 3; ++k) {
                        if (w[k] < lo[k]) lo[k] = w[k];
                        if (w[k] > hi[k]) hi[k] = w[k];
                    }
                }
            }
            auto near_to = [](double a, double b) { return a - b < 0.01 && b - a < 0.01; };
            CHECK(rotated > 500);
            CHECK(near_to(lo[0], 29155) && near_to(hi[0], 31255));
            CHECK(near_to(lo[1], 10740) && near_to(hi[1], 14600));
            CHECK(near_to(lo[2], 0) && near_to(hi[2], 12000));
        }
    }

    // Fragment utan pose mot en riktig modell. Arkitekturmodellen har tolv,
    // för byggnadens yttre fackverk. Deras plats är geometrins mittpunkt: då
    // står fackverket runt byggnaden och modellen håller sig inom tomten. I
    // origo hade de stuckit ut tjugo meter utanför.
    {
        const char* path = testdata::kSampleNwc[0];
        if (!testdata::exists(path)) {
            std::printf("  SKIP — %s saknas\n", path);
            ++g_skipped;
        } else {
            HNwd::Document doc;
            CHECK(doc.open(path) == HNwd::Status::Ok);
            CHECK(doc.load_fragments() == HNwd::Status::Ok);
            double lo[3] = {1e300, 1e300, 1e300};
            double hi[3] = {-1e300, -1e300, -1e300};
            for (size_t i = 0; i < doc.fragment_count(); ++i) {
                HNwd::Fragment f;
    doc.fragment(i, f);
                for (int corner = 0; corner < 8; ++corner) {
                    const float v[3] = {corner & 1 ? f.bounds.max[0] : f.bounds.min[0],
                                        corner & 2 ? f.bounds.max[1] : f.bounds.min[1],
                                        corner & 4 ? f.bounds.max[2] : f.bounds.min[2]};
                    double w[3];
                    HNwd::world_point(f.pose, v, w);
                    for (int k = 0; k < 3; ++k) {
                        if (w[k] < lo[k]) lo[k] = w[k];
                        if (w[k] > hi[k]) hi[k] = w[k];
                    }
                }
            }
            auto near_to = [](double a, double b) { return a - b < 0.01 && b - a < 0.01; };
            CHECK(near_to(lo[0], -2478.684) && near_to(hi[0], 47625));
            CHECK(near_to(lo[1], -2500) && near_to(hi[1], 25000));
            CHECK(near_to(lo[2], -1300) && near_to(hi[2], 14237.589));
        }
    }

    std::printf("test_limits: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
