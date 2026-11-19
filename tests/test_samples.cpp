#include <cstdio>
#include <string>
#include <vector>

#include "container.h"
#include "inflate.h"
#include "mapped_file.h"
#include "test_data.h"
#include "test_support.h"

using namespace HNwd::detail;

namespace {

// Öppnar en exempelfil och prövar det yttre lagret: katalogen hittas och
// täcker datasektionen, de paket läsaren behöver finns, och varje pakets sista
// ström går att packa upp på exakt den längd katalogen anger.
int check_file(const char* path, size_t expected_packages) {
    int fails = 0;

    MappedFile file;
    CHECK(file.open(path) == ErrorCode::Ok);
    if (!file.is_open()) return fails;

    Container c;
    std::string detail;
    const ErrorCode e = read_container(file.data(), file.size(), c, &detail);
    if (e != ErrorCode::Ok) {
        std::printf("  FAIL — %s: %s %s\n", path, error_message(e), detail.c_str());
        return fails + 1;
    }
    CHECK(c.directory_section.version == testdata::kSampleVersion);
    CHECK(c.directory_section.marker == "lizlib");
    CHECK(c.data_section.is_double);
    CHECK(c.entries.size() == expected_packages);

    for (const char* name : {"LcOpNwdGeometry", "LcOpNwdFragments",
                             "LcOpNwdLogicalHierarchy", "LcOaPartitionProps",
                             "LcOpGuidStore", "LcOpNwdPartition"}) {
        if (!find_entry(c.entries, name)) {
            std::printf("  FAIL — %s: paketet %s saknas\n", path, name);
            ++fails;
        }
    }

    std::vector<uint8_t> out;
    for (const DbCacheEntry& p : c.entries) {
        if (p.flags != 1) continue;
        StreamSpan span;
        size_t consumed = 0;
        const bool ok =
            last_stream(p, file.size(), span) == ErrorCode::Ok &&
            inflate_zlib(file.data() + span.offset, static_cast<size_t>(span.length),
                         out, kDefaultMaxStreamBytes, &consumed) == ErrorCode::Ok &&
            consumed == span.length;
        if (!ok) {
            std::printf("  FAIL — %s: paketet %s\n", path, std::string(p.leaf()).c_str());
            ++fails;
        }
    }
    return fails;
}

} // namespace

int test_samples() {
    SKIP_IF_MISSING(testdata::kSampleNwd);
    int fails = check_file(testdata::kSampleNwd, testdata::kSampleNwdPackages);

    for (const char* path : testdata::kSampleNwc) {
        if (!testdata::exists(path)) {
            std::printf("  SKIP — %s saknas\n", path);
            ++g_skipped;
            continue;
        }
        fails += check_file(path, testdata::kSampleNwcPackages);
    }

    // En fil som inte finns, och en sökväg med tecken utanför ASCII.
    MappedFile missing;
    CHECK(missing.open("nwd/finns-inte.nwd") == ErrorCode::FileNotFound);
    CHECK(missing.open("nwd/finns-inte-\xC3\xA5\xC3\xA4\xC3\xB6.nwd") ==
          ErrorCode::FileNotFound);
    CHECK(missing.open("") == ErrorCode::FileNotFound);
    CHECK(!missing.is_open());

    std::printf("test_samples: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
