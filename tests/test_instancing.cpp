// Instanserna (viewer/instancing.h): posen som en placering mot
// HNwd::world_point, grupperingen av fragment efter geometripost och
// trösklarna, på en syntetisk fil och på exempelfilerna, och hur ofta posterna
// återanvänds (record_use). Ren std och HNwd — ingen fönstertyp — så det
// prövas utan visaren.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <stop_token>
#include <string>
#include <vector>

#include "../tools/synth_writer.h"
#include "HNwd.h"
#include "cells.h"
#include "instancing.h"
#include "lod_tree.h"
#include "model.h"
#include "test_data.h"
#include "test_support.h"

using namespace nwdview;

namespace {

constexpr double kPi = 3.14159265358979323846;

// Tar bort filen när den går ur räckvidd. Modellen som öppnat den ska
// deklareras efter den, så att den stängs först.
struct Removed {
    std::string path;
    ~Removed() { testdata::remove_utf8(path); }
};

// Slumptal som är lika på alla plattformar.
struct Rng {
    uint64_t state;
    uint64_t next() {
        state += 0x9E3779B97F4A7C15ull;
        uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double unit() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
    double sym() { return 2 * unit() - 1; }
};

// ---------------------------------------------------------------------------
// Matriser och kvaternioner, skrivna för sig så att de inte delar kod med
// biblioteket.

using Quat = std::array<double, 4>;   // x, y, z, w

Quat qmul(const Quat& a, const Quat& b) {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

// En vridning kring axeln (0 = x, 1 = y, 2 = z) med vinkeln i grader, moturs
// sett från axelns spets.
Quat axis_angle(int axis, double degrees) {
    Quat q = {0, 0, 0, std::cos(degrees * kPi / 360)};
    q[static_cast<size_t>(axis)] = std::sin(degrees * kPi / 360);
    return q;
}

// R = Rz(−yaw) · Rx(pitch) · Ry(roll) som kvaternion.
Quat quat_from_angles(double yaw, double pitch, double roll) {
    return qmul(axis_angle(2, -yaw), qmul(axis_angle(0, pitch), axis_angle(1, roll)));
}

struct Mat {
    double v[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};   // radvis
};

Mat mul(const Mat& a, const Mat& b) {
    Mat r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            double sum = 0;
            for (int k = 0; k < 3; ++k) sum += a.v[3 * i + k] * b.v[3 * k + j];
            r.v[3 * i + j] = sum;
        }
    return r;
}

Mat rot_x(double radians) {
    const double c = std::cos(radians), s = std::sin(radians);
    Mat m;
    m.v[4] = c;
    m.v[5] = -s;
    m.v[7] = s;
    m.v[8] = c;
    return m;
}

Mat rot_y(double radians) {
    const double c = std::cos(radians), s = std::sin(radians);
    Mat m;
    m.v[0] = c;
    m.v[2] = s;
    m.v[6] = -s;
    m.v[8] = c;
    return m;
}

Mat rot_z(double radians) {
    const double c = std::cos(radians), s = std::sin(radians);
    Mat m;
    m.v[0] = c;
    m.v[1] = -s;
    m.v[3] = s;
    m.v[4] = c;
    return m;
}

// Placeringens R ur vinklarna i grader.
Mat rotation_of(const InstancePose& p) {
    const double deg = kPi / 180;
    return mul(rot_z(-static_cast<double>(p.yaw) * deg),
               mul(rot_x(static_cast<double>(p.pitch) * deg),
                   rot_y(static_cast<double>(p.roll) * deg)));
}

HNwd::Pose quaternion_pose(const Quat& q, double s, const double t[3]) {
    HNwd::Pose p;
    for (int k = 0; k < 4; ++k) p.q[k] = q[static_cast<size_t>(k)];
    p.s = s;
    for (int k = 0; k < 3; ++k) p.t[k] = t[k];
    return p;
}

HNwd::Pose matrix_pose(const Mat& m, const double t[3]) {
    HNwd::Pose p;
    p.affine = true;
    for (int k = 0; k < 9; ++k) p.m[k] = m.v[k];
    for (int k = 0; k < 3; ++k) p.t[k] = t[k];
    return p;
}

// ---------------------------------------------------------------------------
// instance_pose mot world_point.

// Placeringen ska ge samma punkter som posen: för varje punkt v är
// world_point(pose, v) · metres lika med at + R · (scale · ((v − center) ·
// metres)), inom 1e-6 av den längsta lokala vektorn.
int check_pose(const HNwd::Pose& pose, double metres, const float center[3], const char* what) {
    int fails = 0;
    InstancePose placed;
    if (!instance_pose(pose, metres, center, placed)) {
        std::printf("  %s: instance_pose gav false\n", what);
        return 1;
    }
    CHECK(placed.pitch >= -90.0f && placed.pitch <= 90.0f);
    CHECK(placed.yaw >= -180.0f && placed.yaw <= 180.0f);
    CHECK(placed.roll >= -180.0f && placed.roll <= 180.0f);
    CHECK(placed.scale[0] > 0 && placed.scale[1] > 0);
    // Bara en spegling har en negativ skala, och den ligger på den tredje axeln.
    CHECK(HNwd::mirrors(pose) == (placed.scale[2] < 0));

    const Mat r = rotation_of(placed);
    std::vector<std::array<float, 3>> offsets;
    for (int c = 0; c < 8; ++c)
        offsets.push_back({(c & 1) ? 4.0f : -4.0f, (c & 2) ? 3.0f : -3.0f, (c & 4) ? 2.0f : -2.0f});
    offsets.push_back({0.0f, 0.0f, 0.0f});
    Rng rng{static_cast<uint64_t>(metres * 1e6) + 17};
    for (int i = 0; i < 24; ++i)
        offsets.push_back({static_cast<float>(rng.sym() * 5), static_cast<float>(rng.sym() * 5),
                           static_cast<float>(rng.sym() * 5)});

    double radius = 0;
    std::vector<double> errors;
    for (const auto& o : offsets) {
        const float v[3] = {center[0] + o[0], center[1] + o[1], center[2] + o[2]};
        double want[3];
        HNwd::world_point(pose, v, want);
        double local[3];
        for (int k = 0; k < 3; ++k) {
            local[k] = (static_cast<double>(v[k]) - static_cast<double>(center[k])) * metres *
                       static_cast<double>(placed.scale[k]);
            want[k] *= metres;
        }
        double got[3];
        for (int i = 0; i < 3; ++i)
            got[i] = placed.at[i] + r.v[3 * i] * local[0] + r.v[3 * i + 1] * local[1] +
                     r.v[3 * i + 2] * local[2];
        radius = std::max(radius, std::sqrt(local[0] * local[0] + local[1] * local[1] +
                                            local[2] * local[2]));
        errors.push_back(std::max({std::fabs(got[0] - want[0]), std::fabs(got[1] - want[1]),
                                   std::fabs(got[2] - want[2])}));
    }
    const double at_size =
        std::max({std::fabs(placed.at[0]), std::fabs(placed.at[1]), std::fabs(placed.at[2])});
    const double tolerance = 1e-6 * radius + 1e-13 * at_size;
    size_t bad = 0;
    double worst = 0;
    for (const double e : errors) {
        if (!(e <= tolerance)) ++bad;
        worst = std::max(worst, e);
    }
    if (bad != 0)
        std::printf("  %s: %zu punkter av %zu avviker, värst %.3g mot toleransen %.3g\n", what, bad,
                    errors.size(), worst, tolerance);
    CHECK(bad == 0);
    return fails;
}

int check_instance_pose() {
    int fails = 0;
    Rng rng{7};
    const double units[] = {1.0, 0.001, 0.01, 0.3048};
    const auto run = [&](const HNwd::Pose& pose, const char* what) {
        for (const double metres : units) {
            float center[3];
            for (float& c : center) c = static_cast<float>(rng.sym() * 60);
            fails += check_pose(pose, metres, center, what);
        }
    };
    const auto random_t = [&](double t[3], double magnitude) {
        for (int k = 0; k < 3; ++k) t[k] = rng.sym() * magnitude;
    };
    double zero[3] = {0, 0, 0};

    // Ingen vridning, bara en flytt, också en mycket stor.
    run(HNwd::Pose{}, "identitet");
    for (const double magnitude : {10.0, 4e6}) {
        double t[3];
        random_t(t, magnitude);
        run(quaternion_pose({0, 0, 0, 1}, 1, t), "flytt");
    }

    // Vridning kring varje axel, och kvaternioner som inte är normerade.
    for (int axis = 0; axis < 3; ++axis)
        for (const double degrees : {30.0, -75.0, 90.0, 170.0, -180.0, 1e-4}) {
            double t[3];
            random_t(t, 1000);
            run(quaternion_pose(axis_angle(axis, degrees), 1, t), "axelvridning");
        }
    for (int i = 0; i < 300; ++i) {
        Quat q = {rng.sym(), rng.sym(), rng.sym(), rng.sym()};
        const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (n < 1e-3) continue;
        const double factor = (i % 3 == 0 ? 3.7 : 1.0) / n;
        for (double& x : q) x *= factor;
        double t[3];
        random_t(t, 5000);
        run(quaternion_pose(q, 1, t), "slumpad vridning");
    }

    // Pitch på och nära ±90, där yaw och roll inte går att skilja åt.
    for (const double pitch : {90.0, -90.0, 90.0 - 1e-6, -90.0 + 1e-6, 90.0 - 1e-7, 90.0 - 1e-8,
                               89.0, -89.9999})
        for (int i = 0; i < 6; ++i) {
            double t[3];
            random_t(t, 100);
            run(quaternion_pose(quat_from_angles(rng.sym() * 180, pitch, rng.sym() * 180), 1, t),
                "pitch nära 90");
        }

    // Enhetlig skala, också negativ (spegling).
    for (const double s : {0.001, 0.5, 2.5, 1000.0, -1.0, -2.5, -0.01}) {
        double t[3];
        random_t(t, 200);
        run(quaternion_pose(quat_from_angles(rng.sym() * 180, rng.sym() * 90, rng.sym() * 180), s, t),
            "enhetlig skala");
    }

    // Skala per axel: R · diag(S) som matris, med alla teckenkombinationer.
    for (int signs = 0; signs < 8; ++signs)
        for (int i = 0; i < 6; ++i) {
            Mat m = mul(rot_z(rng.sym() * kPi), mul(rot_x(rng.sym() * kPi / 2), rot_y(rng.sym() * kPi)));
            for (int col = 0; col < 3; ++col) {
                const double s = (0.1 + 5 * rng.unit()) * ((signs >> col) & 1 ? -1 : 1);
                for (int row = 0; row < 3; ++row) m.v[3 * row + col] *= s;
            }
            double t[3];
            random_t(t, 300);
            run(matrix_pose(m, t), "skala per axel");
        }

    // Konventionen: yaw medurs ovanifrån, pitch lyfter +y, roll sänker +x.
    {
        const float center[3] = {0, 0, 0};
        InstancePose p;
        CHECK(instance_pose(quaternion_pose(axis_angle(2, -90), 1, zero), 1, center, p));
        CHECK(std::fabs(p.yaw - 90.0f) < 1e-3f && std::fabs(p.pitch) < 1e-3f &&
              std::fabs(p.roll) < 1e-3f);
        double w[3];
        const float north[3] = {0, 1, 0};
        HNwd::world_point(quaternion_pose(axis_angle(2, -90), 1, zero), north, w);
        CHECK(std::fabs(w[0] - 1) < 1e-9 && std::fabs(w[1]) < 1e-9);   // +y pekar mot öster
        CHECK(instance_pose(quaternion_pose(axis_angle(0, 30), 1, zero), 1, center, p));
        CHECK(std::fabs(p.pitch - 30.0f) < 1e-3f && std::fabs(p.yaw) < 1e-3f &&
              std::fabs(p.roll) < 1e-3f);
        CHECK(instance_pose(quaternion_pose(axis_angle(1, 30), 1, zero), 1, center, p));
        CHECK(std::fabs(p.roll - 30.0f) < 1e-3f && std::fabs(p.yaw) < 1e-3f &&
              std::fabs(p.pitch) < 1e-3f);
        // Skalan är posens, utan enhetens faktor; läget är i meter.
        CHECK(instance_pose(quaternion_pose({0, 0, 0, 1}, 2.5, zero), 0.001, center, p));
        CHECK(p.scale[0] == 2.5f && p.scale[1] == 2.5f && p.scale[2] == 2.5f);
        const double t[3] = {1000, 2000, 3000};
        const float c[3] = {0, 0, 0};
        CHECK(instance_pose(quaternion_pose({0, 0, 0, 1}, 1, t), 0.001, c, p));
        CHECK(std::fabs(p.at[0] - 1) < 1e-12 && std::fabs(p.at[1] - 2) < 1e-12 &&
              std::fabs(p.at[2] - 3) < 1e-12);
        // En spegling har en negativ tredje skala.
        CHECK(instance_pose(quaternion_pose({0, 0, 0, 1}, -2, zero), 1, center, p));
        CHECK(p.scale[0] == 2.0f && p.scale[1] == 2.0f && p.scale[2] == -2.0f);
    }

    // Det som inte är en vridning gånger en skala per axel.
    {
        const float center[3] = {1, 2, 3};
        InstancePose before;
        before.at[0] = 11;
        before.yaw = 22;
        before.scale[1] = 33;
        const auto rejected = [&](const HNwd::Pose& pose, double metres, const char* what) {
            InstancePose p = before;
            const bool ok = instance_pose(pose, metres, center, p);
            if (ok) std::printf("  %s: instance_pose gav true\n", what);
            CHECK(!ok);
            // Vid false är `out` orörd.
            CHECK(p.at[0] == 11 && p.yaw == 22 && p.scale[1] == 33);
        };
        Mat shear;
        shear.v[1] = 0.5;
        rejected(matrix_pose(shear, zero), 1, "skjuvning");
        Mat late = mul(Mat{{2, 0, 0, 0, 1, 0, 0, 0, 1}}, rot_z(kPi / 4));   // skala efter vridningen
        rejected(matrix_pose(late, zero), 1, "skala efter vridning");
        Mat shear_small;
        shear_small.v[1] = 1e-4;
        rejected(matrix_pose(shear_small, zero), 1, "liten skjuvning");
        Mat flat;
        flat.v[4] = 0;
        rejected(matrix_pose(flat, zero), 1, "skala noll");
        rejected(quaternion_pose({0, 0, 0, 1}, 0, zero), 1, "enhetlig skala noll");
        Mat bad_nan;
        bad_nan.v[0] = std::nan("");
        rejected(matrix_pose(bad_nan, zero), 1, "matris med NaN");
        const double inf_t[3] = {HUGE_VAL, 0, 0};
        rejected(quaternion_pose({0, 0, 0, 1}, 1, inf_t), 1, "oändlig flytt");
        const double nan_t[3] = {0, std::nan(""), 0};
        rejected(quaternion_pose({0, 0, 0, 1}, 1, nan_t), 1, "flytt med NaN");
        rejected(quaternion_pose({0, 0, 0, 1}, HUGE_VAL, zero), 1, "oändlig skala");
        Mat very_large;
        very_large.v[0] = 1e300;
        rejected(matrix_pose(very_large, zero), 1, "skala som inte ryms som float");
        Mat very_small;
        very_small.v[0] = 1e-300;
        rejected(matrix_pose(very_small, zero), 1, "skala som blir noll som float");
        rejected(HNwd::Pose{}, 0.0, "enhet noll");
        rejected(HNwd::Pose{}, -1.0, "negativ enhet");
        rejected(HNwd::Pose{}, std::nan(""), "enhet NaN");
        rejected(HNwd::Pose{}, HUGE_VAL, "oändlig enhet");
        const double big_t[3] = {1e308, 0, 0};
        rejected(quaternion_pose({0, 0, 0, 1}, 1, big_t), 1e10, "läget spränger double");

        // Gränsen: en skjuvning på 1e-7 i cosinus tas emot, 1e-4 gör det inte.
        Mat almost;
        almost.v[1] = 1e-7;
        InstancePose p;
        CHECK(instance_pose(matrix_pose(almost, zero), 1, center, p));
    }
    return fails;
}

// ---------------------------------------------------------------------------
// Grupperingen: ett facit som räknar efter fragment för fragment, och planen
// mot det.

struct Fixture {
    Removed file;
    LoadResult loaded;
};

// Skriver en syntetisk fil och läser den.
bool write_and_load(Fixture& fx, const std::string& name, const synth::Options& o) {
    fx.file.path = testdata::tmp_path(name);
    synth::Result written;
    std::string why;
    if (!synth::write(fx.file.path, o, written, why)) {
        std::printf("  write: %s\n", why.c_str());
        return false;
    }
    fx.loaded = load_model(fx.file.path, {});
    if (!fx.loaded.model) std::printf("  load_model: %s\n", fx.loaded.error.c_str());
    return fx.loaded.model != nullptr;
}

// Ger fragmentet en plats i en grupp? Samma villkor som i headern, skrivna
// ur det publika API:t.
bool instanceable(const Model& m, size_t i, const InstancingSettings& s, HNwd::Fragment& f) {
    if (!m.doc->fragment(i, f)) return false;
    if (f.record == 0 || f.record > m.doc->record_count()) return false;
    if (f.primitives == 0 || f.primitives > s.max_triangles) return false;
    HNwd::Look look;
    if (m.doc->look(f.look, look)) {
        if (!(1.0f - look.transparency >= 0.996f)) return false;
        HNwd::Material mat;
        if (look.material != HNwd::kNone && m.doc->material(look.material, mat) && mat.has_texture)
            return false;
    }
    Box box;
    if (!fragment_box(m, i, box) || !box.valid()) return false;
    for (int a = 0; a < 3; ++a)
        if (!std::isfinite(box.min[a]) || !std::isfinite(box.max[a])) return false;
    float center[3];
    for (int a = 0; a < 3; ++a) center[a] = (f.bounds.min[a] + f.bounds.max[a]) / 2;
    InstancePose placed;
    return instance_pose(f.pose, m.scale, center, placed);
}

struct Expected {
    InstancePlan plan;
    std::vector<double> diagonal;   // per grupp: det största fragmentets diagonal
};

// Planen räknad rakt på: grupper per post, kandidaterna i sparandets ordning,
// postfunktionen, taket.
Expected expected_plan(const Model& m, const InstancingSettings& s) {
    Expected e;
    std::map<uint32_t, std::vector<uint32_t>> usable;
    std::map<uint32_t, uint32_t> primitives;
    for (size_t i = 0; i < m.fragment_count; ++i) {
        HNwd::Fragment f;
        if (!instanceable(m, i, s, f)) continue;
        usable[f.record].push_back(static_cast<uint32_t>(i));
        primitives[f.record] = std::max(primitives[f.record], f.primitives);
    }
    struct Candidate {
        uint32_t record;
        uint64_t saving;
    };
    std::vector<Candidate> candidates;
    const size_t floor = std::max<size_t>(2, s.min_count);
    for (const auto& [record, list] : usable)
        if (list.size() >= floor)
            candidates.push_back({record, static_cast<uint64_t>(primitives[record]) * (list.size() - 1)});
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.saving != b.saving) return a.saving > b.saving;
        return a.record < b.record;
    });
    int64_t saved = 0;
    for (const Candidate& c : candidates) {
        if (e.plan.groups.size() >= s.max_groups) break;
        if (s.accept_record && !s.accept_record(c.record)) continue;
        InstanceGroup g;
        g.record = c.record;
        g.triangles = primitives[c.record];
        g.fragments = usable[c.record];
        double diagonal = 0;
        for (const uint32_t i : g.fragments) {
            Box b;
            fragment_box(m, i, b);
            diagonal = std::max(diagonal, b.diagonal());
        }
        e.diagonal.push_back(diagonal);
        e.plan.fragments += g.fragments.size();
        saved += static_cast<int64_t>(g.triangles) * static_cast<int64_t>(g.fragments.size() - 1) *
                     static_cast<int64_t>(kGpuBytesPerVertex + kIndexBytesPerTriangle) -
                 static_cast<int64_t>(kInstanceBytes) * static_cast<int64_t>(g.fragments.size());
        e.plan.groups.push_back(std::move(g));
    }
    e.plan.saved_bytes = saved > 0 ? static_cast<uint64_t>(saved) : 0;
    return e;
}

