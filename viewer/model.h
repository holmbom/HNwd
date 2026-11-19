#pragma once

// Modellen som visaren arbetar med: dokumentet med fragment och rumsligt
// index, enheten, lådorna i meter och vyns låda. Ren std och HNwd — inga
// fönstertyper — så att samma kod går att använda utanför visaren.
//
// Allt här byggs i en arbetstråd (load_model) och är oföränderligt när det
// väl lämnats ut: dokumentet används därefter genom const-anrop och genom en
// GeometryReader per tråd, och trädet läses in i det med ett enda
// load_tree() i trädets arbetstråd, som HNwd.h tillåter samtidigt med de
// andra (trådkontraktet där).

#include "HNwd.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace nwdview {

// En låda i världen i meter, i modellens eget koordinatsystem.
struct Box {
    double min[3] = {1e300, 1e300, 1e300};
    double max[3] = {-1e300, -1e300, -1e300};

    bool valid() const { return min[0] <= max[0] && min[1] <= max[1] && min[2] <= max[2]; }
    void add(const Box& b);
    void add_point(const double p[3]);
    double diagonal() const;
    void center(double out[3]) const;
};

// En nod i det rumsliga indexet, med lådan i meter.
struct SpatialCell {
    uint32_t parent = HNwd::kNone;
    uint32_t first_child = 0;   // index i Model::children
    uint32_t child_count = 0;
    uint32_t item_begin = 0;    // index i Model::items
    uint32_t item_count = 0;
    uint64_t primitives = 0;
    Box bounds;
};

// En post bland filens sparade vyer: en vy eller en mapp.
struct SavedViewInfo {
    std::string name;
    bool folder = false;
    uint32_t parent = HNwd::kNone;   // index i Model::saved_views; kNone på översta nivån
    HNwd::Camera camera;             // bara en vy
};

// En rutnätslinje i meter, med etiketten.
struct GridLineInfo {
    std::string label;
    double a[3] = {0, 0, 0};
    double b[3] = {0, 0, 0};
};

// En nivå i ett rutnät; höjden i meter.
struct GridLevelInfo {
    std::string system;
    std::string name;
    double elevation = 0;
};

struct Model {
    std::string path;
    std::shared_ptr<const HNwd::Document> doc;
    // Samma dokument som `doc`, för load_tree(): det enda anrop som inte är
    // const. Bara load_tree() nedan använder det.
    std::shared_ptr<HNwd::Document> tree_doc;

    HNwd::Unit unit;              // som filen anger den
    bool unit_known = false;
    double scale = 1;             // meter per filenhet; 1 när enheten är okänd
    std::string unit_text;        // för statusraden

    size_t fragment_count = 0;
    Box world;                    // alla fragment
    Box view;                     // modellens huvuddel: det kameran passas in på

    std::vector<SpatialCell> nodes;   // preorder, [0] är roten
    std::vector<uint32_t> children;   // varje nods barn i följd
    const uint32_t* items = nullptr;  // fragmentindex i lövordning (dokumentets)
    bool spatial_from_file = false;

    size_t memory_bytes = 0;      // det dokumentet håller efter inläsningen
    int normal_bits = 0;          // geometrins normalbredd, avgjord vid inläsningen

    // Filens kameror: den aktuella vyn, hemvyn och de sparade vyerna i
    // preorder (en mapp före sina poster). Kamerorna i filens enhet.
    bool has_current_view = false;
    HNwd::Camera current_view;
    bool has_home_view = false;
    HNwd::Camera home_view;
    std::vector<SavedViewInfo> saved_views;
    // Rutnäten: linjerna i meter och nivåerna.
    std::vector<GridLineInfo> grid_lines;
    std::vector<GridLevelInfo> grid_levels;
    // Det som inte gick att läsa av vyerna och rutnäten; tomt när allt lästes.
    std::string extras_error;
};

// En kamera som visarens orbit: målet och avståndet i meter, och vinklarna i
// grader som visarens kamera räknar dem — azimuten är kompassriktningen
// blicken pekar mot (0 = +y, 90 = +x), elevationen hur mycket blicken lutar
// nedåt.
struct OrbitPose {
    double target[3] = {0, 0, 0};
    double distance = 1;
    double azimuth = 0;
    double elevation = 0;
};

// Filens kamera som orbit. Målet är punkten på blicklinjen närmast mitten av
// modellens huvuddel. false när kameran inte ser på modellen: mitten ligger
// bakom ögat, eller lådan ligger helt utanför en kon kring blicken.
bool orbit_from_camera(const Model& m, const HNwd::Camera& camera, OrbitPose& out);

// Snittet som visaren ritar: högst sex plan i meter i modellens koordinater.
// Plan k behåller punkterna x där
//
//   plane[k][0..2] · x + plane[k][3] ≥ 0
//
// alltså sidan som normalen (plane[k][0..2]) pekar mot.
struct ClipState {
    int count = 0;
    double plane[HNwd::kClipPlaneCount][4] = {};
};

// Ett plan genom punkten `point` med normalen `normal` (meter), som behåller
// sidan normalen pekar mot. false när normalen saknar längd, ett tal inte är
// ändligt eller planen redan är sex.
bool add_clip_plane(ClipState& clip, const double point[3], const double normal[3]);

// Är punkten (meter) bortklippt?
bool clipped_away(const ClipState& clip, const double x[3]);

