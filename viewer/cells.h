#pragma once

// Geometrin för bitarna: fragment som byggs till meshar, detaljnivåerna i en
// mesh och minnet de tar. Ren std och HNwd — inga fönstertyper — så att samma
// kod går att använda utanför visaren.
//
// Koordinaterna räknas i double i världen, skalas till meter och görs
// relativa delens mitt innan de blir float.

#include "lod_tree.h"
#include "model.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace nwdview {

// Var en triangel kommer ifrån: trianglarna från `first_triangle` fram till
// nästa post hör till fragmentet, vars diagonal är `size` meter.
struct FragmentRange {
    uint32_t first_triangle = 0;
    uint32_t fragment = 0;
    float size = 0;
};

// Fragmentet som triangeln kommer ifrån; HNwd::kNone utanför.
uint32_t fragment_of(const std::vector<FragmentRange>& ranges, uint32_t triangle);

// Antalet trianglar i en del vars fragment har diagonalen minst `min_size`.
// Fragmenten ska stå störst först (FragmentRange::size avtar), som de gör i
// en del som build_tile() bygger; svaret är då fragmentens trianglar från
// början, alltså det prefix av delens `triangles` trianglar som består av
// fragment med size >= min_size.
uint32_t triangles_at_least(const std::vector<FragmentRange>& ranges, uint32_t triangles,
                            float min_size);

// En detaljnivå i en mesh: de första `triangles` trianglarna ritas när meshens
// låda ligger högst `max_distance` meter från ögat. 0 = obegränsat, bara sist.
struct LodStep {
    uint32_t triangles = 0;
    double max_distance = 0;
};

// Nivåerna för en del vars fragment står störst först, för en kamera med
// `pixels_per_radian` px per radian (bildhöjden delad med 2 · tan(halva
// synfältet)) och ett fragment som ska synas så länge det är minst
// `min_pixels` px.
//
// Ett fragment med diagonalen s syns minst min_pixels px på avståndet d när
// s · pixels_per_radian / d >= min_pixels, alltså så länge d <=
// s · pixels_per_radian / min_pixels. Nivåerna står i växande avstånd, med
// hela trianglar som avtar. Den första nivån har alla trianglar. För varje
// avstånd d som någon nivå når innehåller den första nivå vars max_distance
// är minst d varje fragment som syns minst min_pixels px på d. Den sista
// nivåns max_distance är avståndet där delens största fragment blir
// min_pixels px; bortom det ritas ingenting. Nivåer som sparar mindre än tio
// procent av trianglarna slås ihop med nivån före. Högst `max_steps` nivåer.
//
// Utan fragment eller trianglar blir svaret tomt. Är min_pixels eller
// pixels_per_radian inte positivt, eller ingen storlek positiv, blir det en
// enda nivå med alla trianglar och obegränsat avstånd.
std::vector<LodStep> lod_steps(const std::vector<FragmentRange>& ranges, uint32_t triangles,
                               double min_pixels, double pixels_per_radian, int max_steps = 4);

// Hur materialens bildkoordinater tolkas. RealWorld delar UV med bildens
// verkliga storlek (Material::real_scale_x/y) när materialet anger den, och
// tar dem som de står annars; Raw tar dem alltid som de står.
enum class UvMode : uint8_t { Raw, RealWorld };

// Linjernas färg: mörkgrå, ogenomskinlig, 0xRRGGBBAA.
inline constexpr uint32_t kLineColor = 0x282828FFu;

