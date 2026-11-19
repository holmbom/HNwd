#pragma once
#include "nwd_error.h"
#include <cstddef>
#include <cstdint>
#include <string>

namespace HNwd::detail {

// Varifrån en .nwc kommer: paketet LcOpDBCache.
//
//   [int64 7][str källsökväg][int32 -1] PAD8 [int64 unixtid][int64 källstorlek][str loader] …
//
// Efter loader-namnet följer inställningar för exporten. De tolkas inte.
struct SourceHeader {
    std::string path;         // källfilens sökväg; tom om path_null
    bool path_null = false;   // sökvägen är en null-sträng
    int64_t unix_time = 0;    // källfilens ändringstid i sekunder, 0 om ingen
    int64_t source_size = 0;  // källfilens storlek i byte, 0 om ingen
    std::string loader;       // den inläsare som gjorde filen
};

// Tolkar början av paketet, fram till och med loader-namnet. CorruptPackage
// om något fält fram till dess inte stämmer: det första talet är inte 7, talet
// efter sökvägen är inte -1, utfyllnad är inte noll eller strömmen tar slut.
ErrorCode parse_source_header(const uint8_t* data, size_t size, SourceHeader& out,
                              std::string* detail = nullptr);

} // namespace HNwd::detail
