#pragma once
#include "guid_store.h"
#include "material.h"
#include "nwd_error.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace HNwd::detail {

// De små paketen: inställningar för visningen, filens identitet och
// inläsarens logg. Varje paket är en enda zlib-ström (formen (0, 0)), och
// tolkarna nedan tar den uppackade strömmen.
//
// Gemensamt för alla: strängar, PAD8 och 64-bitarsvärden är som i
// objektströmmarna (format.md), och varje paket slutar med nollor upp till en
// multipel av 8 och sedan inget mer. `out` ersätts. `detail` får, om den ges,
// en rad "<paket> at byte <n>: <regel>".
//
// Felkoder:
//   CorruptPackage  brott mot grammatiken: strömmen tar slut, en längd eller
//                   utfyllnad stämmer inte, något följer efter slutet.
//                   (CorruptObject i de paket som är objektströmmar:
//                   LcOpLightsElement, LcOpNwdSharedNodes,
//                   LcOpSavedViewsElement och LcReMaterialElement.)
//   Unsupported     ett tal som väljer form har ett värde som inte är avkodat,
//                   eller ett paket som är tomt i alla exempel har innehåll.
//   LimitExceeded   mappar i LcOpSavedViewsElement djupare än kMaxNesting,
//                   eller ett materials JSON över taket `max_json_bytes`
//                   (material.h).
//
// Ett antal ur strömmen prövas mot det som är kvar av den, och listorna
// växer med posterna som läses, aldrig efter antalet. LcOpShadOverridesElement
// och LcOpTextureSpaceElement är undantagen: där tar en post lika många byte
// i minnet som i strömmen, så listan får sin storlek direkt.
//
// Vad fälten betyder står bara där det är belagt; "okänt" annars.
//
// Flera paket har poster med samma huvud:
//
//   [i32 slag] [str namn] [i32 0] [16 byte GUID] [i32 0]
//
// där slaget säger vad som följer: 0 en sparad vy och 4 en mapp
// (LcOpSavedViewsElement), 80 ett material (LcReMaterialElement), 83 ett ljus
// (LcOpLightsElement), 50 ett test och 53, 54, 55 dess rötter
// (LcOpClashElement).

// ---------------------------------------------------------------------------
// [i32 0][i32 0]: LcOpCommonSchemas, LcOpZoneElement,
// LcOpSelectionSetsElement, LcOpPropertyOverrideElement och
// LcOdpDBDatabaseLinksElement i varje fil som har dem. Formen när paketet inte
// är tomt är inte känd: Unsupported om något av talen inte är 0. `what` är
// paketets namn och inleder `detail`.
ErrorCode parse_empty_pair(const uint8_t* data, size_t size, const char* what,
                           std::string* detail = nullptr);

// LcOpToolElement: [i32 tool] nollor upp till 8. Okänt.
ErrorCode parse_tool(const uint8_t* data, size_t size, int32_t& out,
                     std::string* detail = nullptr);

// LcOpNextCommentIdElement: [i32 a] [i32 0] [i32 b] [i32 0]. Okänt; att talen
// är två 64-bitarsvärden går inte att utesluta, och nollorna ska vara 0
// (Unsupported annars).
struct NextCommentId {
    int32_t values[2] = {};
};
ErrorCode parse_next_comment_id(const uint8_t* data, size_t size, NextCommentId& out,
                                std::string* detail = nullptr);

// ---------------------------------------------------------------------------
// LcOpNwdStats: [str text] nollor upp till 8.
//
// Texten är inläsarens logg. I en `.nwd` är den loggarna för de inlästa
// modellerna efter varandra, var och en inledd av "\n" <modellens namn> "\n\n".
// `non_utf8` blir sann om texten inte är giltig UTF-8 (inget fel).
ErrorCode parse_stats(const uint8_t* data, size_t size, std::string& text, bool& non_utf8,
                      std::string* detail = nullptr);

