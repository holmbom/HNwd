// Visarens geometri för bitar (viewer/cells.h): minnet per del i RAM och på
// grafikkortet, posens matriser per fragment, detaljnivåerna i en mesh och
// geometrin som byggs ur en syntetisk fil, för ett fragment och för en hel bit.
// Ren std och HNwd — ingen fönstertyp — så det prövas utan visaren.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../tools/synth_writer.h"
#include "HNwd.h"
#include "cells.h"
#include "lod_tree.h"
#include "model.h"
#include "test_data.h"
#include "test_support.h"

using namespace nwdview;

namespace {

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
};

// ---------------------------------------------------------------------------
// part_bytes och pick_bytes.

int check_part_bytes() {
    int fails = 0;

    // Tomt kostar ingenting.
    {
        const MeshPart empty;
        const PartBytes b = part_bytes(empty);
        CHECK(b.ram == 0 && b.gpu == 0);
    }

    // 10 hörn, 8 trianglar och 3 poster, utan uv: RAM 12 + 12 + 4 byte per hörn,
    // 4 per index och en FragmentRange per post; grafikkortet 12 + 4 + 4 byte
    // per hörn och 4 per index.
    MeshPart part;
    part.positions.assign(10 * 3, 0.0f);
    part.normals.assign(10 * 3, 0.0f);
    part.colors.assign(10, 0u);
    part.indices.assign(8 * 3, 0u);
    part.ranges.assign(3, FragmentRange{});
    {
        const PartBytes b = part_bytes(part);
        CHECK(b.ram == 10 * 28 + 24 * 4 + 3 * sizeof(FragmentRange));
        CHECK(b.gpu == 10 * 20 + 24 * 4);
    }
    // Med uv: 8 byte till per hörn, i båda.
    part.uvs.assign(10 * 2, 0.0f);
    {
        const PartBytes b = part_bytes(part);
        CHECK(b.ram == 10 * (28 + 8) + 24 * 4 + 3 * sizeof(FragmentRange));
        CHECK(b.gpu == 10 * (20 + 8) + 24 * 4);
    }
    // Konstanterna som schemaläggarens uppskattning bygger på är samma tal.
    CHECK(kRamBytesPerVertex == 28);
    CHECK(kGpuBytesPerVertex == 20);
    CHECK(kIndexBytesPerTriangle == 12);

    // En linjedel: 2 segment är 8 hörn och 12 index. RAM har läge, andra
    // änden, sida och ände samt färg per hörn; grafikkortet packar sida och
    // ände i 4 byte.
    CHECK(kRamBytesPerLineVertex == 12 + 12 + 8 + 4);
    CHECK(kGpuBytesPerLineVertex == 12 + 12 + 4 + 4);
    {
        MeshPart lines;
        lines.lines = true;
        lines.positions.assign(8 * 3, 0.0f);
        lines.other.assign(8 * 3, 0.0f);
        lines.corner.assign(8 * 2, 0.0f);
        lines.colors.assign(8, 0u);
        lines.indices.assign(12, 0u);
        lines.ranges.assign(2, FragmentRange{});
        const PartBytes b = part_bytes(lines);
        CHECK(b.ram == 8 * kRamBytesPerLineVertex + 12 * 4 + 2 * sizeof(FragmentRange));
        CHECK(b.gpu == 8 * kGpuBytesPerLineVertex + 12 * 4);
        const MeshPart empty_lines = [] {
            MeshPart p;
            p.lines = true;
            return p;
        }();
        const PartBytes none = part_bytes(empty_lines);
        CHECK(none.ram == 0 && none.gpu == 0);
    }

    // Sökträdet räknas för sig, och bara i RAM.
    CHECK(kPickBytesPerTriangle == 20);
    {
        const PartBytes b = pick_bytes(1000);
        CHECK(b.ram == 1000 * kPickBytesPerTriangle);
        CHECK(b.gpu == 0);
        const PartBytes none = pick_bytes(0);
        CHECK(none.ram == 0 && none.gpu == 0);
    }
    return fails;
}

// ---------------------------------------------------------------------------
// PoseTransform mot HNwd::world_point och HNwd::world_normal.