bool same_plan(const InstancePlan& a, const InstancePlan& b) {
    if (a.groups.size() != b.groups.size() || a.fragments != b.fragments ||
        a.saved_bytes != b.saved_bytes)
        return false;
    for (size_t i = 0; i < a.groups.size(); ++i) {
        const InstanceGroup &x = a.groups[i], &y = b.groups[i];
        if (x.record != y.record || x.triangles != y.triangles || x.max_size != y.max_size ||
            x.fragments != y.fragments)
            return false;
    }
    return true;
}

// Planen mot facit.
int check_against(const Model& m, const InstancingSettings& s, const char* what) {
    int fails = 0;
    InstancePlan plan;
    const bool done = plan_instancing(m, s, std::stop_token{}, plan);
    CHECK(done);
    const Expected e = expected_plan(m, s);
    if (plan.groups.size() != e.plan.groups.size())
        std::printf("  %s: %zu grupper, väntade %zu\n", what, plan.groups.size(),
                    e.plan.groups.size());
    CHECK(plan.groups.size() == e.plan.groups.size());
    CHECK(plan.fragments == e.plan.fragments);
    CHECK(plan.saved_bytes == e.plan.saved_bytes);
    uint64_t sum = 0;
    for (size_t g = 0; g < plan.groups.size() && g < e.plan.groups.size(); ++g) {
        const InstanceGroup &got = plan.groups[g], &want = e.plan.groups[g];
        CHECK(got.record == want.record);
        CHECK(got.triangles == want.triangles);
        CHECK(got.fragments == want.fragments);
        // Storleken är diagonalen, avrundad uppåt som float.
        CHECK(static_cast<double>(got.max_size) >= e.diagonal[g]);
        CHECK(static_cast<double>(got.max_size) <= e.diagonal[g] * (1 + 1e-6));
        CHECK(std::is_sorted(got.fragments.begin(), got.fragments.end()));
        sum += got.fragments.size();
        if (g > 0) {
            const InstanceGroup& before = plan.groups[g - 1];
            CHECK(static_cast<uint64_t>(before.triangles) * (before.fragments.size() - 1) >=
                  static_cast<uint64_t>(got.triangles) * (got.fragments.size() - 1));
        }
    }
    CHECK(sum == plan.fragments);
    return fails;
}

