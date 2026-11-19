// HNwd::Document: allt utom trädet. Syntetiska filer med geometri, fragment,
// GUID-tabell, källhuvud och XRef-tabell, deras fel, och exempelfilerna.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
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

// Alla fragment och utseenden i dokumentet, i ordning; tomma före load_fragments().
std::vector<HNwd::Fragment> all_fragments(const Document& doc) {
    std::vector<HNwd::Fragment> out(doc.fragment_count());
    for (size_t i = 0; i < out.size(); ++i) doc.fragment(i, out[i]);
    return out;
}

std::vector<HNwd::Look> all_looks(const Document& doc) {
    std::vector<HNwd::Look> out(doc.look_count());
    for (size_t i = 0; i < out.size(); ++i) doc.look(i, out[i]);
    return out;
}

// ---------------------------------------------------------------------------
// Filer.

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

// En temporärfil som tas bort när den går ur räckvidd. Dokumentet som öppnat
// den ska deklareras efter den, så att det stängs först.
struct TempFile {
    std::string path;
    TempFile(const char* name, const Bytes& bytes) : path(testdata::tmp_path(name)) {
        write_file(path, bytes);
    }
    ~TempFile() { testdata::remove_utf8(path); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
};

// En fil, ett dokument och resultatet av open().
struct Opened {
    TempFile file;
    Document doc;
    Status status;
    Opened(const char* name, const Bytes& bytes)
        : file(name, bytes), status(doc.open(file.path.c_str())) {}
};

bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// ---------------------------------------------------------------------------
// Geometriposterna R1 (två trianglar) och L1 (ett linjesegment med färg).

Bytes ints(std::initializer_list<int32_t> v) {
    ByteWriter w;
    for (const int32_t x : v) w.i32(x);
    return w.data;
}

Bytes cat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const Bytes& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

Bytes hx(const char* s) {
    ByteWriter w;
    w.hex(s);
    return w.data;
}

Bytes record_head(int32_t cls, int32_t p1, const std::vector<float>& pos) {
    ByteWriter w;
    w.i32(100);
    w.i32(cls);
    w.i32(101);
    w.i32(60);
    w.i32(p1);
    w.i32(static_cast<int32_t>(pos.size()));
    for (const float f : pos) w.f32(f);
    return w.data;
}

// Fyra hörn i en strip: trianglarna (0,1,2) och (2,1,3), alla normaler +Z.
Bytes r1() {
    ByteWriter w;
    w.raw(record_head(94, 4, {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0}));
    w.i32(0);
    w.raw(cat({ints({102, 100, -4, 0}), hx("D4 D4 D4 D4")}));
    w.i32(0);
    w.raw(ints({1, 4, 1, 0x40000000}));
    w.i32(0);
    w.pad(8);
    return w.data;
}

// Två hörn, ett segment, färgen 0xFF0000FF på båda.
Bytes l1() {
    ByteWriter w;
    w.raw(record_head(95, 2, {0, 0, 0, 10, 0, 0}));
    w.i32(0);
    w.i32(0);
    w.raw(ints({102, 58, 5634, -2, 2}));
    w.hex("FF0000FF FF0000FF");
    w.raw(ints({1, 2, 1, static_cast<int32_t>(0x80000000u)}));
    w.pad(8);
    return w.data;
}

// En cirkel i XY-planet med radien 2: 32 segment, lådan (-2, -2, 0) .. (2, 2, 0).
Bytes circle2() {
    ByteWriter w;
    w.raw(ints({100, 104, 3}));
    for (const float f : {0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 2.f}) w.f32(f);
    w.i32(0);
    return w.data;
}

// Geometripaketet med R1 och en andra post, L1 om inget annat ges, i en chunk.
Bytes geometry_package(int32_t& b, const Bytes& second) {
    const Bytes a = r1(), c = second.empty() ? l1() : second;
    Bytes chunk = a;
    chunk.insert(chunk.end(), c.begin(), c.end());
    ByteWriter toc;
    toc.i32(2);
    toc.i32(1);
    toc.i32(static_cast<int32_t>(testdata::zlib_store(chunk).size()));
    toc.i32(static_cast<int32_t>(a.size()));
    toc.i32(static_cast<int32_t>(c.size()));
    toc.pad(8);
    return testdata::prefixed(65536, {chunk}, toc.data, b);
}

// ---------------------------------------------------------------------------
// Fragmenten.

struct Frag {
    int pose_class = 15;   // 14, 15, 16 eller 0 för en null-pose
    double t[3] = {0, 0, 0};
    double q[4] = {0, 0, 0, 1};
    double s = 1;
    int32_t seq = 2, a = 2, record = 1;
    float bbox[6] = {0, 0, 0, 1, 1, 0};
    float diffuse = 0.5f;
    int app_from = -1;    // index för ett tidigare fragment vars utseende delas
    int geom_from = -1;   // index för ett tidigare fragment vars geometrireferens delas
    const char* material_json = nullptr;   // ger utseendet en klass 185 med den här texten
    int material_from = -1;   // index för ett tidigare fragment vars klass 185 delas
};

constexpr double kSqrtHalf = 0.70710678118654752440;

std::vector<Frag> default_frags() {
    std::vector<Frag> v(3);
    // Ett: pose 15, 90 grader kring Z.
    v[0].pose_class = 15;
    v[0].t[0] = 10, v[0].t[1] = 20, v[0].t[2] = 30;
    v[0].q[2] = kSqrtHalf, v[0].q[3] = kSqrtHalf;
    v[0].seq = 2;
    // Två: speglat, klass 16 med s = -1, delar utseende och geometri med det första.
    v[1].pose_class = 16;
    v[1].t[0] = 1, v[1].t[1] = 2, v[1].t[2] = 3;
    v[1].s = -1;
    v[1].seq = 7;
    v[1].app_from = 0;
    v[1].geom_from = 0;
    // Tre: ingen pose, linjeposten, eget utseende.
    v[2].pose_class = 0;
    v[2].seq = 7;
    v[2].a = 1;
    v[2].record = 2;
    v[2].bbox[3] = 10;
    v[2].bbox[4] = 0;
    v[2].diffuse = 0.25f;
    return v;
}

Bytes fragment_stream(const std::vector<Frag>& frags) {
    ByteWriter w;
    w.i32(static_cast<int32_t>(frags.size()));
    std::vector<int32_t> app_ids, geom_ids, material_ids;
    for (const Frag& f : frags) {
        w.obj(13);
        w.f64(1.5);
        w.i32(f.a);
        w.i32(4);
        w.i32(7);
        w.i32(f.seq);

        if (f.pose_class == 0) {
            w.null_ptr();
        } else {
            w.obj(f.pose_class);
            for (const double v : f.t) w.f64(v);
            if (f.pose_class >= 15)
                for (const double v : f.q) w.f64(v);
            if (f.pose_class == 16) w.f64(f.s);
        }

        if (f.app_from >= 0) {
            w.ref(app_ids[static_cast<size_t>(f.app_from)]);
            app_ids.push_back(app_ids[static_cast<size_t>(f.app_from)]);
            material_ids.push_back(material_ids[static_cast<size_t>(f.app_from)]);
        } else {
            app_ids.push_back(w.obj(55));
            const bool has_material = f.material_json || f.material_from >= 0;
            w.i32(has_material ? 33 : 1);
            w.obj(54);
            const float d = f.diffuse;
            const float mat[14] = {0.125f, 0.125f, 0.125f, d, d, d, 0.5f, 0.5f, 0.5f, 0, 0, 0, 32, 0.75f};
            for (const float v : mat) w.f32(v);
            if (f.material_from >= 0) {
                w.ref(material_ids[static_cast<size_t>(f.material_from)]);
                material_ids.push_back(material_ids[static_cast<size_t>(f.material_from)]);
            } else if (f.material_json) {
                // JSON, en sträng, och bilden "tak.png" inbäddad med sex byte.
                material_ids.push_back(w.obj(185));
                w.str(f.material_json);
                w.str("");
                w.i32(1);
                w.str("tak.png");
                w.i32(1);
                w.str("navis");
                w.str("png");
                w.i64a(6);
                w.hex("89504E470D0A");
                w.pad(4);
            } else {
                material_ids.push_back(0);
            }
        }

        if (f.geom_from >= 0) {
            w.ref(geom_ids[static_cast<size_t>(f.geom_from)]);
            geom_ids.push_back(geom_ids[static_cast<size_t>(f.geom_from)]);
        } else {
            geom_ids.push_back(w.obj(93));
            for (const float v : f.bbox) w.f32(v);
            for (int i = 0; i < 3; ++i) w.f64(0.5);
            w.f32(2.5f);
            w.i32(7);
            w.i32(f.record);
        }
    }
    w.pad(8);
    return w.data;
}

// ---------------------------------------------------------------------------
// Hela filer.

constexpr char kXrefText[] = "{\"Type\":\"XRefTable\",\"Items\":[]}";
constexpr char kLoader[] = "LcNwcLoaderPlugin:IFC";
constexpr char kSourcePath[] = "C:\\models\\a.ifc";

struct Variant {
    std::vector<Frag> frags = default_frags();
    bool geometry = true, fragments = true, guids = true, source = true, xref = true;
    int32_t guid_flags = 1;
    bool dup_guid = false;
    Bytes guid_bytes;   // tom: två GUID
    Bytes xref_bytes;   // tom: kXrefText
    Bytes second_record;   // tom: L1
    bool partition = true;
    Bytes partition_bytes;   // tom: åtta nollor, som inte går att tolka
    Bytes spatial_bytes;     // tom: inget LcOpNwdSpatialHierarchy
};

// En partitionsström (LcOpNwdPartition) utan attribut och ljus, med enhetskoden `unit`.
Bytes partition_with_unit(int32_t unit) {
    ByteWriter w;
    w.obj(32);
    w.i32(0);
    w.str("a.ifc");
    w.obj(52);
    w.str("File");
    w.str("LcOaPartition");
    w.i32(0);   // flags
    w.i32(0);   // element
    w.i32(0);   // attribut
    w.i32(2);
    w.i32(0);   // ljus
    for (int i = 0; i < 3; ++i) w.f64(0);
    w.i32(0);
    w.str(kLoader);
    w.i32(448);
    w.i32(unit);
    w.i32(1);
    for (const double v : {0.0, 0.0, 1.0, 0.0, 0.0, 0.0}) w.f64(v);
    w.i32(0);
    w.i32(-1);
    w.i32(1);
    w.str(kSourcePath);
    for (const double v : {0.0, 0.0, 0.0, 1.0, 1.0, 0.0, 0.0}) w.f64(v);
    w.i32(1);
    w.pad(8);
    return w.data;
}

Bytes guid_bytes_default() {
    ByteWriter w;
    w.i32(1);
    w.i32(2);
    for (uint8_t i = 0; i < 32; ++i) w.u8(i);
    return w.data;
}

Bytes xref_bytes_default() {
    ByteWriter w;
    w.i32(static_cast<int32_t>(std::strlen(kXrefText)));
    w.text(kXrefText);
    w.pad(8);
    return w.data;
}

Bytes source_bytes() {
    ByteWriter w;
    w.i64a(7);
    w.str(kSourcePath);
    w.i32(-1);
    w.i64a(1700000000);
    w.i64a(12345);
    w.str(kLoader);
    w.i32(5);   // inställningarna efter loader-namnet tolkas inte
    w.pad(8);
    return w.data;
}

Bytes build(const Variant& v) {
    using testdata::Package;
    using testdata::zlib_store;
    std::vector<Package> pk;
    pk.push_back({"cache\\LcOpNwdLogicalHierarchy", 1, 0, 0, zlib_store(Bytes(8, 0))});
    if (v.fragments) pk.push_back({"cache\\LcOpNwdFragments", 1, 0, 0, zlib_store(fragment_stream(v.frags))});
    if (v.geometry) {
        int32_t b = 0;
        const Bytes g = geometry_package(b, v.second_record);
        pk.push_back({"cache\\LcOpNwdGeometry", 1, 4, b, g});
    }
    if (v.guids) {
        const Bytes g = zlib_store(v.guid_bytes.empty() ? guid_bytes_default() : v.guid_bytes);
        pk.push_back({"cache\\LcOpGuidStore", v.guid_flags, 0, 0, g});
        if (v.dup_guid) pk.push_back({"cache\\LcOpGuidStore", 1, 0, 0, g});
    }
    {
        ByteWriter toc;
        toc.i32(1);
        toc.i32(static_cast<int32_t>(zlib_store(Bytes(8, 0)).size()));
        toc.pad(8);
        int32_t b = 0;
        pk.push_back({"cache\\LcOaPartitionProps", 1, 4, 0, Bytes()});
        pk.back().bytes = testdata::prefixed(0, {Bytes(8, 0)}, toc.data, b);
        pk.back().b = b;
    }
    if (v.partition)
        pk.push_back({"cache\\LcOpNwdPartition", 1, 0, 0,
                      zlib_store(v.partition_bytes.empty() ? Bytes(8, 0) : v.partition_bytes)});
    if (!v.spatial_bytes.empty())
        pk.push_back({"cache\\LcOpNwdSpatialHierarchy", 1, 0, 0, zlib_store(v.spatial_bytes)});
    if (v.xref)
        pk.push_back({"cache\\LcOpXRefTable", 1, 0, 0,
                      zlib_store(v.xref_bytes.empty() ? xref_bytes_default() : v.xref_bytes)});
    if (v.source) pk.push_back({"cache\\LcOpDBCache", 1, 0, 0, zlib_store(source_bytes())});
    return testdata::build_file(pk, true);
}

bool close_to(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }

// ---------------------------------------------------------------------------
// Den hela filen.

int test_whole_file() {
    int fails = 0;
    Opened o("hnwd_test_document_whole.nwd", build(Variant{}));
    Document& doc = o.doc;
    CHECK(o.status == Status::Ok);
    CHECK(*doc.last_error() == 0);
    CHECK(doc.is_open());
    if (!doc.is_open()) return fails + 1;

    // Katalogen.
    CHECK(doc.entry_count() == 8);
    HNwd::DirectoryEntry e;
    CHECK(doc.entry(0, e));
    CHECK(e.name == "cache\\LcOpNwdLogicalHierarchy");
    CHECK(e.leaf == "LcOpNwdLogicalHierarchy");
    CHECK(e.flags == 1 && e.a == 0 && e.b == 0 && e.length > 0 && e.offset > 0);
    CHECK(doc.entry(2, e));
    CHECK(e.leaf == "LcOpNwdGeometry" && e.a == 4 && e.b > 0);
    CHECK(!doc.entry(8, e));
    for (const HNwd::Package p :
         {HNwd::Package::Hierarchy, HNwd::Package::Fragments, HNwd::Package::Geometry,
          HNwd::Package::GuidStore, HNwd::Package::PartitionProps, HNwd::Package::Partition,
          HNwd::Package::XRefTable, HNwd::Package::SourceHeader})
        CHECK(doc.has(p));
    CHECK(doc.version() == 448);

    // Före varje load_…().
    HNwd::Mesh mesh;
    CHECK(doc.record_count() == 0 && doc.normal_bits() == 0);
    CHECK(doc.record_mesh(1, mesh) == Status::NotLoaded && *doc.last_error() != 0);
    CHECK(doc.fragment_count() == 0 && all_fragments(doc).empty());
    CHECK(doc.look_count() == 0 && all_looks(doc).empty());
    CHECK(doc.fragment_mesh(0, mesh) == Status::NotLoaded && *doc.last_error() != 0);
    CHECK(doc.guid_count() == 0);
    uint8_t g[16];
    CHECK(!doc.guid(0, g));

    // Geometrin.
    CHECK(doc.load_geometry() == Status::Ok);
    CHECK(*doc.last_error() == 0);
    CHECK(doc.load_geometry() == Status::Ok);   // en gång till gör ingenting
    CHECK(*doc.last_error() == 0);
    CHECK(doc.record_count() == 2);
    CHECK(doc.record_mesh(0, mesh) == Status::OutOfRange && *doc.last_error() != 0);
    CHECK(doc.record_mesh(3, mesh) == Status::OutOfRange);
    CHECK(doc.record_mesh(1, mesh) == Status::Ok);
    CHECK(mesh.kind == HNwd::MeshKind::Triangles);
    CHECK(mesh.positions == (std::vector<float>{0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0}));
    CHECK(mesh.indices == (std::vector<uint32_t>{0, 1, 2, 2, 1, 3}));
    CHECK(mesh.normals.size() == 12);
    for (size_t i = 0; i < mesh.normals.size(); ++i)
        CHECK(close_to(mesh.normals[i], i % 3 == 2 ? 1.0 : 0.0, 1e-6));
    CHECK(mesh.colors.empty());
    CHECK(mesh.bounds.min[0] == 0 && mesh.bounds.max[0] == 1 && mesh.bounds.max[1] == 1 &&
          mesh.bounds.max[2] == 0);
    CHECK(doc.record_mesh(2, mesh) == Status::Ok);
    CHECK(mesh.kind == HNwd::MeshKind::Lines);
    CHECK(mesh.positions == (std::vector<float>{0, 0, 0, 10, 0, 0}));
    CHECK(mesh.indices == (std::vector<uint32_t>{0, 1}));
    CHECK(mesh.normals.empty());
    CHECK(mesh.colors == (std::vector<uint32_t>{0xFF0000FFu, 0xFF0000FFu}));
    CHECK(mesh.bounds.max[0] == 10);
    CHECK(doc.normal_bits() == 16);   // båda posterna går med båda bredderna

    // Fragmenten.
    CHECK(doc.load_fragments() == Status::Ok);
    CHECK(*doc.last_error() == 0);
    CHECK(doc.load_fragments() == Status::Ok);
    CHECK(doc.fragment_count() == 3);
    CHECK(doc.look_count() == 2);
    const std::vector<HNwd::Fragment> f_all = all_fragments(doc);
    const HNwd::Fragment* f = f_all.empty() ? nullptr : f_all.data();
    CHECK(f != nullptr);
    if (f && doc.fragment_count() == 3) {
        CHECK(f[0].record == 1 && f[0].seq == 2 && f[0].primitives == 2 && f[0].look == 0);
        CHECK(f[0].pose.t[0] == 10 && f[0].pose.t[1] == 20 && f[0].pose.t[2] == 30);
        CHECK(f[0].pose.q[0] == 0 && f[0].pose.q[1] == 0 && f[0].pose.q[2] == kSqrtHalf &&
              f[0].pose.q[3] == kSqrtHalf && f[0].pose.s == 1);
        CHECK(f[0].bounds.min[0] == 0 && f[0].bounds.max[0] == 1 && f[0].bounds.max[1] == 1);
        CHECK(!HNwd::mirrors(f[0].pose));

        CHECK(f[1].record == 1 && f[1].seq == 7 && f[1].primitives == 2 && f[1].look == 0);
        CHECK(f[1].pose.t[0] == 1 && f[1].pose.t[1] == 2 && f[1].pose.t[2] == 3);
        CHECK(f[1].pose.q[3] == 1 && f[1].pose.s == -1);
        CHECK(HNwd::mirrors(f[1].pose));

        CHECK(f[2].record == 2 && f[2].seq == 7 && f[2].primitives == 1 && f[2].look == 1);
        CHECK(f[2].pose.t[0] == 0.5 && f[2].pose.t[1] == 0.5 && f[2].pose.t[2] == 0.5);
        CHECK(f[2].pose.q[0] == 0 && f[2].pose.q[1] == 0 && f[2].pose.q[2] == 0 &&
              f[2].pose.q[3] == 1 && f[2].pose.s == 1);
        CHECK(f[2].bounds.max[0] == 10 && f[2].bounds.max[1] == 0);
    }
    const std::vector<HNwd::Look> l_all = all_looks(doc);
    const HNwd::Look* l = l_all.empty() ? nullptr : l_all.data();
    CHECK(l != nullptr);
    if (l && doc.look_count() == 2) {
        CHECK(l[0].ambient[0] == 0.125f && l[0].diffuse[0] == 0.5f && l[0].diffuse[2] == 0.5f);
        CHECK(l[0].specular[1] == 0.5f && l[0].emissive[2] == 0 && l[0].shininess == 32 &&
              l[0].transparency == 0.75f);
        CHECK(l[1].diffuse[1] == 0.25f);
    }

    // Geometrin per fragment.
    CHECK(doc.fragment_mesh(0, mesh) == Status::Ok);
    CHECK(mesh.indices == (std::vector<uint32_t>{0, 1, 2, 2, 1, 3}));
    CHECK(doc.fragment_mesh(1, mesh) == Status::Ok);   // speglat: ordningen vänd
    CHECK(mesh.indices == (std::vector<uint32_t>{0, 2, 1, 2, 3, 1}));
    CHECK(mesh.positions == (std::vector<float>{0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0}));
    CHECK(mesh.normals.size() == 12);
    CHECK(doc.fragment_mesh(2, mesh) == Status::Ok);   // linjer lämnas
    CHECK(mesh.kind == HNwd::MeshKind::Lines);
    CHECK(mesh.indices == (std::vector<uint32_t>{0, 1}));
    CHECK(doc.fragment_mesh(3, mesh) == Status::OutOfRange && *doc.last_error() != 0);
    CHECK(doc.fragment_mesh(0, mesh) == Status::Ok && *doc.last_error() == 0);
    CHECK(mesh.indices == (std::vector<uint32_t>{0, 1, 2, 2, 1, 3}));   // posten är oförändrad

    // GUID-tabellen.
    CHECK(doc.load_guids() == Status::Ok);
    CHECK(doc.guid_count() == 2);
    CHECK(doc.guid(0, g) && g[0] == 0 && g[15] == 15);
    CHECK(doc.guid(1, g) && g[0] == 16 && g[15] == 31);
    CHECK(!doc.guid(2, g));
    CHECK(doc.load_guids() == Status::Ok);

    // Källan.
    HNwd::Source src;
    CHECK(doc.file_source(src) == Status::Ok);
    CHECK(src.path == kSourcePath && src.loader == kLoader);
    CHECK(src.unix_time == 1700000000 && src.size == 12345);
    CHECK(doc.file_source(src) == Status::Ok);
    CHECK(src.loader == kLoader);

    // XRef-tabellen.
    std::string json;
    CHECK(doc.xref_json(json) == Status::Ok);
    CHECK(json == kXrefText);
    CHECK(*doc.last_error() == 0);

    // Stäng och öppna igen.
    doc.close();
    CHECK(!doc.is_open() && doc.entry_count() == 0 && doc.fragment_count() == 0 &&
          doc.record_count() == 0 && doc.guid_count() == 0 && doc.version() == -1);
    CHECK(!doc.entry(0, e) && !doc.guid(0, g) && !doc.has(HNwd::Package::Geometry));
    CHECK(all_fragments(doc).empty() && all_looks(doc).empty());
    CHECK(*doc.last_error() == 0);
    CHECK(doc.load_geometry() == Status::NotLoaded && *doc.last_error() != 0);
    CHECK(doc.load_fragments() == Status::NotLoaded);
    CHECK(doc.load_guids() == Status::NotLoaded);
    CHECK(doc.file_source(src) == Status::NotLoaded);
    CHECK(doc.xref_json(json) == Status::NotLoaded);
    CHECK(doc.record_mesh(1, mesh) == Status::NotLoaded);
    CHECK(doc.fragment_mesh(0, mesh) == Status::NotLoaded);
    doc.close();   // en gång till är harmlöst

    CHECK(doc.open(o.file.path.c_str()) == Status::Ok);
    CHECK(doc.record_count() == 0 && doc.fragment_count() == 0);   // inget är läst
    CHECK(doc.load_fragments() == Status::Ok && doc.fragment_count() == 3);   // läser geometrin först
    CHECK(doc.record_count() == 2);
    CHECK(doc.fragment_mesh(1, mesh) == Status::Ok);
    CHECK(mesh.indices == (std::vector<uint32_t>{0, 2, 1, 2, 3, 1}));

    // Öppnar en annan fil i samma dokument: det förra stängs.
    CHECK(doc.open("hnwd-finns-inte.nwd") == Status::FileNotFound);
    CHECK(!doc.is_open() && doc.fragment_count() == 0);
    return fails;
}

// ---------------------------------------------------------------------------
// Move.

int test_move() {
    int fails = 0;
    Opened o("hnwd_test_document_move.nwd", build(Variant{}));
    CHECK(o.status == Status::Ok);

    Document a;
    CHECK(a.open(o.file.path.c_str()) == Status::Ok);
    CHECK(a.load_fragments() == Status::Ok);
    HNwd::Fragment before;
    CHECK(a.fragment(1, before));

    Document b(std::move(a));
    HNwd::Fragment after;
    CHECK(b.is_open() && b.fragment_count() == 3 && b.fragment(1, after));
    CHECK(after.record == before.record && after.seq == before.seq && after.pose.s == before.pose.s);
    HNwd::Mesh mesh;
    CHECK(b.fragment_mesh(1, mesh) == Status::Ok);
    CHECK(mesh.indices == (std::vector<uint32_t>{0, 2, 1, 2, 3, 1}));
    CHECK(b.has(HNwd::Package::GuidStore) && b.entry_count() == 8);
    CHECK(b.load_guids() == Status::Ok && b.guid_count() == 2);

    // Den som flyttats ifrån är stängd men går att använda igen.
    CHECK(!a.is_open() && a.fragment_count() == 0 && a.entry_count() == 0);
    CHECK(a.load_geometry() == Status::NotLoaded);
    CHECK(*a.last_error() != 0);
    CHECK(a.open(o.file.path.c_str()) == Status::Ok);
    CHECK(a.load_geometry() == Status::Ok && a.record_count() == 2);

    Document c;
    c = std::move(b);
    CHECK(c.is_open() && c.fragment_count() == 3 && c.guid_count() == 2);
    CHECK(c.fragment_mesh(0, mesh) == Status::Ok);
    CHECK(mesh.indices == (std::vector<uint32_t>{0, 1, 2, 2, 1, 3}));
    CHECK(!b.is_open());
    CHECK(b.open(o.file.path.c_str()) == Status::Ok);
    return fails;
}

// ---------------------------------------------------------------------------
// Filer som inte går att öppna.

int test_open_errors() {
    int fails = 0;
    {
        Document doc;
        CHECK(doc.open("hnwd-finns-inte.nwd") == Status::FileNotFound);
        CHECK(*doc.last_error() != 0 && !doc.is_open());
        CHECK(doc.open(nullptr) == Status::FileNotFound);
        CHECK(doc.open("") == Status::FileNotFound);
    }
    {
        Bytes text;
        const std::string s = "This is an ordinary text file and not a Navisworks file at all, "
                              "although it is long enough to hold a section header.\n";
        text.assign(s.begin(), s.end());
        Opened o("hnwd_test_document_text.nwd", text);
        CHECK(o.status == Status::NotNavisworks);
        CHECK(*o.doc.last_error() != 0 && !o.doc.is_open());
    }
    {
        ByteWriter w;
        w.text(testdata::kDataHeader);
        w.zeros(300);
        Opened o("hnwd_test_document_nodir.nwd", w.data);
        CHECK(o.status == Status::NoDirectory);
        CHECK(*o.doc.last_error() != 0 && !o.doc.is_open());
    }
    {
        // Samma namn två gånger: öppningen säger vilket.
        Variant v;
        v.dup_guid = true;
        Opened o("hnwd_test_document_dup.nwd", build(v));
        CHECK(o.status == Status::Corrupt);
        CHECK(contains(o.doc.last_error(), "LcOpGuidStore"));
        CHECK(!o.doc.is_open());
        CHECK(o.doc.entry_count() == 0);
    }
    return fails;
}

// ---------------------------------------------------------------------------
// Paket som saknas, och paket med fel form eller innehåll.

int test_missing_and_corrupt_packages() {
    int fails = 0;
    {
        Variant v;
        v.guids = false;
        v.source = false;
        v.xref = false;
        Opened o("hnwd_test_document_missing.nwd", build(v));
        CHECK(o.status == Status::Ok);
        CHECK(!o.doc.has(HNwd::Package::GuidStore) && !o.doc.has(HNwd::Package::SourceHeader) &&
              !o.doc.has(HNwd::Package::XRefTable));
        CHECK(o.doc.has(HNwd::Package::Geometry));
        CHECK(o.doc.load_guids() == Status::Missing && contains(o.doc.last_error(), "LcOpGuidStore"));
        HNwd::Source src;
        CHECK(o.doc.file_source(src) == Status::Missing && contains(o.doc.last_error(), "LcOpDBCache"));
        std::string json;
        CHECK(o.doc.xref_json(json) == Status::Missing && contains(o.doc.last_error(), "LcOpXRefTable"));
        CHECK(o.doc.load_fragments() == Status::Ok);   // det som finns fungerar
    }
    {
        Variant v;
        v.geometry = false;
        Opened o("hnwd_test_document_nogeom.nwd", build(v));
        CHECK(o.status == Status::Ok);
        CHECK(o.doc.load_geometry() == Status::Missing && contains(o.doc.last_error(), "LcOpNwdGeometry"));
        CHECK(o.doc.load_fragments() == Status::Missing && contains(o.doc.last_error(), "LcOpNwdGeometry"));
        CHECK(o.doc.fragment_count() == 0);
        HNwd::Mesh mesh;
        CHECK(o.doc.record_mesh(1, mesh) == Status::NotLoaded);
    }
    {
        Variant v;
        v.fragments = false;
        Opened o("hnwd_test_document_nofrag.nwd", build(v));
        CHECK(o.status == Status::Ok);
        CHECK(o.doc.load_fragments() == Status::Missing && contains(o.doc.last_error(), "LcOpNwdFragments"));
    }
    {
        // Fel form: råa byte i stället för zlib.
        Variant v;
        v.guid_flags = 3;
        Opened o("hnwd_test_document_form.nwd", build(v));
        CHECK(o.status == Status::Ok);   // formen prövas när paketet används
        CHECK(o.doc.load_guids() == Status::Corrupt && contains(o.doc.last_error(), "LcOpGuidStore"));
        CHECK(o.doc.guid_count() == 0);
    }
    {
        // GUID-tabellens längd stämmer inte.
        Variant v;
        ByteWriter w;
        w.i32(1);
        w.i32(3);
        w.zeros(32);
        v.guid_bytes = w.data;
        Opened o("hnwd_test_document_guidlen.nwd", build(v));
        CHECK(o.status == Status::Ok);
        CHECK(o.doc.load_guids() == Status::Corrupt && *o.doc.last_error() != 0);
        CHECK(o.doc.guid_count() == 0);
    }
    {
        // XRef-tabellens längd ryms inte.
        Variant v;
        ByteWriter w;
        w.i32(100);
        w.text("{}");
        w.pad(8);
        v.xref_bytes = w.data;
        Opened o("hnwd_test_document_xreflen.nwd", build(v));
        std::string json;
        CHECK(o.doc.xref_json(json) == Status::Corrupt && contains(o.doc.last_error(), "LcOpXRefTable"));
    }
    {
        // Resten efter texten är inte nollor.
        Variant v;
        ByteWriter w;
        w.i32(2);
        w.text("{}");
        w.u8(0);
        w.u8(7);
        w.pad(8);
        v.xref_bytes = w.data;
        Opened o("hnwd_test_document_xrefpad.nwd", build(v));
        std::string json;
        CHECK(o.doc.xref_json(json) == Status::Corrupt && contains(o.doc.last_error(), "padding"));
    }
    {
        // För mycket efter texten.
        Variant v;
        ByteWriter w;
        w.i32(2);
        w.text("{}");
        w.zeros(8);
        v.xref_bytes = w.data;
        Opened o("hnwd_test_document_xrefmore.nwd", build(v));
        std::string json;
        CHECK(o.doc.xref_json(json) == Status::Corrupt);
    }
    {
        // En tom text och bara nollor därefter är giltigt.
        Variant v;
        ByteWriter w;
        w.i32(0);
        w.zeros(4);
        v.xref_bytes = w.data;
        Opened o("hnwd_test_document_xrefempty.nwd", build(v));
        std::string json = "x";
        CHECK(o.doc.xref_json(json) == Status::Ok && json.empty());
    }
    return fails;
}

// ---------------------------------------------------------------------------
// Länkarna mellan fragment och geometri.

int test_link_errors() {
    int fails = 0;
    HNwd::Mesh mesh;

    {   // Lådan skiljer sig från postens: bara det fragmentet fallerar.
        Variant v;
        v.frags[2].bbox[3] = 11;
        Opened o("hnwd_test_document_box.nwd", build(v));
        CHECK(o.status == Status::Ok && o.doc.load_fragments() == Status::Ok);
        CHECK(o.doc.fragment_mesh(0, mesh) == Status::Ok);
        CHECK(o.doc.fragment_mesh(2, mesh) == Status::Corrupt);
        CHECK(contains(o.doc.last_error(), "fragment 2") && contains(o.doc.last_error(), "box"));
    }
    {   // Lådan skiljer sig med en enda bit av ett flyttal.
        Variant v;
        v.frags[0].bbox[4] = std::nextafter(1.0f, 2.0f);
        Opened o("hnwd_test_document_boxbit.nwd", build(v));
        CHECK(o.doc.load_fragments() == Status::Ok);
        CHECK(o.doc.fragment_mesh(0, mesh) == Status::Corrupt && contains(o.doc.last_error(), "fragment 0"));
        CHECK(o.doc.fragment_mesh(1, mesh) == Status::Corrupt && contains(o.doc.last_error(), "fragment 1"));
        CHECK(o.doc.fragment_mesh(2, mesh) == Status::Ok);
    }
    {   // En form har lådan uträknad: den får skilja på avrundningen men inte mer.
        const float exact[6] = {-2, -2, 0, 2, 2, 0};
        auto with_box = [&](int at, float value) {
            Variant v;
            v.second_record = circle2();
            v.frags[2].a = 32;
            for (int i = 0; i < 6; ++i) v.frags[2].bbox[i] = exact[i];
            if (at >= 0) v.frags[2].bbox[at] = value;
            return v;
        };
        Opened same("hnwd_test_document_shape.nwd", build(with_box(-1, 0)));
        CHECK(same.status == Status::Ok && same.doc.load_fragments() == Status::Ok);
        CHECK(same.doc.fragment_mesh(2, mesh) == Status::Ok);
        CHECK(mesh.kind == HNwd::MeshKind::Lines && mesh.indices.size() == 64 &&
              mesh.positions.size() == 96);
        CHECK(mesh.bounds.min[0] == -2 && mesh.bounds.max[1] == 2);

        Opened ulp("hnwd_test_document_shapeulp.nwd", build(with_box(3, std::nextafter(2.0f, 3.0f))));
        CHECK(ulp.doc.load_fragments() == Status::Ok);
        CHECK(ulp.doc.fragment_mesh(2, mesh) == Status::Ok);

        Opened off("hnwd_test_document_shapeoff.nwd", build(with_box(3, 2.00001f)));
        CHECK(off.doc.load_fragments() == Status::Ok);
        CHECK(off.doc.fragment_mesh(2, mesh) == Status::Corrupt);
        CHECK(contains(off.doc.last_error(), "fragment 2") && contains(off.doc.last_error(), "box"));

        Opened flat("hnwd_test_document_shapeflat.nwd", build(with_box(5, 0.001f)));
        CHECK(flat.doc.load_fragments() == Status::Ok);
        CHECK(flat.doc.fragment_mesh(2, mesh) == Status::Corrupt);

        Variant count = with_box(-1, 0);
        count.frags[2].a = 31;
        Opened prims("hnwd_test_document_shapeprims.nwd", build(count));
        CHECK(prims.doc.load_fragments() == Status::Ok);
        CHECK(prims.doc.fragment_mesh(2, mesh) == Status::Corrupt);
        CHECK(contains(prims.doc.last_error(), "primitives"));
    }
    {   // Antalet primitiver skiljer sig.
        Variant v;
        v.frags[2].a = 2;
        Opened o("hnwd_test_document_prims.nwd", build(v));
        CHECK(o.doc.load_fragments() == Status::Ok);
        CHECK(o.doc.fragment_mesh(1, mesh) == Status::Ok);
        CHECK(o.doc.fragment_mesh(2, mesh) == Status::Corrupt);
        CHECK(contains(o.doc.last_error(), "fragment 2") && contains(o.doc.last_error(), "primitives"));
    }
    {   // Posten finns inte.
        Variant v;
        v.frags[2].record = 3;
        Opened o("hnwd_test_document_record.nwd", build(v));
        CHECK(o.status == Status::Ok);
        CHECK(o.doc.load_fragments() == Status::Corrupt);
        CHECK(contains(o.doc.last_error(), "fragment 2") && contains(o.doc.last_error(), "record 3"));
        CHECK(o.doc.fragment_count() == 0 && all_fragments(o.doc).empty() && o.doc.look_count() == 0);
        CHECK(o.doc.fragment_mesh(0, mesh) == Status::NotLoaded);
        CHECK(o.doc.load_fragments() == Status::Corrupt);   // samma fel igen
    }
    {   // Post 0 och en negativ post.
        for (const int32_t record : {0, -1}) {
            Variant v;
            v.frags[0].record = record;
            Opened o("hnwd_test_document_record0.nwd", build(v));
            CHECK(o.doc.load_fragments() == Status::Corrupt);
            CHECK(contains(o.doc.last_error(), "fragment 0"));
        }
    }
    {   // seq avtar.
        Variant v;
        v.frags[2].seq = 3;
        Opened o("hnwd_test_document_seq.nwd", build(v));
        CHECK(o.doc.load_fragments() == Status::Corrupt);
        CHECK(contains(o.doc.last_error(), "fragment 2") && contains(o.doc.last_error(), "seq"));
        CHECK(o.doc.fragment_count() == 0);
    }
    {   // seq som är lika är inte avtagande.
        Variant v;
        v.frags[1].seq = 2;
        v.frags[2].seq = 2;
        Opened o("hnwd_test_document_seqeq.nwd", build(v));
        CHECK(o.doc.load_fragments() == Status::Ok);
    }
    {   // negativ seq.
        Variant v;
        v.frags[0].seq = -1;
        Opened o("hnwd_test_document_seqneg.nwd", build(v));
        CHECK(o.doc.load_fragments() == Status::Corrupt);
        CHECK(contains(o.doc.last_error(), "fragment 0"));
    }
    {   // negativt antal primitiver.
        Variant v;
        v.frags[1].a = -2;
        Opened o("hnwd_test_document_primneg.nwd", build(v));
        CHECK(o.doc.load_fragments() == Status::Corrupt);
        CHECK(contains(o.doc.last_error(), "fragment 1"));
    }
    {   // Ett fel i strömmen själv: en pose utan kropp.
        Variant v;
        v.frags[0].pose_class = 16;
        v.frags[0].s = 0;
        Opened o("hnwd_test_document_scale0.nwd", build(v));
        CHECK(o.doc.load_fragments() == Status::Corrupt && *o.doc.last_error() != 0);
    }
    return fails;
}

// ---------------------------------------------------------------------------
// Världen.

int test_world() {
    int fails = 0;
    const float x[3] = {1, 0, 0}, y[3] = {0, 1, 0}, z[3] = {0, 0, 1};
    double p[3];
    float n[3];

    HNwd::Pose rz;   // 90 grader kring Z, flytt och skala
    rz.q[2] = kSqrtHalf;
    rz.q[3] = kSqrtHalf;
    rz.t[0] = 10, rz.t[1] = 20, rz.t[2] = 30;
    rz.s = 2;
    HNwd::world_point(rz, x, p);
    CHECK(close_to(p[0], 10) && close_to(p[1], 22) && close_to(p[2], 30));
    HNwd::world_point(rz, y, p);
    CHECK(close_to(p[0], 8) && close_to(p[1], 20) && close_to(p[2], 30));
    HNwd::world_point(rz, z, p);
    CHECK(close_to(p[0], 10) && close_to(p[1], 20) && close_to(p[2], 32));

    rz.s = -2;   // negativ skala
    HNwd::world_point(rz, x, p);
    CHECK(close_to(p[0], 10) && close_to(p[1], 18) && close_to(p[2], 30));
    CHECK(HNwd::mirrors(rz));

    HNwd::Pose big = rz;   // en kvaternion som inte är normerad
    big.q[2] = 3 * kSqrtHalf;
    big.q[3] = 3 * kSqrtHalf;
    big.s = 1;
    HNwd::world_point(big, x, p);
    CHECK(close_to(p[0], 10) && close_to(p[1], 21) && close_to(p[2], 30));

    HNwd::Pose zero;   // ingen rotation om |q| är 0
    zero.q[3] = 0;
    zero.t[0] = 1;
    zero.s = 3;
    HNwd::world_point(zero, y, p);
    CHECK(close_to(p[0], 1) && close_to(p[1], 3) && close_to(p[2], 0));

    HNwd::Pose rx, ry;   // 90 grader kring X och Y fastlägger matrisens riktning
    rx.q[0] = kSqrtHalf, rx.q[3] = kSqrtHalf;
    ry.q[1] = kSqrtHalf, ry.q[3] = kSqrtHalf;
    HNwd::world_point(rx, y, p);
    CHECK(close_to(p[0], 0) && close_to(p[1], 0) && close_to(p[2], 1));
    HNwd::world_point(rx, z, p);
    CHECK(close_to(p[0], 0) && close_to(p[1], -1) && close_to(p[2], 0));
    HNwd::world_point(ry, z, p);
    CHECK(close_to(p[0], 1) && close_to(p[1], 0) && close_to(p[2], 0));
    HNwd::world_point(ry, x, p);
    CHECK(close_to(p[0], 0) && close_to(p[1], 0) && close_to(p[2], -1));

    // Normaler.
    HNwd::Pose plain = rz;
    plain.s = 5;   // skalan påverkar inte normalen
    HNwd::world_normal(plain, x, n);
    CHECK(close_to(n[0], 0, 1e-6) && close_to(n[1], 1, 1e-6) && close_to(n[2], 0, 1e-6));
    HNwd::world_normal(rz, x, n);   // speglad: vänd
    CHECK(close_to(n[0], 0, 1e-6) && close_to(n[1], -1, 1e-6) && close_to(n[2], 0, 1e-6));
    const float longer[3] = {3, 0, 0};
    HNwd::world_normal(plain, longer, n);   // normerad
    CHECK(close_to(n[0], 0, 1e-6) && close_to(n[1], 1, 1e-6));
    const float none[3] = {0, 0, 0};
    HNwd::world_normal(rz, none, n);   // noll förblir noll
    CHECK(n[0] == 0 && n[1] == 0 && n[2] == 0);
    HNwd::Pose id;
    HNwd::world_normal(id, z, n);
    CHECK(n[0] == 0 && n[1] == 0 && n[2] == 1);

    CHECK(!HNwd::mirrors(id) && !HNwd::mirrors(plain) && HNwd::mirrors(rz));
    HNwd::Pose zero_scale;
    zero_scale.s = 0;
    CHECK(!HNwd::mirrors(zero_scale));
    return fails;
}

int test_status_text() {
    int fails = 0;
    std::set<std::string> seen;
    for (int i = 0; i <= static_cast<int>(Status::OutOfRange); ++i) {
        const char* text = HNwd::status_text(static_cast<Status>(i));
        CHECK(text != nullptr && *text != 0);
        if (text) seen.insert(text);
    }
    CHECK(seen.size() == static_cast<size_t>(Status::OutOfRange) + 1);
    const char* other = HNwd::status_text(static_cast<Status>(200));
    CHECK(other != nullptr && *other != 0);
    return fails;
}

// ---------------------------------------------------------------------------
// Materialen och UV.

// R1 med råa UV: ett par per hörn.
Bytes r1_uv() {
    ByteWriter w;
    w.raw(record_head(94, 4, {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0}));
    w.raw(ints({102, 61, 4, 8}));
    for (const float f : {0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 1.f, 1.f}) w.f32(f);
    w.raw(cat({ints({103, 100, -4, 0}), hx("D4 D4 D4 D4")}));
    w.i32(0);
    w.raw(ints({1, 4, 1, 0x40000000}));
    w.i32(0);
    w.pad(8);
    return w.data;
}

const char kMaterialJson[] =
    R"({"version":2,"userassets":["0"],"materials":{"0":{"tag":"Generic","definition":"Generic",)"
    R"("properties":{"colors":{"generic_diffuse":{"IsConnectionEnabled":true,"connections":["1"],)"
    R"("values":[{"r":0.5,"g":0.25,"b":0.125,"a":1}]}}}},)"
    R"("1":{"tag":"UnifiedBitmap","definition":"UnifiedBitmap","properties":{)"
    R"("uris":{"thumbnail":{"values":[]},"unifiedbitmap_Bitmap":{"values":["tak.png"]}},)"
    R"("scalars":{"texture_UScale":{"units":"","values":[2]},)"
    R"("texture_RealWorldScaleX":{"units":"mm","values":[500]}},)"
    R"("booleans":{"texture_VRepeat":{"values":[false]}}}}}})";

int test_materials() {
    int fails = 0;
    Variant v;
    v.second_record = r1_uv();
    v.frags = default_frags();
    v.frags[2].a = 2;   // posten 2 är nu trianglar med fragmentets låda
    v.frags[2].bbox[3] = 1;
    v.frags[2].bbox[4] = 1;
    v.frags[2].material_json = kMaterialJson;
    Frag shared;   // ett nytt utseende som delar klass 185 med det tredje
    shared.record = 2;
    shared.seq = 8;
    shared.diffuse = 0.75f;
    shared.material_from = 2;
    v.frags.push_back(shared);
    Opened o("hnwd_test_document_material.nwd", build(v));
    Document& doc = o.doc;
    CHECK(o.status == Status::Ok);

    CHECK(doc.material_count() == 0);
    HNwd::Material m;
    CHECK(!doc.material(0, m));
    CHECK(doc.load_fragments() == Status::Ok);
    CHECK(*doc.last_error() == 0);
    CHECK(doc.material_count() == 1);
    CHECK(doc.look_count() == 3);
    const std::vector<HNwd::Look> looks_all = all_looks(doc);
    const HNwd::Look* looks = looks_all.empty() ? nullptr : looks_all.data();
    const std::vector<HNwd::Fragment> f_all = all_fragments(doc);
    const HNwd::Fragment* f = f_all.empty() ? nullptr : f_all.data();
    if (looks && f && doc.look_count() == 3 && doc.fragment_count() == 4) {
        CHECK(looks[0].material == HNwd::kNone);
        CHECK(f[2].look == 1 && looks[1].material == 0 && looks[1].diffuse[0] == 0.25f);
        CHECK(f[3].look == 2 && looks[2].material == 0 && looks[2].diffuse[0] == 0.75f);
    }

    CHECK(doc.material(0, m));
    CHECK(m.json == kMaterialJson);
    CHECK(m.has_color && m.base_color[0] == 0.5f && m.base_color[2] == 0.125f && m.base_color[3] == 1);
    CHECK(m.has_texture && m.texture_name == "tak.png" && m.texture_type == "png");
    CHECK(m.texture_size == 6 && m.texture_bytes != nullptr);
    if (m.texture_bytes && m.texture_size == 6)
        CHECK(m.texture_bytes[0] == 0x89 && m.texture_bytes[1] == 'P' && m.texture_bytes[5] == 0x0A);
    CHECK(m.u_scale == 2 && m.v_scale == 1 && m.u_offset == 0 && m.w_angle == 0);
    CHECK(m.real_scale_x == 500 && m.real_units_x == "mm" && m.real_units_y.empty());
    CHECK(m.u_repeat && !m.v_repeat);
    CHECK(!doc.material(1, m));

    // UV per vertex ut ur posten, också genom ett fragment.
    HNwd::Mesh mesh;
    CHECK(doc.fragment_mesh(2, mesh) == Status::Ok);
    CHECK(mesh.uvs == (std::vector<float>{0, 0, 1, 0, 0, 1, 1, 1}));
    CHECK(doc.record_mesh(1, mesh) == Status::Ok && mesh.uvs.empty());
    CHECK(doc.record_mesh(2, mesh) == Status::Ok && mesh.uvs.size() == 8);

    // Stängt finns inga material.
    doc.close();
    CHECK(doc.material_count() == 0 && !doc.material(0, m));
    return fails;
}

// ---------------------------------------------------------------------------
// Exempelfilerna.

int check_sample(const testdata::SampleCounts& s) {
    int fails = 0;
    const bool nwd = std::strcmp(s.path, testdata::kSampleNwd) == 0;
    Document doc;
    if (doc.open(s.path) != Status::Ok) {
        std::printf("  FAIL — %s: open: %s\n", s.path, doc.last_error());
        return 1;
    }
    CHECK(doc.version() == testdata::kSampleVersion);
    CHECK(doc.entry_count() == (nwd ? testdata::kSampleNwdPackages : testdata::kSampleNwcPackages));
    CHECK(doc.has(HNwd::Package::Geometry) && doc.has(HNwd::Package::Fragments) &&
          doc.has(HNwd::Package::GuidStore) && doc.has(HNwd::Package::XRefTable));
    CHECK(doc.has(HNwd::Package::SourceHeader) == !nwd);

    if (doc.load_fragments() != Status::Ok) {
        std::printf("  FAIL — %s: load_fragments: %s\n", s.path, doc.last_error());
        return fails + 1;
    }
    CHECK(doc.fragment_count() == s.fragments);
    CHECK(doc.record_count() == s.records);
    CHECK(doc.look_count() > 0);

    if (doc.load_guids() != Status::Ok)
        std::printf("  FAIL — %s: load_guids: %s\n", s.path, doc.last_error());
    CHECK(doc.guid_count() == s.guids);

    // Varje fragments post ska ha fragmentets låda och antal.
    HNwd::Mesh mesh, record;
    uint64_t bad = 0, mirrored = 0;
    double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
    const std::vector<HNwd::Fragment> fragments_all = all_fragments(doc);
    const HNwd::Fragment* fragments = fragments_all.empty() ? nullptr : fragments_all.data();
    for (size_t i = 0; i < doc.fragment_count(); ++i) {
        const HNwd::Fragment& f = fragments[i];
        if (HNwd::mirrors(f.pose)) ++mirrored;
        for (int corner = 0; corner < 8; ++corner) {
            const float v[3] = {corner & 1 ? f.bounds.max[0] : f.bounds.min[0],
                                corner & 2 ? f.bounds.max[1] : f.bounds.min[1],
                                corner & 4 ? f.bounds.max[2] : f.bounds.min[2]};
            double w[3];
            HNwd::world_point(f.pose, v, w);
            for (int k = 0; k < 3; ++k) {
                lo[k] = std::min(lo[k], w[k]);
                hi[k] = std::max(hi[k], w[k]);
            }
        }

        if (doc.fragment_mesh(i, mesh) == Status::Ok) continue;
        if (bad++ >= 3) continue;
        std::printf("  FAIL — %s: fragment %zu: %s\n", s.path, i, doc.last_error());
        std::printf("    record %u, fragment box (%.9g %.9g %.9g)-(%.9g %.9g %.9g), primitives %u\n",
                    f.record, f.bounds.min[0], f.bounds.min[1], f.bounds.min[2], f.bounds.max[0],
                    f.bounds.max[1], f.bounds.max[2], f.primitives);
        if (doc.record_mesh(f.record, record) == Status::Ok)
            std::printf("    record box (%.9g %.9g %.9g)-(%.9g %.9g %.9g), primitives %zu\n",
                        record.bounds.min[0], record.bounds.min[1], record.bounds.min[2],
                        record.bounds.max[0], record.bounds.max[1], record.bounds.max[2],
                        record.indices.size() / (record.kind == HNwd::MeshKind::Lines ? 2 : 3));
    }
    CHECK(bad == 0);
    CHECK(doc.normal_bits() == (nwd ? 8 : 16));

    HNwd::Source src;
    if (nwd) {
        CHECK(doc.file_source(src) == Status::Missing && *doc.last_error() != 0);
    } else {
        CHECK(doc.file_source(src) == Status::Ok);
        CHECK(src.loader.rfind("LcNwcLoaderPlugin:", 0) == 0);
    }

    std::string json;
    CHECK(doc.xref_json(json) == Status::Ok);
    CHECK(json.rfind("{\"Type\":\"XRefTable\"", 0) == 0);

    std::printf("  %s\n    fragments %zu, mirrored %llu, failed %llu, normal bits %d\n"
                "    world x %.4f..%.4f, y %.4f..%.4f, z %.4f..%.4f\n",
                s.path, doc.fragment_count(), static_cast<unsigned long long>(mirrored),
                static_cast<unsigned long long>(bad), doc.normal_bits(), lo[0], hi[0], lo[1], hi[1],
                lo[2], hi[2]);
    return fails;
}

int test_samples() {
    int fails = 0;
    for (const testdata::SampleCounts& s : testdata::kSamples) {
        if (!testdata::exists(s.path)) {
            std::printf("  SKIP — %s saknas\n", s.path);
            ++g_skipped;
            continue;
        }
        fails += check_sample(s);
    }
    return fails;
}

// ---------------------------------------------------------------------------
// Förlopp och avbrott, enheten, världslådan och minnet.

// Ett förlopp som svarar false vid anrop nummer `stop` (1 = det första).
struct StopAt {
    int stop;
    int calls = 0;
    HNwd::Progress fn() {
        return [this](uint64_t, uint64_t) { return ++calls < stop; };
    }
};

int test_progress() {
    int fails = 0;
    Opened o("hnwd_test_document_progress.nwd", build(Variant{}));
    CHECK(o.status == Status::Ok);
    Document& doc = o.doc;

    // load_geometry: avbrutet lämnar ingenting inläst, och går att göra om.
    StopAt g{1};
    CHECK(doc.load_geometry(g.fn()) == Status::Cancelled && g.calls == 1);
    CHECK(contains(doc.last_error(), "cancelled"));
    CHECK(doc.record_count() == 0 && doc.normal_bits() == 0);
    HNwd::Mesh mesh;
    CHECK(doc.record_mesh(1, mesh) == Status::NotLoaded);
    std::vector<uint64_t> seen;
    CHECK(doc.load_geometry([&](uint64_t d, uint64_t t) {
        seen.push_back(d);
        return t == 2;
    }) == Status::Ok);
    CHECK(!seen.empty() && seen.back() == 2);
    CHECK(doc.record_count() == 2 && doc.normal_bits() == 16);

    // load_fragments: avbrutet i början och i slutet.
    for (const int stop : {1, 2}) {
        StopAt f{stop};
        CHECK(doc.load_fragments(f.fn()) == Status::Cancelled && f.calls == stop);
        CHECK(contains(doc.last_error(), "LcOpNwdFragments: cancelled"));
        CHECK(doc.fragment_count() == 0 && doc.look_count() == 0 && doc.material_count() == 0);
        HNwd::Fragment fr;
        CHECK(!doc.fragment(0, fr));
        CHECK(doc.fragment_mesh(0, mesh) == Status::NotLoaded);
    }
    CHECK(doc.load_fragments() == Status::Ok && doc.fragment_count() == 3);
    CHECK(doc.fragment_mesh(0, mesh) == Status::Ok);

    // load_spatial: avbrutet före, under och efter trädet.
    for (const int stop : {1, 2, 3}) {
        StopAt s{stop};
        CHECK(doc.load_spatial(s.fn()) == Status::Cancelled && s.calls == stop);
        CHECK(doc.spatial_node_count() == 0 && doc.spatial_items() == nullptr);
    }
    CHECK(doc.load_spatial() == Status::Ok && doc.spatial_node_count() == 1);
    CHECK(!doc.spatial_from_file());   // filen har inget paket

    // Ett nytt dokument: load_fragments avbryts redan i geometrin, och
    // load_spatial i fragmenten.
    {
        Opened p("hnwd_test_document_progress2.nwd", build(Variant{}));
        StopAt f{1};
        CHECK(p.doc.load_fragments(f.fn()) == Status::Cancelled);
        CHECK(p.doc.record_count() == 0 && p.doc.fragment_count() == 0);
        StopAt s{1};
        CHECK(p.doc.load_spatial(s.fn()) == Status::Cancelled);
        CHECK(p.doc.record_count() == 0 && p.doc.fragment_count() == 0);
        CHECK(p.doc.load_spatial() == Status::Ok && p.doc.fragment_count() == 3);
    }
    return fails;
}

int test_units() {
    int fails = 0;
    struct Case {
        int32_t code;
        Status status;
        HNwd::LengthUnit unit;
        double metres;
    };
    const Case cases[] = {
        {0, Status::Ok, HNwd::LengthUnit::Metre, 1.0},
        {2, Status::Ok, HNwd::LengthUnit::Millimetre, 0.001},
        {3, Status::Ok, HNwd::LengthUnit::Foot, 0.3048},
        {1, Status::Ok, HNwd::LengthUnit::Centimetre, 0.01},
        {4, Status::Unsupported, HNwd::LengthUnit::Unknown, 0},
        {7, Status::Unsupported, HNwd::LengthUnit::Unknown, 0},
    };
    for (const Case& c : cases) {
        Variant v;
        v.partition_bytes = partition_with_unit(c.code);
        Opened o("hnwd_test_document_unit.nwd", build(v));
        HNwd::Unit u;
        CHECK(o.doc.length_unit(u) == c.status);
        CHECK(u.code == c.code && u.unit == c.unit && u.metres_per_unit == c.metres);
        if (c.status != Status::Ok)
            CHECK(contains(o.doc.last_error(), ("code " + std::to_string(c.code)).c_str()));
    }
    // Utan partitionen, med en partition som inte går att tolka, och utan fil.
    {
        Variant v;
        v.partition = false;
        Opened o("hnwd_test_document_unit.nwd", build(v));
        HNwd::Unit u;
        CHECK(o.doc.length_unit(u) == Status::Missing && u.code == -1);
    }
    {
        Opened o("hnwd_test_document_unit.nwd", build(Variant{}));
        HNwd::Unit u;
        CHECK(o.doc.length_unit(u) == Status::Corrupt && u.unit == HNwd::LengthUnit::Unknown);
    }
    {
        Document doc;
        HNwd::Unit u;
        CHECK(doc.length_unit(u) == Status::NotLoaded);
    }
    // Exempelfilerna: koden ur tabellen.
    for (const testdata::SampleFacts& f : testdata::kFacts) {
        if (!testdata::exists(f.path)) continue;   // räknas som överhoppat av test_samples
        Document doc;
        HNwd::Unit u;
        CHECK(doc.open(f.path) == Status::Ok && doc.length_unit(u) == Status::Ok);
        CHECK(u.code == f.unit_code);
        CHECK(u.unit == (f.unit_code == 0 ? HNwd::LengthUnit::Metre : HNwd::LengthUnit::Millimetre));
    }
    return fails;
}

// Världslådan rymmer postens hörn och dess positioner genom posen.
int check_world_boxes(Document& doc, const char* what) {
    int fails = 0;
    size_t outside = 0, corners_off = 0;
    HNwd::Mesh mesh;
    for (size_t i = 0; i < doc.fragment_count(); ++i) {
        HNwd::Fragment f;
        HNwd::BoxD box;
        CHECK(doc.fragment(i, f) && doc.fragment_world_box(i, box));
        bool touch[6] = {};
        for (int corner = 0; corner < 8; ++corner) {
            const float v[3] = {corner & 1 ? f.bounds.max[0] : f.bounds.min[0],
                                corner & 2 ? f.bounds.max[1] : f.bounds.min[1],
                                corner & 4 ? f.bounds.max[2] : f.bounds.min[2]};
            double w[3];
            HNwd::world_point(f.pose, v, w);
            for (int k = 0; k < 3; ++k) {
                if (w[k] < box.min[k] || w[k] > box.max[k]) ++outside;
                touch[k] = touch[k] || w[k] == box.min[k];
                touch[3 + k] = touch[3 + k] || w[k] == box.max[k];
            }
        }
        for (const bool t : touch)
            if (!t) ++corners_off;
        if (doc.fragment_mesh(i, mesh) != Status::Ok) continue;
        const double slack = 1e-9 * (1 + std::fabs(box.max[0]) + std::fabs(box.max[1]) +
                                     std::fabs(box.max[2]) + std::fabs(box.min[0]) +
                                     std::fabs(box.min[1]) + std::fabs(box.min[2]));
        for (size_t p = 0; p + 2 < mesh.positions.size(); p += 3) {
            double w[3];
            HNwd::world_point(f.pose, &mesh.positions[p], w);
            for (int k = 0; k < 3; ++k)
                if (w[k] < box.min[k] - slack || w[k] > box.max[k] + slack) ++outside;
        }
    }
    if (outside || corners_off) {
        std::printf("  FAIL — %s: %zu points outside the world box, %zu loose sides\n", what,
                    outside, corners_off);
        ++fails;
    }
    HNwd::BoxD box;
    CHECK(!doc.fragment_world_box(doc.fragment_count(), box));
    return fails;
}

int test_world_box_and_memory() {
    int fails = 0;
    {
        Opened o("hnwd_test_document_box.nwd", build(Variant{}));
        HNwd::BoxD box;
        CHECK(!o.doc.fragment_world_box(0, box));
        const size_t opened = o.doc.memory_bytes();
        CHECK(opened > 0);
        CHECK(o.doc.load_fragments() == Status::Ok);
        CHECK(o.doc.memory_bytes() > opened);
        fails += check_world_boxes(o.doc, "synthetic");
        // Fragment 0: 90 grader kring Z, så lådan (0..1, 0..1, 0) står i x −1..0.
        CHECK(o.doc.fragment_world_box(0, box));
        CHECK(close_to(box.min[0], 9) && close_to(box.max[0], 10) && close_to(box.min[1], 20) &&
              close_to(box.max[1], 21) && box.min[2] == 30 && box.max[2] == 30);
        HNwd::Look l;
        CHECK(o.doc.look(1, l) && l.diffuse[0] == 0.25f && !o.doc.look(2, l));
        o.doc.close();
        CHECK(o.doc.memory_bytes() < opened + 1024);
    }
    {
        Document none;
        CHECK(none.memory_bytes() > 0);
    }
    for (const testdata::SampleCounts& s : testdata::kSamples) {
        if (!testdata::exists(s.path)) continue;   // räknas som överhoppat av test_samples
        Document doc;
        CHECK(doc.open(s.path) == Status::Ok && doc.load_fragments() == Status::Ok);
        fails += check_world_boxes(doc, s.path);
        // Minnet i ett nytt dokument, utan uppackade chunkar i cachen.
        if (std::strcmp(s.path, testdata::kSampleNwd) == 0) {
            Document fresh;
            CHECK(fresh.open(s.path) == Status::Ok && fresh.load_geometry() == Status::Ok);
            const size_t geometry = fresh.memory_bytes();
            CHECK(fresh.load_fragments() == Status::Ok);
            const size_t fragments = fresh.memory_bytes();
            CHECK(fresh.load_spatial() == Status::Ok);
            const size_t both = fresh.memory_bytes();
            const double n = static_cast<double>(fresh.fragment_count());
            std::printf("  %s: memory %zu bytes with the geometry, %zu with the fragments "
                        "(%.1f per fragment), %zu with the spatial index (%.1f per fragment)\n",
                        s.path, geometry, fragments, (fragments - geometry) / n, both,
                        (both - geometry) / n);
            CHECK(fragments > geometry && both > fragments);
            CHECK((fragments - geometry) / n < 100);
        }
    }
    return fails;
}

// Det rumsliga indexet ur en syntetisk fil: det egna, filens, en form som inte
// avkodas och ett trasigt paket.
int test_spatial_synthetic() {
    int fails = 0;
    // 200 fragment i ett rutnät om 20 × 10, alla med samma utseende och post.
    Variant v;
    v.frags.assign(200, Frag{});
    for (int i = 0; i < 200; ++i) {
        Frag& f = v.frags[static_cast<size_t>(i)];
        f.pose_class = 14;
        f.t[0] = 10.0 * (i % 20);
        f.t[1] = 10.0 * (i / 20);
        f.seq = 2 + i;
        if (i > 0) {
            f.app_from = 0;
            f.geom_from = 0;
        }
    }
    {
        Opened o("hnwd_test_document_spatial.nwd", build(v));
        CHECK(o.doc.load_spatial() == Status::Ok && !o.doc.spatial_from_file());
        const size_t nodes = o.doc.spatial_node_count();
        CHECK(nodes > 7);
        const uint32_t* items = o.doc.spatial_items();
        std::vector<int> count(200, 0);
        size_t leaves = 0, big = 0;
        uint64_t prims = 0;
        for (uint32_t i = 0; i < nodes; ++i) {
            HNwd::SpatialNode n;
            CHECK(o.doc.spatial_node(i, n));
            if (n.child_count != 0) continue;
            ++leaves;
            if (n.item_count > 32) ++big;
            prims += n.primitives;
            for (uint32_t k = 0; k < n.item_count; ++k) ++count[items[n.item_begin + k]];
        }
        CHECK(big == 0 && leaves >= 7 && prims == 400);
        CHECK(std::count(count.begin(), count.end(), 1) == 200);
        HNwd::SpatialNode root;
        CHECK(o.doc.spatial_node(0, root) && root.primitives == 400);
        CHECK(root.bounds.min[0] == 0 && root.bounds.max[0] == 191 && root.bounds.max[1] == 91);
    }
    // Filens index, en okänd nodtagg och ett trasigt paket. Ett paket som
    // finns men inte går att tolka ger Corrupt; det egna indexet byggs bara
    // när anroparen ber om det.
    {
        Variant w;   // tre fragment
        w.spatial_bytes = ints({5, 2, 2, 2, 1, 2, 1, 0, 4, 1});
        Opened o("hnwd_test_document_spatial.nwd", build(w));
        CHECK(o.doc.load_spatial() == Status::Ok && o.doc.spatial_from_file());
        CHECK(o.doc.spatial_node_count() == 3);
        const uint32_t* items = o.doc.spatial_items();
        CHECK(items && items[0] == 2 && items[1] == 0 && items[2] == 1);
        HNwd::SpatialNode n;
        CHECK(o.doc.spatial_node(2, n) && n.parent == 0 && n.item_begin == 2 && n.item_count == 1);
        CHECK(o.doc.spatial_child(0, 1) == 2 && n.primitives == 2);
        // Tvingat eget, och tillbaka.
        CHECK(o.doc.load_spatial({}, HNwd::SpatialSource::Own) == Status::Ok);
        CHECK(!o.doc.spatial_from_file() && o.doc.spatial_node_count() == 1);
        CHECK(o.doc.load_spatial() == Status::Ok && o.doc.spatial_from_file());
    }
    {
        Variant w;
        w.spatial_bytes = ints({9, 3});
        Opened o("hnwd_test_document_spatial.nwd", build(w));
        CHECK(o.doc.load_spatial() == Status::Corrupt);
        CHECK(contains(o.doc.last_error(), "node tag 9"));
        CHECK(o.doc.spatial_node_count() == 0 && !o.doc.spatial_from_file());
        CHECK(o.doc.load_spatial({}, HNwd::SpatialSource::Own) == Status::Ok);
        CHECK(!o.doc.spatial_from_file() && o.doc.spatial_node_count() == 1);
    }
    {
        Variant w;
        w.spatial_bytes = ints({5, 0});
        Opened o("hnwd_test_document_spatial.nwd", build(w));
        CHECK(o.doc.load_spatial() == Status::Corrupt);
        CHECK(contains(o.doc.last_error(), "LcOpNwdSpatialHierarchy"));
        CHECK(o.doc.spatial_node_count() == 0);
        CHECK(o.doc.load_spatial({}, HNwd::SpatialSource::Own) == Status::Ok);
    }
    // Utan fragment: inga noder.
    {
        Variant w;
        w.frags.clear();
        Opened o("hnwd_test_document_spatial.nwd", build(w));
        CHECK(o.doc.load_spatial() == Status::Ok);
        CHECK(o.doc.spatial_node_count() == 0 && o.doc.spatial_items() == nullptr);
    }
    return fails;
}

} // namespace

int test_document() {
    int fails = 0;
    fails += test_whole_file();
    fails += test_move();
    fails += test_open_errors();
    fails += test_missing_and_corrupt_packages();
    fails += test_link_errors();
    fails += test_world();
    fails += test_status_text();
    fails += test_materials();
    fails += test_progress();
    fails += test_units();
    fails += test_world_box_and_memory();
    fails += test_spatial_synthetic();
    fails += test_samples();
    std::printf("test_document: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