// ---------------------------------------------------------------------------
// LcOpOdyScenePropsChunk:
//
//   [str sökväg] [16 byte GUID] [i32 0] [i32 0] nollor upp till 8
//
// `path` är sökvägen till filen själv när den sparades: sista ledet är filens
// eget namn, och strängen är samma som "SavedFilename" i LcOpXRefTable. I en
// `.nwc` är `guid` samma som FileInfo::scene_guid. De två nollorna ska vara 0
// (Unsupported annars).
struct SceneProps {
    std::string path;
    bool path_null = false;
    Guid16 guid{};
};
ErrorCode parse_scene_props(const uint8_t* data, size_t size, SceneProps& out,
                            std::string* detail = nullptr);

// ---------------------------------------------------------------------------
// LcOpOdyFileInfoChunk:
//
//   [str null] [i32 0] [GUID file_guid] [i32 0] [str tag] [str name]
//   [i32 0] [GUID scene_guid] [i32 0] [str prefix]
//   [i32 0] [i32 0] [i32 -1] [i32 -1] [str prefix_third]
//   [GUID scene_guid_again] [str prefix_again]
//   [i32 0] [GUID guid_c] [GUID guid_d]
//   nollor upp till 8
//
// Markörerna (null, 0 och -1) ska ha just de värdena, annars Unsupported.
// `prefix_third` är null i de tio exemplen och lika med `prefix` i
// formatversion 451.
struct FileInfo {
    Guid16 file_guid{};          // okänt; i en `.nwc` samma som guid_c
    std::string tag;             // "G" i alla exempel; okänt
    std::string name;            // `.nwc`: filens eget namn; `.nwd`: = prefix
    Guid16 scene_guid{};         // `.nwc`: = SceneProps::guid; `.nwd`: inte
    std::string prefix;          // prefixet på paketnamnen i katalogen ("cache" i `.nwc`)
    std::string prefix_third;    // null i exemplen, = prefix i formatversion 451
    bool prefix_third_null = false;
    Guid16 scene_guid_again{};   // = scene_guid i alla exempel
    std::string prefix_again;    // = prefix i alla exempel
    Guid16 guid_c{};             // okänt
    Guid16 guid_d{};             // okänt; noll-GUID i `.nwc`
};
ErrorCode parse_file_info(const uint8_t* data, size_t size, FileInfo& out,
                          std::string* detail = nullptr);

// ---------------------------------------------------------------------------
// LcOpXRefTable: [str json] nollor upp till 8, där json är
//
//   {"Type":"XRefTable","Version":1,"SavedFilename":"<sökväg>","Table":[]}
//
// Nycklarna får stå i vilken ordning som helst men ska alla finnas.
// Unsupported: en annan "Type" eller "Version", en okänd nyckel, eller en
// "Table" som inte är tom (posternas form är inte känd). CorruptPackage: JSON
// som inte går att tolka, eller en nyckel med fel slags värde.
struct XRefTable {
    std::string json;             // texten som den står
    int32_t version = 0;          // 1
    std::string saved_filename;   // sökvägen, med JSON-escape upplösta
};
ErrorCode parse_xref_table(const uint8_t* data, size_t size, XRefTable& out,
                           std::string* detail = nullptr);

