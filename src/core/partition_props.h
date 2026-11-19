#pragma once
#include "container.h"
#include "dbcache.h"
#include "nwd_error.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace HNwd::detail {

// Paketet LcOaPartitionProps: de egenskapsflikar som bara en nod använder.
// Formen (4, b) med prefixet int32 0:
//
//   [int32 0] ZSTREAM{k} ZTOC
//   ZTOC := [int32 k][k × int32 komprimerad längd] nollor upp till 8
//
// Varje ZSTREAM är en objektström med rötter som trädet delar ut till sina
// tomma attributplatser; de tolkas av parse_hierarchy().
//
// Var de k strömmarna ligger i filen, i ordning, utan att någon av dem packas
// upp: bara innehållsförteckningen packas upp (med taket `max_stream`).
// `spans` ersätts. En ström packas sedan upp för sig, med inflate_span() eller
// genom en InflateStream över filen från span.offset, och ska ta exakt
// span.length byte.
//
// CorruptPackage: paketet har inte formen (4, b) inuti filen, prefixet är inte
// 0, ZTOC är inte exakt roundup8(4 + 4k) byte, dess utfyllnad är inte noll, k
// är negativt, en längd är 0 eller negativ, eller längderna går inte ihop med
// paketets längd. Fel från uppackningen av ZTOC lämnas vidare som de är.
ErrorCode locate_partition_props(const uint8_t* file, size_t size, const DbCacheEntry& entry,
                                 size_t max_stream, std::vector<StreamSpan>& spans,
                                 std::string* detail = nullptr);

// Packar upp de k strömmarna, i ordning, och håller dem alla i minnet.
// `streams` ersätts. `max_stream` är taket för strömmarna TILLSAMMANS.
//
// Samma fel som locate_partition_props(); fel från uppackningen av en ström
// lämnas vidare som de är.
ErrorCode read_partition_props(const uint8_t* file, size_t size, const DbCacheEntry& entry,
                               size_t max_stream, std::vector<std::vector<uint8_t>>& streams,
                               std::string* detail = nullptr);

} // namespace HNwd::detail
