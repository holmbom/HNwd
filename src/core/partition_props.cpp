#include "partition_props.h"

#include "byte_reader.h"

namespace HNwd::detail {

namespace {

ErrorCode corrupt(std::string* detail, const DbCacheEntry& entry, uint64_t at,
                  const char* rule) {
    if (detail)
        *detail = std::string(entry.leaf()) + " at byte " + std::to_string(at) + ": " + rule;
    return ErrorCode::CorruptPackage;
}

} // namespace

ErrorCode locate_partition_props(const uint8_t* file, size_t size, const DbCacheEntry& entry,
                                 size_t max_stream, std::vector<StreamSpan>& spans,
                                 std::string* detail) {
    spans.clear();
    if (detail) detail->clear();

    // Formen (4, b) och gränserna mot filen.
    StreamSpan toc_span;
    if (const ErrorCode e = last_stream(entry, size, toc_span); e != ErrorCode::Ok || entry.a != 4) {
        if (detail)
            *detail = std::string(entry.leaf()) + " at byte " + std::to_string(entry.offset) +
                      ": package is not of the form (4, b) inside the file";
        return ErrorCode::CorruptPackage;
    }

    int32_t prefix = 0;
    if (!read_prefix(file, size, entry, prefix))
        return corrupt(detail, entry, entry.offset, "package prefix cannot be read");
    if (prefix != 0) return corrupt(detail, entry, entry.offset, "package prefix is not 0");

    // Innehållsförteckningen: [int32 k][k × int32 längd] nollor upp till 8.
    std::vector<uint8_t> toc;
    if (const ErrorCode e = inflate_span(file, size, toc_span, max_stream, toc, detail);
        e != ErrorCode::Ok)
        return e;

    ByteReader t(toc.data(), toc.size());
    int32_t k = 0;
    if (!t.i32(k)) return corrupt(detail, entry, toc_span.offset, "table of contents is shorter than 4 bytes");
    if (k < 0) return corrupt(detail, entry, toc_span.offset, "table of contents count k is negative");

    // k prövas mot innehållsförteckningens längd innan något allokeras för den.
    const uint64_t expected = (4 + 4 * static_cast<uint64_t>(k) + 7) & ~uint64_t(7);
    if (toc.size() != expected)
        return corrupt(detail, entry, toc_span.offset,
                       "table of contents is not exactly roundup8(4 + 4k) bytes");

    std::vector<uint32_t> sizes;
    sizes.reserve(static_cast<size_t>(k));
    for (int32_t i = 0; i < k; ++i) {
        int32_t length = 0;
        t.i32(length);
        if (length <= 0)
            return corrupt(detail, entry, toc_span.offset,
                           "table of contents has a stream length that is 0 or negative");
        sizes.push_back(static_cast<uint32_t>(length));
    }
    if (!t.zero_pad(8) || !t.at_end())
        return corrupt(detail, entry, toc_span.offset, "table of contents pad is not zero");

    if (const ErrorCode e = split_prefixed(entry, size, sizes, spans, detail); e != ErrorCode::Ok) {
        spans.clear();
        return e;
    }
    return ErrorCode::Ok;
}

ErrorCode read_partition_props(const uint8_t* file, size_t size, const DbCacheEntry& entry,
                               size_t max_stream, std::vector<std::vector<uint8_t>>& streams,
                               std::string* detail) {
    streams.clear();
    std::vector<StreamSpan> spans;
    if (const ErrorCode e = locate_partition_props(file, size, entry, max_stream, spans, detail);
        e != ErrorCode::Ok)
        return e;

    // Taket gäller strömmarna tillsammans: de hålls alla i minnet samtidigt.
    size_t budget = max_stream;
    streams.reserve(spans.size());
    for (const StreamSpan& span : spans) {
        std::vector<uint8_t> data;
        if (const ErrorCode e = inflate_span(file, size, span, budget, data, detail);
            e != ErrorCode::Ok) {
            streams.clear();
            return e;
        }
        budget -= data.size();
        data.shrink_to_fit();
        streams.push_back(std::move(data));
    }
    return ErrorCode::Ok;
}

} // namespace HNwd::detail
