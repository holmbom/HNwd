#pragma once
#include "nwd_error.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace HNwd::detail {

// Geometriposterna i paketet LcOpNwdGeometry. `int` är int32 little-endian,
// och varje utfyllnadsbyte och oanvänd packad bit är 0.
//
//   RECORD := [100][cls] V UVSLOT NSLOT CSLOT P SFIELD? pad8        klass 94 och 95
//           | [100][96] V UVSLOT NSLOT CSLOT [0 eller 1] pad8         klass 96
//   V      := [101][60][p1][p2][p2 × f32] VMAP?           npos = p2/3; VMAP om p1 != npos
//   UVSLOT := 0 | [id][61] UVBODY
//   NSLOT  := 0 | [id][100][-p1][pal] PACKED(3·pal, B) NTOK?   NTOK om pal != p1
//   CSLOT  := 0 | [id][58][5634][-p1][pal][pal × 4 byte] CTOK?  CTOK om pal != p1
//   P      := [n][bits][n][ceil(n·bits/32) ord]            strip- eller polylinjelängder
//   SFIELD := [S] TOKENS(S, p1)?                           bara klass 94; tokens om S > 0
//   UVBODY := [p1][fc][fc × f32] TOKENS(p1, fc/2)?         tokens om fc/2 != p1
//           | [-p1][4 × f32][w][m][ceil(m·(bu+bv)/32) ord] TOKENS(p1, m)?   tokens om m != p1
//
// cls 94 är trianglar (alltid normaler; färgen får saknas), 95 linjer och 96
// punkter (aldrig UV eller normaler; färgen får saknas). En punktpost har
// inga längder: varje render-vertex är en punkt. p1 är antalet render-vertexar och p2/3 antalet lagrade
// positioner; VMAP, NTOK och CTOK är TOKENS som mappar varje render-vertex på
// en position eller en palettpost. Platsernas id räknas från 102 i den
// ordning platserna finns. bu = w & 0xFF och bv = (w >> 8) & 0xFF; en bredd
// på 0 är en koordinat som är densamma i alla par.
//
// UV-värdena: den råa formen har fc/2 par (u, v) av f32, och den andra formen
// m par om bu + bv bitar, u först, var och en med mest signifikanta biten
// först som i PACKED. TOKENS mappar varje render-vertex på ett par. De fyra
// f32 i den kvantiserade formen tolkas som (umin, vmin, umax, vmax) och ett
// par som u = umin + qu / (2^bu − 1) · (umax − umin), v likadant; en bredd på
// 0 ger umin eller vmin. Den tolkningen är inte prövad mot någon fil.

// Tre klasser är former i stället för listor av positioner. Varje vektor är
// tre f32:
//
//   RECORD := [100][104][3][o][a][b][f32 r][0]          cirkel, 56 byte
//           | [100][105][0x30003][o][p][a][b][f32 r]    cylindermantel, 64 byte
//           | [100][103][sträng][f32 1][f32 1][f32 v][n][min][max][int typsnitt] pad8   text
//
// o är nollvektorn. a och b är vinkelräta enhetsvektorer som spänner cirkelns
// plan, och r är radien: cirkeln är r·(cos v · a + sin v · b) kring origo.
// Cylinderns axel p går från bottencirkelns mitt till toppcirkelns och är
// parallell med a × b; manteln har inga ändytor. Formen delas i kShapeSides
// lika steg med början i riktningen a: en cirkel blir så många segment, en
// mantel dubbelt så många trianglar.
inline constexpr uint32_t kShapeSides = 32;

// TOKENS(count, fresh): en bytekod som ger `count` index. Tillståndet är
// out = [] och next = 0.
//
//   00–3F       lägg till next .. next+b, sedan next += b+1
//   40–7F       kopiera out[n-d], d = b-0x40
//   80–BF       upprepa förra värdet b-0x7F gånger
//   D0–D5       bara med `axis`: axelkod, som ger -1 .. -6
//   E0–EF xx    kopiera out[n-d], d = ((b & 15) << 8) | xx
//   F0 hh ll    literal (hh << 8) | ll, som ska vara mindre än next
//
// Vid slutet ska next == fresh. `pos` flyttas förbi tokens men INTE förbi
// utfyllnaden upp till 4 byte; den tar anroparen. `out` ersätts.
// CorruptGeometry vid varje brott.
ErrorCode decode_tokens(const uint8_t* data, size_t size, size_t& pos, uint32_t count,
                        uint32_t fresh, bool axis, std::vector<int32_t>& out,
                        std::string* detail = nullptr);

