#include "model.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace nwdview {

void Box::add(const Box& b) {
    for (int a = 0; a < 3; ++a) {
        min[a] = std::min(min[a], b.min[a]);
        max[a] = std::max(max[a], b.max[a]);
    }
}

void Box::add_point(const double p[3]) {
    for (int a = 0; a < 3; ++a) {
        min[a] = std::min(min[a], p[a]);
        max[a] = std::max(max[a], p[a]);
    }
}

double Box::diagonal() const {
    if (!valid()) return 0;
    const double dx = max[0] - min[0], dy = max[1] - min[1], dz = max[2] - min[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void Box::center(double out[3]) const {
    for (int a = 0; a < 3; ++a) out[a] = (min[a] + max[a]) / 2;
}

std::string format_number(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    return buf;
}

namespace {

Box to_metres(const HNwd::BoxD& b, double scale) {
    Box out;
    for (int a = 0; a < 3; ++a) {
        out.min[a] = b.min[a] * scale;
        out.max[a] = b.max[a] * scale;
    }
    return out;
}

} // namespace

std::string unit_text(const HNwd::Unit& u, HNwd::Status status, const std::string& why) {
    switch (u.unit) {
    case HNwd::LengthUnit::Metre: return "enhet meter";
    case HNwd::LengthUnit::Millimetre: return "enhet millimeter";
    case HNwd::LengthUnit::Centimetre: return "enhet centimeter";
    case HNwd::LengthUnit::Foot: return "enhet fot (0,3048 m)";
    case HNwd::LengthUnit::Unknown: break;
    }
    if (status == HNwd::Status::Missing) return "enhet okänd (filen anger ingen)";
    if (u.code >= 0) return "enhet okänd (kod " + std::to_string(u.code) + ")";
    return "enhet okänd (" + why + ")";
}

namespace {

// Modellens huvuddel: per axel lägena mellan den lägsta och den högsta
// tusendelen av fragmentens mittpunkter, med marginal. Enstaka delar långt
// bort — en delmodell som står kvar i origo — gör då inte resten till en
// prick. Fler än en miljon fragment tas med ett jämnt steg.
Box robust_view(const Model& m) {
    const size_t count = m.fragment_count;
    if (count < 1000) return m.world;
    const size_t stride = std::max<size_t>(1, count / 1'000'000);
    std::vector<double> centers[3];
    for (auto& c : centers) c.reserve(count / stride + 1);
    for (size_t i = 0; i < count; i += stride) {
        Box b;
        if (!fragment_box(m, i, b)) continue;
        double c[3];
        b.center(c);
        for (int a = 0; a < 3; ++a) centers[a].push_back(c[a]);
    }
    const size_t n = centers[0].size();
    if (n < 1000) return m.world;
    Box view;
    const size_t cut = n / 1000;
    for (int a = 0; a < 3; ++a) {
        std::vector<double>& v = centers[a];
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(cut), v.end());
        double lo = v[cut];
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(n - 1 - cut), v.end());
        double hi = v[n - 1 - cut];
        const double margin = (hi - lo) * 0.05;
        lo -= margin;
        hi += margin;
        view.min[a] = std::max(m.world.min[a], lo);
        view.max[a] = std::min(m.world.max[a], hi);
    }
    return view.valid() ? view : m.world;
}

// Vyerna och rutnäten. Ett paket som inte går att läsa stoppar inte
// inläsningen: felet står i extras_error, och modellen visas utan det.
void read_views_and_grids(HNwd::Document& doc, Model& m) {
    auto note = [&](const char* what) {
        if (!m.extras_error.empty()) m.extras_error += "; ";
        m.extras_error += std::string(what) + ": " + doc.last_error();
    };
    if (doc.load_views() == HNwd::Status::Ok) {
        m.has_current_view = doc.current_view(m.current_view);
        m.has_home_view = doc.home_view(m.home_view);
        m.saved_views.resize(doc.saved_view_count());
        for (size_t i = 0; i < m.saved_views.size(); ++i) {
            HNwd::SavedView v;
            doc.saved_view(i, v);
            SavedViewInfo& s = m.saved_views[i];
            s.name = v.name;
            s.folder = v.kind == HNwd::SavedViewKind::Folder;
            s.parent = v.parent;
            s.camera = v.camera;
        }
    } else {
        note("vyerna");
    }
    if (doc.load_grids() == HNwd::Status::Ok) {
        for (size_t i = 0; i < doc.grid_count(); ++i) {
            HNwd::GridSystem g;
            doc.grid(i, g);
            for (uint32_t k = 0; k < g.line_count; ++k) {
                HNwd::GridLine l;
                doc.grid_line(i, k, l);
                GridLineInfo info;
                info.label = l.label;
                for (int a = 0; a < 3; ++a) {
                    info.a[a] = l.world[0][a] * m.scale;
                    info.b[a] = l.world[1][a] * m.scale;
                }
                m.grid_lines.push_back(std::move(info));
            }
            for (uint32_t k = 0; k < g.level_count; ++k) {
                HNwd::GridLevel l;
                doc.grid_level(i, k, l);
                m.grid_levels.push_back({std::string(g.name), std::string(l.name), l.elevation * m.scale});
            }
        }
    } else {
        note("rutnäten");
    }
}

} // namespace