// ---------------------------------------------------------------------------
// LcOpCurrentViewElement och LcOpHomeViewElement: en kamera. Samma vy står i
// varje sparad vy i LcOpSavedViewsElement.
//
//   [i32 form] [i32 projection]
//   [3 × f64 position] [4 × f64 rotation] [f64 u] [f64 angle]
//   [f64 near] [f64 far] [2 × f64 zero]
//   form 843, 895, 1919:
//             [4 × f64 avatar_values] [str avatar] [4 × f64 after_avatar]
//             [i32 avatar_flag] PAD8 [4 × f64 more]
//   form 9, 843:  [f64 distance]
//   form 895, 1919: [6 × f64 extra]
//   form 9:   [2 × i32 tail_ints]
//   form 843, 895: [6 × i32 tail_ints]
//   form 1919: [4 × i32 tail_ints] [f64 ortho_extra] [2 × i32 tail_ints]
//   SNITT
//
//   SNITT := [6 × i32 head] [3 × f64 head_values] [i32 6] 6 × PLAN
//            [2 × i32 flags_a] [6 × f64 values_a] [2 × i32 flags_b] [6 × f64 values_b]
//            [4 × f64 rotation]                                    680 byte
//   PLAN  := [i32 enabled] [i32 mode] [3 × f64 point] [3 × f64 normal] [3 × f64 tangent]
//
// SNITT börjar alltid på jämn 8 byte, eftersom det som står före slutar så;
// det första planet har därför PAD8 efter `mode` och är 84 byte, de andra 80.
// Talet 6 och `enabled` (0 eller 1) ger Unsupported för ett annat värde.
//
// `form` är 9 i varje `.nwc` och i LcOpHomeViewElement, 843 i exemplet
// `.nwd`, och 895 eller 1919 i formatversion 451; ett annat värde ger
// Unsupported. Bitvis innehåller varje form den förra (9 ⊂ 843 ⊂ 895 ⊂ 1919),
// men vilken bit som lägger till vilket fält går inte att avgöra.
// `projection` är 0 eller 1 (Unsupported annars); 1 bara i form 1919.
//
// Kameran tittar längs R(rotation) · (0, 0, −1), med R som i fragmentens
// poser. Belagt:
// - rotation har längden 1 i varje vy, i exemplen och i formatversion 451;
// - i en `.nwc` är position + distance · blickriktningen mitten av
//   fragmentens världslåda;
// - i exemplet `.nwd` ligger varje vertex i modellen mellan near och far
//   längs blickriktningen;
// - avatar_values är avatarens mått i filens längdenhet: samma fyra mått,
//   omräknade till längdenheten, i exemplet `.nwd` och i formatversion 451.
// I form 1919 är `angle` ett mått i längdenheten i stället för en vinkel, vilket
// tyder på att projection 1 är en ortografisk vy; det är inte prövat.
//
// SNITT är vyns snittplan. Belagt i formatversion 451, i varje vy: `normal`
// och `tangent` har längden 1 och är vinkelräta; i två påslagna plan med
// motsatta normaler är skillnaden mellan punkterna parallell med normalen, så
// `point` ligger i planet och `normal` är dess normal. Ett påslaget plan har
// `mode` 10; ett avslaget har ett eget värde, 6, 7, 3, 2, 4 och 5 i ordning,
// i exemplen och i formatversion 451. I ett påslaget plan är `normal` och
// `tangent` ±R(rotation)·x eller ±R(rotation)·y, och flags_a är (1, 1) och
// flags_b (0, 1) precis när något plan är påslaget, annars (0, 0) och
// (0, 0). Lika i alla vyer: head (2, 0, 0, 0, 2, 2), head_values (1, 0, 0),
// values_a och values_b (1, 1, 1, 0, 0, 0). I de tio exemplen
// är SNITT detsamma: inga plan påslagna, alla med normalen (0, 1, 0) och
// tangenten (0, 0, 1), och `rotation` identiteten. Okänt: `mode`, flaggorna,
// head och values.
struct ClipPlane {
    int32_t enabled = 0;      // 0 eller 1
    int32_t mode = 0;         // okänt: 10 när planet är påslaget
    double point[3] = {};     // en punkt i planet, i filens koordinater
    double normal[3] = {};
    double tangent[3] = {};   // vinkelrät mot normalen; okänt vad den styr
};
struct ViewClip {
    int32_t head[6] = {};          // okänt
    double head_values[3] = {};    // okänt
    ClipPlane planes[6];
    int32_t flags_a[2] = {};       // okänt
    double values_a[6] = {};       // okänt
    int32_t flags_b[2] = {};       // okänt
    double values_b[6] = {};       // okänt
    double rotation[4] = {0, 0, 0, 1};   // en kvaternion (x, y, z, w) för planens riktningar
};
struct ViewElement {
    int32_t form = 0;
    int32_t projection = 0;              // 0 eller 1; se ovan
    double position[3] = {};
    double rotation[4] = {0, 0, 0, 1};   // x, y, z, w
    double u = 0;                        // okänt: 1 i `.nwc`, 1,4496 i `.nwd`
    double angle = 0;                    // okänt: 0,785398 i alla tio
    double near_depth = 0, far_depth = 0;   // 1 och 10 i `.nwc`
    double zero[2] = {};                 // okänt: 0 i exemplen och i formatversion 451
    // Form 843, 895 och 1919.
    double avatar_values[4] = {};
    std::string avatar;                  // namnet på en avatar
    double after_avatar[4] = {};
    int32_t avatar_flag = 0;
    double more[4] = {};
    // Form 9 och 843.
    double distance = 0;
    // Form 895 och 1919: okänt. extra[0..2] är (0, 0, 1) och extra[5] är
    // π/4 med tio decimaler i varje vy i formatversion 451; extra[3] och
    // extra[4] varierar.
    double extra[6] = {};
    double ortho_extra = 0;              // bara form 1919; okänt
    std::vector<int32_t> tail_ints;      // okänt: 2 tal i form 9, annars 6
    ViewClip clip;
    std::vector<uint8_t> rest;           // SNITT som byten står, 680 byte
};
// Byten i SNITT, efter tail_ints.
inline constexpr size_t kViewRestBytes = 680;
// `what` är paketets namn och inleder `detail`.
ErrorCode parse_view_element(const uint8_t* data, size_t size, ViewElement& out,
                             std::string* detail = nullptr,
                             const char* what = "LcOpCurrentViewElement");