// En del av geometrin: trianglar med en gemensam sorts ritning. Trianglarna
// står fragment för fragment i den ordning delen byggdes, med det största
// fragmentet först när den kommer ur build_tile().
//
// Linjedelen (`lines`) har samma fält men en annan tolkning. Ett linjesegment
// från ände A till ände B är fyra hörn och två trianglar, i den här
// ordningen (s är segmentets nummer i delen, hörnen s·4 till s·4 + 3):
//
//   hörn   positions  other  corner (sidan, änden)
//   s·4       A         B     (−1, 0)
//   s·4+1     A         B     (+1, 0)
//   s·4+2     B         A     (−1, 1)
//   s·4+3     B         A     (+1, 1)
//
// och trianglarna (s·4, s·4+1, s·4+2) och (s·4+2, s·4+1, s·4+3). Varje hörn
// ligger alltså på sin egen ände (`positions`), bär den andra änden (`other`)
// och vet vilken sida av linjen och vilken ände det är. Programmet flyttar
// hörnet i bilden vinkelrätt mot linjen, sidan gånger halva bredden i px, så
// att linjen får en fast bredd. Sidan −1 är till vänster om riktningen A → B i
// bilden (y uppåt), +1 till höger, så trianglarna är moturs i bilden; hur
// linjen vänder mot kameran är inte bestämt, och programmet ritar utan
// bortsortering av baksidor. `normals` och `uvs` är tomma, och `colors` är
// kLineColor i alla hörn. FragmentRange::first_triangle är två gånger
// segmentets nummer, så triangles_at_least() och lod_steps() gäller
// linjedelen som de andra delarna.
struct MeshPart {
    std::vector<float> positions;    // xyz per hörn, relativt CellGeometry::origin, meter
    std::vector<float> normals;      // xyz per hörn
    std::vector<float> uvs;          // uv per hörn; bara i en texturerad del
    std::vector<uint32_t> colors;    // 0xRRGGBBAA per hörn, rak alfa
    std::vector<uint32_t> indices;   // tre per triangel, framsidan moturs
    std::vector<FragmentRange> ranges;
    bool lines = false;              // en linjedel: fyra hörn och två trianglar per segment
    std::vector<float> other;        // linjedel: xyz per hörn, segmentets andra ände relativt origin
    std::vector<float> corner;       // linjedel: två tal per hörn — sidan (−1 eller +1) och änden (0 eller 1)
    bool transparent = false;
    uint32_t material = HNwd::kNone;   // texturerad del: materialet med bilden
    float base_color[4] = {1, 1, 1, 1}; // texturerad del: färgen om bilden inte går att visa
    bool repeat_u = true, repeat_v = true;   // texturerad del: bilden upprepas

    size_t triangle_count() const { return indices.size() / 3; }
};

// En text i modellen: var den står, hur hög den är och vad den säger.
struct TextLabel {
    double at[3] = {0, 0, 0};   // textlådans mitt i världen, meter
    float height = 0;           // textens höjd, meter
    std::string text;           // UTF-8
    uint32_t fragment = 0;
};

// De modeller som är dolda, som fragmentintervall [first, end) i växande
// ordning. Ett dolt fragment byggs inte.
struct ModelFilter {
    std::vector<uint32_t> first, end;

    bool hidden(uint32_t fragment) const;
    bool empty() const { return first.empty(); }
};

struct BuildLimits {
    size_t max_segments = 20'000;       // linjesegment per bit; resten räknas; 0 = inga linjer
    size_t max_labels = 64;             // texter per bit som blir etiketter; resten räknas
    // Materialet har en bild som visaren kan rita. Utan funktion ritas inga
    // bilder.
    std::function<bool(uint32_t material)> texture_usable;
    UvMode uv = UvMode::RealWorld;
    std::shared_ptr<const ModelFilter> filter;   // nullptr: inget dolt
    // Fragment med mindre diagonal än så här byggs inte; 0 = alla. I build_tile()
    // är det ett prefix av bitens fragment (de står störst först), och de
    // som faller bort läses inte.
    float min_size = 0;
};

struct CellGeometry {
    double origin[3] = {0, 0, 0};   // delens mitt i världen, meter
    double diagonal = 0;            // delens diagonal, meter
    // [0] ogenomskinligt, [1] genomskinligt, [2] linjer (`lines`), sedan en del
    // per material med bild. Tomma delar finns kvar.
    std::vector<MeshPart> parts;
    std::vector<TextLabel> labels;

    uint64_t triangles = 0;          // trianglarna i meshar, utan linjedelens
    uint64_t segments = 0;           // linjesegment i fragmenten som lästes
    uint64_t segments_skipped = 0;   // över taket
    uint64_t points = 0;             // punkter, ritas inte
    uint64_t texts = 0;              // texter som inte blev etiketter
    uint64_t undecoded = 0;          // fragment vars geometri HNwd inte avkodar
    uint64_t rotated_textures = 0;   // fragment med en vriden bild (vridningen är obelagd)
    uint32_t fragments = 0;          // fragment som togs med

    // Mätvärden från bygget.
    double read_seconds = 0;      // i GeometryReader::fragment_mesh
    double convert_seconds = 0;   // världen, normalerna och delarna
    uint64_t vertices = 0;        // hörn i delarna, linjedelens med
    uint64_t flat_fragments = 0;  // triangelfragment vars normaler alla är ytornas (inom 1e-3)
};

// En posts placering som matriser, räknade en gång för ett helt fragment i
// stället för en gång per hörn. point() och normal() ger exakt samma tal som
// HNwd::world_point() och HNwd::world_normal() för samma pose.
class PoseTransform {
public:
    explicit PoseTransform(const HNwd::Pose& pose);