int check_pose_transform() {
    int fails = 0;
    Random rnd{12345};

    std::vector<HNwd::Pose> poses;
    poses.emplace_back();   // identitet
    {
        HNwd::Pose p;
        p.t[0] = 10;
        p.t[1] = -20;
        p.t[2] = 30;
        poses.push_back(p);
    }
    {
        // En kvaternion som inte är normerad.
        HNwd::Pose p;
        p.q[0] = 0;
        p.q[1] = 0;
        p.q[2] = 2;
        p.q[3] = 2;
        p.t[0] = 1.5;
        poses.push_back(p);
    }
    {
        // Kvaternionen noll: enhetsmatris.
        HNwd::Pose p;
        for (double& q : p.q) q = 0;
        p.s = 3;
        poses.push_back(p);
    }
    for (const double s : {2.5, -1.5}) {
        // Enhetlig skala, också negativ, med en slumpad vridning.
        HNwd::Pose p;
        for (double& q : p.q) q = rnd.next();
        for (double& t : p.t) t = 1000 * rnd.next();
        p.s = s;
        poses.push_back(p);
    }
    {
        // Matris med olika skala per axel.
        HNwd::Pose p;
        p.affine = true;
        const double m[9] = {1, 2, 0, 0, 1, 3, 0.5, 0, 1};
        for (int i = 0; i < 9; ++i) p.m[i] = m[i];
        p.t[2] = 7;
        poses.push_back(p);
    }
    {
        // Matris som speglar.
        HNwd::Pose p;
        p.affine = true;
        const double m[9] = {-1, 0, 0, 0, 2, 0, 0, 0, 3};
        for (int i = 0; i < 9; ++i) p.m[i] = m[i];
        CHECK(HNwd::mirrors(p));
        poses.push_back(p);
    }

    size_t bad_points = 0, bad_normals = 0;
    for (const HNwd::Pose& pose : poses) {
        const PoseTransform transform(pose);
        for (int k = 0; k < 40; ++k) {
            const float v[3] = {static_cast<float>(100 * rnd.next()),
                                static_cast<float>(100 * rnd.next()),
                                static_cast<float>(100 * rnd.next())};
            double expected[3], got[3];
            HNwd::world_point(pose, v, expected);
            transform.point(v, got);
            for (int a = 0; a < 3; ++a)
                if (std::fabs(expected[a] - got[a]) > 1e-9 * (1 + std::fabs(expected[a])))
                    ++bad_points;

            const float n[3] = {static_cast<float>(rnd.next()), static_cast<float>(rnd.next()),
                                static_cast<float>(rnd.next())};
            float expected_n[3], got_n[3];
            HNwd::world_normal(pose, n, expected_n);
            transform.normal(n, got_n);
            for (int a = 0; a < 3; ++a)
                if (std::fabs(expected_n[a] - got_n[a]) > 1e-6f) ++bad_normals;
        }
        // En normal som blir 0 lämnas som 0.
        const float zero[3] = {0, 0, 0};
        float out[3] = {1, 1, 1};
        transform.normal(zero, out);
        CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0);
    }
    CHECK(bad_points == 0);
    CHECK(bad_normals == 0);
    return fails;
}

// ---------------------------------------------------------------------------
// normals_are_surface.

int check_normals_are_surface() {
    int fails = 0;
    // En triangel i XY-planet, moturs sedd ovanifrån: ytans normal är +Z.
    const std::vector<double> world = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const std::vector<uint32_t> one = {0, 1, 2};

    const float up[9] = {0, 0, 1, 0, 0, 1, 0, 0, 1};
    CHECK(normals_are_surface(world, up, one, 3));

    const float down[9] = {0, 0, -1, 0, 0, -1, 0, 0, -1};
    CHECK(!normals_are_surface(world, down, one, 3));

    // Inom och utanför 1e-3 från ytans normal.
    const float near_surface[9] = {0.0005f, 0, 1, 0, 0, 1, 0, 0, 1};
    CHECK(normals_are_surface(world, near_surface, one, 3));
    const float off[9] = {0, 0, 1, 0.002f, 0, 1, 0, 0, 1};
    CHECK(!normals_are_surface(world, off, one, 3));

    // Ett enda hörn som avviker räcker.
    const float one_wrong[9] = {0, 0, 1, 0, 0, 1, 1, 0, 0};
    CHECK(!normals_are_surface(world, one_wrong, one, 3));

    // Normaler i noll är inte ytans.
    const float zero[9] = {};
    CHECK(!normals_are_surface(world, zero, one, 3));

    // Ingen triangel, en utan yta och ett index utanför räknas inte; utan
    // någon som räknas är svaret nej.
    CHECK(!normals_are_surface(world, up, {}, 3));
    const std::vector<double> line = {0, 0, 0, 1, 0, 0, 2, 0, 0};
    CHECK(!normals_are_surface(line, up, one, 3));
    CHECK(!normals_are_surface(world, up, {0, 1, 5}, 3));

    // En triangel utan yta bland riktiga räknas inte, och en riktig räcker.
    const std::vector<double> mixed = {0, 0, 0, 1, 0, 0, 0, 1, 0, 2, 0, 0};
    const float mixed_normals[12] = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
    CHECK(normals_are_surface(mixed, mixed_normals, {0, 1, 2, 0, 1, 3}, 4));
    return fails;
}

// ---------------------------------------------------------------------------
// triangles_at_least och lod_steps.

// Fragment med slumpade storlekar, störst först, och slumpat antal trianglar.
std::vector<FragmentRange> random_ranges(Random& rnd, size_t count, uint32_t& triangles) {
    std::vector<float> sizes(count);
    const double lo = std::log(0.01), hi = std::log(100.0);
    for (float& s : sizes) s = static_cast<float>(std::exp(lo + (rnd.next() + 1) / 2 * (hi - lo)));
    std::sort(sizes.begin(), sizes.end(), std::greater<float>());
    std::vector<FragmentRange> ranges;
    triangles = 0;
    for (size_t i = 0; i < count; ++i) {
        ranges.push_back({triangles, static_cast<uint32_t>(i + 7), sizes[i]});
        triangles += 1 + static_cast<uint32_t>((rnd.next() + 1) * 5);
    }
    return ranges;
}

int check_triangles_at_least() {
    int fails = 0;
    Random rnd{99};

    // Ingen del: allt som finns (inget) är med.
    CHECK(triangles_at_least({}, 0, 1.0f) == 0);
    CHECK(triangles_at_least({}, 17, 1.0f) == 17);

    for (int round = 0; round < 40; ++round) {
        uint32_t triangles = 0;
        const std::vector<FragmentRange> ranges =
            random_ranges(rnd, 1 + static_cast<size_t>((rnd.next() + 1) * 30), triangles);
        // Gränserna ligger vid fragmentens egna storlekar, strax över och under, och
        // långt utanför.
        std::vector<float> limits = {0.0f, 1e-9f, 1e9f};
        for (const FragmentRange& r : ranges) {
            limits.push_back(r.size);
            limits.push_back(std::nextafter(r.size, 1e9f));
            limits.push_back(std::nextafter(r.size, 0.0f));
        }
        for (const float limit : limits) {
            // En linjär sökning: trianglarna före det första fragmentet som är mindre.
            uint32_t expected = triangles;
            for (const FragmentRange& r : ranges)
                if (r.size < limit) {
                    expected = r.first_triangle;
                    break;
                }
            CHECK(triangles_at_least(ranges, triangles, limit) == expected);
        }
        CHECK(triangles_at_least(ranges, triangles, 0.0f) == triangles);
        CHECK(triangles_at_least(ranges, triangles, 1e9f) == 0);
    }
    return fails;
}