// Kamerans påslagna snittplan som visarens snitt. Vilken sida filen behåller
// är inte belagt; visaren väljer den så här:
//
// - två påslagna plan med motsatta normaler vars punkter skiljer sig längs
//   normalen är ett par, och paret behåller det som ligger MELLAN planen.
//   Sidan som det ger (normalen pekar in mot det behållna, eller ut från det)
//   gäller då varje plan i vyn;
// - utan något par behålls den sida av det första påslagna planet där
//   kamerans öga står, och samma sida av normalen för de andra planen.
//
// Ger par i samma vy olika svar går snittet inte att tolka: `clip` blir tomt
// och `error` säger varför. Annars är `error` tomt.
void clip_from_camera(const Model& m, const HNwd::Camera& camera, ClipState& clip,
                      std::string& error);

// Fragmentets världslåda i meter.
bool fragment_box(const Model& m, size_t fragment, Box& out);

// En punkt i postens koordinater till världen i meter.
void world_metres(const Model& m, const HNwd::Pose& pose, const float v[3], double out[3]);

// Förloppet under inläsningen: vilket steg och hur långt det kommit.
// Svarar funktionen false avbryts inläsningen.
using LoadProgress = std::function<bool(const char* step, uint64_t done, uint64_t total)>;

struct LoadResult {
    std::shared_ptr<const Model> model;
    HNwd::Status status = HNwd::Status::Ok;
    std::string error;   // tom när status är Ok
};

// Öppnar filen och läser geometrins förteckning, fragmenten och det rumsliga
// indexet, i den ordningen, och räknar lådorna i meter.
LoadResult load_model(const std::string& path, const LoadProgress& progress);

// En inläst modell i filen, som modellistan visar den.
struct ModelInfo {
    std::string name;
    std::string unit_text;
    uint32_t node = 0;
    uint32_t first_fragment = 0;
    uint32_t fragment_count = 0;
};

// Trädets skelett (HNwd::TreeContent::Structure), inläst i modellens eget
// dokument medan arbetstrådarna fortsätter med sina const-anrop och
// läsare. Fragmenten är redan lästa, så modellerna (Document::model()) finns
// så snart trädet är publicerat.
struct TreeResult {
    bool loaded = false;   // trädet är läst; modellens dokument svarar på det
    std::vector<ModelInfo> models;
    HNwd::Status status = HNwd::Status::Ok;
    std::string error;
    size_t memory_bytes = 0;   // dokumentet med trädet
};
// Anropas en gång per modell, i en och samma arbetstråd. Förloppet räknas i
// trädets packade byte; svarar det false avbryts läsningen, och dokumentet
// står kvar utan träd.
TreeResult load_tree(const Model& model, const LoadProgress& progress);

// En rad i egenskapslistan: en rubrik (category) eller namn och värde.
struct PropertyRow {
    std::string name;
    std::string value;
    bool category = false;
};

// Det som går att säga om fragmentet utan trädet: geometrin och materialet.
std::vector<PropertyRow> describe_geometry(const Model& m, size_t fragment);

// Objektet som fragmentet sitter på: namn, väg i trädet (modell / grupp /
// objekt), GUID och flikarna för noden och dess förfäder. Flikarna läses med
// `reader` i stigande nodordning. Ett fel lägger en rad om det och svarar
// med det som gick att läsa.
std::vector<PropertyRow> describe_object(const HNwd::Document& tree,
                                         const std::vector<ModelInfo>& models,
                                         HNwd::PropertyReader& reader, uint32_t leaf);

// Lådan kring hela objektet som fragmentet hör till: alla fragment med samma
// seq, som ligger i följd eftersom seq aldrig avtar. Ett objekt med mycket
// många fragment ramas in av de närmaste.
Box object_box(const Model& m, size_t fragment);

// Hur ofta geometriposterna återanvänds: hur många fragment som visar varje
// post som något fragment visar. hist[k] = poster som visas av [2^k, 2^(k+1))
// fragment; hist är lika lång som det högsta k som förekommer plus ett.
// triangles_once räknar varje posts trianglar (eller segment eller punkter,
// Fragment::primitives) en gång, triangles_placed en gång per fragment.
//
// De fyra listorna efter triangles_placed har samma längd som hist och är
// uppdelade på samma sätt: per hink, de poster hinken räknar (hist[k]) och
// deras fragment, och primitiverna räknade en gång per post och en gång per
// fragment. Summan över hinkarna är records, triangles_once och
// triangles_placed; bucket_fragments summerar till antalet fragment som visar
// en post.
struct RecordUse {
    std::vector<uint64_t> hist;
    uint64_t records = 0, triangles_once = 0, triangles_placed = 0;
    std::vector<uint64_t> bucket_triangles_once;     // per hink: primitiver, en gång per post
    std::vector<uint64_t> bucket_triangles_placed;   // per hink: primitiver, en gång per fragment
    std::vector<uint64_t> bucket_fragments;          // per hink: fragmenten som visar hinkens poster
};

// Vad en gräns "minst 2^first_bucket fragment per post" sparar i grova drag:
// de poster som hinkarna från first_bucket och uppåt räknar (varje sådan post
// blir en grupp), deras fragment, och skillnaden mellan primitiver räknade en
// gång per fragment och en gång per post. Hinkar som inte finns i `use` räknas
// som tomma; en first_bucket över den sista ger noll överallt.
struct RecordUseSaving {
    uint64_t groups = 0;
    uint64_t fragments = 0;
    uint64_t saved_primitives = 0;
};
RecordUseSaving record_use_saving(const RecordUse& use, size_t first_bucket);

// Läser fragmenten i en tråd, utan att avkoda geometrin. Avbryts `stop` är
// svaret tomt (records är 0).
RecordUse record_use(const Model& m, std::stop_token stop);

// Enhetens text för statusraden och modellistan.
std::string unit_text(const HNwd::Unit& u, HNwd::Status status, const std::string& why);

std::string format_number(double v);

} // namespace nwdview