bool orbit_from_camera(const Model& m, const HNwd::Camera& camera, OrbitPose& out) {
    if (!m.view.valid()) return false;
    double eye[3], f[3];
    HNwd::camera_forward(camera, f);
    const double len = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (!(len > 0)) return false;
    for (int a = 0; a < 3; ++a) {
        eye[a] = camera.position[a] * m.scale;
        f[a] /= len;
    }
    double c[3];
    m.view.center(c);
    const double to[3] = {c[0] - eye[0], c[1] - eye[1], c[2] - eye[2]};
    const double t = to[0] * f[0] + to[1] * f[1] + to[2] * f[2];
    if (!(t > 0)) return false;
    // Mittens avstånd från blicklinjen, mot konen med visarens synfält (45°)
    // vid mitten, vidgad med lådans halva diagonal.
    const double perp[3] = {to[0] - t * f[0], to[1] - t * f[1], to[2] - t * f[2]};
    const double off = std::sqrt(perp[0] * perp[0] + perp[1] * perp[1] + perp[2] * perp[2]);
    if (off > t * std::tan(22.5 * 3.14159265358979323846 / 180) + m.view.diagonal() / 2)
        return false;

    for (int a = 0; a < 3; ++a) out.target[a] = eye[a] + t * f[a];
    out.distance = t;
    constexpr double kDeg = 180 / 3.14159265358979323846;
    out.elevation = std::asin(std::clamp(-f[2], -1.0, 1.0)) * kDeg;
    // Kompassriktningen ur blickens vågräta del. Rakt upp eller ner har den
    // ingen, och bildens upp tas då ur kvaternionen: R · (0, 1, 0). Att det
    // är kamerans upp i filen är inte belagt; det ger en norrvänd bild för
    // identiteten.
    double hx = f[0], hy = f[1];
    if (std::hypot(hx, hy) < 1e-9) {
        const double x = camera.rotation[0], y = camera.rotation[1], z = camera.rotation[2],
                     w = camera.rotation[3];
        hx = 2 * (x * y - z * w);
        hy = 1 - 2 * (x * x + z * z);
        if (f[2] > 0) {
            hx = -hx;
            hy = -hy;
        }
    }
    out.azimuth = std::atan2(hx, hy) * kDeg;
    return true;
}

bool add_clip_plane(ClipState& clip, const double point[3], const double normal[3]) {
    if (clip.count >= static_cast<int>(HNwd::kClipPlaneCount)) return false;
    const double len = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
    if (!(len > 0) || !std::isfinite(len)) return false;
    double* p = clip.plane[clip.count];
    for (int a = 0; a < 3; ++a) p[a] = normal[a] / len;
    p[3] = -(p[0] * point[0] + p[1] * point[1] + p[2] * point[2]);
    if (!std::isfinite(p[3])) return false;
    ++clip.count;
    return true;
}

bool clipped_away(const ClipState& clip, const double x[3]) {
    for (int k = 0; k < clip.count; ++k) {
        const double* p = clip.plane[k];
        if (p[0] * x[0] + p[1] * x[1] + p[2] * x[2] + p[3] < 0) return true;
    }
    return false;
}

