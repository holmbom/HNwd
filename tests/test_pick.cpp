// Visarens plockning (viewer/pick.h): strålen mot en bits trianglar, jämförd
// med en rak genomgång i double över världskoordinaterna och mot handbyggd
// geometri med kända svar, och bitarna längs en stråle mot en mätning av varje
// bit. Ren std och HNwd — ingen fönstertyp — så det prövas utan visaren.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stop_token>
#include <string>
#include <vector>

#include "../tools/synth_writer.h"
#include "HNwd.h"
#include "cells.h"
#include "lod_tree.h"
#include "model.h"
#include "pick.h"
#include "test_data.h"
#include "test_support.h"

using namespace nwdview;

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

// Tar bort filen när den går ur räckvidd.
struct Removed {
    std::string path;
    ~Removed() { testdata::remove_utf8(path); }
};

// Slumptal i [-1, 1), ur ett frö.
struct Random {
    uint64_t state;
    double next() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<double>(state >> 11) / 4503599627370496.0 - 1.0;
    }
    // I [0, 1).
    double unit() { return (next() + 1) / 2; }
    size_t below(size_t n) { return static_cast<size_t>(unit() * static_cast<double>(n)) % n; }
};

bool close(double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol * (1 + std::fabs(b)); }

PickRay make_ray(double ox, double oy, double oz, double dx, double dy, double dz) {
    PickRay r;
    r.origin[0] = ox;
    r.origin[1] = oy;
    r.origin[2] = oz;
    r.dir[0] = dx;
    r.dir[1] = dy;
    r.dir[2] = dz;
    return r;
}

using Tri = std::array<float, 9>;

// En del med en triangel per post, med eget fragment var och en.
MeshPart make_part(const std::vector<Tri>& tris, const std::vector<uint32_t>& fragments,
                   bool transparent = false) {
    MeshPart p;
    p.transparent = transparent;
    for (size_t i = 0; i < tris.size(); ++i) {
        for (const float c : tris[i]) p.positions.push_back(c);
        for (int k = 0; k < 9; ++k) p.normals.push_back(0.0f);
        for (int k = 0; k < 3; ++k) p.colors.push_back(0xffffffffu);
        for (uint32_t k = 0; k < 3; ++k) p.indices.push_back(static_cast<uint32_t>(3 * i) + k);
        FragmentRange r;
        r.first_triangle = static_cast<uint32_t>(i);
        r.fragment = fragments[i];
        r.size = 1;
        p.ranges.push_back(r);
    }
    return p;
}

Box make_box(double x0, double y0, double z0, double x1, double y1, double z1) {
    Box b;
    const double lo[3] = {x0, y0, z0}, hi[3] = {x1, y1, z1};
    b.add_point(lo);
    b.add_point(hi);
    return b;
}

// ---------------------------------------------------------------------------
// Linjedelen prövas inte.

int check_lines_skipped() {
    int fails = 0;

    // Del 0: en triangel i z = 0 (fragment 7). Del 1: tom, genomskinlig. Del
    // 2: en linjedel vars trianglar skulle träffas först, i z = 5 (fragment 5).
    // Del 3: en del med bild i z = 2 (fragment 3).
    const Tri ground = {0, 0, 0, 4, 0, 0, 0, 4, 0};
    const Tri line_like = {0, 0, 5, 4, 0, 5, 0, 4, 5};
    const Tri textured = {0, 0, 2, 4, 0, 2, 0, 4, 2};
    CellGeometry g;
    g.parts.push_back(make_part({ground}, {7}));
    g.parts.push_back(MeshPart{});
    g.parts[1].transparent = true;
    g.parts.push_back(make_part({line_like}, {5}));
    g.parts[2].lines = true;
    g.parts.push_back(make_part({textured}, {3}));
    g.parts[3].material = 4;
    const PickRay down = make_ray(1, 1, 10, 0, 0, -1);

    // Strålen går genom linjedelens trianglar, men de räknas inte: närmast är
    // delen med bild (8 m).
    {
        const PickHit h = pick_geometry(g, down, PickLimits{}, 0);
        CHECK(h.hit && close(h.distance, 8) && h.fragment == 3);
        CHECK(close(h.point[2], 2));
    }
    // Delen med bild dold: marken, 10 m, med linjedelens trianglar emellan.
    {
        PickLimits limits;
        limits.drawn = {1, 1, 1, 0};
        const PickHit h = pick_geometry(g, down, limits, 0);
        CHECK(h.hit && close(h.distance, 10) && h.fragment == 7);
    }
    // `drawn` räknas efter delens plats: linjedelen på plats 2 ändrar inget, och
    // delen med bild står på plats 3.
    {
        PickLimits limits;
        limits.drawn = {0, 0, 1000, 1};
        const PickHit h = pick_geometry(g, down, limits, 0);
        CHECK(h.hit && h.fragment == 3);
        limits.drawn = {0, 0, 1000, 0};
        CHECK(!pick_geometry(g, down, limits, 0).hit);
    }
    // Bara en linjedel: ingen träff, hur strålen än går, och ett tak bryr den inte.
    {
        CellGeometry only;
        only.parts.push_back(MeshPart{});
        only.parts.push_back(MeshPart{});
        only.parts[1].transparent = true;
        only.parts.push_back(make_part({line_like}, {5}));
        only.parts[2].lines = true;
        CHECK(!pick_geometry(only, down, PickLimits{}, 0).hit);
        CHECK(!pick_geometry(only, make_ray(1, 1, 0, 0, 0, 1), PickLimits{}, 0).hit);
        CHECK(!pick_geometry(only, down, PickLimits{}, 100).hit);
    }
    return fails;
}

// ---------------------------------------------------------------------------
// Strålen mot handbyggd geometri.