int check_synthetic() {
    int fails = 0;
    synth::Options o;
    o.fragments = 4000;
    o.records = 50;
    o.nodes = 1500;
    o.seed = 23;
    o.min_size = 0.05;
    o.max_size = 20;
    Fixture fx;
    if (!write_and_load(fx, "hnwd_test_instancing.nwd", o)) return fails + 1;
    const Model& model = *fx.loaded.model;
    CHECK(model.fragment_count == o.fragments);

    // Hur många fragment varje post har, och hur många av dem som går att
    // instansiera.
    std::map<uint32_t, size_t> shown;
    for (size_t i = 0; i < model.fragment_count; ++i) {
        HNwd::Fragment f;
        CHECK(model.doc->fragment(i, f));
        ++shown[f.record];
    }

    // Förvalen och trösklarna, och samma plan med en och fyra trådar.
    InstancingSettings base;
    base.min_count = 16;
    InstancePlan one, four;
    {
        InstancingSettings s = base;
        s.threads = 1;
        CHECK(plan_instancing(model, s, std::stop_token{}, one));
        s.threads = 4;
        CHECK(plan_instancing(model, s, std::stop_token{}, four));
        CHECK(same_plan(one, four));
        s.threads = 32;
        InstancePlan many;
        CHECK(plan_instancing(model, s, std::stop_token{}, many));
        CHECK(same_plan(one, many));
        s.threads = 0;   // klipps till en
        InstancePlan zero;
        CHECK(plan_instancing(model, s, std::stop_token{}, zero));
        CHECK(same_plan(one, zero));
    }
    CHECK(!one.groups.empty());
    fails += check_against(model, base, "förval");

    // Fragment med samma post som inte går att instansiera står utanför: i
    // filen finns genomskinliga utseenden.
    {
        bool outside = false;
        for (const InstanceGroup& g : one.groups)
            if (g.fragments.size() < shown[g.record]) outside = true;
        CHECK(outside);
        for (const InstanceGroup& g : one.groups) {
            HNwd::Fragment f;
            for (const uint32_t i : g.fragments) {
                CHECK(model.doc->fragment(i, f));
                CHECK(f.record == g.record);
                HNwd::Look look;
                if (model.doc->look(f.look, look)) CHECK(look.transparency < 0.01f);
            }
        }
    }

    // min_count: en tröskel över varje posts antal ger inga grupper, och en
    // tröskel på 1 gäller som 2.
    {
        InstancingSettings s = base;
        s.min_count = 1'000'000;
        InstancePlan none;
        CHECK(plan_instancing(model, s, std::stop_token{}, none));
        CHECK(none.groups.empty() && none.fragments == 0 && none.saved_bytes == 0);
        s.min_count = 1;
        InstancePlan low, two;
        CHECK(plan_instancing(model, s, std::stop_token{}, low));
        s.min_count = 2;
        CHECK(plan_instancing(model, s, std::stop_token{}, two));
        CHECK(same_plan(low, two));
        fails += check_against(model, s, "min_count 2");
        // En högre tröskel tar bort grupper och aldrig lägger till.
        s.min_count = 60;
        InstancePlan high;
        CHECK(plan_instancing(model, s, std::stop_token{}, high));
        CHECK(high.groups.size() <= one.groups.size());
        for (const InstanceGroup& g : high.groups) CHECK(g.fragments.size() >= 60);
        fails += check_against(model, s, "min_count 60");
    }

    // max_triangles: poster med fler primitiver tas inte med.
    {
        InstancingSettings s = base;
        s.max_triangles = 8;
        InstancePlan few;
        CHECK(plan_instancing(model, s, std::stop_token{}, few));
        for (const InstanceGroup& g : few.groups) CHECK(g.triangles <= 8);
        CHECK(few.groups.size() < one.groups.size());
        fails += check_against(model, s, "max_triangles 8");
        s.max_triangles = 0;
        InstancePlan zero;
        CHECK(plan_instancing(model, s, std::stop_token{}, zero));
        CHECK(zero.groups.empty());
    }

    // max_groups: de som sparar mest behålls.
    {
        CHECK(one.groups.size() >= 5);
        for (const size_t cap : {size_t(0), size_t(1), size_t(3), size_t(5)}) {
            InstancingSettings s = base;
            s.max_groups = cap;
            InstancePlan capped;
            CHECK(plan_instancing(model, s, std::stop_token{}, capped));
            CHECK(capped.groups.size() == std::min(cap, one.groups.size()));
            for (size_t g = 0; g < capped.groups.size() && g < one.groups.size(); ++g)
                CHECK(capped.groups[g].record == one.groups[g].record &&
                      capped.groups[g].fragments == one.groups[g].fragments);
            fails += check_against(model, s, "max_groups");
            // Varje grupp som föll bort sparar högst lika mycket som den minsta kvar.
            if (!capped.groups.empty() && capped.groups.size() < one.groups.size()) {
                const InstanceGroup& last = capped.groups.back();
                const uint64_t kept = static_cast<uint64_t>(last.triangles) * (last.fragments.size() - 1);
                for (size_t g = capped.groups.size(); g < one.groups.size(); ++g)
                    CHECK(static_cast<uint64_t>(one.groups[g].triangles) *
                              (one.groups[g].fragments.size() - 1) <= kept);
            }
        }
    }

    // accept_record: avvisade poster ersätts av nästa; funktionen anropas inte
    // för fler poster än kandidaterna.
    {
        InstancingSettings s = base;
        std::vector<uint32_t> asked;
        const uint32_t refused = one.groups[0].record;
        s.accept_record = [&](uint32_t record) {
            asked.push_back(record);
            return record != refused;
        };
        InstancePlan filtered;
        CHECK(plan_instancing(model, s, std::stop_token{}, filtered));
        CHECK(filtered.groups.size() + 1 == one.groups.size());
        for (const InstanceGroup& g : filtered.groups) CHECK(g.record != refused);
        CHECK(!asked.empty() && asked.front() == refused);
        CHECK(asked.size() <= one.groups.size());
        fails += check_against(model, s, "accept_record");
        // Med ett tak ersätter nästa kandidat den som avvisades.
        s.max_groups = 3;
        InstancePlan capped;
        CHECK(plan_instancing(model, s, std::stop_token{}, capped));
        CHECK(capped.groups.size() == 3);
        for (size_t g = 0; g < 3; ++g) CHECK(capped.groups[g].record == one.groups[g + 1].record);

        // Bara trianglar: poster som avkodas till linjer tas inte med.
        HNwd::GeometryReader reader;
        CHECK(reader.open(*model.doc) == HNwd::Status::Ok);
        HNwd::Mesh mesh;
        s = base;
        s.accept_record = [&](uint32_t record) {
            return reader.record_mesh(record, mesh) == HNwd::Status::Ok &&
                   mesh.kind == HNwd::MeshKind::Triangles;
        };
        InstancePlan triangles;
        CHECK(plan_instancing(model, s, std::stop_token{}, triangles));
        size_t lines = 0;
        for (const InstanceGroup& g : one.groups) {
            CHECK(reader.record_mesh(g.record, mesh) == HNwd::Status::Ok);
            if (mesh.kind != HNwd::MeshKind::Triangles) ++lines;
        }
        CHECK(lines > 0);
        CHECK(triangles.groups.size() + lines == one.groups.size());
        for (const InstanceGroup& g : triangles.groups) {
            CHECK(reader.record_mesh(g.record, mesh) == HNwd::Status::Ok);
            CHECK(mesh.kind == HNwd::MeshKind::Triangles);
        }
        fails += check_against(model, s, "bara trianglar");
    }

    // Avbrott: före planeringen och i en postfunktion.
    {
        std::stop_source source;
        source.request_stop();
        InstancePlan plan;
        plan.fragments = 99;
        CHECK(!plan_instancing(model, base, source.get_token(), plan));
        CHECK(plan.groups.empty() && plan.fragments == 0 && plan.saved_bytes == 0);

        std::stop_source later;
        InstancingSettings s = base;
        s.accept_record = [&](uint32_t) {
            later.request_stop();
            return true;
        };
        InstancePlan interrupted;
        CHECK(!plan_instancing(model, s, later.get_token(), interrupted));
        CHECK(interrupted.groups.empty() && interrupted.fragments == 0);
    }

    // Med planen som `skip` hamnar gruppernas fragment i ingen bit i trädet och
    // alla andra i en.
    {
        std::vector<uint8_t> skip(model.fragment_count, 0);
        for (const InstanceGroup& g : one.groups)
            for (const uint32_t i : g.fragments) skip[i] = 1;
        LodTreeSettings ts;
        ts.skip = &skip;
        ts.threads = 2;
        LodTree tree;
        LodTreeStats stats;
        CHECK(plan_lod_tree(model, ts, std::stop_token{}, tree, stats));
        size_t placed = 0;
        for (size_t i = 0; i < skip.size(); ++i) {
            if (skip[i]) {
                CHECK(tree.tile_of[i] == HNwd::kNone);
            } else if (tree.tile_of[i] != HNwd::kNone) {
                ++placed;
            }
        }
        CHECK(placed == tree.frags.size());
        CHECK(placed + one.fragments == model.fragment_count);
    }
    return fails;
}

