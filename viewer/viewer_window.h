#pragma once

// Visarens fönster: ritytan med modellen, modellistan, egenskapslistan och
// statusraden.
//
// UI-tråden gör bara det som måste ske där: ta emot färdiga svar, sätta former
// i lagren, läsa kameran och uppdatera texter. Allt som läser filen, räknar över
// bitar, bygger geometri eller träffprovar går i arbetstrådar. Inläsningen
// (fragment och rumsligt index), planeringen av detaljträdet och
// schemaläggarens grundtillstånd görs i `loader_`, bitarna byggs i `builder_`
// med en GeometryReader per tråd, lådorna i början och modellernas påverkade
// bitar räknas i `boxer_`, trädet läses i `tree_loader_` och det klickade
// objektets egenskaper i `prop_loader_`. Postanvändningen för --profile räknas
// i `use_loader_`.
//
// Urvalet har en egen tråd (`selector_worker_`, tillståndet i `SelectorCore`):
// schemaläggaren (TileScheduler) och regulatorn (DrawGovernor) körs bara där.
// UI-tråden lämnar kamerans ögonblicksbild (bara den senaste räknas när
// mätningarna kommer tätare än tråden hinner), svaren från byggjobben och
// ogiltigförklaringar, alla med ett löpnummer, och tar emot färdiga svar
// (`SelAnswer`): jobben att starta, jobb att avbryta, en lista med operationer
// i ordning (släpp, ta emot en byggd version, parkera, visa, sätt ritad andel),
// ordningen närmast först, ritningens tröskel och talen som statusraden och
// rapporterna visar. Operationerna läggs i `ops_` och görs i `run_queues` inom
// tidsbudgeten; fönstret läser aldrig schemaläggarens tillstånd. En byggd
// version väntar i `pending_` tills urvalet sagt om den ryms.
//
// Plockningen (ett klick, koordinaten under pekaren, riktningen från gångläget)
// och marksökningen i gångläget går också i arbetstrådar och rör aldrig
// ramverkets plockning: modellens meshar skapas utan CPU-kopia för plockning.
// Strålen genom pekaren tas i UI-tråden; ett jobb tar bitarna längs strålen
// (närmast först), bygger deras geometri ur filen (till samma fragmentstorlek
// som den visade biten är byggd till, så att det som inte ritas inte träffas)
// eller hämtar den ur
// `pick_cache_` (en cache med tak i byte, senast använd släpps först) och
// prövar den; svaret är ett färdigt träffresultat. Marksökningen läser bara ur
// cachen och lämnar ytans höjd under ögat; ett eget jobb håller bitarna under
// ögat i cachen.
//
// Vilka bitar som byggs, släpps, visas och parkeras avgör TileScheduler.
// Fönstret håller bara de byggda bitarna (`views_`) och de platser i lagren
// som bitarna har. En bit får sina platser första gången den visas och behåller
// dem tills den släpps. En parkerad bit står kvar i sina platser med ritad
// andel noll (stilens `range` med noll element, som ger inget ritanrop och
// ingen uppladdning), så att den visas igen utan ny uppladdning och utan att
// lagret byggs om. En plats som blir ledig får en gemensam, mycket liten
// platshållarmesh med ritad andel noll (`placeholder_`) eller genast en annan
// bits del; en plats töms aldrig, för ramverket räknar en tom form som död och
// bygger om hela lagret när en fjärdedel är död.
//
// En bit byggs bara till den fragmentstorlek som behövs (`min_size`): de
// fragment som är minst så stora, ett prefix av bitens fragment. När ögat
// kommer så nära att mindre fragment behövs byggs biten om med en mindre
// storlek; den gamla versionen visas tills den nya är mottagen, och den nya tar
// då dess platser i lagren (`replace_view`). Delarna är [0] ogenomskinligt,
// [1] genomskinligt, [2] linjer och sedan en per material med bild. Linjerna är
// meshar som ritas av ett eget program (textures.h) i lagret `line_layer_`, med
// samma detaljnivåer, parkering, ordning och ritad andel som ytorna.
//
// Ritningens tröskel (hur små fragment som ritas) räknas i urvalets tråd ur de
// byggda bitarna i bild och ritningens egen kostnad (grafikkortets tid och
// ritfasens tid ur ramverkets profilerare, `draw_cost_ms`); UI-tråden tar emot
// tröskeln. Tröskeln styr vilka bitar som visas och hur många trianglar av
// varje del som ritas (`PartShape::drawn_triangles`, som stilens `range`);
// urvalet säger per visad bit hur många trianglar av varje del och bara när det
// ändrats märkbart. Medan kameran rör sig sänks kantutjämningen när bildtiden
// är över målet, och uppladdningsbudgeten är mindre. De visade bitarnas former
// ligger i lagrens platser med den närmaste först, så att grafikkortets
// djuptest hoppar över täckta pixlar; urvalet räknar ordningen och fönstret
// byter platser i omgångar.
//
// Det UI-tråden gör i flera steg (ta emot det urvalet sagt, byta platser,
// släppa bitar, lägga om snittet, lämna bort ersatta versioner) delar en
// tidsbudget per pumpvarv (`run_queues`, ett par ms); det som inte hinns tas
// av `slice_timer_`. Meshar lämnas till `trash_worker_` så att de dör i en
// arbetstråd. Texterna ändras i klump, högst två gånger per sekund.

