#pragma once

// HNwd — läser Navisworks-filer (.nwd, .nwc) utan Navisworks.
//
// Det här är den enda header ett program behöver.
//
//   HNwd::Document doc;
//   if (doc.open("modell.nwd") != HNwd::Status::Ok) return 1;   // doc.last_error() säger varför
//   if (doc.load_fragments() != HNwd::Status::Ok) return 1;
//   HNwd::Mesh mesh;
//   HNwd::Fragment f;
//   for (size_t i = 0; i < doc.fragment_count(); ++i) {
//       if (doc.fragment_mesh(i, mesh) != HNwd::Status::Ok) return 1;
//       doc.fragment(i, f);
//       // mesh.positions i postens egna koordinater; HNwd::world_point(f.pose, …) ger världen
//   }

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace HNwd {

// Bibliotekets version. Samma tal som project(VERSION) i CMakeLists.txt; de
// två ändras tillsammans.
inline constexpr int kVersionMajor = 0;
inline constexpr int kVersionMinor = 2;
inline constexpr int kVersionPatch = 0;
inline constexpr const char* kVersionString = "0.2.0";

// Inget index: ingen förälder, inget barn, inget syskon.
inline constexpr uint32_t kNone = UINT32_MAX;

enum class Status : uint8_t {
    Ok = 0,
    FileNotFound,
    FileOpenFailed,
    MmapFailed,
    NotNavisworks,   // filen är inte i det här formatet
    NoDirectory,     // filen saknar katalogen över sina paket
    Corrupt,         // något i filen bryter formatet; last_error() säger vad och var
    Unsupported,     // giltigt, men HNwd avkodar det inte
    LimitExceeded,   // ett tak i Options nåddes
    Missing,         // filen har inte det paket anropet behöver
    NotLoaded,       // anropet kräver ett load_…() som inte är gjort
    OutOfRange,      // ett index utanför det som finns, eller ett val i Options som inte går
    Cancelled,       // anroparen avbröt genom förloppsfunktionen
};

// Aldrig nullptr.
const char* status_text(Status status);

// Förloppet i ett långt anrop: hur mycket som är gjort av hur mycket. Svarar
// funktionen false avbryts anropet med Status::Cancelled, och det som anropet
// läste släpps; anropet går att göra om. En tom funktion frågas aldrig.
// Funktionen anropas i den tråd som gjorde anropet.
using Progress = std::function<bool(uint64_t done, uint64_t total)>;

// En låda i världen.
struct BoxD {
    double min[3] = {0, 0, 0};
    double max[3] = {0, 0, 0};
};

// Längdenheten för filens koordinater.
enum class LengthUnit : uint8_t { Unknown, Metre, Millimetre, Foot, Centimetre };

struct Unit {
    LengthUnit unit = LengthUnit::Unknown;
    int32_t code = -1;            // koden som den står i filen; -1 om den inte är läst
    double metres_per_unit = 0;   // 0 om enheten inte är känd
};

// Tak och val vid öppningen. Taken skyddar mot en trasig fil; ett tak som en
// riktig fil slår i ger LimitExceeded, och går då att höja.
struct Options {
    size_t max_stream_bytes = size_t(2) << 30;      // största uppackade ström
    size_t chunk_cache_bytes = size_t(256) << 20;   // uppackad geometri som hålls i minnet
    uint64_t max_tree_nodes = uint64_t(1) << 23;    // största träd, räknat i noder
    size_t max_record_bytes = size_t(16) << 20;     // längsta geometripost; tolkad tar en post upp till ~70 gånger mer
    // Största uppackade småpaket: vyerna, rutnäten, typsnitten, scenens
    // material, bakgrunden, ljusen, filens identitet och loggen.
    size_t max_element_bytes = size_t(256) << 20;
    // Längsta JSON-text för ett material, och därmed flest värden i den: ett
    // per 16 byte. Gäller fragmentens material, scenens material, ljusen och
    // bakgrunden.
    size_t max_material_json_bytes = size_t(4) << 20;
    int normal_bits = 0;                            // normalernas bredd: 8 eller 16, 0 avgör den ur filen
    // Geometrin ligger i chunkar som packas upp till 65536 byte, alla utom den
    // sista, och en post hämtas då direkt. En fil som inte följer det ger
    // Corrupt; med true mäts i stället varje chunk när geometrin läses in.
    bool measure_chunks = false;
};

// En post i filens katalog.
struct DirectoryEntry {
    std::string_view name;   // som det står i filen
    std::string_view leaf;   // namnet efter sista bakstrecket
    int32_t flags = 0;       // 1 = zlib-strömmar, 3 = råa byte
    uint64_t offset = 0;     // i filen
    uint64_t length = 0;     // i filen
    int32_t a = 0, b = 0;
};

enum class Package : uint8_t {
    Hierarchy,        // objektträdet
    Fragments,        // placeringen
    Geometry,         // geometriposterna
    GuidStore,        // GUID per element
    PartitionProps,   // egenskaper som en nod använder ensam
    Partition,        // filens egen partition
    XRefTable,        // externa referenser
    SourceHeader,     // varifrån en .nwc kommer
};

// Var en geometribit sitter. Vanligen värld = s · R(q) · v + t. En placering
// med olika skala per axel har i stället en matris: värld = M · v + t.
// world_point() räknar rätt i båda fallen.
struct Pose {
    double t[3] = {0, 0, 0};
    double q[4] = {0, 0, 0, 1};   // kvaternion (x, y, z, w), som den står i filen
    double s = 1;                 // enhetlig skala; negativ speglar
    bool affine = false;          // sann: m gäller, och q och s används inte
    double m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};   // radvis: m[3·rad + kolumn]
};

