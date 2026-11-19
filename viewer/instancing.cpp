#include "instancing.h"

#include "cells.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <system_error>
#include <thread>
#include <utility>

namespace nwdview {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Hur långt två kolumner i posens matris får avvika från vinkelrätt (i
// cosinus) och ändå räknas som en vridning gånger en skala per axel.
constexpr double kOrthogonal = 1e-5;

// Under det här är pitch så nära ±90 grader att yaw och roll inte går att skilja
// åt; roll sätts då till 0.
constexpr double kGimbal = 1e-8;

constexpr unsigned kMaxThreads = 32;

// Hur ofta långa slingor frågar efter avbrott.
constexpr uint32_t kStopMask = 0x3FFF;

// Det en fragmentbild måste ha för att räknas som ogenomskinlig: samma gräns som
// bitarnas ritning.
constexpr float kOpaqueAlpha = 0.996f;

// Posens linjära del radvis, så som HNwd::world_point räknar den: s · R(q) för
// det normerade q (enhetsmatrisen om |q| är 0 eller inte ändlig), eller
// matrisen själv.
void linear_part(const HNwd::Pose& pose, double m[9]) {
    if (pose.affine) {
        for (int i = 0; i < 9; ++i) m[i] = pose.m[i];
        return;
    }
    const double x = pose.q[0], y = pose.q[1], z = pose.q[2], w = pose.q[3];
    const double n = std::sqrt(x * x + y * y + z * z + w * w);
    if (!(n > 0) || !std::isfinite(n)) {
        for (int i = 0; i < 9; ++i) m[i] = (i % 4 == 0) ? 1.0 : 0.0;
    } else {
        const double qx = x / n, qy = y / n, qz = z / n, qw = w / n;
        m[0] = 1 - 2 * (qy * qy + qz * qz);
        m[1] = 2 * (qx * qy - qw * qz);
        m[2] = 2 * (qx * qz + qw * qy);
        m[3] = 2 * (qx * qy + qw * qz);
        m[4] = 1 - 2 * (qx * qx + qz * qz);
        m[5] = 2 * (qy * qz - qw * qx);
        m[6] = 2 * (qx * qz - qw * qy);
        m[7] = 2 * (qy * qz + qw * qx);
        m[8] = 1 - 2 * (qx * qx + qy * qy);
    }
    for (int i = 0; i < 9; ++i) m[i] *= pose.s;
}

double dot3(const double a[3], const double b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

double length3(const double a[3]) { return std::sqrt(dot3(a, a)); }

// M = R · diag(S) med R en riktig vridning (radvis) och S med positiva första
// och andra skala; den tredje får tecknet som determinanten har.
bool decompose(const double m[9], double r[9], double s[3]) {
    double c[3][3];   // kolumnerna
    double len[3];
    for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 3; ++i) c[j][i] = m[3 * i + j];
        len[j] = length3(c[j]);
        if (!std::isfinite(len[j]) || !(len[j] > 0)) return false;
    }
    for (int a = 0; a < 3; ++a)
        for (int b = a + 1; b < 3; ++b)
            if (!(std::fabs(dot3(c[a], c[b])) <= kOrthogonal * len[a] * len[b])) return false;

    double u0[3], u1[3], u2[3];
    for (int i = 0; i < 3; ++i) u0[i] = c[0][i] / len[0];
    const double along = dot3(c[1], u0);
    double v1[3];
    for (int i = 0; i < 3; ++i) v1[i] = c[1][i] - along * u0[i];
    const double l1 = length3(v1);
    if (!std::isfinite(l1) || !(l1 > 0)) return false;
    for (int i = 0; i < 3; ++i) u1[i] = v1[i] / l1;
    u2[0] = u0[1] * u1[2] - u0[2] * u1[1];
    u2[1] = u0[2] * u1[0] - u0[0] * u1[2];
    u2[2] = u0[0] * u1[1] - u0[1] * u1[0];

    for (int i = 0; i < 3; ++i) {
        r[3 * i] = u0[i];
        r[3 * i + 1] = u1[i];
        r[3 * i + 2] = u2[i];
    }
    s[0] = len[0];
    s[1] = len[1];
    s[2] = dot3(c[2], u2) < 0 ? -len[2] : len[2];
    return true;
}

// Fragmentets storlek som float, avrundad uppåt så att den aldrig understiger
// den verkliga diagonalen.
float round_up(double d) {
    float f = static_cast<float>(d);
    if (static_cast<double>(f) < d) f = std::nextafter(f, std::numeric_limits<float>::infinity());
    return f;
}

bool finite_box(const Box& b) {
    if (!b.valid()) return false;
    for (int a = 0; a < 3; ++a)
        if (!std::isfinite(b.min[a]) || !std::isfinite(b.max[a])) return false;
    return std::isfinite(b.diagonal());
}

// Kör body(lo, hi) för `chunks` jämna följder av [0, n), en tråd vardera; den
// första i den här tråden. Kan en tråd inte startas körs dess följd här.
template <class F>
void run_chunks(size_t n, unsigned chunks, F&& body) {
    const auto edge = [n, chunks](unsigned t) { return n / chunks * t + std::min<size_t>(t, n % chunks); };
    std::vector<std::thread> pool;
    pool.reserve(chunks);
    for (unsigned t = 1; t < chunks; ++t) {
        try {
            pool.emplace_back([&body, &edge, t] { body(t, edge(t), edge(t + 1)); });
        } catch (const std::system_error&) {
            body(t, edge(t), edge(t + 1));
        }
    }
    body(0u, edge(0), edge(1));
    for (std::thread& th : pool) th.join();
}

// Postens fragmentlåda mitt i postens enhet.
void center_of(const HNwd::Fragment& f, float center[3]) {
    for (int a = 0; a < 3; ++a) center[a] = (f.bounds.min[a] + f.bounds.max[a]) / 2;
}

struct Candidate {
    uint32_t record = 0;
    uint32_t count = 0;
    uint32_t triangles = 0;
    uint64_t saving = 0;
};

} // namespace

