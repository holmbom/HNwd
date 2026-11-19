#include "cells.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>
#include <unordered_map>

namespace nwdview {

uint32_t fragment_of(const std::vector<FragmentRange>& ranges, uint32_t triangle) {
    auto it = std::upper_bound(
        ranges.begin(), ranges.end(), triangle,
        [](uint32_t t, const FragmentRange& r) { return t < r.first_triangle; });
    if (it == ranges.begin()) return HNwd::kNone;
    return std::prev(it)->fragment;
}

uint32_t triangles_at_least(const std::vector<FragmentRange>& ranges, uint32_t triangles,
                            float min_size) {
    const auto first_small = std::partition_point(
        ranges.begin(), ranges.end(), [min_size](const FragmentRange& r) { return r.size >= min_size; });
    if (first_small == ranges.end()) return triangles;
    return std::min(first_small->first_triangle, triangles);
}

namespace {

// Största float som inte överstiger `v`.
float float_down(double v) {
    float f = static_cast<float>(v);
    if (static_cast<double>(f) > v) f = std::nextafter(f, 0.0f);
    return f;
}

constexpr int kMaxLodSteps = 16;

// En nivå som sparar mindre än så här av föregående nivås trianglar slås
// ihop med den.
constexpr double kMinSaving = 0.1;

// Marginalen på storleken som en nivå räknas ut med, så att avrundningen
// aldrig tar bort ett fragment som ska vara med.
constexpr double kSizeMargin = 1e-6;

// Största dynamiken mellan det största och minsta fragmentets räckvidd.
constexpr double kMaxReachRatio = 1e6;

} // namespace

std::vector<LodStep> lod_steps(const std::vector<FragmentRange>& ranges, uint32_t triangles,
                               double min_pixels, double pixels_per_radian, int max_steps) {
    std::vector<LodStep> steps;
    if (ranges.empty() || triangles == 0) return steps;
    float largest = 0, smallest = std::numeric_limits<float>::infinity();
    for (const FragmentRange& r : ranges) {
        largest = std::max(largest, r.size);
        smallest = std::min(smallest, r.size);
    }
    if (!(min_pixels > 0) || !(pixels_per_radian > 0) || !std::isfinite(min_pixels) ||
        !std::isfinite(pixels_per_radian) || !(largest > 0) || !std::isfinite(largest)) {
        steps.push_back({triangles, 0});
        return steps;
    }
    // Räckvidden: det avstånd där ett fragment med storleken s blir
    // min_pixels px är s · reach.
    const double reach = pixels_per_radian / min_pixels;
    const double reach_max = static_cast<double>(largest) * reach;
    double reach_min = std::max(static_cast<double>(smallest) * reach, reach_max / kMaxReachRatio);
    reach_min = std::min(reach_min, reach_max);
    const int count = std::clamp(max_steps, 1, kMaxLodSteps);

    // Avstånden: geometriskt från det minsta fragmentets räckvidd till det
    // största. Den första nivån har alla trianglar, och nivån efter avståndet
    // D har de fragment som når längre än D.
    std::vector<double> limit(static_cast<size_t>(count));
    const double ratio = reach_max / reach_min;
    for (int i = 0; i < count; ++i)
        limit[static_cast<size_t>(i)] = count == 1 || ratio <= 1.0 + 1e-9
                                            ? reach_max
                                            : reach_min * std::pow(ratio, static_cast<double>(i + 1) / count);
    limit.back() = reach_max;

    for (size_t i = 0; i < limit.size(); ++i) {
        if (!steps.empty() && limit[i] <= steps.back().max_distance) continue;
        const uint32_t n =
            i == 0 ? triangles
                   : triangles_at_least(ranges, triangles,
                                        float_down(limit[i - 1] / reach * (1.0 - kSizeMargin)));
        if (!steps.empty() &&
            static_cast<double>(n) > static_cast<double>(steps.back().triangles) * (1.0 - kMinSaving)) {
            steps.back().max_distance = limit[i];
            continue;
        }
        steps.push_back({n, limit[i]});
    }
    return steps;
}

bool ModelFilter::hidden(uint32_t fragment) const {
    auto it = std::upper_bound(first.begin(), first.end(), fragment);
    if (it == first.begin()) return false;
    const size_t k = static_cast<size_t>(std::prev(it) - first.begin());
    return fragment < end[k];
}

PartBytes part_bytes(const MeshPart& p) {
    const size_t vertices = p.positions.size() / 3;
    const size_t uv = p.uvs.empty() ? 0 : 8;
    const size_t index_bytes = p.indices.size() * 4;
    PartBytes b;
    if (p.lines) {
        b.ram = vertices * kRamBytesPerLineVertex + index_bytes +
                p.ranges.size() * sizeof(FragmentRange);
        b.gpu = vertices * kGpuBytesPerLineVertex + index_bytes;
        return b;
    }
    b.ram = vertices * (kRamBytesPerVertex + uv) + index_bytes +
            p.ranges.size() * sizeof(FragmentRange);
    b.gpu = vertices * (kGpuBytesPerVertex + uv) + index_bytes;
    return b;
}

PartBytes pick_bytes(uint64_t triangles) {
    PartBytes b;
    b.ram = static_cast<size_t>(triangles) * kPickBytesPerTriangle;
    return b;
}

PoseTransform::PoseTransform(const HNwd::Pose& pose) {
    for (int i = 0; i < 3; ++i) t_[i] = pose.t[i];
    if (pose.affine) {
        for (int i = 0; i < 9; ++i) linear_[i] = pose.m[i];
    } else {
        // s · R(q) för det normerade q; enhetsmatrisen om |q| är 0 eller inte
        // ändlig.
        const double x = pose.q[0], y = pose.q[1], z = pose.q[2], w = pose.q[3];
        const double n = std::sqrt(x * x + y * y + z * z + w * w);
        if (!(n > 0) || !std::isfinite(n)) {
            for (int i = 0; i < 9; ++i) linear_[i] = (i % 4 == 0) ? 1.0 : 0.0;
        } else {
            const double qx = x / n, qy = y / n, qz = z / n, qw = w / n;
            linear_[0] = 1 - 2 * (qy * qy + qz * qz);
            linear_[1] = 2 * (qx * qy - qw * qz);
            linear_[2] = 2 * (qx * qz + qw * qy);
            linear_[3] = 2 * (qx * qy + qw * qz);
            linear_[4] = 1 - 2 * (qx * qx + qz * qz);
            linear_[5] = 2 * (qy * qz - qw * qx);
            linear_[6] = 2 * (qx * qz - qw * qy);
            linear_[7] = 2 * (qy * qz + qw * qx);
            linear_[8] = 1 - 2 * (qx * qx + qy * qy);
        }
        for (int i = 0; i < 9; ++i) linear_[i] *= pose.s;
    }
    // En normal följer inte matrisen utan dess inverterade transponat, som är
    // kofaktormatrisen delad med determinanten; bara determinantens tecken
    // behövs, eftersom resultatet normeras.
    const double* m = linear_;
    const double c[9] = {m[4] * m[8] - m[5] * m[7], m[5] * m[6] - m[3] * m[8],
                         m[3] * m[7] - m[4] * m[6], m[2] * m[7] - m[1] * m[8],
                         m[0] * m[8] - m[2] * m[6], m[1] * m[6] - m[0] * m[7],
                         m[1] * m[5] - m[2] * m[4], m[2] * m[3] - m[0] * m[5],
                         m[0] * m[4] - m[1] * m[3]};
    const double determinant = m[0] * (m[4] * m[8] - m[5] * m[7]) -
                               m[1] * (m[3] * m[8] - m[5] * m[6]) +
                               m[2] * (m[3] * m[7] - m[4] * m[6]);
    const double sign = determinant < 0 ? -1.0 : 1.0;
    for (int i = 0; i < 9; ++i) cofactor_[i] = sign * c[i];
}

void PoseTransform::point(const float v[3], double out[3]) const {
    for (int r = 0; r < 3; ++r)
        out[r] = linear_[3 * r] * v[0] + linear_[3 * r + 1] * v[1] + linear_[3 * r + 2] * v[2] +
                 t_[r];
}

void PoseTransform::normal(const float n[3], float out[3]) const {
    double r[3];
    for (int i = 0; i < 3; ++i)
        r[i] = cofactor_[3 * i] * n[0] + cofactor_[3 * i + 1] * n[1] + cofactor_[3 * i + 2] * n[2];
    const double length = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (length > 0)
        for (double& v : r) v /= length;
    for (int i = 0; i < 3; ++i) out[i] = static_cast<float>(r[i]);
}

namespace {

uint32_t pack_rgba(float r, float g, float b, float a) {
    auto channel = [](float v) {
        return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return channel(r) << 24 | channel(g) << 16 | channel(b) << 8 | channel(a);
}

bool valid_utf8(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int extra = 0;
        if (c < 0x80) extra = 0;
        else if ((c & 0xE0) == 0xC0 && c >= 0xC2) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0 && c <= 0xF4) extra = 3;
        else return false;
        if (i + static_cast<size_t>(extra) >= s.size()) return false;
        for (int k = 1; k <= extra; ++k)
            if ((static_cast<unsigned char>(s[i + static_cast<size_t>(k)]) & 0xC0) != 0x80)
                return false;
        i += 1 + static_cast<size_t>(extra);
    }
    return true;
}

// Bildkoordinaten för ett hörn: UV ur posten genom materialets transform.
// Ordningen — verklig storlek, skala, vridning, förskjutning — är obelagd.
void texture_coord(const HNwd::Material& mat, UvMode mode, float u, float v, float out[2]) {
    double s = u, t = v;
    if (mode == UvMode::RealWorld && mat.real_scale_x > 0 && mat.real_scale_y > 0) {
        s /= mat.real_scale_x;
        t /= mat.real_scale_y;
    }
    s *= mat.u_scale;
    t *= mat.v_scale;
    if (mat.w_angle != 0) {
        const double a = mat.w_angle * 3.14159265358979323846 / 180.0;
        const double cs = std::cos(a), sn = std::sin(a);
        const double rs = cs * s - sn * t, rt = sn * s + cs * t;
        s = rs;
        t = rt;
    }
    out[0] = static_cast<float>(s + mat.u_offset);
    out[1] = static_cast<float>(t + mat.v_offset);
}

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

// Lägger tiden mellan konstruktionen och slutet på räckvidden till `total`,
// också när räckvidden lämnas med continue eller return.
class Lap {
public:
    explicit Lap(double& total) : total_(total), start_(Clock::now()) {}
    Lap(const Lap&) = delete;
    Lap& operator=(const Lap&) = delete;
    ~Lap() { total_ += seconds_since(start_); }

private:
    double& total_;
    Clock::time_point start_;
};

} // namespace