struct Box {
    float min[3] = {0, 0, 0};
    float max[3] = {0, 0, 0};
};

// Ett materials utseende.
struct Look {
    float ambient[3], diffuse[3], specular[3], emissive[3];
    float shininess;
    float transparency;        // 0 är ogenomskinligt
    uint32_t material = kNone;   // index för Document::material(); kNone om utseendet saknar ett
};

// Ett material med grundfärg och bild, ur filens JSON-beskrivning. Två
// utseenden som pekar på samma material i filen har samma index.
//
// Vyerna och pekaren gäller så länge dokumentet är öppet och fragmenten är
// inlästa, som strängvyerna i övrigt (se Document).
struct Material {
    std::string_view json;            // materialbeskrivningen som den står i filen
    bool has_color = false;           // grundtillgången har generic_diffuse med ett värde
    float base_color[4] = {1, 1, 1, 1};   // r, g, b, a; 1, 1, 1, 1 om has_color är falskt
    // En bild är inkopplad: grundfärgens koppling är påslagen och pekar på en
    // bild med ett filnamn. texture_name är då inte tom.
    bool has_texture = false;
    std::string_view texture_name;    // bildens filnamn som det står i beskrivningen
    std::string_view texture_type;    // filändelsen ("jpg", "png"); tom om bilden inte ligger i filen
    const uint8_t* texture_bytes = nullptr;   // bildfilen; nullptr om den inte ligger i filen
    size_t texture_size = 0;
    // UV-transformen som den står i beskrivningen. Ingenting av den är
    // tillämpat på Mesh::uvs.
    double u_scale = 1, v_scale = 1;
    double u_offset = 0, v_offset = 0;
    double w_angle = 0;               // vridningen, i den enhet beskrivningen har
    double real_scale_x = 0, real_scale_y = 0;   // bildens storlek i världen; 0 om de inte står
    std::string_view real_units_x, real_units_y; // enheten för dem; tom om den inte står
    bool u_repeat = true, v_repeat = true;
};

// En geometribit: en geometripost på en plats, med ett utseende. Flera
// fragment kan visa samma post.
struct Fragment {
    uint32_t record = 0;       // geometripostens nummer, 1-baserat
    uint32_t seq = 0;          // noden i trädet (index för node()); samma seq = samma objekt
    uint32_t primitives = 0;   // trianglar, segment eller punkter i posten
    uint32_t look = 0;         // index för look()
    Pose pose;
    Box bounds;                // postens låda i dess egna koordinater
};

// En nod i det rumsliga indexet över fragmenten. Ett löv har fragment och
// inga barn; en inre nod har barn, och item_begin/item_count täcker då hela
// delträdets fragment, som ligger i följd i spatial_items().
struct SpatialNode {
    uint32_t parent = kNone;    // kNone för roten
    uint32_t child_count = 0;
    uint32_t item_begin = 0;    // index i spatial_items()
    uint32_t item_count = 0;
    uint64_t primitives = 0;    // summan av Fragment::primitives i delträdet
    BoxD bounds;                // världslådan kring delträdets fragment
};

// Varifrån load_spatial() tar indexet.
enum class SpatialSource : uint8_t {
    File,   // filens eget index när filen har det, annars ett eget
    Own,    // alltid ett eget, byggt ur fragmentens lådor
};

enum class MeshKind : uint8_t { Triangles, Lines, Points, Text };

// En geometripost, i sina egna koordinater.
struct Mesh {
    MeshKind kind = MeshKind::Triangles;
    std::vector<float> positions;   // xyz per vertex
    std::vector<float> normals;     // xyz per vertex; tom för linjer och punkter
    std::vector<float> uvs;         // uv per vertex; tom om posten saknar UV
    std::vector<uint32_t> colors;   // 0xRRGGBBAA per vertex; tom om posten saknar färg
    std::vector<uint32_t> indices;  // tre per triangel, framsidan moturs; två per segment; tom för punkter
    Box bounds;
    // MeshKind::Text: posten är en text och har ingen geometri. `bounds` är
    // textens låda, `text` byten som de står i filen, och texten är vriden
    // `text_angle` radianer kring enhetsvektorn `text_axis`. Glyferna står
    // inte i filen; `text_font` är ett index för typsnittet.
    std::string text;
    float text_angle = 0;
    float text_axis[3] = {0, 0, 1};
    int32_t text_font = 0;
};

enum class NodeKind : uint8_t {
    Root,        // nod 0, som håller toppnoderna
    Partition,   // en inläst modell i en fil med flera
    Group,
    Leaf,        // bär geometri
};

struct Node {
    uint32_t parent = kNone;
    uint32_t child_count = 0;
    uint32_t subtree_size = 1;   // noden och allt under den
    NodeKind kind = NodeKind::Group;
    int32_t element = 0;         // 0 om noden inte är ett element; annars är element-1 index för guid()
    std::string_view name;       // nodens namn; visningsnamnet om den saknar eget
    std::string_view display;    // klassens visningsnamn
    std::string_view internal;   // klassens interna namn
    std::string_view material;   // materialets namn; ärvs av noderna under
};