void clip_from_camera(const Model& m, const HNwd::Camera& camera, ClipState& clip,
                      std::string& error) {
    clip = ClipState{};
    error.clear();
    // De påslagna planen i meter.
    struct Plane {
        double point[3];
        double normal[3];
    };
    std::vector<Plane> on;
    for (const HNwd::ClipPlane& c : camera.clip) {
        if (!c.enabled) continue;
        Plane p;
        for (int a = 0; a < 3; ++a) {
            p.point[a] = c.point[a] * m.scale;
            p.normal[a] = c.normal[a];
        }
        on.push_back(p);
    }
    if (on.empty()) return;
    auto dot = [](const double a[3], const double b[3]) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    // +1: normalen pekar mot det som behålls; −1: bort från det.
    int side = 0;
    for (size_t i = 0; i < on.size(); ++i)
        for (size_t j = i + 1; j < on.size(); ++j) {
            if (dot(on[i].normal, on[j].normal) > -(1 - 1e-9)) continue;
            const double d[3] = {on[j].point[0] - on[i].point[0], on[j].point[1] - on[i].point[1],
                                 on[j].point[2] - on[i].point[2]};
            const double s = dot(d, on[i].normal);
            // Plan i samma läge säger inget om vilken sida som behålls.
            if (std::fabs(s) <= 1e-9) continue;
            // Normalerna pekar mot varandra när plan j ligger på normalsidan av plan i.
            const int pair = s > 0 ? 1 : -1;
            if (side != 0 && pair != side) {
                error = "snittplanens par säger olika om vilken sida som behålls; snittet visas inte";
                return;
            }
            side = pair;
        }
    if (side == 0) {
        double eye[3];
        for (int a = 0; a < 3; ++a) eye[a] = camera.position[a] * m.scale - on[0].point[a];
        side = dot(eye, on[0].normal) < 0 ? -1 : 1;
    }
    for (const Plane& p : on) {
        const double n[3] = {side * p.normal[0], side * p.normal[1], side * p.normal[2]};
        if (!add_clip_plane(clip, p.point, n)) {
            clip = ClipState{};
            error = "ett snittplan har ingen normal; snittet visas inte";
            return;
        }
    }
}

bool fragment_box(const Model& m, size_t fragment, Box& out) {
    HNwd::BoxD b;
    if (!m.doc->fragment_world_box(fragment, b)) return false;
    out = to_metres(b, m.scale);
    return true;
}

void world_metres(const Model& m, const HNwd::Pose& pose, const float v[3], double out[3]) {
    HNwd::world_point(pose, v, out);
    out[0] *= m.scale;
    out[1] *= m.scale;
    out[2] *= m.scale;
}

LoadResult load_model(const std::string& path, const LoadProgress& progress) {
    LoadResult r;
    auto doc = std::make_shared<HNwd::Document>();
    auto fail = [&](HNwd::Status s, const char* what) {
        r.status = s;
        r.error = s == HNwd::Status::Cancelled ? std::string("avbruten")
                                               : std::string(what) + ": " + doc->last_error();
        return r;
    };
    auto step = [&](const char* name) {
        return HNwd::Progress([&progress, name](uint64_t done, uint64_t total) {
            return !progress || progress(name, done, total);
        });
    };

    if (progress && !progress("Öppnar", 0, 0)) return fail(HNwd::Status::Cancelled, "");
    if (const HNwd::Status s = doc->open(path.c_str()); s != HNwd::Status::Ok)
        return fail(s, "Filen kunde inte öppnas");

    auto model = std::make_shared<Model>();
    model->path = path;
    {
        const HNwd::Status s = doc->length_unit(model->unit);
        model->unit_known = s == HNwd::Status::Ok && model->unit.metres_per_unit > 0;
        model->scale = model->unit_known ? model->unit.metres_per_unit : 1.0;
        model->unit_text = unit_text(model->unit, s, doc->last_error());
    }

    if (const HNwd::Status s = doc->load_geometry(step("Geometrins förteckning"));
        s != HNwd::Status::Ok)
        return fail(s, "Geometrin kunde inte läsas");
    if (const HNwd::Status s = doc->load_fragments(step("Fragment")); s != HNwd::Status::Ok)
        return fail(s, "Placeringen kunde inte läsas");
    if (const HNwd::Status s = doc->load_spatial(step("Rumsligt index")); s != HNwd::Status::Ok)
        return fail(s, "Det rumsliga indexet kunde inte läsas");

    model->fragment_count = doc->fragment_count();
    model->spatial_from_file = doc->spatial_from_file();
    model->items = doc->spatial_items();
    model->memory_bytes = doc->memory_bytes();
    model->normal_bits = doc->normal_bits();

    const size_t node_count = doc->spatial_node_count();
    model->nodes.resize(node_count);
    for (uint32_t i = 0; i < node_count; ++i) {
        if (progress && (i & 0xFFFF) == 0 && !progress("Lådor", i, node_count))
            return fail(HNwd::Status::Cancelled, "");
        HNwd::SpatialNode n;
        doc->spatial_node(i, n);
        SpatialCell& c = model->nodes[i];
        c.parent = n.parent;
        c.child_count = n.child_count;
        c.first_child = static_cast<uint32_t>(model->children.size());
        c.item_begin = n.item_begin;
        c.item_count = n.item_count;
        c.primitives = n.primitives;
        c.bounds = to_metres(n.bounds, model->scale);
        for (uint32_t k = 0; k < n.child_count; ++k)
            model->children.push_back(doc->spatial_child(i, k));
    }
    if (progress && !progress("Vyer och rutnät", 0, 0)) return fail(HNwd::Status::Cancelled, "");
    read_views_and_grids(*doc, *model);
    model->memory_bytes = doc->memory_bytes();
    model->doc = doc;
    model->tree_doc = doc;
    if (!model->nodes.empty() && model->fragment_count > 0) model->world = model->nodes[0].bounds;
    if (!model->world.valid()) {
        // En modell utan fragment: en meterlåda kring origo, så att kameran
        // har något att stå på.
        model->world.min[0] = model->world.min[1] = model->world.min[2] = 0;
        model->world.max[0] = model->world.max[1] = model->world.max[2] = 1;
    }
    if (progress && !progress("Vyn", 0, 0)) return fail(HNwd::Status::Cancelled, "");
    model->view = robust_view(*model);
    r.model = std::move(model);
    return r;
}

