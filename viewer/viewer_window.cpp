#include "viewer_window.h"

#include "pick.h"
#include "textures.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <functional>
#include <limits>
#include <queue>
#include <iterator>
#include <list>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>

namespace nwdview {

namespace {

// Lådor som visas innan de fasta bitarna är byggda.
constexpr size_t kMaxBoxes = 50'000;
// Den tid UI-tråden får lägga på en omgång ur köerna (operationerna ur urvalet,
// nytt snitt, ordningen i lagren, ogiltigförklaringar): ett pumpvarv.
constexpr double kQueueSliceMs = 1.5;
// Ett stopp i UI-tråden som räknas efter startvyn, och det som ger ett fel på
// stderr.
constexpr double kStallCountMs = 4;
constexpr double kStallErrorMs = 16;
// Så många bitar visar eller parkerar urvalet per genomgång; är det fler
// gör det en genomgång till direkt.
constexpr size_t kMaxShownChanges = 400;
// Urvalet håller inne visa, parkera och ritad andel medan fönstret har så här
// många operationer kvar att göra.
constexpr size_t kOpBacklog = 1'500;
// De närmaste visade bitarna som tas med när platserna i lagren ordnas.
constexpr size_t kOrderMaxTiles = 2'048;
// Plockningens läsare: ett klick, koordinaten och bitarna under ögat.
constexpr unsigned kPickReaders = 3;
// Marksökningen: bitarna under ögat hålls i cachen inom så här många meter i
// sidled, högst så här många bitar per omgång, och ögat flyttas så här långt
// (meter) innan de hålls om respektive innan ytan söks igen.
constexpr double kGroundPrimeRadius = 6;
constexpr size_t kGroundPrimeTiles = 32;
constexpr double kGroundPrimeMove = 2.5;
constexpr double kGroundProbeMove = 0.05;
// Ytan som en sökning gav gäller så här långt (meter) i sidled från där den söktes.
constexpr double kGroundStale = 2.0;
// Grundbildens (de fasta bitarnas) andel av budgeten.
constexpr double kPinnedShare = 0.2;
// Budgeten när maskinens minne inte går att läsa.
constexpr size_t kFallbackBudget = size_t(1500) << 20;
// Uppackad geometri som varje läsare håller: den större när RAM-budgeten är
// minst så här stor, annars den mindre.
constexpr size_t kReaderCacheLarge = size_t(64) << 20;
constexpr size_t kReaderCacheSmall = size_t(16) << 20;
constexpr size_t kReaderCacheThreshold = size_t(8) << 30;
// Detaljnivåerna bakas för en kamera med så här mycket större bild än den
// som byggde jobbet startade med, så att ett något större fönster inte bryter
// garantin.
constexpr double kLodMargin = 1.5;
// En del med färre trianglar än så här får bara den sista nivån (den som
// döljer den bortom avståndet där dess största fragment är för litet).
constexpr uint32_t kLodSplitTriangles = 8'192;
constexpr int kLodMaxSteps = 4;
// Ett orbitvarv med --spin tar så här lång tid.
constexpr double kSpinSecondsPerTurn = 10;
// Bilder och texturer på ritytan.
constexpr size_t kCanvasImageBudget = size_t(1024) << 20;
// Texter som ritas som etiketter, i hela scenen.
constexpr size_t kMaxLabels = 2000;
// En text ritas i sin verkliga storlek, men aldrig större än så här per em i
// bild, och döljs när den blir mindre än den minsta läsbara.
constexpr float kLabelMaxPixels = 18;
constexpr double kLabelMinPixels = 6;
// Gångläget: närplanet, och radien kring ögat där detaljen byggs också
// bakom betraktaren.
constexpr double kWalkNear = 0.05;
// Orbitens klipplan, meter. Närplanet är halva avståndet från ögat till det
// närmaste i bilden som kan ha geometri, aldrig under golvet: ingenting
// ligger närmare än så, och ett större närplan ger djupet bättre upplösning,
// så att ytor nära varandra inte flimrar. Bortre planet når modellens
// huvuddel med en marginal, inte strödelar långt därifrån.
constexpr double kOrbitNearFloor = 0.1;
constexpr double kOrbitNearOfGap = 0.5;
constexpr double kOrbitFarMargin = 1.05;
// Sökningen efter det närmaste i bilden besöker högst så här många noder.
constexpr int kNearSearchVisits = 4096;
constexpr double kWalkNearRadius = 30;
// --png: bilden tas om tills två i följd är lika, högst så länge.
constexpr double kCaptureTimeout = 30;
constexpr size_t kCaptureNoiseDivisor = 10'000;
// Uppladdningsbudgeten per bildruta medan --png tar sin bild: allt som syns
// ska hinna upp i en bildruta.
constexpr size_t kCaptureUploadBudget = size_t(2) << 30;
// Bildens storlek med --png: urvalet mäts med den, så att garantin gäller i
// bilden.
constexpr int kCaptureWidth = 1600;
constexpr int kCaptureHeight = 1000;
// Kameran räknas röra sig så här länge efter senaste händelsen.
constexpr double kMovingSeconds = 0.3;
// Så många av profilerarens senaste bilder läser ritningens kostnad ur.
constexpr size_t kCostFrames = 8;
// Texterna i etikettlagret ändras högst en gång så här ofta (ms): varje ändring
// kostar ett textbygge i ramverket.
constexpr int kLabelFlushMs = 500;
// Delar lämnas till en arbetstråd först efter så här lång tid (ms), när ramverket
// hunnit släppa sina egna referenser till meshen.
constexpr int kRetireDelayMs = 100;
// Mätningens takt i ms medan kameran rör sig och annars.
constexpr int kUpdateMovingMs = 50;
constexpr int kUpdateIdleMs = 100;
// Uppladdningsbudgeten per bild medan kameran rör sig eller ritytan tickar, och
// när den står still. Uppladdningen sker i ritytans renderingstråd, så den
// håller inte UI-tråden; budgeten håller bildtiden.
constexpr size_t kUploadMoving = size_t(32) << 20;
constexpr size_t kUploadStill = size_t(64) << 20;
// Förhandsbygget: kameran så här många sekunder framåt, ur en förflyttning som
// är högst så här gammal.
constexpr double kAheadSeconds = 1.0;
constexpr double kAheadMaxAge = 0.5;
// Så mycket måste antalet trianglar i en del ändras (andel och absolut) för att
// dess `range` ska sättas om.
constexpr double kRangeChangeShare = 0.1;
constexpr uint64_t kRangeChangeMin = 64;
// Högst så här många byten av platser (två former var) per omgång, och ögats
// förflyttning (meter) som gör att ordningen prövas.
constexpr size_t kMaxSwaps = 150;
constexpr double kReorderMove = 0.02;
constexpr size_t kReorderPending = 8;
// Avståndsbanden för ordningen: första bandet till så här många meter, sedan
// växer de med så här mycket per band.
constexpr double kBandNear = 2.0;
constexpr double kBandRatio = 1.5;
constexpr int kBandMax = 64;
// Omordningen görs bara när minst så här många platser (och en sextondel av
// dem) står i fel band.
constexpr size_t kReorderMinMismatch = 4;
// Högst så här många bitar byggs om per genomgång för ett nytt K.
constexpr size_t kMaxKRebuilds = 24;
// Linjernas djupbias mot ögat (djupformatets steg respektive gånger ytans
// lutning): en linje i samma plan som en yta vinner över ytan.
constexpr float kLineDepthBias = -2.0f;
constexpr float kLineSlopeBias = -1.0f;
// Programmens kompileringsfel prövas så här ofta (sekunder) medan något visas;
// ett program kompileras första bilden det används.
constexpr double kProgramCheckSeconds = 1.0;

// Två trådar lämnas åt fönstret och grafikdrivrutinen.
unsigned build_threads() {
    const unsigned hw = std::thread::hardware_concurrency();
    return std::clamp(hw > 2 ? hw - 2 : 2u, 2u, 16u);
}

double seconds_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

void accumulate_bytes(PartBytes& to, const PartBytes& more) {
    to.ram += more.ram;
    to.gpu += more.gpu;
}

// Minnet per triangel i en del med lika många hörn som trianglar (ett hörn
// per triangel): meshen i RAM och på grafikkortet. Grundbildens triangelbudget
// och planens uppskattning räknas ur det.
PartBytes bytes_per_triangle() {
    constexpr size_t kN = 1024;
    MeshPart p;
    p.positions.resize(kN * 3);
    p.normals.resize(kN * 3);
    p.colors.resize(kN);
    p.indices.resize(kN * 3);
    PartBytes b = part_bytes(p);
    b.ram /= kN;
    b.gpu /= kN;
    return b;
}

std::string file_name(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string megabytes(size_t bytes) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.0f", static_cast<double>(bytes) / (1 << 20));
    return buf;
}

// RAM och grafikkort var för sig.
std::string ram_gpu(const PartBytes& b) {
    return "RAM " + megabytes(b.ram) + " MB, grafikkort " + megabytes(b.gpu) + " MB";
}

std::string millions(uint64_t n) {
    char buf[32];
    if (n >= 1'000'000)
        std::snprintf(buf, sizeof buf, "%.1f M", static_cast<double>(n) / 1e6);
    else if (n >= 1000)
        std::snprintf(buf, sizeof buf, "%.1f k", static_cast<double>(n) / 1e3);
    else
        std::snprintf(buf, sizeof buf, "%llu", static_cast<unsigned long long>(n));
    return buf;
}

std::string seconds_text(double s) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3f s", s);
    return buf;
}

// Ett pixeltal: heltal när det är ett, annars med en decimal.
std::string pixels_text(double px) {
    char buf[32];
    if (std::fabs(px - std::round(px)) < 0.05)
        std::snprintf(buf, sizeof buf, "%.0f", px);
    else
        std::snprintf(buf, sizeof buf, "%.1f", px);
    return buf;
}

// Budgeten med det fasta minnet avdraget; högst tre fjärdedelar går åt, så att
// något alltid återstår.
size_t after_fixed(size_t budget, size_t fixed) {
    return budget - std::min(fixed, budget / 4 * 3);
}

glaze::PropertyGrid::Row text_row(const std::string& name, const std::string& value) {
    glaze::PropertyGrid::Row r;
    r.name = name;
    r.value = value;
    r.readOnly = true;
    return r;
}

glaze::Box3 to_box3(const Box& b) {
    return glaze::Box3{{b.min[0], b.min[1], b.min[2]}, {b.max[0], b.max[1], b.max[2]}};
}

gfx::Color unpack(uint32_t c) {
    return gfx::Color{static_cast<std::uint8_t>(c >> 24), static_cast<std::uint8_t>(c >> 16),
                      static_cast<std::uint8_t>(c >> 8), static_cast<std::uint8_t>(c)};
}

gfx::Color to_color(const float rgba[4]) {
    auto channel = [](float v) {
        return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return gfx::Color{channel(rgba[0]), channel(rgba[1]), channel(rgba[2]), channel(rgba[3])};
}

// Kör `f` när räckvidden lämnas, hur den än lämnas.
template <class F>
struct AtExit {
    F f;
    ~AtExit() { f(); }
};
template <class F>
AtExit(F) -> AtExit<F>;

// Lådorna som visas innan de fasta bitarna är byggda: en instans av
// enhetslådan per låda.
glaze::Shape box_shape(const std::vector<NodeBox>& boxes, double floor) {
    std::vector<glaze::Instance> instances;
    for (const NodeBox& b : boxes) {
        double c[3];
        b.bounds.center(c);
        glaze::Instance in;
        in.at = {c[0], c[1], c[2]};
        in.scale = {static_cast<float>(std::max(b.bounds.max[0] - b.bounds.min[0], floor)),
                    static_cast<float>(std::max(b.bounds.max[1] - b.bounds.min[1], floor)),
                    static_cast<float>(std::max(b.bounds.max[2] - b.bounds.min[2], floor))};
        instances.push_back(in);
    }
    if (instances.empty()) return glaze::Shape{};
    glaze::MeshStyle style;
    style.color = gfx::Color{196, 198, 204, 255};
    return glaze::Shape::instances(
        glaze::Shape::box(glaze::Box3{{-0.5, -0.5, -0.5}, {0.5, 0.5, 0.5}}), std::move(instances),
        style);
}

// Rutnätens linjer och texter som former; byggs i en arbetstråd.
std::vector<glaze::Shape> grid_shapes(const Model& model) {
    std::vector<glaze::Shape> shapes;
    glaze::Stroke stroke;
    stroke.paint = gfx::Color{40, 90, 200, 255};
    stroke.width = glaze::px(1);
    glaze::TextStyle label;
    label.color = gfx::Color{40, 90, 200, 255};
    label.background = gfx::Color{255, 255, 255, 220};
    label.padding = glaze::px(2);
    shapes.reserve(model.grid_lines.size() * 3);
    for (const GridLineInfo& l : model.grid_lines) {
        const glaze::Point3 a{l.a[0], l.a[1], l.a[2]}, b{l.b[0], l.b[1], l.b[2]};
        shapes.push_back(glaze::Shape::line3(a, b, stroke));
        if (!l.label.empty()) {
            shapes.push_back(glaze::Shape::text3(a, l.label, label));
            shapes.push_back(glaze::Shape::text3(b, l.label, label));
        }
    }
    return shapes;
}

// En text i modellen: i världsstorlek, vänd mot kameran, med ett tak i
// pixlar.
glaze::Shape label_shape(const TextLabel& label) {
    glaze::TextStyle style;
    style.size = glaze::world(label.height);
    style.color = gfx::Color{30, 30, 30, 255};
    style.halo = gfx::Color{255, 255, 255, 200};
    style.maxSize = glaze::px(kLabelMaxPixels);
    return glaze::Shape::text3({label.at[0], label.at[1], label.at[2]}, label.text, style);
}

} // namespace

// Ett materials bild, avkodad en gång av den tråd som först behöver den.
struct TextureSlot {
    std::once_flag once;
    glaze::Bitmap bitmap;
    std::string error;
};

// Det arbetstrådarna delar för en öppen modell: modellen, läsarna (en per
// tråd som bygger) och bilderna ur materialen. Lever så länge något jobb
// håller den, också efter att en ny fil öppnats.
struct Session {
    std::shared_ptr<const Model> model;
    BuildLimits limits;   // utan filter; jobbet sätter sitt eget
    size_t reader_cache = kReaderCacheSmall;   // uppackad geometri per läsare

    std::mutex mutex;
    std::vector<std::unique_ptr<HNwd::GeometryReader>> readers;
    size_t readers_created = 0;
    std::unordered_map<uint32_t, std::shared_ptr<TextureSlot>> textures;

    // En läsare för den här tråden så länge jobbet pågår.
    std::unique_ptr<HNwd::GeometryReader> acquire(std::string& error) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!readers.empty()) {
                auto r = std::move(readers.back());
                readers.pop_back();
                return r;
            }
        }
        auto r = std::make_unique<HNwd::GeometryReader>();
        if (r->open(*model->doc, reader_cache) != HNwd::Status::Ok) {
            error = r->last_error();
            return nullptr;
        }
        std::lock_guard<std::mutex> lock(mutex);
        ++readers_created;
        return r;
    }
    void release(std::unique_ptr<HNwd::GeometryReader> r) {
        std::lock_guard<std::mutex> lock(mutex);
        readers.push_back(std::move(r));
    }

    // Materialets bild, avkodad en gång (PNG eller JPEG, nedskalad).
    std::shared_ptr<TextureSlot> texture(uint32_t material) {
        std::shared_ptr<TextureSlot> slot;
        {
            std::lock_guard<std::mutex> lock(mutex);
            std::shared_ptr<TextureSlot>& s = textures[material];
            if (!s) s = std::make_shared<TextureSlot>();
            slot = s;
        }
        std::call_once(slot->once, [&] {
            HNwd::Material mat;
            if (!model->doc->material(material, mat) || !mat.texture_bytes) {
                slot->error = "bilden ligger inte i filen";
                return;
            }
            DecodedImage img;
            if (!decode_texture(mat.texture_bytes, mat.texture_size, kMaxTextureSide, img,
                                slot->error))
                return;
            slot->bitmap = glaze::Bitmap(img.width, img.height, img.rgba.data(),
                                         static_cast<size_t>(img.width) * 4);
        });
        return slot;
    }
};

// Läsaren av egenskaper: bara arbetstråden i `prop_loader_` rör den.
struct PropState {
    std::shared_ptr<const HNwd::Document> tree;   // modellens dokument, med trädet läst
    std::vector<ModelInfo> models;
    HNwd::PropertyReader reader;
    bool opened = false;
    std::string open_error;
};

// Det inläsningen lämnar till UI-tråden.
struct Loaded {
    uint64_t generation = 0;
    LoadResult result;
    double seconds = 0;
    // Materialens bilder som saknas eller inte går att visa.
    uint64_t missing_textures = 0;
};

// Det planeringen av detaljträdet lämnar till UI-tråden, med schemaläggarens
// grundtillstånd färdigt och formerna för lådor och rutnät byggda.
struct Planned {
    uint64_t generation = 0;
    bool ok = false;
    std::shared_ptr<const LodTree> tree;
    LodTreeStats stats;
    std::unique_ptr<TileScheduler> sched;
    size_t tree_bytes = 0;
    glaze::Shape boxes_shape;
    std::shared_ptr<const std::vector<NodeBox>> box_list;
    std::vector<glaze::Shape> grid_shapes;
    size_t boxes = 0;
    double seconds = 0;
};

// Plockningens geometri: bitarnas geometri i en cache med tak i byte, senast
// använd släpps först. Arbetstrådarna läser och fyller den; ett lås skyddar den.
// En ny `epoch` (efter ett byte av modellfilter) gör att jobb som började före
// inte lägger sin geometri här.
struct PickCache {
    explicit PickCache(size_t capacity_bytes) : capacity(capacity_bytes) {}

    struct Entry {
        std::shared_ptr<const CellGeometry> geometry;
        size_t bytes = 0;
        float min_size = 0;   // fragmentstorleken geometrin byggdes till; 0 = hela biten
        std::list<uint32_t>::iterator pos;
    };
    using Map = std::unordered_map<uint32_t, Entry>;

    // Bitens geometri, byggd till vilken storlek som helst.
    std::shared_ptr<const CellGeometry> find(uint32_t tile) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = map.find(tile);
        if (it == map.end()) return nullptr;
        order.splice(order.begin(), order, it->second.pos);
        return it->second.geometry;
    }
    // Bitens geometri bara när den byggdes till `min_size`: delarna står då i
    // samma ordning som i den visade biten.
    std::shared_ptr<const CellGeometry> find(uint32_t tile, float min_size) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = map.find(tile);
        if (it == map.end() || it->second.min_size != min_size) return nullptr;
        order.splice(order.begin(), order, it->second.pos);
        return it->second.geometry;
    }
    bool contains(uint32_t tile) {
        std::lock_guard<std::mutex> lock(mutex);
        return map.count(tile) != 0;
    }
    uint64_t current_epoch() {
        std::lock_guard<std::mutex> lock(mutex);
        return epoch;
    }
    uint64_t current_generation() {
        std::lock_guard<std::mutex> lock(mutex);
        return generation;
    }
    // Lägger in geometrin om cachen inte tömts sedan `seen_epoch`. Det som
    // måste släppas för att få plats ges i `dropped`, så att anroparen kan
    // släppa det utanför låset.
    void insert(uint32_t tile, std::shared_ptr<const CellGeometry> geometry, size_t bytes,
                float min_size, uint64_t seen_epoch,
                std::vector<std::shared_ptr<const CellGeometry>>& dropped) {
        std::lock_guard<std::mutex> lock(mutex);
        if (seen_epoch != epoch || bytes > capacity) return;
        const auto old = map.find(tile);
        if (old != map.end()) {
            used -= old->second.bytes;
            order.erase(old->second.pos);
            dropped.push_back(std::move(old->second.geometry));
            map.erase(old);
        }
        while (used + bytes > capacity && !order.empty()) {
            const auto victim = map.find(order.back());
            order.pop_back();
            if (victim == map.end()) continue;
            used -= victim->second.bytes;
            dropped.push_back(std::move(victim->second.geometry));
            map.erase(victim);
        }
        order.push_front(tile);
        map[tile] = Entry{std::move(geometry), bytes, min_size, order.begin()};
        used += bytes;
        ++generation;
    }
    // Tömmer cachen; det som låg i den ges tillbaka för att släppas i en
    // arbetstråd.
    Map clear() {
        std::lock_guard<std::mutex> lock(mutex);
        Map old = std::move(map);
        map.clear();
        order.clear();
        used = 0;
        ++epoch;
        ++generation;
        return old;
    }

    const size_t capacity;

private:
    std::mutex mutex;
    Map map;
    std::list<uint32_t> order;   // senast använd först
    size_t used = 0;
    uint64_t epoch = 0, generation = 0;
};

// De visade bitarna och hur mycket av varje del som ritas, för
// plockningsjobben. UI-tråden skriver, arbetstrådarna läser.
struct PickShared {
    // Biten visas (eller visas i en ny version): hur mycket av varje del som
    // ritas och till vilken fragmentstorlek den är byggd.
    void add(uint32_t tile, std::vector<uint32_t> drawn, float min_size) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto [it, fresh] = tiles.try_emplace(tile);
        if (fresh) {
            it->second.pos = shown.size();
            shown.push_back(tile);
        }
        it->second.drawn = std::move(drawn);
        it->second.min_size = min_size;
    }
    void set_drawn(uint32_t tile, std::vector<uint32_t> drawn) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = tiles.find(tile);
        if (it != tiles.end()) it->second.drawn = std::move(drawn);
    }
    void remove(uint32_t tile) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = tiles.find(tile);
        if (it == tiles.end()) return;
        const size_t pos = it->second.pos;
        const uint32_t last = shown.back();
        shown[pos] = last;
        shown.pop_back();
        if (last != tile) tiles[last].pos = pos;
        tiles.erase(tile);
    }
    std::vector<uint32_t> shown_copy() {
        std::lock_guard<std::mutex> lock(mutex);
        return shown;
    }
    // Hur många trianglar av varje del som ritas och, om `min_size` ges, till
    // vilken storlek biten är byggd; false när biten inte visas.
    bool drawn_of(uint32_t tile, std::vector<uint32_t>& out, float* min_size = nullptr) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = tiles.find(tile);
        if (it == tiles.end()) return false;
        out = it->second.drawn;
        if (min_size) *min_size = it->second.min_size;
        return true;
    }

private:
    struct Slot {
        size_t pos = 0;
        std::vector<uint32_t> drawn;
        float min_size = 0;
    };
    std::mutex mutex;
    std::vector<uint32_t> shown;
    std::unordered_map<uint32_t, Slot> tiles;
};

// Ett plockningsjobbs svar.
struct PickAnswer {
    uint64_t generation = 0;
    PickPurpose purpose = PickPurpose::Click;
    PickHit hit;
    bool box_hit = false;          // strålen når en låda som visas före grundbilden
    bool cancelled = false;
    std::string error;             // det första felet när en bits geometri byggdes
    uint64_t tiles_tested = 0;
    double seconds = 0;
    uint64_t epoch = 0;            // för Target: vilken blick svaret hör till
};

// Marksökningens svar: ytans höjd under ögat.
struct GroundAnswer {
    uint64_t generation = 0;
    bool surface = false;
    double origin[3] = {0, 0, 0};
    double z = 0;
    uint64_t cache_generation = 0;
    double seconds = 0;
};

// Svaret när bitarna under ögat hållits i cachen.
struct PrimeAnswer {
    uint64_t generation = 0;
    double origin[3] = {0, 0, 0};
    uint64_t built = 0;
    bool more = false;             // fler bitar väntar på sin geometri
    std::string error;
};

// Postanvändningen och tiden att räkna den.
struct UseResult {
    RecordUse use;
    double seconds = 0;
};

double TimeSamples::p95() const {
    if (samples.empty()) return 0;
    std::vector<float> sorted = samples;
    const size_t rank = static_cast<size_t>(std::ceil(0.95 * static_cast<double>(sorted.size())));
    const size_t k = std::min(sorted.size(), std::max<size_t>(rank, 1)) - 1;
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(k),
                     sorted.end());
    return sorted[k];
}

void BuildStats::add(const BuildTimes& t) {
    ++jobs;
    fragments += t.fragments;
    flat_fragments += t.flat_fragments;
    vertices += t.vertices;
    triangles += t.triangles;
    read.add(t.read_seconds);
    convert.add(t.convert_seconds);
    mesh.add(t.mesh_seconds);
}