// En modell utan fragment ger en tom plan.
int check_empty() {
    int fails = 0;
    Model empty;
    InstancePlan plan;
    plan.fragments = 5;
    plan.groups.resize(2);
    CHECK(plan_instancing(empty, InstancingSettings{}, std::stop_token{}, plan));
    CHECK(plan.groups.empty() && plan.fragments == 0 && plan.saved_bytes == 0);
    return fails;
}

// Exempelfilen: planen mot facit med några inställningar, och med en och tre
// trådar. Utseenden med bild och poser som inte går att instansiera ligger i
// facit lika väl som i planen.
int check_sample(const char* path) {
    int fails = 0;
    SKIP_IF_MISSING(path);
    const LoadResult loaded = load_model(path, {});
    CHECK(loaded.model != nullptr);
    if (!loaded.model) {
        std::printf("  load_model %s: %s\n", path, loaded.error.c_str());
        return fails;
    }
    const Model& model = *loaded.model;
    for (const uint32_t min_count : {uint32_t(4), uint32_t(16), uint32_t(64)}) {
        InstancingSettings s;
        s.min_count = min_count;
        s.threads = 1;
        fails += check_against(model, s, "exempelfil");
        InstancePlan one, three;
        CHECK(plan_instancing(model, s, std::stop_token{}, one));
        s.threads = 3;
        CHECK(plan_instancing(model, s, std::stop_token{}, three));
        CHECK(same_plan(one, three));
    }
    InstancingSettings s;
    s.min_count = 4;
    s.max_groups = 7;
    s.max_triangles = 500;
    s.threads = 2;
    fails += check_against(model, s, "exempelfil med tak");

    // Bara trianglar.
    HNwd::GeometryReader reader;
    CHECK(reader.open(*model.doc) == HNwd::Status::Ok);
    HNwd::Mesh mesh;
    s = InstancingSettings{};
    s.min_count = 4;
    s.accept_record = [&](uint32_t record) {
        return reader.record_mesh(record, mesh) == HNwd::Status::Ok &&
               mesh.kind == HNwd::MeshKind::Triangles;
    };
    fails += check_against(model, s, "exempelfil, trianglar");
    return fails;
}