int check_hand_built() {
    int fails = 0;

    // Origo långt från noll. Del 0 har en triangel A i planet z = 300 med
    // fragment 7, del 1 (genomskinlig) en lika stor B i z = 301 med fragment 9.
    CellGeometry g;
    g.origin[0] = 100;
    g.origin[1] = 200;
    g.origin[2] = 300;
    const Tri ta = {0, 0, 0, 4, 0, 0, 0, 4, 0};
    const Tri tb = {0, 0, 1, 4, 0, 1, 0, 4, 1};
    g.parts.push_back(make_part({ta}, {7}));
    g.parts.push_back(make_part({tb}, {9}, true));
    const PickLimits none;

    // Framifrån: B först, på 9 m, med fragment 9 och punkten i världen.
    {
        const PickHit h = pick_geometry(g, make_ray(101, 201, 310, 0, 0, -1), none, 0);
        CHECK(h.hit && close(h.distance, 9) && h.fragment == 9);
        CHECK(close(h.point[0], 101) && close(h.point[1], 201) && close(h.point[2], 301));
    }
    // En riktning som inte är normerad ger samma avstånd i meter.
    {
        const PickHit h = pick_geometry(g, make_ray(101, 201, 310, 0, 0, -2.5), none, 0);
        CHECK(h.hit && close(h.distance, 9) && h.fragment == 9);
        const PickHit slanted = pick_geometry(g, make_ray(101, 201, 310, 0.1, 0, -1), none, 0);
        CHECK(slanted.hit && slanted.fragment == 9);
        CHECK(close(slanted.distance, 9 * std::sqrt(1.01)));
        CHECK(close(slanted.point[0], 101.9) && close(slanted.point[2], 301));
    }
    // Bakifrån: A först (baksidan räknas), på 10 m.
    {
        const PickHit h = pick_geometry(g, make_ray(101, 201, 290, 0, 0, 1), none, 0);
        CHECK(h.hit && close(h.distance, 10) && h.fragment == 7);
        CHECK(close(h.point[2], 300));
    }
    // Kanten, hörnet och strax utanför. Hypotenusan går genom (2, 2).
    {
        CHECK(pick_geometry(g, make_ray(102, 202, 310, 0, 0, -1), none, 0).hit);
        CHECK(pick_geometry(g, make_ray(102, 200, 310, 0, 0, -1), none, 0).hit);
        CHECK(pick_geometry(g, make_ray(100, 200, 310, 0, 0, -1), none, 0).hit);
        CHECK(pick_geometry(g, make_ray(100, 204, 310, 0, 0, -1), none, 0).hit);
        CHECK(!pick_geometry(g, make_ray(102.001, 202.001, 310, 0, 0, -1), none, 0).hit);
        CHECK(!pick_geometry(g, make_ray(99.999, 201, 310, 0, 0, -1), none, 0).hit);
        CHECK(!pick_geometry(g, make_ray(101, 199.999, 310, 0, 0, -1), none, 0).hit);
    }
    // Parallell med planet, i det och ovanför det: ingen träff.
    {
        CHECK(!pick_geometry(g, make_ray(90, 201, 300, 1, 0, 0), none, 0).hit);
        CHECK(!pick_geometry(g, make_ray(90, 201, 300.5, 1, 0, 0), none, 0).hit);
    }
    // Bakom strålens början räknas inte; mitt emellan ser den bara A. På
    // triangeln själv är avståndet 0.
    {
        CHECK(!pick_geometry(g, make_ray(101, 201, 299, 0, 0, -1), none, 0).hit);
        const PickHit mid = pick_geometry(g, make_ray(101, 201, 300.5, 0, 0, -1), none, 0);
        CHECK(mid.hit && close(mid.distance, 0.5) && mid.fragment == 7);
        const PickHit on = pick_geometry(g, make_ray(101, 201, 300, 0, 0, -1), none, 0);
        CHECK(on.hit && on.distance == 0 && on.fragment == 7);
    }
    // max_distance precis före och efter. B ligger på 9 m och A på 10 m.
    {
        const PickRay r = make_ray(101, 201, 310, 0, 0, -1);
        CHECK(pick_geometry(g, r, none, 9.000001).fragment == 9);
        CHECK(!pick_geometry(g, r, none, 8.9999).hit);
        // Med del 1 inte ritad (drawn = {1, 0}) är det A som träffas.
        PickLimits only_a;
        only_a.drawn = {1, 0};
        const PickHit a = pick_geometry(g, r, only_a, 10.000001);
        CHECK(a.hit && a.fragment == 7 && close(a.distance, 10));
        CHECK(!pick_geometry(g, r, only_a, 9.9999).hit);
        // 0, negativt och icke-ändligt är obegränsat.
        CHECK(pick_geometry(g, r, none, 0).fragment == 9);
        CHECK(pick_geometry(g, r, none, -1).fragment == 9);
        CHECK(pick_geometry(g, r, none, kInf).fragment == 9);
        CHECK(pick_geometry(g, r, none, std::nan("")).fragment == 9);
    }
    // Strålar som inte går att följa.
    {
        CHECK(!pick_geometry(g, make_ray(101, 201, 310, 0, 0, 0), none, 0).hit);
        CHECK(!pick_geometry(g, make_ray(101, 201, 310, 0, 0, std::nan("")), none, 0).hit);
        CHECK(!pick_geometry(g, make_ray(101, 201, 310, kInf, 0, -1), none, 0).hit);
        CHECK(!pick_geometry(g, make_ray(std::nan(""), 201, 310, 0, 0, -1), none, 0).hit);
        CHECK(!pick_geometry(g, make_ray(101, kInf, 310, 0, 0, -1), none, 0).hit);
        CHECK(!pick_geometry(CellGeometry{}, make_ray(0, 0, 10, 0, 0, -1), none, 0).hit);
    }
    // Lika avstånd: den lägsta delen vinner, och inom en del den lägsta
    // triangeln.
    {
        CellGeometry t;
        t.parts.push_back(make_part({ta, ta}, {3, 4}));
        const PickHit same_part = pick_geometry(t, make_ray(1, 1, 10, 0, 0, -1), none, 0);
        CHECK(same_part.hit && same_part.fragment == 3);

        CellGeometry u;
        u.parts.push_back(make_part({ta}, {5}));
        u.parts.push_back(make_part({ta}, {6}));
        const PickHit parts = pick_geometry(u, make_ray(1, 1, 10, 0, 0, -1), none, 0);
        CHECK(parts.hit && parts.fragment == 5);
        // Ordningen i geometrin avgör, inte fragmentets nummer.
        CellGeometry v;
        v.parts.push_back(make_part({ta}, {6}));
        v.parts.push_back(make_part({ta}, {5}));
        CHECK(pick_geometry(v, make_ray(1, 1, 10, 0, 0, -1), none, 0).fragment == 6);
    }
    // Ritad andel: de första trianglarna i del 0 är den bortre (fragment 1) och
    // den närmare (fragment 2); del 1 har en ännu bortre (fragment 3).
    {
        const Tri far_tri = {0, 0, 0, 4, 0, 0, 0, 4, 0};
        const Tri near_tri = {0, 0, 1, 4, 0, 1, 0, 4, 1};
        const Tri lowest = {0, 0, -1, 4, 0, -1, 0, 4, -1};
        CellGeometry d;
        d.parts.push_back(make_part({far_tri, near_tri}, {1, 2}));
        d.parts.push_back(make_part({lowest}, {3}));
        const PickRay r = make_ray(1, 1, 10, 0, 0, -1);

        CHECK(pick_geometry(d, r, none, 0).fragment == 2);
        PickLimits lim;
        lim.drawn = {2};
        CHECK(pick_geometry(d, r, lim, 0).fragment == 2);
        lim.drawn = {1};
        {
            const PickHit h = pick_geometry(d, r, lim, 0);
            CHECK(h.hit && h.fragment == 1 && close(h.distance, 10));
        }
        lim.drawn = {5};   // över delens antal: hela delen
        CHECK(pick_geometry(d, r, lim, 0).fragment == 2);
        lim.drawn = {0};   // inget av del 0; del 1 saknar post och prövas hel
        {
            const PickHit h = pick_geometry(d, r, lim, 0);
            CHECK(h.hit && h.fragment == 3 && close(h.distance, 11));
        }
        lim.drawn = {0, 0};
        CHECK(!pick_geometry(d, r, lim, 0).hit);
        lim.drawn = {1, 1};
        CHECK(pick_geometry(d, r, lim, 0).fragment == 1);
    }
    // Snittplan: träffen bakom planet räknas inte, nästa räknas.
    {
        const PickRay r = make_ray(101, 201, 310, 0, 0, -1);
        const double at[3] = {0, 0, 300.5};
        const double keep_below[3] = {0, 0, -1};
        const double keep_above[3] = {0, 0, 1};

        ClipState below;
        CHECK(add_clip_plane(below, at, keep_below));
        PickLimits lim;
        lim.clip = &below;
        const PickHit a = pick_geometry(g, r, lim, 0);   // B (301) bortklippt
        CHECK(a.hit && a.fragment == 7 && close(a.distance, 10));

        ClipState above;
        CHECK(add_clip_plane(above, at, keep_above));
        lim.clip = &above;
        CHECK(pick_geometry(g, r, lim, 0).fragment == 9);   // A står kvar bakom B

        // Båda träffarna bortklippta: ingen träff. Ett tomt snitt klipper inget.
        const double low[3] = {0, 0, 299};
        ClipState gone;
        CHECK(add_clip_plane(gone, low, keep_below));
        lim.clip = &gone;
        CHECK(!pick_geometry(g, r, lim, 0).hit);
        const ClipState empty;
        lim.clip = &empty;
        CHECK(pick_geometry(g, r, lim, 0).fragment == 9);

        // Två plan: skivan 299,5..300,5 innehåller A men inte B, uppifrån och
        // underifrån.
        ClipState slab;
        const double lo[3] = {0, 0, 299.5};
        CHECK(add_clip_plane(slab, lo, keep_above));
        CHECK(add_clip_plane(slab, at, keep_below));
        lim.clip = &slab;
        const PickHit in_slab = pick_geometry(g, r, lim, 0);
        CHECK(in_slab.hit && in_slab.fragment == 7);
        const PickHit up = pick_geometry(g, make_ray(101, 201, 290, 0, 0, 1), lim, 0);
        CHECK(up.hit && up.fragment == 7);
        // Skivan 300,2..300,8 innehåller ingen av dem.
        ClipState empty_slab;
        const double lo2[3] = {0, 0, 300.2}, hi2[3] = {0, 0, 300.8};
        CHECK(add_clip_plane(empty_slab, lo2, keep_above));
        CHECK(add_clip_plane(empty_slab, hi2, keep_below));
        lim.clip = &empty_slab;
        CHECK(!pick_geometry(g, r, lim, 0).hit);
        CHECK(!pick_geometry(g, make_ray(101, 201, 290, 0, 0, 1), lim, 0).hit);
    }
    // Ett index utanför delens hörn hoppas över; resten prövas.
    {
        CellGeometry bad;
        MeshPart p = make_part({ta}, {1});
        p.indices.push_back(0);
        p.indices.push_back(1);
        p.indices.push_back(99);
        FragmentRange r;
        r.first_triangle = 1;
        r.fragment = 2;
        p.ranges.push_back(r);
        bad.parts.push_back(p);
        const PickHit h = pick_geometry(bad, make_ray(1, 1, 10, 0, 0, -1), none, 0);
        CHECK(h.hit && h.fragment == 1);
    }
    // Stora världskoordinater: delens origo är en miljon meter bort, och
    // avståndet och punkten är ändå exakta.
    {
        CellGeometry far_away;
        far_away.origin[0] = 1e6;
        far_away.origin[1] = 2e6;
        far_away.origin[2] = 3e6;
        far_away.parts.push_back(make_part({ta}, {11}));
        const PickHit h =
            pick_geometry(far_away, make_ray(1e6 + 1.25, 2e6 + 1.5, 3e6 + 10, 0, 0, -1), none, 0);
        CHECK(h.hit && h.fragment == 11);
        CHECK(close(h.distance, 10, 1e-12));
        CHECK(h.point[0] == 1e6 + 1.25 && h.point[1] == 2e6 + 1.5 && h.point[2] == 3e6);
    }
    // Marksökningen: strålen rakt ned från ögat. Golvet är två trianglar 1,7 m
    // under ögat; ett tak 1,3 m över ögat ligger bakom strålens början.
    {
        const float s = 50;
        const Tri f0 = {-s, -s, 0, s, -s, 0, s, s, 0};
        const Tri f1 = {-s, -s, 0, s, s, 0, -s, s, 0};
        const Tri roof = {-s, -s, 3, s, -s, 3, s, s, 3};
        CellGeometry room;
        room.origin[0] = 10;
        room.origin[1] = 20;
        room.origin[2] = 5;
        room.parts.push_back(make_part({f0, f1, roof}, {20, 21, 22}));
        // Ögat 1,7 m över golvet (rel z = 1,7), taket på rel z = 3.
        const PickHit down = pick_geometry(room, make_ray(10 + 3, 20 - 4, 5 + 1.7, 0, 0, -1), none, 0);
        CHECK(down.hit && close(down.distance, 1.7) && close(down.point[2], 5));
        CHECK(down.fragment == 20);
        CHECK(!pick_geometry(room, make_ray(13, 16, 5 + 1.7, 0, 0, -1), none, 1.0).hit);
    }
    return fails;
}

