// Document: vyerna, rutnäten, typsnitten, scenens material, bakgrunden,
// ljusen, filens identitet och loggen. Syntetiska filer med påhittade värden
// genom det publika API:t, och exempelfilerna.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "HNwd.h"
#include "path_utf8.h"
#include "test_data.h"
#include "test_support.h"

using HNwd::Document;
using HNwd::Status;
using testdata::Bytes;
using testdata::ByteWriter;

namespace {

bool write_file(const std::string& path, const Bytes& bytes) {
#ifdef _WIN32
    const std::wstring w = HNwd::detail::utf8_to_wide(path);
    if (w.empty()) return false;
    FILE* f = _wfopen(w.c_str(), L"wb");
#else
    FILE* f = std::fopen(path.c_str(), "wb");
#endif
    if (!f) return false;
    const bool wrote = bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    return std::fclose(f) == 0 && wrote;
}

// En temporärfil som tas bort när den går ur räckvidd; dokumentet deklareras
// efter den, så att det stängs först.
struct TempFile {
    std::string path;
    TempFile(const char* name, const Bytes& bytes) : path(testdata::tmp_path(name)) {
        write_file(path, bytes);
    }
    ~TempFile() { testdata::remove_utf8(path); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
};

struct Opened {
    TempFile file;
    Document doc;
    Status status;
    Opened(const char* name, const Bytes& bytes, const HNwd::Options& options = {})
        : file(name, bytes), status(doc.open(file.path.c_str(), options)) {}
};

bool close_to(double a, double b, double eps = 1e-12) { return std::fabs(a - b) <= eps; }
bool contains(const char* s, const char* part) { return std::strstr(s, part) != nullptr; }

void guid_bytes(ByteWriter& w, uint8_t base) {
    for (int i = 0; i < 16; ++i) w.u8(static_cast<uint8_t>(base + i));
}

bool guid_is(const uint8_t g[16], uint8_t base) {
    for (int i = 0; i < 16; ++i)
        if (g[i] != static_cast<uint8_t>(base + i)) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Paketen.

const double kHalf = std::sqrt(0.5);

// En vy: läget, kvaternionen och avståndet (form 9 och 843) eller sex tal
// (form 895 och 1919). Avatarblocket och de sista byten är påhittade.
//
// Snittplanen: med `clip` Pair är plan 0 och 1 påslagna, en skiva mellan
// z = 3 och z = 8 (punkterna (1, 2, 3) och (1, 2, 8), normalerna (0, 0, 1)
// och (0, 0, −1)); med BadNormal är plan 0 påslaget med normalen (0, 0, 2).
enum class Clip { None, Pair, BadNormal };

void write_view(ByteWriter& w, int32_t form, int32_t projection, const double pos[3],
                const double q[4], double distance, Clip clip = Clip::None) {
    w.i32(form);
    w.i32(projection);
    for (int k = 0; k < 3; ++k) w.f64(pos[k]);
    for (int k = 0; k < 4; ++k) w.f64(q[k]);
    for (const double v : {1.25, 0.5, 2.0, 80.0, 0.0, 0.0}) w.f64(v);   // u, angle, near, far, 0, 0
    if (form != 9) {
        for (const double v : {10.0, 20.0, 30.0, 40.0}) w.f64(v);
        w.str("figur");
        for (const double v : {1.0, 2.0, 3.0, 4.0}) w.f64(v);
        w.i32(1);
        for (const double v : {5.0, 6.0, 7.0, 8.0}) w.f64(v);
    }
    if (form == 9 || form == 843) {
        w.f64(distance);
    } else {
        for (const double v : {0.0, 0.0, 1.0, 3.0, 4.0, 0.75}) w.f64(v);
    }
    const int ints = form == 9 ? 2 : 6;
    for (int i = 0; i < ints; ++i) {
        if (form == 1919 && i == 4) w.f64(0.5);
        w.i32(i);
    }
    // Snittplanen: sex heltal, tre tal, antalet 6, sex plan (påslaget, läge,
    // punkt, normal, tangent), två flaggpar med sex tal var, och en
    // kvaternion.
    for (int32_t i = 0; i < 6; ++i) w.i32(10 + i);
    for (const double v : {0.5, 1.5, 2.5}) w.f64(v);
    w.i32(6);
    for (int k = 0; k < 6; ++k) {
        const bool on = (clip == Clip::Pair && k < 2) || (clip == Clip::BadNormal && k == 0);
        w.i32(on ? 1 : 0);
        w.i32(on ? 10 : 20 + k);
        if (on && clip == Clip::Pair) {
            for (const double v : {1.0, 2.0, k == 0 ? 3.0 : 8.0}) w.f64(v);
            for (const double v : {0.0, 0.0, k == 0 ? 1.0 : -1.0}) w.f64(v);
        } else {
            for (const double v : {1.0 * k, 2.0 * k, 3.0 * k}) w.f64(v);
            for (const double v : {0.0, 0.0, on ? 2.0 : 1.0}) w.f64(v);
        }
        for (const double v : {1.0, 0.0, 0.0}) w.f64(v);
    }
    w.i32(1);
    w.i32(2);
    for (const double v : {1.0, 2.0, 3.0, 4.0, 5.0, 6.0}) w.f64(v);
    w.i32(3);
    w.i32(4);
    for (const double v : {7.0, 8.0, 9.0, 10.0, 11.0, 12.0}) w.f64(v);
    for (const double v : {0.0, 0.0, 0.6, 0.8}) w.f64(v);
}

Bytes view_package(int32_t form, const double pos[3], const double q[4], double distance,
                   Clip clip = Clip::None) {
    ByteWriter w;
    write_view(w, form, 0, pos, q, distance, clip);
    w.pad(8);
    return w.data;
}

// Två poster på översta nivån: vyn "Norr" med en text och en linje, och
// mappen "Plan" med vyn "Plan 1" (form 1919) och en tom mapp "Tom".
Bytes saved_views_package() {
    const double p1[3] = {5, 6, 7};
    const double q1[4] = {0, 0, 0, 1};
    const double p2[3] = {-1, -2, 30};
    const double q2[4] = {kHalf, 0, 0, kHalf};
    ByteWriter w;
    w.i32(2);
    w.i32(0);
    w.str("Norr");
    w.i32(0);
    guid_bytes(w, 0x40);
    w.i32(0);
    write_view(w, 895, 0, p1, q1, 0, Clip::Pair);
    w.i32(2);
    w.i32(4);
    w.str("Anteckning");
    w.f64(0.25);
    w.f64(-0.5);
    w.i32(3);
    for (const double c : {1.0, 0.5, 0.0}) w.f64(c);
    w.i32(9);
    w.i32(1);
    for (const double v : {-0.75, -0.25, 0.5, 0.125}) w.f64(v);
    w.i32(3);
    for (const double c : {0.0, 0.0, 1.0}) w.f64(c);
    w.i32(9);
    w.i32(1);
    w.obj(89);   // 100
    w.str("LcOpShadOverridesElement");
    for (const int32_t v : {1, 7, 0}) w.i32(v);
    // Mappen.
    w.i32(4);
    w.str("Plan");
    w.i32(0);
    guid_bytes(w, 0x50);
    w.i32(0);
    w.i32(2);
    w.i32(0);
    w.str("Plan 1");
    w.i32(0);
    guid_bytes(w, 0x60);
    w.i32(0);
    write_view(w, 1919, 1, p2, q2, 0);
    w.i32(0);
    w.i32(1);
    w.ref(100);
    w.i32(4);
    w.str("Tom");
    w.i32(0);
    guid_bytes(w, 0x70);
    w.i32(0);
    w.i32(0);
    w.pad(8);
    return w.data;
}

// Ett system vridet 90° (axis_x = y, axis_y = −x) med två linjer och två
// nivåer, och ett tomt system.
Bytes grid_package() {
    ByteWriter w;
    w.i32(2);
    w.str("Hus A");
    for (const double v : {100.0, 200.0, 3.0}) w.f64(v);
    for (const double v : {0.0, 1.0, 0.0, -1.0, 0.0, 0.0, -1.0, 0.0, 0.0}) w.f64(v);
    w.i32(2);
    w.str("1");
    w.f64(-1);
    w.f64(2);
    for (const int32_t v : {2, 0, 3}) w.i32(v);
    for (const double v : {0.0, -5.0, 0.0, 25.0}) w.f64(v);
    w.str("B");
    w.f64(-1);
    w.f64(2);
    for (const int32_t v : {2, 0, 3}) w.i32(v);
    for (const double v : {-5.0, 8.0, 20.0, 8.0}) w.f64(v);
    w.i32(2);
    w.str("Plan 0");
    w.f64(0);
    w.str("Plan 1");
    w.f64(3.25);
    w.i32(1);
    w.str("Utan");
    for (int k = 0; k < 12; ++k) w.f64(k == 3 || k == 7 || k == 10 ? 1 : 0);
    w.i32(0);
    w.i32(0);
    w.i32(-1);
    w.i32(1);
    w.i32(4);
    w.pad(8);
    return w.data;
}

Bytes background_package() {
    ByteWriter w;
    w.i32(1);
    for (int k = 0; k < 7; ++k)
        for (int i = 0; i < 3; ++i) w.f64((k * 30 + i * 5) / 255.0);
    w.i32(0);
    w.null_ptr();
    w.pad(8);
    return w.data;
}

// Tre poster: två typsnitt och en bakåtpekare på det första.
Bytes fonts_package() {
    ByteWriter w;
    w.i32(3);
    w.obj(102);   // 100
    w.str("Rubrik");
    w.f64(3.5);
    for (const int32_t v : {4, 0, 0, 0}) w.i32(v);
    w.obj(102);   // 101
    w.str("Brödtext");
    w.f64(1.75);
    for (const int32_t v : {4, 0, 0, 0}) w.i32(v);
    w.ref(100);
    w.pad(8);
    return w.data;
}

constexpr const char* kMaterialJson = "{\"version\":2,\"userassets\":[],\"materials\":{}}";

// Tre material: det andra delar det förstas beskrivning, det tredje saknar en.
Bytes materials_package() {
    ByteWriter w;
    w.i32(3);
    w.i32(80);
    w.str("Tegel");
    w.i32(0);
    guid_bytes(w, 0x80);
    w.i32(0);
    w.obj(185);   // 100
    w.str(kMaterialJson);
    w.null_str();
    w.i32(1);
    w.str("tegel.png");
    w.i32(1);
    w.str("navis");
    w.str("png");
    w.i64a(4);
    w.text("abcd");
    w.pad(4);
    w.i32(80);
    w.str("Tegel 2");
    w.i32(0);
    guid_bytes(w, 0x90);
    w.i32(0);
    w.ref(100);
    w.i32(80);
    w.str("Tom");
    w.i32(0);
    guid_bytes(w, 0xA0);
    w.i32(0);
    w.null_ptr();
    w.pad(8);
    return w.data;
}

Bytes lights_package() {
    ByteWriter w;
    w.i32(1);
    w.i32(83);
    w.str("Sol");
    w.i32(0);
    guid_bytes(w, 0xB0);
    w.i32(0);
    w.obj(27);   // 100
    w.i32(1);
    w.obj(185);  // 101
    w.str(kMaterialJson);
    w.null_str();
    w.i32(0);
    w.i32(0);
    for (const double v : {0.0, 0.0, 0.0, 0.5, 0.25, 0.125, 0.5, 0.25, 0.125, 0.0, 0.6, 0.8}) w.f64(v);
    for (const double v : {0.0, 0.0, 0.0, 0.0, 0.6, 0.8}) w.f64(v);
    w.pad(8);
    return w.data;
}

Bytes scene_props_package() {
    ByteWriter w;
    w.str("D:\\projekt\\hus.nwd");
    guid_bytes(w, 0xC0);
    w.i32(0);
    w.i32(0);
    w.pad(8);
    return w.data;
}

Bytes stats_package(const std::string& text) {
    ByteWriter w;
    w.str(text);
    w.pad(8);
    return w.data;
}

testdata::Package pkg(const char* leaf, const Bytes& stream) {
    return {std::string("cache\\") + leaf, 1, 0, 0, testdata::zlib_store(stream)};
}

const double kPos[3] = {1, 2, 3};
const double kQuarter[4] = {kHalf, 0, 0, kHalf};   // 90° kring x: blicken längs +y
const double kIdentity[4] = {0, 0, 0, 1};

Bytes full_file() {
    std::vector<testdata::Package> p;
    p.push_back(pkg("LcOpCurrentViewElement", view_package(9, kPos, kQuarter, 12.5)));
    p.push_back(pkg("LcOpHomeViewElement", view_package(843, kPos, kIdentity, 2)));
    p.push_back(pkg("LcOpSavedViewsElement", saved_views_package()));
    p.push_back(pkg("LcOpGridElement", grid_package()));
    p.push_back(pkg("LcOpBackgroundElement", background_package()));
    p.push_back(pkg("LcOpNwdSharedNodes", fonts_package()));
    p.push_back(pkg("LcReMaterialElement", materials_package()));
    p.push_back(pkg("LcOpLightsElement", lights_package()));
    p.push_back(pkg("LcOpOdyScenePropsChunk", scene_props_package()));
    p.push_back(pkg("LcOpNwdStats", stats_package("Läst utan fel.\n")));
    return testdata::build_file(p, true);
}

// ---------------------------------------------------------------------------
// Syntetiska filer.

int test_views_synthetic() {
    int fails = 0;
    Opened o("hnwd_test_views_full.nwd", full_file());
    CHECK(o.status == Status::Ok);
    Document& doc = o.doc;

    // Inget är läst före load_…().
    HNwd::Camera cam;
    CHECK(!doc.current_view(cam) && doc.saved_view_count() == 0 && doc.grid_count() == 0);
    CHECK(doc.font_count() == 0 && doc.scene_material_count() == 0);
    const size_t before = doc.memory_bytes();

    CHECK(doc.load_views() == Status::Ok);
    CHECK(doc.load_views() == Status::Ok);
    CHECK(doc.current_view(cam));
    CHECK(cam.form == 9 && cam.projection == 0 && cam.has_target);
    CHECK(cam.position[0] == 1 && cam.position[1] == 2 && cam.position[2] == 3);
    CHECK(cam.near_depth == 2 && cam.far_depth == 80 && cam.angle == 0.5 && cam.distance == 12.5);
    double f[3];
    HNwd::camera_forward(cam, f);
    CHECK(close_to(f[0], 0) && close_to(f[1], 1) && close_to(f[2], 0));
    CHECK(close_to(cam.target[0], 1) && close_to(cam.target[1], 14.5) && close_to(cam.target[2], 3));
    CHECK(HNwd::clip_plane_count(cam) == 0 && !cam.clip[0].enabled);

    HNwd::Camera home;
    CHECK(doc.home_view(home) && home.form == 843 && home.has_target && home.distance == 2);
    CHECK(close_to(home.target[2], 1));   // identiteten tittar längs −z

    CHECK(doc.saved_view_count() == 4);
    std::vector<HNwd::SavedView> sv(doc.saved_view_count());
    for (size_t i = 0; i < sv.size(); ++i) CHECK(doc.saved_view(i, sv[i]));
    if (sv.size() == 4) {
        CHECK(sv[0].kind == HNwd::SavedViewKind::View && sv[0].name == "Norr");
        CHECK(guid_is(sv[0].guid, 0x40) && sv[0].parent == HNwd::kNone && sv[0].next_sibling == 1);
        CHECK(sv[0].camera.form == 895 && !sv[0].camera.has_target && sv[0].redline_count == 2);
        CHECK(sv[0].camera.position[2] == 7);
        // Två påslagna snittplan, en skiva mellan z = 3 och z = 8.
        const HNwd::Camera& c0 = sv[0].camera;
        CHECK(HNwd::clip_plane_count(c0) == 2);
        CHECK(c0.clip[0].enabled && c0.clip[1].enabled && !c0.clip[2].enabled && !c0.clip[5].enabled);
        CHECK(c0.clip[0].point[0] == 1 && c0.clip[0].point[1] == 2 && c0.clip[0].point[2] == 3);
        CHECK(c0.clip[0].normal[2] == 1 && c0.clip[0].normal[0] == 0);
        CHECK(c0.clip[1].point[2] == 8 && c0.clip[1].normal[2] == -1);
        // Ett avslaget plan lämnas ut som det står.
        CHECK(c0.clip[3].point[0] == 3 && c0.clip[3].point[2] == 9 && c0.clip[3].normal[2] == 1);
        CHECK(HNwd::clip_plane_count(sv[2].camera) == 0);
        CHECK(sv[1].kind == HNwd::SavedViewKind::Folder && sv[1].name == "Plan");
        CHECK(sv[1].child_count == 2 && sv[1].next_sibling == HNwd::kNone);
        CHECK(sv[2].name == "Plan 1" && sv[2].parent == 1 && sv[2].next_sibling == 3);
        CHECK(sv[2].camera.form == 1919 && sv[2].camera.projection == 1 && sv[2].redline_count == 0);
        double f2[3];
        HNwd::camera_forward(sv[2].camera, f2);
        CHECK(close_to(f2[1], 1));
        CHECK(sv[3].kind == HNwd::SavedViewKind::Folder && sv[3].name == "Tom");
        CHECK(sv[3].parent == 1 && sv[3].child_count == 0 && sv[3].next_sibling == HNwd::kNone);
        CHECK(guid_is(sv[3].guid, 0x70));
    }
    HNwd::Redline r;
    CHECK(doc.saved_view_redline(0, 0, r) && r.kind == HNwd::RedlineKind::Text);
    CHECK(r.text == "Anteckning" && r.points[0] == 0.25 && r.points[1] == -0.5 && r.colour[1] == 0.5);
    CHECK(doc.saved_view_redline(0, 1, r) && r.kind == HNwd::RedlineKind::Line);
    CHECK(r.points[3] == 0.125 && r.colour[2] == 1);
    CHECK(!doc.saved_view_redline(0, 2, r) && !doc.saved_view_redline(1, 0, r));
    HNwd::SavedView r_view;
    CHECK(!doc.saved_view(4, r_view));

    // Rutnäten.
    CHECK(doc.load_grids() == Status::Ok);
    CHECK(doc.grid_count() == 2);
    HNwd::GridSystem g;
    CHECK(doc.grid(0, g) && g.name == "Hus A" && g.line_count == 2 && g.level_count == 2 && g.level == 1);
    CHECK(g.origin[0] == 100 && g.axis_x[1] == 1 && g.axis_y[0] == -1);
    HNwd::GridLine l;
    CHECK(doc.grid_line(0, 0, l) && l.label == "1" && l.ends[3] == 25);
    // (0, −5) och (0, 25): origo + x · (0, 1, 0) + y · (−1, 0, 0).
    CHECK(l.world[0][0] == 105 && l.world[0][1] == 200 && l.world[0][2] == 3);
    CHECK(l.world[1][0] == 75 && l.world[1][1] == 200);
    CHECK(doc.grid_line(0, 1, l) && l.label == "B");
    CHECK(l.world[0][0] == 92 && l.world[0][1] == 195 && l.world[1][1] == 220);
    CHECK(!doc.grid_line(0, 2, l));
    HNwd::GridLevel lv;
    CHECK(doc.grid_level(0, 1, lv) && lv.name == "Plan 1" && lv.elevation == 3.25);
    CHECK(doc.grid(1, g) && g.name == "Utan" && g.line_count == 0 && g.level == -1);
    CHECK(!doc.grid_level(1, 0, lv) && !doc.grid(2, g));

    // Typsnitten: listans ordning, med bakåtpekaren.
    CHECK(doc.load_fonts() == Status::Ok && doc.font_count() == 3);
    HNwd::Font font;
    CHECK(doc.font(0, font) && font.name == "Rubrik" && font.size == 3.5);
    CHECK(doc.font(1, font) && font.name == "Brödtext" && font.size == 1.75);
    CHECK(doc.font(2, font) && font.name == "Rubrik");
    CHECK(!doc.font(3, font));

    // Scenens material: två poster delar en beskrivning och dess bild.
    CHECK(doc.load_scene_materials() == Status::Ok && doc.scene_material_count() == 3);
    HNwd::SceneMaterial m0, m1, m2;
    CHECK(doc.scene_material(0, m0) && doc.scene_material(1, m1) && doc.scene_material(2, m2));
    CHECK(m0.name == "Tegel" && guid_is(m0.guid, 0x80) && m0.has_material);
    CHECK(m0.material.json == kMaterialJson);
    CHECK(m1.name == "Tegel 2" && m1.has_material && m1.material.json.data() == m0.material.json.data());
    CHECK(m2.name == "Tom" && !m2.has_material);
    CHECK(doc.memory_bytes() > before);

    // Uppslagen.
    HNwd::Background bg;
    CHECK(doc.background(bg) == Status::Ok);
    CHECK(close_to(bg.colours[2][1], 65 / 255.0) && close_to(bg.colours[6][2], 190 / 255.0));
    CHECK(!bg.has_sky && bg.sky_json.empty());
    std::vector<HNwd::Light> lights;
    CHECK(doc.lights(lights) == Status::Ok && lights.size() == 1);
    if (lights.size() == 1) {
        CHECK(lights[0].name == "Sol" && guid_is(lights[0].guid, 0xB0));
        CHECK(lights[0].direction[1] == 0.6 && lights[0].direction[2] == 0.8);
        CHECK(lights[0].specular[0] == 0.5 && lights[0].specular[2] == 0.125);
        CHECK(lights[0].json == kMaterialJson);
    }
    HNwd::FileIdentity id;
    CHECK(doc.file_identity(id) == Status::Ok);
    CHECK(id.has_scene && id.saved_path == "D:\\projekt\\hus.nwd" && guid_is(id.scene_guid, 0xC0));
    CHECK(!id.has_file_info && id.package_prefix.empty());
    std::string log;
    CHECK(doc.load_log(log) == Status::Ok && log == "Läst utan fel.\n");

    // Efter close() är inget kvar.
    doc.close();
    CHECK(!doc.current_view(cam) && doc.saved_view_count() == 0 && doc.grid_count() == 0);
    CHECK(doc.load_views() == Status::NotLoaded);
    return fails;
}

// En fil utan något av paketen: tomt, inte fel; uppslagen ger Missing.
int test_views_missing() {
    int fails = 0;
    std::vector<testdata::Package> p;
    p.push_back(pkg("LcOpNwdStats", stats_package("")));
    Opened o("hnwd_test_views_empty.nwd", testdata::build_file(p, true));
    CHECK(o.status == Status::Ok);
    Document& doc = o.doc;
    HNwd::Camera cam;
    CHECK(doc.load_views() == Status::Ok && !doc.current_view(cam) && !doc.home_view(cam));
    CHECK(doc.saved_view_count() == 0);
    CHECK(doc.load_grids() == Status::Ok && doc.grid_count() == 0);
    CHECK(doc.load_fonts() == Status::Ok && doc.font_count() == 0);
    CHECK(doc.load_scene_materials() == Status::Ok && doc.scene_material_count() == 0);
    HNwd::Background bg;
    CHECK(doc.background(bg) == Status::Missing && contains(doc.last_error(), "LcOpBackgroundElement"));
    std::vector<HNwd::Light> lights;
    CHECK(doc.lights(lights) == Status::Missing);
    HNwd::FileIdentity id;
    CHECK(doc.file_identity(id) == Status::Missing);
    std::string log = "x";
    CHECK(doc.load_log(log) == Status::Ok && log.empty());
    return fails;
}

// Former som inte avkodas, trasiga paket och två paket med samma namn.
int test_views_errors() {
    int fails = 0;
    {
        // En vy i form 10: Unsupported, och ingenting av vyerna hålls kvar.
        std::vector<testdata::Package> p;
        p.push_back(pkg("LcOpCurrentViewElement", view_package(10, kPos, kIdentity, 1)));
        p.push_back(pkg("LcOpSavedViewsElement", saved_views_package()));
        Opened o("hnwd_test_views_form.nwd", testdata::build_file(p, true));
        CHECK(o.status == Status::Ok);
        CHECK(o.doc.load_views() == Status::Unsupported);
        CHECK(contains(o.doc.last_error(), "LcOpCurrentViewElement at byte "));
        CHECK(contains(o.doc.last_error(), "view form"));
        HNwd::Camera cam;
        CHECK(!o.doc.current_view(cam) && o.doc.saved_view_count() == 0);
        // Ett nytt anrop läser om och ger samma fel.
        CHECK(o.doc.load_views() == Status::Unsupported);
    }
    {
        // Ett påslaget snittplan vars normal inte har längden 1: Unsupported.
        std::vector<testdata::Package> p;
        p.push_back(pkg("LcOpHomeViewElement", view_package(9, kPos, kIdentity, 1, Clip::BadNormal)));
        Opened o("hnwd_test_views_clip.nwd", testdata::build_file(p, true));
        CHECK(o.status == Status::Ok);
        CHECK(o.doc.load_views() == Status::Unsupported);
        CHECK(contains(o.doc.last_error(), "LcOpHomeViewElement"));
        CHECK(contains(o.doc.last_error(), "clip plane 0"));
        HNwd::Camera cam;
        CHECK(!o.doc.home_view(cam));
    }
    {
        // Rutnätet avhugget: Corrupt.
        Bytes g = grid_package();
        g.resize(g.size() - 16);
        std::vector<testdata::Package> p;
        p.push_back(pkg("LcOpGridElement", g));
        Opened o("hnwd_test_views_grid.nwd", testdata::build_file(p, true));
        CHECK(o.doc.load_grids() == Status::Corrupt && o.doc.grid_count() == 0);
        CHECK(contains(o.doc.last_error(), "LcOpGridElement"));
    }
    {
        // Två paket LcOpNwdSharedNodes: Corrupt.
        std::vector<testdata::Package> p;
        p.push_back(pkg("LcOpNwdSharedNodes", fonts_package()));
        p.push_back({"other\\LcOpNwdSharedNodes", 1, 0, 0, testdata::zlib_store(fonts_package())});
        Opened o("hnwd_test_views_dup.nwd", testdata::build_file(p, true));
        CHECK(o.status == Status::Ok);
        CHECK(o.doc.load_fonts() == Status::Corrupt && o.doc.font_count() == 0);
        CHECK(contains(o.doc.last_error(), "2 packages named LcOpNwdSharedNodes"));
    }
    {
        // Ett ljus av ett annat slag: Unsupported.
        Bytes l = lights_package();
        l[4] = 84;
        std::vector<testdata::Package> p;
        p.push_back(pkg("LcOpLightsElement", l));
        Opened o("hnwd_test_views_light.nwd", testdata::build_file(p, true));
        std::vector<HNwd::Light> lights;
        CHECK(o.doc.lights(lights) == Status::Unsupported && lights.empty());
        CHECK(contains(o.doc.last_error(), "light kind is not 83"));
    }
    return fails;
}

// ---------------------------------------------------------------------------
// Exempelfilerna.

std::string base_name(const char* path) {
    const std::string p = path;
    const size_t slash = p.find_last_of("/\\");
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

int check_sample(const char* path, bool nwd) {
    int fails = 0;
    Document doc;
    CHECK(doc.open(path) == Status::Ok);
    CHECK(doc.load_views() == Status::Ok);
    HNwd::Camera cam;
    CHECK(doc.current_view(cam));
    const double* q = cam.rotation;
    CHECK(std::fabs(std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]) - 1) < 1e-9);
    CHECK(cam.form == (nwd ? 843 : 9) && cam.has_target);
    // Inget snittplan påslaget; varje plan har punkten i origo och normalen (0, 1, 0).
    CHECK(HNwd::clip_plane_count(cam) == 0);
    for (const HNwd::ClipPlane& c : cam.clip) {
        CHECK(!c.enabled && c.point[0] == 0 && c.point[1] == 0 && c.point[2] == 0);
        CHECK(c.normal[0] == 0 && c.normal[1] == 1 && c.normal[2] == 0);
    }
    HNwd::Camera home;
    CHECK(!doc.home_view(home));
    CHECK(doc.saved_view_count() == 0);
    CHECK(doc.load_grids() == Status::Ok && doc.grid_count() == 0);
    CHECK(doc.load_fonts() == Status::Ok && doc.font_count() == 0);
    CHECK(doc.load_scene_materials() == Status::Ok && doc.scene_material_count() == 0);

    if (!nwd) {
        // Läget + avståndet längs blickriktningen är fragmentlådans mitt.
        CHECK(doc.load_fragments() == Status::Ok);
        double mn[3] = {HUGE_VAL, HUGE_VAL, HUGE_VAL}, mx[3] = {-HUGE_VAL, -HUGE_VAL, -HUGE_VAL};
        for (size_t i = 0; i < doc.fragment_count(); ++i) {
            HNwd::BoxD b;
            if (!doc.fragment_world_box(i, b)) continue;
            for (int k = 0; k < 3; ++k) {
                mn[k] = std::fmin(mn[k], b.min[k]);
                mx[k] = std::fmax(mx[k], b.max[k]);
            }
        }
        const double diag = std::sqrt((mx[0] - mn[0]) * (mx[0] - mn[0]) +
                                      (mx[1] - mn[1]) * (mx[1] - mn[1]) +
                                      (mx[2] - mn[2]) * (mx[2] - mn[2]));
        for (int k = 0; k < 3; ++k)
            CHECK(std::fabs(cam.target[k] - (mn[k] + mx[k]) / 2) <= 1e-9 * diag);
        HNwd::Background bg;
        CHECK(doc.background(bg) == Status::Missing);
    } else {
        HNwd::Background bg;
        CHECK(doc.background(bg) == Status::Ok && !bg.has_sky);
        // (105, 140, 230) i 255-delar.
        CHECK(close_to(bg.colours[1][0], 105 / 255.0) && close_to(bg.colours[1][2], 230 / 255.0));
        std::vector<HNwd::Light> lights;
        CHECK(doc.lights(lights) == Status::Ok && lights.size() == 2);
        for (const HNwd::Light& l : lights) {
            const double* d = l.direction;
            CHECK(std::fabs(std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) - 1) < 1e-9);
            CHECK(!l.json.empty());
        }
    }