namespace {

// Meshen ur delen. `steps` är detaljnivåerna: de första `triangles` trianglarna
// (de största fragmenten står först) ritas så länge ögat är inom
// `max_distance` från meshens låda, och bortom den sista nivån ritas ingenting.
// Meshen har ingen CPU-kopia för ramverkets plockning: ramverket bygger inget
// sökträd för den och plockar aldrig mot den; plockningen är visarens egen.
// Linjedelen har inga normaler; hörnets andra ände och sida går i user0
// (xyz och w) för linjeprogrammet.
glaze::Mesh to_mesh(MeshPart& p, const double origin[3], const std::vector<LodStep>& steps) {
    glaze::MeshDesc d;
    const size_t vertices = p.positions.size() / 3;
    d.positions.resize(vertices);
    d.colors.resize(vertices);
    for (size_t v = 0; v < vertices; ++v) {
        d.positions[v] = {p.positions[v * 3], p.positions[v * 3 + 1], p.positions[v * 3 + 2]};
        d.colors[v] = unpack(p.colors[v]);
    }
    if (p.lines) {
        // Sidan är första talet av de två per hörn (det andra, änden, behövs
        // inte: hörnets egen ände är dess läge).
        const bool has_corner = p.corner.size() >= vertices * 2;
        const bool has_other = p.other.size() >= vertices * 3;
        d.user[0].resize(vertices);
        for (size_t v = 0; v < vertices; ++v)
            d.user[0][v] = {has_other ? p.other[v * 3] : 0.0f, has_other ? p.other[v * 3 + 1] : 0.0f,
                            has_other ? p.other[v * 3 + 2] : 0.0f,
                            has_corner ? p.corner[v * 2] : 0.0f};
    } else {
        d.normals.resize(vertices);
        for (size_t v = 0; v < vertices; ++v)
            d.normals[v] = {p.normals[v * 3], p.normals[v * 3 + 1], p.normals[v * 3 + 2]};
    }
    if (p.uvs.size() == vertices * 2) {
        d.uvs.resize(vertices);
        for (size_t v = 0; v < vertices; ++v) d.uvs[v] = {p.uvs[v * 2], p.uvs[v * 2 + 1]};
    }
    // Delens egna kopior behövs inte längre; minnet släpps innan meshen byggs.
    std::vector<float>().swap(p.positions);
    std::vector<float>().swap(p.normals);
    std::vector<float>().swap(p.uvs);
    std::vector<float>().swap(p.other);
    std::vector<float>().swap(p.corner);
    std::vector<uint32_t>().swap(p.colors);
    d.indices = std::move(p.indices);
    d.origin = {origin[0], origin[1], origin[2]};
    d.keepCpuCopy = false;
    const uint32_t triangles = static_cast<uint32_t>(d.indices.size() / 3);
    d.lods.reserve(steps.size());
    for (const LodStep& s : steps)
        d.lods.push_back(glaze::Lod{0, std::min(s.triangles, triangles) * 3, s.max_distance});
    return glaze::Mesh(std::move(d));
}

// Det jobbets tider som kommer ur geometrin.
void take_times(BuildTimes& t, const CellGeometry& g) {
    t.read_seconds = g.read_seconds;
    t.convert_seconds = g.convert_seconds;
    t.fragments = g.fragments;
    t.flat_fragments = g.flat_fragments;
    t.vertices = g.vertices;
    t.triangles = g.triangles;
}

// Bygger en bit i en arbetstråd: geometrin ur filen, sedan meshar och former.
// Detaljnivåerna i varje mesh bakas för en kamera med `pixels_per_radian`
// (redan med marginalen), så att varje fragment som är minst `min_pixels`
// stort ritas. Bara fragment med minst diagonalen `min_size` byggs (0 = alla).
// Fångar allt — ett undantag blir ett fel i svaret.
BuiltTile build_tile_job(const std::shared_ptr<Session>& session,
                         const std::shared_ptr<const LodTree>& lod, uint32_t tile,
                         const std::shared_ptr<const ModelFilter>& filter,
                         const glaze::Lighting& light, uint64_t generation, double min_pixels,
                         double pixels_per_radian, float min_size, std::stop_token stop) {
    BuiltTile b;
    b.tile = tile;
    b.min_size = min_size;
    b.generation = generation;
    b.baked_ppr = pixels_per_radian;
    try {
        std::string error;
        auto reader = session->acquire(error);
        if (!reader) {
            b.status = HNwd::Status::Corrupt;
            b.error = error;
            return b;
        }
        BuildLimits limits = session->limits;
        limits.filter = filter;
        limits.min_size = min_size;
        CellGeometry g;
        b.status = build_tile(*reader, *session->model, *lod, tile, limits, stop, g, b.error);
        session->release(std::move(reader));
        if (b.status != HNwd::Status::Ok) return b;

        take_times(b.times, g);
        b.triangles = g.triangles;
        b.segments = g.segments;
        b.segments_skipped = g.segments_skipped;
        b.points = g.points;
        b.texts = g.texts;
        b.undecoded = g.undecoded;
        b.rotated_textures = g.rotated_textures;
        b.labels = std::move(g.labels);
        b.parts.resize(g.parts.size());
        auto stat = std::make_shared<TileStatic>();
        stat->pinned = lod->tiles[tile].pinned;
        stat->max_size = lod->tiles[tile].max_size;
        stat->parts.resize(g.parts.size());
        for (size_t i = 0; i < g.parts.size(); ++i) {
            if (stop.stop_requested()) {
                b.status = HNwd::Status::Cancelled;
                return b;
            }
            MeshPart& p = g.parts[i];
            PartShape& out = b.parts[i];
            if (p.indices.empty()) continue;
            accumulate_bytes(b.bytes, part_bytes(p));
            const uint32_t triangles = static_cast<uint32_t>(p.triangle_count());
            out.triangles = triangles;
            out.drawn_triangles = triangles;
            // En liten del får bara den sista nivån: den döljer delen bortom
            // avståndet där dess största fragment är för litet.
            std::vector<LodStep> steps =
                lod_steps(p.ranges, triangles, min_pixels, pixels_per_radian,
                          triangles < kLodSplitTriangles ? 1 : kLodMaxSteps);
            // En fast bit är grundbilden: dess sista nivå ritas på alla avstånd.
            // Den ritas alltid åtminstone med sin sista nivå.
            if (lod->tiles[tile].pinned) {
                if (!steps.empty()) steps.back().max_distance = 0;
                out.floor_triangles =
                    steps.empty() ? triangles : std::min(steps.back().triangles, triangles);
            }
            out.ranges = std::make_shared<const std::vector<FragmentRange>>(std::move(p.ranges));
            out.lines = p.lines;
            // Visarens program belyser ytan lika från båda sidor; linjedelen
            // ritas av linjeprogrammet, utan belysning.
            glaze::MeshStyle style;
            if (p.lines) {
                style.program = kLineProgram;
                style.uniforms = line_uniforms(kLineWidthPx);
            } else {
                style.program = kLitProgram;
                style.uniforms = lit_uniforms(light, false, false);
            }
            if (!p.lines && p.material != HNwd::kNone) {
                const std::shared_ptr<TextureSlot> tex = session->texture(p.material);
                if (tex->bitmap.valid()) {
                    // Bilden upprepas i ett eget program; utan det ritar
                    // det inbyggda med kanten utdragen.
                    style.texture = tex->bitmap;
                    style.program = kRepeatProgram;
                    style.textures = {tex->bitmap};
                    style.uniforms = lit_uniforms(light, p.repeat_u, p.repeat_v);
                } else {
                    // Bilden går inte att avkoda: materialets färg, som stilens
                    // färg i det inbyggda programmet.
                    style.program.clear();
                    style.uniforms.clear();
                    style.color = to_color(p.base_color);
                    ++b.missing_textures;
                    if (b.error.empty()) b.error = tex->error;
                }
            }
            const auto mesh_start = std::chrono::steady_clock::now();
            glaze::Mesh mesh = to_mesh(p, g.origin, steps);
            b.times.mesh_seconds += seconds_since(mesh_start);
            if (!mesh.valid()) {
                b.status = HNwd::Status::Corrupt;
                b.error = "ritytan godtog inte meshen (detaljnivåerna eller indexen stämmer inte)";
                return b;
            }
            out.shape = glaze::Shape::mesh(std::move(mesh), {}, style);
            for (int a = 0; a < 3; ++a) out.origin[a] = g.origin[a];
            TileStatic::Part& sp = stat->parts[i];
            sp.ranges = out.ranges;
            sp.triangles = triangles;
            sp.floor_triangles = static_cast<uint32_t>(out.floor_triangles);
            sp.present = true;
        }
        b.stat = std::move(stat);
    } catch (const std::exception& e) {
        b.status = HNwd::Status::LimitExceeded;
        b.error = e.what();
    }
    return b;
}

// Trianglar en del ritar vid tröskeln `px` när biten mäts med `size_per_px`
// meter per px: de som består av fragment med minst så stor diagonal, och för
// en fast bit aldrig färre än dess sista nivå.
uint64_t triangles_for_part(const RangeList& ranges, uint32_t total, uint32_t floor_triangles,
                            bool pinned, double px, double size_per_px) {
    if (!ranges) return 0;
    uint64_t n = triangles_at_least(*ranges, total, static_cast<float>(px * size_per_px));
    if (pinned) n = std::max<uint64_t>(n, std::min<uint32_t>(floor_triangles, total));
    return n;
}

// Hur många trianglar av varje del som ritas, för plockningen.
std::vector<uint32_t> drawn_vector(const std::vector<PartShape>& parts) {
    std::vector<uint32_t> drawn(parts.size(), 0);
    for (size_t i = 0; i < parts.size(); ++i)
        if (parts[i].shape.kind() != glaze::Shape::Kind::None)
            drawn[i] = static_cast<uint32_t>(
                std::min<uint64_t>(parts[i].drawn_triangles, UINT32_MAX));
    return drawn;
}

// En byggd bit i bild för ritningens tröskel: dess fasta del och dess P.
struct DrawSnap {
    std::shared_ptr<const TileStatic> stat;
    double pixels = 0;
};

// Ritningens tröskel och belastningen vid den.
struct DrawResult {
    double wanted = 0;
    DrawLoad load;                 // belastningen vid max(minnets tröskel, wanted)
    bool reduce = false;
};

// Ritningens tröskel ur de byggda bitarna i bild. De fasta bitarna räknas
// alltid; de andra störst P först så länge deras P når tröskeln.
DrawResult compute_draw(std::vector<DrawSnap>& snaps, DrawGovernor& governor, double frame_ms,
                        bool deterministic, double threshold) {
    const auto mid = std::partition(snaps.begin(), snaps.end(),
                                    [](const DrawSnap& s) { return s.stat->pinned; });
    std::sort(mid, snaps.end(),
              [](const DrawSnap& a, const DrawSnap& b) { return a.pixels > b.pixels; });
    auto load_at = [&](double px) {
        uint64_t triangles = 0, draws = 0;
        auto add = [&](const DrawSnap& s) {
            const double size_per_px = static_cast<double>(s.stat->max_size) / s.pixels;
            for (const TileStatic::Part& p : s.stat->parts) {
                if (!p.present) continue;
                const uint64_t n = triangles_for_part(p.ranges, p.triangles, p.floor_triangles,
                                                      s.stat->pinned, px, size_per_px);
                if (n == 0) continue;
                triangles += n;
                ++draws;
            }
        };
        for (auto it = snaps.begin(); it != mid; ++it) add(*it);
        for (auto it = mid; it != snaps.end(); ++it) {
            if (it->pixels < px) break;
            add(*it);
        }
        DrawLoad load;
        load.triangles = triangles;
        load.draws = static_cast<uint32_t>(std::min<uint64_t>(draws, UINT32_MAX));
        return load;
    };
    DrawResult a;
    a.wanted = governor.next(load_at, frame_ms, deterministic);
    a.reduce = governor.reduce_quality();
    a.load = load_at(std::max(threshold, a.wanted));
    return a;
}

// Det ett plockningsjobb behöver ur fönstret, som värden.
struct PickJob {
    std::shared_ptr<Session> session;
    std::shared_ptr<const LodTree> lod;
    std::shared_ptr<PickCache> cache;
    std::shared_ptr<PickShared> shared;
    std::shared_ptr<const ModelFilter> filter;
    std::shared_ptr<const std::vector<NodeBox>> boxes;   // lådorna före grundbilden; kan vara null
    uint64_t epoch = 0;                                   // cachens epok när jobbet skapades
    uint64_t generation = 0;
};

// Minnet en cachad geometri tar.
size_t geometry_bytes(const CellGeometry& g) {
    size_t bytes = sizeof(CellGeometry);
    for (const MeshPart& p : g.parts)
        bytes += p.positions.capacity() * sizeof(float) + p.indices.capacity() * sizeof(uint32_t) +
                 p.ranges.capacity() * sizeof(FragmentRange) + sizeof(MeshPart);
    return bytes;
}

// Bitens geometri för plockning, byggd till fragmentstorleken `min_size` — den
// som den visade biten är byggd till, så att det som inte ritas inte träffas
// och delarna står i samma ordning som i den visade biten (det
// `PickLimits::drawn` indexeras efter). Ur cachen när posten där har samma
// storlek, annars byggd ur filen med en läsare ur sessionen (med `build`) och
// lagd i cachen i stället för den gamla posten. Bara det plockningen läser
// behålls (lägen, index och fragmentens intervall). null när biten inte finns i
// cachen utan `build`, när bygget avbröts eller när det föll (`error` säger då
// varför).
std::shared_ptr<const CellGeometry> tile_geometry(const PickJob& j, uint32_t tile,
                                                  std::stop_token stop, bool build,
                                                  float min_size, std::string& error) {
    if (std::shared_ptr<const CellGeometry> g = j.cache->find(tile, min_size)) return g;
    if (!build) return nullptr;
    auto reader = j.session->acquire(error);
    if (!reader) return nullptr;
    BuildLimits limits = j.session->limits;
    limits.filter = j.filter;
    limits.min_size = min_size;
    auto g = std::make_shared<CellGeometry>();
    std::string build_error;
    const HNwd::Status status =
        build_tile(*reader, *j.session->model, *j.lod, tile, limits, stop, *g, build_error);
    j.session->release(std::move(reader));
    if (status == HNwd::Status::Cancelled) return nullptr;
    if (status != HNwd::Status::Ok) {
        error = build_error.empty() ? "geometrin för en bit gick inte att bygga" : build_error;
        return nullptr;
    }
    g->labels.clear();
    g->labels.shrink_to_fit();
    for (MeshPart& p : g->parts) {
        std::vector<float>().swap(p.normals);
        std::vector<float>().swap(p.uvs);
        std::vector<uint32_t>().swap(p.colors);
        if (p.lines) {
            // Träffprovet prövar inte linjedelen; den står kvar som en tom del
            // så att delarnas platser stämmer.
            std::vector<float>().swap(p.positions);
            std::vector<float>().swap(p.other);
            std::vector<float>().swap(p.corner);
            std::vector<uint32_t>().swap(p.indices);
            std::vector<FragmentRange>().swap(p.ranges);
        }
    }
    std::vector<std::shared_ptr<const CellGeometry>> dropped;
    j.cache->insert(tile, g, geometry_bytes(*g), min_size, j.epoch, dropped);
    return g;
}

// Strålen når lådan inom `max_t`.
bool ray_reaches_box(const PickRay& r, const Box& b, double max_t) {
    double t0 = 0, t1 = max_t;
    for (int k = 0; k < 3; ++k) {
        const double d = r.dir[k];
        if (std::fabs(d) < 1e-12) {
            if (r.origin[k] < b.min[k] || r.origin[k] > b.max[k]) return false;
            continue;
        }
        double near_t = (b.min[k] - r.origin[k]) / d, far_t = (b.max[k] - r.origin[k]) / d;
        if (near_t > far_t) std::swap(near_t, far_t);
        t0 = std::max(t0, near_t);
        t1 = std::min(t1, far_t);
        if (t0 > t1) return false;
    }
    return true;
}

// En stråle mot de visade bitarna: bitarna längs strålen närmast först, var
// och en prövad mot sin geometri (hur mycket av delarna som ritas när
// `drawn`) tills en träff ligger närmare än nästa bit.
PickAnswer run_pick_job(const PickJob& j, const PickRay& ray, double max_distance,
                        const ClipState& clip, bool drawn, std::stop_token stop) {
    const auto t0 = std::chrono::steady_clock::now();
    PickAnswer a;
    a.generation = j.generation;
    std::vector<TileAlongRay> order;
    tiles_along_ray(*j.lod, j.shared->shown_copy(), ray, max_distance, order);
    PickHit best;
    for (const TileAlongRay& t : order) {
        if (stop.stop_requested()) {
            a.cancelled = true;
            return a;
        }
        if (best.hit && best.distance < t.enter) break;
        PickLimits limits;
        float min_size = 0;
        if (drawn && !j.shared->drawn_of(t.tile, limits.drawn, &min_size))
            continue;   // parkerad sedan dess
        if (clip.count > 0) limits.clip = &clip;
        std::string error;
        const std::shared_ptr<const CellGeometry> g =
            tile_geometry(j, t.tile, stop, true, min_size, error);
        if (!g) {
            if (!error.empty() && a.error.empty())
                a.error = "bit " + std::to_string(t.tile) + ": " + error;
            continue;
        }
        const PickHit h = pick_geometry(*g, ray, limits, best.hit ? best.distance : max_distance);
        ++a.tiles_tested;
        if (h.hit && (!best.hit || h.distance < best.distance)) best = h;
    }
    if (stop.stop_requested()) {
        a.cancelled = true;
        return a;
    }
    a.hit = best;
    if (!best.hit && j.boxes)
        for (const NodeBox& b : *j.boxes)
            if (ray_reaches_box(ray, b.bounds, max_distance)) {
                a.box_hit = true;
                break;
            }
    a.seconds = seconds_since(t0);
    return a;
}

// Ytan rakt under `origin`, bara ur cachen: en bit utan geometri där hoppas
// över, så att svaret blir "ingen yta" när ingenting finns än.
GroundAnswer run_ground_job(const PickJob& j, const double origin[3], double reach,
                            std::stop_token stop) {
    const auto t0 = std::chrono::steady_clock::now();
    GroundAnswer a;
    a.generation = j.generation;
    for (int k = 0; k < 3; ++k) a.origin[k] = origin[k];
    a.cache_generation = j.cache->current_generation();
    PickRay ray;
    for (int k = 0; k < 3; ++k) ray.origin[k] = origin[k];
    ray.dir[0] = ray.dir[1] = 0;
    ray.dir[2] = -1;
    std::vector<TileAlongRay> order;
    tiles_along_ray(*j.lod, j.shared->shown_copy(), ray, reach, order);
    PickHit best;
    for (const TileAlongRay& t : order) {
        if (stop.stop_requested()) return a;
        if (best.hit && best.distance < t.enter) break;
        const std::shared_ptr<const CellGeometry> g = j.cache->find(t.tile);
        if (!g) continue;
        const PickHit h = pick_geometry(*g, ray, PickLimits{}, best.hit ? best.distance : reach);
        if (h.hit && (!best.hit || h.distance < best.distance)) best = h;
    }
    a.surface = best.hit;
    if (best.hit) a.z = origin[2] - best.distance;
    a.seconds = seconds_since(t0);
    return a;
}

// Håller geometrin för de visade bitarna under ögat i cachen: bitar vars låda
// ligger inom `kGroundPrimeRadius` i sidled och når ner till sökvidden, de
// största först, högst `kGroundPrimeTiles` åt gången.
PrimeAnswer run_prime_job(const PickJob& j, const double eye[3], double reach,
                          std::stop_token stop) {
    PrimeAnswer a;
    a.generation = j.generation;
    for (int k = 0; k < 3; ++k) a.origin[k] = eye[k];
    struct Candidate {
        uint32_t tile;
        float size;
        float min_size;   // fragmentstorleken den visade biten är byggd till
    };
    std::vector<Candidate> wanted;
    for (const uint32_t t : j.shared->shown_copy()) {
        const Tile& tile = j.lod->tiles[t];
        const Box& b = tile.bounds;
        if (eye[0] < b.min[0] - kGroundPrimeRadius || eye[0] > b.max[0] + kGroundPrimeRadius ||
            eye[1] < b.min[1] - kGroundPrimeRadius || eye[1] > b.max[1] + kGroundPrimeRadius)
            continue;
        if (b.max[2] < eye[2] - reach || b.min[2] > eye[2] + 2) continue;
        if (j.cache->contains(t)) continue;
        float min_size = 0;
        std::vector<uint32_t> drawn;
        if (!j.shared->drawn_of(t, drawn, &min_size)) continue;   // parkerad sedan dess
        wanted.push_back({t, tile.max_size, min_size});
    }
    std::sort(wanted.begin(), wanted.end(),
              [](const Candidate& x, const Candidate& y) { return x.size > y.size; });
    a.more = wanted.size() > kGroundPrimeTiles;
    if (a.more) wanted.resize(kGroundPrimeTiles);
    for (const Candidate& c : wanted) {
        if (stop.stop_requested()) break;
        std::string error;
        if (tile_geometry(j, c.tile, stop, true, c.min_size, error))
            ++a.built;
        else if (!error.empty() && a.error.empty())
            a.error = "bit " + std::to_string(c.tile) + ": " + error;
    }
    return a;
}

} // namespace

// ---------------------------------------------------------------------------
// Urvalets tråd. TileScheduler och DrawGovernor är inte trådsäkra och rörs bara
// av den här tråden. UI-tråden lämnar meddelanden (kamerans ögonblicksbild,
// byggda jobbs svar, ogiltigförklaringar), alla med ett löpnummer, och får
// tillbaka färdiga svar genom `Worker::post`.

namespace {

// Avståndsbandet för ett avstånd i meter: det första upp till kBandNear, sedan
// växer varje band med kBandRatio.
int distance_band(double d) {
    if (!std::isfinite(d)) return kBandMax;
    if (!(d > kBandNear)) return 0;
    return std::min(1 + static_cast<int>(std::log(d / kBandNear) / std::log(kBandRatio)),
                    kBandMax);
}

} // namespace

// Kamerans ögonblicksbild för en mätning.
struct MeasureMsg {
    uint64_t seq = 0;
    ViewFrustum view;
    bool skip_ahead = false;      // kameran sattes om: förhandsbygget börjar om
    bool deterministic = false;   // ritningens tröskel är bara uppskattningen
    bool want_holes = false;      // räkna hålen (gången mäter dem)
    double frame_ms = 0;          // ritningens kostnad per bild; 0 = okänd
    size_t backlog = 0;           // operationer som väntar i fönstret
};

// Ett byggt jobbs svar.
struct SelBuilt {
    uint32_t tile = 0;
    float min_size = 0;
    PartBytes bytes;
    bool ok = false;
    bool cancelled = false;
    uint64_t install = 0;                       // vilken byggd version (för Install/Discard)
    std::shared_ptr<const TileStatic> stat;
    double baked_ppr = 0;
};

// Bitar som ska ogiltigförklaras (en modell dolts eller visats).
struct SelInvalidate {
    std::vector<uint32_t> tiles;
};

struct SelItem {
    uint64_t seq = 0;
    std::variant<SelBuilt, SelInvalidate> what;
};

// Det urvalet svarar med.
struct SelAnswer {
    uint64_t generation = 0;
    uint64_t seq = 0;                        // alla meddelanden t.o.m. det här löpnumret är hanterade
    std::vector<TileScheduler::Job> jobs;    // att starta nu
    std::vector<uint32_t> cancel;            // påbörjade jobb att avbryta
    std::vector<SelOp> ops;                  // i ordning
    bool has_order = false;
    std::vector<OrderEntry> order;           // de visade bitarna närmast ögat först, med band
    double draw_pixels = 3;
    DrawLoad load;
    bool reduce = false;
    SelStats stats;
    bool has_holes = false;
    size_t holes = 0;
    bool throttled = false;                  // visa, parkera och ritad andel hölls inne
    uint64_t k_rebuilds = 0;                 // bitar som ogiltigförklarats för ett nytt K
    double seconds = 0;                      // tiden genomgången tog i urvalets tråd
};

struct SelectorInit {
    std::shared_ptr<const LodTree> lod;
    std::unique_ptr<TileScheduler> sched;
    GovernorSettings governor;
    size_t max_inflight = 4;       // bygg som får pågå samtidigt
    double diagonal = 1;           // modellens storlek, meter
    std::chrono::steady_clock::time_point opened;
};

// Urvalets tillstånd. Inkorgen delas med UI-tråden under `mutex`; allt annat
// rör bara urvalets tråd.
struct SelectorCore {
    using Clock = std::chrono::steady_clock;

    SelectorCore(SelectorInit init, uint64_t generation_)
        : generation(generation_),
          lod(std::move(init.lod)),
          sched(std::move(init.sched)),
          governor(init.governor),
          max_inflight(init.max_inflight),
          diagonal(init.diagonal),
          opened(init.opened),
          tiles(lod->tiles.size()),
          draw_pixels(init.governor.min_pixels) {}

    // ---- UI-trådens sida: bara inkorgen.
    void post_measure(MeasureMsg m) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            // Bara den senaste kameran räknas, men det som begärts går inte förlorat.
            if (measure) {
                m.skip_ahead = m.skip_ahead || measure->skip_ahead;
                m.want_holes = m.want_holes || measure->want_holes;
            }
            measure = std::move(m);
        }
        cv.notify_one();
    }
    void post_item(SelItem item) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            inbox.push_back(std::move(item));
        }
        cv.notify_one();
    }

    // ---- Urvalets tråd. Kör tills `stop` begärs; varje genomgång levereras.
    void run(std::stop_token stop, const std::function<void(SelAnswer&&)>& deliver) {
        std::vector<SelItem> items;
        bool again = false;
        for (;;) {
            std::optional<MeasureMsg> m;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, stop, [&] { return again || !inbox.empty() || measure.has_value(); });
                if (stop.stop_requested()) return;
                items.assign(std::make_move_iterator(inbox.begin()),
                             std::make_move_iterator(inbox.end()));
                inbox.clear();
                m = std::move(measure);
                measure.reset();
            }
            const auto t0 = Clock::now();
            SelAnswer a;
            again = false;
            pass(items, m, a, again, stop);
            a.seconds = std::chrono::duration<double>(Clock::now() - t0).count();
            if (stop.stop_requested()) return;
            deliver(std::move(a));
        }
    }