// ---------------------------------------------------------------------------
// record_use: hinkarna mot en rak räkning.

int check_record_use(const Model& model) {
    int fails = 0;
    const RecordUse use = record_use(model, std::stop_token{});

    std::map<uint32_t, uint64_t> shown, placed;
    std::map<uint32_t, uint32_t> primitives;
    uint64_t total_placed = 0;
    for (size_t i = 0; i < model.fragment_count; ++i) {
        HNwd::Fragment f;
        CHECK(model.doc->fragment(i, f));
        ++shown[f.record];
        placed[f.record] += f.primitives;
        primitives[f.record] = f.primitives;
        total_placed += f.primitives;
    }
    std::vector<uint64_t> hist, once, per_fragment, fragments;
    uint64_t total_once = 0;
    for (const auto& [record, count] : shown) {
        size_t k = 0;
        while ((count >> (k + 1)) != 0) ++k;
        if (hist.size() <= k) {
            hist.resize(k + 1, 0);
            once.resize(k + 1, 0);
            per_fragment.resize(k + 1, 0);
            fragments.resize(k + 1, 0);
        }
        ++hist[k];
        once[k] += primitives[record];
        per_fragment[k] += placed[record];
        fragments[k] += count;
        total_once += primitives[record];
    }
    CHECK(use.records == shown.size());
    CHECK(use.triangles_once == total_once);
    CHECK(use.triangles_placed == total_placed);
    CHECK(use.hist == hist);
    CHECK(use.bucket_triangles_once == once);
    CHECK(use.bucket_triangles_placed == per_fragment);
    CHECK(use.bucket_fragments == fragments);
    CHECK(use.bucket_triangles_once.size() == use.hist.size());
    CHECK(use.bucket_triangles_placed.size() == use.hist.size());
    CHECK(use.bucket_fragments.size() == use.hist.size());
    CHECK(!use.hist.empty());

    uint64_t sum_fragments = 0;
    for (const uint64_t f : use.bucket_fragments) sum_fragments += f;
    CHECK(sum_fragments == model.fragment_count);

    // Det en gräns sparar: poster, fragment och primitiver ur hinkarna från k.
    for (size_t k = 0; k <= use.hist.size() + 1; ++k) {
        const RecordUseSaving saving = record_use_saving(use, k);
        uint64_t groups = 0, in_groups = 0, saved = 0;
        for (const auto& [record, count] : shown) {
            if (count < (uint64_t(1) << k)) continue;
            ++groups;
            in_groups += count;
            saved += placed[record] - primitives[record];
        }
        CHECK(saving.groups == groups);
        CHECK(saving.fragments == in_groups);
        CHECK(saving.saved_primitives == saved);
    }
    CHECK(record_use_saving(use, 0).saved_primitives == total_placed - total_once);
    CHECK(record_use_saving(use, use.hist.size()).groups == 0);
    CHECK(record_use_saving(RecordUse{}, 0).groups == 0);

    // Ett avbrott ger ett tomt svar.
    std::stop_source source;
    source.request_stop();
    const RecordUse stopped = record_use(model, source.get_token());
    CHECK(stopped.records == 0 && stopped.hist.empty() && stopped.bucket_fragments.empty());
    return fails;
}

int check_record_use_synthetic() {
    int fails = 0;
    synth::Options o;
    o.fragments = 3000;
    o.records = 40;
    o.nodes = 1000;
    o.seed = 5;
    Fixture fx;
    if (!write_and_load(fx, "hnwd_test_record_use.nwd", o)) return fails + 1;
    fails += check_record_use(*fx.loaded.model);
    return fails;
}

int check_record_use_sample(const char* path) {
    int fails = 0;
    SKIP_IF_MISSING(path);
    const LoadResult loaded = load_model(path, {});
    CHECK(loaded.model != nullptr);
    if (!loaded.model) return fails;
    fails += check_record_use(*loaded.model);
    return fails;
}

} // namespace

int test_instancing() {
    int fails = 0;
    fails += check_instance_pose();
    fails += check_empty();
    fails += check_synthetic();
    fails += check_record_use_synthetic();
    fails += check_sample(testdata::kSampleNwc[1]);
    fails += check_sample(testdata::kSampleNwc[5]);
    fails += check_sample(testdata::kSampleNwd);
    fails += check_record_use_sample(testdata::kSampleNwd);
    std::printf("test_instancing: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
