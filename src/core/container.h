#pragma once
#include "dbcache.h"
#include "inflate.h"
#include "nwd_error.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace HNwd::detail {

// En sektionsrubrik: en ASCII-rad som slutar på '\n', till exempel
//
//   #LcUStream-V1.1 binary double release utf8 navisworks:448 lizlib
//   #LcUStream-V1.1 binary double release C lichunk-008
struct SectionHeader {
    uint64_t offset = 0;      // var i filen raden börjar
    uint32_t length = 0;      // radens längd med avslutande '\n'
    std::string line;         // raden utan '\n'
    std::string format;       // "LcUStream-V1.1"
    std::string encoding;     // "utf8" eller "C", tom om ingen angavs
    std::string marker;       // "lizlib" eller "lichunk-NNN", tom om ingen angavs
    int version = -1;         // talet i "navisworks:448", -1 om det saknas
    bool is_double = false;   // flyttal i poser och matriser är 64 bitar
};

// Tolkar rubriken som börjar på `offset`. CorruptHeader om där inte står en
// rubrik: fel inledning, ingen radbrytning inom rimlig längd eller tecken som
// inte är skrivbar ASCII.
ErrorCode parse_section_header(const uint8_t* data, size_t size, uint64_t offset,
                               SectionHeader& out);

// Filens yttre form: datasektionen först, och katalogsektionen som listar
// paketen i den.
struct Container {
    SectionHeader data_section;        // rubriken på offset 0
    SectionHeader directory_section;   // rubriken före katalogströmmen
    uint64_t directory_offset = 0;     // var katalogens zlib-ström börjar
    uint64_t directory_length = 0;     // dess komprimerade längd
    std::vector<DbCacheEntry> entries; // katalogen, i filordning
};

// Hittar katalogen och tolkar den.
//
// Katalogsektionen ligger efter datasektionen, nära filens slut:
//
//   [rubrik][nollor till 8 byte][katalogens zlib-ström][8 byte][int64 offset][rubrik]
//
// där offseten pekar på katalogsektionens rubrik och den sista rubriken saknar
// innehåll. Slutar filen inte så letas katalogen upp bakifrån: varje förekomst
// av en rubrik prövas genom att strömmen efter den packas upp och tolkas som en
// katalog, så att en bytesekvens som bara råkar se ut som en rubrik mitt i
// komprimerad data inte kan tas för en.
//
// En katalog godtas bara om den täcker datasektionen exakt: det första paketet
// börjar direkt efter datasektionens rubrik, varje paket börjar där det förra
// slutar, och det sista slutar där katalogsektionens rubrik börjar.
//
// NotLcUStream: filen börjar inte med en rubrik. NoDirectory: ingen rubrik
// följs av en katalog. CorruptDirectory: katalogen täcker inte datasektionen.
// `detail` får, om den ges, en rad om vad som inte gick ihop.
ErrorCode read_container(const uint8_t* data, size_t size, Container& out,
                         std::string* detail = nullptr);

// Zlib-strömmarna i ett paket, i filordning.
struct StreamSpan {
    uint64_t offset = 0;   // absolut i filen
    uint64_t length = 0;   // komprimerad längd
};

// Var paketets SISTA ström ligger: hela paketet för formen (0, 0), och de `b`
// sista byten för formen (4, b). I formen (4, b) är den sista strömmen en
// innehållsförteckning över de övriga. OutOfRange om posten pekar utanför
// filen eller formen är en annan.
ErrorCode last_stream(const DbCacheEntry& entry, size_t file_size, StreamSpan& out);

// Prefixet i ett paket av formen (4, b): de fyra första byten som int32.
// false om paketet inte har den formen eller pekar utanför filen.
bool read_prefix(const uint8_t* file, size_t size, const DbCacheEntry& entry,
                 int32_t& prefix);

// Strömmarna FÖRE den sista i ett paket av formen (4, b), i filordning.
// `sizes` är deras komprimerade längder, så som innehållsförteckningen i den
// sista strömmen anger dem. Strömmarna ligger tätt efter prefixet:
//
//   ström i börjar på offset + 4 + summan av längderna före den
//
// CorruptPackage om en längd är 0 eller om 4 + summan + b inte är paketets
// längd. OutOfRange om paketet inte har formen (4, b) eller pekar utanför
// filen.
ErrorCode split_prefixed(const DbCacheEntry& entry, size_t file_size,
                         const std::vector<uint32_t>& sizes,
                         std::vector<StreamSpan>& out, std::string* detail = nullptr);

// Packar upp strömmen i `span`, som ska ta exakt span.length byte.
// OutOfRange om spannet pekar utanför filen, CorruptStream om strömmen är
// trasig eller slutar före eller efter spannets slut, StreamTooLarge om den
// uppackade datan blir större än `max_output`.
ErrorCode inflate_span(const uint8_t* file, size_t size, const StreamSpan& span,
                       size_t max_output, std::vector<uint8_t>& out,
                       std::string* detail = nullptr);

} // namespace HNwd::detail
