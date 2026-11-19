#include "container.h"

#include <algorithm>
#include <cstring>
#include <string_view>

namespace HNwd::detail {

namespace {

constexpr char kMarker[] = "#LcUStream";
constexpr size_t kMarkerLength = sizeof kMarker - 1;
// En rubrik är en kort rad. Taket gör att en saknad radbrytning ger ett fel i
// stället för en sökning genom resten av filen.
constexpr size_t kMaxHeaderLength = 256;
// Katalogen är liten: några tiotal poster. Taket skyddar mot en ström som
// bara råkar börja som en katalog.
constexpr size_t kMaxDirectoryBytes = size_t(16) * 1024 * 1024;

bool starts_with_marker(const uint8_t* data, size_t size, uint64_t offset) {
    return offset <= size && size - offset >= kMarkerLength &&
           std::memcmp(data + offset, kMarker, kMarkerLength) == 0;
}

void set_detail(std::string* detail, std::string text) {
    if (detail) *detail = std::move(text);
}

// Katalogen ska täcka datasektionen utan glapp och utan överlapp.
bool tiles(const Container& c, std::string* detail) {
    uint64_t expect = c.data_section.length;
    for (size_t i = 0; i < c.entries.size(); ++i) {
        const DbCacheEntry& e = c.entries[i];
        if (e.offset != expect) {
            set_detail(detail, "package " + std::to_string(i) + " (" +
                                   std::string(e.leaf()) + ") starts at " +
                                   std::to_string(e.offset) + ", expected " +
                                   std::to_string(expect));
            return false;
        }
        if (e.length > c.directory_section.offset - e.offset) {
            set_detail(detail, "package " + std::to_string(i) + " (" +
                                   std::string(e.leaf()) +
                                   ") runs past the directory section");
            return false;
        }
        expect = e.offset + e.length;
    }
    if (expect != c.directory_section.offset) {
        set_detail(detail, "packages end at " + std::to_string(expect) +
                               ", directory section starts at " +
                               std::to_string(c.directory_section.offset));
        return false;
    }
    return true;
}

} // namespace

ErrorCode parse_section_header(const uint8_t* data, size_t size, uint64_t offset,
                               SectionHeader& out) {
    if (!starts_with_marker(data, size, offset)) return ErrorCode::CorruptHeader;

    const size_t start = static_cast<size_t>(offset);
    const size_t limit = std::min(size - start, kMaxHeaderLength);
    size_t end = 0;
    for (; end < limit; ++end) {
        const uint8_t c = data[start + end];
        if (c == '\n') break;
        if (c < 0x20 || c > 0x7E) return ErrorCode::CorruptHeader;
    }
    if (end == limit) return ErrorCode::CorruptHeader;

    out = SectionHeader{};
    out.offset = offset;
    out.length = static_cast<uint32_t>(end + 1);
    out.line.assign(reinterpret_cast<const char*>(data + start), end);

    // Orden skiljs av ett blanksteg. Det första är formatet med sitt '#'.
    std::string_view rest = out.line;
    bool first = true;
    while (!rest.empty()) {
        const size_t space = rest.find(' ');
        const std::string_view token = rest.substr(0, space);
        rest = space == std::string_view::npos ? std::string_view() : rest.substr(space + 1);
        if (first) {
            out.format = token.substr(1);
            first = false;
        } else if (token == "double") {
            out.is_double = true;
        } else if (token == "utf8" || token == "C") {
            out.encoding = token;
        } else if (token == "lizlib" || token.starts_with("lichunk-")) {
            out.marker = token;
        } else if (const size_t colon = token.find(':'); colon != std::string_view::npos) {
            // "navisworks:448" — bara siffror efter kolonet räknas som en version.
            const std::string_view digits = token.substr(colon + 1);
            int value = 0;
            bool valid = !digits.empty() && digits.size() <= 9;
            for (const char ch : digits) {
                if (ch < '0' || ch > '9') {
                    valid = false;
                    break;
                }
                value = value * 10 + (ch - '0');
            }
            if (valid) out.version = value;
        }
    }
    return ErrorCode::Ok;
}

// Prövar om det på `pos` står en rubrik följd av en katalog, och fyller i så
// fall i `out`. Mellan rubriken och strömmen ligger nollor som fyller ut
// rubriken till en multipel av 8 byte.
static bool try_directory(const uint8_t* data, size_t size, size_t pos, Container& out) {
    SectionHeader header;
    if (parse_section_header(data, size, pos, header) != ErrorCode::Ok) return false;

    size_t stream = pos + header.length;
    for (int pad = 0; pad < 7 && stream < size && data[stream] == 0; ++pad) ++stream;
    if (stream >= size) return false;

    std::vector<uint8_t> inflated;
    size_t consumed = 0;
    if (inflate_zlib(data + stream, size - stream, inflated, kMaxDirectoryBytes,
                     &consumed) != ErrorCode::Ok)
        return false;
    if (parse_dbcache(inflated.data(), inflated.size(), out.entries) != ErrorCode::Ok)
        return false;

    out.directory_section = header;
    out.directory_offset = stream;
    out.directory_length = consumed;
    return true;
}

// Filen slutar med en rubrik utan innehåll, och de 8 byten före den är
// katalogsektionens offset som int64. Ger den offseten, eller 0 om filen inte
// slutar så.
static size_t directory_offset_from_tail(const uint8_t* data, size_t size) {
    const size_t window = std::min(size, kMaxHeaderLength);
    for (size_t pos = size - kMarkerLength; pos >= size - window && pos > 0; --pos) {
        if (data[pos] != '#' || !starts_with_marker(data, size, pos)) continue;
        SectionHeader tail;
        if (parse_section_header(data, size, pos, tail) != ErrorCode::Ok ||
            tail.offset + tail.length != size || pos < 8)
            return 0;
        uint64_t offset = 0;
        for (int i = 7; i >= 0; --i) offset = (offset << 8) | data[pos - 8 + i];
        return offset < pos ? static_cast<size_t>(offset) : 0;
    }
    return 0;
}

ErrorCode read_container(const uint8_t* data, size_t size, Container& out,
                         std::string* detail) {
    out = Container{};
    if (!data || size < kMarkerLength ||
        parse_section_header(data, size, 0, out.data_section) != ErrorCode::Ok)
        return ErrorCode::NotLcUStream;
    const size_t first = out.data_section.length;

    // Den snabba vägen: filens slut säger var katalogsektionen börjar.
    bool found = false;
    if (const size_t at = directory_offset_from_tail(data, size); at >= first)
        found = try_directory(data, size, at, out);

    // Annars bakifrån, rubrik för rubrik — aldrig den på offset 0, som är
    // datasektionens.
    for (size_t pos = size; !found && pos-- > first;) {
        if (data[pos] == '#' && starts_with_marker(data, size, pos))
            found = try_directory(data, size, pos, out);
    }
    if (!found) {
        out.entries.clear();
        return ErrorCode::NoDirectory;
    }
    return tiles(out, detail) ? ErrorCode::Ok : ErrorCode::CorruptDirectory;
}

ErrorCode last_stream(const DbCacheEntry& entry, size_t file_size, StreamSpan& out) {
    if (entry.offset > file_size || entry.length > file_size - entry.offset)
        return ErrorCode::OutOfRange;

    if (entry.a == 0 && entry.b == 0) {
        out.offset = entry.offset;
        out.length = entry.length;
        return ErrorCode::Ok;
    }
    if (entry.a == 4 && entry.b > 0) {
        const uint64_t tail = static_cast<uint64_t>(entry.b);
        // Prefixet på 4 byte ska rymmas före den sista strömmen.
        if (entry.length < 4 || tail > entry.length - 4) return ErrorCode::OutOfRange;
        out.offset = entry.offset + entry.length - tail;
        out.length = tail;
        return ErrorCode::Ok;
    }
    return ErrorCode::OutOfRange;
}

bool read_prefix(const uint8_t* file, size_t size, const DbCacheEntry& entry,
                 int32_t& prefix) {
    // Formen och gränserna prövas av last_stream; den garanterar också att
    // paketet rymmer de fyra byten.
    StreamSpan unused;
    if (!file || last_stream(entry, size, unused) != ErrorCode::Ok || entry.a != 4)
        return false;
    const uint8_t* p = file + static_cast<size_t>(entry.offset);
    prefix = static_cast<int32_t>(static_cast<uint32_t>(p[0]) |
                                  (static_cast<uint32_t>(p[1]) << 8) |
                                  (static_cast<uint32_t>(p[2]) << 16) |
                                  (static_cast<uint32_t>(p[3]) << 24));
    return true;
}

ErrorCode split_prefixed(const DbCacheEntry& entry, size_t file_size,
                         const std::vector<uint32_t>& sizes,
                         std::vector<StreamSpan>& out, std::string* detail) {
    out.clear();
    StreamSpan last;
    if (entry.a != 4 || last_stream(entry, file_size, last) != ErrorCode::Ok) {
        set_detail(detail, std::string(entry.leaf()) + " at byte " +
                               std::to_string(entry.offset) +
                               ": package is not of the form (4, b) inside the file");
        return ErrorCode::OutOfRange;
    }

    // Summan räknas i 64 bitar: en längd är högst 2^32 - 1 och antalet längder
    // ryms i minnet, så summan kan inte slå runt.
    uint64_t sum = 0;
    for (size_t i = 0; i < sizes.size(); ++i) {
        if (sizes[i] == 0) {
            set_detail(detail, std::string(entry.leaf()) + " at byte " +
                                   std::to_string(entry.offset) + ": stream " +
                                   std::to_string(i) + " has length 0");
            return ErrorCode::CorruptPackage;
        }
        sum += sizes[i];
    }
    if (sum != entry.length - 4 - last.length) {
        set_detail(detail, std::string(entry.leaf()) + " at byte " +
                               std::to_string(entry.offset) +
                               ": 4 + stream lengths + b does not equal the package length");
        return ErrorCode::CorruptPackage;
    }

    out.reserve(sizes.size());
    uint64_t at = entry.offset + 4;
    for (const uint32_t length : sizes) {
        out.push_back({at, length});
        at += length;
    }
    return ErrorCode::Ok;
}

ErrorCode inflate_span(const uint8_t* file, size_t size, const StreamSpan& span,
                       size_t max_output, std::vector<uint8_t>& out, std::string* detail) {
    out.clear();
    if (span.offset > size || span.length > size - span.offset) {
        set_detail(detail, "stream at byte " + std::to_string(span.offset) +
                               ": the stream runs past the end of the file");
        return ErrorCode::OutOfRange;
    }
    size_t consumed = 0;
    const ErrorCode e = inflate_zlib(file + static_cast<size_t>(span.offset),
                                     static_cast<size_t>(span.length), out, max_output,
                                     &consumed);
    if (e != ErrorCode::Ok) {
        set_detail(detail, "stream at byte " + std::to_string(span.offset) + ": " +
                               error_message(e));
        return e;
    }
    if (consumed != span.length) {
        set_detail(detail, "stream at byte " + std::to_string(span.offset) +
                               ": the zlib stream ends at " + std::to_string(consumed) +
                               " of " + std::to_string(span.length) + " bytes");
        return ErrorCode::CorruptStream;
    }
    return ErrorCode::Ok;
}

} // namespace HNwd::detail