private:
    // Urvalets bild av en byggd bit: vad fönstret har, och dess ritade andel.
    struct SelTile {
        std::shared_ptr<const TileStatic> stat;
        double baked_ppr = 0;
        bool installed = false;
        bool shown = false;
        uint32_t shown_pos = 0;
        std::vector<uint32_t> drawn;   // trianglar per del som fönstret ritar
    };

    double seconds() const { return std::chrono::duration<double>(Clock::now() - opened).count(); }

    void inconsistent(const char* what, uint32_t tile) {
        if (inconsistencies++ >= 10) return;
        std::fprintf(stderr,
                     "nwdview: schemaläggaren och fönstret är inte överens: %s för bit %u som inte "
                     "är byggd eller visad\n",
                     what, tile);
    }

    void mark_shown(uint32_t t) {
        SelTile& s = tiles[t];
        s.shown = true;
        s.shown_pos = static_cast<uint32_t>(shown_list.size());
        shown_list.push_back(t);
    }
    void mark_unshown(uint32_t t) {
        SelTile& s = tiles[t];
        if (!s.shown) return;
        const uint32_t pos = s.shown_pos;
        const uint32_t moved = shown_list.back();
        shown_list[pos] = moved;
        tiles[moved].shown_pos = pos;
        shown_list.pop_back();
        s.shown = false;
    }
    // Biten släpps: fönstret får veta, och bilden av den töms.
    void drop(SelAnswer& a, uint32_t t) {
        if (t >= tiles.size() || !tiles[t].installed) return;
        mark_unshown(t);
        tiles[t] = SelTile{};
        SelOp op;
        op.kind = SelOp::Drop;
        op.tile = t;
        a.ops.push_back(std::move(op));
    }

    // Hur många trianglar av varje del som ritas vid ritningens tröskel `draw_px`.
    std::vector<uint32_t> drawn_for(uint32_t t, double draw_px) const {
        const TileStatic& s = *tiles[t].stat;
        std::vector<uint32_t> d(s.parts.size(), 0);
        const double p = sched->pixels(t);
        for (size_t i = 0; i < s.parts.size(); ++i) {
            const TileStatic::Part& part = s.parts[i];
            if (!part.present) continue;
            // Utan mätning (0 px) ritas allt.
            if (!(p > 0)) {
                d[i] = part.triangles;
                continue;
            }
            const uint64_t n = triangles_for_part(part.ranges, part.triangles,
                                                  part.floor_triangles, s.pinned, draw_px,
                                                  static_cast<double>(s.max_size) / p);
            d[i] = static_cast<uint32_t>(std::min<uint64_t>(n, UINT32_MAX));
        }
        return d;
    }

    double current_draw_pixels() const { return std::max(draw_pixels, sched->threshold()); }

    void handle_built(const SelBuilt& b, SelAnswer& a) {
        if (b.tile >= tiles.size()) return;
        const double now = seconds();
        std::vector<uint32_t> evict;
        if (b.cancelled) {
            // Ett avbrutet jobb är inget fel: biten byggs igen när den behövs. Ett
            // avbrutet bygge av en bit som redan är byggd lämnar den gamla versionen
            // kvar; annars gäller biten inte längre.
            sched->tile_done(b.tile, b.min_size, PartBytes{}, false, now, evict);
            if (!sched->built(b.tile)) {
                sched->invalidate(b.tile);
                drop(a, b.tile);
            }
            for (const uint32_t t : evict) drop(a, t);
            return;
        }
        const bool keep = sched->tile_done(b.tile, b.min_size, b.bytes, b.ok, now, evict);
        // Ett bygge som föll eller inte ryms lämnar ingen ny version; en gammal
        // version som schemaläggaren inte längre räknar som byggd tas bort.
        if (!(b.ok && keep) && !sched->built(b.tile)) drop(a, b.tile);
        for (const uint32_t t : evict) drop(a, t);
        if (!b.ok) return;
        SelOp op;
        op.tile = b.tile;
        op.install = b.install;
        if (!keep) {
            op.kind = SelOp::Discard;
            a.ops.push_back(std::move(op));
            return;
        }
        SelTile& st = tiles[b.tile];
        st.stat = b.stat;
        st.baked_ppr = b.baked_ppr;
        st.installed = true;
        op.kind = SelOp::Install;
        op.shown = st.shown;
        if (st.shown) {
            // En ombyggnad av en visad bit: den nya versionen visas i samma platser.
            st.drawn = drawn_for(b.tile, current_draw_pixels());
            op.drawn = st.drawn;
            ++order_pending;
        } else {
            st.drawn.assign(st.stat->parts.size(), 0);
        }
        a.ops.push_back(std::move(op));
    }

    void handle_invalidate(const SelInvalidate& inv, SelAnswer& a) {
        for (const uint32_t t : inv.tiles) {
            if (t >= tiles.size()) continue;
            if (sched->invalidate(t)) drop(a, t);
        }
    }

    // Kameran om en sekund, ur förflyttningen sedan förra mätningen.
    std::optional<ViewFrustum> predict_ahead(double now) {
        std::optional<ViewFrustum> out;
        if (have_prev_eye && !skip_ahead) {
            const double dt = now - prev_eye_at;
            const double d[3] = {view.eye[0] - prev_eye[0], view.eye[1] - prev_eye[1],
                                 view.eye[2] - prev_eye[2]};
            const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (dt > 0.005 && dt < kAheadMaxAge && len > 1e-9) {
                // Farten gånger en sekund, aldrig längre än modellen är stor.
                const double lead = std::min(len / dt * kAheadSeconds, diagonal);
                ViewFrustum f = view;
                for (int k = 0; k < 3; ++k) f.eye[k] += d[k] / len * lead;
                out = f;
            }
        }
        for (int k = 0; k < 3; ++k) prev_eye[k] = view.eye[k];
        prev_eye_at = now;
        have_prev_eye = true;
        skip_ahead = false;
        return out;
    }

    // Bitar vars nivåer bakades för ett mindre K än kamerans byggs om, störst först.
    void stale_k(SelAnswer& a, bool& again) {
        const double ppr = view.pixels_per_radian();
        if (!(ppr > 0)) return;
        std::vector<std::pair<double, uint32_t>> stale;
        for (uint32_t t = 0; t < tiles.size(); ++t) {
            const SelTile& s = tiles[t];
            if (!s.installed || !(s.baked_ppr > 0) || ppr <= s.baked_ppr) continue;
            const double px = sched->pixels(t);
            if (px > 0) stale.emplace_back(px, t);
        }
        if (stale.empty()) return;
        const size_t n = std::min(stale.size(), kMaxKRebuilds);
        std::partial_sort(stale.begin(), stale.begin() + static_cast<std::ptrdiff_t>(n),
                          stale.end(),
                          [](const auto& x, const auto& y) { return x.first > y.first; });
        for (size_t i = 0; i < n; ++i) {
            if (sched->invalidate(stale[i].second))
                drop(a, stale[i].second);
            else
                tiles[stale[i].second].baked_ppr = 0;   // inget att bygga om: prövas inte igen
            ++a.k_rebuilds;
        }
        // Resten tas i nästa genomgång.
        if (stale.size() > n) {
            again = true;
            again_full = true;
        }
    }

    void start_jobs(SelAnswer& a) {
        const size_t inflight = sched->pending();
        if (inflight >= max_inflight) return;
        std::vector<uint32_t> evict;
        a.jobs = sched->next_build_jobs(max_inflight - inflight, evict);
        for (const uint32_t t : evict) drop(a, t);
    }

    // Ritningens tröskel ur de byggda bitarna i bild.
    void govern() {
        std::vector<DrawSnap> snaps;
        for (uint32_t t = 0; t < tiles.size(); ++t) {
            const SelTile& s = tiles[t];
            if (!s.stat || !sched->built(t)) continue;
            const double px = sched->pixels(t);
            if (!(px > 0)) continue;
            snaps.push_back({s.stat, px});
        }
        const DrawResult r = compute_draw(snaps, governor, deterministic ? 0 : frame_ms,
                                          deterministic, sched->threshold());
        draw_pixels = std::max(sched->threshold(), r.wanted);
        load = r.load;
        reduce = r.reduce;
    }

    // Visar och parkerar de bitar schemaläggaren pekar ut.
    void show_and_park(SelAnswer& a, bool& again) {
        const double px = current_draw_pixels();
        std::vector<uint32_t> show, park;
        sched->shown_changes(px, kMaxShownChanges, show, park);
        for (const uint32_t t : park) {
            if (t >= tiles.size() || !tiles[t].installed || !tiles[t].shown) {
                inconsistent("parkera", t);
                continue;
            }
            mark_unshown(t);
            tiles[t].drawn.assign(tiles[t].drawn.size(), 0);
            SelOp op;
            op.kind = SelOp::Park;
            op.tile = t;
            a.ops.push_back(std::move(op));
        }
        for (const uint32_t t : show) {
            if (t >= tiles.size() || !tiles[t].installed) {
                inconsistent("visa", t);
                continue;
            }
            if (tiles[t].shown) continue;
            mark_shown(t);
            tiles[t].drawn = drawn_for(t, px);
            SelOp op;
            op.kind = SelOp::Show;
            op.tile = t;
            op.drawn = tiles[t].drawn;
            a.ops.push_back(std::move(op));
            ++order_pending;
        }
        if (show.size() + park.size() >= kMaxShownChanges) again = true;
    }

    // Hur mycket av varje del som ritas: bara det som ändrats märkbart.
    void fit(SelAnswer& a) {
        const double px = current_draw_pixels();
        for (const uint32_t t : shown_list) {
            SelTile& st = tiles[t];
            const double p = sched->pixels(t);
            if (!(p > 0)) continue;
            const TileStatic& s = *st.stat;
            const double size_per_px = static_cast<double>(s.max_size) / p;
            std::vector<uint32_t> want;
            for (size_t i = 0; i < s.parts.size(); ++i) {
                const TileStatic::Part& part = s.parts[i];
                if (!part.present) continue;
                const uint64_t n = std::min<uint64_t>(
                    triangles_for_part(part.ranges, part.triangles, part.floor_triangles,
                                       s.pinned, px, size_per_px),
                    UINT32_MAX);
                const uint64_t cur = st.drawn[i];
                if (n == cur) continue;
                // Märkbart: mer än en tiondel (och en bit), eller hela delen
                // börjar eller slutar ritas.
                const uint64_t diff = n > cur ? n - cur : cur - n;
                const bool ends = (n == part.triangles) != (cur == part.triangles);
                const bool notable = ends || (diff >= kRangeChangeMin &&
                                              static_cast<double>(diff) >
                                                  kRangeChangeShare * static_cast<double>(cur));
                if (!notable) continue;
                if (want.empty()) want = st.drawn;
                want[i] = static_cast<uint32_t>(n);
            }
            if (want.empty()) continue;
            st.drawn = want;
            SelOp op;
            op.kind = SelOp::Fit;
            op.tile = t;
            op.drawn = std::move(want);
            a.ops.push_back(std::move(op));
        }
    }

    // De visade bitarna närmast ögat först, med avståndsband, när ögat flyttats
    // eller flera bitar visats sedan sist.
    void order(SelAnswer& a) {
        const double dx = view.eye[0] - order_eye[0], dy = view.eye[1] - order_eye[1],
                     dz = view.eye[2] - order_eye[2];
        const bool moved =
            !have_order_eye || dx * dx + dy * dy + dz * dz > kReorderMove * kReorderMove;
        if (!moved && order_pending < kReorderPending) return;
        for (int k = 0; k < 3; ++k) order_eye[k] = view.eye[k];
        have_order_eye = true;
        order_pending = 0;
        std::vector<uint32_t> list;
        sched->shown_order(view, list);
        // Bara de närmaste bitarna ordnas: de är det som djuptestet vinner mest på.
        if (list.size() > kOrderMaxTiles) list.resize(kOrderMaxTiles);
        a.has_order = true;
        a.order.reserve(list.size());
        for (const uint32_t t : list)
            a.order.push_back({t, distance_band(view.distance(lod->tiles[t].bounds))});
    }

    void select(SelAnswer& a, bool full, bool& again, const std::stop_token& stop) {
        if (full) {
            const double now = seconds();
            const std::optional<ViewFrustum> ahead = predict_ahead(now);
            sched->update(*lod, view, ahead ? &*ahead : nullptr, now);
            a.cancel = sched->take_cancellations();
            if (stop.stop_requested()) return;
            stale_k(a, again);
        }
        start_jobs(a);
        if (full) {
            if (stop.stop_requested()) return;
            govern();
        }
        const bool throttled = backlog > kOpBacklog;
        a.throttled = throttled;
        if (!throttled) {
            show_and_park(a, again);
            fit(a);
        }
        if (full) order(a);
        if (want_holes) {
            want_holes = false;
            a.has_holes = true;
            a.holes = sched->holes(2);
        }
    }

    void pass(std::vector<SelItem>& items, std::optional<MeasureMsg>& m, SelAnswer& a,
              bool& again, const std::stop_token& stop) {
        a.generation = generation;
        uint64_t seq = acked;
        bool built = false, invalidated = false;
        for (SelItem& it : items) {
            seq = std::max(seq, it.seq);
            if (const SelBuilt* b = std::get_if<SelBuilt>(&it.what)) {
                handle_built(*b, a);
                built = true;
            } else {
                handle_invalidate(std::get<SelInvalidate>(it.what), a);
                invalidated = true;
            }
        }
        items.clear();
        bool full = invalidated || again_full;
        again_full = false;
        if (m) {
            seq = std::max(seq, m->seq);
            view = m->view;
            have_view = true;
            skip_ahead = skip_ahead || m->skip_ahead;
            deterministic = m->deterministic;
            frame_ms = m->frame_ms;
            want_holes = want_holes || m->want_holes;
            backlog = m->backlog;
            full = true;
        } else if (built && sched->pending() == 0) {
            // När det sista jobbet är klart mäts allt om: uppskattningarna har
            // ersatts av det byggda, och en bit som inte fick plats kan få det nu.
            full = true;
        }
        acked = seq;
        a.seq = seq;
        if (have_view) select(a, full, again, stop);
        a.draw_pixels = current_draw_pixels();
        a.load = load;
        a.reduce = reduce;
        a.stats.threshold = sched->threshold();
        a.stats.resident = sched->resident();
        a.stats.built = sched->built_count();
        a.stats.pending = sched->pending();
        a.stats.shown = shown_list.size();
        a.stats.evictions = sched->evictions();
        a.stats.idle = sched->idle();
        // Fördröjningen sorteras högst två gånger per sekund.
        const double now = seconds();
        if (now - latency_at >= 0.5) {
            latency_at = now;
            latency = sched->latency();
        }
        a.stats.latency = latency;
    }

    // ---- delat med UI-tråden
    std::mutex mutex;
    std::condition_variable_any cv;
    std::deque<SelItem> inbox;
    std::optional<MeasureMsg> measure;

    // ---- bara urvalets tråd
    const uint64_t generation;
    std::shared_ptr<const LodTree> lod;
    std::unique_ptr<TileScheduler> sched;
    DrawGovernor governor;
    size_t max_inflight;
    double diagonal;
    Clock::time_point opened;
    std::vector<SelTile> tiles;
    std::vector<uint32_t> shown_list;
    uint64_t acked = 0;
    ViewFrustum view;
    bool have_view = false;
    bool skip_ahead = false;
    bool deterministic = false;
    bool want_holes = false;
    double frame_ms = 0;
    size_t backlog = 0;
    bool again_full = false;
    double draw_pixels;
    DrawLoad load;
    bool reduce = false;
    bool have_prev_eye = false;
    double prev_eye[3] = {0, 0, 0};
    double prev_eye_at = 0;
    bool have_order_eye = false;
    double order_eye[3] = {0, 0, 0};
    size_t order_pending = 0;
    double latency_at = -1;
    TileScheduler::Latency latency;
    uint32_t inconsistencies = 0;
};

ViewerWindow::ViewerWindow(ViewerOptions options)
    : glaze::AppWindow("nwdview", 1280, 800),
      options_(std::move(options)),
      build_pool_(build_threads()) {
    properties_.name = "properties";
    properties_.dock = glaze::Dock::Right;
    properties_.width = 380;
    properties_.setColumnSizing(0, glaze::ColumnSizing::AutoMin);

    model_list_.name = "models";
    model_list_.dock = glaze::Dock::Left;
    model_list_.width = 280;
    model_list_.addColumn("Modell", 150);
    model_list_.addColumn("Enhet", 70);
    model_list_.addColumn("Fragment", 60, glaze::Align::Right);
    model_list_.setShowChecks(true);
    model_list_.onCheck = [this] { toggle_model(model_list_.changedRow()); };
    model_list_.hide();   // visas när trädet har gett modellerna

    // Vyerna och nivåerna: en kolumn bredvid modellistan, dold när filen
    // saknar båda. Nivåerna står under vyerna.
    side_.name = "side";
    side_.dock = glaze::Dock::Left;
    side_.width = 240;
    levels_list_.name = "levels";
    levels_list_.dock = glaze::Dock::Bottom;
    levels_list_.addColumn("Nivå", 110);
    levels_list_.addColumn("Höjd", 70, glaze::Align::Right);
    levels_list_.addColumn("Rutnät", 60);
    views_tree_.name = "views";
    views_tree_.dock = glaze::Dock::Fill;
    views_tree_.addColumn("Vyer", 230);
    views_tree_.onChange = [this] { choose_view(); };
    side_.adopt(levels_list_);
    side_.adopt(views_tree_);
    side_.hide();

    scene_.name = "scene";
    scene_.dock = glaze::Dock::Fill;
    scene_.setFlat(true);
    scene_.setBackground(gfx::Color{236, 238, 241, 255});
    scene_.setPannable(true);
    scene_.setZoomable(true);
    scene_.setRotatable(true);
    scene_.setPitchable(true);
    // Filens kamera kan se rakt nedåt; orbiten får nå dit.
    scene_.setElevationRange(-90, 90);
    scene_.setMemoryBudget(kCanvasImageBudget);
    // Bildtakten: ritytan ritar det som ryms i tiden medan kameran rör sig
    // och fyller på resten när den står still. En bild som ska sparas utan
    // rörelse ritas hel.
    const bool whole_image = !options_.png.empty() && options_.walk_distance <= 0 &&
                             options_.spin <= 0 && !options_.tour;
    if (whole_image || options_.pacing_fps < 0) {
        scene_.setFramePacing(glaze::FramePacing::DrawAll);
    } else if (options_.pacing_fps > 0) {
        scene_.setFrameRate(options_.pacing_fps);
        scene_.setFramePacing(glaze::FramePacing::Fps);
    } else {
        scene_.setFramePacing(glaze::FramePacing::VSync);
    }
    light_.ambient = 0.5f;
    light_.diffuse = 0.6f;
    // Motljuset: det som vänder sig bort från ljuset lyses upp lite.
    light_.rim = 0.25f;
    light_.rimDirection = light_.direction;
    scene_.setLighting(light_);
    scene_.addProgram(repeat_program());
    scene_.addProgram(lit_program());
    scene_.addProgram(line_program());
    boxes_layer_ = scene_.addLayer("Lådor");
    opaque_layer_ = scene_.addLayer("Modell");
    textured_layer_ = scene_.addLayer("Bilder");
    // Bitarnas linjedelar: meshar som ritas efter ytorna, med djuptest och en
    // liten bias mot ögat så att en linje i samma plan som en yta syns ovanpå.
    line_layer_ = scene_.addLayer("Linjer");
    grid_layer_ = scene_.addLayer("Rutnät");
    transparent_layer_ = scene_.addLayer("Genomskinligt");
    labels_layer_ = scene_.addLayer("Texter");
    pick_layer_ = scene_.addLayer("Markering");
    for (const int layer :
         {boxes_layer_, opaque_layer_, textured_layer_, line_layer_, transparent_layer_})
        scene_.setLayerSnap(layer, false);
    glaze::RenderState see_through;
    see_through.depthWrite = false;
    scene_.setLayerRenderState(transparent_layer_, see_through);
    glaze::RenderState line_state = scene_.layerRenderState(line_layer_);
    line_state.depthBias = kLineDepthBias;
    line_state.slopeBias = kLineSlopeBias;
    scene_.setLayerRenderState(line_layer_, line_state);

    scene_.onMouseDown = [this](glaze::Widget*, glaze::CanvasMouseArgs& e) {
        if (e.button == gfx::MouseButton::Left) press_ = e.local;
        if (!walking_) return;
        // Gångläget tar trycken: ingen orbit; ett drag med en knapp nere
        // vrider blicken.
        e.handled = true;
        scene_.focus();
        if (!looking_) {
            looking_ = true;
            look_button_ = e.button;
            look_last_ = e.local;
        }
    };
    scene_.onMouseCancel = [this](glaze::Widget*, glaze::MouseArgs&) { looking_ = false; };
    scene_.onMouseWheel = [this](glaze::Widget*, glaze::CanvasMouseArgs& e) {
        if (!walking_) return;
        e.handled = true;
        if (e.precise) {
            // Styrplattans tvåfingersdrag vrider blicken: i sidled svänger
            // det, uppåt och nedåt lutar det. Bilden följer fingrarna.
            WalkInput in;
            in.look_dx = static_cast<double>(e.wheelX) * gfx::kPrecisePxPerRow;
            in.look_dy = -static_cast<double>(e.wheelY) * gfx::kPrecisePxPerRow;
            if (walk_step(walk_, in, walk_settings_, {})) apply_walk_camera();
            return;
        }
        // Hjulet ändrar gångfarten.
        int steps = static_cast<int>(std::lround(e.wheelY));
        if (steps == 0 && e.wheelY != 0) steps = e.wheelY > 0 ? 1 : -1;
        if (steps == 0) return;
        walk_.speed = walk_speed_after(walk_.speed, steps, walk_settings_);
        update_status();
    };
    scene_.onKeyDown = [this](glaze::Widget*, glaze::KeyArgs& e) {
        if (!walking_) return;
        // En gångtangent som tas håller ritytan tickande så länge den hålls.
        if (walk_key(e.key)) e.handled = true;
        // Plus och minus ändrar gångfarten, ett steg per tryck och upprepning.
        using K = gfx::Key;
        const bool faster = e.key == K::Plus || e.key == K::Equals || e.key == K::KP_Add;
        const bool slower = e.key == K::Minus || e.key == K::KP_Subtract;
        if (faster || slower) {
            e.handled = true;
            walk_.speed = walk_speed_after(walk_.speed, faster ? 1 : -1, walk_settings_);
            update_status();
        }
    };
    scene_.onTick = [this](glaze::Widget*, glaze::TickArgs& e) { walk_tick(e.seconds); };
    scene_.onMouseUp = [this](glaze::Widget*, glaze::CanvasMouseArgs& e) {
        if (looking_ && e.button == look_button_) looking_ = false;
        const double dx = e.local.x - press_.x, dy = e.local.y - press_.y;
        const double limit = style().dragThreshold;
        // Ett drag vred kameran; bara ett klick väljer.
        if (e.button != gfx::MouseButton::Left || dx * dx + dy * dy > limit * limit) return;
        pick(e.local);
    };
    scene_.onMouseMove = [this](glaze::Widget*, glaze::CanvasMouseArgs& e) {
        if (looking_) {
            // Blicken följer musen direkt; tangenterna går i ticken.
            WalkInput in;
            in.look_dx = e.local.x - look_last_.x;
            in.look_dy = e.local.y - look_last_.y;
            look_last_ = e.local;
            if (walk_step(walk_, in, walk_settings_, {})) apply_walk_camera();
            return;
        }
        // Högst var 50:e ms; den sista positionen tas när tiden gått.
        pending_coordinate_ = e.local;
        const auto now = Clock::now();
        if (now - last_coordinate_ >= std::chrono::milliseconds(50)) {
            update_coordinate(e.local);
        } else if (!coordinate_timer_.isRunning()) {
            coordinate_timer_.start();
        }
    };
    scene_.onCameraChange = [this] {
        fit_orbit_near();
        request_update();
    };
    scene_.onResize = [this] { request_update(); };

    coordinate_timer_.setInterval(50);
    coordinate_timer_.setSingleShot(true);
    coordinate_timer_.onTick = [this] { update_coordinate(pending_coordinate_); };
    update_timer_.setInterval(100);
    update_timer_.setSingleShot(true);
    update_timer_.onTick = [this] { remeasure(); };
    // Prövar om kameran har stannat; körs bara medan den rört sig.
    settle_timer_.setInterval(100);
    settle_timer_.onTick = [this] { settle(); };
    // Köerna (operationerna ur urvalet, nytt snitt, ogiltigförklaringar) tas en
    // omgång åt gången.
    slice_timer_.setInterval(10);
    slice_timer_.setSingleShot(true);
    slice_timer_.onTick = [this] { run_queues(); };
    // Texterna ändras högst två gånger per sekund.
    label_timer_.setInterval(kLabelFlushMs);
    label_timer_.setSingleShot(true);
    label_timer_.onTick = [this] { flush_labels(false); };
    // Platshållaren: en gemensam mesh med en enda triangel och ritad andel noll,
    // som platser utan del står med. Ritas aldrig och får ingen uppladdning av
    // någon betydelse.
    {
        glaze::MeshDesc d;
        d.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
        d.indices = {0, 1, 2};
        d.keepCpuCopy = false;
        glaze::MeshStyle style;
        style.range = glaze::IndexRange{0, 0};
        placeholder_ = glaze::Shape::mesh(glaze::Mesh(std::move(d)), {}, style);
    }

    status_.name = "status";
    unit_.name = "unit";
    coordinate_.name = "coordinate";
    progress_.name = "progress";
    progress_.width = 160;
    progress_.hide();
    statusBar.show();
    statusBar.adopt(status_);
    statusBar.addSpacer();
    statusBar.adopt(progress_);
    statusBar.adopt(unit_);
    statusBar.adopt(coordinate_);

    root().adopt(model_list_);
    root().adopt(side_);
    root().adopt(properties_);
    root().adopt(scene_);

    open_dialog_.setFilters({{"Navisworks", "*.nwd;*.nwc"}, {"Alla filer", "*"}});

    glaze::AppWindow::onClose = [this] {
        note_profile("hela körningen");
        note_stalls("hela körningen");
    };
    glaze::AppWindow::onLoad = [this] {
        // Systemets lista över grafikkort tar tiotals ms och går bara i
        // UI-tråden: den läses här, innan första bilden, och inte när en fil
        // öppnats.
        {
            const auto t0 = Clock::now();
            gpu_info_ = window_gpu();
            have_gpu_info_ = true;
            if (options_.profile) {
                double& worst = stalls_["grafikkort (start)"];
                worst = std::max(
                    worst, std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
            }
        }
        glaze::Menu& file = menuBar.addMenu("Arkiv");
        file.addItem("Öppna…").onClick = [this] { choose_file(); };
        file.addItem("Visa allt").onClick = [this] { show_all(); };
        file.addSeparator();
        file.addItem("Avsluta").onClick = [this] { close(); };
        glaze::Menu& show = menuBar.addMenu("Visa");
        show.addCheck("Rutnät", true).onClick = [this] {
            grid_visible_ = !grid_visible_;
            scene_.setLayerVisible(grid_layer_, grid_visible_);
        };
        show.addSeparator();
        glaze::MenuItem& walk = show.addCheck("Gångläge", false);
        walk.shortcut = {gfx::Key::F2, 0};
        walk.onClick = [this] { set_walking(!walking_); };
        show.addCheck("Håll höjden över ytan", options_.follow_ground).onClick = [this] {
            walk_.follow_ground = !walk_.follow_ground;
            options_.follow_ground = walk_.follow_ground;
            menuBar.setItemChecked({"Visa", "Håll höjden över ytan"}, walk_.follow_ground);
            update_status();
        };
        show.addSeparator();
        // Den valda vyns snittplan klipper modellen.
        show.addCheck("Snitt", options_.clip).onClick = [this] {
            options_.clip = !options_.clip;
            menuBar.setItemChecked({"Visa", "Snitt"}, options_.clip);
            apply_clip();
        };

        if (!options_.png.empty()) {
            png_timer_.setInterval(200);
            png_timer_.onTick = [this] { png_tick(); };
            png_timer_.start();
        }
        if (options_.path.empty())
            status_.setText("Arkiv → Öppna… väljer en fil.");
        else
            open_file(options_.path);
    };
}

ViewerWindow::~ViewerWindow() {
    // Det som pågår ska inte fortsätta i onödan medan fönstret rivs; den som
    // redan kör väntas in av sin worker.
    loader_.cancel();
    builder_.cancel();
    boxer_.cancel();
    tree_loader_.cancel();
    prop_loader_.cancel();
    click_worker_.cancel();
    coord_worker_.cancel();
    ground_worker_.cancel();
    prime_worker_.cancel();
    trash_worker_.cancel();
    selector_worker_.cancel();
    use_loader_.cancel();
}

void ViewerWindow::stall(const char* what, Clock::time_point since) {
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - since).count();
    if (options_.profile) {
        double& worst = stalls_[what];
        worst = std::max(worst, ms);
    }
    // Efter startvyn ska inget i UI-tråden ta märkbar tid: stoppen räknas, och
    // det första som är för långt står på stderr.
    if (!idle_noted_) return;
    if (ms > kStallCountMs) ++stalls_over_;
    if (ms > stall_worst_ms_) {
        stall_worst_ms_ = ms;
        stall_worst_name_ = what;
    }
    if (ms > kStallErrorMs && !stall_error_written_) {
        stall_error_written_ = true;
        std::fprintf(stderr,
                     "nwdview: UI-tråden var upptagen %.1f ms i \"%s\" efter startvyn "
                     "(gränsen är %.0f ms)\n",
                     ms, what, kStallErrorMs);
    }
}

void ViewerWindow::check_programs() {
    const auto now = Clock::now();
    if (std::chrono::duration<double>(now - programs_checked_).count() < kProgramCheckSeconds)
        return;
    programs_checked_ = now;
    for (const char* name : {kRepeatProgram, kLitProgram, kLineProgram}) {
        const std::string error = scene_.programError(name);
        if (error.empty() || !programs_reported_.insert(name).second) continue;
        std::fprintf(stderr, "nwdview: programmet %s kompilerar inte: %s\n", name, error.c_str());
    }
}

