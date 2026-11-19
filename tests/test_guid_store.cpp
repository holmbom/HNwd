#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "container.h"
#include "guid_store.h"
#include "mapped_file.h"
#include "test_data.h"
#include "test_support.h"

using namespace HNwd::detail;
using testdata::Bytes;
using testdata::ByteWriter;

namespace {

// [1][n] och n GUID, där GUID i har byten i, i+1, … (och därmed ett eget värde).
Bytes make_store(int32_t a, int32_t n, size_t guids) {
    ByteWriter w;
    w.i32(a);
    w.i32(n);
    for (size_t g = 0; g < guids; ++g)
        for (size_t i = 0; i < 16; ++i) w.u8(static_cast<uint8_t>(g * 16 + i));
    return w.data;
}

} // namespace

int test_guid_store() {
    int fails = 0;
    std::vector<Guid16> guids;
    std::string detail;

    // Rätt: [1][2][32 byte].
    {
        const Bytes d = make_store(1, 2, 2);
        CHECK(d.size() == 40);
        CHECK(parse_guid_store(d.data(), d.size(), guids, &detail) == ErrorCode::Ok);
        CHECK(detail.empty());
        CHECK(guids.size() == 2);
        if (guids.size() == 2) {
            for (size_t g = 0; g < 2; ++g)
                for (size_t i = 0; i < 16; ++i) CHECK(guids[g][i] == g * 16 + i);
        }
    }

    // Tom tabell, och att `out` ersätts.
    {
        const Bytes d = make_store(1, 0, 0);
        guids.assign(5, Guid16{});
        CHECK(parse_guid_store(d.data(), d.size(), guids) == ErrorCode::Ok);
        CHECK(guids.empty());
    }

    // Fel huvud: a != 1, negativt antal, fel längd, för kort.
    {
        const Bytes a2 = make_store(2, 2, 2);
        guids.assign(1, Guid16{});
        CHECK(parse_guid_store(a2.data(), a2.size(), guids, &detail) == ErrorCode::CorruptPackage);
        CHECK(detail.rfind("LcOpGuidStore at byte 0: ", 0) == 0);
        CHECK(guids.empty());

        const Bytes a0 = make_store(0, 2, 2);
        CHECK(parse_guid_store(a0.data(), a0.size(), guids) == ErrorCode::CorruptPackage);

        const Bytes neg = make_store(1, -1, 0);
        CHECK(parse_guid_store(neg.data(), neg.size(), guids) == ErrorCode::CorruptPackage);

        const Bytes few = make_store(1, 3, 2);     // ett GUID för lite
        CHECK(parse_guid_store(few.data(), few.size(), guids, &detail) == ErrorCode::CorruptPackage);
        CHECK(detail.rfind("LcOpGuidStore at byte 4: ", 0) == 0);

        const Bytes many = make_store(1, 1, 2);    // ett GUID för mycket
        CHECK(parse_guid_store(many.data(), many.size(), guids) == ErrorCode::CorruptPackage);

        Bytes extra = make_store(1, 2, 2);         // en byte för mycket
        extra.push_back(0);
        CHECK(parse_guid_store(extra.data(), extra.size(), guids) == ErrorCode::CorruptPackage);

        Bytes shorter = make_store(1, 2, 2);       // en byte för lite
        shorter.pop_back();
        CHECK(parse_guid_store(shorter.data(), shorter.size(), guids) == ErrorCode::CorruptPackage);

        CHECK(parse_guid_store(nullptr, 0, guids) == ErrorCode::CorruptPackage);
    }

    // Ett antal som inte ryms får inte leda till en allokering, och 16·n får
    // inte slå runt i 32 bitar.
    for (const int32_t n : {0x7FFFFFFF, 0x10000000, 0x20000000, 0x40000000}) {
        const Bytes d = make_store(1, n, 1);
        CHECK(parse_guid_store(d.data(), d.size(), guids) == ErrorCode::CorruptPackage);
        CHECK(guids.empty());
    }

    // Varje avhuggen längd ger ett fel.
    {
        const Bytes d = make_store(1, 3, 3);
        for (size_t n = 0; n < d.size(); ++n)
            CHECK(parse_guid_store(d.data(), n, guids) == ErrorCode::CorruptPackage);
        CHECK(parse_guid_store(d.data(), d.size(), guids) == ErrorCode::Ok);
    }

    // guid_text: de tre första fälten vänds, resten ligger som de är.
    {
        const Guid16 g = {0x33, 0x22, 0x11, 0x00, 0x55, 0x44, 0x77, 0x66,
                          0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
        char text[37];
        std::memset(text, 'x', sizeof text);
        guid_text(g, text);
        CHECK(std::strcmp(text, "00112233-4455-6677-8899-AABBCCDDEEFF") == 0);

        const Guid16 zero{};
        guid_text(zero, text);
        CHECK(std::strcmp(text, "00000000-0000-0000-0000-000000000000") == 0);

        Guid16 ff;
        ff.fill(0xFF);
        guid_text(ff, text);
        CHECK(std::strcmp(text, "FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF") == 0);
    }

    // Exempelfilerna: antalet GUID enligt tabellen i test_data.h.
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
        const DbCacheEntry* e = find_entry(c.entries, "LcOpGuidStore");
        CHECK(e != nullptr);
        if (!e) continue;
        StreamSpan span;
        Bytes data;
        CHECK(last_stream(*e, file.size(), span) == ErrorCode::Ok);
        CHECK(inflate_span(file.data(), file.size(), span, kDefaultMaxStreamBytes, data) ==
              ErrorCode::Ok);
        const ErrorCode r = parse_guid_store(data.data(), data.size(), guids, &detail);
        if (r != ErrorCode::Ok) std::printf("  %s: %s\n", sample.path, detail.c_str());
        CHECK(r == ErrorCode::Ok);
        CHECK(guids.size() == sample.guids);
        if (!guids.empty()) {
            char text[37];
            guid_text(guids.front(), text);
            CHECK(std::strlen(text) == 36 && text[8] == '-' && text[13] == '-' &&
                  text[18] == '-' && text[23] == '-');
        }
    }

    std::printf("test_guid_store: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