enum class ValueType : uint8_t {
    None,
    Double,   // d[0]
    Int,      // i
    String,   // text
    Time,     // i: unix-sekunder
    Name,     // text: ett visningsnamn
    Point3,   // d[0..2]
    Point2,   // d[0..1]
};

// En flik med egenskaper på en nod.
struct Tab {
    std::string_view name;       // flikens namn; visningsnamnet om den saknar eget
    std::string_view internal;   // flikens interna namn
    uint32_t property_count = 0;
};

struct Property {
    std::string_view name;       // visningsnamn
    std::string_view internal;   // internt namn
    int32_t raw_type = 0;        // typen som den står i filen
    ValueType type = ValueType::None;
    double d[3] = {0, 0, 0};
    int64_t i = 0;
    std::string_view text;
};

// Varifrån en modell kommer.
struct Source {
    std::string_view path;     // källfilens sökväg
    std::string_view loader;   // den inläsare som gjorde filen
    int64_t unix_time = 0;     // källfilens ändringstid, 0 om ingen
    int64_t size = 0;          // källfilens storlek i byte, 0 om ingen
};

// Vad Document::load_tree() håller i minnet.
enum class TreeContent : uint8_t {
    Full,        // noderna med sina flikar och egenskaper
    Structure,   // noderna utan flikar; en nods flikar läses ur filen med PropertyReader
};

// En inläst modell (se Document::model()).
struct Model {
    std::string_view name;     // nodens namn, som Node::name; för en fil med en modell partitionens
    std::string_view source;   // källfilens sökväg som den står i filen; tom om den saknas
    std::string_view loader;   // inläsaren som gjorde modellen; tom om den saknas
    // Längdenheten i modellens partition, med koden som den står där. En kod
    // som inte är känd ger LengthUnit::Unknown och metres_per_unit 0, med
    // koden i code. Om fragmentens poser i en modell som sitter i en annan
    // redan är omräknade till den omslutande modellens enhet är inte belagt.
    Unit unit;
    uint32_t node = 0;             // modellens nod: en nod av klass 32, eller roten för en fil med en modell
    // Fragmenten i modellens delträd, de i modellerna under den inräknade; de
    // ligger i följd.
    uint32_t first_fragment = 0;
    uint32_t fragment_count = 0;
    uint32_t parent = kNone;       // närmaste omslutande modell (index för model()); kNone på översta nivån
    uint32_t depth = 0;            // antalet omslutande modeller; 0 på översta nivån
};

// En nods flikar och egenskaper, ur PropertyReader::read(). Egenskaperna
// ligger flik för flik i följd: flik k:s Tab::property_count egenskaper
// börjar där flikarna före den slutar.
struct NodeProperties {
    std::vector<Tab> tabs;
    std::vector<Property> properties;
};

// En kamera ur filen: den aktuella vyn, hemvyn eller en sparad vy.
//
// Koordinaterna är filens: samma rymd som fragmentens världspunkter
// (world_point()), i filens längdenhet. Document::length_unit() ger meter
// per enhet.
//
// Kameran tittar längs blickriktningen f = R(rotation) · (0, 0, −1), där R är
// kvaternionens rotation som i Pose. Med rotation = (x, y, z, w):
//
//   f = (−2(xz + yw), −2(yz − xw), −(1 − 2(x² + y²)))
//
// camera_forward() räknar den. Belagt i exempelfilerna: rotation har
// längden 1; i varje .nwc är position + distance · f mitten av fragmentens
// världslåda; i .nwd ligger varje vertex i modellen mellan near_depth och
// far_depth längs f. Vart kameran har sitt upp är inte belagt.
//
// Varje vy har sex snittplan (clip). Ett plan är punkten `point` i planet och
// dess normal `normal`, i filens koordinater och längdenhet, samma som
// kamerans `position`. Belagt: i ett påslaget plan har normalen längden 1
// (load_views() ger Unsupported annars); i två påslagna plan med motsatta
// normaler är skillnaden mellan punkterna parallell med normalen; plan 0 och 1
// slås på tillsammans, och plan 2 och 3 tillsammans med dem, med motsatta
// normaler i varje par. I exempelfilerna är inget plan påslaget.
//
// Vilken sida av ett plan som behålls är INTE belagt, och inte heller vad ett
// avslaget plans punkt och normal betyder. Planens övriga fält i filen (en
// tangent, ett läge, flaggor och en kvaternion) lämnas inte ut, eftersom
// deras betydelse inte är känd.
struct ClipPlane {
    bool enabled = false;
    double point[3] = {0, 0, 0};
    double normal[3] = {0, 0, 0};
};

inline constexpr size_t kClipPlaneCount = 6;

struct Camera {
    int32_t form = 0;                    // vyns form som den står i filen: 9, 843, 895 eller 1919
    // 0 eller 1 som det står; 1 bara i form 1919. Att 1 betyder en
    // ortografisk vy är inte belagt.
    int32_t projection = 0;
    double position[3] = {0, 0, 0};      // ögat
    double rotation[4] = {0, 0, 0, 1};   // kvaternion (x, y, z, w)
    // Djup längs f. Att geometrin ligger mellan dem är belagt i .nwd; i varje
    // .nwc är de 1 och 10, så att de är klipplan är inte belagt.
    double near_depth = 0, far_depth = 0;
    // Talet som det står, rollen inte belagd: π/4 avrundat till sex decimaler
    // i varje exempelfil, ett mått i längdenheten i form 1919.
    double angle = 0;
    // Bara form 9 och 843: avståndet längs f till blickpunkten, och
    // blickpunkten target = position + distance · f.
    bool has_target = false;
    double distance = 0;
    double target[3] = {0, 0, 0};
    // Vyns sex snittplan i filens ordning, påslagna och avslagna (ClipPlane).
    ClipPlane clip[kClipPlaneCount];
};

