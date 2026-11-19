#include <cstdio>
#include <string>

#include "container.h"
#include "dbcache.h"
#include "mapped_file.h"
#include "test_data.h"
#include "test_support.h"

using namespace HNwd::detail;
using testdata::Bytes;
using testdata::ByteWriter;

using testdata::Package;
using testdata::build_file;
using testdata::directory;
using testdata::kDataHeader;
using testdata::kDirHeader;

namespace {

std::vector<Package> sample_packages() {
    const Bytes one = testdata::zlib_store(testdata::pattern(300));
    // Formen (4, b): prefix, två strömmar, och den sista är b byte lång.
    const Bytes first = testdata::zlib_store(testdata::pattern(50));
    const Bytes toc = testdata::zlib_store(testdata::pattern(16));
    ByteWriter seg;
    seg.i32(65536);
    seg.raw(first);
    seg.raw(toc);
    return {
        {"LcOpNwdGeometryCompress", 3, 0, 0, Bytes(40, 0xAB)},
        {"model.ifc\\LcOpNwdGeometry", 1, 4, static_cast<int32_t>(toc.size()), seg.data},
        {"cache\\LcOpNwdFragments", 1, 0, 0, one},
        {"LcOpXRefTable", 1, 0, 0, one},
    };
}

} // namespace

