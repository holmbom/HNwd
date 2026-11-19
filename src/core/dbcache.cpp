#include "dbcache.h"
#include "byte_reader.h"

namespace HNwd::detail {

namespace {
// Namnlängd, flaggor, offset, längd, a och b — utan namnbyte och utan PAD8.
constexpr size_t kMinEntrySize = 32;
}

std::string_view DbCacheEntry::leaf() const {
    const std::string_view n = name;
    const size_t slash = n.rfind('\\');
    return slash == std::string_view::npos ? n : n.substr(slash + 1);
}

ErrorCode parse_dbcache(const uint8_t* data, size_t size, std::vector<DbCacheEntry>& out) {
    out.clear();
    ByteReader r(data, size);

    int32_t head, count;
    if (!r.i32(head) || !r.i32(count) || head != -1 || count < 0)
        return ErrorCode::CorruptDirectory;
    // Antalet prövas mot datan innan något allokeras efter det.
    if (static_cast<size_t>(count) > r.remaining() / kMinEntrySize)
        return ErrorCode::CorruptDirectory;

    out.reserve(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i) {
        DbCacheEntry e;
        bool is_null = false;
        int64_t offset, length;
        if (!r.str(e.name, &is_null) || is_null || !r.i32(e.flags) || !r.zero_pad(8) ||
            !r.i64(offset) || !r.i64(length) || !r.i32(e.a) || !r.i32(e.b))
            return ErrorCode::CorruptDirectory;
        if (offset < 0 || length < 0) return ErrorCode::CorruptDirectory;
        e.offset = static_cast<uint64_t>(offset);
        e.length = static_cast<uint64_t>(length);
        out.push_back(std::move(e));
    }
    if (!r.at_end()) return ErrorCode::CorruptDirectory;
    return ErrorCode::Ok;
}

const DbCacheEntry* find_entry(const std::vector<DbCacheEntry>& entries,
                               std::string_view leaf) {
    for (const DbCacheEntry& e : entries)
        if (e.leaf() == leaf) return &e;
    return nullptr;
}

} // namespace HNwd::detail