bool instance_pose(const HNwd::Pose& pose, double metres_per_unit, const float center[3],
                   InstancePose& out) {
    if (!std::isfinite(metres_per_unit) || !(metres_per_unit > 0)) return false;
    double m[9];
    linear_part(pose, m);
    for (const double v : m)
        if (!std::isfinite(v)) return false;
    double r[9], s[3];
    if (!decompose(m, r, s)) return false;

    InstancePose result;
    for (int i = 0; i < 3; ++i) {
        const double at = metres_per_unit *
                          (m[3 * i] * center[0] + m[3 * i + 1] * center[1] + m[3 * i + 2] * center[2] +
                           pose.t[i]);
        if (!std::isfinite(at)) return false;
        result.at[i] = at;
        const float scale = static_cast<float>(s[i]);
        if (!std::isfinite(scale) || scale == 0.0f) return false;
        result.scale[i] = scale;
    }

    // R = Rz(−yaw) · Rx(pitch) · Ry(roll): r[7] = sin(pitch), r[1] = −sin(−yaw) ·
    // cos(pitch), r[4] = cos(−yaw) · cos(pitch), r[6] = −cos(pitch) · sin(roll),
    // r[8] = cos(pitch) · cos(roll).
    const double cos_pitch = std::hypot(r[1], r[4]);
    double yaw, roll;
    const double pitch = std::atan2(r[7], cos_pitch);
    if (cos_pitch > kGimbal) {
        yaw = std::atan2(r[1], r[4]);
        roll = std::atan2(-r[6], r[8]);
    } else {
        yaw = -std::atan2(r[3], r[0]);
        roll = 0;
    }
    constexpr double kDegrees = 180.0 / kPi;
    result.yaw = static_cast<float>(yaw * kDegrees);
    result.pitch = static_cast<float>(pitch * kDegrees);
    result.roll = static_cast<float>(roll * kDegrees);
    if (!std::isfinite(result.yaw) || !std::isfinite(result.pitch) || !std::isfinite(result.roll))
        return false;
    out = result;
    return true;
}

