// Urvalet (viewer/scheduler.h): kameran, mätningen av bitarna, garantin, de två
// budgetarna, minnets tröskel, de fasta bitarna, vad som släpps först, vad som
// visas och i vilken ordning, latensen, förutsägelsen och avbrotten. Ren std och HNwd — ingen fönstertyp — så det prövas utan
// visaren.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "../tools/synth_writer.h"
#include "HNwd.h"
#include "lod_tree.h"
#include "model.h"
#include "scheduler.h"
#include "test_data.h"
#include "test_support.h"

using namespace nwdview;

namespace {

// Tar bort filen när den går ur räckvidd. Modellen som öppnat den ska
// deklareras efter den, så att den stängs först.
struct Removed {
    std::string path;
    ~Removed() { testdata::remove_utf8(path); }
};

// Slumptal i [0, 1), ur ett frö.
struct Random {
    uint64_t state;
    double next() {
        state += 0x9E3779B97F4A7C15ull;
        uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        return static_cast<double>(z >> 11) / 9007199254740992.0;
    }
};

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr size_t kHuge = size_t(1) << 40;

// Uppskattningen för en bit med 1000 primitiver innan något byggts, och det
// som en sådan bit då verkligen tar.
constexpr size_t kEstimateRam = (kRamBytesPerVertex + kIndexBytesPerTriangle) * 1000 + 4096;
constexpr size_t kEstimateGpu = (kGpuBytesPerVertex + kIndexBytesPerTriangle) * 1000 + 4096;
constexpr PartBytes kBuilt{(kRamBytesPerVertex + kIndexBytesPerTriangle) * 1000,
                           (kGpuBytesPerVertex + kIndexBytesPerTriangle) * 1000};

TileScheduler::Settings settings(PartBytes budget, bool shared = false) {
    TileScheduler::Settings s;
    s.budget = budget;
    s.shared = shared;
    s.max_pixels = 1e6;
    return s;
}

// Jobbens storlek, per schemaläggare och bit, så att done() kan svara med den
// storlek som jobbet fick.
std::map<std::pair<const TileScheduler*, uint32_t>, float>& job_sizes() {
    static std::map<std::pair<const TileScheduler*, uint32_t>, float> sizes;
    return sizes;
}

// Bitarna i nästa jobb, i ordning; jobbens storlek sparas för done().
std::vector<uint32_t> next_jobs(TileScheduler& s, size_t max_jobs, std::vector<uint32_t>& evict) {
    std::vector<uint32_t> tiles;
    for (const TileScheduler::Job& job : s.next_build_jobs(max_jobs, evict)) {
        tiles.push_back(job.tile);
        job_sizes()[{&s, job.tile}] = job.min_size;
    }
    return tiles;
}

// Ett jobb är klart, med den storlek det fick (0 för en bit utan jobb).
bool done(TileScheduler& s, uint32_t tile, PartBytes bytes, bool ok, double now,
          std::vector<uint32_t>& evict) {
    const auto it = job_sizes().find({&s, tile});
    return s.tile_done(tile, it == job_sizes().end() ? 0.0f : it->second, bytes, ok, now, evict);
}

// Det ett jobb med den storlek som next_jobs() gav tar när det är byggt: prefixets
// primitiver.
PartBytes job_bytes(const TileScheduler& s, const LodTree& tree, uint32_t tile) {
    const auto it = job_sizes().find({&s, tile});
    const float size = it == job_sizes().end() ? 0.0f : it->second;
    const size_t p = static_cast<size_t>(tree.prefix_at_least(tile, size).primitives);
    return {p * (kRamBytesPerVertex + kIndexBytesPerTriangle),
            p * (kGpuBytesPerVertex + kIndexBytesPerTriangle)};
}

// Bitar med små lådor på rad framför ögat, var och en med 1000 primitiver och
// en storlek som ger just så många px: ögat står i origo och ser mot +y med
// 45 graders synfält och 1000 px bildhöjd. En bit med 0 px ligger bakom ögat.
struct Rig {
    LodTree tree;
    ViewFrustum view;

    explicit Rig(const std::vector<double>& pixels, const std::vector<size_t>& pinned = {}) {
        const double eye[3] = {0, 0, 0};
        view = ViewFrustum::orbit(eye, 0, 0, 45, 1000, 1000);
        tree.nodes.resize(pixels.size() + 1);
        tree.tiles.resize(pixels.size());
        tree.nodes[0].first_child = 0;
        tree.nodes[0].child_count = static_cast<uint32_t>(pixels.size());
        for (uint32_t i = 0; i < pixels.size(); ++i) {
            tree.children.push_back(i + 1);
            tree.nodes[i + 1].parent = 0;
            tree.nodes[i + 1].first_tile = i;
            tree.nodes[i + 1].tile_count = 1;
            tree.tiles[i].node = i + 1;
            tree.tiles[i].primitives = 1000;
            place(i, pixels[i]);
        }
        for (const size_t p : pinned) tree.tiles[p].pinned = true;
    }

