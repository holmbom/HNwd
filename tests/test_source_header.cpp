#include <cstdio>
#include <string>
#include <vector>

#include "container.h"
#include "mapped_file.h"
#include "source_header.h"
#include "test_data.h"
#include "test_support.h"

using namespace HNwd::detail;
using testdata::Bytes;
using testdata::ByteWriter;

namespace {

const char kLoader[] = "LcNwcLoaderPlugin:IFC";

// Huvudet följt av en bit inställningar som läsaren inte ska tolka.
Bytes make_header(const std::string* path, int64_t time, int64_t size, const std::string& loader,
                  size_t* loader_end = nullptr) {
    ByteWriter w;
    w.i64(7);
    if (path) w.str(*path);
    else w.null_str();
    w.i32(-1);
    w.i64a(time);
    w.i64a(size);
    w.str(loader);
    if (loader_end) *loader_end = w.data.size();
    w.str("option one");
    w.i32(12345);
    w.f64(0.5);
    return w.data;
}

} // namespace

int test_source_header() {
    int fails = 0;
    SourceHeader h;
    std::string detail;

    // Sökvägar som ger båda fallen av PAD8 efter -1: "abcd" ger 4 mod 8, och
    // "abcdefgh" ger 0 mod 8. Dessutom en tom sträng och en med utfyllnad.
    for (const std::string& path : {std::string("abcd"), std::string("abcdefgh"),
                                   std::string("C:\\Models\\x.ifc"), std::string(""),
                                   std::string("a")}) {
        const Bytes d = make_header(&path, 1700000000, 123456, kLoader);
        h.path = "stale";
        CHECK(parse_source_header(d.data(), d.size(), h, &detail) == ErrorCode::Ok);
        CHECK(detail.empty());
        CHECK(h.path == path);
        CHECK(!h.path_null);
        CHECK(h.unix_time == 1700000000);
        CHECK(h.source_size == 123456);
        CHECK(h.loader == kLoader);
    }

    // Null-sökväg.
    {
        const Bytes d = make_header(nullptr, 0, 0, kLoader);
        CHECK(parse_source_header(d.data(), d.size(), h, &detail) == ErrorCode::Ok);
        CHECK(h.path.empty() && h.path_null);
        CHECK(h.unix_time == 0 && h.source_size == 0);
        CHECK(h.loader == kLoader);
    }

    // Utan någonting efter loader-namnet.
    {
        const std::string path = "abcd";
        size_t end = 0;
        const Bytes d = make_header(&path, 5, 6, kLoader, &end);
        CHECK(parse_source_header(d.data(), end, h) == ErrorCode::Ok);
        CHECK(h.loader == kLoader);
    }

    // Varje avhuggen längd fram till och med loader-namnet ger CorruptPackage.
    for (const bool null_path : {false, true}) {
        for (const std::string& path : {std::string("abcd"), std::string("abcdefgh")}) {
            size_t end = 0;
            const Bytes d = make_header(null_path ? nullptr : &path, 5, 6, kLoader, &end);
            for (size_t n = 0; n < end; ++n)
                CHECK(parse_source_header(d.data(), n, h, &detail) == ErrorCode::CorruptPackage);
            CHECK(parse_source_header(d.data(), end, h) == ErrorCode::Ok);
            CHECK(detail.rfind("LcOpDBCache at byte ", 0) == 0);
        }
    }

    // Det första talet är inte 7.
    {
        const std::string path = "abcd";
        Bytes d = make_header(&path, 5, 6, kLoader);
        d[0] = 8;
        CHECK(parse_source_header(d.data(), d.size(), h, &detail) == ErrorCode::CorruptPackage);
        CHECK(detail.find("not 7") != std::string::npos);
        d[0] = 7;
        d[4] = 1;   // den övre halvan
        CHECK(parse_source_header(d.data(), d.size(), h) == ErrorCode::CorruptPackage);
    }

    // Talet efter sökvägen är inte -1.
    {
        const std::string path = "abcd";
        Bytes d = make_header(&path, 5, 6, kLoader);
        d[16] = 0;   // 8 + 4 + 4: början på talet efter sökvägen
        CHECK(parse_source_header(d.data(), d.size(), h, &detail) == ErrorCode::CorruptPackage);
        CHECK(detail.find("-1") != std::string::npos);
    }

    // PAD8 som inte är noll.
    {
        const std::string path = "abcd";    // -1 slutar på 4 mod 8, så en pad följer
        Bytes d = make_header(&path, 5, 6, kLoader);
        CHECK(d[20] == 0 && d[21] == 0 && d[22] == 0 && d[23] == 0);
        d[20] = 1;
        CHECK(parse_source_header(d.data(), d.size(), h, &detail) == ErrorCode::CorruptPackage);
        CHECK(detail.find("alignment") != std::string::npos);
    }
    // Utfyllnad i sökvägen som inte är noll.
    {
        const std::string path = "a";
        Bytes d = make_header(&path, 5, 6, kLoader);
        d[13] = 1;   // 8 + 4 + 1: första utfyllnadsbyten
        CHECK(parse_source_header(d.data(), d.size(), h) == ErrorCode::CorruptPackage);
    }

    // Negativ tid, negativ storlek, null-loader, och en loader som tar slut.
    {
        const std::string path = "abcd";
        Bytes d = make_header(&path, -1, 6, kLoader);
        CHECK(parse_source_header(d.data(), d.size(), h, &detail) == ErrorCode::CorruptPackage);
        CHECK(detail.find("time") != std::string::npos);
        d = make_header(&path, 5, -6, kLoader);
        CHECK(parse_source_header(d.data(), d.size(), h, &detail) == ErrorCode::CorruptPackage);
        CHECK(detail.find("size") != std::string::npos);

        ByteWriter w;
        w.i64(7);
        w.str(path);
        w.i32(-1);
        w.i64a(5);
        w.i64a(6);
        w.null_str();
        CHECK(parse_source_header(w.data.data(), w.data.size(), h, &detail) ==
              ErrorCode::CorruptPackage);
        CHECK(detail.find("null") != std::string::npos);

        ByteWriter v;
        v.i64(7);
        v.str(path);
        v.i32(-1);
        v.i64a(5);
        v.i64a(6);
        v.i32(1000);   // loader-namnet ryms inte
        v.zeros(16);
        CHECK(parse_source_header(v.data.data(), v.data.size(), h) == ErrorCode::CorruptPackage);
    }

    // `out` är orörd efter ett fel.
    {
        const Bytes d(3, 0);
        h.loader = "kvar";
        CHECK(parse_source_header(d.data(), d.size(), h) == ErrorCode::CorruptPackage);
        CHECK(h.loader.empty() && h.path.empty());
        CHECK(parse_source_header(nullptr, 0, h) == ErrorCode::CorruptPackage);
    }

    // Exempelfilerna: varje .nwc har paketet och loadern börjar med
    // LcNwcLoaderPlugin:, en .nwd saknar det.
    for (const testdata::SampleCounts& sample : testdata::kSamples) {
        if (!testdata::exists(sample.path)) {
            std::printf("  SKIP — %s saknas\n", sample.path);
            ++g_skipped;
            continue;
        }
        MappedFile file;
        Container c;
        CHECK(file.open(sample.path) == ErrorCode::Ok);
        if (!file.is_open() || read_container(file.data(), file.size(), c) != ErrorCode::Ok) {
            ++fails;
            continue;
        }
        const DbCacheEntry* e = find_entry(c.entries, "LcOpDBCache");
        if (sample.top_class == 32) {
            CHECK(e == nullptr);
            continue;
        }
        CHECK(e != nullptr);
        if (!e) continue;
        StreamSpan span;
        Bytes data;
        CHECK(last_stream(*e, file.size(), span) == ErrorCode::Ok);
        CHECK(inflate_span(file.data(), file.size(), span, kDefaultMaxStreamBytes, data) ==
              ErrorCode::Ok);
        const ErrorCode r = parse_source_header(data.data(), data.size(), h, &detail);
        if (r != ErrorCode::Ok) std::printf("  %s: %s\n", sample.path, detail.c_str());
        CHECK(r == ErrorCode::Ok);
        CHECK(h.loader.rfind("LcNwcLoaderPlugin:", 0) == 0);
        CHECK(!h.path_null && !h.path.empty());
        CHECK(h.unix_time > 0 && h.source_size > 0);
    }

    std::printf("test_source_header: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