TreeResult load_tree(const Model& model, const LoadProgress& progress) {
    TreeResult r;
    HNwd::Document& doc = *model.tree_doc;
    const HNwd::Status s = doc.load_tree(
        HNwd::TreeContent::Structure, [&progress](uint64_t done, uint64_t total) {
            return !progress || progress("Trädet", done, total);
        });
    // last_error() och memory_bytes() läses i den här tråden, som körde
    // load_tree() (trådkontraktet i HNwd.h).
    if (s != HNwd::Status::Ok) {
        r.status = s;
        r.error = s == HNwd::Status::Cancelled ? std::string("avbrutet") : doc.last_error();
        return r;
    }
    for (size_t i = 0; i < doc.model_count(); ++i) {
        HNwd::Model hm;
        if (!doc.model(i, hm)) continue;
        // Listan, filtret och klicket räknar med modellerna på översta nivån:
        // en nästlad modells fragment ligger i förälderns intervall.
        if (hm.parent != HNwd::kNone) continue;
        ModelInfo info;
        info.name = hm.name.empty() ? "modell " + std::to_string(r.models.size() + 1)
                                    : std::string(hm.name);
        info.unit_text = unit_text(hm.unit, hm.unit.unit == HNwd::LengthUnit::Unknown
                                                ? HNwd::Status::Unsupported
                                                : HNwd::Status::Ok,
                                   "okänd");
        info.node = hm.node;
        info.first_fragment = hm.first_fragment;
        info.fragment_count = hm.fragment_count;
        r.models.push_back(std::move(info));
    }
    r.memory_bytes = doc.memory_bytes();
    r.loaded = true;
    return r;
}

namespace {

std::string guid_text(const uint8_t g[16]) {
    // Windows-ordning: de tre första fälten är little-endian.
    char buf[40];
    std::snprintf(buf, sizeof buf,
                  "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                  g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6], g[8], g[9], g[10], g[11],
                  g[12], g[13], g[14], g[15]);
    return buf;
}

std::string triple(const double v[3]) {
    char buf[128];
    std::snprintf(buf, sizeof buf, "%.3f  %.3f  %.3f", v[0], v[1], v[2]);
    return buf;
}

std::string value_text(const HNwd::Property& prop) {
    switch (prop.type) {
    case HNwd::ValueType::None: return {};
    case HNwd::ValueType::Double: return format_number(prop.d[0]);
    case HNwd::ValueType::Int:
    case HNwd::ValueType::Time: return std::to_string(prop.i);
    case HNwd::ValueType::String:
    case HNwd::ValueType::Name: return std::string(prop.text);
    case HNwd::ValueType::Point3:
        return format_number(prop.d[0]) + ", " + format_number(prop.d[1]) + ", " +
               format_number(prop.d[2]);
    case HNwd::ValueType::Point2:
        return format_number(prop.d[0]) + ", " + format_number(prop.d[1]);
    }
    return {};
}

} // namespace

