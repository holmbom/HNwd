#include "pick.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace nwdview {

namespace {

// Slack i barycentriska koordinater, så att en stråle genom en kant eller ett
// hörn inte halkar mellan två trianglar på grund av avrundningen.
constexpr double kEdgeSlack = 1e-9;

// Ett plan som strålen löper parallellt med har en determinant under det här,
// som del av kanternas kvadratsumma.
constexpr double kParallel = 1e-12;

// Strålen med normerad riktning. false när den inte går att följa: ett tal som
// inte är ändligt, eller en riktning utan längd.
struct Ray {
    double o[3];
    double d[3];
};

bool normalise(const PickRay& in, Ray& out) {
    double len2 = 0;
    for (int a = 0; a < 3; ++a) {
        if (!std::isfinite(in.origin[a]) || !std::isfinite(in.dir[a])) return false;
        out.o[a] = in.origin[a];
        len2 += in.dir[a] * in.dir[a];
    }
    const double len = std::sqrt(len2);
    if (!(len > 0) || !std::isfinite(len)) return false;
    for (int a = 0; a < 3; ++a) out.d[a] = in.dir[a] / len;
    return true;
}

double limit_of(double max_distance) {
    if (!(max_distance > 0) || !std::isfinite(max_distance))
        return std::numeric_limits<double>::infinity();
    return max_distance;
}

} // namespace

PickHit pick_geometry(const CellGeometry& g, const PickRay& pick_ray, const PickLimits& limits,
                      double max_distance) {
    PickHit result;
    Ray ray;
    if (!normalise(pick_ray, ray)) return result;
    const double reach = limit_of(max_distance);

    // Strålens början i delarnas ram.
    const double o[3] = {ray.o[0] - g.origin[0], ray.o[1] - g.origin[1], ray.o[2] - g.origin[2]};
    const double* d = ray.d;

    double best = std::numeric_limits<double>::infinity();
    size_t best_part = 0;
    uint32_t best_triangle = 0;

    for (size_t pi = 0; pi < g.parts.size(); ++pi) {
        const MeshPart& part = g.parts[pi];
        if (part.lines) continue;
        size_t count = part.triangle_count();
        if (pi < limits.drawn.size()) count = std::min<size_t>(count, limits.drawn[pi]);
        const size_t vertices = part.positions.size() / 3;
        const float* pos = part.positions.data();
        const uint32_t* idx = part.indices.data();

        for (size_t t = 0; t < count; ++t) {
            const uint32_t i0 = idx[3 * t], i1 = idx[3 * t + 1], i2 = idx[3 * t + 2];
            if (i0 >= vertices || i1 >= vertices || i2 >= vertices) continue;
            const float* p0 = pos + 3 * static_cast<size_t>(i0);
            const float* p1 = pos + 3 * static_cast<size_t>(i1);
            const float* p2 = pos + 3 * static_cast<size_t>(i2);

            const double e1[3] = {double(p1[0]) - p0[0], double(p1[1]) - p0[1], double(p1[2]) - p0[2]};
            const double e2[3] = {double(p2[0]) - p0[0], double(p2[1]) - p0[1], double(p2[2]) - p0[2]};
            // pvec = d × e2
            const double pv[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2],
                                  d[0] * e2[1] - d[1] * e2[0]};
            const double det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
            const double scale = e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2] + e2[0] * e2[0] +
                                 e2[1] * e2[1] + e2[2] * e2[2];
            if (!(std::fabs(det) > kParallel * scale)) continue;
            const double inv = 1.0 / det;

            const double tv[3] = {o[0] - p0[0], o[1] - p0[1], o[2] - p0[2]};
            const double u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * inv;
            if (u < -kEdgeSlack || u > 1 + kEdgeSlack) continue;
            // qvec = tvec × e1
            const double qv[3] = {tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2],
                                  tv[0] * e1[1] - tv[1] * e1[0]};
            const double v = (d[0] * qv[0] + d[1] * qv[1] + d[2] * qv[2]) * inv;
            if (v < -kEdgeSlack || u + v > 1 + kEdgeSlack) continue;
            const double dist = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * inv;
            // Strikt mindre: lika avstånd behåller den lägsta delen och triangeln.
            if (!(dist >= 0) || dist > reach || !(dist < best)) continue;

            if (limits.clip) {
                const double w[3] = {ray.o[0] + d[0] * dist, ray.o[1] + d[1] * dist,
                                     ray.o[2] + d[2] * dist};
                if (clipped_away(*limits.clip, w)) continue;
            }
            best = dist;
            best_part = pi;
            best_triangle = static_cast<uint32_t>(t);
        }
    }

    if (best == std::numeric_limits<double>::infinity()) return result;
    result.hit = true;
    result.distance = best;
    for (int a = 0; a < 3; ++a) result.point[a] = ray.o[a] + d[a] * best;
    result.fragment = fragment_of(g.parts[best_part].ranges, best_triangle);
    return result;
}

void tiles_along_ray(const LodTree& tree, const std::vector<uint32_t>& tiles, const PickRay& pick_ray,
                     double max_distance, std::vector<TileAlongRay>& out) {
    out.clear();
    Ray ray;
    if (!normalise(pick_ray, ray)) return;
    const double reach = limit_of(max_distance);

    for (const uint32_t ti : tiles) {
        if (ti >= tree.tiles.size()) continue;
        const Box& b = tree.tiles[ti].bounds;
        if (!b.valid()) continue;

        // Plattprovet: strålens intervall [t0, t1] innanför varje axels platta.
        double t0 = 0, t1 = std::numeric_limits<double>::infinity();
        bool miss = false;
        for (int a = 0; a < 3 && !miss; ++a) {
            if (ray.d[a] == 0) {
                // Parallell med plattan: inuti den (sluten) eller utanför för alltid.
                if (ray.o[a] < b.min[a] || ray.o[a] > b.max[a]) miss = true;
                continue;
            }
            double near_t = (b.min[a] - ray.o[a]) / ray.d[a];
            double far_t = (b.max[a] - ray.o[a]) / ray.d[a];
            if (near_t > far_t) std::swap(near_t, far_t);
            t0 = std::max(t0, near_t);
            t1 = std::min(t1, far_t);
            if (t0 > t1) miss = true;
        }
        // Ett intervall som svämmat över till oändligheten är ingen skärning.
        if (miss || !std::isfinite(t0) || t0 > reach) continue;
        out.push_back({ti, t0});
    }
    std::sort(out.begin(), out.end(), [](const TileAlongRay& a, const TileAlongRay& b) {
        if (a.enter != b.enter) return a.enter < b.enter;
        return a.tile < b.tile;
    });
}

} // namespace nwdview