    // Flyttar bit i så att den mäts `pixels` px; 0 lägger den bakom ögat.
    void place(size_t i, double pixels) {
        Tile& t = tree.tiles[i];
        const double x = 0.2 * static_cast<double>(i);
        const double y = pixels > 0 ? 100.0 : -100.1;
        t.bounds = Box{};
        t.bounds.min[0] = x;
        t.bounds.max[0] = x + 0.1;
        t.bounds.min[1] = y;
        t.bounds.max[1] = y + 0.1;
        t.bounds.min[2] = 0;
        t.bounds.max[2] = 0.1;
        t.max_size = 1.0f;
        if (pixels > 0)
            t.max_size = static_cast<float>(pixels * view.distance(t.bounds) /
                                            view.pixels_per_radian());
        t.min_size = t.max_size;
        LodNode& node = tree.nodes[i + 1];
        node.bounds = t.bounds;
        node.max_size = t.max_size;
        LodNode& root = tree.nodes[0];
        root.bounds = Box{};
        root.max_size = 0;
        for (size_t k = 1; k < tree.nodes.size(); ++k) {
            root.bounds.add(tree.nodes[k].bounds);
            root.max_size = std::max(root.max_size, tree.nodes[k].max_size);
        }
    }
};

// ---------------------------------------------------------------------------
// Kameran.

int check_frustum() {
    int fails = 0;
    const double eye[3] = {0, 0, 0};
    // 90 graders synfält på höjden och en kvadratisk bild: tan(45°) = 1.
    ViewFrustum v = ViewFrustum::orbit(eye, 0, 0, 90, 100, 100);
    CHECK(std::fabs(v.pixels_per_radian() - 50) < 1e-9);

    const auto box = [](double x0, double y0, double z0, double x1, double y1, double z1) {
        Box b;
        b.min[0] = x0;
        b.min[1] = y0;
        b.min[2] = z0;
        b.max[0] = x1;
        b.max[1] = y1;
        b.max[2] = z1;
        return b;
    };
    // Framför, bakom, vid sidan, på kanten av synfältet och runt ögat.
    const Box front = box(-1, 10, -1, 1, 12, 1);
    CHECK(v.sees(front));
    CHECK(std::fabs(v.distance(front) - 10) < 1e-12);
    const Box behind = box(-1, -12, -1, 1, -10, 1);
    CHECK(!v.sees(behind));
    CHECK(std::fabs(v.distance(behind) - 10) < 1e-12);
    CHECK(!v.sees(box(50, 10, -1, 52, 12, 1)));    // långt till höger
    CHECK(v.sees(box(5, 10, -1, 6, 12, 1)));       // inom 45 grader
    CHECK(!v.sees(box(-52, 10, -1, -50, 12, 1)));  // långt till vänster
    CHECK(!v.sees(box(-1, 10, 40, 1, 12, 42)));    // långt över
    CHECK(!v.sees(box(-1, 10, -42, 1, 12, -40)));  // långt under
    const Box around = box(-1, -1, -1, 1, 1, 1);
    CHECK(v.distance(around) == 0);
    CHECK(v.sees(around));
    // Delvis bakom ögats plan räcker.
    CHECK(v.sees(box(-1, -5, -1, 1, 5, 1)));
    // En ogiltig låda syns inte och har inget avstånd.
    CHECK(!v.sees(Box{}));
    CHECK(v.distance(Box{}) == kInf);
    // Avståndet är till närmaste punkt, också snett.
    CHECK(std::fabs(v.distance(box(3, 4, 0, 4, 5, 1)) - 5) < 1e-12);

    // En låda inuti en annan syns bara när den yttre gör det.
    const Box outer = box(-1, 10, -1, 60, 12, 1);
    const Box inner = box(50, 10, -1, 52, 12, 1);
    CHECK(v.sees(outer));
    CHECK(!v.sees(inner));

    // Azimut 90 ser mot +x.
    ViewFrustum east = ViewFrustum::orbit(eye, 90, 0, 90, 100, 100);
    CHECK(east.sees(box(10, -1, -1, 12, 1, 1)));
    CHECK(!east.sees(box(-12, -1, -1, -10, 1, 1)));

    // En bred bild ser mer åt sidan än en kvadratisk.
    ViewFrustum wide = ViewFrustum::orbit(eye, 0, 0, 90, 200, 100);
    CHECK(wide.sees(box(15, 10, -1, 16, 12, 1)));
    CHECK(!v.sees(box(15, 10, -1, 16, 12, 1)));

    // Utan storlek eller synfält finns inga px.
    ViewFrustum empty = v;
    empty.height = 0;
    CHECK(empty.pixels_per_radian() == 0);
    CHECK(!empty.sees(front));
    empty = v;
    empty.tan_half_fov = 0;
    CHECK(empty.pixels_per_radian() == 0);

    // pixels_at är oförändrad: 1 m på 10 m djup är K / 10 px.
    const double p[3] = {0, 10, 0};
    CHECK(std::fabs(v.pixels_at(p, 1) - 5) < 1e-9);
    const double back[3] = {0, -10, 0};
    CHECK(v.pixels_at(back, 1) == 0);
    return fails;
}

// ---------------------------------------------------------------------------
// Budgetarna, i RAM och på grafikkortet.

int check_budgets() {
    int fails = 0;
    std::vector<uint32_t> evict;

    // RAM-budgeten räcker för tre bitar: tre byggs, och minnets tröskel går över
    // den fjärde.
    {
        Rig rig({100, 90, 80, 70});
        TileScheduler s;
        s.reset(rig.tree, settings({3 * kEstimateRam + 10, kHuge}));
        s.update(rig.tree, rig.view, 0);
        CHECK(s.threshold() > s.pixels(3));
        CHECK(s.threshold() <= s.pixels(2));
        CHECK(s.pixels(3) > 69 && s.pixels(3) < 71);
        const std::vector<uint32_t> jobs = next_jobs(s, 10, evict);
        CHECK(jobs == (std::vector<uint32_t>{0, 1, 2}));
        CHECK(evict.empty());
        CHECK(s.pending() == 3);
        CHECK(s.idle() == false);
        // Inget mer önskas, med de tre påbörjade.
        CHECK(next_jobs(s, 10, evict).empty());
    }
    // Grafikkortets budget räcker för två: två byggs.
    {
        Rig rig({100, 90, 80, 70});
        TileScheduler s;
        s.reset(rig.tree, settings({kHuge, 2 * kEstimateGpu + 10}));
        s.update(rig.tree, rig.view, 0);
        CHECK(next_jobs(s, 10, evict) == (std::vector<uint32_t>{0, 1}));
        CHECK(s.budget().ram == kHuge && s.budget().gpu == 2 * kEstimateGpu + 10);
    }
    // Det som släpps räknas av i båda: grafikkortet rymmer två byggda bitar, och
    // en större bit som ska in släpper den minsta av dem.
    {
        Rig rig({50, 40, 0});
        TileScheduler s;
        // Två uppskattningar ryms, men inte två byggda och en uppskattning till;
        // att släppa en byggd räcker.
        s.reset(rig.tree, settings({kHuge, 2 * kEstimateGpu + 100}));
        s.update(rig.tree, rig.view, 0);
        std::vector<uint32_t> jobs = next_jobs(s, 10, evict);
        CHECK(jobs == (std::vector<uint32_t>{0, 1}));
        for (const uint32_t t : jobs) CHECK(done(s, t, kBuilt, true, 0, evict));
        CHECK(evict.empty());
        CHECK(s.resident().ram == 2 * kBuilt.ram && s.resident().gpu == 2 * kBuilt.gpu);
        CHECK(s.built_count() == 2);

        rig.place(2, 200);
        s.update(rig.tree, rig.view, 1);
        jobs = next_jobs(s, 10, evict);
        CHECK(jobs == (std::vector<uint32_t>{2}));
        CHECK(evict == (std::vector<uint32_t>{1}));
        CHECK(!s.built(1) && s.built(0));
        CHECK(s.resident().ram == kBuilt.ram && s.resident().gpu == kBuilt.gpu);
        evict.clear();
        CHECK(done(s, 2, kBuilt, true, 1, evict));
        CHECK(evict.empty());
        CHECK(s.resident().ram == 2 * kBuilt.ram && s.resident().gpu == 2 * kBuilt.gpu);
        CHECK(s.evictions() == 1);
    }
    // En gemensam budget räknar RAM och grafikkort tillsammans: den rymmer två
    // bitar, också med grafikkortets egen budget i en byte; utan den är det
    // grafikkortets byte som stoppar.
    {
        Rig rig({100, 90, 80});
        const size_t joint = 2 * (kEstimateRam + kEstimateGpu) + 10;
        {
            TileScheduler s;
            s.reset(rig.tree, settings({joint, 1}, true));
            s.update(rig.tree, rig.view, 0);
            CHECK(next_jobs(s, 10, evict) == (std::vector<uint32_t>{0, 1}));
        }
        {
            TileScheduler s;
            s.reset(rig.tree, settings({joint, 1}, false));
            s.update(rig.tree, rig.view, 0);
            CHECK(next_jobs(s, 10, evict).empty());
        }
    }
    // En byggd bit som inte ryms behålls inte, och dess byte räknas inte.
    {
        Rig rig({100});
        TileScheduler s;
        s.reset(rig.tree, settings({size_t(1) << 20, kHuge}));
        s.update(rig.tree, rig.view, 0);
        CHECK(next_jobs(s, 1, evict).size() == 1);
        CHECK(!done(s, 0, PartBytes{size_t(5) << 20, 1000}, true, 0, evict));
        CHECK(!s.built(0));
        CHECK(s.resident().ram == 0 && s.resident().gpu == 0);
        CHECK(s.pending() == 0);
    }
    // Plockningens byte läggs till en byggd bit och släpps med den; en bit som
    // inte är byggd påverkas inte.
    {
        Rig rig({100, 90});
        TileScheduler s;
        s.reset(rig.tree, settings({kHuge, kHuge}));
        s.update(rig.tree, rig.view, 0);
        const std::vector<uint32_t> jobs = next_jobs(s, 1, evict);
        CHECK(jobs == (std::vector<uint32_t>{0}));
        CHECK(done(s, 0, kBuilt, true, 0, evict));
        s.add_bytes(0, PartBytes{20000, 0});
        s.add_bytes(1, PartBytes{777, 777});
        CHECK(s.resident().ram == kBuilt.ram + 20000 && s.resident().gpu == kBuilt.gpu);
        CHECK(s.invalidate(0));
        CHECK(s.resident().ram == 0 && s.resident().gpu == 0);
        s.add_bytes(0, PartBytes{5, 5});   // släppt: ingenting händer
        CHECK(s.resident().ram == 0 && s.resident().gpu == 0);
        s.add_bytes(99, PartBytes{5, 5});  // utanför: ingenting händer
        CHECK(!s.built(99) && s.pixels(99) == 0 && !s.invalidate(99));
    }
    // Högst max_jobs åt gången, och ingen bit två gånger.
    {
        Rig rig({100, 90, 80, 70});
        TileScheduler s;
        s.reset(rig.tree, settings({kHuge, kHuge}));
        s.update(rig.tree, rig.view, 0);
        CHECK(next_jobs(s, 2, evict) == (std::vector<uint32_t>{0, 1}));
        CHECK(next_jobs(s, 2, evict) == (std::vector<uint32_t>{2, 3}));
        CHECK(next_jobs(s, 2, evict).empty());
        CHECK(next_jobs(s, 0, evict).empty());
        s.update(rig.tree, rig.view, 1);
        CHECK(next_jobs(s, 10, evict).empty());
        CHECK(s.pending() == 4);
    }
    return fails;
}

// ---------------------------------------------------------------------------
// Minnets tröskel och hysteresen.

int check_threshold() {
    int fails = 0;
    std::vector<uint32_t> evict;
    const auto above = [](double v) { return std::nextafter(v, kInf); };

    // Allt ryms: tröskeln är min_pixels.
    {
        Rig rig({100, 90, 80});
        TileScheduler s;
        s.reset(rig.tree, settings({kHuge, kHuge}));
        CHECK(s.threshold() == 3);
        s.update(rig.tree, rig.view, 0);
        CHECK(s.threshold() == 3);
    }
    // Högre än max_pixels höjs den inte, och det som inte ryms byggs inte.
    {
        Rig rig({100, 90, 80, 70});
        TileScheduler::Settings st = settings({2 * kEstimateRam + 10, kHuge});
        st.max_pixels = 75;
        TileScheduler s;
        s.reset(rig.tree, st);
        s.update(rig.tree, rig.view, 0);
        CHECK(s.threshold() == 75);
        CHECK(next_jobs(s, 10, evict) == (std::vector<uint32_t>{0, 1}));
        // Tre mäts minst 75 px men bara två ryms: ett hål, och de påbörjade
        // räknas också.
        CHECK(s.holes(1.0) == 3);
        CHECK(s.holes(2.0) == 0);   // inget över 150 px
    }
    // Hysteres: tröskeln höjs genast och sänks först när det ryms i 0,9 av
    // budgeten.
    {
        Rig rig({100, 90, 80, 70, 60});
        TileScheduler s;
        s.reset(rig.tree, settings({3 * kEstimateRam + 10, kHuge}));
        s.update(rig.tree, rig.view, 0);
        CHECK(s.threshold() == above(s.pixels(3)));

        // Den fjärde blir större: tröskeln höjs genast.
        rig.place(3, 75);
        s.update(rig.tree, rig.view, 1);
        const double raised = s.threshold();
        CHECK(raised == above(s.pixels(3)));
        CHECK(raised > 74 && raised < 76);

        // Tre ryms vid 60, men inte i 0,9 av budgeten: tröskeln står kvar.
        rig.place(2, 50);
        s.update(rig.tree, rig.view, 2);
        CHECK(s.threshold() == raised);
        CHECK(s.threshold() > s.pixels(4));

        // Nu ryms tre med marginal vid 60: tröskeln sänks dit.
        rig.place(3, 20);
        s.update(rig.tree, rig.view, 3);
        CHECK(s.threshold() == above(s.pixels(4)));
        CHECK(s.threshold() < raised);
        CHECK(s.pixels(4) > 59 && s.pixels(4) < 61);
    }
    // Det som är byggt räknas med sin verkliga storlek, och uppskattningen för
    // resten följer den: två byggda bitar som tog hälften av uppskattningen ger
    // plats för den tredje, och tröskeln sänks.
    {
        Rig rig({100, 90, 80});
        TileScheduler s;
        s.reset(rig.tree, settings({120100, kHuge}));
        s.update(rig.tree, rig.view, 0);
        CHECK(s.threshold() > s.pixels(2));   // tre uppskattningar ryms inte
        const std::vector<uint32_t> jobs = next_jobs(s, 10, evict);
        CHECK(jobs == (std::vector<uint32_t>{0, 1}));
        for (const uint32_t t : jobs) CHECK(done(s, t, PartBytes{20000, 16000}, true, 0, evict));
        s.update(rig.tree, rig.view, 1);
        CHECK(s.threshold() == 3);
        CHECK(next_jobs(s, 10, evict) == (std::vector<uint32_t>{2}));
        CHECK(evict.empty());
    }
    return fails;
}

// ---------------------------------------------------------------------------
// De fasta bitarna.

int check_pinned() {
    int fails = 0;
    std::vector<uint32_t> evict;

    // Bitarna 2 är fast: den byggs först och bygget släpper aldrig den.
    {
        Rig rig({100, 90, 80}, {2});
        TileScheduler s;
        s.reset(rig.tree, settings({2 * kEstimateRam + 10, kHuge}));
        s.update(rig.tree, rig.view, 0);
        // Den fasta tar en plats av två; en till ryms.
        const std::vector<uint32_t> jobs = next_jobs(s, 10, evict);
        CHECK(jobs == (std::vector<uint32_t>{2, 0}));
        for (const uint32_t t : jobs) CHECK(done(s, t, kBuilt, true, 0, evict));
        CHECK(evict.empty());

        // Bit 1 blir störst och behöver plats: den släpper bit 0, inte den fasta.
        rig.place(1, 500);
        s.update(rig.tree, rig.view, 1);
        const std::vector<uint32_t> more = next_jobs(s, 10, evict);
        CHECK(more == (std::vector<uint32_t>{1}));
        CHECK(evict == (std::vector<uint32_t>{0}));
        CHECK(s.built(2) && !s.built(0));
        CHECK(done(s, 1, kBuilt, true, 1, evict));
        CHECK(s.built(2) && s.built(1));

        // Och bit 2, den fasta, släpps inte ens för en bit som mäts mer.
        rig.place(0, 900);
        s.update(rig.tree, rig.view, 2);
        evict.clear();
        CHECK(next_jobs(s, 10, evict) == (std::vector<uint32_t>{0}));
        CHECK(evict == (std::vector<uint32_t>{1}));
        CHECK(s.built(2));
        CHECK(s.evictions() == 2);
    }
    // En fast bit byggs också utanför bilden, och före allt annat.
    {
        Rig rig({0, 100}, {0});
        TileScheduler s;
        s.reset(rig.tree, settings({kHuge, kHuge}));
        s.update(rig.tree, rig.view, 0);
        CHECK(s.pixels(0) == 0);
        CHECK(!s.idle());
        CHECK(next_jobs(s, 1, evict) == (std::vector<uint32_t>{0}));
        CHECK(next_jobs(s, 1, evict) == (std::vector<uint32_t>{1}));
    }
    // Den fasta bitens byte räknas mot budgeten också när den inte är byggd, så
    // att minnets tröskel tar hänsyn till den.
    {
        Rig rig({100, 90}, {1});
        TileScheduler s;
        s.reset(rig.tree, settings({kEstimateRam + 10, kHuge}));
        s.update(rig.tree, rig.view, 0);
        // Den fasta tar hela budgeten: bit 0 behövs inte.
        CHECK(s.threshold() > s.pixels(0));
        CHECK(next_jobs(s, 10, evict) == (std::vector<uint32_t>{1}));
    }
    return fails;
}

// ---------------------------------------------------------------------------
// Vad som släpps först.

int check_release_order() {
    int fails = 0;
    std::vector<uint32_t> evict;

    // Det som ligger utanför bilden släpps före det som syns, också när det
    // som syns är mindre.
    {
        Rig rig({50, 40, 30, 0});
        TileScheduler s;
        s.reset(rig.tree, settings({3 * kEstimateRam + 10, kHuge}));
        s.update(rig.tree, rig.view, 0);
        const std::vector<uint32_t> jobs = next_jobs(s, 10, evict);
        CHECK(jobs == (std::vector<uint32_t>{0, 1, 2}));
        for (const uint32_t t : jobs) CHECK(done(s, t, kBuilt, true, 0, evict));

        rig.place(0, 0);     // ur bild
        rig.place(3, 45);    // in i bild
        s.update(rig.tree, rig.view, 1);
        CHECK(s.pixels(0) == 0);
        CHECK(s.threshold() == 3);
        evict.clear();
        CHECK(next_jobs(s, 10, evict) == (std::vector<uint32_t>{3}));
        CHECK(evict == (std::vector<uint32_t>{0}));
        CHECK(!s.built(0) && s.built(1) && s.built(2));
    }
    // Bland det som syns släpps den som mäts minst först.
    {
        Rig rig({100, 90, 80, 0});
        TileScheduler s;
        s.reset(rig.tree, settings({3 * kEstimateRam + 10, kHuge}));
        s.update(rig.tree, rig.view, 0);
        const std::vector<uint32_t> jobs = next_jobs(s, 10, evict);
        CHECK(jobs == (std::vector<uint32_t>{0, 1, 2}));
        for (const uint32_t t : jobs) CHECK(done(s, t, kBuilt, true, 0, evict));
        rig.place(0, 30);    // blir minst av de byggda
        rig.place(3, 200);   // en ny bit som mäts mest
        s.update(rig.tree, rig.view, 1);
        // Den minsta byggda ryms inte längre: den behövs inte.
        CHECK(s.threshold() > s.pixels(0));
        evict.clear();
        CHECK(next_jobs(s, 10, evict) == (std::vector<uint32_t>{3}));
        CHECK(evict == (std::vector<uint32_t>{0}));
        CHECK(!s.built(0) && s.built(1) && s.built(2));
    }
    // Den som väntar släpper bara bitar som mäts mindre än den. Utan det byggs
    // ingenting nytt, och de byggda står kvar. Tröskeln är klämd till 40.
    {
        Rig rig({100, 90, 0});
        TileScheduler::Settings st = settings({2 * kEstimateRam + 10, kHuge});
        st.max_pixels = 40;
        TileScheduler s;
        s.reset(rig.tree, st);
        s.update(rig.tree, rig.view, 0);
        const std::vector<uint32_t> jobs = next_jobs(s, 10, evict);
        CHECK(jobs == (std::vector<uint32_t>{0, 1}));
        for (const uint32_t t : jobs) CHECK(done(s, t, kBuilt, true, 0, evict));

        rig.place(2, 50);
        s.update(rig.tree, rig.view, 1);
        CHECK(s.threshold() == 40);
        CHECK(s.holes(1.0) == 1);
        evict.clear();
        CHECK(next_jobs(s, 10, evict).empty());
        CHECK(evict.empty());
        CHECK(s.built(0) && s.built(1));
        CHECK(s.idle());   // den som inte fick plats önskas inte igen i den här mätningen

        // Blir den större än de byggda släpper den den minsta av dem.
        rig.place(2, 150);
        s.update(rig.tree, rig.view, 2);
        CHECK(next_jobs(s, 10, evict) == (std::vector<uint32_t>{2}));
        CHECK(evict == (std::vector<uint32_t>{1}));
        CHECK(s.built(0) && !s.built(1));
        CHECK(s.evictions() == 1);
    }
    return fails;
}

// ---------------------------------------------------------------------------
// Misslyckade och ogiltiga bygge.

int check_failures() {
    int fails = 0;
    std::vector<uint32_t> evict;

    // Ett bygge som misslyckas prövas inte igen, förrän biten ogiltigförklaras.
    {
        Rig rig({100});
        TileScheduler s;
        s.reset(rig.tree, settings({kHuge, kHuge}));
        s.update(rig.tree, rig.view, 0);
        CHECK(next_jobs(s, 1, evict) == (std::vector<uint32_t>{0}));
        CHECK(!done(s, 0, PartBytes{}, false, 0, evict));
        CHECK(!s.built(0));
        CHECK(s.pending() == 0);
        s.update(rig.tree, rig.view, 1);
        CHECK(next_jobs(s, 1, evict).empty());
        CHECK(s.idle());
        CHECK(!s.invalidate(0));
        s.update(rig.tree, rig.view, 2);
        CHECK(!s.idle());
        CHECK(next_jobs(s, 1, evict) == (std::vector<uint32_t>{0}));
    }
    // Ett bygge för en bit som ogiltigförklarats under tiden räknas inte.
    {
        Rig rig({100});
        TileScheduler s;
        s.reset(rig.tree, settings({kHuge, kHuge}));
        s.update(rig.tree, rig.view, 0);
        CHECK(next_jobs(s, 1, evict) == (std::vector<uint32_t>{0}));
        CHECK(!s.invalidate(0));   // påbörjad, inte byggd
        CHECK(!done(s, 0, kBuilt, true, 0, evict));
        CHECK(!s.built(0));
        CHECK(s.resident().ram == 0 && s.pending() == 0);
        CHECK(next_jobs(s, 1, evict) == (std::vector<uint32_t>{0}));
        // Ett jobb som inte är påbörjat tas inte emot, och inte en bit utanför.
        CHECK(done(s, 0, kBuilt, true, 0, evict));
        CHECK(s.built(0));
        CHECK(!done(s, 0, kBuilt, true, 0, evict));
        CHECK(!done(s, 7, kBuilt, true, 0, evict));
    }
    return fails;
}

// ---------------------------------------------------------------------------
// Idle och hål.

int check_idle_and_holes() {
    int fails = 0;
    std::vector<uint32_t> evict;
    {
        Rig rig({100, 4});
        TileScheduler s;
        s.reset(rig.tree, settings({kHuge, kHuge}));
        // Utan någon mätning finns ingenting att bygga.
        CHECK(s.idle());
        CHECK(s.holes(1.0) == 0);
        s.update(rig.tree, rig.view, 0);
        CHECK(!s.idle());
        CHECK(s.holes(1.0) == 2);
        CHECK(s.holes(2.0) == 1);    // 6 px: bara den stora
        CHECK(s.holes(100.0) == 0);
        const std::vector<uint32_t> jobs = next_jobs(s, 10, evict);
        CHECK(jobs.size() == 2);
        CHECK(!s.idle());           // påbörjade
        CHECK(s.holes(1.0) == 2);   // inte byggda än
        CHECK(done(s, jobs[0], kBuilt, true, 0, evict));
        CHECK(!s.idle());
        CHECK(s.holes(1.0) == 1);
        CHECK(done(s, jobs[1], kBuilt, true, 0, evict));
        CHECK(s.idle());
        CHECK(s.holes(1.0) == 0);
        CHECK(s.built_count() == 2 && s.pending() == 0);
    }
    // En bit som är för liten byggs inte.
    {
        Rig rig({2.5});
        TileScheduler s;
        s.reset(rig.tree, settings({kHuge, kHuge}));
        s.update(rig.tree, rig.view, 0);
        CHECK(s.idle());
        CHECK(s.pixels(0) == 0);
    }
    return fails;
}

// ---------------------------------------------------------------------------
// Vad som visas.

int check_shown() {
    int fails = 0;
    std::vector<uint32_t> evict;
    {
        Rig rig({100, 90, 80, 70});
        TileScheduler s;
        s.reset(rig.tree, settings({kHuge, kHuge}));
        s.update(rig.tree, rig.view, 0);
        for (const uint32_t t : next_jobs(s, 10, evict)) CHECK(done(s, t, kBuilt, true, 0, evict));
        // En ny bit är inte visad.
        for (uint32_t t = 0; t < 4; ++t) CHECK(!s.shown(t));
        std::vector<uint32_t> show{99}, park{98};

        s.shown_changes(85, 10, show, park);
        CHECK(show == (std::vector<uint32_t>{0, 1}));
        CHECK(park.empty());
        CHECK(s.shown(0) && s.shown(1) && !s.shown(2));
        CHECK(s.shown_count() == 2);
        s.shown_changes(85, 10, show, park);
        CHECK(show.empty() && park.empty());

        // Hysteres: en visad bit parkeras först under 0,8 av ritningens tröskel.
        s.shown_changes(95, 10, show, park);
        CHECK(show.empty() && park.empty());   // 90 >= 76, och 100 är redan visad
        s.shown_changes(120, 10, show, park);
        CHECK(show.empty());
        CHECK(park == (std::vector<uint32_t>{1}));   // 90 < 96; 100 står kvar
        CHECK(!s.shown(1) && s.shown(0) && s.shown_count() == 1);

        // Högst max_changes, största först.
        s.shown_changes(1, 2, show, park);
        CHECK(show == (std::vector<uint32_t>{1, 2}));
        CHECK(park.empty());
        s.shown_changes(1, 2, show, park);
        CHECK(show == (std::vector<uint32_t>{3}));
        CHECK(s.shown_count() == 4);
        s.shown_changes(1, 0, show, park);
        CHECK(show.empty() && park.empty());

        // En bit som släpps är inte längre visad.
        CHECK(s.invalidate(0));
        CHECK(!s.shown(0) && s.shown_count() == 3);
    }
    // Visa och parkera växelvis under taket.
    {
        Rig rig({100, 90, 80, 70});
        TileScheduler s;
        s.reset(rig.tree, settings({kHuge, kHuge}));
        s.update(rig.tree, rig.view, 0);
        for (const uint32_t t : next_jobs(s, 10, evict)) CHECK(done(s, t, kBuilt, true, 0, evict));
        std::vector<uint32_t> show, park;
        s.shown_changes(95, 10, show, park);
        CHECK(show == (std::vector<uint32_t>{0}));

        rig.place(0, 0);     // ur bild: parkeras
        rig.place(1, 200);   // större än gränsen: visas
        s.update(rig.tree, rig.view, 1);
        CHECK(s.pixels(0) == 0);
        s.shown_changes(95, 1, show, park);
        CHECK(show == (std::vector<uint32_t>{1}));
        CHECK(park.empty());
        s.shown_changes(95, 1, show, park);
        CHECK(show.empty());
        CHECK(park == (std::vector<uint32_t>{0}));
        CHECK(!s.shown(0) && s.shown(1));
    }
    {
        Rig rig({100, 90, 80, 70});
        TileScheduler s;
        s.reset(rig.tree, settings({kHuge, kHuge}));
        s.update(rig.tree, rig.view, 0);
        for (const uint32_t t : next_jobs(s, 10, evict)) CHECK(done(s, t, kBuilt, true, 0, evict));
        std::vector<uint32_t> show, park;
        s.shown_changes(60, 10, show, park);   // alla fyra visas
        CHECK(show.size() == 4);
        rig.place(0, 0);
        rig.place(1, 0);
        s.update(rig.tree, rig.view, 1);
        // Två ligger nu ur bild och ska parkeras, en i taget under taket 1.
        s.shown_changes(60, 1, show, park);
        CHECK(show.empty() && park.size() == 1);
        s.shown_changes(60, 5, show, park);
        CHECK(show.empty() && park.size() == 1);
        CHECK(s.shown_count() == 2);
    }
    return fails;
}

// ---------------------------------------------------------------------------
// Latensen.

int check_latency() {
    int fails = 0;
    std::vector<uint32_t> evict;
    {
        Rig rig({100, 90});
        TileScheduler s;
        s.reset(rig.tree, settings({kHuge, kHuge}));
        CHECK(s.latency().count == 0);
        s.update(rig.tree, rig.view, 1.0);
        CHECK(next_jobs(s, 10, evict) == (std::vector<uint32_t>{0, 1}));
        CHECK(done(s, 0, kBuilt, true, 1.25, evict));
        CHECK(done(s, 1, kBuilt, true, 1.75, evict));
        const TileScheduler::Latency l = s.latency();
        CHECK(l.count == 2);
        CHECK(std::fabs(l.p50 - 0.25) < 1e-12);
        CHECK(std::fabs(l.p95 - 0.75) < 1e-12);
        CHECK(std::fabs(l.max - 0.75) < 1e-12);
    }
    // Tiden räknas från att biten först behövs, och en bit som slutar behövas
    // börjar om.
    {
        Rig rig({100, 90, 80});
        TileScheduler s;
        s.reset(rig.tree, settings({kHuge, kHuge}));
        s.update(rig.tree, rig.view, 1.0);
        CHECK(next_jobs(s, 1, evict) == (std::vector<uint32_t>{0}));
        CHECK(done(s, 0, kBuilt, true, 1.2, evict));

        rig.place(1, 0);   // slutar behövas
        s.update(rig.tree, rig.view, 2.0);
        rig.place(1, 90);  // behövs igen
        s.update(rig.tree, rig.view, 3.0);
        CHECK(next_jobs(s, 10, evict) == (std::vector<uint32_t>{1, 2}));
        CHECK(done(s, 1, kBuilt, true, 3.5, evict));
        CHECK(done(s, 2, kBuilt, true, 4.0, evict));

        const TileScheduler::Latency l = s.latency();
        CHECK(l.count == 3);
        CHECK(std::fabs(l.p50 - 0.5) < 1e-12);   // 0,2; 0,5; 3,0
        CHECK(std::fabs(l.p95 - 3.0) < 1e-12);
        CHECK(std::fabs(l.max - 3.0) < 1e-12);
    }
    // En bit som släpps och behövs igen mäts på nytt; ett misslyckat bygge ger
    // ingen tid.
    {
        Rig rig({100});
        TileScheduler s;
        s.reset(rig.tree, settings({kHuge, kHuge}));
        s.update(rig.tree, rig.view, 0.0);
        CHECK(next_jobs(s, 1, evict) == (std::vector<uint32_t>{0}));
        CHECK(!done(s, 0, PartBytes{}, false, 0.5, evict));
        CHECK(s.latency().count == 0);
    }
    return fails;
}

// ---------------------------------------------------------------------------
// Förutsägelsen: kameran om en stund.

// Kameran vänd åt andra hållet än Rig::view: den ser bitarna som ligger bakom
// ögat (cirka 12 px för en bit som Rig placerar med 0 px).
ViewFrustum turned_view() {
    const double eye[3] = {0, 0, 0};
    return ViewFrustum::orbit(eye, 180, 0, 45, 1000, 1000);
}

int check_ahead() {
    int fails = 0;
    std::vector<uint32_t> evict;
    const ViewFrustum ahead = turned_view();

    // Det som `ahead` ser byggs efter det som behövs nu, närmast först, och
    // räknas varken som hål, i latensen eller i minnets tröskel.
    {
        Rig rig({100, 0, 0});
        TileScheduler s;
        s.reset(rig.tree, settings({kHuge, kHuge}));
        s.update(rig.tree, rig.view, &ahead, 1.0);
        CHECK(s.threshold() == 3);
        CHECK(s.pixels(0) > 99 && s.pixels(1) == 0 && s.pixels(2) == 0);
        CHECK(s.holes(1.0) == 1);
        CHECK(!s.idle());
        CHECK(next_jobs(s, 1, evict) == (std::vector<uint32_t>{0}));
        CHECK(next_jobs(s, 10, evict) == (std::vector<uint32_t>{1, 2}));
        CHECK(s.pending() == 3);
        CHECK(!s.idle());   // påbörjade
        for (uint32_t t = 0; t < 3; ++t) CHECK(done(s, t, kBuilt, true, 1.5, evict));
        CHECK(evict.empty());
        CHECK(s.idle());
        const TileScheduler::Latency l = s.latency();
        CHECK(l.count == 1);
        CHECK(std::fabs(l.max - 0.5) < 1e-12);
        // De förhandsbyggda är inte i bild och visas inte.
        std::vector<uint32_t> show, park;
        s.shown_changes(10, 10, show, park);
        CHECK(show == (std::vector<uint32_t>{0}));
        CHECK(park.empty());
    }
    // Utan `ahead`, eller med nullptr, byggs bara det som behövs: samma som
    // den gamla anropsformen.
    {
        Rig rig({100, 90, 0});
        TileScheduler a, b;
        a.reset(rig.tree, settings({kHuge, kHuge}));
        b.reset(rig.tree, settings({kHuge, kHuge}));
        a.update(rig.tree, rig.view, 0.0);
        b.update(rig.tree, rig.view, nullptr, 0.0);
        for (uint32_t t = 0; t < 3; ++t) CHECK(a.pixels(t) == b.pixels(t));
        CHECK(a.threshold() == b.threshold());
        std::vector<uint32_t> ea, eb;
        CHECK(next_jobs(a, 10, ea) == (std::vector<uint32_t>{0, 1}));
        CHECK(next_jobs(b, 10, eb) == (std::vector<uint32_t>{0, 1}));
        CHECK(a.idle() == b.idle());
    }
    // Förhandsbygget höjer inte minnets tröskel och tränger inte undan det som
    // behövs: med plats för en bit byggs den som behövs nu, och ingenting
    // annat.
    {
        Rig rig({100, 0, 0});
        TileScheduler s;
        s.reset(rig.tree, settings({kEstimateRam + 10, kHuge}));
        s.update(rig.tree, rig.view, &ahead, 0.0);
        CHECK(s.threshold() == 3);
        CHECK(next_jobs(s, 10, evict) == (std::vector<uint32_t>{0}));
        CHECK(evict.empty());
        CHECK(done(s, 0, kBuilt, true, 0, evict));
        CHECK(s.idle());   // de som inte fick plats önskas inte igen i den här mätningen
    }
    // Plats för två: den som behövs och den närmaste av förhandsbitarna.
    {
        Rig rig({100, 0, 0});
        TileScheduler s;
        s.reset(rig.tree, settings({2 * kEstimateRam + 100, kHuge}));
        s.update(rig.tree, rig.view, &ahead, 0.0);
        CHECK(next_jobs(s, 10, evict) == (std::vector<uint32_t>{0, 1}));
        CHECK(evict.empty());
    }
    // En förhandsbit släpper ingenting som `ahead` ser, men det som behövs nu
    // släpper det om inget annat finns.
    {
        Rig rig({100, 90, 0});
        TileScheduler s;
        s.reset(rig.tree, settings({2 * kEstimateRam + 100, kHuge}));
        s.update(rig.tree, rig.view, 0.0);
        const std::vector<uint32_t> jobs = next_jobs(s, 10, evict);
        CHECK(jobs == (std::vector<uint32_t>{0, 1}));
        for (const uint32_t t : jobs) CHECK(done(s, t, kBuilt, true, 0, evict));

        rig.place(1, 0);   // ur bild, men inom `ahead`
        s.update(rig.tree, rig.view, &ahead, 1.0);
        CHECK(next_jobs(s, 10, evict).empty());   // bit 2 är en förhandsbit och får inte släppa bit 1
        CHECK(evict.empty());
        CHECK(s.built(0) && s.built(1));
        CHECK(s.idle());

        rig.place(2, 200);   // behövs nu
        s.update(rig.tree, rig.view, &ahead, 2.0);
        CHECK(next_jobs(s, 10, evict) == (std::vector<uint32_t>{2}));
        CHECK(evict == (std::vector<uint32_t>{1}));
        CHECK(s.built(0) && !s.built(1));
    }
    return fails;
}

// ---------------------------------------------------------------------------
// Avbrott.

int check_cancellations() {
    int fails = 0;
    std::vector<uint32_t> evict;

    // Påbörjade bitar under N_eff / 4 (0,75 px) avbryts, en gång var, och de
    // byggs igen när de behövs.
    {
        Rig rig({100, 90, 80, 70});
        TileScheduler s;
        s.reset(rig.tree, settings({kHuge, kHuge}));
        s.update(rig.tree, rig.view, 0);
        CHECK(s.take_cancellations().empty());
        CHECK(next_jobs(s, 10, evict) == (std::vector<uint32_t>{0, 1, 2, 3}));
        rig.place(1, 0);     // ur bild
        rig.place(2, 0.8);   // över gränsen
        rig.place(3, 0.7);   // under gränsen
        s.update(rig.tree, rig.view, 1);
        CHECK(s.take_cancellations() == (std::vector<uint32_t>{1, 3}));
        CHECK(s.take_cancellations().empty());
        CHECK(s.cancelled() == 2);
        CHECK(s.pending() == 4);   // jobben är kvar tills de tas emot

        // Ett avbrutet jobb tas emot som ett jobb utan resultat, och biten är
        // inte misslyckad.
        CHECK(!done(s, 1, kBuilt, true, 1, evict));
        CHECK(!done(s, 3, PartBytes{}, false, 1, evict));
        CHECK(!s.built(1) && !s.built(3));
        CHECK(s.pending() == 2);
        CHECK(s.resident().ram == 0 && s.resident().gpu == 0);

        rig.place(1, 90);
        rig.place(3, 70);
        s.update(rig.tree, rig.view, 2);
        CHECK(s.take_cancellations().empty());
        CHECK(next_jobs(s, 10, evict) == (std::vector<uint32_t>{1, 3}));
        CHECK(done(s, 1, kBuilt, true, 3, evict));
        CHECK(done(s, 3, kBuilt, true, 3, evict));
        CHECK(s.built(1) && s.built(3));
    }
    // En bit som stiger igen före take_cancellations() avbryts inte.
    {
        Rig rig({100, 90});
        TileScheduler s;
        s.reset(rig.tree, settings({kHuge, kHuge}));
        s.update(rig.tree, rig.view, 0);
        CHECK(next_jobs(s, 10, evict).size() == 2);
        rig.place(1, 0);
        s.update(rig.tree, rig.view, 1);
        rig.place(1, 90);
        s.update(rig.tree, rig.view, 2);
        CHECK(s.take_cancellations().empty());
    }
    // Bitar som tagits emot eller ogiltigförklarats sedan mätningen avbryts inte.
    {
        Rig rig({100, 90, 80});
        TileScheduler s;
        s.reset(rig.tree, settings({kHuge, kHuge}));
        s.update(rig.tree, rig.view, 0);
        CHECK(next_jobs(s, 10, evict).size() == 3);
        rig.place(0, 0);
        rig.place(1, 0);
        rig.place(2, 0);
        s.update(rig.tree, rig.view, 1);
        CHECK(done(s, 0, kBuilt, true, 1, evict));   // klar
        CHECK(!s.invalidate(1));                          // ogiltigförklarad medan den byggs
        CHECK(s.take_cancellations() == (std::vector<uint32_t>{2}));
    }
    // Fasta bitar avbryts aldrig; byggda bitar inte heller.
    {
        Rig rig({100, 90, 80}, {1});
        TileScheduler s;
        s.reset(rig.tree, settings({kHuge, kHuge}));
        s.update(rig.tree, rig.view, 0);
        CHECK(next_jobs(s, 10, evict) == (std::vector<uint32_t>{1, 0, 2}));
        CHECK(done(s, 0, kBuilt, true, 0, evict));
        rig.place(0, 0);
        rig.place(1, 0);
        rig.place(2, 0);
        s.update(rig.tree, rig.view, 1);
        CHECK(s.take_cancellations() == (std::vector<uint32_t>{2}));
    }
    // En bit som `ahead` ser avbryts inte; utan `ahead` avbryts den.
    {
        const ViewFrustum ahead = turned_view();
        Rig rig({100, 0});
        TileScheduler s;
        s.reset(rig.tree, settings({kHuge, kHuge}));
        s.update(rig.tree, rig.view, &ahead, 0.0);
        CHECK(next_jobs(s, 10, evict) == (std::vector<uint32_t>{0, 1}));
        s.update(rig.tree, rig.view, &ahead, 1.0);
        CHECK(s.take_cancellations().empty());
        s.update(rig.tree, rig.view, nullptr, 2.0);
        CHECK(s.take_cancellations() == (std::vector<uint32_t>{1}));
    }
    // Bitar inom near_radius avbryts inte, hur litet de än mäts.
    {
        Rig rig({100, 90});
        TileScheduler s;
        s.reset(rig.tree, settings({kHuge, kHuge}));
        s.update(rig.tree, rig.view, 0);
        CHECK(next_jobs(s, 10, evict).size() == 2);
        rig.place(1, 0);                     // 100 m bakom ögat
        rig.tree.tiles[1].max_size = 0.005f; // 0,03 px
        ViewFrustum near_view = rig.view;
        near_view.near_radius = 150;
        s.update(rig.tree, near_view, 1);
        CHECK(s.take_cancellations().empty());
        near_view.near_radius = 50;
        s.update(rig.tree, near_view, 2);
        CHECK(s.take_cancellations() == (std::vector<uint32_t>{1}));
    }
    // Gränsen följer minnets tröskel: vid N_eff = 40 avbryts det som mäts under
    // 10 px.
    {
        Rig rig({100, 90, 80, 70});
        TileScheduler::Settings st = settings({kHuge, kHuge});
        st.min_pixels = 40;
        TileScheduler s;
        s.reset(rig.tree, st);
        s.update(rig.tree, rig.view, 0);
        CHECK(next_jobs(s, 10, evict).size() == 4);
        rig.place(2, 12);
        rig.place(3, 8);
        s.update(rig.tree, rig.view, 1);
        CHECK(s.threshold() == 40);
        CHECK(s.take_cancellations() == (std::vector<uint32_t>{3}));
    }
    return fails;
}

// ---------------------------------------------------------------------------
// De visade bitarna, närmast ögat först.

int check_shown_order() {
    int fails = 0;
    std::vector<uint32_t> evict;
    Rig rig({100, 90, 80, 70});
    // Bitarna 1 och 3 på samma plats, 2 längre bort och 0 längst bort.
    const auto at = [&rig](size_t i, double y) {
        Box& b = rig.tree.tiles[i].bounds;
        b.min[0] = 0;
        b.max[0] = 0.1;
        b.min[1] = y;
        b.max[1] = y + 0.1;
    };
    at(0, 300);
    at(1, 50);
    at(2, 200);
    at(3, 50);
    TileScheduler s;
    s.reset(rig.tree, settings({kHuge, kHuge}));
    s.update(rig.tree, rig.view, 0);
    for (const uint32_t t : next_jobs(s, 10, evict)) CHECK(done(s, t, kBuilt, true, 0, evict));

    std::vector<uint32_t> out{99, 98};
    s.shown_order(rig.view, out);
    CHECK(out.empty());   // ingenting visat än

    // Bara det som mäts minst 150 px visas (bit 1 med 180 px); byggda men
    // ovisade bitar är inte med.
    std::vector<uint32_t> show, park;
    s.shown_changes(150, 10, show, park);
    CHECK(show == (std::vector<uint32_t>{1}));
    s.shown_order(rig.view, out);
    CHECK(out == (std::vector<uint32_t>{1}));

    s.shown_changes(1, 10, show, park);
    CHECK(s.shown_count() == 4);
    s.shown_order(rig.view, out);
    CHECK(out == (std::vector<uint32_t>{1, 3, 2, 0}));   // 1 och 3 på samma avstånd: nummerordning

    // Från en annan plats: bit 0 är närmast, och 1 och 3 längst bort.
    const double eye[3] = {0, 400, 0};
    const ViewFrustum other = ViewFrustum::orbit(eye, 0, 0, 45, 1000, 1000);
    s.shown_order(other, out);
    CHECK(out == (std::vector<uint32_t>{0, 2, 1, 3}));

    // En parkerad och en släppt bit är inte längre med.
    s.shown_changes(1e6, 1, show, park);   // allt är under gränsen: den minsta, bit 0, parkeras
    CHECK(show.empty() && park == (std::vector<uint32_t>{0}));
    s.shown_order(rig.view, out);
    CHECK(out == (std::vector<uint32_t>{1, 3, 2}));
    CHECK(s.invalidate(2));
    s.shown_order(rig.view, out);
    CHECK(out == (std::vector<uint32_t>{1, 3}));
    CHECK(s.shown_count() == 2);
    return fails;
}

// ---------------------------------------------------------------------------
// Bitar byggda till den storlek som behövs: bitar med egna fragmentlistor.

// Bitar på rad framför ögat som i Rig, men med fragment: bit i har fragment med
// storlekarna sizes[i] (störst först) och prims[i] primitiver vardera, och står
// vid x = 0,2 · i, y = 100. Ögat står i origo och ser mot +y med 45 graders
// synfält och 1000 px bildhöjd.
struct FragRig {
    LodTree tree;
    ViewFrustum view;