// Antalet påslagna snittplan i kameran.
size_t clip_plane_count(const Camera& camera);

// Blickriktningen R(rotation) · (0, 0, −1), med längden 1 när kvaternionen har det.
void camera_forward(const Camera& camera, double out[3]);

// En post bland de sparade vyerna: en vy eller en mapp. Posterna ligger i
// preorder, en mapp före sina poster, så en mapps första post är den direkt
// efter mappen.
enum class SavedViewKind : uint8_t { View, Folder };

struct SavedView {
    SavedViewKind kind = SavedViewKind::View;
    std::string_view name;
    uint8_t guid[16] = {};             // som de 16 byten ligger i filen
    uint32_t parent = kNone;           // mappen; kNone på översta nivån
    uint32_t child_count = 0;          // mappens poster; 0 för en vy
    uint32_t next_sibling = kNone;     // nästa post i samma mapp; kNone efter den sista
    uint32_t redline_count = 0;        // vyns markeringar (saved_view_redline())
    Camera camera;                     // bara en vy
};

// En markering i en sparad vy. Grammatiken är belagd; punkterna ligger i
// [−1, 1] i varje markering som prövats, vilket tyder på normerade
// skärmkoordinater, men det är inte prövat. Linjens bredd och mönster lämnas
// inte ut, eftersom deras betydelse inte är känd.
enum class RedlineKind : uint8_t { Line, Text };

struct Redline {
    RedlineKind kind = RedlineKind::Line;
    std::string_view text;             // bara Text
    double points[4] = {0, 0, 0, 0};   // Line: x0, y0, x1, y1; Text: x, y och två nollor
    double colour[3] = {0, 0, 0};      // r, g, b i [0, 1]
};

// Ett rutnätssystem (Document::load_grids()).
//
// En linje står i systemets egna tal (x0, y0, x1, y1) och som två punkter i
// filens koordinater, räknade
//
//   punkt = origin + x · axis_x + y · axis_y
//
// Belagt: origin är en punkt i filens koordinater och längdenhet; axis_x och
// axis_y har längden 1 och är vinkelräta; varje linje är parallell med en av
// axlarna (x0 = x1 eller y0 = y1). Att formeln ger linjernas läge i världen
// är inte prövat mot någon fil med både rutnät och geometri på samma plats.
// Systemets tredje axel, som är lika med axis_y i varje system som prövats,
// lämnas inte ut.
struct GridSystem {
    std::string_view name;
    double origin[3] = {0, 0, 0};
    double axis_x[3] = {1, 0, 0};
    double axis_y[3] = {0, 1, 0};
    uint32_t line_count = 0;
    uint32_t level_count = 0;
    // Index i nivåerna, −1 när systemet saknar nivåer. Det ser ut som den
    // valda nivån; rollen är inte belagd.
    int32_t level = -1;
};

struct GridLine {
    std::string_view label;
    double ends[4] = {0, 0, 0, 0};     // x0, y0, x1, y1 i systemets tal
    double world[2][3] = {};           // de två punkterna i filens koordinater (formeln vid GridSystem)
};

// En nivå: namnet och höjden i filens längdenhet, stigande inom ett system.
// Mot vilken nollpunkt höjden räknas är inte belagt.
struct GridLevel {
    std::string_view name;
    double elevation = 0;
};

// LcOpBackgroundElement (Document::background()). Sju färger som de står i
// filen; varje komponent ligger i [0, 1]. Vilken färg som används till vad är
// inte belagt. Talet som inleder paketet lämnas inte ut: dess roll är okänd.
struct Background {
    double colours[7][3] = {};
    bool has_sky = false;      // paketet har en himmel: ett material vars grundtillgång är "SunAndSky"
    std::string sky_json;      // himlens materialbeskrivning som den står i filen; tom utan himmel
};

// Ett ljus ur LcOpLightsElement (Document::lights()).
struct Light {
    std::string name;
    uint8_t guid[16] = {};          // samma som "VersionGUID" i ljusets materialbeskrivning
    double origin[3] = {0, 0, 0};   // (0, 0, 0) i varje exempel; rollen är inte belagd
    // Tre tal med längden 1, lika med de tre sista i ljusets kropp. Namnet är
    // en tolkning; att det är ljusets riktning är inte prövat mot en bild.
    double direction[3] = {0, 0, 0};
    double specular[3] = {0, 0, 0}; // lika med "specularColor" i materialbeskrivningen
    std::string json;               // ljusets materialbeskrivning; tom om ljuset saknar en
};

// Filens identitet (Document::file_identity()).
struct FileIdentity {
    // LcOpOdyScenePropsChunk: sökvägen till filen själv när den sparades
    // (sista ledet är filens namn), och scenens GUID: av version 5 i varje
    // .nwc, version 4 i .nwd.
    bool has_scene = false;
    std::string saved_path;
    uint8_t scene_guid[16] = {};
    // LcOpOdyFileInfoChunk: prefixet på paketnamnen i katalogen ("cache" i en
    // .nwc), och en GUID vars roll inte är känd.
    bool has_file_info = false;
    std::string package_prefix;
    uint8_t file_guid[16] = {};
};