int check_lod_steps() {
    int fails = 0;
    Random rnd{4711};

    // Utan fragment eller trianglar ingen nivå; utan giltiga tal en enda,
    // obegränsad nivå.
    CHECK(lod_steps({}, 0, 3, 1000).empty());
    {
        uint32_t triangles = 0;
        const std::vector<FragmentRange> ranges = random_ranges(rnd, 5, triangles);
        CHECK(lod_steps(ranges, 0, 3, 1000).empty());
        for (const std::pair<double, double> bad : {std::pair<double, double>{0, 1000},
                                                    std::pair<double, double>{3, 0},
                                                    std::pair<double, double>{-1, 1000},
                                                    std::pair<double, double>{3, -5}}) {
            const std::vector<LodStep> steps = lod_steps(ranges, triangles, bad.first, bad.second);
            CHECK(steps.size() == 1);
            if (steps.size() == 1) CHECK(steps[0].triangles == triangles && steps[0].max_distance == 0);
        }
    }
    // Ett fragment, eller lika stora: en nivå med alla trianglar, till avståndet där
    // fragmentet blir min_pixels.
    {
        std::vector<FragmentRange> one = {{0, 5, 2.0f}};
        const std::vector<LodStep> steps = lod_steps(one, 10, 4, 1000);
        CHECK(steps.size() == 1);
        if (!steps.empty()) {
            CHECK(steps[0].triangles == 10);
            CHECK(std::fabs(steps[0].max_distance - 2.0 * 1000 / 4) < 1e-9);
        }
        std::vector<FragmentRange> equal = {{0, 1, 2.0f}, {4, 2, 2.0f}, {9, 3, 2.0f}};
        const std::vector<LodStep> same = lod_steps(equal, 12, 4, 1000, 4);
        CHECK(same.size() == 1);
    }
    // En enda nivå på begäran.
    {
        uint32_t triangles = 0;
        const std::vector<FragmentRange> ranges = random_ranges(rnd, 50, triangles);
        const std::vector<LodStep> steps = lod_steps(ranges, triangles, 3, 1000, 1);
        CHECK(steps.size() == 1);
        if (!steps.empty()) CHECK(steps[0].triangles == triangles);
    }

    // Slumpade delar och kameror: nivåerna växer, är hela trianglar, den sista
    // når dit det största fragmentet blir min_pixels, och den nivå som väljs
    // innehåller varje fragment som syns minst min_pixels.
    size_t multi = 0;
    for (int round = 0; round < 60; ++round) {
        uint32_t triangles = 0;
        const std::vector<FragmentRange> ranges =
            random_ranges(rnd, 1 + static_cast<size_t>((rnd.next() + 1) * 200), triangles);
        const double min_px = 1 + (rnd.next() + 1) * 5;
        const double ppr = 300 + (rnd.next() + 1) * 800;
        const int max_steps = 1 + static_cast<int>((rnd.next() + 1) * 3);
        const std::vector<LodStep> steps = lod_steps(ranges, triangles, min_px, ppr, max_steps);
        CHECK(!steps.empty());
        if (steps.empty()) continue;
        CHECK(steps.size() <= static_cast<size_t>(max_steps));
        if (steps.size() > 1) ++multi;
        CHECK(steps[0].triangles == triangles);
        for (size_t i = 0; i < steps.size(); ++i) {
            CHECK(steps[i].triangles <= triangles);
            CHECK(steps[i].max_distance > 0);
            if (i == 0) continue;
            CHECK(steps[i].max_distance > steps[i - 1].max_distance);
            CHECK(steps[i].triangles <= steps[i - 1].triangles);
            // Det sparar minst en tiondel, annars hade nivåerna slagits ihop.
            CHECK(static_cast<double>(steps[i].triangles) <=
                  0.9 * static_cast<double>(steps[i - 1].triangles) + 1e-9);
        }
        const double last = steps.back().max_distance;
        const double largest = static_cast<double>(ranges.front().size) * ppr / min_px;
        CHECK(std::fabs(last - largest) <= 1e-9 * largest);

        for (int k = 0; k < 300; ++k) {
            const double d = (rnd.next() + 1) / 2 * 1.3 * last + 1e-6;
            const LodStep* level = nullptr;
            for (const LodStep& step : steps)
                if (step.max_distance == 0 || step.max_distance >= d) {
                    level = &step;
                    break;
                }
            for (size_t i = 0; i < ranges.size(); ++i) {
                const double p = static_cast<double>(ranges[i].size) * ppr / d;
                const uint32_t end = i + 1 < ranges.size() ? ranges[i + 1].first_triangle : triangles;
                if (!level) {
                    // Bortom den sista nivån syns inget fragment minst min_pixels.
                    CHECK(p < min_px * (1 + 1e-9));
                } else if (p >= min_px) {
                    CHECK(end <= level->triangles);
                }
            }
        }
    }
    // Spridda storlekar ger flera nivåer ibland.
    CHECK(multi > 0);
    return fails;
}

// ---------------------------------------------------------------------------
// build_fragments och build_tile mot en syntetisk fil.

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