// ---------------------------------------------------------------------------
// LcOpSavedViewsElement: sparade vyer i mappar. En objektström (id från 100).
//
//   VYER    := [i32 n] n × POST, nollor upp till 8
//   POST    := [i32 slag] [str namn] [i32 0] [GUID guid] [i32 0]
//              slag 0: VY [i32 r] r × MARKERING [i32 m] m × PTR(89)
//              slag 4: [i32 c] c × POST
//   VY      := som i LcOpCurrentViewElement, från form
//   MARKERING := [i32 typ]
//              typ 1: [4 × f64 punkter]          en linje (x0, y0, x1, y1)
//              typ 4: [str text] [2 × f64 punkter]  en text (x, y)
//              [i32 width] PAD8 [3 × f64 colour] [i32 pattern]
//   89      := [str element] KROPP efter element:
//              "LcOpShadOverridesElement":    ([i32 1] [i32 index])* [i32 0]
//              "LcOpShadFragMaterialElement": [i32 0] [i32 0]
//
// Slag 0 är en vy och slag 4 en mapp med c poster; ett annat slag, en annan
// markeringstyp, ett annat element eller ett FragMaterial som inte är tomt ger
// Unsupported. Ett 89-objekt delas mellan vyer genom bakåtpekare.
//
// Belagt i formatversion 451: grammatiken tolkar paketet till exakt slut;
// varje vy har form 895 eller 1919 och en kvaternion med längden 1; varje
// markerings punkter ligger i [−1, 1], och färgen är tre tal i [0, 1].
// Indexen i ett ShadOverrides-objekt är samma mängd som i paketet
// LcOpShadOverridesElement, men stigande. Okänt: vad indexen pekar ut,
// `width` och `pattern`.
struct Redline {
    int32_t type = 0;              // 1 linje, 4 text
    std::string text;              // bara typ 4
    double points[4] = {};         // typ 1: x0, y0, x1, y1; typ 4: x, y och två nollor
    int32_t width = 0;             // okänt
    double colour[3] = {};
    int32_t pattern = 0;           // okänt
};
struct ViewState {
    std::string element;           // "LcOpShadOverridesElement" eller "LcOpShadFragMaterialElement"
    std::vector<int32_t> indices;  // bara ShadOverrides
};
//
// En post bär bara sitt huvud; en vy står i `views` och pekar ut sina
// markeringar och klass-89-platser som följder i de gemensamma listorna. En
// mapp tar därför inte mer minne än sitt namn och huvudet. Listorna växer med
// det som läses, aldrig efter ett antal ur filen. `rest` (SNITT som byten
// står) sparas inte i en sparad vy; fälten i `clip` har samma innehåll.
struct SavedViews {
    struct Item {
        int32_t kind = 0;               // 0 vy, 4 mapp
        uint32_t parent = UINT32_MAX;   // index i `items`; UINT32_MAX på översta nivån
        std::string name;
        Guid16 guid{};
        uint32_t child_count = 0;       // bara en mapp
        uint32_t view = UINT32_MAX;     // index i `views`; bara en vy
    };
    struct View {
        ViewElement view;               // utan `rest`
        uint32_t redline_begin = 0;     // i `redlines`
        uint32_t redline_count = 0;
        uint32_t state_begin = 0;       // i `state_refs`
        uint32_t state_count = 0;
    };
    std::vector<Item> items;            // i läsordning: en mapp före sina poster
    std::vector<View> views;            // vyerna, i läsordning
    std::vector<Redline> redlines;      // vyernas markeringar, vy för vy
    std::vector<uint32_t> state_refs;   // index i `states`, vy för vy
    std::vector<ViewState> states;      // klass 89, i läsordning
};
ErrorCode parse_saved_views(const uint8_t* data, size_t size, SavedViews& out,
                            std::string* detail = nullptr);