Box object_box(const Model& m, size_t fragment) {
    Box box;
    HNwd::Fragment f;
    if (!m.doc->fragment(fragment, f)) return box;
    const uint32_t seq = f.seq;
    // Ett objekt med mycket många fragment ramas in av de närmaste; lådan
    // är en markering, inte ett mått.
    constexpr size_t kMaxSpan = 100'000;
    size_t lo = fragment, hi = fragment;
    HNwd::Fragment g;
    while (lo > 0 && fragment - lo < kMaxSpan && m.doc->fragment(lo - 1, g) && g.seq == seq) --lo;
    while (hi + 1 < m.fragment_count && hi - fragment < kMaxSpan && m.doc->fragment(hi + 1, g) &&
           g.seq == seq)
        ++hi;
    for (size_t i = lo; i <= hi; ++i) {
        Box b;
        if (fragment_box(m, i, b)) box.add(b);
    }
    return box;
}

RecordUse record_use(const Model& m, std::stop_token stop) {
    RecordUse use;
    const size_t record_count = m.doc->record_count();
    // Per post, 1-baserat: fragment som visar den och dess primitiver.
    std::vector<uint32_t> shown(record_count + 1, 0), primitives(record_count + 1, 0);
    std::vector<uint64_t> placed(record_count + 1, 0);   // per post: primitiver, en gång per fragment
    for (size_t i = 0; i < m.fragment_count; ++i) {
        if ((i & 0xFFFF) == 0 && stop.stop_requested()) return {};
        HNwd::Fragment f;
        // load_fragments() har prövat att varje fragment pekar på en post som
        // finns.
        if (!m.doc->fragment(i, f) || f.record == 0 || f.record > record_count) continue;
        ++shown[f.record];
        primitives[f.record] = f.primitives;
        placed[f.record] += f.primitives;
        use.triangles_placed += f.primitives;
    }
    for (size_t r = 1; r <= record_count; ++r) {
        if (shown[r] == 0) continue;
        ++use.records;
        use.triangles_once += primitives[r];
        size_t k = 0;
        while ((static_cast<uint64_t>(shown[r]) >> (k + 1)) != 0) ++k;   // 2^k <= shown < 2^(k+1)
        if (use.hist.size() <= k) {
            use.hist.resize(k + 1, 0);
            use.bucket_triangles_once.resize(k + 1, 0);
            use.bucket_triangles_placed.resize(k + 1, 0);
            use.bucket_fragments.resize(k + 1, 0);
        }
        ++use.hist[k];
        use.bucket_triangles_once[k] += primitives[r];
        use.bucket_triangles_placed[k] += placed[r];
        use.bucket_fragments[k] += shown[r];
    }
    return use;
}

RecordUseSaving record_use_saving(const RecordUse& use, size_t first_bucket) {
    RecordUseSaving saving;
    for (size_t k = first_bucket; k < use.hist.size(); ++k) {
        saving.groups += use.hist[k];
        if (k < use.bucket_fragments.size()) saving.fragments += use.bucket_fragments[k];
        if (k < use.bucket_triangles_placed.size() && k < use.bucket_triangles_once.size())
            saving.saved_primitives +=
                use.bucket_triangles_placed[k] - use.bucket_triangles_once[k];
    }
    return saving;
}