    FragRig(const std::vector<std::vector<float>>& sizes,
            const std::vector<std::vector<uint32_t>>& prims,
            const std::vector<size_t>& pinned = {}) {
        view = eye_at(0);
        const size_t count = sizes.size();
        tree.nodes.resize(count + 1);
        tree.tiles.resize(count);
        tree.nodes[0].child_count = static_cast<uint32_t>(count);
        uint32_t next_fragment = 0;
        for (uint32_t i = 0; i < count; ++i) {
            tree.children.push_back(i + 1);
            tree.nodes[i + 1].parent = 0;
            tree.nodes[i + 1].first_tile = i;
            tree.nodes[i + 1].tile_count = 1;
            Tile& t = tree.tiles[i];
            t.node = i + 1;
            t.frag_begin = static_cast<uint32_t>(tree.frags.size());
            t.frag_count = static_cast<uint32_t>(sizes[i].size());
            uint64_t running = 0;
            for (size_t k = 0; k < sizes[i].size(); ++k) {
                tree.frags.push_back(next_fragment++);
                tree.sizes.push_back(sizes[i][k]);
                running += prims[i][k];
                tree.prefix_primitives.push_back(static_cast<uint32_t>(running));
            }
            t.primitives = running;
            t.max_size = sizes[i].front();
            t.min_size = sizes[i].back();
            const double x = 0.2 * static_cast<double>(i);
            t.bounds = Box{};
            t.bounds.min[0] = x;
            t.bounds.max[0] = x + 0.1;
            t.bounds.min[1] = 100;
            t.bounds.max[1] = 100.1;
            t.bounds.min[2] = 0;
            t.bounds.max[2] = 0.1;
            tree.nodes[i + 1].bounds = t.bounds;
            tree.nodes[i + 1].max_size = t.max_size;
            tree.nodes[0].bounds.add(t.bounds);
            tree.nodes[0].max_size = std::max(tree.nodes[0].max_size, t.max_size);
        }
        for (const size_t p : pinned) tree.tiles[p].pinned = true;
    }