// ---------------------------------------------------------------------------
// LcOpShadOverridesElement: [i32 n] n × ([i32 index] [i32 flags]) nollor upp
// till 8. Indexen är samma mängd som i varje ShadOverrides-objekt i de sparade
// vyerna i formatversion 451; vad de pekar ut, och `flags`, är okänt.
struct ShadOverride {
    int32_t index = 0;
    int32_t flags = 0;
};
ErrorCode parse_shad_overrides(const uint8_t* data, size_t size,
                               std::vector<ShadOverride>& out, std::string* detail = nullptr);

// LcOpTextureSpaceElement: [i32 0] [i32 n] n × ([i32 index] [i32 kind]
// [16 × f64 values]) nollor upp till 8. Nollan först ska vara 0
// (Unsupported annars). Okänt: vad indexen pekar ut, `kind` och talen.
struct TextureSpace {
    int32_t index = 0;
    int32_t kind = 0;
    double values[16] = {};
};
ErrorCode parse_texture_space(const uint8_t* data, size_t size,
                              std::vector<TextureSpace>& out, std::string* detail = nullptr);

// ---------------------------------------------------------------------------
// LcOpNwdSharedNodes: [i32 n] n × PTR(102), nollor upp till 8. En
// objektström (id från 100); en post får vara en bakåtpekare.
//
//   102 := [str name] PAD8 [f64 size] [4 × i32 values]
//
// `name` är namnet på ett typsnitt. Okänt: `size` (i vilken enhet) och de fyra
// talen. Tomt paket: [i32 0] och nollor.
struct FontEntry {
    std::string name;
    double size = 0;
    int32_t values[4] = {};
};
struct SharedNodes {
    std::vector<uint32_t> entries;   // index i `fonts`
    std::vector<FontEntry> fonts;    // klass 102, i läsordning
};
ErrorCode parse_shared_nodes(const uint8_t* data, size_t size, SharedNodes& out,
                             std::string* detail = nullptr);

