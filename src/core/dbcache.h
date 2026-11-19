#pragma once
#include "nwd_error.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace HNwd::detail {

// En post i katalogen: ett namngivet paket och var i filen det ligger.
//
//   DIRECTORY := [int32 -1][int32 n] ENTRY{n}        slutar exakt vid strömmens slut
//   ENTRY     := [str name][int32 flags] PAD8 [int64 offset][int64 length][int32 a][int32 b]
//   PAD8      := en int32 0 om markören står på 4 mod 8, annars ingenting
struct DbCacheEntry {
    std::string name;      // som den står i filen, med eventuellt sökvägsprefix
    int32_t flags = 0;     // 1 = zlib-strömmar, 3 = råa byte
    uint64_t offset = 0;   // absolut i filen
    uint64_t length = 0;   // antal byte i filen
    int32_t a = 0;         // (0, 0): en zlib-ström på hela längden
    int32_t b = 0;         // (4, b): 4 byte prefix, sedan strömmar; den sista är b byte

    // Namnet efter sista bakstrecket — det paket matchas på.
    std::string_view leaf() const;
};

// Tolkar en uppackad katalogström. `out` ersätts.
ErrorCode parse_dbcache(const uint8_t* data, size_t size, std::vector<DbCacheEntry>& out);

// Paketet med det här namnet efter sista bakstrecket, eller nullptr.
const DbCacheEntry* find_entry(const std::vector<DbCacheEntry>& entries,
                               std::string_view leaf);

} // namespace HNwd::detail