std::vector<PropertyRow> describe_geometry(const Model& m, size_t fragment) {
    std::vector<PropertyRow> rows;
    auto category = [&](std::string name) { rows.push_back({std::move(name), {}, true}); };
    auto row = [&](std::string name, std::string value) {
        rows.push_back({std::move(name), std::move(value), false});
    };

    HNwd::Fragment f;
    if (!m.doc->fragment(fragment, f)) return rows;
    Box box;
    fragment_box(m, fragment, box);
    double center[3];
    box.center(center);
    const double size[3] = {box.max[0] - box.min[0], box.max[1] - box.min[1],
                            box.max[2] - box.min[2]};

    category("Geometri");
    row("Fragment", std::to_string(fragment));
    row("Geometripost", std::to_string(f.record));
    row("Trianglar eller segment", std::to_string(f.primitives));
    row("Storlek (m)", triple(size));
    row("Mitt (m)", triple(center));

    category("Utseende");
    HNwd::Look look;
    if (m.doc->look(f.look, look)) {
        char rgb[64];
        std::snprintf(rgb, sizeof rgb, "%.3f, %.3f, %.3f", look.diffuse[0], look.diffuse[1],
                      look.diffuse[2]);
        row("Utseende", std::to_string(f.look));
        row("Grundfärg", rgb);
        row("Genomskinlighet", format_number(look.transparency));
        HNwd::Material mat;
        if (look.material != HNwd::kNone && m.doc->material(look.material, mat)) {
            row("Material", std::to_string(look.material));
            if (mat.has_texture) {
                row("Bild", std::string(mat.texture_name) +
                                (mat.texture_bytes ? " (" + std::string(mat.texture_type) + ", " +
                                                         std::to_string(mat.texture_size) +
                                                         " byte)"
                                                   : " (inte i filen)"));
                if (mat.real_scale_x > 0 || mat.real_scale_y > 0)
                    row("Bildens storlek", format_number(mat.real_scale_x) + " × " +
                                               format_number(mat.real_scale_y) + " " +
                                               std::string(mat.real_units_x));
                if (mat.w_angle != 0) row("Bildens vridning", format_number(mat.w_angle));
            }
        }
    }
    return rows;
}

std::vector<PropertyRow> describe_object(const HNwd::Document& tree,
                                         const std::vector<ModelInfo>& models,
                                         HNwd::PropertyReader& reader, uint32_t leaf) {
    std::vector<PropertyRow> rows;
    auto category = [&](std::string name) { rows.push_back({std::move(name), {}, true}); };
    auto row = [&](std::string name, std::string value) {
        rows.push_back({std::move(name), std::move(value), false});
    };
    auto name_of = [](const HNwd::Node& node) {
        return std::string(node.name.empty() ? node.display : node.name);
    };

    // Noden och dess förfäder, närmast först. Egenskaperna sitter ofta på
    // en förfader och inte på lövet som bär geometrin.
    std::vector<uint32_t> chain;
    for (uint32_t n = leaf; n != HNwd::kNone && n != 0; n = tree.parent(n)) chain.push_back(n);
    std::string path;
    for (size_t i = chain.size(); i-- > 0;) {
        HNwd::Node node;
        tree.node(chain[i], node);
        if (!path.empty()) path += " / ";
        path += name_of(node);
    }

    category("Objekt");
    HNwd::Node leaf_node;
    if (!chain.empty() && tree.node(chain.front(), leaf_node)) {
        row("Namn", std::string(leaf_node.name));
        row("Klass", std::string(leaf_node.display));
        row("Material", std::string(leaf_node.material));
    }
    for (const ModelInfo& mi : models) {
        HNwd::Node mn;
        if (!tree.node(mi.node, mn)) continue;
        if (leaf >= mi.node && leaf < mi.node + mn.subtree_size) {
            row("Modell", mi.name);
            break;
        }
    }
    row("Väg", path);

    // Flikarna läses i stigande nodordning (förfäderna har lägre index i
    // preorder) och visas närmast först.
    std::vector<std::vector<PropertyRow>> per_node(chain.size());
    std::string error;
    HNwd::NodeProperties props;
    for (size_t i = chain.size(); i-- > 0;) {
        const uint32_t n = chain[i];
        HNwd::Node node;
        tree.node(n, node);
        const std::string node_name = name_of(node);
        std::vector<PropertyRow>& out = per_node[i];
        if (node.element > 0 && static_cast<size_t>(node.element) <= tree.guid_count()) {
            uint8_t g[16];
            if (tree.guid(static_cast<size_t>(node.element) - 1, g))
                out.push_back({"GUID (" + node_name + ")", guid_text(g), false});
        }
        if (reader.read(n, props) != HNwd::Status::Ok) {
            if (error.empty()) error = reader.last_error();
            continue;
        }
        size_t first = 0;
        for (const HNwd::Tab& tab : props.tabs) {
            out.push_back({node_name + " — " + std::string(tab.name), {}, true});
            for (size_t p = 0; p < tab.property_count && first + p < props.properties.size(); ++p) {
                const HNwd::Property& prop = props.properties[first + p];
                out.push_back({std::string(prop.name), value_text(prop), false});
            }
            first += tab.property_count;
        }
    }
    for (std::vector<PropertyRow>& part : per_node)
        for (PropertyRow& r : part) rows.push_back(std::move(r));
    if (!error.empty()) {
        category("Fel");
        row("Egenskaperna", error);
    }
    return rows;
}

} // namespace nwdview