// ---------------------------------------------------------------------------
// LcOpGridElement: rutnät.
//
//   NÄT    := [i32 n] n × SYSTEM [i32 tail_a] [i32 tail_b] nollor upp till 8
//   SYSTEM := [str name] PAD8 [3 × f64 origin] [3 × (3 × f64) axes]
//             [i32 nl] nl × LINJE [i32 nv] nv × NIVÅ [i32 level]
//   LINJE  := [str label] PAD8 [2 × f64 values] [3 × i32 ints] PAD8 [4 × f64 ends]
//   NIVÅ   := [str name] PAD8 [f64 elevation]
//
// Belagt i formatversion 451: grammatiken tolkar paketet till exakt slut;
// axes[0] och axes[1] har längden 1 och är vinkelräta, och axes[2] är lika
// med axes[1] i varje system; varje linjes `ends` är två punkter
// (x0, y0, x1, y1) med x0 = x1 eller y0 = y1; nivåernas `elevation` är
// stigande inom ett system och i filens längdenhet; `level` ligger i
// [−1, nv) och är −1 precis när nv är 0. Ett `level` utanför ger
// Unsupported. Okänt: values, ints, tail_a och tail_b.
struct GridLine {
    std::string label;
    double values[2] = {};
    int32_t ints[3] = {};
    double ends[4] = {};
};
struct GridLevel {
    std::string name;
    double elevation = 0;
};
struct GridSystem {
    std::string name;
    double origin[3] = {};
    double axes[3][3] = {};
    std::vector<GridLine> lines;
    std::vector<GridLevel> levels;
    int32_t level = -1;   // index i `levels`, −1 utan nivåer
};
struct GridElement {
    std::vector<GridSystem> systems;
    int32_t tail[2] = {};   // okänt
};
ErrorCode parse_grid(const uint8_t* data, size_t size, GridElement& out,
                     std::string* detail = nullptr);

// ---------------------------------------------------------------------------
// LcReMaterialElement: material. En objektström (id från 100).
//
//   [i32 n] n × ( [i32 80] [str name] [i32 0] [GUID guid] [i32 0] PTR(185 eller null) )
//   nollor upp till 8
//
// Klass 185 är ett material som JSON med inbäddade filer (material.h). Ett
// annat slag än 80 ger Unsupported. Belagt i formatversion 451: namnet står i
// materialets JSON, och GUID:en är av version 4. Att GUID:en är grundtillgångens
// "VersionGUID" stämmer för en del av materialen men inte för alla.
struct ReMaterials {
    struct Entry {
        std::string name;
        Guid16 guid{};
        uint32_t material = kNoMaterial;   // index i `materials`
    };
    std::vector<Entry> entries;
    std::vector<MaterialData> materials;   // klass 185, i läsordning
};
ErrorCode parse_re_materials(const uint8_t* data, size_t size, ReMaterials& out,
                             std::string* detail = nullptr,
                             size_t max_json_bytes = kDefaultMaxMaterialJsonBytes);

