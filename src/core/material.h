#pragma once
#include "nwd_error.h"
#include "object_reader.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace HNwd::detail {

// Klass 185: ett material som JSON, med de filer det använder. Kroppen är
//
//   185  := [str json][str][i32 n] n × FIL
//   FIL  := [str namn][i32 inbäddad 0|1][str slag] ( [str typ][i64 längd][längd byte, nollor upp till 4] )?
//
// där parentesen bara står om inbäddad = 1. Den andra strängen efter json
// läses men sparas inte. För en inbäddad fil är `slag` "navis", `typ` är
// filändelsen ("jpg", "png") och byten är filen som den är.
//
// `json` är ett träd av tillgångar:
//
//   {"version":2,"userassets":["0"],"materials":{"0":{…},"1":{…}}}
//
// En tillgång är {"definition":…, …, "properties":{…}}, och sektionerna nedan
// ("colors", "uris", "scalars", "booleans") ligger i "properties" — en
// sektion direkt i tillgången läses inte.
//
// Grundtillgången är den första i "userassets" vars "definition" är
// "Generic". Ur dess "properties" hämtas
//
//   "colors":{"generic_diffuse":{"IsConnectionEnabled":true,"connections":["1"],
//                                "values":[{"r":…,"g":…,"b":…,"a":…}]}}
//
// där "connections" pekar ut tillgångar i "materials". Är "IsConnectionEnabled"
// false följs de inte, och ingen bild är inkopplad. Annars är den första av dem
// vars "definition" är "UnifiedBitmap" bilden, och ur dess "properties" hämtas
//
//   "uris":{"unifiedbitmap_Bitmap":{"values":["<filnamn>"]}}
//   "scalars":{"texture_UScale", "texture_VScale", "texture_UOffset", "texture_VOffset",
//              "texture_WAngle", "texture_RealWorldScaleX", "texture_RealWorldScaleY"}
//   "booleans":{"texture_URepeat", "texture_VRepeat"}
//
// Ett skalärt eller sant/falskt värde står antingen direkt eller som
// {"values":[x]} med det första elementet som värdet; skalorna får också ha
// "units" (en text). Filnamnet paras ihop med en FIL med exakt samma namn.
//
// Ett material utan grundtillgång, utan färg eller utan inkopplad bild är
// inget fel: fälten behåller sina förval. Okända nycklar hoppas över. Fel är
// JSON som inte går att tolka, en känd nyckel med fel slags värde (också
// "properties" som inte är ett objekt), och en
// pekare till en tillgång som inte finns.

// Inget material eller ingen fil.
inline constexpr uint32_t kNoMaterial = UINT32_MAX;

// Det JSON-texten säger om färgen och bilden.
struct MaterialTexture {
    bool has_color = false;
    float color[4] = {1, 1, 1, 1};   // generic_diffuse: r, g, b, a ("a" saknas: 1)
    std::string bitmap;              // bildens filnamn; tomt om ingen bild är inkopplad
    uint32_t file = kNoMaterial;     // index i MaterialData::files med samma namn; kNoMaterial om ingen
    double u_scale = 1, v_scale = 1;
    double u_offset = 0, v_offset = 0;
    double w_angle = 0;
    double real_scale_x = 0, real_scale_y = 0;   // 0 om de inte står
    std::string real_units_x, real_units_y;      // "units" för skalorna; tomt om de inte står
    bool u_repeat = true, v_repeat = true;
};

struct MaterialFile {
    std::string name;
    std::string kind;    // strängen efter flaggan
    std::string type;    // filändelsen; tom om filen inte är inbäddad
    bool embedded = false;
    std::vector<uint8_t> bytes;   // filen; tom om den inte är inbäddad
};

struct MaterialData {
    std::string json;
    std::vector<MaterialFile> files;
    MaterialTexture texture;
};

// Taket för en JSON-text: `max_json_bytes` tecken, och högst
// max_json_bytes / kMaterialJsonBytesPerValue värden (objekt, listor,
// strängar, tal och konstanter). Ett värde kostar omkring 130 byte i det
// tolkade trädet, så trädet blir högst omkring åtta gånger texten.
// Förvalet är samma som Options::max_material_json_bytes.
inline constexpr size_t kDefaultMaxMaterialJsonBytes = size_t(4) << 20;
inline constexpr size_t kMaterialJsonBytesPerValue = 16;

// Läser kroppen i ett klass-185-objekt (efter pekaren) och tolkar dess JSON.
// `out` ersätts. Ett fel i JSON-texten ger ObjectReader-felet med regeln
// "class-185 json at character <n>: <regel>": CorruptObject, eller
// LimitExceeded när texten eller antalet värden går över taket
// `max_json_bytes`; regeln namnger då Options::max_material_json_bytes. En
// inbäddad fils byte läses i steg, så minnet följer det som finns i strömmen
// och inte längden ur filen.
bool read_class185(ObjectReader& r, MaterialData& out, size_t max_json_bytes);

// Tolkar materialets JSON-text och parar ihop bilden med `files`. `out`
// ersätts. Vid fel är `at` tecknet i texten (för en pekare: där den står)
// och `rule` en kort engelsk text; `limit` (om given) är sann när felet är
// att texten är längre än `max_json_bytes` eller har fler värden än taket
// ovan.
bool parse_material_json(const std::string& json, const std::vector<MaterialFile>& files,
                         MaterialTexture& out, size_t& at, std::string& rule,
                         size_t max_json_bytes, bool* limit = nullptr);

} // namespace HNwd::detail