// ---------------------------------------------------------------------------
// Bitarna längs en stråle, handbyggda.

int check_tiles_hand_built() {
    int fails = 0;
    LodTree tree;
    tree.tiles.resize(8);
    tree.tiles[0].bounds = make_box(0, 0, 0, 1, 1, 1);
    tree.tiles[1].bounds = make_box(2, 0, 0, 3, 1, 1);
    tree.tiles[2].bounds = make_box(4, 0, 0, 5, 1, 1);
    tree.tiles[3].bounds = make_box(0, 0, 3, 1, 1, 4);   // ovanför bit 0
    tree.tiles[4].bounds = make_box(0, 0, 0, 1, 1, 1);   // samma låda som bit 0
    // bit 5 har en ogiltig låda
    tree.tiles[6].bounds = make_box(-10, -10, -10, 10, 10, 10);   // rymmer ögat
    tree.tiles[7].bounds = make_box(0, 2, 0, 1, 3, 1);

    std::vector<TileAlongRay> out;
    auto tiles_of = [&out]() {
        std::vector<uint32_t> ids;
        for (const TileAlongRay& t : out) ids.push_back(t.tile);
        return ids;
    };
    using Ids = std::vector<uint32_t>;

    // Längs x: bitarna 0, 1, 2 i den ordning strålen når dem, hur de än anges.
    {
        const PickRay r = make_ray(-1, 0.5, 0.5, 1, 0, 0);
        out.assign(5, TileAlongRay{99, 99});   // tömmas först
        tiles_along_ray(tree, {2, 0, 1}, r, 0, out);
        CHECK(tiles_of() == (Ids{0, 1, 2}));
        CHECK(out.size() == 3 && close(out[0].enter, 1) && close(out[1].enter, 3) &&
              close(out[2].enter, 5));
        // En riktning som inte är normerad ger samma avstånd i meter.
        tiles_along_ray(tree, {2, 0, 1}, make_ray(-1, 0.5, 0.5, 4, 0, 0), 0, out);
        CHECK(tiles_of() == (Ids{0, 1, 2}));
        CHECK(out.size() == 3 && close(out[0].enter, 1) && close(out[2].enter, 5));
        // Åt andra hållet.
        tiles_along_ray(tree, {0, 1, 2}, make_ray(6, 0.5, 0.5, -1, 0, 0), 0, out);
        CHECK(tiles_of() == (Ids{2, 1, 0}));
        CHECK(close(out[0].enter, 1) && close(out[2].enter, 5));
    }
    // max_distance: inklusive, och 0, negativt och icke-ändligt är obegränsat.
    {
        const PickRay r = make_ray(-1, 0.5, 0.5, 1, 0, 0);
        tiles_along_ray(tree, {0, 1, 2}, r, 3, out);
        CHECK(tiles_of() == (Ids{0, 1}));
        tiles_along_ray(tree, {0, 1, 2}, r, 2.999, out);
        CHECK(tiles_of() == (Ids{0}));
        tiles_along_ray(tree, {0, 1, 2}, r, 0.999, out);
        CHECK(out.empty());
        tiles_along_ray(tree, {0, 1, 2}, r, -5, out);
        CHECK(out.size() == 3);
        tiles_along_ray(tree, {0, 1, 2}, r, kInf, out);
        CHECK(out.size() == 3);
        tiles_along_ray(tree, {0, 1, 2}, r, std::nan(""), out);
        CHECK(out.size() == 3);
    }
    // Strålen börjar inuti en låda: avståndet 0. Det som ligger bakom räknas inte.
    {
        tiles_along_ray(tree, {0, 1, 2}, make_ray(2.5, 0.5, 0.5, 1, 0, 0), 0, out);
        CHECK(tiles_of() == (Ids{1, 2}));
        CHECK(out[0].enter == 0 && close(out[1].enter, 1.5));
        // Ögat i en stor låda som rymmer allt: den kommer först med 0.
        tiles_along_ray(tree, {0, 6, 1}, make_ray(-1, 0.5, 0.5, 1, 0, 0), 0, out);
        CHECK(tiles_of() == (Ids{6, 0, 1}));
        CHECK(out[0].enter == 0);
    }
    // Lika avstånd: bitens nummer avgör, hur de än anges.
    {
        tiles_along_ray(tree, {4, 0}, make_ray(-1, 0.5, 0.5, 1, 0, 0), 0, out);
        CHECK(tiles_of() == (Ids{0, 4}));
    }
    // Axelparallell stråle utanför alla lådor, och i ett sidoplan (lådan är
    // sluten): y = 0 är bitarnas undersida, y = 1 deras översida.
    {
        tiles_along_ray(tree, {0, 1, 2}, make_ray(-1, 2, 0.5, 1, 0, 0), 0, out);
        CHECK(out.empty());
        tiles_along_ray(tree, {0, 1, 2}, make_ray(-1, 0, 0.5, 1, 0, 0), 0, out);
        CHECK(tiles_of() == (Ids{0, 1, 2}));
        tiles_along_ray(tree, {0, 1, 2}, make_ray(-1, 1, 0.5, 1, 0, 0), 0, out);
        CHECK(tiles_of() == (Ids{0, 1, 2}));
        // Längs en kant.
        tiles_along_ray(tree, {0, 1, 2}, make_ray(-1, 0, 0, 1, 0, 0), 0, out);
        CHECK(tiles_of() == (Ids{0, 1, 2}));
        // Strax utanför sidoplanet.
        tiles_along_ray(tree, {0, 1, 2}, make_ray(-1, -1e-6, 0.5, 1, 0, 0), 0, out);
        CHECK(out.empty());
        tiles_along_ray(tree, {0, 1, 2}, make_ray(-1, 1.000001, 0.5, 1, 0, 0), 0, out);
        CHECK(out.empty());
        // En stråle i x-led som börjar efter lådorna.
        tiles_along_ray(tree, {0, 1, 2}, make_ray(6, 0.5, 0.5, 1, 0, 0), 0, out);
        CHECK(out.empty());
    }
    // Rakt nedåt (marksökningen): bit 3 ligger över bit 0 med z 3..4 och 0..1.
    {
        tiles_along_ray(tree, {0, 3, 7}, make_ray(0.5, 0.5, 10, 0, 0, -1), 0, out);
        CHECK(tiles_of() == (Ids{3, 0}));
        CHECK(close(out[0].enter, 6) && close(out[1].enter, 9));
        tiles_along_ray(tree, {0, 3, 7}, make_ray(0.5, 0.5, 10, 0, 0, -1), 6, out);
        CHECK(tiles_of() == (Ids{3}));
        // Rakt uppåt träffar ingenting ovanför ögat.
        tiles_along_ray(tree, {0, 3, 7}, make_ray(0.5, 0.5, 10, 0, 0, 1), 0, out);
        CHECK(out.empty());
    }
    // Snett: en stråle genom bit 0 och bit 7 diagonalt i xy, och en som missar.
    {
        // Från (0; 3,5; 0,5) med riktningen (0,2; -1; 0): genom bit 7 (y 3..2) och sedan
        // bit 0 (y 1..0).
        tiles_along_ray(tree, {0, 7}, make_ray(0, 3.5, 0.5, 0.2, -1, 0), 0, out);
        CHECK(tiles_of() == (Ids{7, 0}));
        CHECK(close(out[0].enter, 0.5 * std::sqrt(1.04)) && close(out[1].enter, 2.5 * std::sqrt(1.04)));
        // Förbi lådorna.
        tiles_along_ray(tree, {0, 7}, make_ray(-1, 5, 0.5, 1, -0.5, 0), 0, out);
        CHECK(out.empty());
    }
    // Ogiltiga index och lådor hoppas över; en bit som anges två gånger ger
    // två poster.
    {
        tiles_along_ray(tree, {5, 8, 100, 0xffffffffu, 1}, make_ray(-1, 0.5, 0.5, 1, 0, 0), 0, out);
        CHECK(tiles_of() == (Ids{1}));
        tiles_along_ray(tree, {1, 1}, make_ray(-1, 0.5, 0.5, 1, 0, 0), 0, out);
        CHECK(tiles_of() == (Ids{1, 1}));
        tiles_along_ray(tree, {}, make_ray(-1, 0.5, 0.5, 1, 0, 0), 0, out);
        CHECK(out.empty());
        tiles_along_ray(LodTree{}, {0}, make_ray(-1, 0.5, 0.5, 1, 0, 0), 0, out);
        CHECK(out.empty());
    }
    // Strålar som inte går att följa, och en riktning så nära en axel att
    // delningen svämmar över.
    {
        tiles_along_ray(tree, {0, 1, 2}, make_ray(-1, 0.5, 0.5, 0, 0, 0), 0, out);
        CHECK(out.empty());
        tiles_along_ray(tree, {0, 1, 2}, make_ray(-1, 0.5, std::nan(""), 1, 0, 0), 0, out);
        CHECK(out.empty());
        tiles_along_ray(tree, {0, 1, 2}, make_ray(-1, 0.5, 0.5, 1, kInf, 0), 0, out);
        CHECK(out.empty());
        // y-ledet är 1e-320: strålen når aldrig y-intervallet 0..1 från y = -5.
        tiles_along_ray(tree, {0, 1, 2}, make_ray(-1, -5, 0.5, 1, 1e-320, 0), 0, out);
        CHECK(out.empty());
        tiles_along_ray(tree, {0, 1, 2}, make_ray(-1, 5, 0.5, 1, 1e-320, 0), 0, out);
        CHECK(out.empty());
        // Men inuti y-intervallet går den att följa.
        tiles_along_ray(tree, {0, 1, 2}, make_ray(-1, 0.5, 0.5, 1, 1e-320, 0), 0, out);
        CHECK(tiles_of() == (Ids{0, 1, 2}));
    }
    return fails;
}