void ViewerWindow::note_stalls(const char* when) {
    if (!options_.profile) return;
    // Det längsta UI-tråden var upptagen utan att vänta: ett pumpvarvs
    // händelser, postade funktioner, timrar och stängningar, och den längsta
    // bilden i fönstret.
    float iteration = 0, posted = 0, events = 0, timers = 0;
    for (const glaze::PumpProfile& p : glaze::profiler().pump()) {
        iteration = std::max(iteration, p.events + p.posted + p.timers + p.commits + p.closes);
        posted = std::max(posted, p.postedMax);
        events = std::max(events, p.events);
        timers = std::max(timers, p.timers);
    }
    glaze::FrameProfile longest;
    for (const glaze::FrameProfile& f : glaze::profiler().frames(*this))
        if (f.total > longest.total) longest = f;
    auto phase = [&](glaze::ProfilePhase p) { return longest.ms[size_t(p)]; };
    auto count = [&](glaze::ProfileCount c) {
        return static_cast<unsigned long long>(longest.count[size_t(c)]);
    };
    char buf[768];
    std::snprintf(buf, sizeof buf,
                  "UI-tråden %s: längsta pumpvarv utan väntan %.1f ms (händelser %.1f, "
                  "timrar %.1f), längsta postade funktion %.1f ms, längsta bild %.1f ms "
                  "(bild nr %llu%s: innehåll %.1f, ritytan %.1f, kompilering %.1f, present %.1f + "
                  "väntan %.1f, gpu %.1f; %llu byte uppladdade, %llu ritanrop)",
                  when, iteration, events, timers, posted, longest.total,
                  static_cast<unsigned long long>(longest.frame),
                  longest.outsidePump ? ", utanför pumpen" : "",
                  phase(glaze::ProfilePhase::Content),
                  phase(glaze::ProfilePhase::CanvasDraw) +
                      phase(glaze::ProfilePhase::CanvasPrepare),
                  phase(glaze::ProfilePhase::Compile), phase(glaze::ProfilePhase::Present),
                  phase(glaze::ProfilePhase::PresentWait), phase(glaze::ProfilePhase::Gpu),
                  count(glaze::ProfileCount::BytesUploaded), count(glaze::ProfileCount::DrawCalls));
    std::string text = buf;
    if (!stalls_.empty()) {
        text += "; visarens fortsättningar, längst:";
        for (const auto& [name, ms] : stalls_) {
            char part[96];
            std::snprintf(part, sizeof part, " %s %.1f ms", name.c_str(), ms);
            text += part;
        }
    }
    note(text);
    char after[256];
    if (idle_noted_)
        std::snprintf(after, sizeof after,
                      "UI-tråden %s efter startvyn: %llu stopp över %.0f ms; längsta %.1f ms (%s)",
                      when, static_cast<unsigned long long>(stalls_over_), kStallCountMs,
                      stall_worst_ms_, stall_worst_name_.empty() ? "-" : stall_worst_name_.c_str());
    else
        std::snprintf(after, sizeof after, "UI-tråden %s: startvyn blev inte klar, inga stopp räknade",
                      when);
    note(after);
}

void ViewerWindow::note(const std::string& what) {
    const double ms =
        std::chrono::duration<double, std::milli>(Clock::now() - opened_at_).count();
    char buf[64];
    std::snprintf(buf, sizeof buf, "%9.0f ms  ", ms);
    timings_ += buf + what + "\n";
}

void ViewerWindow::choose_file() {
    if (open_dialog_.run(*this) == glaze::DialogResult::OK) open_file(open_dialog_.path());
}

void ViewerWindow::reset_scene() {
    ++generation_;
    loader_.cancel();
    builder_.cancel();
    boxer_.cancel();
    tree_loader_.cancel();
    prop_loader_.cancel();
    click_worker_.cancel();
    coord_worker_.cancel();
    ground_worker_.cancel();
    prime_worker_.cancel();
    stop_selector();
    use_loader_.cancel();
    model_.reset();
    session_.reset();
    lod_.reset();
    lod_stats_ = LodTreeStats{};
    lod_bytes_ = 0;
    // Meshar och byggda versioner dör i en arbetstråd.
    {
        std::deque<Retired> retired = std::move(retired_);
        retired_.clear();
        trash_worker_.run(
            [views = std::move(views_), pending = std::move(pending_),
             retired = std::move(retired)]() mutable {
                views.clear();
                pending.clear();
                retired.clear();
            },
            [](glaze::threads::Result<void>&) {});
        views_.clear();
        pending_.clear();
    }
    counted_.clear();
    label_tiles_.clear();
    shown_tiles_ = 0;
    ops_.clear();
    ops_next_ = 0;
    ops_count_ = 0;
    order_.clear();
    order_stage_ = 3;
    stats_ = SelStats{};
    stats_.threshold = options_.min_pixels;
    sent_seq_ = acked_seq_ = 0;
    answer_calm_ = false;
    sel_throttled_ = false;
    clip_queue_.clear();
    clip_next_ = 0;
    rebuild_queue_.clear();
    label_pending_.clear();
    label_on_scene_.clear();
    label_timer_.stop();
    stalls_over_ = 0;
    stall_worst_ms_ = 0;
    stall_worst_name_.clear();
    stall_error_written_ = false;
    box_list_.reset();
    pick_shared_ = std::make_shared<PickShared>();
    if (pick_cache_) {
        // Det som låg i cachen släpps i en arbetstråd.
        trash_worker_.run([old = pick_cache_->clear()]() mutable { old.clear(); },
                          [](glaze::threads::Result<void>&) {});
        pick_cache_.reset();
    }
    click_busy_ = coord_busy_ = false;
    coord_pending_.reset();
    pick_error_.clear();
    pick_jobs_ = 0;
    pick_ms_sum_ = pick_ms_max_ = 0;
    ground_ = Ground{};
    ground_busy_ = ground_want_ = ground_force_ = false;
    prime_busy_ = false;
    prime_dirty_ = true;
    prime_have_ = false;
    ground_cache_gen_ = 0;
    draw_reduce_ = false;
    cost_frame_ = 0;
    pinned_seen_.clear();
    pinned_pending_ = 0;
    base_noted_ = false;
    inconsistencies_ = 0;
    have_view_ = false;
    draw_pixels_ = draw_pixels_max_ = options_.min_pixels;
    last_load_ = DrawLoad{};
    aa_reductions_ = jobs_cancelled_ = slots_reordered_ = k_rebuilds_ = size_rebuilds_ = 0;
    job_stops_.clear();
    restore_quality();
    tree_.reset();
    models_.clear();
    props_.reset();
    tree_started_ = false;
    tree_error_.clear();
    tree_progress_.clear();
    props_busy_ = false;
    hidden_models_.clear();
    filter_.reset();
    load_failed_ = false;
    build_error_.clear();
    failed_tiles_ = 0;
    shown_triangles_ = 0;
    points_ = texts_ = undecoded_ = segments_skipped_ = missing_textures_ = rotated_textures_ = 0;
    labels_shown_ = 0;
    labels_dropped_ = 0;
    boxes_cleared_ = false;
    idle_noted_ = false;
    selected_ = SIZE_MAX;
    budget_ = PartBytes{};
    budget_shared_ = false;
    gpu_name_.clear();
    reader_bytes_ = tree_bytes_ = pick_cache_bytes_ = 0;
    tile_stats_ = BuildStats{};
    use_ = RecordUse{};
    use_seconds_ = 0;
    use_busy_ = use_done_ = use_noted_ = false;
    for (std::vector<int>* f : {&free_opaque_, &free_transparent_, &free_textured_, &free_lines_,
                                &free_labels_})
        f->clear();
    for (std::vector<SlotOwner>* o :
         {&opaque_owner_, &transparent_owner_, &textured_owner_, &line_owner_, &unused_owner_})
        o->clear();
    for (const int layer : {boxes_layer_, opaque_layer_, textured_layer_, line_layer_,
                            transparent_layer_, labels_layer_, pick_layer_, grid_layer_})
        scene_.setShapes(layer, {});
    properties_.setRows({});
    model_list_.setRows({});
    model_list_.hide();
    view_nodes_.clear();
    views_tree_.setNodes({});
    levels_list_.setRows({});
    side_.hide();
    view_clip_ = ClipState{};
    clip_error_.clear();
    coordinate_.setText("");
    unit_.setText("");
}

void ViewerWindow::fail_load(const std::string& text) {
    load_failed_ = true;
    std::fprintf(stderr, "nwdview: %s\n", text.c_str());
    status_.setText(text);
    properties_.setRows({text_row("Fel", text)});
    progress_.hide();
    setUseWaitCursor(false);
}

void ViewerWindow::open_file(const std::string& path) {
    reset_scene();
    opened_at_ = Clock::now();
    timings_.clear();
    stalls_.clear();
    setTitle("nwdview — " + file_name(path));
    status_.setText(file_name(path) + ": öppnas…");
    progress_.setIndeterminate(true);
    progress_.show();
    setUseWaitCursor(true);

    const uint64_t gen = generation_;
    glaze::threads::Worker* worker = &loader_;
    loader_.run(
        [this, worker, path, gen](std::stop_token stop) -> Loaded {
            // `this` används här bara för att nå `worker`s mellanbesked; de
            // körs i UI-tråden.
            const auto begin = Clock::now();
            auto last = begin - std::chrono::seconds(1);
            Loaded out;
            out.generation = gen;
            out.result = load_model(path, [&](const char* step, uint64_t done, uint64_t total) {
                const auto now = Clock::now();
                if (now - last >= std::chrono::milliseconds(50)) {
                    last = now;
                    std::string text = step;
                    worker->post(stop, [this, text, done, total] {
                        if (total > 0) {
                            progress_.setIndeterminate(false);
                            progress_.setMax(static_cast<float>(total));
                            progress_.setValue(static_cast<float>(done));
                        } else {
                            progress_.setIndeterminate(true);
                        }
                        status_.setText(text + "…");
                    });
                }
                return !stop.stop_requested();
            });
            // Materialens bilder som saknas eller inte går att visa räknas
            // här och inte i UI-tråden.
            if (out.result.model) {
                const HNwd::Document& doc = *out.result.model->doc;
                for (size_t i = 0; i < doc.material_count() && !stop.stop_requested(); ++i) {
                    HNwd::Material mat;
                    if (doc.material(i, mat) && mat.has_texture &&
                        !(mat.texture_bytes &&
                          texture_format_known(mat.texture_bytes, mat.texture_size)))
                        ++out.missing_textures;
                }
            }
            out.seconds = std::chrono::duration<double>(Clock::now() - begin).count();
            return out;
        },
        [this](glaze::threads::Result<Loaded>& r) {
            if (!r) {
                fail_load("Filen kunde inte läsas: " + r.error());
                return;
            }
            on_loaded(*r);
        });
}

void ViewerWindow::on_loaded(Loaded& loaded) {
    const auto t0 = Clock::now();
    AtExit guard{[&] { stall("inläst", t0); }};
    if (loaded.generation != generation_) return;
    if (!loaded.result.model) {
        fail_load(loaded.result.error);
        return;
    }
    note("modellen läst (" + std::to_string(loaded.result.model->fragment_count) +
         " fragment, " + std::to_string(loaded.result.model->nodes.size()) +
         " noder i det rumsliga indexet)");
    model_ = loaded.result.model;

    // Budgeterna: förvalen ur maskinens minne, grafikkortets minne och
    // dokumentets storlek, och flaggorna ovanpå.
    if (!have_gpu_info_) {
        gpu_info_ = window_gpu();
        have_gpu_info_ = true;
    }
    const GpuInfo& gpu = gpu_info_;
    gpu_name_ = gpu.name;
    {
        SystemMemory sys;
        std::string error;
        Budgets budgets;
        if (system_memory(sys, error)) {
            budgets = default_budgets(sys, gpu.memory, gpu.shared, model_->memory_bytes);
        } else {
            std::fprintf(stderr, "nwdview: maskinens minne gick inte att läsa: %s\n",
                         error.c_str());
            budgets.budget.ram = budgets.budget.gpu = kFallbackBudget;
            budgets.shared = true;
        }
        if (options_.budget_mb) budgets.budget.ram = options_.budget_mb << 20;
        if (options_.gpu_budget_mb) {
            // Ett uttryckligt grafikkortsminne gör budgetarna skilda.
            budgets.budget.gpu = options_.gpu_budget_mb << 20;
            budgets.shared = false;
        }
        budget_ = budgets.budget;
        budget_shared_ = budgets.shared;
    }
    // Ritytans egen budget räknar allt den håller, också bitarnas meshar på
    // grafikkortet och i RAM. Urvalet här avgör vad som är inladdat, så
    // ritytans budget rymmer hela visarens och bildernas: den vräker då inget
    // som urvalet räknar som inladdat.
    scene_.setMemoryBudget(budget_.ram + (budget_shared_ ? 0 : budget_.gpu) + kCanvasImageBudget);

    session_ = std::make_shared<Session>();
    session_->model = model_;
    session_->limits.uv = options_.uv;
    if (!options_.lines) session_->limits.max_segments = 0;
    // Läsarnas cache är fast minne: en läsare per byggtråd och per
    // plockningsjobb som läser ur filen.
    session_->reader_cache =
        budget_.ram >= kReaderCacheThreshold ? kReaderCacheLarge : kReaderCacheSmall;
    reader_bytes_ = session_->reader_cache * (build_pool_.threadCount() + kPickReaders);
    // Plockningens geometricache är också fast minne, högst en fjärdedel av
    // RAM-budgeten.
    pick_cache_bytes_ = std::min(options_.pick_cache_mb << 20, budget_.ram / 4);
    pick_cache_ = std::make_shared<PickCache>(pick_cache_bytes_);
    {
        std::shared_ptr<const Model> m = model_;
        session_->limits.texture_usable = [m](uint32_t material) {
            HNwd::Material mat;
            return m->doc->material(material, mat) && mat.has_texture && mat.texture_bytes &&
                   texture_format_known(mat.texture_bytes, mat.texture_size);
        };
    }
    // Ett materials bild som inte ligger i filen, eller är varken PNG eller
    // JPEG, ritas med materialets färg; de räknades i inläsningen.
    missing_textures_ += loaded.missing_textures;

    // Detaljträdet planeras i en arbetstråd. Grundbilden, de fasta bitarna,
    // får sin andel av budgeten, delad med vad en triangel i den kostar; den
    // snålaste av RAM och grafikkort avgör.
    LodTreeSettings settings;
    settings.max_tile_triangles = options_.tile_triangles;
    // En nod med färre egna trianglar än en fjärdedel av taket lämnar dem
    // till föräldern: få och stora bitar håller ritanropen nere.
    settings.min_tile_triangles = std::max<uint64_t>(1, options_.tile_triangles / 4);
    settings.threads = std::max(1u, std::thread::hardware_concurrency());
    {
        const PartBytes per = bytes_per_triangle();
        // Med en gemensam budget kostar en triangel RAM och grafikkort tillsammans.
        const double ram_per = static_cast<double>(
            std::max<size_t>(budget_shared_ ? per.ram + per.gpu : per.ram, 1));
        const double by_ram =
            static_cast<double>(after_fixed(budget_.ram, reader_bytes_ + pick_cache_bytes_)) *
            kPinnedShare / ram_per;
        const double by_gpu = budget_shared_
                                  ? by_ram
                                  : static_cast<double>(budget_.gpu) * kPinnedShare /
                                        static_cast<double>(std::max<size_t>(per.gpu, 1));
        settings.bytes_per_primitive = ram_per;
        settings.pinned_bytes = static_cast<size_t>(std::min(by_ram, by_gpu) * ram_per);
    }
    status_.setText("Detaljträdet planeras…");
    const uint64_t gen = generation_;
    const double floor = std::max(model_->view.diagonal() * 1e-4, 1e-3);
    // Läsarnas cache, plockningens cache och detaljträdet är fast RAM: de
    // dras av budgeten schemaläggaren får. Schemaläggarens grundtillstånd
    // sätts här, i arbetstråden, och lämnas färdigt.
    TileScheduler::Settings sched_settings;
    sched_settings.min_pixels = options_.min_pixels;
    sched_settings.budget = budget_;
    sched_settings.shared = budget_shared_;
    sched_settings.size_margin = options_.size_margin;
    const size_t fixed_ram = reader_bytes_ + pick_cache_bytes_;
    loader_.run(
        [model = model_, settings, sched_settings, fixed_ram, floor,
         gen](std::stop_token stop) -> Planned {
            const auto begin = Clock::now();
            Planned out;
            out.generation = gen;
            auto tree = std::make_shared<LodTree>();
            out.ok = plan_lod_tree(*model, settings, stop, *tree, out.stats);
            if (!out.ok) return out;
            std::vector<NodeBox> boxes = tree_boxes(*tree, kMaxBoxes);
            out.boxes = boxes.size();
            out.boxes_shape = box_shape(boxes, floor);
            out.box_list = std::make_shared<const std::vector<NodeBox>>(std::move(boxes));
            out.grid_shapes = grid_shapes(*model);
            out.tree_bytes = tree->memory_bytes();
            TileScheduler::Settings ss = sched_settings;
            ss.budget.ram = after_fixed(sched_settings.budget.ram, fixed_ram + out.tree_bytes);
            out.sched = std::make_unique<TileScheduler>();
            out.sched->reset(*tree, ss);
            out.tree = std::move(tree);
            out.seconds = std::chrono::duration<double>(Clock::now() - begin).count();
            return out;
        },
        [this](glaze::threads::Result<Planned>& r) {
            if (!r) {
                fail_load("Detaljträdet kunde inte planeras: " + r.error());
                return;
            }
            on_planned(*r);
        });

    if (options_.profile) {
        use_busy_ = true;
        use_loader_.run(
            [model = model_](std::stop_token stop) {
                const auto t0 = std::chrono::steady_clock::now();
                UseResult out;
                out.use = record_use(*model, stop);
                out.seconds = seconds_since(t0);
                return out;
            },
            [this, gen](glaze::threads::Result<UseResult>& r) {
                if (gen != generation_) return;
                use_busy_ = false;
                if (!r) {
                    note("postanvändningen kunde inte räknas: " + std::string(r.error()));
                    return;
                }
                use_ = std::move(r->use);
                use_seconds_ = r->seconds;
                use_done_ = true;
            });
    }
}

void ViewerWindow::on_planned(Planned& planned) {
    const auto t0 = Clock::now();
    AtExit guard{[&] { stall("planerat", t0); }};
    if (planned.generation != generation_ || !model_) return;
    if (!planned.ok || !planned.tree) {
        fail_load("Detaljträdet planerades inte");
        return;
    }
    lod_ = std::move(planned.tree);
    lod_stats_ = planned.stats;
    lod_bytes_ = planned.tree_bytes;
    box_list_ = std::move(planned.box_list);
    pinned_pending_ = lod_stats_.pinned_tiles;
    {
        char buf[360];
        std::snprintf(
            buf, sizeof buf,
            "detaljträdet planerat på %s: %zu noder, %zu bitar (%u fasta med %s trianglar), %u "
            "delade löv, %s fragment uppflyttade, %zu lådor, %s MB",
            seconds_text(planned.seconds).c_str(), lod_->nodes.size(), lod_->tiles.size(),
            lod_stats_.pinned_tiles, millions(lod_stats_.pinned_primitives).c_str(),
            lod_stats_.split_nodes, millions(lod_stats_.moved_up).c_str(), planned.boxes,
            megabytes(lod_bytes_).c_str());
        note(buf);
    }

    // Ritningens kostnad per bild läses ur profilerarens bilder: den är på så
    // länge en fil är öppen.
    glaze::profiler().setEnabled(true);
    start_selector(std::move(planned.sched));

    scene_.setShapes(boxes_layer_, {std::move(planned.boxes_shape)});

    progress_.hide();
    setUseWaitCursor(false);
    unit_.setText(model_->unit_text);
    show_all();
    fill_views();
    show_grids(std::move(planned.grid_shapes));
    if (!model_->extras_error.empty())
        std::fprintf(stderr, "nwdview: %s\n", model_->extras_error.c_str());
    // Startkameran: ögat och punkten ur argumenten, den begärda sparade vyn,
    // annars filens aktuella vy när den ser på modellen, annars inpassningen
    // ovan.
    walk_.speed = walk_speed_after(options_.walk_speed, 0, walk_settings_);
    walk_.follow_ground = options_.follow_ground;
    if (options_.has_eye) {
        WalkState pose;
        if (!walk_look_at(pose, options_.eye, options_.look)) {
            fail_load("--eye och --look är samma punkt");
            return;
        }
        double f[3];
        walk_forward(pose, f);
        const double d = std::sqrt(std::pow(options_.look[0] - options_.eye[0], 2) +
                                   std::pow(options_.look[1] - options_.eye[1], 2) +
                                   std::pow(options_.look[2] - options_.eye[2], 2));
        glaze::Camera3D c = scene_.camera3D();
        c.target = {options_.look[0], options_.look[1], options_.look[2]};
        c.distance = d;
        c.azimuth = static_cast<float>(pose.yaw);
        c.elevation = static_cast<float>(-pose.pitch);
        c.roll = 0;
        scene_.setCamera(c);
        char buf[160];
        std::snprintf(buf, sizeof buf, "kameran: ögat %.3f %.3f %.3f, riktningen %.1f°, %.1f° upp",
                      options_.eye[0], options_.eye[1], options_.eye[2], pose.yaw, pose.pitch);
        note(buf);
    } else if (!options_.view.empty()) {
        const SavedViewInfo* wanted = nullptr;
        for (const SavedViewInfo& v : model_->saved_views)
            if (!v.folder && v.name == options_.view) {
                wanted = &v;
                break;
            }
        if (!wanted) {
            fail_load("Vyn \"" + options_.view + "\" finns inte bland filens sparade vyer");
            return;
        }
        if (!apply_file_camera(wanted->camera, false)) {
            fail_load("Vyn \"" + options_.view + "\" ser inte på modellen");
            return;
        }
        note("kameran: den sparade vyn " + options_.view);
        set_view_clip(wanted->camera);
    } else if (model_->has_current_view && apply_file_camera(model_->current_view, false)) {
        note("kameran: filens aktuella vy");
        set_view_clip(model_->current_view);
    } else {
        note("kameran: inpassad på modellen");
    }
    if (options_.clip_test.count > 0) set_clip(options_.clip_test, "");
    if (view_clip_.count > 0)
        note("snittet: " + std::to_string(view_clip_.count) + " plan, " +
             (options_.clip ? "påslaget" : "avslaget"));
    {
        char buf[200];
        const Box& w = model_->world;
        std::snprintf(buf, sizeof buf, "modellens låda: %.3f %.3f %.3f – %.3f %.3f %.3f m",
                      w.min[0], w.min[1], w.min[2], w.max[0], w.max[1], w.max[2]);
        note(buf);
    }
    if (options_.walk) set_walking(true);
    start_camera_ = scene_.camera3D();
    update_status();
    // Kameran gäller först när ritytan har en yta; mätningen görs om då.
    remeasure();
    update_timer_.start();
    // En modell utan bitar har ingen grundbild att vänta på.
    note_base();
}

void ViewerWindow::show_all() {
    if (!model_) return;
    set_walking(false);
    skip_ahead_ = true;
    const Box& v = model_->view;
    const double diagonal = std::max(v.diagonal(), 1e-6);
    glaze::Camera3D camera;
    double c[3];
    v.center(c);
    camera.target = {c[0], c[1], c[2]};
    camera.distance = diagonal * 1.2;
    camera.azimuth = 35;
    camera.elevation = 25;
    scene_.setCamera(camera);
    scene_.setDistanceRange(diagonal * 1e-4, diagonal * 20);
    // Sättarna fyrar inte onCameraChange.
    request_update();
}

bool ViewerWindow::apply_file_camera(const HNwd::Camera& camera, bool glide) {
    OrbitPose pose;
    if (!model_ || !orbit_from_camera(*model_, camera, pose)) return false;
    set_walking(false);
    skip_ahead_ = true;
    glaze::Camera3D c = scene_.camera3D();
    c.target = {pose.target[0], pose.target[1], pose.target[2]};
    c.distance = pose.distance;
    c.azimuth = static_cast<float>(pose.azimuth);
    c.elevation = static_cast<float>(pose.elevation);
    c.roll = 0;
    if (glide && options_.png.empty())
        scene_.easeTo(c, 400);
    else
        scene_.setCamera(c);
    request_update();
    return true;
}

void ViewerWindow::set_view_clip(const HNwd::Camera& camera) {
    if (!model_) return;
    ClipState clip;
    std::string error;
    clip_from_camera(*model_, camera, clip, error);
    set_clip(clip, error);
}

void ViewerWindow::set_clip(const ClipState& clip, const std::string& error) {
    view_clip_ = clip;
    clip_error_ = error;
    if (!error.empty()) std::fprintf(stderr, "nwdview: %s\n", error.c_str());
    apply_clip();
}

const ClipState& ViewerWindow::active_clip() const {
    return options_.clip ? view_clip_ : no_clip_;
}

glaze::Shape ViewerWindow::clipped_shape(const PartShape& part) const {
    if (part.shape.kind() != glaze::Shape::Kind::Mesh) return part.shape;
    // Bara visarens egna program klipper; en del i det inbyggda (en bild som
    // inte gick att avkoda) ritas hel.
    const bool own_program = !part.shape.meshStyle().program.empty();
    const bool cut = part.drawn_triangles < part.triangles;
    if (!own_program && !cut) return part.shape;
    glaze::MeshStyle style = part.shape.meshStyle();
    if (own_program) {
        if (part.lines)
            set_line_clip_uniforms(style.uniforms, active_clip(), part.origin);
        else
            set_clip_uniforms(style.uniforms, active_clip(), part.origin);
    }
    // Elementen räknas i index, tre per triangel.
    if (cut) style.range = glaze::IndexRange{0, static_cast<uint32_t>(part.drawn_triangles * 3)};
    return glaze::Shape::mesh(part.shape.mesh(), part.shape.placement(), std::move(style));
}

void ViewerWindow::apply_clip() {
    // Uniformerna byts med setShape; meshen och bilden laddas inte upp igen.
    // De visade bitarna köas och får snittet en omgång åt gången
    // (`run_queues`); de parkerade står i sina platser med ritad andel noll och
    // får snittet när de visas.
    clip_queue_.clear();
    clip_next_ = 0;
    for (const auto& entry : views_)
        if (entry.second.shown) clip_queue_.push_back(entry.first);
    // Texterna: en text vars mitt snittet tar bort visas inte.
    if (have_view_) update_labels(last_view_);
    update_status();
    start_slices();
}

void ViewerWindow::apply_clip_tile(uint32_t tile) {
    const auto it = views_.find(tile);
    if (it == views_.end() || !it->second.shown) return;
    TileView& v = it->second;
    for (size_t i = 0; i < v.parts.size(); ++i) put_part(v, i);
}

bool ViewerWindow::queues_empty() const {
    return ops_.empty() && clip_next_ >= clip_queue_.size() && rebuild_queue_.empty() &&
           order_stage_ >= 3 && label_pending_.empty();
}

void ViewerWindow::start_slices() {
    // Texterna har sin egen timer.
    const bool work = !ops_.empty() || clip_next_ < clip_queue_.size() || !rebuild_queue_.empty() ||
                      order_stage_ < 3 || !retired_.empty();
    if (work && !slice_timer_.isRunning()) slice_timer_.start();
}