// PACKED(count, width): `count` värden om `width` bitar (1–32), packade i
// little-endian-ord om 32 bitar. Bit j i följden är bit 31 - j%32 i ord j/32,
// och varje värde står med sin mest signifikanta bit först. Bitar som blir
// över i sista ordet ska vara 0. `pos` flyttas förbi orden. `out` ersätts.
ErrorCode decode_packed(const uint8_t* data, size_t size, size_t& pos, uint32_t count,
                        uint32_t width, std::vector<uint32_t>& out,
                        std::string* detail = nullptr);

// En palettnormal: tre tvåkomplementsvärden om `bits` bitar, delade med
// 2^(bits-1) - 1 och normerade. Nollvektorn förblir noll.
void palette_normal(uint32_t x, uint32_t y, uint32_t z, int bits, float out[3]);

// En avkodad post, i postens egna koordinater.
struct RecordData {
    bool lines = false;               // klass 95
    bool points = false;              // klass 96; varken lines eller points är trianglar
    std::vector<float> positions;     // xyz per render-vertex, VMAP redan tillämpad
    std::vector<float> normals;       // xyz per render-vertex; tom för linjer
    std::vector<float> uvs;           // uv per render-vertex; tom om posten saknar UV
    std::vector<uint32_t> colors;     // 0xRRGGBBAA per render-vertex; tom om posten saknar färg
    std::vector<uint32_t> indices;    // tre per triangel, moturs; två per segment; tom för punkter
    float bbox[6] = {};               // min xyz, max xyz över de LAGRADE positionerna
    bool box_exact = true;            // false för en form: lådan är uträknad ur a, b, r och p
    // En text (klass 103) har inga positioner och inga primitiver: det som
    // lämnas ut är texten, dess låda och hur den ligger.
    bool text = false;
    std::string text_utf8;            // byten som de står i posten
    float text_angle = 0;             // vridningen i radianer kring text_axis
    float text_axis[3] = {0, 0, 1};   // enhetsvektor
    int32_t text_font = 0;            // index för typsnittet; att det pekar in i filens typsnittslista är inte belagt
    uint32_t primitives = 0;          // antal trianglar, segment eller punkter
    uint32_t normal_palette = 0;      // palettens storlek; 0 betyder att bredden inte spelar roll
};

// Tolkar en post på exakt `size` byte. `normal_bits` är palettnormalernas
// bredd, 8 eller 16: den står inte i posten.
//
// Trianglar: hörnen är 0 .. p1-1 om S = 0, annars TOKENS(S, p1). De delas
// efter längderna i P i strips; i varje strip är triangel i
// (c[i], c[i+1], c[i+2]) för jämnt i och (c[i+1], c[i], c[i+2]) för udda i.
// Linjer: 0 .. p1-1 delas efter längderna i öppna polylinjer.
// En axelkod i normalernas tokens ger en axelvektor: -1 +X, -2 -X, -3 +Y,
// -4 -Y, -5 +Z, -6 -Z.
//
// En cirkel ger linjer och en cylindermantel trianglar med normaler rakt ut
// från axeln. Deras låda är min(0, p) - e .. max(0, p) + e per axel, med
// e = r·(|a| + |b|): lådan kring kvadraten som omsluter cirkeln.
//
// En text ger ingen geometri: strängen står som [int längd][byte][utfyllnad
// till 4], v är en vinkel i radianer och n en enhetsvektor, min och max är
// textens låda, och typsnittet är ett index från 0. Glyferna står inte i
// posten, så `primitives` är 0 och fragmentets antal går inte att pröva.
//
// Unsupported: en text vars två första tal inte är 1, en form vars o inte är noll, kvantiserade
// positioner (negativt p1), eller en kvantiserad UV-bredd bu eller bv över 32.
// CorruptGeometry: varje annat brott mot grammatiken, ett UV-värde eller
// UV-intervall som inte är ändligt, och en post som inte slutar exakt på
// `size`. `detail` anger byten räknad från postens början.
ErrorCode parse_record(const uint8_t* record, size_t size, int normal_bits,
                       RecordData& out, std::string* detail = nullptr);

// Geometrins innehållsförteckning, den sista strömmen i paketet:
//
//   [int32 N][int32 ch][ch × int32 komprimerad chunkstorlek][N × int32 postlängd] pad8
struct GeometryToc {
    std::vector<uint32_t> chunk_sizes;      // komprimerad längd per chunk
    std::vector<uint32_t> record_lengths;   // uppackad längd per post
};

// CorruptGeometry om strömmen inte är exakt roundup8(8 + 4·ch + 4·N) byte, om
// utfyllnaden inte är noll, om ett antal är negativt, om en chunkstorlek är 0
// eller negativ, eller om en postlängd inte är ett positivt tal delbart med 8.
ErrorCode parse_geometry_toc(const uint8_t* data, size_t size, GeometryToc& out,
                             std::string* detail = nullptr);

} // namespace HNwd::detail