// Ett typsnitt ur LcOpNwdSharedNodes (Document::font()).
struct Font {
    std::string_view name;   // typsnittets namn
    double size = 0;         // som det står; vad det mäter och i vilken enhet är inte belagt
};

// Ett material ur LcReMaterialElement (Document::scene_material()).
struct SceneMaterial {
    std::string_view name;
    uint8_t guid[16] = {};      // en GUID av version 4; vad den identifierar är inte belagt
    bool has_material = false;  // posten har en materialbeskrivning
    Material material;          // samma innehåll som fragmentens material; bara om has_material
};

// En öppen fil.
//
// open() minnesmappar filen och läser bara katalogen. Varje del läses första
// gången dess load_…() anropas, och geometrin packas upp bit för bit när den
// efterfrågas, så en fil på flera GB kostar bara det som används.
//
// Varje anrop som kan misslyckas returnerar en Status, och last_error() ger
// en text om det senaste felet: vilket paket, vilken byte, vilken regel.
// Ingenting gissas — det som inte går att tolka ger ett fel. Inga undantag
// lämnar biblioteket: tar minnet slut ger anropet LimitExceeded, och
// dokumentet ska då stängas.
//
// Strängvyer och pekare som lämnas ut gäller tills dokumentet stängs, öppnas
// på nytt, flyttas till eller förstörs. Texten ur last_error() gäller bara
// till nästa anrop.
//
// Strängar lämnas ut med de byte filen har. De är UTF-8 i varje fil som har
// prövats, men ingenting kontrollerar det.
//
// Filen är minnesmappad så länge dokumentet är öppet och ska ligga kvar
// oförändrad under tiden. Kortas den av någon annan, eller försvinner disken
// den ligger på, avbryter systemet processen vid nästa läsning.
//
// Trådar. Biblioteket startar inga trådar och har inga lås:
// - open(), close(), flytt och varje load_…() utom load_tree() får inte
//   köras samtidigt med något annat anrop på samma Document.
// - När de är klara får de const-anropen (fragment(), look(), material(),
//   node(), spatial_node() och de andra) göras från flera trådar samtidigt.
// - load_tree() får köras i en tråd samtidigt med const-anropen utom
//   memory_bytes() och last_error(), med GeometryReader::open() och
//   PropertyReader::open() på dokumentet, och med läsare som redan är
//   öppna. Inget annat anrop på dokumentet får gå samtidigt med den, inte
//   heller ett andra load_tree(). Trädet byggs vid sidan av och publiceras
//   först när det är färdigt och prövat; till dess svarar trädets
//   const-anrop (has_tree(), node(), parent(), node_of_fragment(),
//   model_count(), model() och de andra) som före anropet: "inte inläst"
//   (false, 0, kNone) om inget träd var läst, annars ur det tidigare
//   trädet, som har samma noder. Därefter svarar de ur det nya. Varje
//   enskilt anrop svarar ur ett och samma träd. guid_count() och guid()
//   ger 0 och false tills GUID-tabellen, som load_tree() läser vid behov,
//   är publicerad. Det som redan var läst (fragmenten, utseendena,
//   materialen, det rumsliga indexet, geometrins layout) ändras inte av
//   load_tree().
// - last_error() skrivs av varje anrop som inte är const, också av
//   load_tree(), och hör till den tråd som gör dem. Medan load_tree() kör
//   får den bara läsas i den tråden; en annan tråd läser den först när
//   load_tree() har returnerat och trådarna är synkroniserade (till exempel
//   genom join).
// - memory_bytes() är const men läser det som varje anrop som inte är const
//   skriver. Den får göras från flera trådar samtidigt med andra const-anrop,
//   men inte samtidigt med något anrop som inte är const (load_tree(),
//   fragment_mesh(), record_mesh() och de andra).
// - fragment_mesh(), record_mesh() och de andra anropen som inte är const
//   ändrar dokumentet (en gemensam buffert och last_error()) och får bara
//   göras från en tråd i taget. En tråd som läser geometri samtidigt med
//   andra har en egen GeometryReader.
// - En GeometryReader håller filen och det som är läst vid liv, så
//   dokumentet får stängas eller förstöras medan läsare arbetar.
// - En tråd som läser nodernas flikar samtidigt med andra har en egen
//   PropertyReader, som på samma sätt håller filen och trädet vid liv.
class Document {
public:
    Document();
    ~Document();
    Document(Document&&) noexcept;
    Document& operator=(Document&&) noexcept;
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    // Sökvägen är UTF-8 på alla plattformar.
    Status open(const char* path, const Options& options = {});
    void close();
    bool is_open() const;

    // Katalogen.
    size_t entry_count() const;
    bool entry(size_t i, DirectoryEntry& out) const;
    bool has(Package package) const;
    int version() const;   // formatversionen, -1 om filen inte anger någon

    // Geometrin. load_geometry() läser innehållsförteckningen och avgör
    // normalernas bredd (se Options::normal_bits), med förlopp räknat i
    // prövade poster; posterna packas upp när de efterfrågas.
    Status load_geometry(const Progress& progress = {});
    uint32_t record_count() const;
    int normal_bits() const;                         // 0 före load_geometry()
    Status record_mesh(uint32_t record, Mesh& out);  // 1-baserat, som Fragment::record