    // Punkten i världen, i filens enhet.
    void point(const float v[3], double out[3]) const;
    // Normalen i världen, med längden 1; en normal som blir 0 lämnas som 0.
    void normal(const float n[3], float out[3]) const;

private:
    double linear_[9];    // radvis: s · R(q), eller matrisen själv
    double cofactor_[9];  // kofaktormatrisen, med tecknet på determinanten inräknat
    double t_[3];
};

// Ligger varje hörns normal inom 1e-3 från ytans normal i trianglarna som
// hörnet sitter i? `world` är hörnens lägen i världen (xyz per hörn),
// `normals` deras normaler i världen (xyz per hörn), `vertices` antalet hörn.
// Ytans normal är kryssprodukten av trianglarnas kanter, med framsidan
// moturs. En triangel utan yta, och en med ett index utanför, räknas inte;
// utan någon triangel som räknas är svaret nej.
bool normals_are_surface(const std::vector<double>& world, const float* normals,
                         const std::vector<uint32_t>& indices, size_t vertices);

// Läser fragmenten med läsaren och bygger geometrin kring `bounds` mitt, med
// fragmenten i den givna ordningen i varje del (FragmentRange::size är
// fragmentets diagonal). Avbryts när `stop` begärs (Status::Cancelled). En
// post som HNwd inte avkodar räknas i `undecoded`; varje annat fel avbryter
// med felet i `error`.
// flat_fragments räknar de triangelfragment med normaler i posten vars
// normaler alla ligger inom 1e-3 från ytans normal (kryssprodukten av
// triangelns kanter i världen; en triangel utan yta räknas inte).
HNwd::Status build_fragments(HNwd::GeometryReader& reader, const Model& m,
                             const uint32_t* fragments, size_t count, const Box& bounds,
                             const BuildLimits& limits, std::stop_token stop,
                             CellGeometry& out, std::string& error);

// En bits alla fragment. Fragmenten läses i geometripostens ordning
// (Fragment::record växande, eftersom utspridda poster är långsamma att läsa)
// och delarnas trianglar läggs i bitens ordning, störst först, med
// FragmentRange::size satt till bitens storlek för fragmentet. Delarna,
// linjerna, texterna, modellfiltret och räknarna är som i build_fragments;
// när taken för linjesegment och etiketter nås är det de största fragmenten
// som behålls. Origo är bitens lådas mitt. En bit utanför trädet ger
// Status::OutOfRange.
//
// BuildLimits::min_size väljer prefixet av bitens fragment som är minst så
// stora (LodTree::prefix_at_least); de andra läses inte, och delarnas
// FragmentRange har bara prefixets fragment.
HNwd::Status build_tile(HNwd::GeometryReader& reader, const Model& m, const LodTree& tree,
                        uint32_t tile, const BuildLimits& limits, std::stop_token stop,
                        CellGeometry& out, std::string& error);

// Minne i byte, i RAM och på grafikkortet var för sig.
struct PartBytes {
    size_t ram = 0, gpu = 0;
};

// Byte per hörn utan uv (uv lägger till 8 i båda) och per triangels tre index
// (4 byte per index, i båda), som part_bytes räknar dem.
inline constexpr size_t kRamBytesPerVertex = 12 + 12 + 4;   // läge, normal, färg
inline constexpr size_t kGpuBytesPerVertex = 12 + 4 + 4;    // läge, packad normal, packad färg
inline constexpr size_t kIndexBytesPerTriangle = 3 * 4;
// Byte per hörn i en linjedel: läge, andra änden, sida och ände, färg.
inline constexpr size_t kRamBytesPerLineVertex = 12 + 12 + 8 + 4;
inline constexpr size_t kGpuBytesPerLineVertex = 12 + 12 + 4 + 4;   // sida och ände packade i 4 byte

// Uppskattat minne för en del, utan plockningens sökträd. CPU-kopian har
// kRamBytesPerVertex (+ 8 med uv) byte per hörn och 4 per index, och
// dessutom delens FragmentRange; grafikkortet har kGpuBytesPerVertex (+ 8)
// byte per hörn och 4 per index. En linjedel har kRamBytesPerLineVertex
// respektive kGpuBytesPerLineVertex byte per hörn.
PartBytes part_bytes(const MeshPart& p);

// Plockningens sökträd för en del med så många trianglar, bara i RAM.
inline constexpr size_t kPickBytesPerTriangle = 20;
PartBytes pick_bytes(uint64_t triangles);

} // namespace nwdview