#include "cells.h"
#include "governor.h"
#include "lod_tree.h"
#include "model.h"
#include "scheduler.h"
#include "sysmem.h"
#include "walk.h"

#include <glaze/glaze.h>
#include <glaze_threads/threads.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nwdview {

struct Session;
struct Loaded;
struct Planned;
struct PropState;
struct PickCache;
struct PickShared;
struct PickAnswer;
struct GroundAnswer;
struct PrimeAnswer;
struct SelectorCore;
struct SelAnswer;

struct ViewerOptions {
    std::string path;
    std::string png;                     // tom: fönstret visas tills det stängs
    // Minnesbudgeten för meshar (bitarna, läsarnas cache och plockningens
    // geometri): RAM och grafikkort var för sig. 0 = förvalet ur maskinens minne.
    size_t budget_mb = 0;
    size_t gpu_budget_mb = 0;
    // Plockningens geometricache, MB; fast RAM i budgeten. Högst en fjärdedel av
    // RAM-budgeten.
    size_t pick_cache_mb = 256;
    uint64_t tile_triangles = 131'072;   // största bit i trianglar
    double min_pixels = 3;               // garantin: allt som är minst så här stort i bild syns
    bool profile = false;
    // --pacing: bildtakten. < 0 = varje bild ritas hel (förvalet), 0 =
    // skärmens, > 0 = så många bilder per sekund.
    double pacing_fps = -1;
    bool tour = false;                  // --png: zooma in och ut före bilden
    double spin = 0;                     // --png: så många orbitvarv före bilden, 10 s per varv
    UvMode uv = UvMode::RealWorld;
    std::string view;                    // en sparad vy att starta i; tom: filens aktuella vy
    // Startkameran ur ögat och en punkt att titta mot, meter i modellens
    // koordinater; gäller före `view`.
    bool has_eye = false;
    double eye[3] = {0, 0, 0};
    double look[3] = {0, 0, 0};
    bool walk = false;                   // starta i gångläget
    bool follow_ground = false;          // gångläget håller höjden över ytan under
    double walk_distance = 0;            // --png: gå så många meter rakt fram före bilden
    double walk_speed = 1.5;            // gångfarten, m/s
    // Taken för ritningens tröskel (--max-draws, --max-triangles); förvalen är
    // GovernorSettings egna. min_pixels sätts ur `min_pixels` ovan.
    GovernorSettings governor = [] {
        GovernorSettings g;
        g.max_draws = 8'000;
        return g;
    }();
    bool lines = true;                  // modellens linjer ritas (--no-lines stänger av dem)
    // En bit byggs ned till den fragmentstorlek som behövs delad med detta
    // (--size-margin): mindre tar mindre minne och bygger om oftare.
    double size_margin = 1.3;
    bool clip = true;                   // vyns snittplan klipper modellen (Visa → Snitt)
    // Ett snitt som ersätter startvyns: plan i meter i modellens koordinater,
    // som behåller sidan normalen pekar mot. Ett hjälpmedel för att pröva
    // snittet i en fil utan vyer med snitt.
    ClipState clip_test;
};

// Fragmentens intervall i en del, delade mellan fönstret och arbetstrådarna.
using RangeList = std::shared_ptr<const std::vector<FragmentRange>>;

// Delarnas platser i en bit: [0] ogenomskinligt, [1] genomskinligt, [2] linjer,
// sedan en del per material med bild.
inline constexpr size_t kPartOpaque = 0;
inline constexpr size_t kPartTransparent = 1;
inline constexpr size_t kPartLines = 2;
inline constexpr size_t kFirstTexturedPart = 3;

// En del som scenen visar: formen, meshens origo i meter (snittets plan
// räknas relativt det) och var trianglarna kommer ifrån.
struct PartShape {
    glaze::Shape shape;
    double origin[3] = {0, 0, 0};
    bool lines = false;   // linjedelen: kvadar som linjeprogrammet ritar

    RangeList ranges;
    uint64_t triangles = 0;
    // De första så här många trianglarna ritas (de största fragmenten står
    // först); `triangles` är hela delen.
    uint64_t drawn_triangles = 0;
    // De första så här många trianglarna ritas alltid: en fast bits sista
    // detaljnivå, som ritas på alla avstånd. 0 för andra bitar.
    uint64_t floor_triangles = 0;
};

// Det i en byggd bit som aldrig ändras och som urvalets tråd räknar ritad andel
// och ritningens tröskel ur; delat med den tråden.
struct TileStatic {
    struct Part {
        RangeList ranges;
        uint32_t triangles = 0;
        uint32_t floor_triangles = 0;
        bool present = false;   // delen har en form
    };
    std::vector<Part> parts;
    bool pinned = false;
    double max_size = 0;
};