    // Ögat på (0, y, 0), med blicken mot +y.
    static ViewFrustum eye_at(double y) {
        const double eye[3] = {0, y, 0};
        return ViewFrustum::orbit(eye, 0, 0, 45, 1000, 1000);
    }
};

// Minnet för ett prefix med så många primitiver, som schemaläggaren räknar det
// när inget är byggt.
PartBytes bytes_of_primitives(uint64_t primitives) {
    const size_t p = static_cast<size_t>(primitives);
    return {p * (kRamBytesPerVertex + kIndexBytesPerTriangle),
            p * (kGpuBytesPerVertex + kIndexBytesPerTriangle)};
}
PartBytes estimate_of_primitives(uint64_t primitives) {
    PartBytes b = bytes_of_primitives(primitives);
    b.ram += 4096;
    b.gpu += 4096;
    return b;
}

// Tio fragment som halveras i storlek, från 10 m, med få primitiver bland de
// stora och många bland de små.
std::vector<float> halving_sizes() {
    std::vector<float> s;
    for (int k = 0; k < 10; ++k) s.push_back(10.0f / static_cast<float>(1 << k));
    return s;
}
std::vector<uint32_t> skewed_prims() { return {10, 10, 10, 10, 10, 10, 10, 300, 300, 300}; }

// Storleken följer avståndet och tröskeln, ombyggnaden kommer när ögat blir
// närmare och inte annars, och den gamla formen visas tills den nya är klar.
int check_prefix_size_and_rebuild() {
    int fails = 0;
    FragRig rig({halving_sizes()}, {skewed_prims()});
    const double ppr = rig.view.pixels_per_radian();
    const Tile& tile = rig.tree.tiles[0];
    TileScheduler s;
    s.reset(rig.tree, settings({kHuge, kHuge}));
    std::vector<uint32_t> evict;

    // Ögat på 100 m: s_behövs = 3 · 100 / K, och jobbet får hälften. De sju
    // största fragmenten (till 0,15625 m) är minst så stora; det åttonde
    // (0,078125 m) är det största som inte byggs.
    s.update(rig.tree, rig.view, 0);
    const double need = 3 * rig.view.distance(tile.bounds) / ppr;
    std::vector<TileScheduler::Job> jobs = s.next_build_jobs(10, evict);
    CHECK(jobs.size() == 1);
    if (jobs.empty()) return fails + 1;
    CHECK(jobs[0].tile == 0);
    CHECK(std::fabs(jobs[0].min_size - need / 2) <= 1e-5 * need);
    {
        const TilePrefix p = rig.tree.prefix_at_least(0, jobs[0].min_size);
        CHECK(p.fragments == 7 && p.primitives == 70 && p.omitted == 0.078125f);
    }
    CHECK(s.pending() == 1);
    CHECK(s.tile_done(0, jobs[0].min_size, bytes_of_primitives(70), true, 0, evict));
    CHECK(s.built(0));
    CHECK(s.built_min_size(0) == jobs[0].min_size);
    CHECK(s.resident().ram == bytes_of_primitives(70).ram);
    const float first_size = jobs[0].min_size;
    std::vector<uint32_t> show, park;
    s.shown_changes(3, 10, show, park);
    CHECK(show == (std::vector<uint32_t>{0}));
    CHECK(s.holes(1.0) == 0);

    // Ögat på 40 m: s_behövs krymper till 0,0994 men det största fragmentet
    // som inte byggdes, 0,078 m, är fortfarande mindre: inget saknas, så ingen
    // ombyggnad, och formen står kvar.
    s.update(rig.tree, FragRig::eye_at(60), 1);
    CHECK(s.next_build_jobs(10, evict).empty());
    CHECK(s.idle());
    CHECK(s.holes(1.0) == 0);
    CHECK(s.built_min_size(0) == first_size);

    // Åt andra hållet, på 400 m, byggs inte om heller.
    s.update(rig.tree, FragRig::eye_at(-300), 2);
    CHECK(s.next_build_jobs(10, evict).empty());
    CHECK(s.built_min_size(0) == first_size);

    // Ögat på 10 m: s_behövs är 0,0249 och fragmentet på 0,078 m saknas, som
    // ett hål. Den som byggs om får hela biten, eftersom hälften av s_behövs är
    // under bitens minsta fragment.
    s.update(rig.tree, FragRig::eye_at(90), 3);
    CHECK(s.holes(1.0) == 1);
    // Tröskeln × 4 i bild kräver fragment på 0,0994 m, och det saknas inget så stort.
    CHECK(s.holes(4.0) == 0);
    CHECK(!s.idle());
    jobs = s.next_build_jobs(10, evict);
    CHECK(jobs.size() == 1);
    if (jobs.empty()) return fails + 1;
    CHECK(jobs[0].tile == 0 && jobs[0].min_size == 0);
    // Den gamla formen är byggd och visad tills den nya tas emot.
    CHECK(s.built(0) && s.shown(0));
    CHECK(s.built_count() == 1);
    CHECK(s.pending() == 1);
    CHECK(s.built_min_size(0) == first_size);
    CHECK(s.resident().ram == bytes_of_primitives(70).ram);
    CHECK(!s.idle());
    CHECK(s.holes(1.0) == 1);
    CHECK(s.next_build_jobs(10, evict).empty());   // jobbet pågår
    CHECK(evict.empty());
    // Den nya formen ersätter den gamla: bytena byts, och biten är kvar.
    CHECK(s.tile_done(0, 0, bytes_of_primitives(970), true, 4, evict));
    CHECK(s.built(0) && s.shown(0));
    CHECK(s.built_count() == 1);
    CHECK(s.pending() == 0);
    CHECK(s.built_min_size(0) == 0);
    CHECK(s.resident().ram == bytes_of_primitives(970).ram);
    CHECK(s.resident().gpu == bytes_of_primitives(970).gpu);
    CHECK(s.holes(1.0) == 0);
    CHECK(s.idle());
    CHECK(evict.empty());
    // Hela biten byggs inte om, hur nära ögat än kommer.
    s.update(rig.tree, FragRig::eye_at(99), 5);
    CHECK(s.next_build_jobs(10, evict).empty());
    CHECK(s.idle());
    return fails;
}

// En ombyggnad som avbryts, misslyckas eller ogiltigförklaras lämnar bitens
// gamla form i ordning.
int check_rebuild_outcomes() {
    int fails = 0;
    const auto start = [](FragRig& rig, TileScheduler& s, std::vector<uint32_t>& evict) {
        s.reset(rig.tree, settings({kHuge, kHuge}));
        s.update(rig.tree, rig.view, 0);
        const std::vector<TileScheduler::Job> first = s.next_build_jobs(10, evict);
        if (first.size() != 1 || !(first[0].min_size > 0)) return false;
        if (!s.tile_done(0, first[0].min_size, bytes_of_primitives(70), true, 0, evict)) return false;
        // Ögat på 10 m: en ombyggnad behövs.
        s.update(rig.tree, FragRig::eye_at(90), 1);
        return true;
    };

    // Misslyckas: den gamla formen är kvar och ombyggnaden prövas inte igen.
    {
        FragRig rig({halving_sizes()}, {skewed_prims()});
        TileScheduler s;
        std::vector<uint32_t> evict;
        CHECK(start(rig, s, evict));
        const std::vector<TileScheduler::Job> jobs = s.next_build_jobs(10, evict);
        CHECK(jobs.size() == 1);
        CHECK(!s.tile_done(0, 0, PartBytes{}, false, 1, evict));
        CHECK(s.built(0) && s.built_count() == 1 && s.pending() == 0);
        CHECK(s.built_min_size(0) > 0);
        CHECK(s.resident().ram == bytes_of_primitives(70).ram);
        s.update(rig.tree, FragRig::eye_at(90), 2);
        CHECK(s.next_build_jobs(10, evict).empty());
        // Ogiltigförklaras biten kan den byggas om från början.
        CHECK(s.invalidate(0));
        CHECK(!s.built(0));
        s.update(rig.tree, FragRig::eye_at(90), 3);
        const std::vector<TileScheduler::Job> again = s.next_build_jobs(10, evict);
        CHECK(again.size() == 1 && again[0].min_size == 0);
    }
    // Ogiltigförklaras under ombyggnaden: biten är borträknad, och den nya
    // formen som kommer är inte längre aktuell.
    {
        FragRig rig({halving_sizes()}, {skewed_prims()});
        TileScheduler s;
        std::vector<uint32_t> evict;
        CHECK(start(rig, s, evict));
        CHECK(s.next_build_jobs(10, evict).size() == 1);
        CHECK(s.invalidate(0));
        CHECK(!s.built(0) && s.built_count() == 0);
        CHECK(s.resident().ram == 0 && s.resident().gpu == 0);
        CHECK(!s.tile_done(0, 0, bytes_of_primitives(970), true, 1, evict));
        CHECK(!s.built(0) && s.built_count() == 0 && s.pending() == 0);
        CHECK(s.resident().ram == 0 && s.resident().gpu == 0);
    }
    // Avbryts under ombyggnaden (biten hamnar långt från ögat): den gamla formen
    // är kvar.
    {
        FragRig rig({halving_sizes()}, {skewed_prims()});
        TileScheduler s;
        std::vector<uint32_t> evict;
        CHECK(start(rig, s, evict));
        CHECK(s.next_build_jobs(10, evict).size() == 1);
        // Ögat långt bort: biten mäts under N_eff / 4 och jobbet avbryts.
        s.update(rig.tree, FragRig::eye_at(-100000), 2);
        const std::vector<uint32_t> cancelled = s.take_cancellations();
        CHECK(cancelled == (std::vector<uint32_t>{0}));
        CHECK(!s.tile_done(0, 0, bytes_of_primitives(970), true, 3, evict));
        CHECK(s.built(0) && s.built_count() == 1 && s.pending() == 0);
        CHECK(s.built_min_size(0) > 0);
        CHECK(s.resident().ram == bytes_of_primitives(70).ram);
    }
    return fails;
}

// Minnet som en bit behöver räknas för prefixet, och N_eff med det: tre bitar
// ryms inte hela, men deras prefix ryms nästan.
int check_prefix_memory() {
    int fails = 0;
    const std::vector<float> sizes = halving_sizes();
    const std::vector<uint32_t> prims = skewed_prims();
    // En bit på 100 m behöver vid 3 px fragment på 0,124 m och uppåt: sju
    // fragment, 70 primitiver. Hela biten har 970.
    const PartBytes prefix = estimate_of_primitives(70);
    const PartBytes whole = estimate_of_primitives(970);
    CHECK(prefix.ram * 2 < 16000 && prefix.ram * 3 > 16000 && whole.ram > 16000);

    {
        // Två bitar ryms med sina prefix i 16 000 byte, utan att höja tröskeln.
        FragRig rig({sizes, sizes}, {prims, prims});
        TileScheduler s;
        s.reset(rig.tree, settings({16000, kHuge}));
        s.update(rig.tree, rig.view, 0);
        CHECK(s.threshold() == 3);
        std::vector<uint32_t> evict;
        const std::vector<TileScheduler::Job> jobs = s.next_build_jobs(10, evict);
        CHECK(jobs.size() == 2);
        for (const TileScheduler::Job& job : jobs) {
            CHECK(job.min_size > 0);
            CHECK(rig.tree.prefix_at_least(job.tile, job.min_size).primitives == 70);
        }
        CHECK(evict.empty());
        CHECK(s.holes(1.0) == 2);
    }
    {
        // Tre bitar ryms inte med sina prefix: tröskeln höjs. Med högre
        // tröskel krymper prefixen, så den höjs bara så långt att de ryms: de
        // tre fragmenten från 2,5 m och uppåt kräver 30 primitiver vardera,
        // från tröskeln 30,2 px.
        FragRig rig({sizes, sizes, sizes}, {prims, prims, prims});
        TileScheduler s;
        s.reset(rig.tree, settings({16000, kHuge}));
        s.update(rig.tree, rig.view, 0);
        const double threshold = s.threshold();
        CHECK(threshold > 30.0 && threshold < 31.0);
        std::vector<uint32_t> evict;
        const std::vector<TileScheduler::Job> jobs = s.next_build_jobs(10, evict);
        CHECK(jobs.size() == 3);
        size_t ram = 0;
        for (const TileScheduler::Job& job : jobs) {
            const TilePrefix p = rig.tree.prefix_at_least(job.tile, job.min_size);
            CHECK(p.fragments == 3 && p.primitives == 30);
            ram += estimate_of_primitives(p.primitives).ram;
        }
        CHECK(ram <= 16000);
        CHECK(evict.empty());
    }
    {
        // Utan tak för minnet byggs allt som behövs vid 3 px, men bara till
        // storleken som behövs.
        FragRig rig({sizes, sizes, sizes}, {prims, prims, prims});
        TileScheduler s;
        s.reset(rig.tree, settings({kHuge, kHuge}));
        s.update(rig.tree, rig.view, 0);
        CHECK(s.threshold() == 3);
        std::vector<uint32_t> evict;
        CHECK(s.next_build_jobs(10, evict).size() == 3);
    }
    return fails;
}

// En fast bit byggs alltid hel, också långt borta, och med hela bitens minne.
int check_pinned_whole() {
    int fails = 0;
    FragRig rig({halving_sizes(), halving_sizes()}, {skewed_prims(), skewed_prims()}, {1});
    TileScheduler s;
    s.reset(rig.tree, settings({kHuge, kHuge}));
    s.update(rig.tree, FragRig::eye_at(-5000), 0);
    std::vector<uint32_t> evict;
    const std::vector<TileScheduler::Job> jobs = s.next_build_jobs(10, evict);
    CHECK(!jobs.empty());
    for (const TileScheduler::Job& job : jobs)
        if (job.tile == 1) CHECK(job.min_size == 0);
    CHECK(!jobs.empty() && jobs[0].tile == 1);

    // Ögat nära: den fasta biten byggs om aldrig, och bit 0 byggs till en storlek.
    TileScheduler t;
    t.reset(rig.tree, settings({kHuge, kHuge}));
    t.update(rig.tree, rig.view, 0);
    const std::vector<TileScheduler::Job> near_jobs = t.next_build_jobs(10, evict);
    CHECK(near_jobs.size() == 2);
    for (const TileScheduler::Job& job : near_jobs) {
        if (job.tile == 1) CHECK(job.min_size == 0);
        else CHECK(job.min_size > 0);
    }
    // Budgeten räknar den fasta biten hel, 970 primitiver, och lämnar inte plats
    // för bit 0 ens som prefix: tröskeln höjs över bit 0, och bara den fasta byggs.
    TileScheduler u;
    u.reset(rig.tree, settings({estimate_of_primitives(970).ram + 10, kHuge}));
    u.update(rig.tree, rig.view, 0);
    CHECK(u.threshold() > 3);
    const std::vector<TileScheduler::Job> tight = u.next_build_jobs(10, evict);
    CHECK(tight.size() == 1 && tight[0].tile == 1 && tight[0].min_size == 0);
    return fails;
}

// ---------------------------------------------------------------------------
// Den syntetiska filen: garantin och vandringen mot en mätning av varje bit.

struct Fixture {
    Removed file;
    LoadResult loaded;
    LodTree tree;
    LodTreeStats stats;
};

bool build(Fixture& fx) {
    synth::Options o;
    o.fragments = 3000;
    o.records = 700;
    o.nodes = 1500;
    o.seed = 21;
    o.min_size = 0.05;
    o.max_size = 30;
    fx.file.path = testdata::tmp_path("hnwd_test_tile_scheduler.nwd");
    synth::Result written;
    std::string why;
    if (!synth::write(fx.file.path, o, written, why)) {
        std::printf("  write: %s\n", why.c_str());
        return false;
    }
    fx.loaded = load_model(fx.file.path, {});
    if (!fx.loaded.model) {
        std::printf("  load_model: %s\n", fx.loaded.error.c_str());
        return false;
    }
    LodTreeSettings s;
    s.max_leaf_fragments = 16;
    s.max_tile_triangles = 200;
    s.min_tile_triangles = 50;
    s.threads = 2;
    return plan_lod_tree(*fx.loaded.model, s, std::stop_token{}, fx.tree, fx.stats);
}

double dot(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// Ligger lådan helt bakom ögats plan? Största värdet av framåt · (hörn − öga)
// över de åtta hörnen.
bool behind_eye(const ViewFrustum& v, const Box& b) {
    double most = -kInf;
    for (int c = 0; c < 8; ++c) {
        const double d[3] = {((c & 1) ? b.max[0] : b.min[0]) - v.eye[0],
                             ((c & 2) ? b.max[1] : b.min[1]) - v.eye[1],
                             ((c & 4) ? b.max[2] : b.min[2]) - v.eye[2]};
        most = std::max(most, dot(d, v.forward));
    }
    return most <= 0;
}

// Mätningen av en låda med fragment av storleken `size`, så som den är
// definierad: s · K / avstånd, noll utanför bilden och närheten, och halva
// vikten bakom ögat inom närheten.
double expected_pixels(const ViewFrustum& v, const Box& b, double size) {
    const double dist = v.distance(b);
    const double potential = size * v.pixels_per_radian() / std::max(dist, 1e-3);
    if (v.sees(b)) return potential;
    if (!(v.near_radius > 0 && dist < v.near_radius)) return 0;
    return behind_eye(v, b) ? potential * 0.5 : potential;
}

// Ligger lådans mittpunkt innanför bilden? Ett eget prov, utan v.sees().
bool centre_in_view(const ViewFrustum& v, const Box& b) {
    double c[3];
    b.center(c);
    const double d[3] = {c[0] - v.eye[0], c[1] - v.eye[1], c[2] - v.eye[2]};
    const double z = dot(d, v.forward), x = dot(d, v.right), y = dot(d, v.up);
    const double ty = v.tan_half_fov, tx = ty * v.width / v.height;
    return z > 0 && std::fabs(x) <= z * tx * (1 - 1e-9) && std::fabs(y) <= z * ty * (1 - 1e-9);
}

// En kamera nära ett slumpat fragment, med slumpad riktning.
ViewFrustum random_view(Random& rnd, const Model& m, double near_radius) {
    double eye[3] = {0, 0, 0};
    m.world.center(eye);
    const size_t f = static_cast<size_t>(rnd.next() * static_cast<double>(m.fragment_count)) %
                     m.fragment_count;
    Box b;
    if (fragment_box(m, f, b)) b.center(eye);
    eye[0] += (rnd.next() - 0.5) * 60;
    eye[1] += (rnd.next() - 0.5) * 60;
    eye[2] += rnd.next() * 20 - 5;
    ViewFrustum v = ViewFrustum::orbit(eye, rnd.next() * 360, rnd.next() * 60 - 30,
                                       40 + rnd.next() * 40, 1280, 720);
    v.near_radius = near_radius;
    return v;
}

// Storleken som next_jobs() gav för bitens senaste jobb.
float job_size(const TileScheduler& s, uint32_t tile) {
    const auto it = job_sizes().find({&s, tile});
    return it == job_sizes().end() ? 0.0f : it->second;
}

int check_guarantee(const Fixture& fx) {
    int fails = 0;
    const Model& m = *fx.loaded.model;
    const LodTree& tree = fx.tree;
    const size_t n = m.fragment_count;
    std::vector<Box> boxes(n);
    std::vector<uint8_t> has_box(n, 0);
    for (size_t f = 0; f < n; ++f) has_box[f] = static_cast<uint8_t>(fragment_box(m, f, boxes[f]) ? 1 : 0);
    // Fragmentets plats i trädets frags, för dess storlek.
    std::vector<uint32_t> place(n, HNwd::kNone);
    for (size_t i = 0; i < tree.frags.size(); ++i) place[tree.frags[i]] = static_cast<uint32_t>(i);

    // Är fragmentet med i det som är byggt av dess bit? Biten är byggd och
    // fragmentet är minst så stort som biten byggdes till.
    const auto in_built_prefix = [&](const TileScheduler& s, size_t f) {
        const uint32_t tile = tree.tile_of[f];
        if (tile == HNwd::kNone || place[f] == HNwd::kNone || !s.built(tile)) return false;
        return tree.sizes[place[f]] >= s.built_min_size(tile);
    };
    // Garantin mot det byggda prefixet: varje fragment som mäts minst
    // `threshold` px, innanför bilden och med närheten, är med.
    size_t guaranteed = 0;
    const auto check_view = [&](const TileScheduler& s, const ViewFrustum& view, double threshold) {
        int bad = 0;
        const double ppr = view.pixels_per_radian();
        for (size_t f = 0; f < n; ++f) {
            if (!has_box[f]) continue;
            const double size = boxes[f].diagonal();
            if (centre_in_view(view, boxes[f])) {
                const double p = size * ppr / std::max(view.distance(boxes[f]), 1e-3);
                if (p >= threshold * (1 + 1e-6)) {
                    ++guaranteed;
                    if (!in_built_prefix(s, f)) ++bad;
                }
            }
            if (expected_pixels(view, boxes[f], size) >= threshold * (1 + 1e-6)) {
                ++guaranteed;
                if (!in_built_prefix(s, f)) ++bad;
            }
        }
        return bad;
    };

    Random rnd{20260};
    size_t strict_checked = 0, generic_checked = 0, squeezed = 0, partial_jobs = 0, whole_jobs = 0,
           rebuilt_total = 0;
    for (int trial = 0; trial < 40; ++trial) {
        const ViewFrustum view = random_view(rnd, m, trial % 2 ? 30.0 : 0.0);
        const double ppr = view.pixels_per_radian();
        TileScheduler::Settings st = settings({kHuge, kHuge});
        st.max_pixels = 1e9;
        std::vector<uint32_t> evict;

        // Med plats för allt är tröskeln 3 px, och allt som behövs byggs, var
        // och en till den storlek som behövs.
        TileScheduler s;
        s.reset(tree, st);
        s.update(tree, view, 0.0);
        CHECK(s.threshold() == 3);
        const std::vector<uint32_t> jobs = next_jobs(s, SIZE_MAX, evict);
        size_t need_ram = 0;
        for (const uint32_t t : jobs) {
            CHECK(s.pixels(t) >= 3);
            const PartBytes bytes = job_bytes(s, tree, t);
            need_ram += bytes.ram;
            // Storleken är s_behövs / 2, eller 0 för hela biten.
            const float size = job_size(s, t);
            const double need = 3 * static_cast<double>(tree.tiles[t].max_size) / s.pixels(t);
            if (size > 0) {
                ++partial_jobs;
                CHECK(std::fabs(size - need / 2) <= 1e-5 * need);
                CHECK(size > tree.tiles[t].min_size);
            } else {
                ++whole_jobs;
                CHECK(need / 2 <= static_cast<double>(tree.tiles[t].min_size) * (1 + 1e-6));
            }
            CHECK(done(s, t, bytes, true, 0.1, evict));
            CHECK(s.built_min_size(t) == size);
        }
        CHECK(evict.empty());
        CHECK(s.idle());
        CHECK(s.pending() == 0);
        CHECK(s.built_count() == jobs.size());

        for (size_t f = 0; f < n; ++f) {
            if (!has_box[f]) continue;
            const double size = boxes[f].diagonal();
            const uint32_t tile = tree.tile_of[f];
            if (tile == HNwd::kNone) {
                ++fails;
                continue;
            }
            // Innanför bilden: minst 3 px ger en byggd bit som mäts minst lika.
            if (centre_in_view(view, boxes[f])) {
                const double p = size * ppr / std::max(view.distance(boxes[f]), 1e-3);
                if (p >= 3 * (1 + 1e-6)) {
                    ++strict_checked;
                    CHECK(s.built(tile));
                    CHECK(s.pixels(tile) >= p * (1 - 1e-9));
                }
            }
            // Med närheten: allt som mäts minst 3 px, också bakom ögat.
            if (expected_pixels(view, boxes[f], size) >= 3 * (1 + 1e-6)) {
                ++generic_checked;
                CHECK(s.built(tile));
            }
        }
        // Och fragmentet ligger i det byggda prefixet, inte bara i en byggd bit.
        CHECK(check_view(s, view, 3) == 0);

        // Kameran flyttas: först rakt framåt, så att bitarna framför kommer
        // närmare, sedan till ett slumpat ställe. Det som nu behövs byggs, och
        // bitar som byggts till en för stor storlek byggs om; sedan står allt
        // stilla och garantin gäller mot den nya kameran.
        ViewFrustum zoom = view;
        for (int a = 0; a < 3; ++a) zoom.eye[a] += view.forward[a] * 25;
        const ViewFrustum other = random_view(rnd, m, trial % 2 ? 0.0 : 30.0);
        const ViewFrustum* const views[2] = {&zoom, &other};
        for (const ViewFrustum* next : views) {
            s.update(tree, *next, 1.0);
            CHECK(s.threshold() == 3);
            std::vector<uint32_t> evict_next;
            const std::vector<uint32_t> more = next_jobs(s, SIZE_MAX, evict_next);
            CHECK(evict_next.empty());
            for (const uint32_t t : more) {
                if (s.built(t)) ++rebuilt_total;   // en ombyggnad: biten var byggd redan
                CHECK(done(s, t, job_bytes(s, tree, t), true, 1.1, evict_next));
                CHECK(s.built_min_size(t) == job_size(s, t));
            }
            CHECK(evict_next.empty());
            CHECK(next_jobs(s, SIZE_MAX, evict_next).empty());
            CHECK(s.idle());
            CHECK(s.pending() == 0);
            CHECK(s.holes(1.0) == 0);
            CHECK(check_view(s, *next, 3) == 0);
        }

        // Med en snål budget höjs tröskeln tills det som behövs ryms, och
        // garantin gäller då vid den högre tröskeln.
        if (trial % 4 == 0 && jobs.size() >= 8) {
            ++squeezed;
            TileScheduler q;
            q.reset(tree, settings({need_ram / 4 + 1, kHuge}));
            // Tills urvalet står still: uppskattningen före bygget skiljer sig från
            // det byggda, så tröskeln kan flytta sig mellan mätningarna.
            std::vector<uint32_t> evict_q;
            size_t built_q = 0;
            int quiet = 0;
            for (int round = 0; round < 64 && quiet < 2; ++round) {
                q.update(tree, view, 0.1 * round);
                const std::vector<uint32_t> few = next_jobs(q, SIZE_MAX, evict_q);
                quiet = few.empty() ? quiet + 1 : 0;
                for (const uint32_t t : few)
                    if (done(q, t, job_bytes(q, tree, t), true, 0.1 * round + 0.05, evict_q)) ++built_q;
            }
            const double threshold = q.threshold();
            CHECK(threshold > 3);
            CHECK(built_q > 0 && q.built_count() < jobs.size());
            CHECK(q.resident().ram <= q.budget().ram);
            for (uint32_t t = 0; t < tree.tiles.size(); ++t)
                // Står tröskeln på sitt tak ryms inte allt som mäts över den; då
                // gäller garantin inte, och det som inte ryms byggs inte.
                if (threshold < 1e6 && q.pixels(t) >= threshold) CHECK(q.built(t));
            for (size_t f = 0; f < n; ++f) {
                if (!has_box[f] || !centre_in_view(view, boxes[f])) continue;
                const double p =
                    boxes[f].diagonal() * ppr / std::max(view.distance(boxes[f]), 1e-3);
                if (threshold < 1e6 && p >= threshold * (1 + 1e-6)) CHECK(in_built_prefix(q, f));
            }
        }
    }
    CHECK(strict_checked > 0);
    CHECK(generic_checked > 0);
    CHECK(squeezed > 0);
    CHECK(partial_jobs > 0);
    CHECK(whole_jobs > 0);
    CHECK(rebuilt_total > 0);
    CHECK(guaranteed > 0);
    return fails;
}

int check_walk(const Fixture& fx) {
    int fails = 0;
    const Model& m = *fx.loaded.model;
    const LodTree& tree = fx.tree;
    Random rnd{777};
    size_t measured = 0, skipped = 0;
    for (int trial = 0; trial < 30; ++trial) {
        const ViewFrustum view = random_view(rnd, m, trial % 3 == 0 ? 25.0 : 0.0);
        TileScheduler s;
        s.reset(tree, settings({kHuge, kHuge}));
        s.update(tree, view, 0.0);
        // Vandringen ger samma mått som en mätning av varje bit, och bitar som
        // inte nåddes mäts 0.
        for (uint32_t t = 0; t < tree.tiles.size(); ++t) {
            const Tile& tile = tree.tiles[t];
            const double e = expected_pixels(view, tile.bounds, static_cast<double>(tile.max_size));
            const double got = s.pixels(t);
            if (e >= 3 * (1 + 1e-9)) {
                CHECK(std::fabs(got - e) <= 1e-9 * e);
                ++measured;
            } else if (e < 3 * (1 - 1e-9)) {
                CHECK(got == 0);
                ++skipped;
            }
        }
    }
    CHECK(measured > 0);
    CHECK(skipped > 0);
    return fails;
}

// Förutsägelsen mot en mätning av varje bit: det som behövs nu byggs först och
// i samma ordning som utan `ahead`, och efter det byggs just de bitar som
// `ahead` ser minst 3 px och som inte behövs nu.
int check_ahead_walk(const Fixture& fx) {
    int fails = 0;
    const Model& m = *fx.loaded.model;
    const LodTree& tree = fx.tree;
    Random rnd{9090};
    size_t extras = 0;
    for (int trial = 0; trial < 30; ++trial) {
        const ViewFrustum view = random_view(rnd, m, trial % 3 == 0 ? 25.0 : 0.0);
        const ViewFrustum ahead = random_view(rnd, m, 0.0);
        TileScheduler plain, with_ahead, nullary;
        plain.reset(tree, settings({kHuge, kHuge}));
        with_ahead.reset(tree, settings({kHuge, kHuge}));
        nullary.reset(tree, settings({kHuge, kHuge}));
        plain.update(tree, view, 0.0);
        with_ahead.update(tree, view, &ahead, 0.0);
        nullary.update(tree, view, nullptr, 0.0);
        CHECK(with_ahead.threshold() == 3 && plain.threshold() == 3);
        std::vector<uint32_t> e0, e1, e2;
        const std::vector<uint32_t> now_jobs = next_jobs(plain, SIZE_MAX, e0);
        const std::vector<uint32_t> all_jobs = next_jobs(with_ahead, SIZE_MAX, e1);
        CHECK(next_jobs(nullary, SIZE_MAX, e2) == now_jobs);
        CHECK(e0.empty() && e1.empty() && e2.empty());
        CHECK(all_jobs.size() >= now_jobs.size());
        if (all_jobs.size() < now_jobs.size()) continue;
        CHECK(std::equal(now_jobs.begin(), now_jobs.end(), all_jobs.begin()));
        std::vector<uint8_t> in_jobs(tree.tiles.size(), 0);
        for (const uint32_t t : all_jobs) in_jobs[t] = 1;
        for (size_t k = now_jobs.size(); k < all_jobs.size(); ++k) {
            const Tile& tile = tree.tiles[all_jobs[k]];
            CHECK(expected_pixels(ahead, tile.bounds, tile.max_size) >= 3 * (1 - 1e-9));
            CHECK(expected_pixels(view, tile.bounds, tile.max_size) < 3 * (1 + 1e-9));
        }
        extras += all_jobs.size() - now_jobs.size();
        for (uint32_t t = 0; t < tree.tiles.size(); ++t) {
            const Tile& tile = tree.tiles[t];
            if (expected_pixels(ahead, tile.bounds, tile.max_size) >= 3 * (1 + 1e-9))
                CHECK(in_jobs[t]);
        }
        // Förhandsbitarna räknas inte som hål.
        CHECK(with_ahead.holes(1.0) == plain.holes(1.0));
    }
    CHECK(extras > 0);
    return fails;
}

int check_synthetic() {
    int fails = 0;
    Fixture fx;
    if (!build(fx)) return fails + 1;
    CHECK(fx.loaded.model->fragment_count == 3000);
    CHECK(fx.tree.tiles.size() > 10);
    fails += check_guarantee(fx);
    fails += check_walk(fx);
    fails += check_ahead_walk(fx);
    return fails;
}

} // namespace

int test_tile_scheduler() {
    int fails = 0;
    fails += check_frustum();
    fails += check_budgets();
    fails += check_threshold();
    fails += check_pinned();
    fails += check_release_order();
    fails += check_failures();
    fails += check_idle_and_holes();
    fails += check_shown();
    fails += check_latency();
    fails += check_ahead();
    fails += check_cancellations();
    fails += check_shown_order();
    fails += check_prefix_size_and_rebuild();
    fails += check_rebuild_outcomes();
    fails += check_prefix_memory();
    fails += check_pinned_whole();
    fails += check_synthetic();
    std::printf("test_tile_scheduler: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
