#include "guid_store.h"

#include <cstring>

namespace HNwd::detail {

namespace {

constexpr char kPackage[] = "LcOpGuidStore";

ErrorCode corrupt(std::string* detail, size_t at, const std::string& rule) {
    if (detail) *detail = std::string(kPackage) + " at byte " + std::to_string(at) + ": " + rule;
    return ErrorCode::CorruptPackage;
}

int32_t read_i32(const uint8_t* p) {
    return static_cast<int32_t>(static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                                (static_cast<uint32_t>(p[2]) << 16) |
                                (static_cast<uint32_t>(p[3]) << 24));
}

} // namespace

ErrorCode parse_guid_store(const uint8_t* data, size_t size, std::vector<Guid16>& out,
                           std::string* detail) {
    out.clear();
    if (!data || size < 8) return corrupt(detail, 0, "shorter than the 8-byte head");

    const int32_t a = read_i32(data);
    if (a != 1) return corrupt(detail, 0, "first int32 is " + std::to_string(a) + ", not 1");

    const int32_t n = read_i32(data + 4);
    // Längden prövas mot antalet innan något allokeras.
    if (n < 0 || static_cast<uint64_t>(size) != 8 + uint64_t(16) * static_cast<uint32_t>(n))
        return corrupt(detail, 4,
                       "count " + std::to_string(n) + " does not fill " + std::to_string(size) +
                           " bytes");

    out.resize(static_cast<size_t>(n));
    for (size_t i = 0; i < out.size(); ++i) std::memcpy(out[i].data(), data + 8 + 16 * i, 16);
    return ErrorCode::Ok;
}

void guid_text(const Guid16& guid, char out[37]) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    // Läsordningen: Data1 (4 byte), Data2 (2), Data3 (2) vända, resten som de ligger.
    static constexpr int kOrder[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    size_t at = 0;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[at++] = '-';
        const uint8_t b = guid[static_cast<size_t>(kOrder[i])];
        out[at++] = kHex[b >> 4];
        out[at++] = kHex[b & 0x0F];
    }
    out[at] = '\0';
}

} // namespace HNwd::detail