// En operation som urvalet ger fönstret. De görs i den ordning de kommer, så
// att en bit som släpps och byggs igen, eller visas och sedan parkeras, hamnar
// rätt.
struct SelOp {
    enum Kind : uint8_t {
        Drop,      // biten släpps: platserna och mesharna
        Install,   // den byggda versionen `install` är kvar och ersätter en eventuell gammal
        Discard,   // den byggda versionen `install` ryms inte och kastas
        Park,      // biten blir ritad med noll element men står kvar i sina platser
        Show,      // biten visas med `drawn` trianglar av varje del
        Fit,       // en visad bit ritar `drawn` trianglar av varje del
    };
    Kind kind = Drop;
    uint32_t tile = 0;
    uint64_t install = 0;
    bool shown = false;               // Install: biten är visad
    std::vector<uint32_t> drawn;      // Install (visad), Show och Fit: trianglar per del
};

// En visad bit och dess avståndsband, närmast ögat först.
struct OrderEntry {
    uint32_t tile = 0;
    int band = 0;
};

// Talen ur schemaläggaren som statusraden och rapporterna visar.
struct SelStats {
    double threshold = 3;            // minnets tröskel
    PartBytes resident;
    size_t built = 0, pending = 0;
    size_t shown = 0;
    uint64_t evictions = 0;
    bool idle = false;
    TileScheduler::Latency latency;  // uppdateras högst två gånger per sekund
};

// Tiderna i ett byggjobb och vad det byggde, för --profile.
struct BuildTimes {
    double read_seconds = 0;      // geometrin ur filen
    double convert_seconds = 0;   // världen, normalerna och delarna
    double mesh_seconds = 0;      // meshen ur delen
    uint64_t fragments = 0, flat_fragments = 0, vertices = 0, triangles = 0;
};

// Det som en byggd bit lämnar till UI-tråden.
struct BuiltTile {
    uint32_t tile = 0;
    uint64_t generation = 0;
    HNwd::Status status = HNwd::Status::Ok;
    std::string error;
    std::vector<PartShape> parts;   // [0] ogenomskinligt, [1] genomskinligt, [2] linjer, sedan texturerade
    std::vector<TextLabel> labels;
    std::shared_ptr<const TileStatic> stat;
    PartBytes bytes;
    BuildTimes times;
    // Fragment med mindre diagonal än så här byggdes inte; 0 = hela biten.
    float min_size = 0;
    // Px per radian som detaljnivåerna bakades för (med marginalen): växer
    // kamerans värde förbi det gäller inte garantin längre.
    double baked_ppr = 0;
    uint64_t triangles = 0, segments = 0, segments_skipped = 0, points = 0, texts = 0,
             undecoded = 0, missing_textures = 0, rotated_textures = 0;
};

// Summorna och tiderna per jobb för ett slags byggjobb.
struct TimeSamples {
    double sum = 0;
    std::vector<float> samples;

    void add(double seconds) {
        sum += seconds;
        samples.push_back(static_cast<float>(seconds));
    }
    // Det värde 95 % av proven ligger under; 0 utan prov.
    double p95() const;
};

struct BuildStats {
    uint64_t jobs = 0, fragments = 0, flat_fragments = 0, vertices = 0, triangles = 0;
    TimeSamples read, convert, mesh;

    void add(const BuildTimes& t);
};

// Vad ett plockningsjobb ska svara på.
enum class PickPurpose : uint8_t {
    Click,        // välj objektet under pekaren
    Coordinate,   // koordinaten under pekaren
    Target,       // ytan rakt fram när gångläget lämnas
};

class ViewerWindow : public glaze::AppWindow {
public:
    explicit ViewerWindow(ViewerOptions options);
    ~ViewerWindow() override;

    // Avslutningskoden för en körning med --png: 0 när bilden skrevs.
    int result() const { return result_; }
    // Mätningarna för --profile: tiderna till varje steg.
    std::string timings() const { return timings_; }

private:
    using Clock = std::chrono::steady_clock;

    // Var en form i ett lager hör hemma: biten och delen.
    struct SlotOwner {
        uint32_t owner = UINT32_MAX;
        uint32_t part = 0;
    };

    // En byggd bit: det den visar, och platserna i lagren från första gången
    // den visas. Bitar som inte är byggda har ingen post.
    struct TileView {
        std::vector<PartShape> parts;      // [0] ogenomskinligt, [1] genomskinligt, [2] linjer, sedan texturerade
        std::shared_ptr<const TileStatic> stat;
        int opaque_slot = -1, transparent_slot = -1, line_slot = -1;
        std::vector<int> textured_slots;   // en per icke-tom del från [3]
        std::vector<int> label_slots;
        std::vector<uint8_t> label_on;     // texten visas (inte för liten i bild, inte bortklippt)
        std::vector<TextLabel> labels;
        size_t labels_dropped = 0;         // texter utan plats under taket, medan biten visas
        bool slotted = false;              // biten har platser i lagren (också när den är parkerad)
        bool shown = false;                // biten visas; annars parkerad eller aldrig visad (ritad andel noll)
        PartBytes bytes;                   // meshar
        uint64_t install = 0;              // vilken byggd version det är
        double baked_ppr = 0;              // px per radian som nivåerna bakades för
        float min_size = 0;                // fragment under den här diagonalen är inte byggda; 0 = hela
        uint64_t primitives = 0;           // byggda primitiver: trianglar och linjesegment
    };

    // Vilket lager en delmängd av platserna ligger i.
    enum class SlotKind : uint8_t { Opaque, Textured, Lines };