// Linjedelen (parts[2]) mot en egen beräkning ur posterna. `order` är
// fragmenten i den ordning de byggdes och `sizes` deras storlek på samma
// platser; ett fragment som inte är en linjepost bidrar inte. `cap` är
// taket för segment i delen. Varje segment är fyra hörn — (A, −1), (A, +1),
// (B, −1), (B, +1), med den andra änden i `other` — och trianglarna (v, v+1,
// v+2) och (v+2, v+1, v+3), i den ordning fragmenten står.
int check_line_part(const Model& model, HNwd::GeometryReader& reference, const CellGeometry& out,
                    const std::vector<uint32_t>& order, const std::vector<float>& sizes,
                    size_t cap) {
    int fails = 0;
    CHECK(out.parts.size() >= 3);
    if (out.parts.size() < 3) return fails;
    const MeshPart& lp = out.parts[2];
    CHECK(lp.lines);
    CHECK(!lp.transparent);
    CHECK(lp.material == HNwd::kNone);
    CHECK(lp.normals.empty());
    CHECK(lp.uvs.empty());
    for (size_t p = 0; p < out.parts.size(); ++p) CHECK(out.parts[p].lines == (p == 2));

    size_t segment = 0, range_index = 0, bad_positions = 0, bad_ends = 0, bad_corners = 0,
           bad_indices = 0, bad_colors = 0, bad_ranges = 0;
    for (size_t k = 0; k < order.size(); ++k) {
        HNwd::Mesh mesh;
        CHECK(reference.fragment_mesh(order[k], mesh) == HNwd::Status::Ok);
        if (mesh.kind != HNwd::MeshKind::Lines) continue;
        HNwd::Fragment fragment;
        CHECK(model.doc->fragment(order[k], fragment));
        const size_t segs = mesh.indices.size() / 2;
        const size_t take = std::min(segs, cap > segment ? cap - segment : size_t(0));
        if (take == 0) continue;
        if (range_index >= lp.ranges.size()) {
            ++bad_ranges;
            break;
        }
        const FragmentRange& range = lp.ranges[range_index++];
        if (range.fragment != order[k] || range.first_triangle != 2 * segment ||
            range.size != sizes[k])
            ++bad_ranges;
        for (size_t s = 0; s < take; ++s) {
            double a[3], b[3];
            HNwd::world_point(fragment.pose, &mesh.positions[3 * size_t(mesh.indices[2 * s])], a);
            HNwd::world_point(fragment.pose, &mesh.positions[3 * size_t(mesh.indices[2 * s + 1])], b);
            for (int c = 0; c < 3; ++c) {
                a[c] = a[c] * model.scale - out.origin[c];
                b[c] = b[c] * model.scale - out.origin[c];
            }
            const size_t v = 4 * (segment + s);
            for (size_t c = 0; c < 4; ++c) {
                const double* own = c < 2 ? a : b;
                const double* other = c < 2 ? b : a;
                for (size_t e = 0; e < 3; ++e) {
                    if (std::fabs(lp.positions[(v + c) * 3 + e] - own[e]) >
                        2e-5 + 1e-6 * std::fabs(own[e]))
                        ++bad_positions;
                    if (std::fabs(lp.other[(v + c) * 3 + e] - other[e]) >
                        2e-5 + 1e-6 * std::fabs(other[e]))
                        ++bad_ends;
                }
                if (lp.corner[(v + c) * 2] != ((c & 1) ? 1.0f : -1.0f) ||
                    lp.corner[(v + c) * 2 + 1] != (c < 2 ? 0.0f : 1.0f))
                    ++bad_corners;
                if (lp.colors[v + c] != kLineColor) ++bad_colors;
            }
            const uint32_t w = static_cast<uint32_t>(v);
            const uint32_t expected[6] = {w, w + 1, w + 2, w + 2, w + 1, w + 3};
            for (size_t j = 0; j < 6; ++j)
                if (lp.indices[6 * (segment + s) + j] != expected[j]) ++bad_indices;
        }
        segment += take;
    }
    CHECK(range_index == lp.ranges.size());
    CHECK(lp.positions.size() == 12 * segment);
    CHECK(lp.other.size() == 12 * segment);
    CHECK(lp.corner.size() == 8 * segment);
    CHECK(lp.colors.size() == 4 * segment);
    CHECK(lp.indices.size() == 6 * segment);
    CHECK(lp.triangle_count() == 2 * segment);
    CHECK(bad_ranges == 0);
    CHECK(bad_positions == 0);
    CHECK(bad_ends == 0);
    CHECK(bad_corners == 0);
    CHECK(bad_colors == 0);
    CHECK(bad_indices == 0);
    return fails;
}