void ViewerWindow::run_queues() {
    const auto t0 = Clock::now();
    AtExit guard{[&] { stall("köer", t0); }};
    const Slice slice(kQueueSliceMs);
    // Det urvalet sagt: släpp, ta emot, parkera, visa och sätt ritad andel, i
    // ordning.
    apply_ops(slice);
    // Modellvalet: bitarnas tal räknas om och de ogiltigförklaras hos urvalet,
    // som ger släpp och nya jobb.
    if (!rebuild_queue_.empty() && selector_) {
        SelInvalidate inv;
        while (!rebuild_queue_.empty() && !slice.spent() && inv.tiles.size() < 4'096) {
            const uint32_t tile = rebuild_queue_.front();
            rebuild_queue_.pop_front();
            counted_.erase(tile);
            inv.tiles.push_back(tile);
        }
        if (!inv.tiles.empty()) {
            SelItem item;
            item.seq = ++sent_seq_;
            item.what = std::move(inv);
            selector_->post_item(std::move(item));
        }
    }
    while (clip_next_ < clip_queue_.size() && !slice.spent())
        apply_clip_tile(clip_queue_[clip_next_++]);
    if (clip_next_ >= clip_queue_.size()) {
        clip_queue_.clear();
        clip_next_ = 0;
    }
    run_order(slice);
    flush_retired(false);
    // Urvalet höll inne operationer medan det var många: när de tagits mäts
    // det om.
    if (sel_throttled_ && ops_count_ < kOpBacklog / 2) {
        sel_throttled_ = false;
        if (!update_timer_.isRunning()) update_timer_.start();
    }
    start_slices();
}

void ViewerWindow::fill_views() {
    view_nodes_.clear();
    views_tree_.setNodes({});
    if (model_->has_current_view)
        view_nodes_.push_back({views_tree_.addNode({}, "Filens vy"), model_->current_view});
    if (model_->has_home_view)
        view_nodes_.push_back({views_tree_.addNode({}, "Hemvy"), model_->home_view});
    // Posterna står i preorder, så en mapp har sin stig före sina poster.
    const std::vector<SavedViewInfo>& saved = model_->saved_views;
    std::vector<glaze::TreePath> paths(saved.size());
    for (size_t i = 0; i < saved.size(); ++i) {
        const SavedViewInfo& v = saved[i];
        const glaze::TreePath parent = v.parent < i ? paths[v.parent] : glaze::TreePath{};
        paths[i] = views_tree_.addNode(parent, v.name.empty() ? std::string("(utan namn)") : v.name);
        if (!v.folder) view_nodes_.push_back({paths[i], v.camera});
    }
    for (size_t i = 0; i < saved.size(); ++i)
        if (saved[i].folder && !paths[i].empty()) views_tree_.setExpanded(paths[i], true);

    std::vector<glaze::Table::Row> rows;
    for (const GridLevelInfo& l : model_->grid_levels) {
        char height[48];
        std::snprintf(height, sizeof height, "%.3f%s", l.elevation, model_->unit_known ? " m" : "");
        glaze::Table::Row row;
        row.cells = {l.name, height, l.system};
        rows.push_back(std::move(row));
    }
    levels_list_.setRows(std::move(rows));
    levels_list_.height = model_->grid_levels.size() > 8 ? 240.0f : 0.0f;
    if (model_->grid_levels.empty())
        levels_list_.hide();
    else
        levels_list_.show();
    if (view_nodes_.empty() && saved.empty() && model_->grid_levels.empty())
        side_.hide();
    else
        side_.show();
    side_.invalidate();
}

void ViewerWindow::show_grids(std::vector<glaze::Shape> shapes) {
    scene_.setShapes(grid_layer_, std::move(shapes));
    scene_.setLayerVisible(grid_layer_, grid_visible_);
}

void ViewerWindow::choose_view() {
    if (!model_) return;
    const glaze::TreePath path = views_tree_.selectedPath();
    for (const auto& [p, camera] : view_nodes_) {
        if (p != path) continue;
        set_view_clip(camera);
        if (!apply_file_camera(camera, true))
            status_.setText(views_tree_.selectedText() + ": vyn ser inte på modellen");
        return;
    }
}

void ViewerWindow::note_motion(Clock::time_point now) {
    const double dt = std::chrono::duration<double>(now - last_motion_).count();
    motion_run_ = dt < kMovingSeconds;
    last_motion_ = now;
}

bool ViewerWindow::moving(Clock::time_point now) const {
    const double since_event = std::chrono::duration<double>(now - last_motion_).count();
    const double since_tick = std::chrono::duration<double>(now - tick_at_).count();
    return since_event < kMovingSeconds || since_tick < kMovingSeconds;
}

double ViewerWindow::draw_cost_ms() {
    const std::vector<glaze::FrameProfile> frames = glaze::profiler().frames(*this, kCostFrames);
    if (frames.empty()) return 0;
    // Ingen ny bild sedan förra frågan: bilderna säger inget om nu.
    if (frames.back().frame == cost_frame_) return 0;
    cost_frame_ = frames.back().frame;
    // Grafikkortets svar kommer några bilder i efterhand och saknas (0) i de
    // senaste; ritfasen finns i varje bild.
    std::vector<float> gpu, draw;
    for (const glaze::FrameProfile& f : frames) {
        const float g = f.ms[size_t(glaze::ProfilePhase::Gpu)];
        const float d = f.ms[size_t(glaze::ProfilePhase::CanvasDraw)];
        if (g > 0) gpu.push_back(g);
        if (d > 0) draw.push_back(d);
    }
    auto median = [](std::vector<float>& v) {
        if (v.empty()) return 0.0;
        const auto mid = v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2);
        std::nth_element(v.begin(), mid, v.end());
        return static_cast<double>(*mid);
    };
    // Grafikkortet och ritfasen arbetar efter varandra i olika enheter: den
    // större av dem sätter bildens takt. Saknas det ena gäller det andra, och
    // saknas båda är kostnaden okänd (0).
    return std::max(median(gpu), median(draw));
}

void ViewerWindow::request_update() {
    // Varje anrop är en kamera- eller storleksändring.
    const auto now = Clock::now();
    note_motion(now);
    // Var 50:e ms medan händelserna kommer i följd, annars var 100:e.
    const int interval = motion_run_ ? kUpdateMovingMs : kUpdateIdleMs;
    const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_update_);
    if (since.count() >= interval) {
        remeasure();
    } else if (!update_timer_.isRunning()) {
        update_timer_.setInterval(
            std::clamp(interval - static_cast<int>(since.count()), 10, interval));
        update_timer_.start();
    }
    if (!settle_timer_.isRunning()) settle_timer_.start();
}

void ViewerWindow::settle() {
    if (moving(Clock::now())) return;
    settle_timer_.stop();
    if (!model_ || !lod_) return;
    restore_quality();
    update_upload_budget(false);
    // En mätning utan rörelse: bildtiden är okänd och uppladdningen får mer.
    remeasure();
}

void ViewerWindow::remeasure() {
    if (!model_ || !lod_ || !selector_) return;
    fit_orbit_near();
    const auto t0 = Clock::now();
    AtExit guard{[&] { stall("mätning", t0); }};
    last_update_ = Clock::now();
    const gfx::Sizef size = scene_.actualSize();
    measured_ = size.w > 0 && size.h > 0;
    if (!measured_) {
        // Ritytan har ingen yta än: mätningen görs om en stund.
        if (!update_timer_.isRunning()) update_timer_.start();
        return;
    }
    const glaze::Camera3D cam = scene_.camera3D();
    const glaze::Point3 eye = cam.eye();
    const double e[3] = {eye.x, eye.y, eye.z};
    // Med --png mäts urvalet med bildens storlek, så att garantin gäller i
    // bilden.
    const bool image_size = !options_.png.empty();
    ViewFrustum view = ViewFrustum::orbit(e, cam.azimuth, cam.elevation, cam.fov,
                                          image_size ? kCaptureWidth : size.w,
                                          image_size ? kCaptureHeight : size.h);
    if (walking_) view.near_radius = kWalkNearRadius;
    // Bitarna som byggs får detaljnivåer för den här kameran.
    last_view_ = view;
    have_view_ = true;
    const auto clock_now = Clock::now();
    // Med --png bara uppskattningen, utom när gången, snurran eller rundan ska
    // mäta den verkliga regleringen.
    const bool deterministic = !options_.png.empty() && options_.walk_distance <= 0 &&
                               options_.spin <= 0 && !options_.tour;
    // Allt urvalet räknar (vilka bitar som byggs, släpps, visas och parkeras,
    // ritad andel, ordningen, ritningens tröskel) görs i dess tråd; här lämnas
    // bara kameran.
    MeasureMsg m;
    m.seq = ++sent_seq_;
    m.view = view;
    m.skip_ahead = skip_ahead_;
    skip_ahead_ = false;
    m.deterministic = deterministic;
    m.frame_ms = deterministic ? 0 : draw_cost_ms();
    m.want_holes = walk_running_;
    m.backlog = ops_count_;
    selector_->post_measure(std::move(m));
    const bool mv = moving(clock_now);
    update_upload_budget(mv);
    update_quality(mv);
    update_labels(view);
    if (shown_tiles_ > 0) check_programs();
}

void ViewerWindow::start_selector(std::unique_ptr<TileScheduler> sched) {
    stop_selector();
    SelectorInit init;
    init.lod = lod_;
    init.sched = std::move(sched);
    init.governor = options_.governor;
    init.governor.min_pixels = options_.min_pixels;
    init.governor.max_pixels = std::max(init.governor.max_pixels, init.governor.min_pixels);
    init.max_inflight = build_pool_.threadCount() + 2;
    init.diagonal = model_->view.diagonal();
    init.opened = opened_at_;
    selector_ = std::make_shared<SelectorCore>(std::move(init), generation_);
    sent_seq_ = acked_seq_ = 0;
    answer_calm_ = false;
    sel_throttled_ = false;
    // Svaren går genom Workerns post: de släpps när fönstret rivs eller filen
    // byts, så att `this` är säkert.
    glaze::threads::Worker* worker = &selector_worker_;
    const std::shared_ptr<SelectorCore> core = selector_;
    selector_worker_.run(
        [this, worker, core](std::stop_token stop) {
            core->run(stop, [&](SelAnswer&& a) {
                worker->post(stop, [this, a = std::move(a)]() mutable { on_answer(a); });
            });
        },
        [](glaze::threads::Result<void>& r) {
            if (!r)
                std::fprintf(stderr, "nwdview: urvalets tråd föll: %s\n",
                             std::string(r.error()).c_str());
        });
}

void ViewerWindow::stop_selector() {
    // Begär stopp utan att vänta: tråden slutar när den är färdig med en pågående
    // genomgång, och dess tillstånd (schemaläggaren och bilden av bitarna) dör i
    // en arbetstråd.
    selector_worker_.cancel();
    if (!selector_) return;
    trash_worker_.run([core = std::move(selector_)]() mutable { core.reset(); },
                      [](glaze::threads::Result<void>&) {});
    selector_.reset();
}

void ViewerWindow::on_answer(SelAnswer& a) {
    const auto t0 = Clock::now();
    AtExit guard{[&] { stall("svar", t0); }};
    if (a.generation != generation_ || !lod_ || !selector_) return;
    acked_seq_ = std::max(acked_seq_, a.seq);
    stats_ = a.stats;
    draw_pixels_ = a.draw_pixels;
    draw_pixels_max_ = std::max(draw_pixels_max_, draw_pixels_);
    last_load_ = a.load;
    draw_reduce_ = a.reduce;
    sel_throttled_ = a.throttled;
    k_rebuilds_ += a.k_rebuilds;
    // Påbörjade jobb som schemaläggaren släppt avbryts; nya jobb startas.
    for (const uint32_t t : a.cancel) {
        const auto it = job_stops_.find(t);
        if (it != job_stops_.end()) it->second.request_stop();
    }
    for (const TileScheduler::Job& j : a.jobs) start_tile_job(j.tile, j.min_size);
    answer_calm_ = a.ops.empty() && a.jobs.empty() && a.cancel.empty() && !a.throttled;
    if (!a.ops.empty()) {
        ops_count_ += a.ops.size();
        ops_.push_back(std::move(a.ops));
    }
    if (a.has_order) {
        order_ = std::move(a.order);
        order_stage_ = 0;
    }
    if (a.has_holes && walk_running_) note_walk_sample(a.holes);
    // Operationerna görs av `slice_timer_`, en tidsbudget per pumpvarv.
    if (sel_throttled_ && ops_count_ < kOpBacklog / 2) {
        sel_throttled_ = false;
        if (!update_timer_.isRunning()) update_timer_.start();
    }
    start_slices();
    after_answer();
}

void ViewerWindow::after_answer() {
    if (!model_ || !lod_) return;
    note_base();
    if (!idle_noted_ && base_noted_ && stats_.idle && !boxer_.busy() && ops_count_ == 0) {
        idle_noted_ = true;
        note("startvyn klar: " + std::to_string(stats_.built) + " bitar byggda, " +
             std::to_string(shown_tiles_) + " visade, " + millions(shown_triangles_) +
             " trianglar, " + ram_gpu(resident_total()) + "; " + threshold_text());
    }
    update_status();
}

bool ViewerWindow::selector_calm() const {
    return selector_ && acked_seq_ == sent_seq_ && answer_calm_ && stats_.idle && ops_count_ == 0 &&
           order_stage_ >= 3 && !sel_throttled_;
}

void ViewerWindow::put_part(TileView& v, size_t part) {
    const PartShape& p = v.parts[part];
    if (p.shape.kind() == glaze::Shape::Kind::None) return;
    if (part == kPartOpaque) {
        if (v.opaque_slot >= 0) scene_.setShape(opaque_layer_, v.opaque_slot, clipped_shape(p));
    } else if (part == kPartTransparent) {
        if (v.transparent_slot >= 0)
            scene_.setShape(transparent_layer_, v.transparent_slot, clipped_shape(p));
    } else if (part == kPartLines) {
        if (v.line_slot >= 0) scene_.setShape(line_layer_, v.line_slot, clipped_shape(p));
    } else {
        // De texturerade delarnas platser står i delarnas ordning, utan de tomma.
        size_t j = 0;
        for (size_t k = kFirstTexturedPart; k < part; ++k)
            if (v.parts[k].shape.kind() != glaze::Shape::Kind::None) ++j;
        if (j < v.textured_slots.size())
            scene_.setShape(textured_layer_, v.textured_slots[j], clipped_shape(p));
    }
}

void ViewerWindow::set_drawn(TileView& v, const std::vector<uint32_t>& drawn) {
    for (size_t i = 0; i < v.parts.size(); ++i) {
        PartShape& p = v.parts[i];
        if (p.shape.kind() == glaze::Shape::Kind::None) continue;
        const uint64_t want = i < drawn.size() ? drawn[i] : 0;
        if (v.shown) shown_triangles_ = shown_triangles_ - p.drawn_triangles + want;
        p.drawn_triangles = want;
    }
}

void ViewerWindow::run_order(const Slice& slice) {
    // Ett lager i taget; det som inte hinns tas i nästa omgång, med samma lista.
    while (order_stage_ < 3 && !slice.spent()) {
        const SlotKind kind = order_stage_ == 0   ? SlotKind::Opaque
                              : order_stage_ == 1 ? SlotKind::Textured
                                                  : SlotKind::Lines;
        std::vector<SlotOwner>& owners = kind == SlotKind::Opaque     ? opaque_owner_
                                         : kind == SlotKind::Textured ? textured_owner_
                                                                      : line_owner_;
        std::vector<OrderItem> items;
        for (const OrderEntry& e : order_) {
            const auto it = views_.find(e.tile);
            if (it == views_.end() || !it->second.shown) continue;
            const TileView& v = it->second;
            if (kind == SlotKind::Opaque) {
                if (v.opaque_slot >= 0)
                    items.push_back({e.tile, static_cast<uint32_t>(kPartOpaque), v.opaque_slot, e.band});
            } else if (kind == SlotKind::Lines) {
                if (v.line_slot >= 0)
                    items.push_back({e.tile, static_cast<uint32_t>(kPartLines), v.line_slot, e.band});
            } else {
                for (const int s : v.textured_slots) {
                    if (s < 0 || static_cast<size_t>(s) >= owners.size()) continue;
                    items.push_back({e.tile, owners[static_cast<size_t>(s)].part, s, e.band});
                }
            }
        }
        const int layer = kind == SlotKind::Opaque     ? opaque_layer_
                          : kind == SlotKind::Textured ? textured_layer_
                                                       : line_layer_;
        bool done = true;
        slots_reordered_ += reorder_layer(layer, owners, kind, items, kMaxSwaps, slice, done) * 2;
        if (!done) return;
        ++order_stage_;
    }
}

size_t ViewerWindow::reorder_layer(int layer, std::vector<SlotOwner>& owners, SlotKind kind,
                                   std::vector<OrderItem>& items, size_t max_swaps,
                                   const Slice& slice, bool& done) {
    const size_t n = items.size();
    if (n < 2) return 0;
    // Närmast först, också om schemaläggarens ordning avviker något.
    std::stable_sort(items.begin(), items.end(),
                     [](const OrderItem& a, const OrderItem& b) { return a.band < b.band; });
    // Position k i `slots` är den k:te lägsta platsen; föremål k ska ligga där.
    std::vector<int> slots(n);
    for (size_t i = 0; i < n; ++i) slots[i] = items[i].slot;
    std::sort(slots.begin(), slots.end());
    std::vector<uint32_t> where(n), occupant(n);
    for (size_t i = 0; i < n; ++i) {
        const size_t k = static_cast<size_t>(
            std::lower_bound(slots.begin(), slots.end(), items[i].slot) - slots.begin());
        where[i] = static_cast<uint32_t>(k);
        occupant[k] = static_cast<uint32_t>(i);
    }
    // Platser vars föremål ligger i fel band mot det som ska ligga där.
    std::vector<uint32_t> bad;
    for (size_t k = 0; k < n; ++k)
        if (items[occupant[k]].band != items[k].band) bad.push_back(static_cast<uint32_t>(k));
    if (bad.size() < std::max(kReorderMinMismatch, n / 16)) return 0;
    // De felplacerade föremålen, sorterade efter band, tar de felplacerade
    // platserna i ordning: banden stämmer då exakt, och de platser som redan
    // har rätt band rörs inte.
    std::vector<uint32_t> movers;
    movers.reserve(bad.size());
    for (const uint32_t k : bad) movers.push_back(occupant[k]);
    std::sort(movers.begin(), movers.end(), [&](uint32_t a, uint32_t b) {
        return items[a].band != items[b].band ? items[a].band < items[b].band : a < b;
    });
    size_t swaps = 0;
    for (size_t j = 0; j < bad.size(); ++j) {
        if (swaps >= max_swaps || slice.spent()) {
            // Det som återstår tas i nästa omgång.
            done = false;
            break;
        }
        const uint32_t k = bad[j];
        const uint32_t w = movers[j];
        const uint32_t a = occupant[k];
        if (a == w) continue;
        const uint32_t q = where[w];
        OrderItem& A = items[a];
        OrderItem& W = items[w];
        const int sa = A.slot, sw = W.slot;
        TileView& va = views_.at(A.tile);
        TileView& vw = views_.at(W.tile);
        // Varje byte är ett giltigt läge: formerna byter plats med varandra.
        scene_.setShape(layer, sa, clipped_shape(vw.parts[W.part]));
        scene_.setShape(layer, sw, clipped_shape(va.parts[A.part]));
        owners[static_cast<size_t>(sa)] = {W.tile, W.part};
        owners[static_cast<size_t>(sw)] = {A.tile, A.part};
        auto retarget = [&](TileView& v, int from, int to) {
            if (kind == SlotKind::Opaque) {
                v.opaque_slot = to;
                return;
            }
            if (kind == SlotKind::Lines) {
                v.line_slot = to;
                return;
            }
            for (int& s : v.textured_slots)
                if (s == from) {
                    s = to;
                    break;
                }
        };
        retarget(va, sa, sw);
        retarget(vw, sw, sa);
        A.slot = sw;
        W.slot = sa;
        occupant[k] = w;
        occupant[q] = a;
        where[w] = k;
        where[a] = q;
        ++swaps;
    }
    return swaps;
}

void ViewerWindow::update_quality(bool moving) {
    // Med --png ändras kantutjämningen aldrig.
    if (!options_.png.empty()) return;
    if (moving) {
        if (draw_reduce_ && !aa_low_) {
            const int full = scene_.multisample();
            if (full > 1) {
                aa_full_ = full;
                scene_.setMultisample(1);
                aa_low_ = true;
                ++aa_reductions_;
            }
        }
    } else {
        restore_quality();
    }
}

void ViewerWindow::restore_quality() {
    if (!aa_low_) return;
    aa_low_ = false;
    scene_.setMultisample(aa_full_);
}

void ViewerWindow::update_upload_budget(bool moving) {
    // Fångsten av en bild har sin egen budget.
    if (capturing_) return;
    const int state = moving ? 1 : 2;
    if (state == upload_state_) return;
    upload_state_ = state;
    glaze::app().setCanvasUploadBudget(moving ? kUploadMoving : kUploadStill);
}

std::string ViewerWindow::draw_text() const {
    if (draw_pixels_ <= stats_.threshold * 1.0001) return {};
    return "ritar ≥ " + pixels_text(draw_pixels_) + " px för bildtakten";
}

void ViewerWindow::start_tile_job(uint32_t tile, float min_size) {
    std::shared_ptr<Session> session = session_;
    const std::shared_ptr<const LodTree> lod = lod_;
    const uint64_t gen = generation_;
    const std::shared_ptr<const ModelFilter> filter = filter_;
    const glaze::Lighting light = light_;
    const double min_pixels = options_.min_pixels;
    // Nivåerna bakas för den här kameran och en något större bild.
    const double pixels_per_radian = last_view_.pixels_per_radian() * kLodMargin;
    // Jobbet har ett eget stopp, så att det går att avbryta utan att röra de
    // andra; arbetarens token stoppar det också.
    std::stop_source job;
    job_stops_[tile] = job;
    builder_.run(
        [session, lod, tile, filter, light, gen, min_pixels, pixels_per_radian, min_size,
         job](std::stop_token stop) -> BuiltTile {
            std::stop_callback link(stop, [s = job]() mutable { s.request_stop(); });
            return build_tile_job(session, lod, tile, filter, light, gen, min_pixels,
                                  pixels_per_radian, min_size, job.get_token());
        },
        [this](glaze::threads::Result<BuiltTile>& r) {
            if (r) {
                on_built(*r);
                return;
            }
            std::fprintf(stderr, "nwdview: ett byggjobb föll: %s\n", std::string(r.error()).c_str());
        });
}

void ViewerWindow::on_built(BuiltTile& b) {
    const auto t0 = Clock::now();
    AtExit guard{[&] { stall("bit", t0); }};
    if (b.generation != generation_ || !lod_ || b.tile >= lod_->tiles.size() || !selector_) {
        retire(std::move(b.parts));
        return;
    }
    job_stops_.erase(b.tile);
    SelBuilt m;
    m.tile = b.tile;
    m.min_size = b.min_size;
    if (b.status == HNwd::Status::Cancelled) {
        // Ett avbrutet jobb är inget fel: biten byggs igen när den behövs, och
        // urvalet avgör vad som händer med en version som redan finns.
        ++jobs_cancelled_;
        m.cancelled = true;
        SelItem item;
        item.seq = ++sent_seq_;
        item.what = std::move(m);
        selector_->post_item(std::move(item));
        return;
    }
    const bool ok = b.status == HNwd::Status::Ok;
    if (!ok) {
        ++failed_tiles_;
        if (build_error_.empty()) {
            build_error_ = "bit " + std::to_string(b.tile) + ": " + b.error;
            std::fprintf(stderr, "nwdview: %s\n", build_error_.c_str());
        }
        retire(std::move(b.parts));
    }
    if (ok) tile_stats_.add(b.times);
    if (ok && counted_.insert(b.tile).second) {
        points_ += b.points;
        texts_ += b.texts;
        undecoded_ += b.undecoded;
        segments_skipped_ += b.segments_skipped;
        missing_textures_ += b.missing_textures;
        rotated_textures_ += b.rotated_textures;
        if (b.missing_textures && build_error_.empty() && !b.error.empty())
            build_error_ = "en bild: " + b.error;
    }
    // En fast bit är klar när den är byggd, eller när den inte går att bygga;
    // en byggd bit räknas när urvalet sagt att den är kvar.
    if (!ok && lod_->tiles[b.tile].pinned && pinned_seen_.insert(b.tile).second &&
        pinned_pending_ > 0)
        --pinned_pending_;
    m.ok = ok;
    m.bytes = b.bytes;
    if (ok) {
        TileView fresh;
        fresh.parts = std::move(b.parts);
        // Ritad andel noll tills biten visas.
        for (PartShape& p : fresh.parts) p.drawn_triangles = 0;
        fresh.stat = std::move(b.stat);
        fresh.labels = std::move(b.labels);
        fresh.bytes = b.bytes;
        fresh.baked_ppr = b.baked_ppr;
        fresh.min_size = b.min_size;
        fresh.primitives = primitives_of(fresh.parts);
        fresh.install = next_install_++;
        m.install = fresh.install;
        m.stat = fresh.stat;
        m.baked_ppr = fresh.baked_ppr;
        // Versionen väntar på urvalets besked: kvar eller inte.
        pending_.emplace(fresh.install, PendingTile{b.tile, std::move(fresh)});
    }
    SelItem item;
    item.seq = ++sent_seq_;
    item.what = std::move(m);
    selector_->post_item(std::move(item));
}

uint64_t ViewerWindow::primitives_of(const std::vector<PartShape>& parts) {
    uint64_t n = 0;
    for (const PartShape& p : parts) {
        if (p.shape.kind() == glaze::Shape::Kind::None) continue;
        // Ett linjesegment är två trianglar.
        n += p.lines ? p.triangles / 2 : p.triangles;
    }
    return n;
}

void ViewerWindow::replace_view(uint32_t tile, TileView fresh, bool shown,
                                const std::vector<uint32_t>& drawn) {
    const auto it = views_.find(tile);
    const bool had = it != views_.end();
    TileView old;
    if (had) {
        old = std::move(it->second);
        if (fresh.min_size < old.min_size) ++size_rebuilds_;
        if (old.shown) {
            for (const PartShape& p : old.parts)
                if (p.shape.kind() != glaze::Shape::Kind::None) shown_triangles_ -= p.drawn_triangles;
            release_labels(old, tile);
            --shown_tiles_;
        }
    }
    // Den gamla versionen visas tills den nya är här: den nya tar dess platser
    // (formerna byter plats i samma platser, så att ordningen står kvar). En
    // bit som inte visas får platser först när den visas: en parkerad bits
    // platser blir lediga, så att den nya meshen inte laddas upp i onödan.
    fresh.shown = shown;
    if (shown) set_drawn(fresh, drawn);
    if (had && old.slotted && shown) {
        place_parts(fresh, tile, &old);
        fresh.slotted = true;
    } else if (had && old.slotted) {
        release_slots(old);
    } else if (shown) {
        place_parts(fresh, tile, nullptr);
        fresh.slotted = true;
    }
    if (shown) {
        install_labels(fresh, tile);
        ++shown_tiles_;
        // Plockningen får det nya: hur mycket som ritas och till vilken storlek
        // biten är byggd.
        pick_shared_->add(tile, drawn_vector(fresh.parts), fresh.min_size);
        prime_dirty_ = true;
    } else if (had && old.shown) {
        pick_shared_->remove(tile);
    }
    views_[tile] = std::move(fresh);
    if (had) retire(std::move(old.parts));
}