bool normals_are_surface(const std::vector<double>& world, const float* normals,
                         const std::vector<uint32_t>& indices, size_t vertices) {
    size_t counted = 0;
    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        const uint32_t corner[3] = {indices[t], indices[t + 1], indices[t + 2]};
        if (corner[0] >= vertices || corner[1] >= vertices || corner[2] >= vertices) continue;
        const double* a = &world[static_cast<size_t>(corner[0]) * 3];
        const double* b = &world[static_cast<size_t>(corner[1]) * 3];
        const double* c = &world[static_cast<size_t>(corner[2]) * 3];
        const double e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const double e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        double n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                       e1[0] * e2[1] - e1[1] * e2[0]};
        const double length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (!(length > 0) || !std::isfinite(length)) continue;
        for (double& v : n) v /= length;
        ++counted;
        for (const uint32_t k : corner) {
            const float* vn = &normals[static_cast<size_t>(k) * 3];
            const double d[3] = {vn[0] - n[0], vn[1] - n[1], vn[2] - n[2]};
            if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 1e-6) return false;
        }
    }
    return counted > 0;
}

namespace {

// Delarnas största reservation i hörn, så att ett enda jättefragment inte
// reserverar mer än det ryms.
constexpr uint64_t kMaxReservedPrimitives = uint64_t(1) << 22;

// Flest linjesegment i en bit: fyra hörn och sex index per segment ryms då i
// 32 bitar. Det som inte ryms räknas som överhoppat.
constexpr size_t kMaxLineSegments = size_t(1) << 28;

// Ett fragment som ska byggas: dess plats i listan (ordningen i delarna),
// diagonalen och posten.
struct Pick {
    uint32_t fragment;
    float size;
    HNwd::Fragment f;
};

// Var ett triangelfragments hörn och index ligger i de tillfälliga delarna.
struct Slot {
    bool triangles = false;
    uint32_t part = 0;
    size_t vertex_base = 0, vertices = 0, index_base = 0, indices = 0;
};

// Ett fragments linjesegment: sex tal per segment, ände A och sedan ände B,
// relativt delarnas origo i meter.
struct LinePiece {
    uint32_t order;
    std::vector<float> ends;
};

struct LabelPiece {
    uint32_t order;
    TextLabel label;
};

// Bygger geometrin för `count` fragment. Fragmenten läses i listans ordning,
// eller i postens ordning med by_record; delarnas trianglar läggs alltid i
// listans ordning. `sizes` ger fragmentens diagonal, eller är nullptr och
// diagonalen räknas ur lådan.
HNwd::Status build_ordered(HNwd::GeometryReader& reader, const Model& m,
                           const uint32_t* fragments, const float* sizes, size_t count,
                           const Box& bounds, bool by_record, const BuildLimits& limits,
                           std::stop_token stop, CellGeometry& out, std::string& error) {
    out = CellGeometry{};
    bounds.center(out.origin);
    out.diagonal = bounds.diagonal();
    const ModelFilter* filter = limits.filter && !limits.filter->empty() ? limits.filter.get()
                                                                         : nullptr;

    std::vector<Pick> picks;
    picks.reserve(count);
    uint64_t primitives = 0;
    for (size_t k = 0; k < count; ++k) {
        if ((k & 1023) == 0 && stop.stop_requested()) return HNwd::Status::Cancelled;
        const uint32_t i = fragments[k];
        if (filter && filter->hidden(i)) continue;
        Pick p{i, 0.0f, {}};
        if (!m.doc->fragment(i, p.f)) continue;
        if (sizes) {
            p.size = sizes[k];
        } else {
            Box b;
            p.size = fragment_box(m, i, b) ? static_cast<float>(b.diagonal()) : 0.0f;
        }
        if (p.size < limits.min_size) continue;
        primitives += p.f.primitives;
        picks.push_back(p);
    }

    // Läsordningen: geometripostens, så att fragment som delar post eller
    // ligger nära varandra i filen läses i följd.
    std::vector<uint32_t> read_order(picks.size());
    std::iota(read_order.begin(), read_order.end(), 0u);
    if (by_record)
        std::stable_sort(read_order.begin(), read_order.end(), [&picks](uint32_t a, uint32_t b) {
            return picks[a].f.record < picks[b].f.record;
        });

    // De tillfälliga delarna fylls i läsordning; [0] är den ogenomskinliga och
    // [1] den genomskinliga. Den ogenomskinliga reserveras för ett hörn per
    // primitiv.
    std::vector<MeshPart> temp(2);
    temp[1].transparent = true;
    {
        const size_t n = static_cast<size_t>(std::min(primitives, kMaxReservedPrimitives));
        MeshPart& opaque = temp[0];
        opaque.positions.reserve(3 * n);
        opaque.normals.reserve(3 * n);
        opaque.colors.reserve(n);
        opaque.indices.reserve(3 * n);
    }
    std::vector<Slot> slots(picks.size());
    std::vector<LinePiece> lines;
    std::vector<LabelPiece> labels;

    std::unordered_map<uint32_t, size_t> textured;   // material → del
    HNwd::Mesh mesh;
    std::vector<float> accumulated;
    std::vector<double> world;   // ett fragments hörn i världen, för jämförelsen med ytorna

    for (const uint32_t order : read_order) {
        if (stop.stop_requested()) return HNwd::Status::Cancelled;
        const Pick& p = picks[order];
        const Clock::time_point read_start = Clock::now();
        const HNwd::Status got = reader.fragment_mesh(p.fragment, mesh);
        out.read_seconds += seconds_since(read_start);
        const Lap lap(out.convert_seconds);
        if (got == HNwd::Status::Unsupported) {
            ++out.undecoded;
            continue;
        }
        if (got != HNwd::Status::Ok) {
            error = "fragment " + std::to_string(p.fragment) + ": " + reader.last_error();
            return got;
        }
        const size_t vertices = mesh.positions.size() / 3;
        if (mesh.kind == HNwd::MeshKind::Text) {
            // Textens låda i världen: höjden är lådans mellersta mått (den
            // längsta är raden, den kortaste tjockleken).
            Box w;
            for (int c = 0; c < 8; ++c) {
                const float corner[3] = {(c & 1) ? mesh.bounds.max[0] : mesh.bounds.min[0],
                                         (c & 2) ? mesh.bounds.max[1] : mesh.bounds.min[1],
                                         (c & 4) ? mesh.bounds.max[2] : mesh.bounds.min[2]};
                double pt[3];
                world_metres(m, p.f.pose, corner, pt);
                w.add_point(pt);
            }
            double dims[3] = {w.max[0] - w.min[0], w.max[1] - w.min[1], w.max[2] - w.min[2]};
            std::sort(dims, dims + 3);
            // Lådans kanter i postens egna axlar, mätta i världen: en text som
            // ligger snett i världen har en världslåda vars mått växer med
            // vridningen, men dess egna kanter gör det inte. Den mindre av de
            // två höjderna gäller.
            double edges[3];
            {
                double o[3];
                world_metres(m, p.f.pose, mesh.bounds.min, o);
                for (int a = 0; a < 3; ++a) {
                    float q[3] = {mesh.bounds.min[0], mesh.bounds.min[1], mesh.bounds.min[2]};
                    q[a] = mesh.bounds.max[a];
                    double e[3];
                    world_metres(m, p.f.pose, q, e);
                    edges[a] = std::sqrt((e[0] - o[0]) * (e[0] - o[0]) +
                                         (e[1] - o[1]) * (e[1] - o[1]) +
                                         (e[2] - o[2]) * (e[2] - o[2]));
                }
                std::sort(edges, edges + 3);
            }
            const double world_height = dims[1] > 0 ? dims[1] : dims[2];
            const double own_height = edges[1] > 0 ? edges[1] : edges[2];
            const double height = own_height > 0 ? std::min(world_height, own_height)
                                                 : world_height;
            if (!mesh.text.empty() && valid_utf8(mesh.text) && height > 0 && w.valid()) {
                TextLabel label;
                w.center(label.at);
                label.height = static_cast<float>(height);
                label.text = mesh.text;
                label.fragment = p.fragment;
                labels.push_back({order, std::move(label)});
            } else {
                ++out.texts;
            }
            continue;
        }
        if (mesh.kind == HNwd::MeshKind::Points) {
            out.points += vertices;
            continue;
        }
        if (mesh.kind == HNwd::MeshKind::Lines) {
            const size_t segs = mesh.indices.size() / 2;
            out.segments += segs;
            const PoseTransform pose(p.f.pose);
            // Änden i meter, relativt delarnas origo.
            auto end_point = [&](const float v[3], float rel[3]) {
                double w[3];
                pose.point(v, w);
                for (int a = 0; a < 3; ++a) rel[a] = static_cast<float>(w[a] * m.scale - out.origin[a]);
            };
            // Ett fragment sparar högst så många segment som taket tillåter;
            // taket för hela biten tillämpas när linjerna är ordnade.
            LinePiece piece{order, {}};
            size_t stored = 0;
            for (size_t k = 0; k < segs; ++k) {
                if (stored >= limits.max_segments) {
                    out.segments_skipped += segs - k;
                    break;
                }
                const uint32_t a = mesh.indices[2 * k], b = mesh.indices[2 * k + 1];
                if (a >= vertices || b >= vertices) continue;
                float ends[6];
                end_point(&mesh.positions[a * 3], ends);
                end_point(&mesh.positions[b * 3], ends + 3);
                piece.ends.insert(piece.ends.end(), ends, ends + 6);
                ++stored;
            }
            if (!piece.ends.empty()) lines.push_back(std::move(piece));
            ++out.fragments;
            continue;
        }

        // Trianglar.
        HNwd::Look look{};
        const bool has_look = m.doc->look(p.f.look, look);
        const float alpha = has_look ? std::clamp(1.0f - look.transparency, 0.0f, 1.0f) : 1.0f;
        bool transparent = alpha < 0.996f;
        const bool vertex_colors = mesh.colors.size() == vertices;
        if (vertex_colors)
            for (const uint32_t c : mesh.colors)
                if ((c & 0xFF) < 255) transparent = true;

        size_t part_index = transparent ? 1 : 0;
        HNwd::Material mat;
        bool is_textured = false;
        if (has_look && look.material != HNwd::kNone && mesh.uvs.size() == vertices * 2 &&
            limits.texture_usable && limits.texture_usable(look.material) &&
            m.doc->material(look.material, mat)) {
            is_textured = true;
            auto it = textured.find(look.material);
            if (it == textured.end()) {
                temp.emplace_back();
                MeshPart& tp = temp.back();
                tp.material = look.material;
                tp.base_color[0] = look.diffuse[0];
                tp.base_color[1] = look.diffuse[1];
                tp.base_color[2] = look.diffuse[2];
                tp.base_color[3] = alpha;
                tp.repeat_u = mat.u_repeat;
                tp.repeat_v = mat.v_repeat;
                it = textured.emplace(look.material, temp.size() - 1).first;
            }
            part_index = it->second;
            if (transparent) temp[part_index].transparent = true;
            if (mat.w_angle != 0) ++out.rotated_textures;
        }
        MeshPart& part = temp[part_index];

        const size_t base = part.positions.size() / 3;
        const uint32_t flat = has_look ? pack_rgba(look.diffuse[0], look.diffuse[1],
                                                   look.diffuse[2], alpha)
                                       : pack_rgba(0.8f, 0.8f, 0.8f, 1.0f);
        const uint32_t white = pack_rgba(1, 1, 1, alpha);
        const bool has_normals = mesh.normals.size() == mesh.positions.size();

        // Posens matriser räknas en gång för fragmentet; delens vektorer
        // växer med fragmentets hörn på en gång. Normalerna är noll i posten
        // utan dem tills ytorna ger dem nedan.
        const PoseTransform pose(p.f.pose);
        part.positions.resize((base + vertices) * 3);
        part.normals.resize((base + vertices) * 3, 0.0f);
        part.colors.resize(base + vertices);
        if (is_textured) part.uvs.resize((base + vertices) * 2);
        if (has_normals) world.resize(vertices * 3);
        float* const positions = part.positions.data() + base * 3;
        float* const normals = part.normals.data() + base * 3;
        for (size_t v = 0; v < vertices; ++v) {
            double w[3];
            pose.point(&mesh.positions[v * 3], w);
            for (int a = 0; a < 3; ++a) {
                w[a] *= m.scale;
                positions[v * 3 + static_cast<size_t>(a)] = static_cast<float>(w[a] - out.origin[a]);
            }
            if (has_normals) {
                for (int a = 0; a < 3; ++a) world[v * 3 + static_cast<size_t>(a)] = w[a];
                pose.normal(&mesh.normals[v * 3], &normals[v * 3]);
            }
            uint32_t color = is_textured ? white : flat;
            if (vertex_colors) {
                const uint32_t c = mesh.colors[v];
                const float a = static_cast<float>(c & 0xFF) / 255.0f * alpha;
                color = (c & 0xFFFFFF00u) | (static_cast<uint32_t>(a * 255.0f + 0.5f) & 0xFF);
            }
            part.colors[base + v] = color;
            if (is_textured) {
                float st[2];
                texture_coord(mat, limits.uv, mesh.uvs[v * 2], mesh.uvs[v * 2 + 1], st);
                part.uvs[(base + v) * 2] = st[0];
                part.uvs[(base + v) * 2 + 1] = st[1];
            }
        }
        const size_t index_base = part.indices.size();
        part.indices.resize(index_base + mesh.indices.size());
        for (size_t k = 0; k < mesh.indices.size(); ++k)
            part.indices[index_base + k] = static_cast<uint32_t>(base) + mesh.indices[k];
        if (has_normals && normals_are_surface(world, normals, mesh.indices, vertices))
            ++out.flat_fragments;

        // Utan normaler i posten: hörnens normal ur ytorna runt dem, viktade
        // med ytan.
        if (!has_normals) {
            accumulated.assign(vertices * 3, 0.0f);
            for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
                const uint32_t ia = mesh.indices[t], ib = mesh.indices[t + 1],
                               ic = mesh.indices[t + 2];
                if (ia >= vertices || ib >= vertices || ic >= vertices) continue;
                const float* pa = &part.positions[(base + ia) * 3];
                const float* pb = &part.positions[(base + ib) * 3];
                const float* pc = &part.positions[(base + ic) * 3];
                const float e1[3] = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
                const float e2[3] = {pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]};
                const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                                    e1[0] * e2[1] - e1[1] * e2[0]};
                for (const uint32_t i : {ia, ib, ic})
                    for (int a = 0; a < 3; ++a) accumulated[i * 3 + a] += n[a];
            }
            for (size_t v = 0; v < vertices; ++v) {
                float* n = &accumulated[v * 3];
                const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                float* dst = &part.normals[(base + v) * 3];
                if (len > 0) {
                    dst[0] = n[0] / len;
                    dst[1] = n[1] / len;
                    dst[2] = n[2] / len;
                } else {
                    dst[2] = 1;
                }
            }
        }

        out.triangles += mesh.indices.size() / 3;
        ++out.fragments;
        Slot& slot = slots[order];
        slot.triangles = true;
        slot.part = static_cast<uint32_t>(part_index);
        slot.vertex_base = base;
        slot.vertices = vertices;
        slot.index_base = index_base;
        slot.indices = mesh.indices.size();
    }

    // Delarna i listans ordning: varje fragments hörn och index flyttas ur den
    // tillfälliga delen till sin plats.
    const Lap assemble(out.convert_seconds);
    out.parts.resize(3);
    out.parts[1].transparent = true;
    out.parts[2].lines = true;
    std::vector<uint32_t> dest(temp.size(), HNwd::kNone);
    dest[0] = 0;
    dest[1] = 1;
    const auto reserve = [&](MeshPart& d, const MeshPart& t) {
        d.positions.reserve(t.positions.size());
        d.normals.reserve(t.normals.size());
        d.colors.reserve(t.colors.size());
        d.uvs.reserve(t.uvs.size());
        d.indices.reserve(t.indices.size());
    };
    reserve(out.parts[0], temp[0]);
    reserve(out.parts[1], temp[1]);
    for (size_t o = 0; o < picks.size(); ++o) {
        const Slot& slot = slots[o];
        if (!slot.triangles) continue;
        const MeshPart& t = temp[slot.part];
        if (dest[slot.part] == HNwd::kNone) {
            dest[slot.part] = static_cast<uint32_t>(out.parts.size());
            out.parts.emplace_back();
            MeshPart& d = out.parts.back();
            d.transparent = t.transparent;
            d.material = t.material;
            for (int c = 0; c < 4; ++c) d.base_color[c] = t.base_color[c];
            d.repeat_u = t.repeat_u;
            d.repeat_v = t.repeat_v;
            reserve(d, t);
        }
        MeshPart& d = out.parts[dest[slot.part]];
        const size_t base = d.positions.size() / 3;
        d.ranges.push_back({static_cast<uint32_t>(d.triangle_count()), picks[o].fragment,
                            picks[o].size});
        const auto vb = static_cast<std::ptrdiff_t>(slot.vertex_base);
        const auto vn = static_cast<std::ptrdiff_t>(slot.vertices);
        d.positions.insert(d.positions.end(), t.positions.begin() + 3 * vb,
                           t.positions.begin() + 3 * (vb + vn));
        d.normals.insert(d.normals.end(), t.normals.begin() + 3 * vb,
                         t.normals.begin() + 3 * (vb + vn));
        d.colors.insert(d.colors.end(), t.colors.begin() + vb, t.colors.begin() + vb + vn);
        if (!t.uvs.empty())
            d.uvs.insert(d.uvs.end(), t.uvs.begin() + 2 * vb, t.uvs.begin() + 2 * (vb + vn));
        const size_t shift = base - slot.vertex_base;
        for (size_t k = 0; k < slot.indices; ++k)
            d.indices.push_back(static_cast<uint32_t>(t.indices[slot.index_base + k] + shift));
    }

    // Linjerna och texterna i listans ordning, så att taken tar de största
    // fragmenten och släpper resten. Varje segment blir fyra hörn och två
    // trianglar (MeshPart).
    std::stable_sort(lines.begin(), lines.end(),
                     [](const LinePiece& a, const LinePiece& b) { return a.order < b.order; });
    {
        MeshPart& lp = out.parts[2];
        const size_t cap = std::min(limits.max_segments, kMaxLineSegments);
        size_t emitted = 0;
        for (const LinePiece& piece : lines) {
            const size_t segs = piece.ends.size() / 6;
            const size_t room = cap > emitted ? cap - emitted : 0;
            const size_t n = std::min(segs, room);
            out.segments_skipped += segs - n;
            if (n == 0) continue;
            emitted += n;
        }
        lp.positions.reserve(emitted * 12);
        lp.other.reserve(emitted * 12);
        lp.corner.reserve(emitted * 8);
        lp.colors.reserve(emitted * 4);
        lp.indices.reserve(emitted * 6);
        size_t at = 0;
        for (const LinePiece& piece : lines) {
            const size_t segs = piece.ends.size() / 6;
            const size_t n = std::min(segs, emitted - at);
            if (n == 0) continue;
            lp.ranges.push_back({static_cast<uint32_t>(2 * at), picks[piece.order].fragment,
                                 picks[piece.order].size});
            for (size_t s = 0; s < n; ++s) {
                const float* a = &piece.ends[6 * s];
                const float* b = a + 3;
                // Hörnen s·4 till s·4+3: (A, −1), (A, +1), (B, −1), (B, +1).
                for (int c = 0; c < 4; ++c) {
                    const float* here = c < 2 ? a : b;
                    const float* there = c < 2 ? b : a;
                    lp.positions.insert(lp.positions.end(), here, here + 3);
                    lp.other.insert(lp.other.end(), there, there + 3);
                    lp.corner.push_back((c & 1) ? 1.0f : -1.0f);
                    lp.corner.push_back(c < 2 ? 0.0f : 1.0f);
                    lp.colors.push_back(kLineColor);
                }
                const uint32_t v = static_cast<uint32_t>(4 * (at + s));
                const uint32_t tri[6] = {v, v + 1, v + 2, v + 2, v + 1, v + 3};
                lp.indices.insert(lp.indices.end(), tri, tri + 6);
            }
            at += n;
        }
    }
    for (const MeshPart& part : out.parts) out.vertices += part.positions.size() / 3;
    std::stable_sort(labels.begin(), labels.end(),
                     [](const LabelPiece& a, const LabelPiece& b) { return a.order < b.order; });
    for (LabelPiece& piece : labels) {
        if (out.labels.size() < limits.max_labels) out.labels.push_back(std::move(piece.label));
        else ++out.texts;
    }
    return HNwd::Status::Ok;
}

} // namespace