    // Placeringen. Läser också geometrin (load_geometry() med samma förlopp),
    // och prövar att varje fragment pekar på en post som finns och att seq
    // aldrig avtar. Förloppet räknas i fragment. Paketet packas upp bit för
    // bit och ligger aldrig helt uppackat i minnet.
    Status load_fragments(const Progress& progress = {});
    size_t fragment_count() const;
    // Fragment i, i filens ordning. false före load_fragments() och för ett
    // index utanför.
    bool fragment(size_t i, Fragment& out) const;
    size_t look_count() const;
    bool look(size_t i, Look& out) const;
    // Materialen som utseendena pekar på (Look::material). Lästa med
    // fragmenten; 0 och false innan load_fragments(), och false för ett index
    // utanför.
    size_t material_count() const;
    bool material(size_t i, Material& out) const;

    // Världslådan kring fragmentets post: postens låda (Fragment::bounds)
    // genom posen, räknad ur de åtta hörnen. Varje hörn som world_point() ger
    // ligger i den. false före load_fragments() och för ett index utanför.
    bool fragment_world_box(size_t i, BoxD& out) const;

    // Geometrin för ett fragment, i postens egna koordinater. Prövar att
    // postens låda och antal är fragmentets. Speglar posen (s < 0) är
    // triangelordningen vänd, så att framsidan är moturs också i världen.
    Status fragment_mesh(size_t fragment, Mesh& out);

    // Längdenheten i filens egen partition (LcOpNwdPartition). Koden 0 är
    // meter, 1 centimeter, 2 millimeter och 3 fot; en annan kod ger Unsupported, med koden
    // i out.code och i last_error(). Missing för en fil utan partitionen.
    Status length_unit(Unit& out);

    // Det rumsliga indexet: ett träd över fragmenten där varje nod har sin
    // världslåda och sin summa av primitiver, räknade nerifrån och upp.
    // Läser fragmenten vid behov (load_fragments() med samma förlopp).
    //
    // Med SpatialSource::File tas trädet ur filens paket
    // LcOpNwdSpatialHierarchy; saknas det bygger biblioteket ett eget:
    // fragmenten delas vid medianen längs den längsta axeln tills ett löv har
    // högst 32. SpatialSource::Own bygger alltid ett eget. Båda ger samma
    // struktur. Ett paket som finns men inte går att tolka ger Corrupt, och
    // inget eget index byggs då i dess ställe.
    //
    // Ett anrop med en annan källa än förra gången byter indexet som anropen
    // svarar ur. Varje källas index byggs en gång och hålls kvar till
    // close(), så en pekare ur spatial_items() gäller så länge dokumentet är
    // öppet, och ett anrop med en källa som redan är byggd bygger inte om.
    //
    // Förloppet räknas i fragment. Utan fragment är indexet tomt. En källa
    // som varken är File eller Own ger OutOfRange.
    Status load_spatial(const Progress& progress = {},
                        SpatialSource source = SpatialSource::File);
    bool spatial_from_file() const;               // indexet kommer ur filen
    size_t spatial_node_count() const;            // 0 före load_spatial()
    bool spatial_node(uint32_t i, SpatialNode& out) const;   // 0 är roten; noderna i preorder
    uint32_t spatial_child(uint32_t node, uint32_t k) const; // barn k; kNone utanför
    // Fragmentindex i lövordning: fragment_count() stycken, varje fragment
    // en gång. nullptr före load_spatial() och utan fragment.
    const uint32_t* spatial_items() const;

    // Minnet som dokumentet håller: katalogen, geometrins layout och de
    // uppackade chunkarna i dess cache, fragmenten med material, de rumsliga
    // indexen (ett per källa som byggts, se load_spatial()), GUID-tabellen,
    // trädet (med skelettet i TreeContent::Structure, och ett tidigare träd
    // som hålls kvar, se load_tree()), vyerna, rutnäten, typsnitten och
    // scenens material.
    //
    // memory_bytes() läser det som anropen som inte är const skriver (den
    // senast tolkade posten, geometrins cache, last_error()), och får inte
    // gå samtidigt med något anrop som inte är const (se trådarna vid
    // Document).
    size_t memory_bytes() const;

    // GUID-tabellen. guid() ger de 16 byten som de ligger i filen.
    Status load_guids();
    size_t guid_count() const;
    bool guid(size_t i, uint8_t out[16]) const;

    // Varifrån filen kommer. Missing för en fil utan den uppgiften.
    Status file_source(Source& out);

    // Tabellen över externa referenser, som JSON-text.
    Status xref_json(std::string& out);

    // Trädet. Nod 0 är roten, och index är preorder: samma tal som
    // Fragment::seq. Läser GUID-tabellen och egenskaperna vid behov. Är
    // fragmenten lästa prövas att varje seq pekar på ett löv.
    Status load_tree();
    bool has_tree() const;
    size_t node_count() const;
    bool node(uint32_t index, Node& out) const;
    uint32_t first_child(uint32_t index) const;    // kNone om noden saknar barn
    uint32_t next_sibling(uint32_t index) const;   // kNone efter det sista

    // Egenskaperna på en nod.
    size_t tab_count(uint32_t node) const;
    bool tab(uint32_t node, size_t tab, Tab& out) const;
    bool property(uint32_t node, size_t tab, size_t property, Property& out) const;