// ---------------------------------------------------------------------------
// LcOpClashElement: kollisionstester. Inga pekare; strängar, PAD8 och
// 64-bitarsvärden som i objektströmmarna.
//
//   KOLL   := [i32 n] n × ROT [i32 nr] nr × REGEL [i32 nm] nm × MODELL
//             nollor upp till 8
//   ROT    := [i32 slag] [str namn] [i32 0] [GUID guid] [i32 0]
//             slag 53: [i32 c] c × TEST
//             slag 54, 55: [i32 0]
//   TEST   := [i32 50] [str namn] [i32 0] [GUID guid] [i32 0]
//             [i32 0] [i32 -1] [i32 0] [i32 0] [i32 -1] [i32 0] [i32 0] [i32 1] [i32 1]
//             [2 × f64 values] [3 × f64 zero] [i32 1] [i32 0]
//   REGEL  := [str namn] [i32 1]
//   MODELL := [str fil] [str sökväg] [str prefix] [GUID source_guid] [str namn]
//             [str loader] [i32 0] [i32 mode] [9 × f64 matrix] [3 × f64 offset]
//             [i32 unit_code] [i32 1] [f64 0] [3 × f64 up] [3 × f64 north]
//             [i32 0] [i32 -1] [i32 -1] [i32 -1] [i32 0] [i32 0] [i32 1]
//             [str fil] [str sökväg] [i64 time] [i64 size] [f64 0] [3 × f64 c]
//             [GUID guid]
//
// Slag 53 är testernas rot, med testerna (slag 50) som poster; 54 och 55 har
// inga poster. Varje tal som står som ett fast värde ovan ska ha just det
// värdet, och ett annat slag på en plats ger Unsupported.
//
// Belagt i formatversion 451: grammatiken tolkar paketet till exakt slut.
// Reglerna är namn på insticksmoduler. En modell är en annan fil:
// `file` är sista ledet i `path`, och båda står två gånger, lika. `prefix`,
// `source_guid` och `name` är som prefixet, scen-GUID:en och namnet i
// LcOpOdyFileInfoChunk: `prefix` är "cache" i en `.nwc`, och modeller med samma
// `name` har samma `source_guid`. `matrix` (radvis) är en rotation kring z
// gånger skalan metres_per_unit(unit_code) / metres_per_unit(filens egen kod),
// `north` är matrisens andra rad normerad och `up` är (0, 0, 1), båda på 1e-15
// när. `c` är (north[0], north[1]) och ett tal nära 2^−52 i z, och står efter
// en nolla. `up`, `north` och `c` har samma form som a, b och c i en
// partitionssvans. `time` och `size` står där LcOpDBCache har källfilens tid
// och storlek; att de är det är inte prövat. Okänt: testernas `values` och
// nollor, `mode`, de fasta talen, och vad `guid` identifierar.
struct ClashTest {
    std::string name;
    Guid16 guid{};
    double values[2] = {};   // okänt
    double zero[3] = {};     // okänt; 0 i formatversion 451
};
struct ClashRoot {
    int32_t kind = 0;        // 53, 54 eller 55
    std::string name;
    Guid16 guid{};
    uint32_t test_begin = 0;   // i ClashElement::tests; bara slag 53
    uint32_t test_count = 0;
};
struct ClashModel {
    std::string file;          // filens namn
    std::string path;          // sökvägen till filen
    std::string prefix;        // som prefixet i LcOpOdyFileInfoChunk
    Guid16 source_guid{};
    std::string name;          // som namnet i LcOpOdyFileInfoChunk
    std::string loader;        // inläsarens namn
    int32_t mode = 0;          // okänt
    double matrix[9] = {};     // radvis: rotation gånger enhetskvoten
    double offset[3] = {};     // flytten, i filens egen längdenhet
    int32_t unit_code = 0;     // modellens längdenhet, som i en partitionssvans
    double up[3] = {};         // (0, 0, 1) på 1e-15 när
    double north[3] = {};      // matrisens andra rad, normerad
    std::string file_again;    // = file
    std::string path_again;    // = path
    int64_t time = 0;          // okänt; står som källfilens tid i LcOpDBCache
    int64_t size = 0;          // okänt; står som källfilens storlek i LcOpDBCache
    double c[3] = {};          // north[0], north[1] och ett tal nära 2^−52
    Guid16 guid{};             // okänt
};
struct ClashElement {
    std::vector<ClashRoot> roots;
    std::vector<ClashTest> tests;      // testerna under rötterna, i läsordning
    std::vector<std::string> rules;    // insticksmodulernas namn
    std::vector<ClashModel> models;
};
ErrorCode parse_clash(const uint8_t* data, size_t size, ClashElement& out,
                      std::string* detail = nullptr);