int check_build_fragments(const Model& model) {
    int fails = 0;
    HNwd::GeometryReader reader, reference;
    CHECK(reader.open(*model.doc) == HNwd::Status::Ok);
    CHECK(reference.open(*model.doc) == HNwd::Status::Ok);
    const BuildLimits limits{};

    // Ett fragment i taget: hörnen, normalerna och indexen är de som posten
    // och posen ger.
    size_t triangle_fragments = 0, line_fragments = 0, bad_positions = 0, bad_normals = 0,
           bad_counts = 0, bad_flat = 0, bad_size = 0;
    for (uint32_t f = 0; f < 120; ++f) {
        Box box;
        CHECK(fragment_box(model, f, box));
        CellGeometry out;
        std::string error;
        const HNwd::Status status = build_fragments(reader, model, &f, 1, box, limits,
                                                    std::stop_token{}, out, error);
        CHECK(status == HNwd::Status::Ok);
        if (status != HNwd::Status::Ok) {
            std::printf("  fragment %u: %s\n", f, error.c_str());
            continue;
        }
        HNwd::Mesh mesh;
        CHECK(reference.fragment_mesh(f, mesh) == HNwd::Status::Ok);
        HNwd::Fragment fragment;
        CHECK(model.doc->fragment(f, fragment));

        if (mesh.kind == HNwd::MeshKind::Lines) {
            ++line_fragments;
            const size_t segments = mesh.indices.size() / 2;
            CHECK(out.triangles == 0);
            CHECK(out.segments == segments);
            CHECK(out.vertices == 4 * segments);
            CHECK(out.flat_fragments == 0);
            CHECK(out.parts.size() >= 3 && !out.parts[2].ranges.empty());
            for (size_t p = 0; p < out.parts.size(); ++p)
                if (p != 2) CHECK(out.parts[p].ranges.empty());
            const std::vector<uint32_t> one = {f};
            const std::vector<float> one_size = {static_cast<float>(box.diagonal())};
            fails += check_line_part(model, reference, out, one, one_size, SIZE_MAX);
            continue;
        }
        CHECK(mesh.kind == HNwd::MeshKind::Triangles);
        ++triangle_fragments;
        CHECK(out.parts.size() >= 3 && out.parts[2].lines && out.parts[2].ranges.empty());
        const size_t vertices = mesh.positions.size() / 3;
        const MeshPart& part = out.parts[0].ranges.empty() ? out.parts[1] : out.parts[0];
        if (part.positions.size() != mesh.positions.size() ||
            part.normals.size() != mesh.positions.size() ||
            part.colors.size() != vertices || part.indices != mesh.indices ||
            out.vertices != vertices || out.triangles != mesh.indices.size() / 3 ||
            part.ranges.size() != 1 || part.ranges[0].fragment != f) {
            ++bad_counts;
            continue;
        }
        if (part.ranges[0].size != static_cast<float>(box.diagonal())) ++bad_size;

        std::vector<double> world(vertices * 3);
        std::vector<float> world_normals(vertices * 3);
        for (size_t v = 0; v < vertices; ++v) {
            double w[3];
            HNwd::world_point(fragment.pose, &mesh.positions[v * 3], w);
            for (int a = 0; a < 3; ++a) {
                w[a] *= model.scale;
                world[v * 3 + static_cast<size_t>(a)] = w[a];
                const double expected = w[a] - out.origin[a];
                if (std::fabs(part.positions[v * 3 + static_cast<size_t>(a)] - expected) >
                    2e-5 + 1e-6 * std::fabs(expected))
                    ++bad_positions;
            }
            HNwd::world_normal(fragment.pose, &mesh.normals[v * 3], &world_normals[v * 3]);
            for (size_t a = 0; a < 3; ++a)
                if (std::fabs(part.normals[v * 3 + a] - world_normals[v * 3 + a]) > 1e-6f)
                    ++bad_normals;
        }
        const bool flat = normals_are_surface(world, world_normals.data(), mesh.indices, vertices);
        if (out.flat_fragments != (flat ? 1u : 0u)) ++bad_flat;
    }
    CHECK(triangle_fragments > 0);
    CHECK(line_fragments > 0);
    CHECK(bad_counts == 0);
    CHECK(bad_positions == 0);
    CHECK(bad_normals == 0);
    CHECK(bad_flat == 0);
    CHECK(bad_size == 0);

    // Flera fragment: delarnas ordning är den givna, och varje triangelfragment
    // kommer med en gång.
    {
        const uint32_t list[6] = {37, 3, 90, 11, 64, 5};
        Box bounds;
        size_t triangles = 0, lines = 0;
        std::vector<uint8_t> is_triangles;
        for (const uint32_t f : list) {
            Box b;
            CHECK(fragment_box(model, f, b));
            bounds.add(b);
            HNwd::Mesh mesh;
            CHECK(reference.fragment_mesh(f, mesh) == HNwd::Status::Ok);
            is_triangles.push_back(static_cast<uint8_t>(mesh.kind == HNwd::MeshKind::Triangles ? 1 : 0));
            if (mesh.kind == HNwd::MeshKind::Triangles) ++triangles;
            else ++lines;
        }
        CellGeometry out;
        std::string error;
        CHECK(build_fragments(reader, model, list, 6, bounds, limits, std::stop_token{}, out,
                              error) == HNwd::Status::Ok);
        CHECK(out.fragments == 6);
        size_t seen = 0, seen_lines = 0;
        for (const MeshPart& part : out.parts) {
            std::ptrdiff_t last = -1;
            for (const FragmentRange& r : part.ranges) {
                const std::ptrdiff_t at = std::find(list, list + 6, r.fragment) - list;
                CHECK(at < 6);
                CHECK(at > last);
                last = at;
                if (part.lines) ++seen_lines;
                else ++seen;
            }
        }
        CHECK(seen == triangles);
        CHECK(seen_lines == lines);

        // Linjedelen står i den givna ordningen, med storleken ur lådan.
        std::vector<uint32_t> order(list, list + 6);
        std::vector<float> order_sizes;
        for (const uint32_t f : list) {
            Box b;
            CHECK(fragment_box(model, f, b));
            order_sizes.push_back(static_cast<float>(b.diagonal()));
        }
        fails += check_line_part(model, reference, out, order, order_sizes, SIZE_MAX);

        // min_size: fragment med mindre diagonal byggs inte, de andra gör det.
        {
            std::vector<float> sorted = order_sizes;
            std::sort(sorted.begin(), sorted.end());
            const float cut = sorted[2];
            BuildLimits sized;
            sized.min_size = cut;
            CellGeometry cut_out;
            CHECK(build_fragments(reader, model, list, 6, bounds, sized, std::stop_token{},
                                  cut_out, error) == HNwd::Status::Ok);
            size_t expected = 0;
            for (const float s : order_sizes)
                if (s >= cut) ++expected;
            CHECK(cut_out.fragments == expected);
            for (const MeshPart& part : cut_out.parts)
                for (const FragmentRange& r : part.ranges) {
                    CHECK(r.size >= cut);
                    const std::ptrdiff_t at = std::find(list, list + 6, r.fragment) - list;
                    CHECK(at < 6 && order_sizes[static_cast<size_t>(at)] >= cut);
                }
            // Över alla storlekar byggs ingenting.
            sized.min_size = sorted.back() * 2 + 1;
            CHECK(build_fragments(reader, model, list, 6, bounds, sized, std::stop_token{},
                                  cut_out, error) == HNwd::Status::Ok);
            CHECK(cut_out.fragments == 0 && cut_out.vertices == 0);
            CHECK(cut_out.parts.size() == 3);
        }

        // Ett dolt fragment byggs inte.
        auto filter = std::make_shared<ModelFilter>();
        filter->first = {37};
        filter->end = {38};
        BuildLimits hiding;
        hiding.filter = filter;
        CellGeometry out2;
        CHECK(build_fragments(reader, model, list, 6, bounds, hiding, std::stop_token{}, out2,
                              error) == HNwd::Status::Ok);
        CHECK(out2.fragments == 5);
        for (const MeshPart& part : out2.parts)
            for (const FragmentRange& r : part.ranges) CHECK(r.fragment != 37);
    }
    // Ett avbrott före första fragmentet.
    {
        std::stop_source source;
        source.request_stop();
        const uint32_t f = 0;
        Box box;
        CHECK(fragment_box(model, f, box));
        CellGeometry out;
        std::string error;
        CHECK(build_fragments(reader, model, &f, 1, box, limits, source.get_token(), out, error) ==
              HNwd::Status::Cancelled);
    }
    return fails;
}