    // Trädet med ett val av innehåll. load_tree() ovan är
    // load_tree(TreeContent::Full). Trädet packas upp medan det läses och
    // ligger aldrig helt uppackat i minnet; förloppet räknas i packade byte av
    // trädets och egenskapernas strömmar. Avbrott ger Cancelled, och det
    // trädet som var läst före anropet (om något) står kvar. Andra trådar
    // får göra const-anrop medan trädet läses (se trådarna vid Document).
    //
    // TreeContent::Structure ger samma noder som Full: node(), first_child(),
    // next_sibling(), parent(), node_of_fragment(), model() och kontrollen av
    // fragmentens seq svarar som med Full. Men inga flikar hålls i minnet:
    // has_tabs() är falsk, tab_count() är 0 och tab() och property() ger
    // false för varje nod, också för en nod som har flikar i filen. De läses
    // med PropertyReader, som ger samma flikar och egenskaper som Full.
    //
    // Ett nytt anrop när ett träd redan är läst:
    // - Full efter Full, och Structure efter Full eller Structure: redan
    //   uppfyllt, ger Ok utan att läsa något (Full har allt som Structure har).
    // - Full efter Structure: trädet läses om med flikarna. Det tidigare
    //   trädet hålls kvar i minnet till close(), så att strängvyer ur det
    //   fortsätter att gälla; nodernas index är desamma. Misslyckas anropet
    //   står Structure-trädet kvar.
    Status load_tree(TreeContent content, const Progress& progress = {});
    // Trädet är läst med TreeContent::Full: tab_count(), tab() och
    // property() svarar. Falsk före load_tree() och med Structure.
    bool has_tabs() const;
    // Nodens förälder, som Node::parent men utan resten av noden. kNone för
    // roten, för ett index utanför och före load_tree().
    uint32_t parent(uint32_t index) const;
    // Lövet som fragmentet sitter på: dess Fragment::seq, som är nodens
    // index. Kräver load_tree() och load_fragments(); kNone annars och för
    // ett index utanför. Gå uppåt med parent() för hela vägen.
    uint32_t node_of_fragment(size_t fragment) const;

    // Modellerna i filen. En fil med flera inlästa modeller har en per nod
    // av klass 32 (NodeKind::Partition) i trädet: toppnoderna av den klassen
    // och de som sitter under dem, på alla nivåer. Modellerna ligger i
    // preorder, en modell före dem som sitter i den, så Model::node växer med
    // indexet. Namn, källa, inläsare och enhet kommer ur nodens egen
    // partition. En nod av en annan klass är ingen modell. En fil utan noder
    // av klass 32 i trädet har en modell ur filens egen partition
    // (Package::Partition) med roten som nod, och ingen modell om den saknar
    // partitionen. En modells fragment är de vars seq ligger i modellens
    // delträd, de i modellerna under den inräknade; modellerna på översta
    // nivån överlappar inte.
    //
    // Kräver load_tree() och load_fragments(), i vilken ordning som helst:
    // model_count() är 0, model() ger false och model_of_node() och
    // model_of_fragment() ger kNone innan båda är gjorda. Inget av dem läser
    // något mer. Partitionen läses av load_tree(); går den inte att tolka ger
    // load_tree() felet.
    size_t model_count() const;
    bool model(size_t i, Model& out) const;
    // Den innersta modellen vars delträd har noden (index för model()). En
    // modells egen nod hör till modellen. kNone när ingen modell har noden,
    // och för ett index utanför.
    uint32_t model_of_node(uint32_t node) const;
    // Den innersta modellen som har fragmentets nod (node_of_fragment()), ur
    // ett och samma träd. kNone när ingen modell har den, och för ett index
    // utanför.
    uint32_t model_of_fragment(size_t fragment) const;

    // Vyerna: den aktuella vyn (LcOpCurrentViewElement), hemvyn
    // (LcOpHomeViewElement) och de sparade vyerna (LcOpSavedViewsElement).
    // Ett paket som saknas är inget fel: vyn finns då inte, och de sparade
    // vyerna är inga. Ett paket i en form som inte avkodas ger Unsupported
    // och ett trasigt Corrupt, och inget av paketen hålls då kvar. Ett nytt
    // anrop när vyerna är lästa ger Ok utan att läsa något.
    Status load_views();
    // false före load_views() och när filen saknar vyn.
    bool current_view(Camera& out) const;
    bool home_view(Camera& out) const;
    // 0 och false före load_views(), och false för ett index utanför.
    size_t saved_view_count() const;
    bool saved_view(size_t i, SavedView& out) const;
    // Markering k i den sparade vyn `view`.
    bool saved_view_redline(size_t view, size_t k, Redline& out) const;

    // Rutnäten (LcOpGridElement). Ett paket som saknas ger inga system; i
    // övrigt som load_views().
    Status load_grids();
    size_t grid_count() const;
    bool grid(size_t i, GridSystem& out) const;
    bool grid_line(size_t system, size_t k, GridLine& out) const;
    bool grid_level(size_t system, size_t k, GridLevel& out) const;

    // Typsnitten i LcOpNwdSharedNodes, i den ordning listan står i filen; två
    // poster kan vara samma typsnitt. Ett paket som saknas ger inga; i
    // övrigt som load_views().
    //
    // Mesh::text_font är ett index för ett typsnitt, men att det pekar in i
    // den här listan är inte belagt: ingen exempelfil har både text och
    // typsnitt. Indexet prövas inte mot listan.
    Status load_fonts();
    size_t font_count() const;
    bool font(size_t i, Font& out) const;