// ---------------------------------------------------------------------------
// LcOpHeadlightElement: [f64 a] [f64 b]. Okänt; 0,3 och 1 i exemplet.
struct HeadlightElement {
    double values[2] = {};
};
ErrorCode parse_headlight(const uint8_t* data, size_t size, HeadlightElement& out,
                          std::string* detail = nullptr);

// LcOpSpeedElement: [f64]. Okänt; 1/6 i exemplet.
ErrorCode parse_speed(const uint8_t* data, size_t size, double& out,
                      std::string* detail = nullptr);

// LcOpCullingElement: [f64] [4 × 8 byte] [i32] nollor upp till 8. Okänt; 1,
// fyra nollor och 1 i exemplet. De fyra nollorna läses som f64; om de är
// flyttal eller heltal går inte att avgöra.
struct CullingElement {
    double first = 0;
    double values[4] = {};
    int32_t flag = 0;
};
ErrorCode parse_culling(const uint8_t* data, size_t size, CullingElement& out,
                        std::string* detail = nullptr);

// LcOpBackgroundElement:
//
//   [i32 mode] PAD8 [7 × (3 × f64)] [i32 0] PTR(185 eller null) nollor upp till 8
//
// Pekaren är en objektström för sig (id från 100). Varje trippel är en färg:
// komponenterna ligger i [0, 1] och är hela 255-delar i exemplet `.nwd` och i
// formatversion 451. Vilken färg som används till vad är inte belagt.
// Pekaren är null i exemplet `.nwd`; i formatversion 451 är den ett material
// av klass 185 vars grundtillgång har definitionen "SunAndSky" (en himmel).
// `mode` är okänt; nollan före pekaren ska vara 0 (Unsupported annars).
struct BackgroundElement {
    int32_t mode = 0;
    double colours[7][3] = {};
    bool has_sky = false;
    MaterialData sky;   // klass 185; bara om has_sky
};
ErrorCode parse_background(const uint8_t* data, size_t size, BackgroundElement& out,
                           std::string* detail = nullptr,
                           size_t max_json_bytes = kDefaultMaxMaterialJsonBytes);

// ---------------------------------------------------------------------------
// LcOpLightsElement: ljusen, en objektström (id från 100).
//
//   [i32 n] n × LJUS, nollor upp till 8
//   LJUS := [i32 kind] [str name] [i32 0] [GUID guid] [i32 0] PTR(27) [3 × f64 origin] [3 × f64 direction]
//   27   := [i32 1] PTR(185 eller null) [i32 0] [12 × f64]
//
// Klass 27 är samma som i en partitionssvans (partition.h), och klass 185 ett
// material som JSON (material.h). Belagt i exemplet: `guid` är samma som
// "VersionGUID" i ljusets JSON; de tolv talen är samma som i ljusen i
// dokumentets egen partition; talen 3–5 och 6–8 är lika, och lika med
// "specularColor" i JSON; de tre sista har längden 1; `direction` är samma
// som de tre sista, och `origin` är (0, 0, 0). `kind` är 83 i exemplet; ett annat värde ger
// Unsupported.
struct LightBody {
    int32_t version = 0;            // 1
    uint32_t material = kNoMaterial;   // index i LightsElement::materials
    double values[12] = {};
};
struct LightsElement {
    struct Entry {
        int32_t kind = 0;
        std::string name;
        Guid16 guid{};
        uint32_t light = 0;   // index i `lights`
        double origin[3] = {};
        double direction[3] = {};
    };
    std::vector<Entry> entries;
    std::vector<LightBody> lights;          // klass 27, i läsordning
    std::vector<MaterialData> materials;    // klass 185, i läsordning
};
ErrorCode parse_lights(const uint8_t* data, size_t size, LightsElement& out,
                       std::string* detail = nullptr,
                       size_t max_json_bytes = kDefaultMaxMaterialJsonBytes);

} // namespace HNwd::detail