void ViewerWindow::apply_ops(const Slice& slice) {
    while (!ops_.empty() && !slice.spent()) {
        std::vector<SelOp>& chunk = ops_.front();
        apply_op(chunk[ops_next_]);
        --ops_count_;
        if (++ops_next_ >= chunk.size()) {
            ops_.pop_front();
            ops_next_ = 0;
        }
    }
}

void ViewerWindow::apply_op(SelOp& op) {
    switch (op.kind) {
    case SelOp::Drop:
        drop_tile(op.tile);
        break;
    case SelOp::Install: {
        const auto it = pending_.find(op.install);
        if (it == pending_.end()) {
            inconsistent("ta emot", op.tile);
            break;
        }
        TileView fresh = std::move(it->second.view);
        pending_.erase(it);
        replace_view(op.tile, std::move(fresh), op.shown, op.drawn);
        if (lod_->tiles[op.tile].pinned && pinned_seen_.insert(op.tile).second &&
            pinned_pending_ > 0)
            --pinned_pending_;
        break;
    }
    case SelOp::Discard: {
        // Versionen ryms inte: den kastas utan att ha synts.
        const auto it = pending_.find(op.install);
        if (it == pending_.end()) break;
        retire(std::move(it->second.view.parts));
        pending_.erase(it);
        break;
    }
    case SelOp::Park:
        park_tile(op.tile);
        break;
    case SelOp::Show:
        show_tile(op.tile, op.drawn);
        break;
    case SelOp::Fit:
        fit_tile(op.tile, op.drawn);
        break;
    }
}

void ViewerWindow::note_base() {
    if (!lod_ || base_noted_) return;
    // Grundbilden är klar när alla fasta bitar är byggda. Finns det inga, eller
    // går någon inte att bygga (den får inte plats), är den klar när en mätning
    // har gjorts och inget mer byggs eller önskas.
    const bool pinned_done =
        lod_stats_.pinned_tiles > 0 ? pinned_pending_ == 0 : lod_->tiles.empty();
    if (!(pinned_done || (measured_ && stats_.idle))) return;
    base_noted_ = true;
    uint64_t tiles = 0, triangles = 0;
    size_t ram = 0;
    for (const auto& entry : views_) {
        if (!lod_->tiles[entry.first].pinned) continue;
        ++tiles;
        ram += entry.second.bytes.ram;
        for (const PartShape& p : entry.second.parts) triangles += p.triangles;
    }
    note("grundbilden klar: " + std::to_string(tiles) + " av " +
         std::to_string(lod_stats_.pinned_tiles) + " fasta bitar, " + millions(triangles) +
         " trianglar, " + megabytes(ram) + " MB RAM");
    // Lådorna har gjort sitt: grundbilden är det som syns innan resten kommer.
    boxes_cleared_ = true;
    scene_.setShapes(boxes_layer_, {});
    if (options_.png.empty()) start_tree();
}

void ViewerWindow::inconsistent(const char* what, uint32_t tile) {
    if (inconsistencies_++ >= 10) return;
    std::fprintf(stderr,
                 "nwdview: schemaläggaren och fönstret är inte överens: %s för bit %u som inte "
                 "är byggd eller visad\n",
                 what, tile);
}

int ViewerWindow::take_slot(int layer, std::vector<int>& free_slots,
                            std::vector<SlotOwner>& owners, SlotOwner owner) {
    int s;
    if (!free_slots.empty()) {
        s = free_slots.back();
        free_slots.pop_back();
    } else {
        // En ny plats börjar som en mesh, aldrig som en tom form: formen byts
        // sedan mot meshen på plats utan att lagret byggs om.
        s = scene_.addShape(layer, layer == labels_layer_ ? glaze::Shape{} : placeholder_);
        if (layer == labels_layer_) {
            if (label_on_scene_.size() <= static_cast<size_t>(s))
                label_on_scene_.resize(static_cast<size_t>(s) + 1, 0);
        }
    }
    if (owners.size() <= static_cast<size_t>(s)) owners.resize(static_cast<size_t>(s) + 1);
    owners[static_cast<size_t>(s)] = owner;
    return s;
}

void ViewerWindow::free_slot(int layer, std::vector<int>& free_slots,
                             std::vector<SlotOwner>& owners, int& slot) {
    if (slot < 0) return;
    // Platsen töms aldrig: ramverket räknar en tom form som död och bygger om
    // hela lagret när en fjärdedel är det. Den står med platshållaren (ritad
    // andel noll) tills en annan bits del tar den.
    if (layer == labels_layer_)
        set_label(slot, glaze::Shape{});
    else
        scene_.setShape(layer, slot, placeholder_);
    if (static_cast<size_t>(slot) < owners.size()) owners[static_cast<size_t>(slot)] = {};
    free_slots.push_back(slot);
    slot = -1;
}

void ViewerWindow::place_parts(TileView& v, uint32_t tile, TileView* old) {
    // Platserna som en tidigare version lämnar går först till den nya, delvis
    // i samma ordning; resten blir lediga.
    int reuse_opaque = -1, reuse_transparent = -1, reuse_line = -1;
    std::vector<int> reuse_textured;
    if (old) {
        reuse_opaque = old->opaque_slot;
        reuse_transparent = old->transparent_slot;
        reuse_line = old->line_slot;
        reuse_textured = std::move(old->textured_slots);
        old->opaque_slot = old->transparent_slot = old->line_slot = -1;
        old->textured_slots.clear();
    }
    auto slot_for = [&](int layer, std::vector<int>& free_slots, std::vector<SlotOwner>& owners,
                        int& reuse, SlotOwner owner) {
        if (reuse < 0) return take_slot(layer, free_slots, owners, owner);
        const int s = reuse;
        reuse = -1;
        if (owners.size() <= static_cast<size_t>(s)) owners.resize(static_cast<size_t>(s) + 1);
        owners[static_cast<size_t>(s)] = owner;
        return s;
    };
    size_t next_textured = 0;
    for (size_t i = 0; i < v.parts.size(); ++i) {
        PartShape& p = v.parts[i];
        if (p.shape.kind() == glaze::Shape::Kind::None) continue;
        const SlotOwner owner{tile, static_cast<uint32_t>(i)};
        if (i == kPartOpaque) {
            v.opaque_slot =
                slot_for(opaque_layer_, free_opaque_, opaque_owner_, reuse_opaque, owner);
            scene_.setShape(opaque_layer_, v.opaque_slot, clipped_shape(p));
        } else if (i == kPartTransparent) {
            v.transparent_slot = slot_for(transparent_layer_, free_transparent_,
                                          transparent_owner_, reuse_transparent, owner);
            scene_.setShape(transparent_layer_, v.transparent_slot, clipped_shape(p));
        } else if (i == kPartLines) {
            v.line_slot = slot_for(line_layer_, free_lines_, line_owner_, reuse_line, owner);
            scene_.setShape(line_layer_, v.line_slot, clipped_shape(p));
        } else {
            int reuse = next_textured < reuse_textured.size() ? reuse_textured[next_textured++] : -1;
            const int slot = slot_for(textured_layer_, free_textured_, textured_owner_, reuse, owner);
            scene_.setShape(textured_layer_, slot, clipped_shape(p));
            v.textured_slots.push_back(slot);
        }
    }
    // Platser som den nya versionen inte behöver.
    free_slot(opaque_layer_, free_opaque_, opaque_owner_, reuse_opaque);
    free_slot(transparent_layer_, free_transparent_, transparent_owner_, reuse_transparent);
    free_slot(line_layer_, free_lines_, line_owner_, reuse_line);
    for (size_t k = next_textured; k < reuse_textured.size(); ++k)
        free_slot(textured_layer_, free_textured_, textured_owner_, reuse_textured[k]);
}

void ViewerWindow::set_label(int slot, glaze::Shape shape) {
    label_pending_[slot] = std::move(shape);
    // Ändringarna samlas och sätts högst en gång var kLabelFlushMs.
    if (label_timer_.isRunning()) return;
    const auto since =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - label_flushed_);
    label_timer_.setInterval(std::max<int>(10, kLabelFlushMs - static_cast<int>(since.count())));
    label_timer_.start();
}

void ViewerWindow::flush_labels(bool force) {
    if (label_pending_.empty()) return;
    const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - label_flushed_);
    if (!force && since.count() < kLabelFlushMs) {
        if (!label_timer_.isRunning()) {
            label_timer_.setInterval(std::max<int>(10, kLabelFlushMs - static_cast<int>(since.count())));
            label_timer_.start();
        }
        return;
    }
    bool changed = false;
    for (auto& [slot, shape] : label_pending_) {
        if (static_cast<size_t>(slot) >= label_on_scene_.size())
            label_on_scene_.resize(static_cast<size_t>(slot) + 1, 0);
        const bool on = shape.kind() != glaze::Shape::Kind::None;
        // En tom plats som redan är tom ändrar ingenting.
        if (!on && !label_on_scene_[static_cast<size_t>(slot)]) continue;
        scene_.setShape(labels_layer_, slot, std::move(shape));
        label_on_scene_[static_cast<size_t>(slot)] = on ? 1 : 0;
        changed = true;
    }
    label_pending_.clear();
    if (changed) label_flushed_ = Clock::now();
}

void ViewerWindow::install_labels(TileView& v, uint32_t tile) {
    for (size_t k = 0; k < v.labels.size(); ++k) {
        // Över taket ritas resten av bitens texter inte; platserna står i
        // samma ordning som texterna.
        if (labels_shown_ >= kMaxLabels) {
            v.labels_dropped = v.labels.size() - k;
            labels_dropped_ += v.labels_dropped;
            break;
        }
        const TextLabel& label = v.labels[k];
        // En text för liten i bild ritas inte; mätningen visar den när den
        // växer.
        const bool on = have_view_ &&
                        last_view_.pixels_at(label.at, label.height) >= kLabelMinPixels &&
                        !clipped_away(active_clip(), label.at);
        const int slot = take_slot(labels_layer_, free_labels_, unused_owner_, {});
        set_label(slot, on ? label_shape(label) : glaze::Shape{});
        v.label_slots.push_back(slot);
        v.label_on.push_back(on ? 1 : 0);
        ++labels_shown_;
    }
    if (!v.label_slots.empty()) label_tiles_.insert(tile);
}

void ViewerWindow::release_labels(TileView& v, uint32_t tile) {
    labels_shown_ -= v.label_slots.size();
    for (int& s : v.label_slots) free_slot(labels_layer_, free_labels_, unused_owner_, s);
    labels_dropped_ -= v.labels_dropped;
    v.labels_dropped = 0;
    v.label_slots.clear();
    v.label_on.clear();
    label_tiles_.erase(tile);
}

void ViewerWindow::show_tile(uint32_t tile, const std::vector<uint32_t>& drawn) {
    const auto it = views_.find(tile);
    if (it == views_.end()) {
        inconsistent("visa", tile);
        return;
    }
    TileView& v = it->second;
    if (v.shown) return;
    v.shown = true;
    set_drawn(v, drawn);
    // Delarna: [0] ogenomskinligt, [1] genomskinligt, [2] linjer, sedan
    // texturerade. Meshen är densamma som förut, så en parkerad bit laddas inte
    // upp igen; en bit som aldrig visats får sina platser nu.
    if (!v.slotted) {
        place_parts(v, tile, nullptr);
        v.slotted = true;
    } else {
        for (size_t i = 0; i < v.parts.size(); ++i) put_part(v, i);
    }
    install_labels(v, tile);
    ++shown_tiles_;
    // Plockningen får veta att biten visas, hur mycket av den som ritas och
    // till vilken storlek den är byggd.
    pick_shared_->add(tile, drawn_vector(v.parts), v.min_size);
    prime_dirty_ = true;
}

void ViewerWindow::fit_tile(uint32_t tile, const std::vector<uint32_t>& drawn) {
    const auto it = views_.find(tile);
    if (it == views_.end() || !it->second.shown) return;   // släppt eller parkerad sedan dess
    TileView& v = it->second;
    for (size_t i = 0; i < v.parts.size(); ++i) {
        PartShape& p = v.parts[i];
        if (p.shape.kind() == glaze::Shape::Kind::None) continue;
        const uint64_t want = i < drawn.size() ? drawn[i] : 0;
        if (want == p.drawn_triangles) continue;
        shown_triangles_ = shown_triangles_ - p.drawn_triangles + want;
        p.drawn_triangles = want;
        put_part(v, i);
    }
    pick_shared_->set_drawn(tile, drawn_vector(v.parts));
}

void ViewerWindow::park_tile(uint32_t tile) {
    const auto it = views_.find(tile);
    if (it == views_.end() || !it->second.shown) {
        inconsistent("parkera", tile);
        return;
    }
    TileView& v = it->second;
    // Biten står kvar i sina platser och ritas med noll element: det ger inget
    // ritanrop och ingen uppladdning, och lagret byggs inte om.
    set_drawn(v, {});
    v.shown = false;
    --shown_tiles_;
    for (size_t i = 0; i < v.parts.size(); ++i) put_part(v, i);
    release_labels(v, tile);
    pick_shared_->remove(tile);
}

void ViewerWindow::release_slots(TileView& v) {
    if (!v.slotted) return;
    free_slot(opaque_layer_, free_opaque_, opaque_owner_, v.opaque_slot);
    free_slot(transparent_layer_, free_transparent_, transparent_owner_, v.transparent_slot);
    free_slot(line_layer_, free_lines_, line_owner_, v.line_slot);
    for (int& s : v.textured_slots) free_slot(textured_layer_, free_textured_, textured_owner_, s);
    v.textured_slots.clear();
    v.slotted = false;
}

void ViewerWindow::drop_tile(uint32_t tile) {
    const auto it = views_.find(tile);
    if (it == views_.end()) return;
    TileView& v = it->second;
    if (v.shown) {
        for (const PartShape& p : v.parts)
            if (p.shape.kind() != glaze::Shape::Kind::None) shown_triangles_ -= p.drawn_triangles;
        release_labels(v, tile);
        v.shown = false;
        --shown_tiles_;
        pick_shared_->remove(tile);
    }
    release_slots(v);
    retire(std::move(v.parts));
    views_.erase(it);
}

void ViewerWindow::retire(std::vector<PartShape>&& parts) {
    if (parts.empty()) return;
    retired_.push_back(Retired{Clock::now(), std::move(parts)});
    start_slices();
}

void ViewerWindow::flush_retired(bool all) {
    if (retired_.empty()) return;
    // Meshhandtagen dör i en arbetstråd. Ramverket håller egna referenser tills
    // nästa bilds förberedelse har bytt ut platsens form; väntan gör att
    // arbetstråden oftast är den som släpper den sista.
    std::vector<std::vector<PartShape>> batch;
    const auto now = Clock::now();
    while (!retired_.empty() &&
           (all || now - retired_.front().at >= std::chrono::milliseconds(kRetireDelayMs))) {
        batch.push_back(std::move(retired_.front().parts));
        retired_.pop_front();
    }
    if (batch.empty()) return;
    trash_worker_.run([batch = std::move(batch)]() mutable { batch.clear(); },
                      [](glaze::threads::Result<void>&) {});
}

void ViewerWindow::update_labels(const ViewFrustum& view) {
    if (!measured_) return;
    last_view_ = view;
    have_view_ = true;
    if (labels_shown_ == 0) return;
    for (const uint32_t tile : label_tiles_) {
        const auto it = views_.find(tile);
        if (it == views_.end()) continue;
        TileView& v = it->second;
        // Etiketterna står i samma ordning som platserna; de som inte fick
        // plats under taket har ingen.
        for (size_t k = 0; k < v.label_slots.size() && k < v.labels.size(); ++k) {
            const TextLabel& label = v.labels[k];
            const bool on = view.pixels_at(label.at, label.height) >= kLabelMinPixels &&
                            !clipped_away(active_clip(), label.at);
            if (on == (v.label_on[k] != 0)) continue;
            v.label_on[k] = on ? 1 : 0;
            set_label(v.label_slots[k], on ? label_shape(label) : glaze::Shape{});
        }
    }
}

void ViewerWindow::start_tree() {
    if (!model_ || tree_started_) return;
    tree_started_ = true;
    const uint64_t gen = generation_;
    const auto begin = Clock::now();
    glaze::threads::Worker* worker = &tree_loader_;
    // Trädet läses i modellens eget dokument medan bitarna byggs; jobbet
    // håller modellen vid liv också om en ny fil öppnas under tiden.
    tree_loader_.run(
        [this, worker, model = model_, gen](std::stop_token stop) -> TreeResult {
            auto last = Clock::now() - std::chrono::seconds(1);
            return load_tree(*model,
                             [&](const char* step, uint64_t done, uint64_t total) {
                                 const auto now = Clock::now();
                                 if (now - last >= std::chrono::milliseconds(200)) {
                                     last = now;
                                     char buf[96];
                                     if (total > 0)
                                         std::snprintf(buf, sizeof buf, "%s läses, %.0f %%", step,
                                                       100.0 * static_cast<double>(done) /
                                                           static_cast<double>(total));
                                     else
                                         std::snprintf(buf, sizeof buf, "%s läses", step);
                                     std::string text = buf;
                                     worker->post(stop, [this, gen, text] {
                                         if (gen != generation_) return;
                                         tree_progress_ = text;
                                         update_status();
                                     });
                                 }
                                 return !stop.stop_requested();
                             });
        },
        [this, gen, begin](glaze::threads::Result<TreeResult>& r) {
            const auto t0 = Clock::now();
            AtExit guard{[&] { stall("träd", t0); }};
            if (gen != generation_) return;
            const double s = std::chrono::duration<double>(Clock::now() - begin).count();
            tree_progress_.clear();
            if (!r) {
                tree_error_ = r.error();
            } else if (!r->loaded) {
                tree_error_ = r->error;
            } else {
                tree_ = model_->doc;
                tree_bytes_ =
                    r->memory_bytes > model_->memory_bytes ? r->memory_bytes - model_->memory_bytes : 0;
                models_ = std::move(r->models);
                props_ = std::make_shared<PropState>();
                props_->tree = tree_;
                props_->models = models_;
                fill_models();
                note("trädet läst på " + seconds_text(s) + " (" + std::to_string(models_.size()) +
                     " modeller, " + megabytes(r->memory_bytes) + " MB)");
            }
            if (!tree_) note("trädet lästes inte: " + tree_error_);
            if (selected_ != SIZE_MAX) select(selected_);
            update_status();
        });
}

void ViewerWindow::fill_models() {
    std::vector<glaze::Table::Row> rows;
    for (const ModelInfo& m : models_) {
        glaze::Table::Row row;
        row.cells = {m.name, m.unit_text, std::to_string(m.fragment_count)};
        rows.push_back(std::move(row));
    }
    model_list_.setRows(std::move(rows));
    for (size_t i = 0; i < models_.size(); ++i) model_list_.setChecked(static_cast<int>(i), true);
    hidden_models_.assign(models_.size(), 0);
    if (!models_.empty()) model_list_.show();
}

void ViewerWindow::toggle_model(int row) {
    if (!model_ || row < 0 || static_cast<size_t>(row) >= models_.size()) return;
    const auto t0 = Clock::now();
    AtExit guard{[&] { stall("modellval", t0); }};
    const bool hide = !model_list_.isChecked(row);
    if (hidden_models_[static_cast<size_t>(row)] == (hide ? 1 : 0)) return;
    hidden_models_[static_cast<size_t>(row)] = hide ? 1 : 0;

    // Filtret: de dolda modellernas fragmentintervall i växande ordning.
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    for (size_t i = 0; i < models_.size(); ++i)
        if (hidden_models_[i])
            ranges.push_back({models_[i].first_fragment,
                              models_[i].first_fragment + models_[i].fragment_count});
    std::sort(ranges.begin(), ranges.end());
    auto filter = std::make_shared<ModelFilter>();
    for (const auto& [a, b] : ranges) {
        filter->first.push_back(a);
        filter->end.push_back(b);
    }
    filter_ = std::move(filter);
    // Geometrin i plockningens cache har det gamla filtret.
    if (pick_cache_)
        trash_worker_.run([old = pick_cache_->clear()]() mutable { old.clear(); },
                          [](glaze::threads::Result<void>&) {});

    // De bitar som har modellens fragment byggs om; de hittas ur `tile_of` i
    // en arbetstråd.
    if (!lod_) return;
    const ModelInfo& mi = models_[static_cast<size_t>(row)];
    const uint32_t a = mi.first_fragment, b = mi.first_fragment + mi.fragment_count;
    struct Affected {
        std::vector<uint32_t> tiles;
    };
    const uint64_t gen = generation_;
    boxer_.run(
        [lod = lod_, a, b]() {
            Affected out;
            std::vector<uint8_t> seen(lod->tiles.size(), 0);
            const uint32_t end = static_cast<uint32_t>(std::min<size_t>(b, lod->tile_of.size()));
            for (uint32_t f = a; f < end; ++f) {
                const uint32_t t = lod->tile_of[f];
                if (t == HNwd::kNone || t >= seen.size() || seen[t]) continue;
                seen[t] = 1;
                out.tiles.push_back(t);
            }
            return out;
        },
        [this, gen](glaze::threads::Result<Affected>& r) {
            const auto t1 = Clock::now();
            AtExit guard2{[&] { stall("modellval", t1); }};
            if (gen != generation_) return;
            if (!r) {
                std::fprintf(stderr, "nwdview: modellvalet gick inte att räkna: %s\n",
                             std::string(r.error()).c_str());
                return;
            }
            // Bitarna ogiltigförklaras hos urvalet en omgång åt gången
            // (`run_queues`); urvalet släpper dem och mäter om.
            rebuild_queue_.insert(rebuild_queue_.end(), r->tiles.begin(), r->tiles.end());
            note("modellvalet: " + std::to_string(r->tiles.size()) + " bitar byggs om");
            if (r->tiles.empty()) {
                remeasure();
                update_status();
            } else {
                start_slices();
            }
        });
}

// Meshminnet mot budgeten: RAM och grafikkort var för sig, eller mot den
// gemensamma budgeten.
std::string ViewerWindow::mesh_memory_text() const {
    const PartBytes r = resident_total(), b = budget_;
    if (budget_shared_)
        return "RAM " + megabytes(r.ram) + " MB + grafikkort " + megabytes(r.gpu) + " MB av " +
               megabytes(b.ram) + " MB (delat minne)";
    return "RAM " + megabytes(r.ram) + "/" + megabytes(b.ram) + " MB, grafikkort " +
           megabytes(r.gpu) + "/" + megabytes(b.gpu) + " MB";
}

PartBytes ViewerWindow::resident_total() const {
    PartBytes r = lod_ ? stats_.resident : PartBytes{};
    r.ram += reader_bytes_ + pick_cache_bytes_ + lod_bytes_;
    return r;
}

std::string ViewerWindow::threshold_text() const {
    const double raised = stats_.threshold, wanted = options_.min_pixels;
    if (raised > wanted * 1.0001)
        return "allt ≥ " + pixels_text(raised) + " px syns (minnet räcker inte för " +
               pixels_text(wanted) + " px)";
    return "allt ≥ " + pixels_text(wanted) + " px syns";
}

std::string ViewerWindow::status_details() const {
    std::string t = std::to_string(model_->fragment_count) + " fragment";
    if (lod_) {
        t += " · bitar " + std::to_string(stats_.built) + "/" +
             std::to_string(lod_->tiles.size()) + " byggda, " + std::to_string(shown_tiles_) +
             " visade · " + millions(shown_triangles_) + " trianglar · " + threshold_text();
        const std::string drawing = draw_text();
        if (!drawing.empty()) t += ", " + drawing;
        t += " · meshar " + mesh_memory_text();
    }
    t += " · modellen " + megabytes(model_->memory_bytes) + " MB";
    if (points_) t += " · " + std::to_string(points_) + " punkter ritas inte";
    if (labels_shown_) t += " · " + std::to_string(labels_shown_) + " texter";
    if (texts_ + labels_dropped_)
        t += " · " + std::to_string(texts_ + labels_dropped_) + " texter ritas inte";
    if (segments_skipped_)
        t += " · " + std::to_string(segments_skipped_) + " linjesegment över taket";
    if (undecoded_) t += " · " + std::to_string(undecoded_) + " fragment avkodas inte";
    if (missing_textures_) {
        t += " · " + std::to_string(missing_textures_) + " bilder visas som färg";
        if (!failed_tiles_ && !build_error_.empty()) t += " (" + build_error_ + ")";
    }
    if (rotated_textures_)
        t += " · " + std::to_string(rotated_textures_) + " fragment med vriden bild";
    return t;
}

void ViewerWindow::update_status() {
    if (!model_) return;
    // Statusraden säger det som går att handla efter: läget, vad som syns,
    // minnet, det som pågår och det som gått fel. Resten står i dess tooltip.
    auto gigabytes = [](size_t bytes) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.1f", static_cast<double>(bytes) / (1024.0 * 1024 * 1024));
        return std::string(buf);
    };
    std::string t;
    auto add = [&t](const std::string& part) {
        if (!t.empty()) t += " · ";
        t += part;
    };
    if (walking_) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "Gångläge %.1f m/s", walk_.speed);
        std::string walk = buf;
        if (walk_.follow_ground) {
            std::snprintf(buf, sizeof buf, ", ögat %.2f m över ytan", walk_.eye_height);
            walk += buf;
        }
        add(walk);
    }
    if (lod_) {
        // Det som syns: den högre av minnets och ritningens tröskel, med skälet
        // när den är över den önskade.
        const double wanted = options_.min_pixels;
        const double memory = stats_.threshold;
        const double drawn = std::max(memory, draw_pixels_);
        std::string seen = "Visar allt ≥ " + pixels_text(drawn) + " px";
        if (drawn > wanted * 1.0001) {
            if (memory >= TileScheduler::Settings{}.max_pixels * 0.9999)
                seen += " (minnet räcker inte; också större saker kan saknas)";
            else if (drawn > memory * 1.0001)
                seen += " (för bildtakten)";
            else
                seen += " (minnet är fullt)";
        }
        add(seen);
        const PartBytes r = resident_total(), b = budget_;
        if (budget_shared_)
            add("minne " + gigabytes(r.ram + r.gpu) + "/" + gigabytes(b.ram) + " GB");
        else
            add("grafikminne " + gigabytes(r.gpu) + "/" + gigabytes(b.gpu) + " GB, RAM " +
                gigabytes(r.ram) + "/" + gigabytes(b.ram) + " GB");
        if (stats_.pending > 0) add("läser in");
    }
    if (failed_tiles_)
        add(std::to_string(failed_tiles_) + " delar gick inte att bygga: " + build_error_);
    if (!tree_progress_.empty()) add(tree_progress_);
    else if (tree_started_ && !tree_ && tree_error_.empty()) add("trädet läses");
    if (!tree_error_.empty()) add("trädet lästes inte: " + tree_error_);
    if (!model_->extras_error.empty()) add("lästes inte: " + model_->extras_error);
    if (view_clip_.count > 0) add(std::string("snitt ") + (options_.clip ? "på" : "av"));
    if (!clip_error_.empty()) add(clip_error_);
    status_.setText(t);
    // Tooltipen byts högst varannan sekund: ett byte gömmer en som visas.
    const auto now = Clock::now();
    if (now - status_details_at_ >= std::chrono::seconds(2)) {
        status_details_at_ = now;
        status_.setToolTip(status_details());
    }
}