int test_dbcache() {
    int fails = 0;
    std::vector<DbCacheEntry> entries;

    // Namn av olika längd ger båda fallen av PAD8: med och utan utfyllnad.
    const std::vector<Package> packages = sample_packages();
    const Bytes dir = directory(packages, 52);
    CHECK(parse_dbcache(dir.data(), dir.size(), entries) == ErrorCode::Ok);
    CHECK(entries.size() == packages.size());
    if (entries.size() == packages.size()) {
        uint64_t offset = 52;
        for (size_t i = 0; i < entries.size(); ++i) {
            CHECK(entries[i].name == packages[i].name);
            CHECK(entries[i].flags == packages[i].flags);
            CHECK(entries[i].a == packages[i].a);
            CHECK(entries[i].b == packages[i].b);
            CHECK(entries[i].offset == offset);
            CHECK(entries[i].length == packages[i].bytes.size());
            offset += packages[i].bytes.size();
        }
        CHECK(entries[0].leaf() == "LcOpNwdGeometryCompress");
        CHECK(entries[1].leaf() == "LcOpNwdGeometry");
        CHECK(entries[2].leaf() == "LcOpNwdFragments");
        const DbCacheEntry* hit = find_entry(entries, "LcOpNwdGeometry");
        CHECK(hit == &entries[1]);
        CHECK(find_entry(entries, "LcOpNwd") == nullptr);
        CHECK(find_entry(entries, "model.ifc\\LcOpNwdGeometry") == nullptr);
    }

    // Tom katalog.
    {
        ByteWriter w;
        w.i32(-1);
        w.i32(0);
        CHECK(parse_dbcache(w.data.data(), w.data.size(), entries) == ErrorCode::Ok);
        CHECK(entries.empty());
    }

    // Varje avhuggen längd ska ge fel.
    for (size_t n = 0; n < dir.size(); ++n)
        CHECK(parse_dbcache(dir.data(), n, entries) == ErrorCode::CorruptDirectory);

    // Byte kvar efter sista posten.
    {
        Bytes bad = dir;
        bad.insert(bad.end(), 4, 0);
        CHECK(parse_dbcache(bad.data(), bad.size(), entries) ==
              ErrorCode::CorruptDirectory);
    }
    // Huvudet är inte -1.
    {
        Bytes bad = dir;
        bad[0] = 0;
        CHECK(parse_dbcache(bad.data(), bad.size(), entries) ==
              ErrorCode::CorruptDirectory);
    }
    // Ett antal som inte ryms i datan får inte leda till en allokering.
    {
        ByteWriter w;
        w.i32(-1);
        w.i32(0x7FFFFFFF);
        w.zeros(64);
        CHECK(parse_dbcache(w.data.data(), w.data.size(), entries) ==
              ErrorCode::CorruptDirectory);
        ByteWriter neg;
        neg.i32(-1);
        neg.i32(-5);
        CHECK(parse_dbcache(neg.data.data(), neg.data.size(), entries) ==
              ErrorCode::CorruptDirectory);
    }
    // Utfyllnad som inte är noll, negativ offset, null-namn.
    {
        ByteWriter w;
        w.i32(-1);
        w.i32(1);
        w.i32(3);
        w.text("abc");
        w.u8(1);        // namnets utfyllnad ska vara noll
        w.i32(1);
        w.pad(8);
        w.i64(52);
        w.i64(10);
        w.i32(0);
        w.i32(0);
        CHECK(parse_dbcache(w.data.data(), w.data.size(), entries) ==
              ErrorCode::CorruptDirectory);
    }
    {
        ByteWriter w;
        w.i32(-1);
        w.i32(1);
        w.str("abcd");
        w.i32(1);
        w.pad(8);
        w.i64(-1);
        w.i64(10);
        w.i32(0);
        w.i32(0);
        CHECK(parse_dbcache(w.data.data(), w.data.size(), entries) ==
              ErrorCode::CorruptDirectory);
    }
    {
        ByteWriter w;
        w.i32(-1);
        w.i32(1);
        w.i32(-1);      // null-sträng som namn
        w.i32(1);
        w.i64(52);
        w.i64(10);
        w.i32(0);
        w.i32(0);
        CHECK(parse_dbcache(w.data.data(), w.data.size(), entries) ==
              ErrorCode::CorruptDirectory);
    }

    std::printf("test_dbcache: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}

int test_container() {
    int fails = 0;

    // Rubriker.
    {
        const Bytes h(kDirHeader, kDirHeader + sizeof kDirHeader - 1);
        SectionHeader s;
        CHECK(parse_section_header(h.data(), h.size(), 0, s) == ErrorCode::Ok);
        CHECK(s.offset == 0 && s.length == 65);
        CHECK(s.format == "LcUStream-V1.1");
        CHECK(s.encoding == "utf8");
        CHECK(s.marker == "lizlib");
        CHECK(s.version == 448);
        CHECK(s.is_double);

        const Bytes d(kDataHeader, kDataHeader + sizeof kDataHeader - 1);
        CHECK(parse_section_header(d.data(), d.size(), 0, s) == ErrorCode::Ok);
        CHECK(s.length == 52);
        CHECK(s.encoding == "C");
        CHECK(s.marker == "lichunk-008");
        CHECK(s.version == -1);

        // Utan radbrytning, med ett styrtecken, och på fel plats.
        CHECK(parse_section_header(d.data(), d.size() - 1, 0, s) ==
              ErrorCode::CorruptHeader);
        Bytes bad = d;
        bad[20] = 0x01;
        CHECK(parse_section_header(bad.data(), bad.size(), 0, s) ==
              ErrorCode::CorruptHeader);
        CHECK(parse_section_header(d.data(), d.size(), 1, s) == ErrorCode::CorruptHeader);
        CHECK(parse_section_header(d.data(), d.size(), d.size() + 10, s) ==
              ErrorCode::CorruptHeader);
        // En rad utan slut inom rimlig längd.
        Bytes longline(kDataHeader, kDataHeader + 20);
        longline.insert(longline.end(), 1000, 'x');
        longline.push_back('\n');
        CHECK(parse_section_header(longline.data(), longline.size(), 0, s) ==
              ErrorCode::CorruptHeader);
    }

    const std::vector<Package> packages = sample_packages();

    // Hela filen, med och utan slutet som pekar ut katalogsektionen.
    for (const bool with_tail : {true, false}) {
        const Bytes file = build_file(packages, with_tail);
        Container c;
        std::string detail;
        CHECK(read_container(file.data(), file.size(), c, &detail) == ErrorCode::Ok);
        CHECK(c.data_section.length == 52);
        CHECK(c.directory_section.version == 448);
        CHECK(c.entries.size() == packages.size());
        CHECK(c.directory_offset == c.directory_section.offset + 72);

        if (c.entries.size() == packages.size()) {
            // Paketens sista ström går att packa upp och är den som lades dit.
            StreamSpan span;
            Bytes out;
            size_t consumed = 0;
            CHECK(last_stream(c.entries[1], file.size(), span) == ErrorCode::Ok);
            CHECK(span.length == static_cast<uint64_t>(packages[1].b));
            CHECK(inflate_zlib(file.data() + span.offset, span.length, out, 1 << 20,
                               &consumed) == ErrorCode::Ok);
            CHECK(out == testdata::pattern(16));
            CHECK(consumed == span.length);

            CHECK(last_stream(c.entries[2], file.size(), span) == ErrorCode::Ok);
            CHECK(span.offset == c.entries[2].offset);
            CHECK(span.length == c.entries[2].length);
            CHECK(inflate_zlib(file.data() + span.offset, span.length, out, 1 << 20,
                               &consumed) == ErrorCode::Ok);
            CHECK(out == testdata::pattern(300));
        }
    }

    // En rubrik som bara råkar stå i ett pakets data får inte tas för
    // katalogsektionen.
    {
        std::vector<Package> tricky = packages;
        Bytes fake(kDirHeader, kDirHeader + sizeof kDirHeader - 1);
        fake.insert(fake.end(), 30, 0x55);
        tricky.push_back({"LcOpRaw", 3, 0, 0, fake});
        for (const bool with_tail : {true, false}) {
            const Bytes file = build_file(tricky, with_tail);
            Container c;
            CHECK(read_container(file.data(), file.size(), c) == ErrorCode::Ok);
            CHECK(c.entries.size() == tricky.size());
        }
    }

    // Katalogen täcker inte datasektionen: ett glapp före katalogsektionen.
    {
        const Bytes file = build_file(packages, true, 5);
        Container c;
        std::string detail;
        CHECK(read_container(file.data(), file.size(), c, &detail) ==
              ErrorCode::CorruptDirectory);
        CHECK(!detail.empty());
    }

    // Inte en sådan fil alls, och en fil utan katalog.
    {
        const Bytes junk = testdata::pattern(500);
        Container c;
        CHECK(read_container(junk.data(), junk.size(), c) == ErrorCode::NotLcUStream);
        CHECK(read_container(nullptr, 0, c) == ErrorCode::NotLcUStream);

        ByteWriter w;
        w.text(kDataHeader);
        w.raw(testdata::pattern(500));
        CHECK(read_container(w.data.data(), w.data.size(), c) == ErrorCode::NoDirectory);
        CHECK(c.entries.empty());

        // Bara rubriken.
        const Bytes only(kDataHeader, kDataHeader + sizeof kDataHeader - 1);
        CHECK(read_container(only.data(), only.size(), c) == ErrorCode::NoDirectory);
    }

    // Slutet pekar fel: katalogen ska ändå hittas bakifrån.
    {
        Bytes file = build_file(packages, true);
        const size_t at = file.size() - 52 - 8;
        file[at] ^= 0x40;
        Container c;
        CHECK(read_container(file.data(), file.size(), c) == ErrorCode::Ok);
        CHECK(c.entries.size() == packages.size());
    }

    // Varje avhuggen fil ska ge ett fel eller en katalog som håller — aldrig
    // en läsning utanför bufferten.
    {
        const Bytes file = build_file(packages, true);
        for (size_t n = 0; n < file.size(); n += 3) {
            Container c;
            const Bytes cut(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(n));
            const ErrorCode e = read_container(cut.data(), cut.size(), c);
            if (e == ErrorCode::Ok) {
                for (const DbCacheEntry& p : c.entries)
                    CHECK(p.offset + p.length <= cut.size());
            }
        }
    }

    // last_stream: former och gränser.
    {
        DbCacheEntry e;
        StreamSpan span;
        e.offset = 100;
        e.length = 50;
        CHECK(last_stream(e, 150, span) == ErrorCode::Ok);
        CHECK(last_stream(e, 149, span) == ErrorCode::OutOfRange);
        e.a = 4;
        e.b = 46;
        CHECK(last_stream(e, 150, span) == ErrorCode::Ok);
        CHECK(span.offset == 104 && span.length == 46);
        e.b = 47;       // prefixet ryms inte
        CHECK(last_stream(e, 150, span) == ErrorCode::OutOfRange);
        e.b = 0;        // (4, 0) är ingen känd form
        CHECK(last_stream(e, 150, span) == ErrorCode::OutOfRange);
        e.a = 2;
        e.b = 10;
        CHECK(last_stream(e, 150, span) == ErrorCode::OutOfRange);
        e.a = 0;
        e.b = 0;
        e.offset = ~uint64_t(0) - 10;   // offset + längd får inte slå runt
        CHECK(last_stream(e, 150, span) == ErrorCode::OutOfRange);
    }

    std::printf("test_container: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}

namespace {

int32_t le32(const Bytes& b, size_t at) {
    return static_cast<int32_t>(static_cast<uint32_t>(b[at]) |
                                (static_cast<uint32_t>(b[at + 1]) << 8) |
                                (static_cast<uint32_t>(b[at + 2]) << 16) |
                                (static_cast<uint32_t>(b[at + 3]) << 24));
}

// Ett paket av formen (4, b) lagt mitt i en fil, med byte före och efter som
// inte hör till det.
struct PrefixedFile {
    Bytes file;
    DbCacheEntry entry;
    std::vector<uint32_t> sizes;   // de komprimerade längderna före den sista strömmen
};

PrefixedFile make_prefixed(int32_t prefix, const std::vector<Bytes>& streams, const Bytes& toc) {
    PrefixedFile p;
    int32_t b = 0;
    const Bytes pkg = testdata::prefixed(prefix, streams, toc, b);
    p.file.assign(100, 0xEE);
    p.file.insert(p.file.end(), pkg.begin(), pkg.end());
    p.file.insert(p.file.end(), 20, 0xEE);
    p.entry.name = "dir\\LcOpNwdGeometry";
    p.entry.flags = 1;
    p.entry.offset = 100;
    p.entry.length = pkg.size();
    p.entry.a = 4;
    p.entry.b = b;
    for (const Bytes& s : streams)
        p.sizes.push_back(static_cast<uint32_t>(testdata::zlib_store(s).size()));
    return p;
}

// Innehållsförteckningen i ett paket, uppackad och tolkad för hand:
//
//   geometri:  [int32 N][int32 ch][ch × int32 längd][N × int32 postlängd] PAD8
//   egenskaper: [int32 k][k × int32 längd] PAD8
struct Toc {
    Bytes data;
    int32_t first = 0;             // N respektive k
    std::vector<uint32_t> sizes;   // de komprimerade längderna
};

bool load_toc(const MappedFile& file, const DbCacheEntry& entry, bool geometry, Toc& out) {
    StreamSpan span;
    if (last_stream(entry, file.size(), span) != ErrorCode::Ok ||
        inflate_span(file.data(), file.size(), span, kDefaultMaxStreamBytes, out.data) !=
            ErrorCode::Ok ||
        out.data.size() < 8)
        return false;
    out.first = le32(out.data, 0);
    const int32_t n = geometry ? le32(out.data, 4) : out.first;
    const size_t start = geometry ? 8 : 4;
    if (n < 0 || out.data.size() < start + 4 * static_cast<uint64_t>(n)) return false;
    out.sizes.clear();
    for (size_t i = 0; i < static_cast<size_t>(n); ++i)
        out.sizes.push_back(static_cast<uint32_t>(le32(out.data, start + 4 * i)));
    return true;
}

} // namespace

int test_prefixed() {
    int fails = 0;

    const std::vector<Bytes> streams = {testdata::pattern(50), testdata::pattern(700),
                                        testdata::pattern(1)};
    const PrefixedFile p = make_prefixed(65536, streams, testdata::pattern(24));

    // read_prefix
    {
        int32_t prefix = 0;
        CHECK(read_prefix(p.file.data(), p.file.size(), p.entry, prefix));
        CHECK(prefix == 65536);

        DbCacheEntry e = p.entry;
        e.a = 0;   // formen (0, 0) har inget prefix
        e.b = 0;
        CHECK(!read_prefix(p.file.data(), p.file.size(), e, prefix));
        e = p.entry;
        e.b = 0;   // (4, 0) är ingen känd form
        CHECK(!read_prefix(p.file.data(), p.file.size(), e, prefix));
        CHECK(!read_prefix(p.file.data(), p.entry.offset + p.entry.length - 1, p.entry, prefix));
        CHECK(!read_prefix(nullptr, 0, p.entry, prefix));
        e = p.entry;
        e.offset = ~uint64_t(0) - 2;   // offset + längd får inte slå runt
        CHECK(!read_prefix(p.file.data(), p.file.size(), e, prefix));
        e = p.entry;
        e.b = static_cast<int32_t>(e.length);   // prefixet ryms inte före den sista strömmen
        CHECK(!read_prefix(p.file.data(), p.file.size(), e, prefix));
    }

    // split_prefixed: strömmarna ligger tätt efter prefixet och går att packa upp.
    {
        std::vector<StreamSpan> spans;
        std::string detail;
        CHECK(split_prefixed(p.entry, p.file.size(), p.sizes, spans, &detail) == ErrorCode::Ok);
        CHECK(detail.empty());
        CHECK(spans.size() == streams.size());
        if (spans.size() == streams.size()) {
            uint64_t at = p.entry.offset + 4;
            for (size_t i = 0; i < spans.size(); ++i) {
                CHECK(spans[i].offset == at);
                CHECK(spans[i].length == p.sizes[i]);
                at += spans[i].length;
                Bytes out;
                CHECK(inflate_span(p.file.data(), p.file.size(), spans[i], 1 << 20, out) ==
                      ErrorCode::Ok);
                CHECK(out == streams[i]);
            }
            // Den sista strömmen börjar där de andra slutar.
            StreamSpan last;
            CHECK(last_stream(p.entry, p.file.size(), last) == ErrorCode::Ok);
            CHECK(last.offset == at);
        }

        // Ett paket utan andra strömmar än innehållsförteckningen.
        const PrefixedFile none = make_prefixed(0, {}, testdata::pattern(24));
        CHECK(split_prefixed(none.entry, none.file.size(), none.sizes, spans) == ErrorCode::Ok);
        CHECK(spans.empty());
    }

    // split_prefixed: fel.
    {
        std::vector<StreamSpan> spans;
        std::string detail;

        std::vector<uint32_t> sizes = p.sizes;
        sizes[1] += 1;   // summan stämmer inte
        CHECK(split_prefixed(p.entry, p.file.size(), sizes, spans, &detail) ==
              ErrorCode::CorruptPackage);
        CHECK(spans.empty());
        CHECK(detail.rfind("LcOpNwdGeometry at byte 100: ", 0) == 0);

        sizes = p.sizes;
        sizes[2] -= 1;
        CHECK(split_prefixed(p.entry, p.file.size(), sizes, spans) == ErrorCode::CorruptPackage);

        sizes = p.sizes;
        sizes.push_back(1);   // en ström för mycket
        CHECK(split_prefixed(p.entry, p.file.size(), sizes, spans) == ErrorCode::CorruptPackage);

        sizes = p.sizes;
        sizes.pop_back();     // en ström för lite
        CHECK(split_prefixed(p.entry, p.file.size(), sizes, spans) == ErrorCode::CorruptPackage);

        // En längd som är 0, fast summan stämmer.
        sizes = p.sizes;
        sizes.push_back(0);
        detail.clear();
        CHECK(split_prefixed(p.entry, p.file.size(), sizes, spans, &detail) ==
              ErrorCode::CorruptPackage);
        CHECK(detail.find("length 0") != std::string::npos);
        CHECK(split_prefixed(p.entry, p.file.size(), std::vector<uint32_t>{0}, spans) ==
              ErrorCode::CorruptPackage);

        // Längder vars summa bara stämmer om den slår runt i 32 bitar.
        const uint32_t target = static_cast<uint32_t>(p.entry.length) - 4u -
                                static_cast<uint32_t>(p.entry.b);
        sizes = {0xFFFFFFFFu, 0xFFFFFFFFu, target + 2u};
        CHECK(split_prefixed(p.entry, p.file.size(), sizes, spans) == ErrorCode::CorruptPackage);

        // Fel form och poster utanför filen.
        DbCacheEntry e = p.entry;
        e.a = 0;
        e.b = 0;
        CHECK(split_prefixed(e, p.file.size(), p.sizes, spans) == ErrorCode::OutOfRange);
        e = p.entry;
        e.b = 0;
        CHECK(split_prefixed(e, p.file.size(), p.sizes, spans) == ErrorCode::OutOfRange);
        CHECK(split_prefixed(p.entry, p.entry.offset + p.entry.length - 1, p.sizes, spans) ==
              ErrorCode::OutOfRange);
        e = p.entry;
        e.offset = ~uint64_t(0) - 2;
        CHECK(split_prefixed(e, p.file.size(), p.sizes, spans) == ErrorCode::OutOfRange);
    }

    // inflate_span
    {
        std::vector<StreamSpan> spans;
        CHECK(split_prefixed(p.entry, p.file.size(), p.sizes, spans) == ErrorCode::Ok);
        const StreamSpan s = spans[1];   // 700 byte, och inte den sista i paketet
        Bytes out;
        std::string detail;
        CHECK(inflate_span(p.file.data(), p.file.size(), s, 1 << 20, out, &detail) ==
              ErrorCode::Ok);
        CHECK(out == streams[1]);
        CHECK(detail.empty());

        // En byte extra i spannet: strömmen slutar före spannets slut.
        StreamSpan longer = s;
        longer.length += 1;
        out.assign(5, 1);
        CHECK(inflate_span(p.file.data(), p.file.size(), longer, 1 << 20, out, &detail) ==
              ErrorCode::CorruptStream);
        CHECK(detail.rfind("stream at byte ", 0) == 0);

        // En byte för lite: strömmen tar slut.
        StreamSpan shorter = s;
        shorter.length -= 1;
        CHECK(inflate_span(p.file.data(), p.file.size(), shorter, 1 << 20, out) ==
              ErrorCode::CorruptStream);

        // Varje avhuggen längd ger ett fel.
        for (uint64_t n = 0; n < s.length; ++n) {
            StreamSpan cut = s;
            cut.length = n;
            CHECK(inflate_span(p.file.data(), p.file.size(), cut, 1 << 20, out) !=
                  ErrorCode::Ok);
        }

        // Taket, en trasig kontrollsumma och spann utanför filen.
        CHECK(inflate_span(p.file.data(), p.file.size(), s, 699, out) ==
              ErrorCode::StreamTooLarge);
        Bytes bad = p.file;
        bad[static_cast<size_t>(s.offset + s.length - 1)] ^= 0x01;
        CHECK(inflate_span(bad.data(), bad.size(), s, 1 << 20, out) == ErrorCode::CorruptStream);

        StreamSpan outside = s;
        outside.length = p.file.size() - s.offset + 1;
        CHECK(inflate_span(p.file.data(), p.file.size(), outside, 1 << 20, out) ==
              ErrorCode::OutOfRange);
        outside.offset = p.file.size() + 1;
        outside.length = 0;
        CHECK(inflate_span(p.file.data(), p.file.size(), outside, 1 << 20, out) ==
              ErrorCode::OutOfRange);
        outside.offset = ~uint64_t(0) - 2;
        outside.length = 10;
        CHECK(inflate_span(p.file.data(), p.file.size(), outside, 1 << 20, out) ==
              ErrorCode::OutOfRange);
        outside = s;
        outside.length = 0;
        CHECK(inflate_span(p.file.data(), p.file.size(), outside, 1 << 20, out) ==
              ErrorCode::CorruptStream);
    }

    // Exempelfilerna: geometrins och egenskapernas strömmar enligt sina
    // innehållsförteckningar.
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
        Bytes chunk;

        const DbCacheEntry* geometry = find_entry(c.entries, "LcOpNwdGeometry");
        CHECK(geometry != nullptr);
        Toc toc;
        if (geometry && load_toc(file, *geometry, true, toc)) {
            int32_t prefix = 0;
            CHECK(read_prefix(file.data(), file.size(), *geometry, prefix));
            CHECK(prefix == 65536);
            CHECK(static_cast<uint32_t>(toc.first) == sample.records);
            CHECK(toc.sizes.size() == sample.chunks);
            // Längden är exakt 8 + 4·ch + 4·N, uppfylld till 8.
            CHECK(toc.data.size() ==
                  ((8 + 4 * static_cast<size_t>(sample.chunks) + 4 * static_cast<size_t>(sample.records)) + 7) / 8 * 8);

            std::vector<StreamSpan> spans;
            std::string detail;
            const ErrorCode e = split_prefixed(*geometry, file.size(), toc.sizes, spans, &detail);
            if (e != ErrorCode::Ok) std::printf("  %s: %s\n", sample.path, detail.c_str());
            CHECK(e == ErrorCode::Ok);

            uint64_t inflated = 0;
            for (const StreamSpan& s : spans) {
                CHECK(inflate_span(file.data(), file.size(), s, kDefaultMaxStreamBytes, chunk) ==
                      ErrorCode::Ok);
                inflated += chunk.size();
            }
            // Postlängderna i innehållsförteckningen summerar till de uppackade
            // chunkarnas storlek.
            const size_t records_at = 8 + 4 * static_cast<size_t>(sample.chunks);
            uint64_t sum = 0;
            for (size_t i = 0; i < sample.records && toc.data.size() >= records_at + 4 * (i + 1); ++i)
                sum += static_cast<uint32_t>(le32(toc.data, records_at + 4 * i));
            CHECK(sum == inflated);
        } else {
            CHECK(false);
        }

        const DbCacheEntry* props = find_entry(c.entries, "LcOaPartitionProps");
        CHECK(props != nullptr);
        Toc ptoc;
        if (props && load_toc(file, *props, false, ptoc)) {
            CHECK(ptoc.first == 1);
            CHECK(ptoc.data.size() == (4 + 4 * static_cast<size_t>(ptoc.first) + 7) / 8 * 8);
            std::vector<StreamSpan> spans;
            std::string detail;
            const ErrorCode e = split_prefixed(*props, file.size(), ptoc.sizes, spans, &detail);
            if (e != ErrorCode::Ok) std::printf("  %s: %s\n", sample.path, detail.c_str());
            CHECK(e == ErrorCode::Ok);
            CHECK(spans.size() == static_cast<size_t>(ptoc.first));
            for (const StreamSpan& s : spans)
                CHECK(inflate_span(file.data(), file.size(), s, kDefaultMaxStreamBytes, chunk) ==
                      ErrorCode::Ok);
        } else {
            CHECK(false);
        }
    }

    std::printf("test_prefixed: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
