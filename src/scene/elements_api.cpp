// Document: de små paketen — vyerna, rutnäten, typsnitten, scenens material,
// bakgrunden, ljusen, filens identitet och inläsarens logg. Tolkarna ligger i
// core (elements.h); här läses paketen ur filen och lämnas ut.

#include "document_impl.h"

#include <cmath>
#include <iterator>
#include <new>

namespace HNwd {

static_assert(Options{}.max_material_json_bytes == detail::kDefaultMaxMaterialJsonBytes,
              "the default of Options::max_material_json_bytes is the core default");

namespace {

// Ger Impl för ett Document, och skapar den om dokumentet har flyttats ifrån.
template <class T>
T& ensure(std::unique_ptr<T>& p) {
    if (!p) p = std::make_unique<T>();
    return *p;
}

// Paketet vars namn efter sista bakstrecket är `leaf`, eller nullptr. Två
// paket med samma namn går inte att välja mellan och ger Corrupt.
template <class ImplT>
Status find_package(ImplT& s, const char* leaf, const detail::DbCacheEntry*& out) {
    out = nullptr;
    int count = 0;
    for (const detail::DbCacheEntry& e : s.container.entries) {
        if (e.leaf() != leaf) continue;
        if (!out) out = &e;
        ++count;
    }
    if (count > 1)
        return s.fail(detail::ErrorCode::CorruptDirectory,
                      std::string("the directory has ") + std::to_string(count) +
                          " packages named " + leaf + "; the package to read cannot be chosen");
    return Status::Ok;
}

// Packar upp paketet `leaf` till `bytes`, högst Options::max_element_bytes.
// `found` blir falsk när filen saknar det, och anropet ger då Ok.
template <class ImplT>
Status read_package(ImplT& s, const char* leaf, bool& found, std::vector<uint8_t>& bytes) {
    found = false;
    const detail::DbCacheEntry* e = nullptr;
    if (const Status st = find_package(s, leaf, e); st != Status::Ok) return st;
    if (!e) return Status::Ok;
    std::string why;
    const detail::ErrorCode ec = unpack_package(*s.file, *e, s.options.max_element_bytes, bytes, why);
    if (ec == detail::ErrorCode::StreamTooLarge)
        return s.fail(ec, why + " (Options::max_element_bytes is " +
                              std::to_string(s.options.max_element_bytes) + ")");
    if (ec != detail::ErrorCode::Ok) return s.fail(ec, why);
    found = true;
    return Status::Ok;
}

// Som read_package(), men en fil utan paketet ger Missing.
template <class ImplT>
Status require_package(ImplT& s, const char* leaf, std::vector<uint8_t>& bytes) {
    bool found = false;
    if (const Status st = read_package(s, leaf, found, bytes); st != Status::Ok) return st;
    if (!found)
        return s.fail(detail::ErrorCode::PackageMissing,
                      std::string("the file has no ") + leaf + " package");
    return Status::Ok;
}

// Fälten som inte lämnas ut, tömda så att de inte hålls i minnet.
void trim_view(detail::ViewElement& v) {
    std::string().swap(v.avatar);
    std::vector<int32_t>().swap(v.tail_ints);
    std::vector<uint8_t>().swap(v.rest);
}

void to_camera(const detail::ViewElement& v, Camera& out) {
    Camera c;
    c.form = v.form;
    c.projection = v.projection;
    for (int k = 0; k < 3; ++k) c.position[k] = v.position[k];
    for (int k = 0; k < 4; ++k) c.rotation[k] = v.rotation[k];
    c.near_depth = v.near_depth;
    c.far_depth = v.far_depth;
    c.angle = v.angle;
    if (v.form == 9 || v.form == 843) {
        c.has_target = true;
        c.distance = v.distance;
        double f[3];
        camera_forward(c, f);
        for (int k = 0; k < 3; ++k) c.target[k] = c.position[k] + v.distance * f[k];
    }
    static_assert(kClipPlaneCount == std::size(detail::ViewClip{}.planes));
    for (size_t i = 0; i < kClipPlaneCount; ++i) {
        const detail::ClipPlane& p = v.clip.planes[i];
        c.clip[i].enabled = p.enabled == 1;
        for (int k = 0; k < 3; ++k) {
            c.clip[i].point[k] = p.point[k];
            c.clip[i].normal[k] = p.normal[k];
        }
    }
    out = c;
}

// Ett påslaget snittplan har en normal med längden 1; annars går planet inte
// att tolka som punkt och normal. `false` med skälet i `why`.
bool check_clip(const detail::ViewElement& v, const std::string& what, std::string& why) {
    for (size_t i = 0; i < kClipPlaneCount; ++i) {
        const detail::ClipPlane& p = v.clip.planes[i];
        if (p.enabled != 1) continue;
        const double len = std::sqrt(p.normal[0] * p.normal[0] + p.normal[1] * p.normal[1] +
                                     p.normal[2] * p.normal[2]);
        if (!(std::fabs(len - 1) <= 1e-6)) {
            why = what + ": the switched-on clip plane " + std::to_string(i) +
                  " has a normal whose length is not 1";
            return false;
        }
    }
    return true;
}

void copy_guid(const detail::Guid16& g, uint8_t out[16]) {
    for (size_t k = 0; k < 16; ++k) out[k] = g[k];
}

size_t material_bytes(const detail::MaterialData& m) {
    size_t n = sizeof(m) + m.json.capacity() + m.texture.bitmap.capacity() +
               m.texture.real_units_x.capacity() + m.texture.real_units_y.capacity() +
               m.files.capacity() * sizeof(detail::MaterialFile);
    for (const detail::MaterialFile& f : m.files)
        n += f.name.capacity() + f.kind.capacity() + f.type.capacity() + f.bytes.capacity();
    return n;
}

size_t view_bytes(const detail::ViewElement& v) {
    return v.avatar.capacity() + v.tail_ints.capacity() * sizeof(int32_t) + v.rest.capacity();
}

} // namespace

void camera_forward(const Camera& camera, double out[3]) {
    const double x = camera.rotation[0], y = camera.rotation[1], z = camera.rotation[2],
                 w = camera.rotation[3];
    out[0] = -2 * (x * z + y * w);
    out[1] = -2 * (y * z - x * w);
    out[2] = -(1 - 2 * (x * x + y * y));
}

size_t clip_plane_count(const Camera& camera) {
    size_t n = 0;
    for (const ClipPlane& p : camera.clip) n += p.enabled ? 1 : 0;
    return n;
}

size_t ViewsData::memory_bytes() const {
    size_t n = view_bytes(current) + view_bytes(home);
    n += items.capacity() * sizeof(detail::SavedViews::Item);
    for (const detail::SavedViews::Item& it : items) n += it.name.capacity();
    n += views.capacity() * sizeof(SavedViewData);
    n += redlines.capacity() * sizeof(detail::Redline);
    for (const detail::Redline& r : redlines) n += r.text.capacity();
    n += next_sibling.capacity() * sizeof(uint32_t);
    return n;
}

size_t Document::Impl::elements_memory_bytes() const {
    size_t n = views.memory_bytes();
    n += grids.systems.capacity() * sizeof(detail::GridSystem);
    for (const detail::GridSystem& g : grids.systems) {
        n += g.name.capacity() + g.lines.capacity() * sizeof(detail::GridLine) +
             g.levels.capacity() * sizeof(detail::GridLevel);
        for (const detail::GridLine& l : g.lines) n += l.label.capacity();
        for (const detail::GridLevel& l : g.levels) n += l.name.capacity();
    }
    n += fonts.entries.capacity() * sizeof(uint32_t) +
         fonts.fonts.capacity() * sizeof(detail::FontEntry);
    for (const detail::FontEntry& f : fonts.fonts) n += f.name.capacity();
    n += scene_materials.entries.capacity() * sizeof(detail::ReMaterials::Entry);
    for (const detail::ReMaterials::Entry& e : scene_materials.entries) n += e.name.capacity();
    for (const detail::MaterialData& m : scene_materials.materials) n += material_bytes(m);
    return n;
}

// ---------------------------------------------------------------------------
// Vyerna.

Status Document::load_views() try {
    Impl& s = ensure(impl_);
    if (!is_open()) return s.fail(Status::NotLoaded, "no file is open");
    if (s.views_loaded) return s.ok();

    ViewsData views;
    std::vector<uint8_t> bytes;
    std::string why;
    bool found = false;

    if (const Status st = read_package(s, "LcOpCurrentViewElement", found, bytes); st != Status::Ok)
        return st;
    if (found) {
        const detail::ErrorCode ec = detail::parse_view_element(
            bytes.data(), bytes.size(), views.current, &why, "LcOpCurrentViewElement");
        if (ec != detail::ErrorCode::Ok) return s.fail(ec, why);
        if (!check_clip(views.current, "LcOpCurrentViewElement", why))
            return s.fail(detail::ErrorCode::Unsupported, why);
        trim_view(views.current);
        views.has_current = true;
    }

    if (const Status st = read_package(s, "LcOpHomeViewElement", found, bytes); st != Status::Ok)
        return st;
    if (found) {
        const detail::ErrorCode ec = detail::parse_view_element(
            bytes.data(), bytes.size(), views.home, &why, "LcOpHomeViewElement");
        if (ec != detail::ErrorCode::Ok) return s.fail(ec, why);
        if (!check_clip(views.home, "LcOpHomeViewElement", why))
            return s.fail(detail::ErrorCode::Unsupported, why);
        trim_view(views.home);
        views.has_home = true;
    }

    if (const Status st = read_package(s, "LcOpSavedViewsElement", found, bytes); st != Status::Ok)
        return st;
    if (found) {
        detail::SavedViews saved;
        const detail::ErrorCode ec =
            detail::parse_saved_views(bytes.data(), bytes.size(), saved, &why);
        if (ec != detail::ErrorCode::Ok) return s.fail(ec, why);
        std::vector<uint8_t>().swap(bytes);
        for (size_t i = 0; i < saved.items.size(); ++i) {
            const uint32_t v = saved.items[i].view;
            if (saved.items[i].kind == 0 &&
                !check_clip(saved.views[v].view, "LcOpSavedViewsElement view " + std::to_string(i), why))
                return s.fail(detail::ErrorCode::Unsupported, why);
        }
        // Bara det som lämnas ut hålls kvar, i listor med precis den storlek
        // de behöver.
        views.views.resize(saved.views.size());
        for (size_t v = 0; v < saved.views.size(); ++v) {
            const detail::SavedViews::View& from = saved.views[v];
            SavedViewData& to = views.views[v];
            to_camera(from.view, to.camera);
            to.redline_begin = from.redline_begin;
            to.redline_count = from.redline_count;
        }
        std::vector<detail::SavedViews::View>().swap(saved.views);
        std::vector<detail::ViewState>().swap(saved.states);
        std::vector<uint32_t>().swap(saved.state_refs);
        saved.items.shrink_to_fit();
        saved.redlines.shrink_to_fit();
        views.items = std::move(saved.items);
        views.redlines = std::move(saved.redlines);
        const std::vector<detail::SavedViews::Item>& items = views.items;
        // Nästa syskon: den senast sedda posten med samma förälder pekar på
        // den här. Platsen items.size() står för den översta nivån.
        views.next_sibling.assign(items.size(), kNone);
        std::vector<uint32_t> last(items.size() + 1, kNone);
        for (uint32_t i = 0; i < items.size(); ++i) {
            const size_t p = items[i].parent == UINT32_MAX ? items.size() : items[i].parent;
            if (last[p] != kNone) views.next_sibling[last[p]] = i;
            last[p] = i;
        }
    }

    s.views = std::move(views);
    s.views_loaded = true;
    return s.ok();
} catch (const std::bad_alloc&) {
    return out_of_memory(ensure(impl_));
}

bool Document::current_view(Camera& out) const {
    if (!impl_ || !impl_->views_loaded || !impl_->views.has_current) return false;
    to_camera(impl_->views.current, out);
    return true;
}

bool Document::home_view(Camera& out) const {
    if (!impl_ || !impl_->views_loaded || !impl_->views.has_home) return false;
    to_camera(impl_->views.home, out);
    return true;
}

size_t Document::saved_view_count() const {
    return impl_ && impl_->views_loaded ? impl_->views.items.size() : 0;
}

bool Document::saved_view(size_t i, SavedView& out) const {
    if (i >= saved_view_count()) return false;
    const ViewsData& vd = impl_->views;
    const detail::SavedViews::Item& it = vd.items[i];
    SavedView v;
    v.kind = it.kind == 4 ? SavedViewKind::Folder : SavedViewKind::View;
    v.name = it.name;
    copy_guid(it.guid, v.guid);
    v.parent = it.parent == UINT32_MAX ? kNone : it.parent;
    v.child_count = it.child_count;
    v.next_sibling = vd.next_sibling[i];
    if (it.kind == 0) {
        const SavedViewData& d = vd.views[it.view];
        v.redline_count = d.redline_count;
        v.camera = d.camera;
    }
    out = v;
    return true;
}

bool Document::saved_view_redline(size_t view, size_t k, Redline& out) const {
    if (view >= saved_view_count()) return false;
    const ViewsData& vd = impl_->views;
    const detail::SavedViews::Item& it = vd.items[view];
    if (it.kind != 0) return false;
    const SavedViewData& d = vd.views[it.view];
    if (k >= d.redline_count) return false;
    const detail::Redline& r = vd.redlines[d.redline_begin + k];
    Redline o;
    o.kind = r.type == 4 ? RedlineKind::Text : RedlineKind::Line;
    o.text = r.text;
    for (int c = 0; c < 4; ++c) o.points[c] = r.points[c];
    for (int c = 0; c < 3; ++c) o.colour[c] = r.colour[c];
    out = o;
    return true;
}

// ---------------------------------------------------------------------------
// Rutnäten.

Status Document::load_grids() try {
    Impl& s = ensure(impl_);
    if (!is_open()) return s.fail(Status::NotLoaded, "no file is open");
    if (s.grids_loaded) return s.ok();

    detail::GridElement grids;
    std::vector<uint8_t> bytes;
    bool found = false;
    if (const Status st = read_package(s, "LcOpGridElement", found, bytes); st != Status::Ok)
        return st;
    if (found) {
        std::string why;
        const detail::ErrorCode ec = detail::parse_grid(bytes.data(), bytes.size(), grids, &why);
        if (ec != detail::ErrorCode::Ok) return s.fail(ec, why);
    }
    s.grids = std::move(grids);
    s.grids_loaded = true;
    return s.ok();
} catch (const std::bad_alloc&) {
    return out_of_memory(ensure(impl_));
}

size_t Document::grid_count() const {
    return impl_ && impl_->grids_loaded ? impl_->grids.systems.size() : 0;
}

bool Document::grid(size_t i, GridSystem& out) const {
    if (i >= grid_count()) return false;
    const detail::GridSystem& g = impl_->grids.systems[i];
    GridSystem o;
    o.name = g.name;
    for (int k = 0; k < 3; ++k) {
        o.origin[k] = g.origin[k];
        o.axis_x[k] = g.axes[0][k];
        o.axis_y[k] = g.axes[1][k];
    }
    o.line_count = static_cast<uint32_t>(g.lines.size());
    o.level_count = static_cast<uint32_t>(g.levels.size());
    o.level = g.level;
    out = o;
    return true;
}

bool Document::grid_line(size_t system, size_t k, GridLine& out) const {
    if (system >= grid_count()) return false;
    const detail::GridSystem& g = impl_->grids.systems[system];
    if (k >= g.lines.size()) return false;
    const detail::GridLine& l = g.lines[k];
    GridLine o;
    o.label = l.label;
    for (int c = 0; c < 4; ++c) o.ends[c] = l.ends[c];
    for (int p = 0; p < 2; ++p) {
        const double x = l.ends[2 * p], y = l.ends[2 * p + 1];
        for (int c = 0; c < 3; ++c) o.world[p][c] = g.origin[c] + x * g.axes[0][c] + y * g.axes[1][c];
    }
    out = o;
    return true;
}

bool Document::grid_level(size_t system, size_t k, GridLevel& out) const {
    if (system >= grid_count()) return false;
    const detail::GridSystem& g = impl_->grids.systems[system];
    if (k >= g.levels.size()) return false;
    GridLevel o;
    o.name = g.levels[k].name;
    o.elevation = g.levels[k].elevation;
    out = o;
    return true;
}

// ---------------------------------------------------------------------------
// Typsnitten och scenens material.

Status Document::load_fonts() try {
    Impl& s = ensure(impl_);
    if (!is_open()) return s.fail(Status::NotLoaded, "no file is open");
    if (s.fonts_loaded) return s.ok();

    detail::SharedNodes fonts;
    std::vector<uint8_t> bytes;
    bool found = false;
    if (const Status st = read_package(s, "LcOpNwdSharedNodes", found, bytes); st != Status::Ok)
        return st;
    if (found) {
        std::string why;
        const detail::ErrorCode ec =
            detail::parse_shared_nodes(bytes.data(), bytes.size(), fonts, &why);
        if (ec != detail::ErrorCode::Ok) return s.fail(ec, why);
    }
    s.fonts = std::move(fonts);
    s.fonts_loaded = true;
    return s.ok();
} catch (const std::bad_alloc&) {
    return out_of_memory(ensure(impl_));
}

size_t Document::font_count() const {
    return impl_ && impl_->fonts_loaded ? impl_->fonts.entries.size() : 0;
}

bool Document::font(size_t i, Font& out) const {
    if (i >= font_count()) return false;
    const detail::FontEntry& f = impl_->fonts.fonts[impl_->fonts.entries[i]];
    Font o;
    o.name = f.name;
    o.size = f.size;
    out = o;
    return true;
}

Status Document::load_scene_materials() try {
    Impl& s = ensure(impl_);
    if (!is_open()) return s.fail(Status::NotLoaded, "no file is open");
    if (s.scene_materials_loaded) return s.ok();

    detail::ReMaterials materials;
    std::vector<uint8_t> bytes;
    bool found = false;
    if (const Status st = read_package(s, "LcReMaterialElement", found, bytes); st != Status::Ok)
        return st;
    if (found) {
        std::string why;
        const detail::ErrorCode ec =
            detail::parse_re_materials(bytes.data(), bytes.size(), materials, &why,
                                       s.options.max_material_json_bytes);
        if (ec != detail::ErrorCode::Ok) return s.fail(ec, why);
    }
    s.scene_materials = std::move(materials);
    s.scene_materials_loaded = true;
    return s.ok();
} catch (const std::bad_alloc&) {
    return out_of_memory(ensure(impl_));
}

size_t Document::scene_material_count() const {
    return impl_ && impl_->scene_materials_loaded ? impl_->scene_materials.entries.size() : 0;
}

bool Document::scene_material(size_t i, SceneMaterial& out) const {
    if (i >= scene_material_count()) return false;
    const detail::ReMaterials& rm = impl_->scene_materials;
    const detail::ReMaterials::Entry& e = rm.entries[i];
    SceneMaterial o;
    o.name = e.name;
    copy_guid(e.guid, o.guid);
    if (e.material != detail::kNoMaterial) {
        o.has_material = true;
        to_material(rm.materials[e.material], o.material);
    }
    out = o;
    return true;
}

// ---------------------------------------------------------------------------
// Uppslagen.

Status Document::background(Background& out) try {
    Impl& s = ensure(impl_);
    out = Background{};
    if (!is_open()) return s.fail(Status::NotLoaded, "no file is open");
    std::vector<uint8_t> bytes;
    if (const Status st = require_package(s, "LcOpBackgroundElement", bytes); st != Status::Ok)
        return st;
    detail::BackgroundElement b;
    std::string why;
    const detail::ErrorCode ec = detail::parse_background(bytes.data(), bytes.size(), b, &why,
                                                          s.options.max_material_json_bytes);
    if (ec != detail::ErrorCode::Ok) return s.fail(ec, why);
    for (int i = 0; i < 7; ++i)
        for (int k = 0; k < 3; ++k) out.colours[i][k] = b.colours[i][k];
    out.has_sky = b.has_sky;
    if (b.has_sky) out.sky_json = std::move(b.sky.json);
    return s.ok();
} catch (const std::bad_alloc&) {
    return out_of_memory(ensure(impl_));
}

Status Document::lights(std::vector<Light>& out) try {
    Impl& s = ensure(impl_);
    out.clear();
    if (!is_open()) return s.fail(Status::NotLoaded, "no file is open");
    std::vector<uint8_t> bytes;
    if (const Status st = require_package(s, "LcOpLightsElement", bytes); st != Status::Ok)
        return st;
    detail::LightsElement le;
    std::string why;
    const detail::ErrorCode ec = detail::parse_lights(bytes.data(), bytes.size(), le, &why,
                                                      s.options.max_material_json_bytes);
    if (ec != detail::ErrorCode::Ok) return s.fail(ec, why);
    out.resize(le.entries.size());
    for (size_t i = 0; i < le.entries.size(); ++i) {
        const detail::LightsElement::Entry& e = le.entries[i];
        Light& l = out[i];
        l.name = e.name;
        copy_guid(e.guid, l.guid);
        for (int k = 0; k < 3; ++k) {
            l.origin[k] = e.origin[k];
            l.direction[k] = e.direction[k];
        }
        // Tolken ger ett index för varje ny och bakåtpekande klass 27.
        const detail::LightBody& body = le.lights[e.light];
        for (int k = 0; k < 3; ++k) l.specular[k] = body.values[3 + k];
        if (body.material != detail::kNoMaterial) l.json = le.materials[body.material].json;
    }
    return s.ok();
} catch (const std::bad_alloc&) {
    out.clear();
    return out_of_memory(ensure(impl_));
}

Status Document::file_identity(FileIdentity& out) try {
    Impl& s = ensure(impl_);
    out = FileIdentity{};
    if (!is_open()) return s.fail(Status::NotLoaded, "no file is open");
    std::vector<uint8_t> bytes;
    std::string why;
    bool found = false;

    if (const Status st = read_package(s, "LcOpOdyScenePropsChunk", found, bytes); st != Status::Ok)
        return st;
    if (found) {
        detail::SceneProps sp;
        const detail::ErrorCode ec = detail::parse_scene_props(bytes.data(), bytes.size(), sp, &why);
        if (ec != detail::ErrorCode::Ok) return s.fail(ec, why);
        out.has_scene = true;
        out.saved_path = std::move(sp.path);
        copy_guid(sp.guid, out.scene_guid);
    }

    if (const Status st = read_package(s, "LcOpOdyFileInfoChunk", found, bytes); st != Status::Ok)
        return st;
    if (found) {
        detail::FileInfo fi;
        const detail::ErrorCode ec = detail::parse_file_info(bytes.data(), bytes.size(), fi, &why);
        if (ec != detail::ErrorCode::Ok) return s.fail(ec, why);
        out.has_file_info = true;
        out.package_prefix = std::move(fi.prefix);
        copy_guid(fi.file_guid, out.file_guid);
    }

    if (!out.has_scene && !out.has_file_info)
        return s.fail(detail::ErrorCode::PackageMissing,
                      "the file has neither an LcOpOdyScenePropsChunk nor an "
                      "LcOpOdyFileInfoChunk package");
    return s.ok();
} catch (const std::bad_alloc&) {
    return out_of_memory(ensure(impl_));
}

Status Document::load_log(std::string& out) try {
    Impl& s = ensure(impl_);
    out.clear();
    if (!is_open()) return s.fail(Status::NotLoaded, "no file is open");
    std::vector<uint8_t> bytes;
    if (const Status st = require_package(s, "LcOpNwdStats", bytes); st != Status::Ok) return st;
    std::string why;
    bool non_utf8 = false;
    const detail::ErrorCode ec = detail::parse_stats(bytes.data(), bytes.size(), out, non_utf8, &why);
    if (ec != detail::ErrorCode::Ok) {
        out.clear();
        return s.fail(ec, why);
    }
    return s.ok();
} catch (const std::bad_alloc&) {
    out.clear();
    return out_of_memory(ensure(impl_));
}

} // namespace HNwd