double ViewerWindow::nearest_in_view(const ViewFrustum& view) const {
    // Bäst först genom filens rumsliga träd: lådan närmast ögat bland dem som
    // når in i bilden. Ett löv har fragment, så dess avstånd är en undre gräns
    // för avståndet till det närmaste som kan synas. Antalet besök har ett
    // tak; nås det gäller den gräns som hittats, som också är en undre gräns.
    const std::vector<SpatialCell>& nodes = model_->nodes;
    if (nodes.empty() || !nodes[0].bounds.valid()) return 0;
    using Entry = std::pair<double, uint32_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
    open.push({view.distance(nodes[0].bounds), 0});
    int visits = 0;
    while (!open.empty()) {
        const Entry top = open.top();
        open.pop();
        if (++visits > kNearSearchVisits) return top.first;
        const SpatialCell& n = nodes[top.second];
        if (n.child_count == 0) {
            if (n.item_count > 0) return top.first;
            continue;
        }
        for (uint32_t k = 0; k < n.child_count; ++k) {
            const uint32_t c = model_->children[n.first_child + k];
            const Box& b = nodes[c].bounds;
            if (!b.valid() || !view.sees(b)) continue;
            open.push({view.distance(b), c});
        }
    }
    return std::numeric_limits<double>::infinity();
}

void ViewerWindow::fit_orbit_near() {
    if (!model_ || walking_) return;
    glaze::Camera3D c = scene_.camera3D();
    const glaze::Point3 eye = c.eye();
    const double e[3] = {eye.x, eye.y, eye.z};
    // Avståndet från ögat till det närmaste i bilden, och till det bortersta
    // hörnet av modellens huvuddel.
    const Box& main = model_->view.valid() ? model_->view : model_->world;
    double gap = 0, far = 0;
    const gfx::Sizef size = scene_.actualSize();
    if (size.w > 0 && size.h > 0) {
        const ViewFrustum view =
            ViewFrustum::orbit(e, c.azimuth, c.elevation, c.fov, size.w, size.h);
        gap = nearest_in_view(view);
        // Inget i bilden: närplanet spelar ingen roll, och hålls måttligt.
        if (!std::isfinite(gap)) gap = 2 * kOrbitNearFloor;
    }
    if (main.valid()) {
        double sq = 0;
        for (int a = 0; a < 3; ++a) {
            const double d = std::max(std::abs(main.min[a] - e[a]), std::abs(main.max[a] - e[a]));
            sq += d * d;
        }
        far = std::sqrt(sq) * kOrbitFarMargin;
    }
    const double near = std::max(kOrbitNearFloor, gap * kOrbitNearOfGap);
    if (!(far > near * 2)) far = 0;   // ingen låda att räkna på: ramverkets eget
    // Att sätta kameran avbryter en pågående glidning. Medan kameran rör sig
    // sätts därför planen bara när närplanet är så stort att det klipper (mer
    // än det dubbla) eller bortre planet för nära; när den står still räcker
    // en märkbar skillnad.
    const bool near_off = std::abs(c.nearPlane - near) > 0.1 * near;
    const bool far_off = far > 0 ? std::abs(c.farPlane - far) > 0.1 * far : c.farPlane != 0;
    if (c.nearPlane > 0) {
        if (moving(Clock::now())) {
            const bool clips = c.nearPlane > 2 * near || (far > 0 && c.farPlane < far / 1.5);
            if (!clips) return;
        } else if (!near_off && !far_off) {
            return;
        }
    }
    c.nearPlane = near;
    c.farPlane = far;
    scene_.setCamera(c);
}

double ViewerWindow::ray_reach(const double origin[3]) const {
    // Så långt som modellens bortersta hörn, med lite marginal.
    const Box& w = model_->world;
    double far = 0;
    for (int k = 0; k < 8; ++k) {
        const double p[3] = {(k & 1) ? w.max[0] : w.min[0], (k & 2) ? w.max[1] : w.min[1],
                             (k & 4) ? w.max[2] : w.min[2]};
        far = std::max(far, std::sqrt((p[0] - origin[0]) * (p[0] - origin[0]) +
                                      (p[1] - origin[1]) * (p[1] - origin[1]) +
                                      (p[2] - origin[2]) * (p[2] - origin[2])));
    }
    return far * 1.01 + 1;
}

void ViewerWindow::update_coordinate(glaze::Point local) {
    last_coordinate_ = Clock::now();
    if (!model_ || !lod_) return;
    // Högst ett jobb åt gången; den senaste punkten väntar på sin tur.
    if (coord_busy_) {
        coord_pending_ = local;
        return;
    }
    run_pick(PickPurpose::Coordinate, local);
}

void ViewerWindow::pick(glaze::Point local) {
    if (!model_ || !lod_) return;
    run_pick(PickPurpose::Click, local);
}

void ViewerWindow::run_pick(PickPurpose purpose, glaze::Point local) {
    // Strålen genom pekaren tas här; träffprovet går i en arbetstråd.
    const std::optional<glaze::Ray3> ray = scene_.rayAt(local);
    if (!ray) {
        if (purpose == PickPurpose::Coordinate) {
            coordinate_.setText("");
        } else if (purpose == PickPurpose::Click) {
            scene_.setShapes(pick_layer_, {});
            selected_ = SIZE_MAX;
            properties_.setRows({});
        }
        return;
    }
    PickRay r;
    r.origin[0] = ray->origin.x;
    r.origin[1] = ray->origin.y;
    r.origin[2] = ray->origin.z;
    r.dir[0] = ray->direction.x;
    r.dir[1] = ray->direction.y;
    r.dir[2] = ray->direction.z;
    const double reach = ray_reach(r.origin);
    PickJob job;
    job.session = session_;
    job.lod = lod_;
    job.cache = pick_cache_;
    job.shared = pick_shared_;
    job.filter = filter_;
    job.boxes = boxes_cleared_ ? nullptr : box_list_;
    job.epoch = pick_cache_->current_epoch();
    job.generation = generation_;
    const ClipState clip = active_clip();
    const PickPurpose what = purpose;
    glaze::threads::Worker& worker =
        purpose == PickPurpose::Coordinate ? coord_worker_ : click_worker_;
    if (purpose == PickPurpose::Coordinate) {
        coord_busy_ = true;
    } else {
        // Det senaste klicket gäller.
        click_worker_.cancel();
        click_busy_ = true;
    }
    worker.run(
        [job, r, reach, clip, what](std::stop_token stop) -> PickAnswer {
            PickAnswer a = run_pick_job(job, r, reach, clip, true, stop);
            a.purpose = what;
            return a;
        },
        [this, what](glaze::threads::Result<PickAnswer>& res) {
            if (!res) {
                if (what == PickPurpose::Coordinate) coord_busy_ = false;
                else click_busy_ = false;
                std::fprintf(stderr, "nwdview: plockningen föll: %s\n",
                             std::string(res.error()).c_str());
                return;
            }
            on_pick_answer(*res);
        });
}

void ViewerWindow::run_target_pick(const double eye[3], const double forward[3]) {
    if (!model_ || !lod_) return;
    PickRay r;
    for (int k = 0; k < 3; ++k) {
        r.origin[k] = eye[k];
        r.dir[k] = forward[k];
    }
    PickJob job;
    job.session = session_;
    job.lod = lod_;
    job.cache = pick_cache_;
    job.shared = pick_shared_;
    job.filter = filter_;
    job.epoch = pick_cache_->current_epoch();
    job.generation = generation_;
    const ClipState clip = active_clip();
    const uint64_t epoch = ++target_epoch_;
    click_worker_.cancel();
    click_busy_ = true;
    click_worker_.run(
        [job, r, clip, epoch](std::stop_token stop) -> PickAnswer {
            PickAnswer a = run_pick_job(job, r, 1000, clip, true, stop);
            a.purpose = PickPurpose::Target;
            a.epoch = epoch;
            return a;
        },
        [this](glaze::threads::Result<PickAnswer>& res) {
            if (!res) {
                click_busy_ = false;
                std::fprintf(stderr, "nwdview: plockningen föll: %s\n",
                             std::string(res.error()).c_str());
                return;
            }
            on_pick_answer(*res);
        });
}

void ViewerWindow::on_pick_answer(PickAnswer& a) {
    const auto t0 = Clock::now();
    AtExit guard{[&] { stall("plockning", t0); }};
    if (a.generation != generation_ || !model_) return;
    const bool coordinate = a.purpose == PickPurpose::Coordinate;
    if (coordinate) coord_busy_ = false;
    else click_busy_ = false;
    if (!a.cancelled) {
        ++pick_jobs_;
        pick_ms_sum_ += a.seconds * 1000;
        pick_ms_max_ = std::max(pick_ms_max_, a.seconds * 1000);
    }
    if (!a.error.empty() && pick_error_.empty()) {
        pick_error_ = a.error;
        std::fprintf(stderr, "nwdview: plockningen: %s\n", a.error.c_str());
    }
    if (a.cancelled) {
        // Den senaste punkten väntar fortfarande.
        if (coordinate && coord_pending_) {
            const glaze::Point p = *coord_pending_;
            coord_pending_.reset();
            update_coordinate(p);
        }
        return;
    }
    switch (a.purpose) {
    case PickPurpose::Coordinate: {
        if (a.hit.hit) {
            char buf[160];
            std::snprintf(buf, sizeof buf, "X %.3f  Y %.3f  Z %.3f %s", a.hit.point[0],
                          a.hit.point[1], a.hit.point[2],
                          model_->unit_known ? "m" : "(filens enhet)");
            coordinate_.setText(buf);
        } else {
            coordinate_.setText("");
        }
        if (coord_pending_) {
            const glaze::Point p = *coord_pending_;
            coord_pending_.reset();
            update_coordinate(p);
        }
        break;
    }
    case PickPurpose::Click:
        if (a.hit.hit && a.hit.fragment != HNwd::kNone) {
            select(a.hit.fragment);
        } else if (a.box_hit) {
            scene_.setShapes(pick_layer_, {});
            selected_ = SIZE_MAX;
            properties_.setRows({text_row("Området", "är inte inläst än")});
        } else {
            scene_.setShapes(pick_layer_, {});
            selected_ = SIZE_MAX;
            properties_.setRows({});
        }
        break;
    case PickPurpose::Target: {
        // Orbiten kring ytan rakt fram: ögat står kvar och målet flyttas längs
        // blicken, om kameran inte rörts sedan gångläget lämnades.
        if (walking_ || a.epoch != target_epoch_ || !a.hit.hit) break;
        glaze::Camera3D c = scene_.camera3D();
        const glaze::Point3 e = c.eye();
        const double moved = std::hypot(e.x - target_eye_[0], std::hypot(e.y - target_eye_[1],
                                                                          e.z - target_eye_[2]));
        if (moved > 0.01) break;
        const double d = std::max(a.hit.distance, 1.0);
        c.target = {target_eye_[0] + target_forward_[0] * d, target_eye_[1] + target_forward_[1] * d,
                    target_eye_[2] + target_forward_[2] * d};
        c.distance = d;
        scene_.setCamera(c);
        request_update();
        break;
    }
    }
}

void ViewerWindow::select(size_t fragment) {
    if (!model_) return;
    selected_ = fragment;
    props_busy_ = true;
    properties_.setRows({text_row("Objektet", "läses…")});
    struct Selection {
        std::vector<PropertyRow> rows;
        Box box;
    };
    // Lådan, geometrin och egenskaperna läses i en arbetstråd; läsaren av
    // egenskaper hör till den tråden.
    prop_loader_.cancel();
    const uint64_t gen = generation_;
    prop_loader_.run(
        [model = model_, props = props_, fragment](std::stop_token stop) -> Selection {
            Selection s;
            s.box = object_box(*model, fragment);
            s.rows = describe_geometry(*model, fragment);
            if (stop.stop_requested()) return s;
            if (!props) {
                s.rows.push_back({"Objekt", {}, true});
                s.rows.push_back({"Trädet", "läses fortfarande", false});
                return s;
            }
            if (!props->opened) {
                if (props->reader.open(*props->tree) == HNwd::Status::Ok)
                    props->opened = true;
                else
                    props->open_error = props->reader.last_error();
            }
            if (!props->opened) {
                s.rows.push_back({"Objekt", {}, true});
                s.rows.push_back({"Egenskaperna", "kunde inte läsas: " + props->open_error, false});
                return s;
            }
            const uint32_t leaf = props->tree->node_of_fragment(fragment);
            std::vector<PropertyRow> object =
                describe_object(*props->tree, props->models, props->reader, leaf);
            s.rows.insert(s.rows.begin(), object.begin(), object.end());
            return s;
        },
        [this, gen, fragment](glaze::threads::Result<Selection>& r) {
            const auto t0 = Clock::now();
            AtExit guard{[&] { stall("egenskaper", t0); }};
            if (gen != generation_ || selected_ != fragment) return;
            props_busy_ = false;
            if (!r) {
                properties_.setRows({text_row("Fel", r.error())});
                return;
            }
            if (r->box.valid()) {
                glaze::MeshStyle highlight;
                highlight.color = gfx::Color{255, 170, 0, 255};
                highlight.opacity = 0.35f;
                highlight.lit = false;
                scene_.setShapes(pick_layer_, {glaze::Shape::box(to_box3(r->box), highlight)});
            }
            using K = glaze::PropertyGrid::Kind;
            std::vector<glaze::PropertyGrid::Row> rows;
            for (const PropertyRow& p : r->rows) {
                if (p.category) {
                    glaze::PropertyGrid::Row c;
                    c.name = p.name;
                    c.kind = K::Category;
                    rows.push_back(std::move(c));
                } else {
                    rows.push_back(text_row(p.name, p.value));
                }
            }
            properties_.setRows(std::move(rows));
        });
}

void ViewerWindow::png_tick() {
    if (load_failed_) {
        result_ = 1;
        png_timer_.stop();
        close();
        return;
    }
    if (!model_ || !lod_) return;
    // Allt som ska visas är visat: urvalets tråd har svarat på allt som lämnats
    // och sagt att inget byggs, ingen bit återstår att visa, parkera eller sätta
    // ritad andel för, ingen operation väntar här och inget modellval räknas.
    // Det bedöms före den nya mätningen, vars svar bedöms nästa gång.
    const bool calm = selector_calm();
    remeasure();
    note_base();
    if (!base_noted_ || !calm || boxer_.busy() || !queues_empty()) {
        png_stable_ = 0;
        return;
    }
    // Med --profile ska postanvändningen vara klar, så att rapporten är hel.
    if (options_.profile && use_busy_) {
        png_stable_ = 0;
        return;
    }
    // Några kontroller i följd utan arbete, och den senaste bilden hade
    // inget kvar att ladda upp till grafikkortet.
    const std::vector<glaze::FrameProfile> last = glaze::profiler().frames(*this, 1);
    if (!last.empty() && last.back().count[size_t(glaze::ProfileCount::UploadBreaks)] > 0) {
        png_stable_ = 0;
        return;
    }
    if (walk_running_ || spin_running_) {
        png_stable_ = 0;
        return;
    }
    if (++png_stable_ < 3) return;
    png_stable_ = 0;
    if (options_.walk_distance > 0 && walk_report_.empty()) {
        // Gången: rakt fram i gångfart, med verklig tid per bildruta.
        if (!walking_) set_walking(true);
        walk_left_ = options_.walk_distance;
        walk_running_ = true;
        walk_began_ = Clock::now();
        walk_time_ = walk_probe_ms_ = walk_probe_max_ms_ = walk_tick_max_ = 0;
        walk_job_ms_ = walk_job_max_ms_ = 0;
        walk_ticks_ = walk_probes_ = walk_jobs_ = walk_holes_max_ = walk_holes_sum_ =
            walk_hole_ticks_ = walk_samples_ = 0;
        walk_vertices_ = rendered_vertices();
        draw_pixels_max_ = draw_pixels_;
        char buf[160];
        std::snprintf(buf, sizeof buf, "gången börjar: %.1f m i %.2f m/s%s", walk_left_,
                      walk_.speed, walk_.follow_ground ? ", med höjden över ytan" : "");
        note(buf);
        scene_.setTicking(true);
        return;
    }
    if (options_.spin > 0 && spin_report_.empty()) {
        start_spin();
        return;
    }
    if (options_.tour && tour_phase_ < 2) {
        glaze::Camera3D c = scene_.camera3D();
        if (tour_phase_ == 0) {
            note("rundan: in (" + std::to_string(stats_.built) + " bitar byggda, " +
                 std::to_string(shown_tiles_) + " visade, " +
                 std::to_string(stats_.evictions) + " släppta)");
            c.distance /= 6;
            c.azimuth += 70;
        } else {
            note("rundan: ut (" + std::to_string(stats_.built) + " bitar byggda, " +
                 std::to_string(shown_tiles_) + " visade, " +
                 std::to_string(stats_.evictions) + " släppta)");
            c = start_camera_;
        }
        ++tour_phase_;
        scene_.setCamera(c);
        remeasure();
        return;
    }
    png_timer_.stop();
    note_stalls("före bilden");
    note("statusraden: " + status_.text());
    note("statusradens detaljer: " + status_details());
    note("bilden ritas (" + std::to_string(stats_.built) + " bitar byggda, " +
         std::to_string(shown_tiles_) + " visade, " + std::to_string(stats_.evictions) +
         " släppta, " + ram_gpu(resident_total()) + "; " + threshold_text() + ")");
    note_profile("före bilden");
    begin_capture(options_.png, [this] {
        if (!options_.profile) {
            close();
            return;
        }
        // Med --profile: trädet och ett klick mitt i bilden, och i en fil med
        // flera modeller den första dold och en bild till, så att de vägarna
        // också mäts.
        start_tree();
        profile_step_ = 0;
        png_timer_.onTick = [this] { profile_tick(); };
        png_timer_.start();
    });
}

void ViewerWindow::profile_tick() {
    if (!(tree_started_ && (tree_ || !tree_error_.empty()))) return;
    if (profile_step_ == 0) {
        profile_step_ = 1;
        const gfx::Sizef size = scene_.actualSize();
        pick({size.w / 2.0, size.h / 2.0});
        return;
    }
    if (profile_step_ == 1) {
        // Klicket svarar från en arbetstråd, och egenskaperna läses i en annan.
        if (click_busy_ || props_busy_) return;
        if (selected_ == SIZE_MAX) timings_ += "klick mitt i bilden: ingenting träffades\n";
        if (selected_ != SIZE_MAX) {
            std::string text = "klick mitt i bilden:\n";
            for (const glaze::PropertyGrid::Row& r : properties_.rows())
                text += "    " + r.name + (r.value.empty() ? "" : " = " + r.value) + "\n";
            timings_ += text;
        }
        if (!models_.empty()) {
            timings_ += "modeller:\n";
            for (const ModelInfo& m : models_)
                timings_ += "    " + m.name + " · " + m.unit_text + " · " +
                            std::to_string(m.fragment_count) + " fragment\n";
        }
        if (models_.size() < 2) {
            png_timer_.stop();
            close();
            return;
        }
        profile_step_ = 2;
        note("modellen " + models_[0].name + " döljs");
        model_list_.setChecked(0, false);
        toggle_model(0);
        png_stable_ = 0;
        return;
    }
    // Den dolda modellen: vänta tills allt är ombyggt, rita en bild till.
    const bool calm = selector_calm();
    remeasure();
    if (!calm || boxer_.busy() || !queues_empty()) {
        png_stable_ = 0;
        return;
    }
    if (++png_stable_ < 3) return;
    png_timer_.stop();
    note("modellen dold: " + std::to_string(stats_.built) + " bitar byggda, " +
         std::to_string(shown_tiles_) + " visade, " + millions(shown_triangles_) + " trianglar");
    std::string path = options_.png;
    const size_t dot = path.find_last_of('.');
    path = (dot == std::string::npos ? path : path.substr(0, dot)) + ".dold.png";
    const int before = result_;
    begin_capture(path, [this, before] {
        // Den första bildens fel står kvar.
        if (before != 0) result_ = before;
        close();
    });
}

void ViewerWindow::begin_capture(const std::string& path, std::function<void()> then) {
    capture_path_ = path;
    capture_then_ = std::move(then);
    capture_last_.reset();
    capture_tries_ = 0;
    capture_started_ = Clock::now();
    // En bild som ritytan ännu inte laddat upp saknas i bilden; budgeten
    // höjs så att allt hinner upp, och en bildruta ritas med den.
    capturing_ = true;
    upload_budget_ = glaze::app().canvasUploadBudget();
    glaze::app().setCanvasUploadBudget(kCaptureUploadBudget);
    scene_.invalidate();
    png_timer_.stop();
    png_timer_.onTick = [this] { capture_tick(); };
    png_timer_.start();
}

void ViewerWindow::capture_tick() {
    const int w = kCaptureWidth, h = kCaptureHeight;
    const size_t bytes = static_cast<size_t>(w) * h * 4;
    std::optional<glaze::Bitmap> image = scene_.renderToBitmap(w, h, scene_.camera3D());
    ++capture_tries_;
    const double waited = std::chrono::duration<double>(Clock::now() - capture_started_).count();
    // Två ritningar med samma kamera skiljer sig i enstaka pixlar där ytor i
    // samma plan möts; en bild som saknas syns som en yta. Lika är därför
    // högst en pixel på tiotusen som skiljer.
    size_t diff = SIZE_MAX;
    if (image && image->valid() && capture_last_ && capture_last_->valid()) {
        diff = 0;
        for (size_t i = 0; i < bytes; i += 4)
            if (std::memcmp(image->pixels() + i, capture_last_->pixels() + i, 4) != 0) ++diff;
    }
    const bool same = diff <= static_cast<size_t>(w) * h / kCaptureNoiseDivisor;
    const bool timed_out = !same && waited > kCaptureTimeout;
    if (!same && !timed_out) {
        // Bilden ändrades: något laddades upp sedan förra. En ny bildruta
        // och ett nytt försök.
        capture_last_ = std::move(image);
        scene_.invalidate();
        return;
    }
    png_timer_.stop();
    glaze::app().setCanvasUploadBudget(upload_budget_);
    capturing_ = false;
    result_ = 1;
    const glaze::Bitmap* out = image && image->valid() ? &*image
                               : capture_last_ && capture_last_->valid() ? &*capture_last_
                                                                          : nullptr;
    if (timed_out) {
        std::fprintf(stderr,
                     "nwdview: %s: bilden blev inte stabil på %.0f s (%d försök) — bilder ur "
                     "materialen kan saknas i den\n",
                     capture_path_.c_str(), kCaptureTimeout, capture_tries_);
    }
    if (!out) {
        std::fprintf(stderr, "nwdview: %s: ritytan gav ingen bild\n", capture_path_.c_str());
    } else {
        const std::vector<std::uint8_t> pixels(out->pixels(), out->pixels() + bytes);
        if (gfx::encodePng(capture_path_, pixels, w, h))
            result_ = timed_out ? 1 : 0;
        else
            std::fprintf(stderr, "nwdview: %s kunde inte skrivas\n", capture_path_.c_str());
    }
    char buf[160];
    std::snprintf(buf, sizeof buf, "bilden tagen efter %d försök på %.2f s%s", capture_tries_,
                  waited, timed_out ? " (tidsgränsen)" : "");
    note(buf);
    auto then = std::move(capture_then_);
    capture_then_ = nullptr;
    if (then) then();
}

bool ViewerWindow::walk_key(gfx::Key k) const {
    using K = gfx::Key;
    switch (k) {
    case K::W: case K::A: case K::S: case K::D: case K::Q: case K::E: case K::R: case K::F:
    case K::Up: case K::Down: case K::Left: case K::Right: case K::PageUp: case K::PageDown:
        return true;
    default:
        return false;
    }
}

void ViewerWindow::set_walking(bool on) {
    if (!model_ || on == walking_) return;
    walking_ = on;
    looking_ = false;
    skip_ahead_ = true;
    if (on) {
        // Ögat där orbitens öga står, blicken som dess.
        const glaze::Camera3D c = scene_.camera3D();
        const glaze::Point3 e = c.eye();
        walk_.eye[0] = e.x;
        walk_.eye[1] = e.y;
        walk_.eye[2] = e.z;
        walk_.yaw = c.azimuth;
        walk_.pitch = std::clamp(-static_cast<double>(c.elevation), -walk_settings_.max_pitch,
                                 walk_settings_.max_pitch);
        scene_.setPannable(false);
        scene_.setZoomable(false);
        scene_.setRotatable(false);
        scene_.setPitchable(false);
        scene_.focus();
        // Marksökningen börjar om: bitarna under ögat hålls i cachen och ytan
        // söks när den första tickan frågar.
        ground_ = Ground{};
        ground_force_ = true;
        prime_dirty_ = true;
        prime_have_ = false;
        apply_walk_camera();
    } else {
        // Orbiten kring en punkt framför ögat: tio meter fram till att ytan
        // rakt fram är funnen (ett jobb i en arbetstråd), då flyttas målet dit
        // om kameran inte rörts under tiden.
        double f[3];
        walk_forward(walk_, f);
        const double d = 10;
        glaze::Camera3D c;
        c.target = {walk_.eye[0] + f[0] * d, walk_.eye[1] + f[1] * d, walk_.eye[2] + f[2] * d};
        c.distance = d;
        c.azimuth = static_cast<float>(walk_.yaw);
        c.elevation = static_cast<float>(-walk_.pitch);
        scene_.setCamera(c);
        for (int k = 0; k < 3; ++k) {
            target_eye_[k] = walk_.eye[k];
            target_forward_[k] = f[k];
        }
        run_target_pick(walk_.eye, f);
        scene_.setTicking(false);
        scene_.setPannable(true);
        scene_.setZoomable(true);
        scene_.setRotatable(true);
        scene_.setPitchable(true);
    }
    menuBar.setItemChecked({"Visa", "Gångläge"}, on);
    note(on ? "gångläget på" : "gångläget av");
    update_status();
    request_update();
}

void ViewerWindow::apply_walk_camera() {
    // Det bortre klipplanet når modellens bortersta hörn.
    const Box& w = model_->world;
    double far = 0;
    for (int k = 0; k < 8; ++k) {
        const double p[3] = {(k & 1) ? w.max[0] : w.min[0], (k & 2) ? w.max[1] : w.min[1],
                             (k & 4) ? w.max[2] : w.min[2]};
        far = std::max(far, std::sqrt((p[0] - walk_.eye[0]) * (p[0] - walk_.eye[0]) +
                                      (p[1] - walk_.eye[1]) * (p[1] - walk_.eye[1]) +
                                      (p[2] - walk_.eye[2]) * (p[2] - walk_.eye[2])));
    }
    walk_far_ = std::max(far * 1.05 + 10, 100.0);
    glaze::Camera3D c = glaze::Camera3D::lookFrom({walk_.eye[0], walk_.eye[1], walk_.eye[2]},
                                                  static_cast<float>(walk_.yaw),
                                                  static_cast<float>(walk_.pitch));
    c.nearPlane = kWalkNear;
    c.farPlane = walk_far_;
    scene_.setCamera(c);
    request_update();
}