HNwd::Status build_fragments(HNwd::GeometryReader& reader, const Model& m,
                             const uint32_t* fragments, size_t count, const Box& bounds,
                             const BuildLimits& limits, std::stop_token stop,
                             CellGeometry& out, std::string& error) {
    return build_ordered(reader, m, fragments, nullptr, count, bounds, false, limits,
                         std::move(stop), out, error);
}

HNwd::Status build_tile(HNwd::GeometryReader& reader, const Model& m, const LodTree& tree,
                        uint32_t tile, const BuildLimits& limits, std::stop_token stop,
                        CellGeometry& out, std::string& error) {
    if (tile >= tree.tiles.size()) {
        error = "tile " + std::to_string(tile) + " is outside the tree";
        return HNwd::Status::OutOfRange;
    }
    const Tile& t = tree.tiles[tile];
    const size_t end = static_cast<size_t>(t.frag_begin) + t.frag_count;
    if (end > tree.frags.size() || end > tree.sizes.size()) {
        error = "tile " + std::to_string(tile) + " has fragments outside the tree";
        return HNwd::Status::OutOfRange;
    }
    // Bara prefixet av fragment som är minst min_size.
    const uint32_t count = limits.min_size > 0 ? tree.prefix_at_least(tile, limits.min_size).fragments
                                               : t.frag_count;
    return build_ordered(reader, m, tree.frags.data() + t.frag_begin,
                         tree.sizes.data() + t.frag_begin, count, t.bounds, true, limits,
                         std::move(stop), out, error);
}

} // namespace nwdview
