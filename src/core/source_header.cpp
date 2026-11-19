#include "source_header.h"

#include "object_reader.h"

namespace HNwd::detail {

namespace {
constexpr char kPackage[] = "LcOpDBCache";
}

ErrorCode parse_source_header(const uint8_t* data, size_t size, SourceHeader& out,
                              std::string* detail) {
    out = SourceHeader{};
    ObjectReader r(data, size, kPackage);

    SourceHeader h;
    int64_t marker = 0;
    int32_t minus_one = 0;
    bool loader_null = false;

    // Varje läsning som misslyckas har redan skrivit sin regel i r.detail().
    bool good = r.i64(marker);
    if (good && marker != 7) good = r.fail("first int64 is not 7");
    good = good && r.str(h.path, &h.path_null);
    good = good && r.i32(minus_one);
    if (good && minus_one != -1) good = r.fail("the int32 after the path is not -1");
    good = good && r.i64(h.unix_time);
    if (good && h.unix_time < 0) good = r.fail("source time is negative");
    good = good && r.i64(h.source_size);
    if (good && h.source_size < 0) good = r.fail("source size is negative");
    good = good && r.str(h.loader, &loader_null);
    if (good && loader_null) good = r.fail("loader name is a null string");

    if (!good) {
        if (detail) *detail = r.detail();
        return ErrorCode::CorruptPackage;
    }
    out = std::move(h);
    return ErrorCode::Ok;
}

} // namespace HNwd::detail