std::optional<double> ViewerWindow::ground_below(const double from[3], double reach) {
    // Svaret är ytan som den senaste sökningen fann (ett jobb i en arbetstråd
    // som läser ur plockningens cache); här bara ett uppslag. En ny sökning
    // begärs för ögats läge nu.
    request_ground(from, reach);
    if (!ground_.known || !ground_.surface) return std::nullopt;
    const double dx = from[0] - ground_.x, dy = from[1] - ground_.y;
    if (dx * dx + dy * dy > kGroundStale * kGroundStale) return std::nullopt;
    const double d = std::max(from[2] - ground_.z, 0.0);
    if (d > reach) return std::nullopt;
    return d;
}

void ViewerWindow::request_ground(const double from[3], double reach) {
    if (!model_ || !lod_ || !pick_cache_) return;
    // Bitarna under ögat hålls i cachen: ett jobb som byggs om när ögat flyttat
    // sig märkbart eller fler bitar visats.
    if (!prime_busy_) {
        const double dx = from[0] - prime_origin_[0], dy = from[1] - prime_origin_[1];
        if (!prime_have_ || prime_dirty_ ||
            dx * dx + dy * dy > kGroundPrimeMove * kGroundPrimeMove)
            start_ground_prime(from, reach);
    }
    for (int k = 0; k < 3; ++k) ground_next_[k] = from[k];
    ground_next_reach_ = reach;
    if (ground_busy_) {
        ground_want_ = true;
        return;
    }
    const double dx = from[0] - ground_.x, dy = from[1] - ground_.y, dz = from[2] - ground_.oz;
    const bool moved =
        !ground_.known || dx * dx + dy * dy + dz * dz > kGroundProbeMove * kGroundProbeMove;
    if (moved || ground_force_) start_ground_probe(from, reach);
}

void ViewerWindow::start_ground_probe(const double from[3], double reach) {
    PickJob job;
    job.session = session_;
    job.lod = lod_;
    job.cache = pick_cache_;
    job.shared = pick_shared_;
    job.epoch = pick_cache_->current_epoch();
    job.generation = generation_;
    ground_busy_ = true;
    ground_want_ = false;
    ground_force_ = false;
    const double origin[3] = {from[0], from[1], from[2]};
    ground_worker_.run(
        [job, origin = std::to_array(origin), reach](std::stop_token stop) -> GroundAnswer {
            return run_ground_job(job, origin.data(), reach, stop);
        },
        [this](glaze::threads::Result<GroundAnswer>& r) {
            if (!r) {
                ground_busy_ = false;
                std::fprintf(stderr, "nwdview: marksökningen föll: %s\n",
                             std::string(r.error()).c_str());
                return;
            }
            on_ground_answer(*r);
        });
}

void ViewerWindow::on_ground_answer(GroundAnswer& a) {
    const auto t0 = Clock::now();
    AtExit guard{[&] { stall("marksökning", t0); }};
    if (a.generation != generation_) return;
    ground_busy_ = false;
    ground_.known = true;
    ground_.surface = a.surface;
    ground_.x = a.origin[0];
    ground_.y = a.origin[1];
    ground_.z = a.z;
    ground_.oz = a.origin[2];
    ground_cache_gen_ = a.cache_generation;
    ++walk_jobs_;
    walk_job_ms_ += a.seconds * 1000;
    walk_job_max_ms_ = std::max(walk_job_max_ms_, a.seconds * 1000);
    // Ögat har flyttat sig medan sökningen gick: sök igen för dess läge nu.
    if (walking_ && ground_want_) {
        ground_want_ = false;
        const double dx = ground_next_[0] - ground_.x, dy = ground_next_[1] - ground_.y,
                     dz = ground_next_[2] - ground_.oz;
        if (dx * dx + dy * dy + dz * dz > kGroundProbeMove * kGroundProbeMove)
            start_ground_probe(ground_next_, ground_next_reach_);
    }
}

void ViewerWindow::start_ground_prime(const double eye[3], double reach) {
    PickJob job;
    job.session = session_;
    job.lod = lod_;
    job.cache = pick_cache_;
    job.shared = pick_shared_;
    job.filter = filter_;
    job.epoch = pick_cache_->current_epoch();
    job.generation = generation_;
    prime_busy_ = true;
    prime_dirty_ = false;
    prime_have_ = true;
    for (int k = 0; k < 3; ++k) prime_origin_[k] = eye[k];
    const double origin[3] = {eye[0], eye[1], eye[2]};
    prime_worker_.run(
        [job, origin = std::to_array(origin), reach](std::stop_token stop) -> PrimeAnswer {
            return run_prime_job(job, origin.data(), reach, stop);
        },
        [this](glaze::threads::Result<PrimeAnswer>& r) {
            if (!r) {
                prime_busy_ = false;
                std::fprintf(stderr, "nwdview: bitarna under ögat gick inte att läsa: %s\n",
                             std::string(r.error()).c_str());
                return;
            }
            on_prime_answer(*r);
        });
}

void ViewerWindow::on_prime_answer(PrimeAnswer& a) {
    const auto t0 = Clock::now();
    AtExit guard{[&] { stall("marken", t0); }};
    if (a.generation != generation_) return;
    prime_busy_ = false;
    if (!a.error.empty() && pick_error_.empty()) {
        pick_error_ = a.error;
        std::fprintf(stderr, "nwdview: plockningen: %s\n", a.error.c_str());
    }
    // Fler bitar väntar på sin geometri; och med ny geometri söks ytan igen.
    if (a.more) prime_dirty_ = true;
    if (a.built > 0) ground_force_ = true;
}

void ViewerWindow::walk_tick(double seconds) {
    tick_at_ = Clock::now();
    if (spin_running_) {
        spin_tick(seconds);
        return;
    }
    if (!walking_ || !model_) {
        scene_.setTicking(false);
        return;
    }
    const auto t0 = Clock::now();
    WalkInput in;
    in.seconds = seconds;
    in.keys.forward = isKeyHeld(gfx::Key::W) || isKeyHeld(gfx::Key::Up);
    in.keys.back = isKeyHeld(gfx::Key::S) || isKeyHeld(gfx::Key::Down);
    in.keys.left = isKeyHeld(gfx::Key::A) || isKeyHeld(gfx::Key::Left);
    in.keys.right = isKeyHeld(gfx::Key::D) || isKeyHeld(gfx::Key::Right);
    in.keys.turn_left = isKeyHeld(gfx::Key::Q);
    in.keys.turn_right = isKeyHeld(gfx::Key::E);
    in.keys.up = isKeyHeld(gfx::Key::R) || isKeyHeld(gfx::Key::PageUp);
    in.keys.down = isKeyHeld(gfx::Key::F) || isKeyHeld(gfx::Key::PageDown);
    in.keys.run = (modifierKeys() & gfx::kModShift) != 0;
    if (walk_running_) {
        in.keys = WalkKeys{};
        in.keys.forward = true;
    }
    const double before[2] = {walk_.eye[0], walk_.eye[1]};
    const double height_before = walk_.eye_height;
    const GroundProbe probe = [this](const double from[3], double reach) {
        const auto p0 = Clock::now();
        std::optional<double> d = ground_below(from, reach);
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - p0).count();
        walk_probe_ms_ += ms;
        walk_probe_max_ms_ = std::max(walk_probe_max_ms_, ms);
        ++walk_probes_;
        return d;
    };
    if (walk_step(walk_, in, walk_settings_, probe)) apply_walk_camera();
    if (walk_.eye_height != height_before) update_status();
    if (walk_running_) {
        ++walk_ticks_;
        walk_time_ += seconds;
        walk_tick_max_ = std::max(walk_tick_max_, seconds);
        walk_left_ -= std::hypot(walk_.eye[0] - before[0], walk_.eye[1] - before[1]);
        if (walk_left_ <= 0) finish_walk();
    }
    stall("gångsteg", t0);
}

void ViewerWindow::finish_walk() {
    walk_running_ = false;
    scene_.setTicking(false);
    const double wall = std::chrono::duration<double>(Clock::now() - walk_began_).count();
    const double samples = static_cast<double>(std::max<uint64_t>(walk_samples_, 1));
    const TileScheduler::Latency latency = stats_.latency;
    char buf[1024];
    std::snprintf(
        buf, sizeof buf,
        "gången klar: %.1f m på %.2f s (%llu bildrutor, längsta tidssteg %.0f ms, väggtid "
        "%.2f s); synliga bitar som är minst två gånger tröskeln i bild och inte byggda: högst "
        "%llu, i snitt %.2f per mätning, %llu av %llu mätningar med något saknat; tiden från att "
        "en bit behövs tills den är byggd, sedan filen öppnades: p50 %.0f ms, p95 %.0f ms, "
        "högst %.0f ms (%zu bitar); strålen ned: %llu frågor i UI-tråden (i snitt %.4f ms, högst "
        "%.4f ms), %llu sökningar i arbetstråd (i snitt %.3f ms, högst %.3f ms); ögat "
        "%.2f %.2f %.2f",
        options_.walk_distance, walk_time_, static_cast<unsigned long long>(walk_ticks_),
        walk_tick_max_ * 1000, wall, static_cast<unsigned long long>(walk_holes_max_),
        static_cast<double>(walk_holes_sum_) / samples,
        static_cast<unsigned long long>(walk_hole_ticks_),
        static_cast<unsigned long long>(walk_samples_), latency.p50 * 1000, latency.p95 * 1000,
        latency.max * 1000, latency.count, static_cast<unsigned long long>(walk_probes_),
        walk_probes_ ? walk_probe_ms_ / static_cast<double>(walk_probes_) : 0.0,
        walk_probe_max_ms_, static_cast<unsigned long long>(walk_jobs_),
        walk_jobs_ ? walk_job_ms_ / static_cast<double>(walk_jobs_) : 0.0, walk_job_max_ms_,
        walk_.eye[0], walk_.eye[1], walk_.eye[2]);
    walk_report_ = buf;
    note(walk_report_);
    note_frames("gången", wall, walk_vertices_);
    note_profile("efter gången");
    // Bilden som den ser ut när gången slutar, innan detaljen hunnit ikapp.
    if (options_.png.empty()) return;
    std::string path = options_.png;
    const size_t dot = path.find_last_of('.');
    path = (dot == std::string::npos ? path : path.substr(0, dot)) + ".gang.png";
    const int w = kCaptureWidth, h = kCaptureHeight;
    const std::optional<glaze::Bitmap> image = scene_.renderToBitmap(w, h, scene_.camera3D());
    if (!image || !image->valid()) return;
    const std::vector<std::uint8_t> pixels(image->pixels(),
                                           image->pixels() + static_cast<size_t>(w) * h * 4);
    if (!gfx::encodePng(path, pixels, w, h))
        std::fprintf(stderr, "nwdview: %s kunde inte skrivas\n", path.c_str());
}

void ViewerWindow::note_walk_sample(size_t holes) {
    // Bitar i bild som är minst två gånger tröskeln stora och inte byggda,
    // räknade av urvalet vid mätningen.
    if (!lod_) return;
    walk_holes_max_ = std::max<uint64_t>(walk_holes_max_, holes);
    walk_holes_sum_ += holes;
    ++walk_samples_;
    if (holes) ++walk_hole_ticks_;
}

void ViewerWindow::start_spin() {
    // Orbiten: ögat kretsar kring samma mål med samma avstånd och lutning.
    if (walking_) set_walking(false);
    spin_start_azimuth_ = scene_.camera3D().azimuth;
    spin_left_ = options_.spin * 360;
    spin_angle_ = 0;
    spin_time_ = spin_tick_max_ = 0;
    spin_ticks_ = 0;
    spin_running_ = true;
    spin_began_ = Clock::now();
    spin_vertices_ = rendered_vertices();
    draw_pixels_max_ = draw_pixels_;
    char buf[160];
    std::snprintf(buf, sizeof buf, "snurran börjar: %.2f varv, %.0f s per varv", options_.spin,
                  kSpinSecondsPerTurn);
    note(buf);
    scene_.setTicking(true);
}

void ViewerWindow::spin_tick(double seconds) {
    const auto t0 = Clock::now();
    ++spin_ticks_;
    spin_time_ += seconds;
    spin_tick_max_ = std::max(spin_tick_max_, seconds);
    // Verklig tid per bildruta, men ett långt uppehåll ger inget hopp.
    const double step = 360.0 / kSpinSecondsPerTurn * std::min(seconds, 0.5);
    spin_angle_ = std::min(spin_angle_ + step, spin_left_);
    glaze::Camera3D c = scene_.camera3D();
    c.azimuth = static_cast<float>(std::fmod(spin_start_azimuth_ + spin_angle_, 360.0));
    scene_.setCamera(c);
    // Sättarna fyrar inte onCameraChange.
    request_update();
    if (spin_angle_ >= spin_left_) finish_spin();
    stall("snurrsteg", t0);
}

void ViewerWindow::finish_spin() {
    spin_running_ = false;
    scene_.setTicking(false);
    const double wall = std::chrono::duration<double>(Clock::now() - spin_began_).count();
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  "snurran klar: %.2f varv på %.2f s (%llu bildrutor, längsta tidssteg %.0f ms, "
                  "väggtid %.2f s)",
                  options_.spin, spin_time_, static_cast<unsigned long long>(spin_ticks_),
                  spin_tick_max_ * 1000, wall);
    spin_report_ = buf;
    note(spin_report_);
    note_frames("snurran", wall, spin_vertices_);
    note_profile("efter snurran");
}

uint64_t ViewerWindow::rendered_vertices() {
    return window().renderer().stats().vertices;
}

ViewerWindow::GpuInfo ViewerWindow::window_gpu() {
    GpuInfo info;
    const gfx::Renderer& renderer = window().renderer();
    info.name = renderer.deviceName();
    const std::string& id = renderer.adapterId();
    for (const gfx::GpuAdapter& a : gfx::gpuAdapters()) {
        const bool same = !id.empty() ? a.id == id : !info.name.empty() && a.name == info.name;
        if (!same) continue;
        info.memory = a.memoryBytes;
        // Ett diskret kort med känt minne har eget; allt annat räknas som
        // delat med processorn.
        info.shared = a.kind != gfx::GpuKind::Discrete || a.memoryBytes == 0;
        break;
    }
    return info;
}

void ViewerWindow::note_frames(const char* what, double seconds, uint64_t vertices) {
    const glaze::WindowSummary s = glaze::profiler().summary(*this, seconds);
    const uint64_t now = rendered_vertices();
    const double frames = static_cast<double>(std::max<size_t>(s.frames, 1));
    // Renderarens ritade hörn är index i de indexerade anropen: tre per
    // triangel.
    const double triangles =
        now > vertices ? static_cast<double>(now - vertices) / 3.0 / frames : 0.0;
    auto p50 = [&](glaze::ProfilePhase p) { return static_cast<double>(s.phaseP50[size_t(p)]); };
    auto p95 = [&](glaze::ProfilePhase p) { return static_cast<double>(s.phaseP95[size_t(p)]); };
    char buf[768];
    std::snprintf(
        buf, sizeof buf,
        "bildtakt %s (%s): %zu bilder, %zu med intervall; intervall p50 %.1f, p95 %.1f, p99 %.1f, "
        "högst %.1f ms; %zu över 20 ms (missed50Hz), %zu över 6,7 ms; %.0f ritanrop och %.0f "
        "trianglar per bild; innehåll p50 %.1f p95 %.1f ms, ritytan p50 %.1f p95 %.1f ms, "
        "present-väntan p50 %.1f ms, gpu p50 %.1f p95 %.1f ms",
        what, s.deviceName.c_str(), s.frames, s.paced, s.intervalP50, s.intervalP95,
        s.intervalP99, s.intervalMax, s.missed50Hz, s.missed150Hz,
        s.perFrame[size_t(glaze::ProfileCount::DrawCalls)], triangles,
        p50(glaze::ProfilePhase::Content), p95(glaze::ProfilePhase::Content),
        p50(glaze::ProfilePhase::CanvasDraw), p95(glaze::ProfilePhase::CanvasDraw),
        p50(glaze::ProfilePhase::PresentWait), p50(glaze::ProfilePhase::Gpu),
        p95(glaze::ProfilePhase::Gpu));
    note(buf);
    // Alla faser och räknare med ramverkets namn: var i bilden tiden går.
    std::string all = std::string("bildens faser ") + what + ", p50/p95 ms:";
    for (size_t i = 0; i < size_t(glaze::ProfilePhase::Count_); ++i) {
        std::snprintf(buf, sizeof buf, " %s %.2f/%.2f",
                      glaze::profilePhaseName(static_cast<glaze::ProfilePhase>(i)),
                      static_cast<double>(s.phaseP50[i]), static_cast<double>(s.phaseP95[i]));
        all += buf;
    }
    all += "; per bild:";
    for (size_t i = 0; i < size_t(glaze::ProfileCount::Count_); ++i) {
        std::snprintf(buf, sizeof buf, " %s %.2f",
                      glaze::profileCountName(static_cast<glaze::ProfileCount>(i)),
                      s.perFrame[i]);
        all += buf;
    }
    note(all);
}

void ViewerWindow::note_governor(const char* when) {
    char buf[768];
    std::snprintf(
        buf, sizeof buf,
        "ritningens tröskel (%s): senaste %s px, högsta %s px (minnets %s px, garanti %s px); "
        "uppskattad belastning %s trianglar och %u ritanrop (taken %s trianglar och %u "
        "ritanrop); kantutjämningen sänkt %llu gånger%s; %llu byggjobb avbrutna; %llu platser "
        "omordnade (närmast först); %llu bitar ombyggda för ett nytt K",
        when, pixels_text(draw_pixels_).c_str(), pixels_text(draw_pixels_max_).c_str(),
        pixels_text(stats_.threshold).c_str(), pixels_text(options_.min_pixels).c_str(),
        millions(last_load_.triangles).c_str(), last_load_.draws,
        millions(options_.governor.max_triangles).c_str(), options_.governor.max_draws,
        static_cast<unsigned long long>(aa_reductions_), aa_low_ ? " (är sänkt nu)" : "",
        static_cast<unsigned long long>(jobs_cancelled_),
        static_cast<unsigned long long>(slots_reordered_),
        static_cast<unsigned long long>(k_rebuilds_));
    note(buf);
}

void ViewerWindow::note_profile(const char* when) {
    if (!options_.profile || !model_ || !lod_) return;
    auto timing = [](const char* name, const TimeSamples& t) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "%s %.2f s (p95 %.1f ms, %zu prov)", name, t.sum,
                      t.p95() * 1000, t.samples.size());
        return std::string(buf);
    };
    auto build = [&](const char* name, const BuildStats& st) {
        if (st.jobs == 0) {
            note(std::string("byggtider, ") + name + " (" + when + "): inga jobb");
            return;
        }
        note(std::string("byggtider, ") + name + " (" + when + "): " + std::to_string(st.jobs) +
             " jobb; " + timing("läsning", st.read) + ", " + timing("omräkning", st.convert) +
             ", " + timing("meshbygge", st.mesh));
        char buf[256];
        std::snprintf(
            buf, sizeof buf,
            "hörn per triangel, %s: r = V/T = %.3f (%llu hörn, %llu trianglar); platta fragment "
            "%.1f %% (%llu av %llu)",
            name,
            st.triangles ? static_cast<double>(st.vertices) / static_cast<double>(st.triangles)
                         : 0.0,
            static_cast<unsigned long long>(st.vertices),
            static_cast<unsigned long long>(st.triangles),
            st.fragments ? 100.0 * static_cast<double>(st.flat_fragments) /
                               static_cast<double>(st.fragments)
                         : 0.0,
            static_cast<unsigned long long>(st.flat_fragments),
            static_cast<unsigned long long>(st.fragments));
        note(buf);
    };
    build("bitar", tile_stats_);
    note_governor(when);
    {
        // Plockningen går i arbetstrådar; UI-tråden tar bara emot svaren.
        char pbuf[320];
        std::snprintf(pbuf, sizeof pbuf,
                      "plockning (%s): %llu jobb (klick, koordinat, riktning), i snitt %.1f ms, "
                      "högst %.1f ms i arbetstråden; marksökning %llu jobb, i snitt %.3f ms, högst "
                      "%.3f ms; geometricache %s MB",
                      when, static_cast<unsigned long long>(pick_jobs_),
                      pick_jobs_ ? pick_ms_sum_ / static_cast<double>(pick_jobs_) : 0.0,
                      pick_ms_max_, static_cast<unsigned long long>(walk_jobs_),
                      walk_jobs_ ? walk_job_ms_ / static_cast<double>(walk_jobs_) : 0.0,
                      walk_job_max_ms_, megabytes(pick_cache_bytes_).c_str());
        note(pbuf);
    }

    {
        // Detaljträdet, minnets tröskel och vad som är byggt, visat och parkerat.
        const size_t built = stats_.built;
        const TileScheduler::Latency latency = stats_.latency;
        char buf[768];
        std::snprintf(
            buf, sizeof buf,
            "detaljträdet (%s): %zu noder, %zu bitar, %u fasta bitar (%s trianglar), %u delade "
            "löv, %s fragment uppflyttade; minnets tröskel %s px (garanti %s px); %zu bitar "
            "byggda, %zu visade, %zu parkerade, %llu släppta, %zu byggs; från behövs till byggd "
            "p50 %.0f ms, p95 %.0f ms, högst %.0f ms (%zu bitar)",
            when, lod_->nodes.size(), lod_->tiles.size(), lod_stats_.pinned_tiles,
            millions(lod_stats_.pinned_primitives).c_str(), lod_stats_.split_nodes,
            millions(lod_stats_.moved_up).c_str(), pixels_text(stats_.threshold).c_str(),
            pixels_text(options_.min_pixels).c_str(), built, shown_tiles_,
            built > shown_tiles_ ? built - shown_tiles_ : size_t(0),
            static_cast<unsigned long long>(stats_.evictions), stats_.pending,
            latency.p50 * 1000, latency.p95 * 1000, latency.max * 1000, latency.count);
        note(buf);
    }

    {
        // Bitarna byggs bara till den storlek som behövs: de byggda primitiverna
        // (trianglar och linjesegment) mot hela bitarnas.
        uint64_t built_primitives = 0, whole_primitives = 0;
        size_t built_tiles = 0, partial = 0;
        for (const auto& entry : views_) {
            ++built_tiles;
            if (entry.second.min_size > 0) ++partial;
            built_primitives += entry.second.primitives;
            whole_primitives += lod_->tiles[entry.first].primitives;
        }
        char buf[512];
        std::snprintf(
            buf, sizeof buf,
            "bitar byggda till en storlek (%s): %llu bitar ombyggda till en mindre storlek; %zu av "
            "%zu byggda bitar är byggda till en storlek (resten hela); byggda primitiver %s av "
            "hela bitarnas %s (%.1f %%)",
            when, static_cast<unsigned long long>(size_rebuilds_), partial, built_tiles,
            millions(built_primitives).c_str(), millions(whole_primitives).c_str(),
            whole_primitives ? 100.0 * static_cast<double>(built_primitives) /
                                   static_cast<double>(whole_primitives)
                             : 100.0);
        note(buf);
    }

    if (use_done_ && !use_noted_) {
        use_noted_ = true;
        std::string hist;
        for (size_t k = 0; k < use_.hist.size(); ++k) {
            if (use_.hist[k] == 0) continue;
            const uint64_t lo = uint64_t(1) << k, hi = (uint64_t(1) << (k + 1)) - 1;
            if (!hist.empty()) hist += ", ";
            hist += (k == 0 ? std::to_string(lo) : std::to_string(lo) + "–" + std::to_string(hi)) +
                    ": " + std::to_string(use_.hist[k]);
        }
        char buf[512];
        std::snprintf(
            buf, sizeof buf,
            "postanvändning (räknad på %s): %llu poster; trianglar en gång per post %llu, en "
            "gång per fragment %llu (%.2f gånger); poster per antal fragment som visar dem: ",
            seconds_text(use_seconds_).c_str(), static_cast<unsigned long long>(use_.records),
            static_cast<unsigned long long>(use_.triangles_once),
            static_cast<unsigned long long>(use_.triangles_placed),
            use_.triangles_once ? static_cast<double>(use_.triangles_placed) /
                                      static_cast<double>(use_.triangles_once)
                                : 0.0);
        note(buf + hist);
        // Vad instanser sparar vid olika gränser: poster som visas av minst
        // så många fragment, och primitiverna som då inte behöver byggas.
        std::string saving = "instanser, per gräns (minst så många fragment per post): ";
        for (size_t k = 1; k < use_.hist.size(); ++k) {
            const RecordUseSaving s = record_use_saving(use_, k);
            if (s.groups == 0) break;
            std::snprintf(buf, sizeof buf, "%s%llu: %llu poster, %llu fragment, sparar %s primitiver",
                          k == 1 ? "" : "; ", static_cast<unsigned long long>(uint64_t(1) << k),
                          static_cast<unsigned long long>(s.groups),
                          static_cast<unsigned long long>(s.fragments),
                          millions(s.saved_primitives).c_str());
            saving += buf;
        }
        note(saving);
    } else if (use_busy_) {
        note("postanvändningen räknas fortfarande");
    }

    // Processens minne mot det som räknats.
    const PartBytes resident = resident_total();
    const PartBytes budget = budget_;
    const size_t meshes = stats_.resident.ram;
    const size_t counted = resident.ram + model_->memory_bytes + tree_bytes_;
    size_t readers = 0;
    if (session_) {
        std::lock_guard<std::mutex> lock(session_->mutex);
        readers = session_->readers_created;
    }
    SystemMemory sys;
    std::string error;
    char buf[768];
    if (system_memory(sys, error)) {
        const double diff = (static_cast<double>(sys.process) - static_cast<double>(counted)) /
                            static_cast<double>(1 << 20);
        std::snprintf(
            buf, sizeof buf,
            "minnet (%s): processen %s MB; räknat %s MB = bitarnas meshar %s MB + "
            "detaljträdet %s MB + plockningens geometricache (taket) %s MB + läsarnas cache %s MB "
            "(%zu läsare skapade, plats för %u) + "
            "modellen %s MB + trädet %s MB; processen minus räknat %.0f MB; maskinen %s MB "
            "fysiskt, %s MB tillgängligt",
            when, megabytes(sys.process).c_str(), megabytes(counted).c_str(),
            megabytes(meshes).c_str(), megabytes(lod_bytes_).c_str(),
            megabytes(pick_cache_bytes_).c_str(), megabytes(reader_bytes_).c_str(), readers,
            static_cast<unsigned>(build_pool_.threadCount() + kPickReaders),
            megabytes(model_->memory_bytes).c_str(), megabytes(tree_bytes_).c_str(), diff,
            megabytes(sys.physical).c_str(), megabytes(sys.available).c_str());
    } else {
        std::fprintf(stderr, "nwdview: processens minne gick inte att läsa: %s\n", error.c_str());
        std::snprintf(buf, sizeof buf, "minnet (%s): kunde inte läsas (%s); räknat %s MB", when,
                      error.c_str(), megabytes(counted).c_str());
    }
    note(buf);
    note(std::string("meshminne (") + when + "): " + mesh_memory_text() +
         (gpu_name_.empty() ? std::string() : ", kort: " + gpu_name_));
    const bool over = budget_shared_ ? resident.ram + resident.gpu > budget.ram
                                     : resident.ram > budget.ram || resident.gpu > budget.gpu;
    if (over)
        std::fprintf(stderr, "nwdview: meshminnet över budgeten (%s): %s\n", when,
                     mesh_memory_text().c_str());
}

} // namespace nwdview