// ---------------------------------------------------------------------------
// Mot en rak genomgång, med geometri ur en syntetisk fil.

struct Fixture {
    Removed file;
    LoadResult loaded;
};

bool make_fixture(Fixture& fx, const char* name) {
    synth::Options o;
    o.fragments = 300;
    o.records = 60;
    o.nodes = 200;
    o.seed = 5;
    o.min_size = 0.05;
    o.max_size = 5;
    fx.file.path = testdata::tmp_path(name);
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
    return fx.loaded.model->fragment_count == o.fragments;
}

// Närmaste träff mot varje triangel i världskoordinater i double, med en
// annan metod än pick_geometry: strålens skärning med triangelns plan och ett
// sidotest per kant.
struct Reference {
    bool hit = false;
    double distance = 0;
    size_t part = 0;
    uint32_t triangle = 0;
    double runner_up = kInf;   // närmaste träff på en annan triangel
};

Reference reference_pick(const CellGeometry& g, const PickRay& ray, const std::vector<uint32_t>& drawn,
                         const ClipState* clip, double max_distance) {
    Reference best;
    const double len = std::sqrt(ray.dir[0] * ray.dir[0] + ray.dir[1] * ray.dir[1] +
                                 ray.dir[2] * ray.dir[2]);
    const double limit = (max_distance > 0 && std::isfinite(max_distance)) ? max_distance : kInf;
    double best_distance = kInf;
    for (size_t pi = 0; pi < g.parts.size(); ++pi) {
        const MeshPart& part = g.parts[pi];
        if (part.lines) continue;   // linjedelen prövas inte
        size_t count = part.triangle_count();
        if (pi < drawn.size()) count = std::min<size_t>(count, drawn[pi]);
        for (size_t t = 0; t < count; ++t) {
            double v[3][3];
            for (int k = 0; k < 3; ++k) {
                const size_t vi = part.indices[3 * t + static_cast<size_t>(k)];
                for (int a = 0; a < 3; ++a)
                    v[k][a] = g.origin[a] + static_cast<double>(part.positions[3 * vi + static_cast<size_t>(a)]);
            }
            auto sub = [](const double* a, const double* b, double* r) {
                for (int i = 0; i < 3; ++i) r[i] = a[i] - b[i];
            };
            auto cross = [](const double* a, const double* b, double* r) {
                r[0] = a[1] * b[2] - a[2] * b[1];
                r[1] = a[2] * b[0] - a[0] * b[2];
                r[2] = a[0] * b[1] - a[1] * b[0];
            };
            auto dot = [](const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
            double e1[3], e2[3], n[3];
            sub(v[1], v[0], e1);
            sub(v[2], v[0], e2);
            cross(e1, e2, n);
            const double n2 = dot(n, n);
            if (n2 == 0) continue;
            const double denom = dot(n, ray.dir);
            if (std::fabs(denom) <= 1e-14 * std::sqrt(n2) * len) continue;
            double to_plane[3];
            sub(v[0], ray.origin, to_plane);
            const double s = dot(n, to_plane) / denom;   // i riktningens längd
            if (s < 0) continue;
            const double p[3] = {ray.origin[0] + ray.dir[0] * s, ray.origin[1] + ray.dir[1] * s,
                                 ray.origin[2] + ray.dir[2] * s};
            bool inside = true;
            for (int k = 0; k < 3 && inside; ++k) {
                double edge[3], rel[3], c[3];
                sub(v[(k + 1) % 3], v[k], edge);
                sub(p, v[k], rel);
                cross(edge, rel, c);
                if (dot(c, n) < -1e-9 * n2) inside = false;
            }
            if (!inside) continue;
            const double distance = s * len;
            if (distance > limit) continue;
            if (clip && clipped_away(*clip, p)) continue;
            if (!best.hit || distance < best_distance) {
                if (best.hit) best.runner_up = std::min(best.runner_up, best_distance);
                best.hit = true;
                best_distance = distance;
                best.distance = distance;
                best.part = pi;
                best.triangle = static_cast<uint32_t>(t);
            } else {
                best.runner_up = std::min(best.runner_up, distance);
            }
        }
    }
    return best;
}

// Jämför pick_geometry med genomgången.
int compare_pick(const CellGeometry& g, const PickRay& ray, const PickLimits& limits,
                 double max_distance, size_t& hits) {
    int fails = 0;
    const Reference ref = reference_pick(g, ray, limits.drawn, limits.clip, max_distance);
    const PickHit got = pick_geometry(g, ray, limits, max_distance);
    CHECK(got.hit == ref.hit);
    if (got.hit != ref.hit || !ref.hit) return fails;
    ++hits;
    CHECK(close(got.distance, ref.distance, 1e-8));
    const double len = std::sqrt(ray.dir[0] * ray.dir[0] + ray.dir[1] * ray.dir[1] +
                                 ray.dir[2] * ray.dir[2]);
    for (int a = 0; a < 3; ++a)
        CHECK(std::fabs(got.point[a] - (ray.origin[a] + ray.dir[a] / len * ref.distance)) <= 1e-7);
    // Fragmentet är bestämt när ingen annan triangel ligger lika nära.
    if (ref.runner_up - ref.distance > 1e-6)
        CHECK(got.fragment == fragment_of(g.parts[ref.part].ranges, ref.triangle));
    return fails;
}

struct World {
    LodTree tree;
    std::vector<CellGeometry> geoms;   // per bit
};

bool build_world(const Model& model, World& w) {
    LodTreeSettings settings;
    settings.max_tile_triangles = 150;
    settings.min_tile_triangles = 0;
    settings.max_leaf_fragments = 16;
    settings.threads = 2;
    LodTreeStats stats;
    if (!plan_lod_tree(model, settings, std::stop_token{}, w.tree, stats)) return false;
    HNwd::GeometryReader reader;
    if (reader.open(*model.doc) != HNwd::Status::Ok) return false;
    const BuildLimits limits{};
    w.geoms.resize(w.tree.tiles.size());
    for (uint32_t t = 0; t < w.tree.tiles.size(); ++t) {
        std::string error;
        if (build_tile(reader, model, w.tree, t, limits, std::stop_token{}, w.geoms[t], error) !=
            HNwd::Status::Ok) {
            std::printf("  tile %u: %s\n", t, error.c_str());
            return false;
        }
    }
    return true;
}

// Världspunkten för en slumpad triangel i geometrin; false utan trianglar.
bool random_triangle_centre(Random& rnd, const CellGeometry& g, double out[3]) {
    std::vector<size_t> parts;
    for (size_t i = 0; i < g.parts.size(); ++i)
        if (g.parts[i].triangle_count() > 0 && !g.parts[i].lines) parts.push_back(i);
    if (parts.empty()) return false;
    const MeshPart& p = g.parts[parts[rnd.below(parts.size())]];
    const size_t t = rnd.below(p.triangle_count());
    for (int a = 0; a < 3; ++a) {
        double sum = 0;
        for (size_t k = 0; k < 3; ++k)
            sum += static_cast<double>(p.positions[3 * p.indices[3 * t + k] + static_cast<size_t>(a)]);
        out[a] = g.origin[a] + sum / 3;
    }
    return true;
}

// En stråle mot en slumpad triangel, från ett slumpat håll och avstånd, med
// riktningen skalad till en längd som inte är 1.
PickRay aimed_ray(Random& rnd, const double centre[3]) {
    double off[3] = {rnd.next(), rnd.next(), rnd.next()};
    double len = std::sqrt(off[0] * off[0] + off[1] * off[1] + off[2] * off[2]);
    if (len < 1e-3) {
        off[0] = 1;
        len = 1;
    }
    const double dist = 0.3 + 10 * rnd.unit();
    PickRay r;
    const double scale = 0.25 + 3 * rnd.unit();
    for (int a = 0; a < 3; ++a) {
        r.origin[a] = centre[a] + off[a] / len * dist;
        r.dir[a] = -off[a] / len * dist * scale;
    }
    return r;
}

int check_against_reference(const World& w) {
    int fails = 0;
    Random rnd{777};
    size_t hits = 0, rays = 0, limited = 0, clipped = 0, with_drawn = 0;

    for (size_t ti = 0; ti < w.geoms.size(); ++ti) {
        const CellGeometry& g = w.geoms[ti];
        const Box& b = w.tree.tiles[ti].bounds;
        double centre[3];
        b.center(centre);
        for (int k = 0; k < 120; ++k) {
            PickRay ray;
            double target[3];
            if (k % 4 != 3 && random_triangle_centre(rnd, g, target)) {
                ray = aimed_ray(rnd, target);
            } else {
                // Fri stråle från en punkt kring biten, åt ett slumpat håll.
                const double r = 1 + b.diagonal();
                for (int a = 0; a < 3; ++a) {
                    ray.origin[a] = centre[a] + r * rnd.next();
                    ray.dir[a] = rnd.next();
                }
            }
            PickLimits limits;
            ClipState clip;
            if (rnd.below(2) == 0) {
                for (const MeshPart& p : g.parts)
                    limits.drawn.push_back(static_cast<uint32_t>(
                        static_cast<double>(p.triangle_count()) * rnd.unit() * 1.2));
                ++with_drawn;
            }
            if (rnd.below(3) == 0) {
                double normal[3] = {rnd.next(), rnd.next(), rnd.next()};
                double at[3] = {centre[0] + 0.2 * b.diagonal() * rnd.next(),
                                centre[1] + 0.2 * b.diagonal() * rnd.next(),
                                centre[2] + 0.2 * b.diagonal() * rnd.next()};
                if (add_clip_plane(clip, at, normal)) {
                    limits.clip = &clip;
                    ++clipped;
                }
            }
            double max_distance = 0;
            switch (rnd.below(4)) {
            case 1: max_distance = 2; ++limited; break;
            case 2: max_distance = 15; ++limited; break;
            default: break;
            }
            ++rays;
            fails += compare_pick(g, ray, limits, max_distance, hits);
        }
        // Rakt nedåt över biten, som marksökningen.
        for (int k = 0; k < 25; ++k) {
            PickRay down = make_ray(b.min[0] + (b.max[0] - b.min[0]) * rnd.unit(),
                                    b.min[1] + (b.max[1] - b.min[1]) * rnd.unit(), b.max[2] + 5, 0,
                                    0, -1);
            ++rays;
            fails += compare_pick(g, down, PickLimits{}, 0, hits);
        }
    }
    // Provet var ett prov: många strålar träffade, med och utan gränser.
    CHECK(rays > 500);
    CHECK(hits > 100);
    CHECK(limited > 50 && clipped > 50 && with_drawn > 50);
    return fails;
}

// En låda mot strålen, mätt på ett annat sätt än plattprovet: strålens
// skärning med lådans sex sidor.
bool measured_enter(const Box& b, const PickRay& ray, double& enter) {
    const double len = std::sqrt(ray.dir[0] * ray.dir[0] + ray.dir[1] * ray.dir[1] +
                                 ray.dir[2] * ray.dir[2]);
    double d[3], o[3];
    for (int a = 0; a < 3; ++a) {
        d[a] = ray.dir[a] / len;
        o[a] = ray.origin[a];
    }
    bool inside = true;
    for (int a = 0; a < 3; ++a)
        if (o[a] < b.min[a] || o[a] > b.max[a]) inside = false;
    if (inside) {
        enter = 0;
        return true;
    }
    double best = kInf;
    for (int a = 0; a < 3; ++a) {
        if (d[a] == 0) continue;
        for (const double plane : {b.min[a], b.max[a]}) {
            const double t = (plane - o[a]) / d[a];
            if (t < 0) continue;
            bool within = true;
            for (int c = 0; c < 3; ++c) {
                if (c == a) continue;
                const double x = o[c] + d[c] * t;
                if (x < b.min[c] - 1e-9 || x > b.max[c] + 1e-9) within = false;
            }
            if (within) best = std::min(best, t);
        }
    }
    if (best == kInf) return false;
    enter = best;
    return true;
}

int check_tiles_against_measurement(const World& w) {
    int fails = 0;
    Random rnd{4242};
    const size_t n = w.tree.tiles.size();
    std::vector<uint32_t> all, odd;
    for (uint32_t t = 0; t < n; ++t) {
        all.push_back(t);
        if (t % 2) odd.push_back(t);
    }
    all.push_back(static_cast<uint32_t>(n));   // ogiltigt index
    odd.push_back(static_cast<uint32_t>(n + 7));

    Box world;
    for (const Tile& t : w.tree.tiles) world.add(t.bounds);
    double centre[3];
    world.center(centre);
    const double radius = world.diagonal();

    size_t rays_with_tiles = 0, multi = 0;
    std::vector<TileAlongRay> out;
    for (int k = 0; k < 400; ++k) {
        PickRay ray;
        for (int a = 0; a < 3; ++a) {
            ray.origin[a] = centre[a] + 0.7 * radius * rnd.next();
            ray.dir[a] = rnd.next();
        }
        if (k % 4 == 1) {
            // Mot en slumpad triangel: den passerar i alla fall dess bit.
            double target[3];
            if (random_triangle_centre(rnd, w.geoms[rnd.below(w.geoms.size())], target))
                ray = aimed_ray(rnd, target);
        }
        if (k % 5 == 0) {
            // Rakt ned, som marksökningen.
            ray.dir[0] = ray.dir[1] = 0;
            ray.dir[2] = -1;
        }
        const double max_distance = (k % 3 == 0) ? 0 : 0.3 * radius * rnd.unit();
        const std::vector<uint32_t>& given = (k % 3 == 0) ? odd : all;

        std::vector<TileAlongRay> expected;
        for (const uint32_t t : given) {
            if (t >= n || !w.tree.tiles[t].bounds.valid()) continue;
            double enter;
            if (!measured_enter(w.tree.tiles[t].bounds, ray, enter)) continue;
            if (max_distance > 0 && enter > max_distance) continue;
            expected.push_back({t, enter});
        }
        std::sort(expected.begin(), expected.end(), [](const TileAlongRay& a, const TileAlongRay& b) {
            return a.enter != b.enter ? a.enter < b.enter : a.tile < b.tile;
        });

        tiles_along_ray(w.tree, given, ray, max_distance, out);
        CHECK(out.size() == expected.size());
        if (out.size() != expected.size()) continue;
        if (!out.empty()) ++rays_with_tiles;
        if (out.size() > 1) ++multi;
        std::vector<uint32_t> got_ids, expected_ids;
        for (size_t i = 0; i < out.size(); ++i) {
            CHECK(close(out[i].enter, expected[i].enter, 1e-8));
            if (i > 0) CHECK(out[i - 1].enter <= out[i].enter);
            got_ids.push_back(out[i].tile);
            expected_ids.push_back(expected[i].tile);
        }
        std::sort(got_ids.begin(), got_ids.end());
        std::sort(expected_ids.begin(), expected_ids.end());
        CHECK(got_ids == expected_ids);
    }
    CHECK(rays_with_tiles > 0);
    CHECK(multi > 0);
    return fails;
}

// Hela vägen som ett klick går den: bitarna längs strålen prövas i ordning
// tills en träff ligger närmare än nästa låda. Svaret är detsamma som det
// närmaste av träffarna i alla bitar.
int check_walk_in_order(const World& w) {
    int fails = 0;
    Random rnd{99};
    std::vector<uint32_t> all;
    for (uint32_t t = 0; t < w.tree.tiles.size(); ++t) all.push_back(t);

    size_t hits = 0, skipped_tiles = 0;
    std::vector<TileAlongRay> order;
    for (int k = 0; k < 400; ++k) {
        const CellGeometry& any = w.geoms[rnd.below(w.geoms.size())];
        double target[3];
        if (!random_triangle_centre(rnd, any, target)) continue;
        const PickRay ray = aimed_ray(rnd, target);

        PickHit brute;
        for (const CellGeometry& g : w.geoms) {
            const PickHit h = pick_geometry(g, ray, PickLimits{}, 0);
            if (h.hit && (!brute.hit || h.distance < brute.distance)) brute = h;
        }

        tiles_along_ray(w.tree, all, ray, 0, order);
        PickHit walked;
        size_t used = 0;
        for (const TileAlongRay& e : order) {
            if (walked.hit && walked.distance <= e.enter) break;
            ++used;
            const PickHit h = pick_geometry(w.geoms[e.tile], ray, PickLimits{}, 0);
            if (h.hit && (!walked.hit || h.distance < walked.distance)) walked = h;
        }
        skipped_tiles += order.size() - used;
        CHECK(walked.hit == brute.hit);
        if (walked.hit && brute.hit) {
            ++hits;
            CHECK(close(walked.distance, brute.distance, 1e-9));
            for (int a = 0; a < 3; ++a) CHECK(std::fabs(walked.point[a] - brute.point[a]) <= 1e-7);
        }
    }
    CHECK(hits > 100);
    // Stoppet gör nytta: ibland slipper de bortre bitarna prövas.
    CHECK(skipped_tiles > 0);
    return fails;
}

int check_synthetic() {
    int fails = 0;
    Fixture fx;
    if (!make_fixture(fx, "hnwd_test_pick.nwd")) return fails + 1;
    World w;
    if (!build_world(*fx.loaded.model, w)) return fails + 1;
    CHECK(!w.geoms.empty());
    uint64_t triangles = 0;
    for (const CellGeometry& g : w.geoms) triangles += g.triangles;
    CHECK(triangles > 0);
    fails += check_against_reference(w);
    fails += check_tiles_against_measurement(w);
    fails += check_walk_in_order(w);
    return fails;
}

} // namespace

int test_pick() {
    int fails = 0;
    fails += check_hand_built();
    fails += check_lines_skipped();
    fails += check_tiles_hand_built();
    fails += check_synthetic();
    std::printf("test_pick: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