bool plan_instancing(const Model& m, const InstancingSettings& settings, std::stop_token stop,
                     InstancePlan& out) {
    out = InstancePlan{};
    const size_t n = m.fragment_count;
    if (n == 0 || !m.doc) return true;
    const HNwd::Document& doc = *m.doc;
    const size_t record_count = doc.record_count();
    if (record_count == 0) return true;
    const uint32_t min_count = std::max<uint32_t>(2, settings.min_count);
    const uint32_t max_triangles = settings.max_triangles;
    const unsigned threads = std::clamp(settings.threads, 1u, kMaxThreads);
    constexpr uint32_t kNone = HNwd::kNone;
    const auto cancelled = [&] {
        if (!stop.stop_requested()) return false;
        out = InstancePlan{};
        return true;
    };

    // Utseendena som går att instansiera: ogenomskinliga och utan bild.
    std::vector<uint8_t> look_ok(doc.look_count(), 1);
    for (size_t i = 0; i < look_ok.size(); ++i) {
        HNwd::Look look;
        if (!doc.look(i, look)) continue;
        const float alpha = std::clamp(1.0f - look.transparency, 0.0f, 1.0f);
        if (!(alpha >= kOpaqueAlpha)) {
            look_ok[i] = 0;
            continue;
        }
        HNwd::Material mat;
        if (look.material != kNone && doc.material(look.material, mat) && mat.has_texture)
            look_ok[i] = 0;
    }

    // Steg 1: fragmenten som kan vara med, och hur många av dem varje post har.
    // Räknarna är atomära, och en summa beror inte på ordningen.
    std::vector<uint8_t> eligible(n, 0);
    const auto counted = std::make_unique<std::atomic<uint32_t>[]>(record_count + 1);
    const auto prims = std::make_unique<std::atomic<uint32_t>[]>(record_count + 1);
    run_chunks(n, threads, [&](unsigned, size_t lo, size_t hi) {
        uint32_t counter = 0;
        for (size_t i = lo; i < hi; ++i) {
            if ((++counter & kStopMask) == 0 && stop.stop_requested()) return;
            HNwd::Fragment f;
            if (!doc.fragment(i, f)) continue;
            if (f.record == 0 || f.record > record_count) continue;
            if (f.primitives == 0 || f.primitives > max_triangles) continue;
            if (f.look < look_ok.size() && !look_ok[f.look]) continue;
            eligible[i] = 1;
            counted[f.record].fetch_add(1, std::memory_order_relaxed);
            uint32_t seen = prims[f.record].load(std::memory_order_relaxed);
            while (seen < f.primitives &&
                   !prims[f.record].compare_exchange_weak(seen, f.primitives,
                                                          std::memory_order_relaxed)) {
            }
        }
    });
    if (cancelled()) return false;

    // Steg 2: av posterna med nog många fragment, de som går att placera.
    const auto usable = std::make_unique<std::atomic<uint32_t>[]>(record_count + 1);
    run_chunks(n, threads, [&](unsigned, size_t lo, size_t hi) {
        uint32_t counter = 0;
        for (size_t i = lo; i < hi; ++i) {
            if ((++counter & kStopMask) == 0 && stop.stop_requested()) return;
            if (!eligible[i]) continue;
            HNwd::Fragment f;
            if (!doc.fragment(i, f) ||
                counted[f.record].load(std::memory_order_relaxed) < min_count) {
                eligible[i] = 0;
                continue;
            }
            Box box;
            float center[3];
            center_of(f, center);
            InstancePose placed;
            if (!fragment_box(m, i, box) || !finite_box(box) ||
                !instance_pose(f.pose, m.scale, center, placed)) {
                eligible[i] = 0;
                continue;
            }
            usable[f.record].fetch_add(1, std::memory_order_relaxed);
        }
    });
    if (cancelled()) return false;

    // Kandidaterna: de som sparar mest först.
    std::vector<Candidate> candidates;
    for (size_t r = 1; r <= record_count; ++r) {
        const uint32_t count = usable[r].load(std::memory_order_relaxed);
        if (count < min_count) continue;
        Candidate c;
        c.record = static_cast<uint32_t>(r);
        c.count = count;
        c.triangles = prims[r].load(std::memory_order_relaxed);
        c.saving = static_cast<uint64_t>(c.triangles) * (count - 1);
        candidates.push_back(c);
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.saving != b.saving) return a.saving > b.saving;
        return a.record < b.record;
    });
    std::vector<uint32_t> group_of(record_count + 1, kNone);
    std::vector<Candidate> chosen;
    for (const Candidate& c : candidates) {
        if (chosen.size() >= settings.max_groups) break;
        if (stop.stop_requested()) break;
        if (settings.accept_record && !settings.accept_record(c.record)) continue;
        group_of[c.record] = static_cast<uint32_t>(chosen.size());
        chosen.push_back(c);
    }
    if (cancelled()) return false;
    if (chosen.empty()) return true;

    // Steg 3: fragmenten i grupperna, och det största i varje. Varje följd
    // samlar sina egna listor, som läggs ihop i följdernas ordning.
    const size_t group_count = chosen.size();
    std::vector<std::vector<std::vector<uint32_t>>> lists(threads);
    std::vector<std::vector<float>> largest(threads);
    run_chunks(n, threads, [&](unsigned t, size_t lo, size_t hi) {
        lists[t].resize(group_count);
        largest[t].assign(group_count, 0.0f);
        uint32_t counter = 0;
        for (size_t i = lo; i < hi; ++i) {
            if ((++counter & kStopMask) == 0 && stop.stop_requested()) return;
            if (!eligible[i]) continue;
            HNwd::Fragment f;
            if (!doc.fragment(i, f)) continue;
            const uint32_t g = group_of[f.record];
            if (g == kNone) continue;
            Box box;
            if (!fragment_box(m, i, box)) continue;
            lists[t][g].push_back(static_cast<uint32_t>(i));
            largest[t][g] = std::max(largest[t][g], round_up(box.diagonal()));
        }
    });
    if (cancelled()) return false;

    out.groups.resize(group_count);
    int64_t saved = 0;
    for (size_t g = 0; g < group_count; ++g) {
        InstanceGroup& group = out.groups[g];
        group.record = chosen[g].record;
        group.triangles = chosen[g].triangles;
        size_t total = 0;
        for (unsigned t = 0; t < threads; ++t) total += lists[t][g].size();
        group.fragments.reserve(total);
        for (unsigned t = 0; t < threads; ++t) {
            group.fragments.insert(group.fragments.end(), lists[t][g].begin(), lists[t][g].end());
            std::vector<uint32_t>().swap(lists[t][g]);
            group.max_size = std::max(group.max_size, largest[t][g]);
        }
        const int64_t count = static_cast<int64_t>(group.fragments.size());
        out.fragments += group.fragments.size();
        saved += static_cast<int64_t>(group.triangles) * (count - 1) *
                     static_cast<int64_t>(kGpuBytesPerVertex + kIndexBytesPerTriangle) -
                 static_cast<int64_t>(kInstanceBytes) * count;
    }
    out.saved_bytes = saved > 0 ? static_cast<uint64_t>(saved) : 0;
    return true;
}

} // namespace nwdview