    // Materialen i LcReMaterialElement. Vad som använder dem är inte belagt.
    // Ett paket som saknas ger inga; i övrigt som load_views(). Två poster
    // kan dela en materialbeskrivning, och har då samma vyer och pekare.
    Status load_scene_materials();
    size_t scene_material_count() const;
    bool scene_material(size_t i, SceneMaterial& out) const;

    // Små uppslag som läser sitt paket vid varje anrop och inte håller
    // något kvar. Missing för en fil utan paketet, Unsupported för en form
    // som inte avkodas.
    Status background(Background& out);       // LcOpBackgroundElement
    Status lights(std::vector<Light>& out);   // LcOpLightsElement
    // LcOpOdyScenePropsChunk och LcOpOdyFileInfoChunk; Missing bara när
    // filen saknar båda.
    Status file_identity(FileIdentity& out);
    // LcOpNwdStats: inläsarens logg. I en .nwd är den de inlästa modellernas
    // loggar efter varandra, var och en inledd av "\n" <modellens namn> "\n\n".
    Status load_log(std::string& out);

    // Aldrig nullptr. Tom efter ett anrop som lyckades.
    const char* last_error() const;

private:
    friend class GeometryReader;
    friend class PropertyReader;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Läser geometri för en tråd. Varje tråd som läser geometri samtidigt med
// andra har en egen läsare, med en egen cache för uppackade chunkar.
//
// open() tar det dokumentet har läst: filen, geometrins layout och
// fragmenten, och håller dem vid liv så länge läsaren finns, också om
// dokumentet stängs eller förstörs. open() räknas som ett const-anrop på
// dokumentet (se trådarna vid Document). Läsaren själv är inte trådsäker.
class GeometryReader {
public:
    GeometryReader();
    ~GeometryReader();
    GeometryReader(GeometryReader&&) noexcept;
    GeometryReader& operator=(GeometryReader&&) noexcept;
    GeometryReader(const GeometryReader&) = delete;
    GeometryReader& operator=(const GeometryReader&) = delete;

    // Kräver load_fragments() på dokumentet. `cache_bytes` är taket för
    // läsarens uppackade chunkar.
    Status open(const Document& doc, size_t cache_bytes = size_t(64) << 20);
    void close();
    bool is_open() const;

    // Som Document::fragment_mesh() och Document::record_mesh().
    Status fragment_mesh(size_t fragment, Mesh& out);
    Status record_mesh(uint32_t record, Mesh& out);

    // Aldrig nullptr. Tom efter ett anrop som lyckades.
    const char* last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Läser nodernas flikar och egenskaper för en tråd. Varje tråd som läser
// egenskaper samtidigt med andra har en egen läsare.
//
// open() tar det dokumentet har läst: filen och trädet, och håller dem vid
// liv så länge läsaren finns, också om dokumentet stängs, förstörs eller
// läser om trädet. open() räknas som ett const-anrop på dokumentet (se
// trådarna vid Document) och tar det träd som är publicerat när den körs.
// Läsaren själv är inte trådsäker.
//
// Med TreeContent::Full kommer flikarna ur dokumentets minne. Med Structure
// läses de ur filen: trädets ström packas upp från närmaste kontrollpunkt
// före noden, eller vidare från där läsaren stod, så noder i stigande
// ordning är billigast. Flikar som flera noder delar, och flikarna för noder
// som sitter på flera ställen i trädet, hålls i en cache med taket
// `cache_bytes`, så att de inte läses om. Därutöver håller läsaren nio
// uppackningar med en buffert på 288 KB var. Resultatet är detsamma i båda
// lägena och med vilket tak som helst.
class PropertyReader {
public:
    PropertyReader();
    ~PropertyReader();
    PropertyReader(PropertyReader&&) noexcept;
    PropertyReader& operator=(PropertyReader&&) noexcept;
    PropertyReader(const PropertyReader&) = delete;
    PropertyReader& operator=(const PropertyReader&) = delete;

    // Kräver load_tree() på dokumentet, med vilket innehåll som helst.
    // `cache_bytes` är cachens tak i TreeContent::Structure; 0 stänger av den.
    Status open(const Document& doc, size_t cache_bytes = size_t(16) << 20);
    void close();
    bool is_open() const;

    // Flikarna och egenskaperna på noden `node` (index som i
    // Document::node()). `out` ersätts; roten och en nod utan flikar ger
    // tomma listor. OutOfRange för ett index utanför trädet. Strängvyerna i
    // `out` gäller till nästa read() eller close() på samma läsare, eller
    // tills läsaren förstörs eller flyttas till. Vid fel är `out` tom.
    Status read(uint32_t node, NodeProperties& out);

    // Aldrig nullptr. Tom efter ett anrop som lyckades.
    const char* last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// En punkt i världen: s · R(q) · v + t, eller M · v + t för en pose med matris.
void world_point(const Pose& pose, const float v[3], double out[3]);

// En normal i världen, med längden 1: vinkelrät mot ytan också efter en
// placering med olika skala per axel, och åt samma håll från ytan som i
// posten också när posen speglar.
void world_normal(const Pose& pose, const float n[3], float out[3]);

// Sann om posen speglar geometrin: s < 0, eller en matris med negativ
// determinant.
bool mirrors(const Pose& pose);

} // namespace HNwd