    HNwd::FileIdentity id;
    CHECK(doc.file_identity(id) == Status::Ok && id.has_scene && id.has_file_info);
    const std::string name = base_name(path);
    CHECK(id.saved_path.size() > name.size() &&
          id.saved_path.compare(id.saved_path.size() - name.size(), name.size(), name) == 0);
    CHECK(nwd || id.package_prefix == "cache");
    std::string log;
    CHECK(doc.load_log(log) == Status::Ok && !log.empty());
    return fails;
}

int test_views_samples() {
    int fails = 0;
    for (const char* path : testdata::kSampleNwc) {
        if (!testdata::exists(path)) {
            std::printf("  SKIP — %s saknas\n", path);
            ++g_skipped;
            continue;
        }
        fails += check_sample(path, false);
    }
    SKIP_IF_MISSING(testdata::kSampleNwd);
    fails += check_sample(testdata::kSampleNwd, true);
    return fails;
}

// Ett material med JSON-texten `json`.
Bytes one_material_package(const std::string& json) {
    ByteWriter w;
    w.i32(1);
    w.i32(80);
    w.str("M");
    w.i32(0);
    guid_bytes(w, 0x10);
    w.i32(0);
    w.obj(185);
    w.str(json);
    w.null_str();
    w.i32(0);
    w.pad(8);
    return w.data;
}

// Taken i Options för de små paketen och för materialens JSON, och minnet för
// många tomma mappar.
int test_views_limits() {
    int fails = 0;
    const Bytes full = full_file();

    // Ett tak på 32 byte: varje paket utom loggen är för stort, och felet
    // namnger fältet. max_stream_bytes gäller inte de små paketen.
    {
        HNwd::Options small;
        small.max_element_bytes = 32;
        Opened o("hnwd_test_views_element_limit.nwd", full, small);
        CHECK(o.status == Status::Ok);
        Document& doc = o.doc;
        CHECK(doc.load_views() == Status::LimitExceeded &&
              contains(doc.last_error(), "Options::max_element_bytes"));
        CHECK(doc.saved_view_count() == 0);
        CHECK(doc.load_grids() == Status::LimitExceeded &&
              contains(doc.last_error(), "Options::max_element_bytes"));
        CHECK(doc.load_fonts() == Status::LimitExceeded);
        CHECK(doc.load_scene_materials() == Status::LimitExceeded);
        HNwd::Background bg;
        CHECK(doc.background(bg) == Status::LimitExceeded);
        std::vector<HNwd::Light> lights;
        CHECK(doc.lights(lights) == Status::LimitExceeded);
        HNwd::FileIdentity id;
        CHECK(doc.file_identity(id) == Status::LimitExceeded);
        std::string log;
        CHECK(doc.load_log(log) == Status::Ok && log == "Läst utan fel.\n");

        HNwd::Options stream;
        stream.max_stream_bytes = 64;
        Opened s("hnwd_test_views_stream_limit.nwd", full, stream);
        CHECK(s.status == Status::Ok);
        CHECK(s.doc.load_views() == Status::Ok && s.doc.saved_view_count() == 4);
        CHECK(s.doc.load_grids() == Status::Ok && s.doc.load_scene_materials() == Status::Ok);
    }

    // Ett material med 2 MB JSON går under förvalet, och ger LimitExceeded med
    // fältets namn när taket sänks under textens längd.
    {
        const std::string json = "{\"version\":2,\"userassets\":[],\"materials\":{},\"x\":\"" +
                                  std::string(size_t(2) << 20, 'a') + "\"}";
        std::vector<testdata::Package> p;
        p.push_back(pkg("LcReMaterialElement", one_material_package(json)));
        const Bytes file = testdata::build_file(p, true);
        Opened o("hnwd_test_views_json.nwd", file);
        CHECK(o.status == Status::Ok);
        CHECK(o.doc.load_scene_materials() == Status::Ok && o.doc.scene_material_count() == 1);
        HNwd::Options small;
        small.max_material_json_bytes = size_t(1) << 20;
        Opened l("hnwd_test_views_json_limit.nwd", file, small);
        CHECK(l.status == Status::Ok);
        CHECK(l.doc.load_scene_materials() == Status::LimitExceeded &&
              contains(l.doc.last_error(), "Options::max_material_json_bytes"));
    }

    // Många tomma mappar, 36 byte var i strömmen: load_views() håller inte
    // mer än några gånger strömmen.
    {
        const int32_t n = 100000;
        ByteWriter w;
        w.i32(n);
        for (int32_t k = 0; k < n; ++k) {
            w.i32(4);
            w.str("");
            w.i32(0);
            guid_bytes(w, 0);
            w.i32(0);
            w.i32(0);
        }
        w.pad(8);
        std::vector<testdata::Package> p;
        p.push_back(pkg("LcOpSavedViewsElement", w.data));
        Opened o("hnwd_test_views_folders.nwd", testdata::build_file(p, true));
        CHECK(o.status == Status::Ok);
        const size_t before = o.doc.memory_bytes();
        CHECK(o.doc.load_views() == Status::Ok && o.doc.saved_view_count() == static_cast<size_t>(n));
        const size_t grew = o.doc.memory_bytes() - before;
        if (grew > 4 * w.data.size())
            std::printf("  %d empty folders: %zu bytes from a %zu-byte stream\n", n, grew, w.data.size());
        CHECK(grew <= 4 * w.data.size());
        HNwd::SavedView v;
        CHECK(o.doc.saved_view(static_cast<size_t>(n) - 1, v) && v.kind == HNwd::SavedViewKind::Folder &&
              v.next_sibling == HNwd::kNone && v.redline_count == 0);
        HNwd::Redline r;
        CHECK(!o.doc.saved_view_redline(0, 0, r));
    }
    return fails;
}

} // namespace

int test_views() {
    std::printf("test_views\n");
    int fails = 0;
    fails += test_views_synthetic();
    fails += test_views_missing();
    fails += test_views_errors();
    fails += test_views_limits();
    fails += test_views_samples();
    return fails;
}