int check_build_tile(const Model& model) {
    int fails = 0;
    LodTreeSettings settings;
    settings.max_tile_triangles = 150;
    settings.min_tile_triangles = 0;
    settings.max_leaf_fragments = 16;
    settings.threads = 2;
    LodTree tree;
    LodTreeStats stats;
    CHECK(plan_lod_tree(model, settings, std::stop_token{}, tree, stats));
    CHECK(!tree.tiles.empty());
    if (tree.tiles.empty()) return fails;

    HNwd::GeometryReader reader, reference;
    CHECK(reader.open(*model.doc) == HNwd::Status::Ok);
    CHECK(reference.open(*model.doc) == HNwd::Status::Ok);
    const BuildLimits limits{};

    // De första bitarna och den med flest fragment.
    std::vector<uint32_t> picked;
    uint32_t widest = 0;
    for (uint32_t t = 0; t < tree.tiles.size(); ++t) {
        if (tree.tiles[t].frag_count > tree.tiles[widest].frag_count) widest = t;
        if (picked.size() < 5) picked.push_back(t);
    }
    if (std::find(picked.begin(), picked.end(), widest) == picked.end()) picked.push_back(widest);
    CHECK(tree.tiles[widest].frag_count >= 2);

    size_t capped_checked = 0;
    for (const uint32_t ti : picked) {
        const Tile& tile = tree.tiles[ti];
        CellGeometry out;
        std::string error;
        const HNwd::Status status =
            build_tile(reader, model, tree, ti, limits, std::stop_token{}, out, error);
        CHECK(status == HNwd::Status::Ok);
        if (status != HNwd::Status::Ok) {
            std::printf("  tile %u: %s\n", ti, error.c_str());
            continue;
        }
        double centre[3];
        tile.bounds.center(centre);
        for (int a = 0; a < 3; ++a) CHECK(out.origin[a] == centre[a]);
        CHECK(out.diagonal == tile.bounds.diagonal());
        if (ti == widest) {
            CHECK(out.read_seconds > 0);
            CHECK(out.convert_seconds > 0);
        }

        // Vad varje fragment i biten är.
        std::unordered_map<uint32_t, uint32_t> position;   // fragment → plats i biten
        std::vector<uint8_t> is_triangles(tile.frag_count, 0);
        uint32_t triangle_fragments = 0, line_fragments = 0;
        uint64_t line_segments = 0;
        for (uint32_t k = 0; k < tile.frag_count; ++k) {
            const uint32_t f = tree.frags[tile.frag_begin + k];
            position[f] = k;
            HNwd::Mesh mesh;
            CHECK(reference.fragment_mesh(f, mesh) == HNwd::Status::Ok);
            if (mesh.kind == HNwd::MeshKind::Triangles) {
                is_triangles[k] = 1;
                ++triangle_fragments;
            } else if (mesh.kind == HNwd::MeshKind::Lines) {
                ++line_fragments;
                line_segments += mesh.indices.size() / 2;
            }
        }
        CHECK(out.fragments == triangle_fragments + line_fragments);
        CHECK(out.undecoded == 0);
        CHECK(out.segments == line_segments);
        if (line_fragments > 0) {
            CHECK(out.segments_skipped == 0);
            CHECK(out.parts.size() >= 3 && out.parts[2].triangle_count() == 2 * line_segments);
        }
        {
            std::vector<uint32_t> order(tile.frag_count);
            std::vector<float> order_sizes(tile.frag_count);
            for (uint32_t k = 0; k < tile.frag_count; ++k) {
                order[k] = tree.frags[tile.frag_begin + k];
                order_sizes[k] = tree.sizes[tile.frag_begin + k];
            }
            fails += check_line_part(model, reference, out, order, order_sizes, SIZE_MAX);
        }

        // Varje triangelfragment står en gång, störst först i sin del, med
        // sin storlek, och dess trianglar är de som en egen byggnad av
        // fragmentet ger.
        std::vector<uint32_t> seen(tile.frag_count, 0);
        uint64_t part_triangles = 0, part_vertices = 0;
        for (const MeshPart& part : out.parts) {
            part_vertices += part.positions.size() / 3;
            if (part.lines) continue;   // prövas av check_line_part
            part_triangles += part.triangle_count();
            std::ptrdiff_t last = -1;
            for (size_t r = 0; r < part.ranges.size(); ++r) {
                const FragmentRange& range = part.ranges[r];
                const auto found = position.find(range.fragment);
                CHECK(found != position.end());
                if (found == position.end()) continue;
                const uint32_t k = found->second;
                ++seen[k];
                CHECK(static_cast<std::ptrdiff_t>(k) > last);
                last = static_cast<std::ptrdiff_t>(k);
                CHECK(range.size == tree.sizes[tile.frag_begin + k]);
                if (r > 0) {
                    CHECK(range.first_triangle > part.ranges[r - 1].first_triangle);
                    CHECK(range.size <= part.ranges[r - 1].size);
                }
                const uint32_t next = r + 1 < part.ranges.size()
                                          ? part.ranges[r + 1].first_triangle
                                          : static_cast<uint32_t>(part.triangle_count());
                HNwd::Fragment fragment;
                CHECK(model.doc->fragment(range.fragment, fragment));
                CHECK(next - range.first_triangle == fragment.primitives);

                CellGeometry single;
                std::string single_error;
                const uint32_t f = range.fragment;
                CHECK(build_fragments(reference, model, &f, 1, tile.bounds, limits,
                                      std::stop_token{}, single, single_error) ==
                      HNwd::Status::Ok);
                const MeshPart* same = nullptr;
                for (const MeshPart& q : single.parts)
                    if (!q.ranges.empty()) same = &q;
                CHECK(same != nullptr);
                if (!same) continue;
                CHECK(same->triangle_count() == fragment.primitives);
                CHECK(same->transparent == part.transparent);
                CHECK(same->material == part.material);
                // Samma hörn och normaler i samma ordning, tal för tal.
                size_t differing = 0;
                for (uint32_t j = 0; j < 3 * fragment.primitives; ++j) {
                    const uint32_t a = part.indices[3 * range.first_triangle + j];
                    const uint32_t b = same->indices[j];
                    for (size_t c = 0; c < 3; ++c) {
                        if (part.positions[a * 3 + c] != same->positions[b * 3 + c]) ++differing;
                        if (part.normals[a * 3 + c] != same->normals[b * 3 + c]) ++differing;
                    }
                    if (part.colors[a] != same->colors[b]) ++differing;
                }
                CHECK(differing == 0);
            }
        }
        for (uint32_t k = 0; k < tile.frag_count; ++k) CHECK(seen[k] == static_cast<uint32_t>(is_triangles[k]));
        CHECK(part_triangles == out.triangles);
        CHECK(part_vertices == out.vertices);
        CHECK(out.flat_fragments <= out.fragments);

        // Taket för linjesegment: bara så många, och resten räknas.
        if (line_segments > 4) {
            BuildLimits capped;
            capped.max_segments = 3;
            CellGeometry small;
            CHECK(build_tile(reader, model, tree, ti, capped, std::stop_token{}, small, error) ==
                  HNwd::Status::Ok);
            CHECK(small.parts.size() >= 3);
            const uint64_t emitted = small.parts.size() >= 3 ? small.parts[2].triangle_count() / 2 : 0;
            CHECK(emitted <= 3);
            CHECK(small.segments == line_segments);
            CHECK(emitted + small.segments_skipped == line_segments);
            // De största fragmentens linjer är de som behålls.
            std::vector<uint32_t> order(tile.frag_count);
            std::vector<float> order_sizes(tile.frag_count);
            for (uint32_t k = 0; k < tile.frag_count; ++k) {
                order[k] = tree.frags[tile.frag_begin + k];
                order_sizes[k] = tree.sizes[tile.frag_begin + k];
            }
            fails += check_line_part(model, reference, small, order, order_sizes, 3);

            // Utan tak för linjer: inga linjer, och allt räknas som överhoppat.
            BuildLimits none;
            none.max_segments = 0;
            CellGeometry nothing;
            CHECK(build_tile(reader, model, tree, ti, none, std::stop_token{}, nothing, error) ==
                  HNwd::Status::Ok);
            CHECK(nothing.parts.size() >= 3 && nothing.parts[2].lines &&
                  nothing.parts[2].ranges.empty() && nothing.parts[2].positions.empty());
            CHECK(nothing.segments == line_segments && nothing.segments_skipped == line_segments);
            ++capped_checked;
        }
    }
    (void)capped_checked;

    // Ett dolt fragment byggs inte.
    {
        const Tile& tile = tree.tiles[widest];
        const uint32_t hidden = tree.frags[tile.frag_begin];
        auto filter = std::make_shared<ModelFilter>();
        filter->first = {hidden};
        filter->end = {hidden + 1};
        BuildLimits hiding;
        hiding.filter = filter;
        CellGeometry all, fewer;
        std::string error;
        CHECK(build_tile(reader, model, tree, widest, limits, std::stop_token{}, all, error) ==
              HNwd::Status::Ok);
        CHECK(build_tile(reader, model, tree, widest, hiding, std::stop_token{}, fewer, error) ==
              HNwd::Status::Ok);
        CHECK(fewer.fragments + 1 == all.fragments);
        for (const MeshPart& part : fewer.parts)
            for (const FragmentRange& r : part.ranges) CHECK(r.fragment != hidden);
    }
    // min_size: exakt prefixet av bitens fragment som är minst så stora, och
    // bara det.
    {
        const Tile& tile = tree.tiles[widest];
        const float* sizes = tree.sizes.data() + tile.frag_begin;
        std::vector<float> cuts = {0.0f,
                                   tile.min_size,
                                   std::nextafter(tile.min_size, 1e9f),
                                   tile.max_size,
                                   std::nextafter(tile.max_size, 1e9f)};
        for (const uint32_t k : {tile.frag_count / 3, tile.frag_count / 2, tile.frag_count - 1}) {
            cuts.push_back(sizes[k]);
            cuts.push_back(std::nextafter(sizes[k], 1e9f));
            cuts.push_back(std::nextafter(sizes[k], 0.0f));
        }
        CellGeometry whole;
        std::string error;
        CHECK(build_tile(reader, model, tree, widest, limits, std::stop_token{}, whole, error) ==
              HNwd::Status::Ok);
        size_t proper_prefixes = 0;
        for (const float cut : cuts) {
            BuildLimits sized;
            sized.min_size = cut;
            CellGeometry got;
            CHECK(build_tile(reader, model, tree, widest, sized, std::stop_token{}, got, error) ==
                  HNwd::Status::Ok);

            uint32_t count = 0;
            while (count < tile.frag_count && sizes[count] >= cut) ++count;
            if (count > 0 && count < tile.frag_count) ++proper_prefixes;
            std::vector<uint32_t> order(count);
            std::vector<float> order_sizes(count);
            uint32_t built = 0;
            uint64_t segments = 0, primitives = 0, triangles = 0;
            for (uint32_t k = 0; k < count; ++k) {
                order[k] = tree.frags[tile.frag_begin + k];
                order_sizes[k] = sizes[k];
                HNwd::Mesh mesh;
                CHECK(reference.fragment_mesh(order[k], mesh) == HNwd::Status::Ok);
                HNwd::Fragment fragment;
                CHECK(model.doc->fragment(order[k], fragment));
                primitives += fragment.primitives;
                ++built;
                if (mesh.kind == HNwd::MeshKind::Lines) segments += fragment.primitives;
                else triangles += fragment.primitives;
            }
            CHECK(got.fragments == built);
            CHECK(got.segments == segments);
            CHECK(got.triangles == triangles);
            CHECK(got.undecoded == 0);
            for (const MeshPart& part : got.parts)
                for (const FragmentRange& r : part.ranges) {
                    CHECK(r.size >= cut);
                    const auto at = std::find(order.begin(), order.end(), r.fragment);
                    CHECK(at != order.end());
                }
            fails += check_line_part(model, reference, got, order, order_sizes, SIZE_MAX);

            // Detaljnivåerna och prefixet gäller linjedelen som de andra delarna.
            if (whole.parts.size() >= 3)
                CHECK(triangles_at_least(whole.parts[2].ranges,
                                         static_cast<uint32_t>(whole.parts[2].triangle_count()), cut) ==
                      2 * segments);

            // Prefixet som trädet anger är samma.
            const TilePrefix prefix = tree.prefix_at_least(widest, cut);
            CHECK(prefix.fragments == count);
            CHECK(prefix.primitives == primitives);
            CHECK(prefix.omitted == (count < tile.frag_count ? sizes[count] : 0.0f));

            if (cut <= tile.min_size) {
                // Hela biten, som utan min_size.
                CHECK(got.fragments == whole.fragments);
                CHECK(got.vertices == whole.vertices);
                CHECK(got.triangles == whole.triangles);
            }
            if (count == 0) {
                CHECK(got.fragments == 0 && got.vertices == 0);
                CHECK(got.parts.size() == 3);
            }
        }
        CHECK(proper_prefixes > 0);
        CHECK(!whole.parts.empty());
        if (whole.parts.size() >= 3 && !whole.parts[2].ranges.empty()) {
            const MeshPart& lp = whole.parts[2];
            const std::vector<LodStep> steps =
                lod_steps(lp.ranges, static_cast<uint32_t>(lp.triangle_count()), 3, 1000);
            CHECK(!steps.empty());
            if (!steps.empty()) {
                CHECK(steps.front().triangles == lp.triangle_count());
                // Varje nivå är ett helt antal segment: två trianglar vardera.
                for (const LodStep& step : steps) CHECK(step.triangles % 2 == 0);
            }
        }
    }
    // En bit utanför trädet, och ett avbrott.
    {
        CellGeometry out;
        std::string error;
        CHECK(build_tile(reader, model, tree, static_cast<uint32_t>(tree.tiles.size()), limits,
                         std::stop_token{}, out, error) == HNwd::Status::OutOfRange);
        CHECK(!error.empty());
        std::stop_source source;
        source.request_stop();
        error.clear();
        CHECK(build_tile(reader, model, tree, 0, limits, source.get_token(), out, error) ==
              HNwd::Status::Cancelled);
    }
    return fails;
}

int check_building() {
    int fails = 0;
    Fixture fx;
    if (!make_fixture(fx, "hnwd_test_viewer_cells.nwd")) return fails + 1;
    fails += check_build_fragments(*fx.loaded.model);
    fails += check_build_tile(*fx.loaded.model);
    return fails;
}

} // namespace

int test_viewer_cells() {
    int fails = 0;
    fails += check_part_bytes();
    fails += check_pose_transform();
    fails += check_normals_are_surface();
    fails += check_triangles_at_least();
    fails += check_lod_steps();
    fails += check_building();
    std::printf("test_viewer_cells: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
