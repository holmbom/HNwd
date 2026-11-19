#include <cstdio>
#include <cstring>

#include "HNwd.h"
#include "nwd_error.h"

// Varje felkod har en egen text, och ingen av dem är reservtexten. En ny kod
// som glöms i error_message() faller här.
int test_error() {
    using HNwd::detail::ErrorCode;
    using HNwd::detail::error_message;

    int fails = 0;
    const int count = static_cast<int>(ErrorCode::kCount);
    const char* const fallback = error_message(ErrorCode::kCount);

    for (int i = 0; i < count; ++i) {
        const char* msg = error_message(static_cast<ErrorCode>(i));
        if (!msg || !*msg || std::strcmp(msg, fallback) == 0) {
            std::printf("  FAIL — felkod %d saknar text\n", i);
            ++fails;
        }
        for (int j = 0; j < i; ++j) {
            if (std::strcmp(msg, error_message(static_cast<ErrorCode>(j))) == 0) {
                std::printf("  FAIL — felkod %d och %d har samma text\n", j, i);
                ++fails;
            }
        }
    }

    // Versionssträngen är byggd av de tre talen.
    char expect[32];
    std::snprintf(expect, sizeof expect, "%d.%d.%d", HNwd::kVersionMajor,
                  HNwd::kVersionMinor, HNwd::kVersionPatch);
    if (std::strcmp(expect, HNwd::kVersionString) != 0) {
        std::printf("  FAIL — kVersionString \"%s\" != \"%s\"\n",
                    HNwd::kVersionString, expect);
        ++fails;
    }

    std::printf("test_error: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