    // Den tid UI-tråden får lägga på ett steg i flera delar innan resten får
    // vänta.
    class Slice {
    public:
        explicit Slice(double ms)
            : end_(Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                      std::chrono::duration<double, std::milli>(ms))) {}
        bool spent() const { return Clock::now() >= end_; }

    private:
        Clock::time_point end_;
    };

    void open_file(const std::string& path);
    void choose_file();
    void reset_scene();
    void show_all();
    void fail_load(const std::string& text);

    void on_loaded(Loaded& loaded);
    void on_planned(Planned& planned);
    // Mäter kameran och lämnar ögonblicksbilden till urvalets tråd.
    void remeasure();
    // Orbitens närplan efter avståndet till målet och modellens räckvidd.
    void fit_orbit_near();
    // Avståndet från ögat till det närmaste i bilden som kan ha geometri: en
    // undre gräns, ur filens rumsliga träd. Oändligt när inget når in i bilden.
    double nearest_in_view(const ViewFrustum& view) const;
    void request_update();
    // Kameran rörde sig (eller ritytan ändrades).
    void note_motion(Clock::time_point now);
    // Kameran rör sig eller ritytan tickar just nu.
    bool moving(Clock::time_point now) const;
    // Ritningens egen kostnad i ms ur profilerarens senaste bilder: grafikkortets
    // tid och ritfasens tid (den större; saknas grafikkortets svar ritfasens
    // ensam); 0 när ingen ny bild ritats sedan förra frågan eller ingen tid är
    // känd.
    double draw_cost_ms();
    // Kameran har stått still en stund: kantutjämningen, uppladdningsbudgeten
    // och mätningen återgår.
    void settle();

    // Urvalets tråd: startas när detaljträdet är planerat, stoppas när filen
    // stängs eller byts.
    void start_selector(std::unique_ptr<TileScheduler> sched);
    void stop_selector();
    // Svaren från urvalet.
    void on_answer(SelAnswer& a);
    // Svaren från bitar som byggts: ett byggt jobb lämnas till urvalet, och den
    // byggda versionen väntar i `pending_` tills urvalet sagt om den ryms.
    void on_built(BuiltTile& b);
    // Bygger biten till `min_size`: bara fragment med minst så stor diagonal
    // (0 = hela biten).
    void start_tile_job(uint32_t tile, float min_size);
    // Primitiverna (trianglar och linjesegment) i delarna.
    static uint64_t primitives_of(const std::vector<PartShape>& parts);

    // Operationerna ur urvalet, så länge `slice` räcker.
    void apply_ops(const Slice& slice);
    void apply_op(SelOp& op);
    // En byggd version av biten ersätter den som finns. Är den gamla visad
    // visas den nya i samma platser (ordningen står kvar); den gamlas meshar
    // lämnas bort genom `retire`.
    void replace_view(uint32_t tile, TileView fresh, bool shown, const std::vector<uint32_t>& drawn);
    // Delens form i sin plats i lagret, med snittet och `range`.
    void put_part(TileView& v, size_t part);
    // Ritad andel per del, och dess summa i `shown_triangles_` när biten visas.
    void set_drawn(TileView& v, const std::vector<uint32_t>& drawn);
    // Lämnar delarna till en arbetstråd som låter meshhandtagen dö där, efter
    // en kort stund så att ramverkets egna referenser hunnit släppas.
    void retire(std::vector<PartShape>&& parts);
    void flush_retired(bool all);

    // Ordningen i lagren: urvalet räknar bitarnas avståndsband, fönstret byter
    // platser i omgångar.
    struct OrderItem {
        uint32_t tile = 0;
        uint32_t part = 0;
        int slot = -1;
        int band = 0;
    };
    void run_order(const Slice& slice);
    // Byter former mellan platserna i ett lager; svarar antalet byten. `done`
    // blir falskt när tiden eller antalet byten tog slut före alla.
    size_t reorder_layer(int layer, std::vector<SlotOwner>& owners, SlotKind kind,
                         std::vector<OrderItem>& items, size_t max_swaps, const Slice& slice,
                         bool& done);

    // Kantutjämningen och uppladdningsbudgeten följer om kameran rör sig.
    void update_quality(bool moving);
    void restore_quality();
    void update_upload_budget(bool moving);
    // "ritar ≥ 6 px för bildtakten" när ritningens tröskel är över minnets.
    std::string draw_text() const;
    // Ritningens tröskel, belastningen och det som reglerats, för --profile och
    // gång- och snurrrapporterna.
    void note_governor(const char* when);
    // Efter ett svar: grundbilden, startvyn och statusraden.
    void after_answer();
    // Är grundbilden klar? Då tas lådorna bort och trädet läses.
    void note_base();
    // Urvalet och fönstret har inget kvar att göra: inget byggs, inget återstår
    // att verkställa och det senaste svaret ändrade ingenting.
    bool selector_calm() const;
    // Biten visas med `drawn` trianglar av varje del.
    void show_tile(uint32_t tile, const std::vector<uint32_t>& drawn);
    // Biten ritas med noll element men står kvar i sina platser.
    void park_tile(uint32_t tile);
    // Biten släpps: platserna (som får platshållaren) och mesharna.
    void drop_tile(uint32_t tile);
    // En visad bit ritar andra mängder av sina delar.
    void fit_tile(uint32_t tile, const std::vector<uint32_t>& drawn);
    // Delarnas former i platser i lagren, med det gällande snittet. Platserna i
    // `old` (en tidigare version av biten, som förlorar dem) används först, så
    // att ordningen står kvar; de som inte behövs blir lediga.
    void place_parts(TileView& v, uint32_t tile, TileView* old);
    // Platserna blir lediga (platshållaren), delarna finns kvar.
    void release_slots(TileView& v);
    // Bitens texter i platser i etikettlagret, och tillbaka.
    void install_labels(TileView& v, uint32_t tile);
    void release_labels(TileView& v, uint32_t tile);
    // Skriver ramverkets fel för visarens program på stderr, en gång per program.
    void check_programs();
    // Schemaläggaren och fönstret är inte överens: står på stderr.
    void inconsistent(const char* what, uint32_t tile);
    void start_tree();
    void fill_models();
    void toggle_model(int row);
    void fill_views();
    void show_grids(std::vector<glaze::Shape> shapes);
    void choose_view();
    // Sätter kameran efter en kamera ur filen; false när den inte ser på modellen.
    bool apply_file_camera(const HNwd::Camera& camera, bool glide);

    // Snittet: vyns plan (clip_from_camera) och om de klipper.
    void set_view_clip(const HNwd::Camera& camera);
    void set_clip(const ClipState& clip, const std::string& error);
    // Lägger det gällande snittet i varje visad del, bit för bit inom
    // tidsbudgeten.
    void apply_clip();
    void apply_clip_tile(uint32_t tile);
    // Snittet som gäller nu: vyns när det är påslaget, annars inget.
    const ClipState& active_clip() const;
    // Delens form med det gällande snittet i sitt GlazeUser-block och med de
    // trianglar som ritas.
    glaze::Shape clipped_shape(const PartShape& part) const;

    // Köerna som tas inom tidsbudgeten: operationerna ur urvalet, bitar som ska
    // få nytt snitt, bitar som ska ogiltigförklaras efter ett modellval, ordningen
    // i lagren, versioner som ska lämnas bort och texterna.
    void run_queues();
    // Inget återstår att verkställa (det som lämnas bort räknas inte).
    bool queues_empty() const;
    void start_slices();
    // Texterna: formerna i etikettlagret ändras i klump, högst två gånger per
    // sekund, och bara det som ändrats.
    void set_label(int slot, glaze::Shape shape);
    void flush_labels(bool force);

    void update_status();
    // Det statusraden inte visar: räknarna för modellen, bitarna och minnet.
    std::string status_details() const;
    std::string mesh_memory_text() const;
    // Vad garantin lovar just nu: "allt ≥ 3 px syns", eller det höjda talet.
    std::string threshold_text() const;
    // Byggda bitars meshar och det fasta minnet (läsarnas cache, plockningens
    // cache, detaljträdet).
    PartBytes resident_total() const;

    // Plockningen: koordinaten under pekaren, ett klick och riktningen från
    // gångläget. Strålen tas här; jobbet går i en arbetstråd.
    void update_coordinate(glaze::Point local);
    void pick(glaze::Point local);
    void run_pick(PickPurpose purpose, glaze::Point local);
    void run_target_pick(const double eye[3], const double forward[3]);
    void on_pick_answer(PickAnswer& a);
    void select(size_t fragment);
    // Det som en plockning behöver ur fönstret.
    double ray_reach(const double origin[3]) const;

    // Gångläget.
    void set_walking(bool on);
    void walk_tick(double seconds);
    void apply_walk_camera();
    bool walk_key(gfx::Key k) const;
    // Avståndet rakt ned till ytan ur den senaste marksökningen (ett jobb i en
    // arbetstråd som läser ur plockningens cache); tomt när ingen yta är känd.
    std::optional<double> ground_below(const double from[3], double reach);
    void request_ground(const double from[3], double reach);
    void start_ground_probe(const double from[3], double reach);
    void start_ground_prime(const double eye[3], double reach);
    void on_ground_answer(GroundAnswer& a);
    void on_prime_answer(PrimeAnswer& a);
    void note_walk_sample(size_t holes);
    void finish_walk();

    // --spin: ett orbitvarv i taget runt målet.
    void start_spin();
    void spin_tick(double seconds);
    void finish_spin();

    // --profile: byggtiderna, hörnen, postanvändningen och minnet.
    void note_profile(const char* when);
    // Bildtakten och ritlasten de senaste `seconds` sekunderna; `vertices` är
    // renderarens ritade hörn när mätningen började.
    void note_frames(const char* what, double seconds, uint64_t vertices);
    uint64_t rendered_vertices();
    // Grafikkortet som fönstret ritar med; minnet 0 när det inte går att läsa.
    struct GpuInfo {
        std::string name;
        uint64_t memory = 0;
        bool shared = true;   // delar minne med processorn, eller okänt
    };
    // Frågar systemet (tiotals ms): görs en gång innan första bilden.
    GpuInfo window_gpu();

    // Texterna: dolda när de blir för små i bild.
    void update_labels(const ViewFrustum& view);

    void png_tick();
    void profile_tick();
    // --png: bilden till `path` när två bilder i följd är lika (bilderna ur
    // materialen uppladdade), sedan `then`. Tidsgränsen ger ett fel på stderr.
    void begin_capture(const std::string& path, std::function<void()> then);
    void capture_tick();
    void note(const std::string& what);
    void note_stalls(const char* when);
    // Hur länge en fortsättning i UI-tråden tog; stopp som är för långa efter
    // startvyn räknas och det första över `kStallErrorMs` står på stderr.
    void stall(const char* what, Clock::time_point since);

    // En plats i ett lager; en ny plats får platshållaren, aldrig en tom form.
    int take_slot(int layer, std::vector<int>& free_slots, std::vector<SlotOwner>& owners,
                  SlotOwner owner);
    // Platsen blir ledig: platshållaren i meshlagren, en tom text i etikettlagret.
    void free_slot(int layer, std::vector<int>& free_slots, std::vector<SlotOwner>& owners,
                   int& slot);

    ViewerOptions options_;
    int result_ = 0;
    std::string timings_;
    Clock::time_point opened_at_{};
    std::map<std::string, double> stalls_;   // längsta tid per slag av fortsättning, ms
    // Stopp i UI-tråden efter startvyn: hur många som var över `kStallCountMs`,
    // det längsta och vad det var.
    uint64_t stalls_over_ = 0;
    double stall_worst_ms_ = 0;
    std::string stall_worst_name_;
    bool stall_error_written_ = false;

    uint64_t generation_ = 0;
    std::shared_ptr<const Model> model_;
    std::shared_ptr<Session> session_;
    // Detaljträdet; null tills planeringen är klar. Schemaläggaren och
    // regulatorn bor i `selector_` och rörs bara av urvalets tråd.
    std::shared_ptr<const LodTree> lod_;
    LodTreeStats lod_stats_;
    size_t lod_bytes_ = 0;                   // detaljträdets minne, fast RAM
    std::shared_ptr<SelectorCore> selector_;
    SelStats stats_;                         // talen ur senaste svaret
    uint64_t sent_seq_ = 0;                  // löpnumret på det senaste meddelandet till urvalet
    uint64_t acked_seq_ = 0;                 // urvalet har hanterat alla t.o.m. det här
    bool answer_calm_ = false;               // senaste svaret ändrade ingenting
    bool sel_throttled_ = false;             // urvalet höll inne operationer medan fönstret hade för många
    // Operationerna som väntar på att göras, i ordning.
    std::deque<std::vector<SelOp>> ops_;
    size_t ops_next_ = 0;                    // nästa operation i den första omgången
    size_t ops_count_ = 0;                   // alla som återstår
    // Byggda versioner som väntar på urvalets besked: Install eller Discard.
    struct PendingTile {
        uint32_t tile = 0;
        TileView view;
    };
    std::unordered_map<uint64_t, PendingTile> pending_;
    // Ordningen i lagren: det senaste urvalet av närmaste bitar, och vilket lager
    // som är på tur.
    std::vector<OrderEntry> order_;
    int order_stage_ = 3;                    // 3 = inget pågår
    // De byggda bitarna, visade eller parkerade.
    std::unordered_map<uint32_t, TileView> views_;
    std::unordered_set<uint32_t> counted_;   // bitar vars tal är med i summorna
    std::unordered_set<uint32_t> label_tiles_;   // visade bitar med texter
    uint64_t next_install_ = 1;
    size_t shown_tiles_ = 0;                 // visade bitar
    // Grundbilden: de fasta bitarna.
    std::unordered_set<uint32_t> pinned_seen_;   // fasta bitar som är byggda eller felat
    size_t pinned_pending_ = 0;              // fasta bitar som återstår
    bool base_noted_ = false;
    PartBytes budget_;                       // meshbudgeten för RAM och grafikkort
    bool budget_shared_ = false;             // grafikkortet delar minne med processorn
    std::string gpu_name_;
    bool have_gpu_info_ = false;             // `gpu_info_` är läst
    GpuInfo gpu_info_;
    size_t reader_bytes_ = 0;                // läsarnas cache, räknad som fast RAM
    size_t pick_cache_bytes_ = 0;            // plockningens geometricache, räknad som fast RAM
    size_t tree_bytes_ = 0;                  // det trädet lägger till i dokumentet
    bool load_failed_ = false;
    bool measured_ = false;                  // ritytan hade en yta vid senaste mätningen
    std::string build_error_;                // det första felet när en bit byggdes
    uint64_t failed_tiles_ = 0;
    uint64_t shown_triangles_ = 0;
    uint64_t points_ = 0, texts_ = 0, undecoded_ = 0, segments_skipped_ = 0,
             missing_textures_ = 0, rotated_textures_ = 0;
    size_t labels_shown_ = 0;
    uint64_t labels_dropped_ = 0;
    bool boxes_cleared_ = false;
    std::shared_ptr<const std::vector<NodeBox>> box_list_;   // lådorna som visas före grundbilden
    bool idle_noted_ = false;
    size_t selected_ = SIZE_MAX;
    uint32_t inconsistencies_ = 0;

    // Ritningens tröskel och det som följer av den.
    double draw_pixels_ = 3;                 // gällande tröskel, minst minnets
    double draw_pixels_max_ = 0;             // högsta sedan öppningen, gången eller snurran
    DrawLoad last_load_;                     // uppskattad belastning vid den senaste tröskeln
    bool draw_reduce_ = false;               // regulatorn räcker inte: sänk kantutjämningen
    uint64_t cost_frame_ = 0;                // senaste bildens löpnummer som `draw_cost_ms` läst
    uint64_t aa_reductions_ = 0, jobs_cancelled_ = 0, slots_reordered_ = 0, k_rebuilds_ = 0;
    // Bitar som byggts om till en mindre storlek (fler fragment byggda).
    uint64_t size_rebuilds_ = 0;
    // Programmens fel: vilka som redan står på stderr, och när de prövades senast.
    std::unordered_set<std::string> programs_reported_;
    Clock::time_point programs_checked_{};
    // Varje byggjobbs eget stopp, kopplat till arbetarens token.
    std::unordered_map<uint32_t, std::stop_source> job_stops_;
    // Kamerans rörelse: tiden för senaste händelsen, tiden mellan de två
    // senaste och tiden mellan tickarna.
    Clock::time_point last_motion_{};
    Clock::time_point tick_at_{};
    bool motion_run_ = false;                // händelserna kommer i följd
    // Kameran sattes om (en vy, inpassning): förhandsbygget börjar om.
    bool skip_ahead_ = false;
    // Kantutjämningen: värdet ritytan hade, och om den är sänkt.
    bool aa_low_ = false;
    int aa_full_ = 0;
    // Uppladdningsbudgeten: 0 orörd, 1 medan kameran rör sig, 2 när den står still.
    int upload_state_ = 0;
    bool capturing_ = false;                 // --png-fångsten har sin egen budget

    // Köerna som körs inom tidsbudgeten.
    std::vector<uint32_t> clip_queue_;       // bitar som ska få nytt snitt
    size_t clip_next_ = 0;
    // Bitar vars modell dolts eller visats: deras tal räknas om, och de ogiltigförklaras
    // hos urvalet i omgångar.
    std::deque<uint32_t> rebuild_queue_;
    // Delar som ska lämnas till en arbetstråd, med tiden de lades hit.
    struct Retired {
        Clock::time_point at;
        std::vector<PartShape> parts;
    };
    std::deque<Retired> retired_;
    // Texterna: formerna som ska sättas i etikettlagret, och vilka platser som
    // har en text i scenen.
    std::unordered_map<int, glaze::Shape> label_pending_;
    std::vector<uint8_t> label_on_scene_;
    Clock::time_point label_flushed_{};
    // En gemensam, mycket liten mesh med ritad andel noll för platser utan del.
    glaze::Shape placeholder_;

    // Plockningen: geometrin för bitar och vilka bitar som visas.
    std::shared_ptr<PickCache> pick_cache_;
    std::shared_ptr<PickShared> pick_shared_;
    bool click_busy_ = false;
    bool coord_busy_ = false;
    std::optional<glaze::Point> coord_pending_;   // den senaste punkten som väntar på sitt jobb
    std::string pick_error_;                 // det första felet i ett plockningsjobb
    uint64_t pick_jobs_ = 0;
    double pick_ms_sum_ = 0, pick_ms_max_ = 0;
    // Riktningen från gångläget: ögat och blicken som kameran sattes för.
    double target_eye_[3] = {0, 0, 0};
    double target_forward_[3] = {0, 1, 0};
    uint64_t target_epoch_ = 0;

    // Marksökningen i gångläget.
    struct Ground {
        bool known = false;     // en sökning har svarat
        bool surface = false;   // den fann en yta
        double x = 0, y = 0, oz = 0;   // ögats läge vid sökningen
        double z = 0;                  // ytans höjd
    };
    Ground ground_;
    bool ground_busy_ = false, ground_want_ = false;
    bool ground_force_ = false;              // cachen har ändrats: sök igen
    double ground_next_[3] = {0, 0, 0};
    double ground_next_reach_ = 0;
    bool prime_busy_ = false, prime_dirty_ = true;
    bool prime_have_ = false;
    double prime_origin_[3] = {0, 0, 0};
    uint64_t ground_cache_gen_ = 0;          // cachens generation vid senaste sökningen

    // --profile.
    BuildStats tile_stats_;
    RecordUse use_;
    double use_seconds_ = 0;
    bool use_busy_ = false, use_done_ = false, use_noted_ = false;

    // Trädet, modellerna och egenskaperna.
    std::shared_ptr<const HNwd::Document> tree_;   // modellens dokument, när trädet är läst i det
    std::vector<ModelInfo> models_;
    std::shared_ptr<PropState> props_;
    bool tree_started_ = false;
    std::string tree_error_;
    std::string tree_progress_;
    bool props_busy_ = false;
    std::vector<uint8_t> hidden_models_;
    std::shared_ptr<const ModelFilter> filter_;

    // png-läget: hur många kontroller i följd allt varit klart, och rundan.
    int png_stable_ = 0;
    int tour_phase_ = 0;
    int profile_step_ = 0;                   // --profile efter bilden: klicket, sedan en dold modell
    glaze::Camera3D start_camera_{};
    // --png: bilden tas två gånger med samma kamera tills de är lika, så att
    // ingen bild ur materialen saknas; `capture_started_` ger tidsgränsen.
    std::optional<glaze::Bitmap> capture_last_;
    int capture_tries_ = 0;
    Clock::time_point capture_started_{};
    size_t upload_budget_ = 0;               // appens uppladdningsbudget före bilden
    std::string capture_path_;
    std::function<void()> capture_then_;

    // Kameran vid senaste mätningen, för texterna som installeras efteråt och
    // för detaljnivåerna i de bitar som byggs.
    ViewFrustum last_view_{};
    bool have_view_ = false;

    // Gångläget.
    bool walking_ = false;
    WalkState walk_;
    WalkSettings walk_settings_;
    bool looking_ = false;                   // en knapp hålls: musen vrider blicken
    gfx::MouseButton look_button_ = gfx::MouseButton::Left;   // knappen som håller blicken
    glaze::Point look_last_{};
    double walk_far_ = 1000;                 // bortre klipplanet i gångläget, meter
    // --walk: sträckan kvar, och mätningarna under gången.
    double walk_left_ = 0;
    bool walk_running_ = false;
    Clock::time_point walk_began_{};
    double walk_time_ = 0, walk_probe_ms_ = 0, walk_probe_max_ms_ = 0;
    double walk_job_ms_ = 0, walk_job_max_ms_ = 0;   // marksökningarnas tid i arbetstråden
    uint64_t walk_ticks_ = 0, walk_probes_ = 0, walk_jobs_ = 0, walk_holes_max_ = 0,
             walk_holes_sum_ = 0;
    uint64_t walk_hole_ticks_ = 0, walk_samples_ = 0;
    double walk_tick_max_ = 0;
    uint64_t walk_vertices_ = 0;             // renderarens ritade hörn när gången började
    std::string walk_report_;
    // --spin: orbitvarven före bilden, och mätningarna under dem.
    bool spin_running_ = false;
    double spin_left_ = 0;                   // grader kvar
    double spin_angle_ = 0;                  // grader hittills
    double spin_start_azimuth_ = 0;
    Clock::time_point spin_began_{};
    double spin_time_ = 0, spin_tick_max_ = 0;
    uint64_t spin_ticks_ = 0, spin_vertices_ = 0;
    std::string spin_report_;

    Clock::time_point last_update_{};
    Clock::time_point last_coordinate_{};
    glaze::Point pending_coordinate_{};

    glaze::Canvas scene_;
    glaze::PropertyGrid properties_;
    glaze::ListView model_list_;
    // Vyerna och rutnätens nivåer, i en kolumn till vänster.
    glaze::Panel side_;
    glaze::TreeView views_tree_;
    glaze::ListView levels_list_;
    std::vector<std::pair<glaze::TreePath, HNwd::Camera>> view_nodes_;   // en per vy i trädet
    bool grid_visible_ = true;
    ClipState view_clip_;                    // vyns snittplan i meter
    ClipState no_clip_;                      // inget snitt
    std::string clip_error_;                 // varför vyns snitt inte visas; tomt annars
    glaze::Label status_;
    Clock::time_point status_details_at_{};   // när statusradens tooltip senast byttes
    glaze::Label unit_;
    glaze::Label coordinate_;
    glaze::Progress progress_;
    glaze::FileDialog open_dialog_{glaze::FileDialogMode::Open};
    glaze::Timer update_timer_;
    glaze::Timer settle_timer_;
    glaze::Timer coordinate_timer_;
    glaze::Timer png_timer_;
    glaze::Timer slice_timer_;
    glaze::Timer label_timer_;
    glaze::Point press_{};
    glaze::Lighting light_{};
    int opaque_layer_ = 0, transparent_layer_ = 0, textured_layer_ = 0, line_layer_ = 0,
        boxes_layer_ = 0, labels_layer_ = 0, pick_layer_ = 0, grid_layer_ = 0;
    std::vector<int> free_opaque_, free_transparent_, free_textured_, free_lines_, free_labels_;
    std::vector<SlotOwner> opaque_owner_, transparent_owner_, textured_owner_, line_owner_;
    std::vector<SlotOwner> unused_owner_;    // lager där ingen frågar efter ägaren

    // Poolerna före de workers som använder dem: en worker dör först och
    // väntar in sitt arbete.
    glaze::threads::Pool load_pool_{1};
    glaze::threads::Pool tree_pool_{1};
    glaze::threads::Pool prop_pool_{1};
    glaze::threads::Pool pick_pool_{5};
    glaze::threads::Pool select_pool_{2};
    glaze::threads::Pool build_pool_;
    glaze::threads::Worker loader_{load_pool_};
    glaze::threads::Worker use_loader_{load_pool_};
    glaze::threads::Worker tree_loader_{tree_pool_};
    glaze::threads::Worker prop_loader_{prop_pool_};
    glaze::threads::Worker click_worker_{pick_pool_};
    glaze::threads::Worker coord_worker_{pick_pool_};
    glaze::threads::Worker ground_worker_{pick_pool_};
    glaze::threads::Worker prime_worker_{pick_pool_};
    glaze::threads::Worker trash_worker_{pick_pool_};
    glaze::threads::Worker selector_worker_{select_pool_};
    glaze::threads::Worker builder_{build_pool_};
    glaze::threads::Worker boxer_{build_pool_};
};

} // namespace nwdview
