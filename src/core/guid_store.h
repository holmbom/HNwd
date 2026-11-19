#pragma once
#include "nwd_error.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace HNwd::detail {

// En GUID som den ligger i filen: 16 byte i Windows-ordning, där de tre första
// fälten är little-endian.
using Guid16 = std::array<uint8_t, 16>;

// Tolkar paketet LcOpGuidStore:
//
//   [int32 1][int32 n][n × 16 byte]
//
// `out` ersätts. CorruptPackage om det första talet inte är 1, om n är
// negativt eller om strömmen inte är exakt 8 + 16n byte.
ErrorCode parse_guid_store(const uint8_t* data, size_t size, std::vector<Guid16>& out,
                           std::string* detail = nullptr);

// Skriver en GUID som "00112233-4455-6677-8899-AABBCCDDEEFF" med avslutande
// NUL: de tre första fälten vända till läsordning, resten som de ligger.
void guid_text(const Guid16& guid, char out[37]);

} // namespace HNwd::detail
