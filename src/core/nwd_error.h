#pragma once
#include <cstdint>

namespace HNwd::detail {

// Varför en läsning misslyckades. Varje kod har en text i error_message(), och
// kCount står sist så att ett test kan gå igenom alla.
enum class ErrorCode : uint8_t {
    Ok = 0,
    FileNotFound,
    FileOpenFailed,
    MmapFailed,
    NotLcUStream,       // filen börjar inte med en sektionsrubrik
    CorruptHeader,      // en sektionsrubrik går inte att tolka
    NoDirectory,        // ingen sektion med en katalog hittades
    CorruptDirectory,   // katalogen går inte att tolka eller täcker inte filen
    CorruptStream,      // en zlib-ström är trasig eller avhuggen
    StreamTooLarge,     // en uppackad ström slår i taket
    OutOfRange,         // en post pekar utanför filen
    CorruptObject,      // en objektström bryter grammatiken: pekare, klass, utfyllnad, antal, slut
    CorruptPackage,     // ett pakets ram: strömmarna i formen (4, b), GUID-tabellens längd
    CorruptGeometry,    // en geometripost eller geometrins innehållsförteckning
    CorruptLink,        // paketen stämmer inte med varandra: seq, post, bbox, antal
    Unsupported,        // giltigt men inte avkodat
    LimitExceeded,      // nästlingsdjup eller antal noder över taket
    PackageMissing,     // paketet finns inte i katalogen
    NotLoaded,          // anropet kräver ett paket som inte är läst
    Cancelled,          // anroparen avbröt genom förloppsfunktionen
    kCount,
};

// Aldrig nullptr, inte heller för ett värde utanför enumet.
const char* error_message(ErrorCode code);

} // namespace HNwd::detail
