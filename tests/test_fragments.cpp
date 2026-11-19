#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "fragment_view.h"
#include "fragments.h"
#include "test_support.h"

using namespace HNwd::detail;
using testdata::Bytes;
using testdata::ByteWriter;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

// Ett material vars bild är filen "c".
const char kMaterialJson[] =
    R"({"version":2,"userassets":["0"],"materials":{"0":{"definition":"Generic","properties":{)"
    R"("colors":{"generic_diffuse":{"IsConnectionEnabled":true,"connections":["1"],)"
    R"("values":[{"r":0.5,"g":0.25,"b":1,"a":1}]}}}},)"
    R"("1":{"definition":"UnifiedBitmap","properties":{)"
    R"("uris":{"unifiedbitmap_Bitmap":{"values":["c"]}}}}}})";

// Ett klass-185-objekt: JSON, en sträng och två filer, den andra inbäddad med
// fem byte. Utan `embedded` är båda externa.
void class185(ByteWriter& w, const char* json = kMaterialJson, bool embedded = true) {
    w.str(json);
    w.str("");
    w.i32(2);
    w.str("a");
    w.i32(0);
    w.str("b");
    w.str("c");
    w.i32(embedded ? 1 : 0);
    w.str("navis");
    if (embedded) {
        w.str("jpg");
        w.i64a(5);
        w.hex("0102030405");
        w.pad(4);
    }
}

// Hur en pekarplats skrivs: ett nytt objekt, null eller en bakåtpekare.
enum class Slot { New, Null, Back };

// Ett fragment med alla knappar som felfallen behöver.
struct Spec {
    int32_t a = 2, b = 4, c = 7, seq = 2;
    double d = 1.5;

    int pose_class = 15;                 // 14, 15, 16 eller 13 (fel klass)
    Slot pose = Slot::New;
    double t[3] = {10, 0, 0};
    double q[4] = {0, 0, 0, 1};
    double s = 1;

    int32_t flags = 1;                   // utseendets flaggor
    Slot app = Slot::New;
    Slot material = Slot::New;
    Slot extra = Slot::Null;             // bara om flags & 32
    int32_t extra_ref = 0;               // bakåtpekarens mål för extra; 0: materialet (fel klass)
    const char* json = kMaterialJson;    // klass 185:s JSON
    bool embedded = true;                // klass 185:s fil "c" ligger i strömmen
    float mat[14] = {0, 0, 0, 0.5f, 0.25f, 0.125f, 0, 0, 0, 0, 0, 0, 0, 0.75f};

    Slot geometry = Slot::New;
    float bbox[6] = {0, 0, 0, 1, 1, 0};
    double center[3] = {0.5, 0.5, 0};
    float f = 2.5f;
    int32_t key = 7, record = 1;
};

// Ids som ett tidigare fragment gav ut, för bakåtpekare.
struct Ids {
    int32_t app = 0, material = 0, geometry = 0, data = 0;   // data: klass 185
};

// Skriver ett fragment som en rot. `ids` är de objekt som finns att peka på;
// de nya ids som skrivs hamnar i `out`.
void fragment(ByteWriter& w, const Spec& s, const Ids& ids, Ids& out) {
    w.obj(13);
    w.f64(s.d);
    w.i32(s.a);
    w.i32(s.b);
    w.i32(s.c);
    w.i32(s.seq);

    // Pose
    if (s.pose == Slot::Null) {
        w.null_ptr();
    } else if (s.pose == Slot::Back) {
        w.ref(100);
    } else {
        w.obj(s.pose_class);
        for (double v : s.t) w.f64(v);
        if (s.pose_class == 15 || s.pose_class == 16)
            for (double v : s.q) w.f64(v);
        if (s.pose_class == 16) w.f64(s.s);
    }

    // Utseende
    if (s.app == Slot::Null) {
        w.null_ptr();
        out.app = ids.app;
    } else if (s.app == Slot::Back) {
        w.ref(ids.app);
        out.app = ids.app;
    } else {
        out.app = w.obj(55);
        w.i32(s.flags);
        if (s.material == Slot::Null) {
            w.null_ptr();
        } else if (s.material == Slot::Back) {
            w.ref(ids.material);
            out.material = ids.material;
        } else {
            out.material = w.obj(54);
            for (float v : s.mat) w.f32(v);
        }
        if (s.flags & 32) {
            if (s.extra == Slot::Null) {
                w.null_ptr();
            } else if (s.extra == Slot::Back) {
                w.ref(s.extra_ref ? s.extra_ref : ids.material);   // utan mål: fel klass
            } else {
                out.data = w.obj(185);
                class185(w, s.json, s.embedded);
            }
        }
    }

    // Geometri
    if (s.geometry == Slot::Null) {
        w.null_ptr();
        out.geometry = ids.geometry;
    } else if (s.geometry == Slot::Back) {
        w.ref(ids.geometry);
        out.geometry = ids.geometry;
    } else {
        out.geometry = w.obj(93);
        for (float v : s.bbox) w.f32(v);
        for (double v : s.center) w.f64(v);
        w.f32(s.f);
        w.i32(s.key);
        w.i32(s.record);
    }
}

// En ström med ett enda fragment; `ids` är det som bakåtpekarna pekar på.
Bytes single(const Spec& s, const Ids& ids = Ids{}) {
    ByteWriter w;
    w.i32(1);
    Ids out;
    fragment(w, s, ids, out);
    w.pad(8);
    return w.data;
}

ErrorCode parse(const Bytes& data, FragmentStore& t, std::string* detail = nullptr,
                std::vector<FragmentExtra>* extra = nullptr) {
    return parse_fragments(data.data(), data.size(), t, extra, detail);
}

// Ger strömmen exakt `code` och ett felmeddelande som inleds med paketets namn?
bool fails_with(const Bytes& data, ErrorCode code) {
    FragmentStore t;
    std::string detail;
    const ErrorCode e = parse(data, t, &detail);
    return e == code && detail.rfind("LcOpNwdFragments at byte ", 0) == 0 &&
           t.items.empty() && t.looks.empty();
}

bool fails_with(const Spec& s, ErrorCode code) { return fails_with(single(s), code); }

} // namespace

int test_fragments() {
    int fails = 0;

    // Flera fragment av alla tre poseklasserna, med delat utseende, delat material
    // och delad geometrireferens.
    {
        ByteWriter w;
        w.i32(6);
        Ids none, ids0, ids1, ids2, ids3, ids4, ids5;

        Spec f0;   // klass 15, nytt utseende, ny geometri
        f0.pose_class = 15;
        f0.t[0] = 10;
        f0.q[0] = 0.5;
        f0.q[1] = 0.5;
        f0.q[2] = -0.5;
        f0.q[3] = 0.5;
        f0.seq = 2;
        f0.a = 2;
        fragment(w, f0, none, ids0);

        Spec f1;   // klass 16 med s = -1, delat utseende och delad geometri
        f1.pose_class = 16;
        f1.t[0] = 1;
        f1.t[1] = 2;
        f1.t[2] = 3;
        f1.q[2] = 0.6;
        f1.q[3] = 0.8;   // längden 1 inom avrundningen: lagras som den står
        f1.s = -1;
        f1.app = Slot::Back;
        f1.geometry = Slot::Back;
        f1.seq = 7;
        f1.a = 2;
        f1.d = 3.5;
        f1.b = 9;
        f1.c = 11;
        fragment(w, f1, ids0, ids1);

        Spec f2;   // klass 14, nytt utseende med flags & 32 och en klass 185, delat material
        f2.pose_class = 14;
        f2.t[0] = 4;
        f2.t[1] = 5;
        f2.t[2] = 6;
        f2.flags = 33;
        f2.extra = Slot::New;
        f2.material = Slot::Back;
        f2.geometry = Slot::New;
        f2.record = 5;
        f2.key = 9;
        f2.bbox[0] = -1;
        f2.bbox[5] = 4;
        f2.center[2] = 7;
        f2.f = -3.5f;
        f2.seq = 7;
        f2.a = 12;
        fragment(w, f2, ids0, ids2);

        Spec f3;   // klass 15 med flags & 32 och en null som extra, nytt material
        f3.pose_class = 15;
        f3.flags = 32;
        f3.extra = Slot::Null;
        f3.mat[0] = 1;
        f3.mat[13] = 0.5f;
        f3.geometry = Slot::Back;
        f3.seq = 9;
        f3.a = 2;
        fragment(w, f3, ids2, ids3);

        Spec f4;   // ingen pose, utseendet från f3 (det andra materialet) och geometrin från f2
        f4.pose = Slot::Null;
        f4.app = Slot::Back;
        f4.geometry = Slot::Back;
        f4.seq = 9;
        f4.a = 12;
        fragment(w, f4, ids3, ids4);

        Spec f5;   // nytt utseende som delar det andra materialet
        f5.material = Slot::Back;
        f5.geometry = Slot::Back;
        f5.seq = 10;
        f5.a = 2;
        fragment(w, f5, ids3, ids5);
        w.pad(8);

        FragmentStore t;
        std::vector<FragmentExtra> extra;
        std::string detail;
        CHECK(parse(w.data, t, &detail, &extra) == ErrorCode::Ok);
        CHECK(detail.empty());
        CHECK(t.items.size() == 6);
        CHECK(extra.size() == 6);
        // Utseendena är paren: f0:s material utan klass 185, samma material med
        // f2:s klass 185, och f3:s material utan.
        CHECK(t.looks.size() == 3);
        CHECK(t.materials.size() == 1);
        if (t.items.size() == 6 && extra.size() == 6 && t.looks.size() == 3 &&
            t.materials.size() == 1) {
            const FragmentView& a = fragment_view(t, 0);
            CHECK(a.pose_class == 15);
            CHECK(a.t[0] == 10 && a.t[1] == 0 && a.t[2] == 0);
            CHECK(a.q[0] == 0.5 && a.q[1] == 0.5 && a.q[2] == -0.5 && a.q[3] == 0.5);
            CHECK(a.s == 1);
            CHECK(a.record == 1 && a.a == 2 && a.seq == 2);
            CHECK(a.bbox[0] == 0 && a.bbox[3] == 1 && a.bbox[4] == 1 && a.bbox[5] == 0);
            CHECK(a.look == 0);
            CHECK(extra[0].d == 1.5 && extra[0].b == 4 && extra[0].c == 7 && extra[0].key == 7);
            CHECK(extra[0].appearance_flags == 1);
            CHECK(extra[0].center[0] == 0.5 && extra[0].center[1] == 0.5 && extra[0].center[2] == 0);
            CHECK(extra[0].f == 2.5f);

            const FragmentView& b = fragment_view(t, 1);
            CHECK(b.pose_class == 16);
            CHECK(b.t[0] == 1 && b.t[1] == 2 && b.t[2] == 3);
            CHECK(b.q[0] == 0 && b.q[1] == 0 && b.q[2] == 0.6 && b.q[3] == 0.8);
            CHECK(b.s == -1);
            CHECK(b.look == a.look);                      // utseendet delas
            CHECK(b.record == a.record && b.a == 2 && b.seq == 7);   // geometrireferensen delas
            CHECK(b.bbox[3] == 1);
            CHECK(extra[1].d == 3.5 && extra[1].b == 9 && extra[1].c == 11);
            CHECK(extra[1].appearance_flags == 1 && extra[1].key == 7);

            const FragmentView& c = fragment_view(t, 2);
            CHECK(c.pose_class == 14);
            CHECK(c.t[0] == 4 && c.t[1] == 5 && c.t[2] == 6);
            CHECK(c.q[0] == 0 && c.q[1] == 0 && c.q[2] == 0 && c.q[3] == 1 && c.s == 1);
            CHECK(c.record == 5 && c.a == 12 && c.seq == 7);
            CHECK(c.bbox[0] == -1 && c.bbox[5] == 4);
            CHECK(c.look == 1 && c.look != a.look);       // materialet delas, men klass 185 skiljer
            CHECK(extra[2].appearance_flags == 33 && extra[2].key == 9);
            CHECK(extra[2].center[2] == 7 && extra[2].f == -3.5f);

            const FragmentView& d = fragment_view(t, 3);
            CHECK(d.pose_class == 15 && d.s == 1);
            CHECK(d.look == 2 && d.look != a.look);
            CHECK(d.record == 5 && d.seq == 9);           // geometrin från f2
            CHECK(extra[3].appearance_flags == 32);

            const FragmentView& e = fragment_view(t, 4);
            CHECK(e.pose_class == 0);   // ingen pose: flytten är geometrins mittpunkt
            CHECK(e.t[0] == 0.5 && e.t[1] == 0.5 && e.t[2] == 7);
            CHECK(e.q[0] == 0 && e.q[1] == 0 && e.q[2] == 0 && e.q[3] == 1 && e.s == 1);
            CHECK(e.look == d.look && e.look == 2);       // utseendet från f3
            CHECK(e.record == 5 && e.a == 12);
            CHECK(extra[4].appearance_flags == 32);

            CHECK(fragment_view(t, 5).look == 2 && fragment_view(t, 5).seq == 10);   // samma par som f3

            // Materialet: 14 flyttal i filens ordning.
            CHECK(t.looks[0].v[3] == 0.5f && t.looks[0].v[4] == 0.25f && t.looks[0].v[5] == 0.125f);
            CHECK(t.looks[0].v[13] == 0.75f && t.looks[0].v[0] == 0);
            CHECK(t.looks[1].v[3] == 0.5f && t.looks[1].v[13] == 0.75f);   // f0:s tal
            CHECK(t.looks[2].v[0] == 1 && t.looks[2].v[13] == 0.5f);
            CHECK(t.looks[0].material == kNoMaterial && t.looks[1].material == 0 &&
                  t.looks[2].material == kNoMaterial);

            // Klass 185: JSON, filerna och bilden.
            const MaterialData& m = t.materials[0];
            CHECK(m.json == kMaterialJson);
            CHECK(m.files.size() == 2);
            if (m.files.size() == 2) {
                CHECK(m.files[0].name == "a" && !m.files[0].embedded && m.files[0].bytes.empty());
                CHECK(m.files[1].name == "c" && m.files[1].embedded && m.files[1].type == "jpg");
                CHECK(m.files[1].bytes == (Bytes{1, 2, 3, 4, 5}));
            }
            CHECK(m.texture.has_color && m.texture.color[1] == 0.25f);
            CHECK(m.texture.bitmap == "c" && m.texture.file == 1);
        }

        // Utan `extra` och utan `detail` går det lika bra.
        CHECK(parse(w.data, t) == ErrorCode::Ok);
        CHECK(t.items.size() == 6);

        // Varje avhuggen längd är ett fel.
        for (size_t len = 0; len < w.data.size(); ++len) {
            FragmentStore cut;
            std::string d;
            const ErrorCode code = parse_fragments(w.data.data(), len, cut, nullptr, &d);
            if (code != ErrorCode::CorruptObject || d.empty() || !cut.items.empty()) {
                std::printf("  cut at %zu gave %d\n", len, static_cast<int>(code));
                ++fails;
            }
        }
    }

    // Klass 185 delad genom bakåtpekare, par som delas av olika utseenden, och
    // en bild som inte ligger i strömmen.
    {
        ByteWriter w;
        w.i32(4);
        Ids ids0, ids1, ids2, ids3;
        Spec g0;   // material A med en ny klass 185 vars bild är extern
        g0.flags = 32;
        g0.extra = Slot::New;
        g0.embedded = false;
        fragment(w, g0, Ids{}, ids0);
        Spec g1;   // nytt material B, klass 185 genom bakåtpekare
        g1.flags = 32;
        g1.extra = Slot::Back;
        g1.extra_ref = ids0.data;
        g1.mat[0] = 0.5f;
        fragment(w, g1, ids0, ids1);
        Spec g2;   // ett nytt utseende med samma par som g0
        g2.flags = 33;
        g2.extra = Slot::Back;
        g2.extra_ref = ids0.data;
        g2.material = Slot::Back;
        fragment(w, g2, ids0, ids2);
        Spec g3;   // material A utan klass 185
        g3.material = Slot::Back;
        fragment(w, g3, ids0, ids3);
        w.pad(8);

        FragmentStore t;
        std::string detail;
        CHECK(parse(w.data, t, &detail) == ErrorCode::Ok);
        CHECK(detail.empty());
        CHECK(t.materials.size() == 1);
        CHECK(t.looks.size() == 3);
        if (t.items.size() == 4 && t.looks.size() == 3 && t.materials.size() == 1) {
            CHECK(fragment_view(t, 0).look == 0 && fragment_view(t, 1).look == 1);
            CHECK(fragment_view(t, 2).look == 0 && fragment_view(t, 3).look == 2);
            CHECK(t.looks[0].material == 0 && t.looks[1].material == 0);
            CHECK(t.looks[2].material == kNoMaterial);
            CHECK(t.looks[0].v[0] == 0 && t.looks[1].v[0] == 0.5f && t.looks[2].v[0] == 0);
            const MaterialData& m = t.materials[0];
            CHECK(m.files.size() == 2 && m.files[1].name == "c" && !m.files[1].embedded &&
                  m.files[1].kind == "navis" && m.files[1].bytes.empty());
            CHECK(m.texture.bitmap == "c" && m.texture.file == 1);   // namnet finns, men utan byte
        }

        // En bakåtpekare till klass 185 tar inte emot en annan klass.
        Spec bad;
        bad.flags = 32;
        bad.extra = Slot::Back;
        bad.extra_ref = 101;   // posen
        CHECK(fails_with(single(bad), ErrorCode::CorruptObject));
    }

    // Klass 185 vars JSON inte går att tolka, och ett material utan bild.
    {
        Spec s;
        s.flags = 32;
        s.extra = Slot::New;
        s.json = "{\"materials\":";
        CHECK(fails_with(s, ErrorCode::CorruptObject));
        FragmentStore t;
        std::string detail;
        CHECK(parse(single(s), t, &detail) == ErrorCode::CorruptObject);
        CHECK(detail.find("class-185 json at character 13: unexpected end") != std::string::npos);
        CHECK(t.materials.empty());

        s.json = "{\"version\":2}";
        CHECK(parse(single(s), t) == ErrorCode::Ok);
        CHECK(t.materials.size() == 1 && t.looks.size() == 1 && t.looks[0].material == 0);
        if (t.materials.size() == 1) {
            CHECK(t.materials[0].texture.bitmap.empty() && !t.materials[0].texture.has_color);
            CHECK(t.materials[0].texture.file == kNoMaterial);
        }
    }

    // En ström med noll fragment.
    {
        ByteWriter w;
        w.i32(0);
        w.pad(8);
        FragmentStore t;
        CHECK(parse(w.data, t) == ErrorCode::Ok);
        CHECK(t.items.empty() && t.looks.empty());
        CHECK(fails_with(Bytes{}, ErrorCode::CorruptObject));
    }

    // Poserna: värden som inte får förekomma.
    {
        Spec s;
        s.pose_class = 15;
        FragmentStore ok;
        CHECK(parse(single(s), ok) == ErrorCode::Ok);

        s = Spec{};
        s.t[1] = kNaN;
        CHECK(fails_with(s, ErrorCode::CorruptObject));
        s = Spec{};
        s.t[2] = kInf;
        CHECK(fails_with(s, ErrorCode::CorruptObject));
        s = Spec{};
        s.pose_class = 14;
        s.t[0] = -kInf;
        CHECK(fails_with(s, ErrorCode::CorruptObject));
        s = Spec{};
        s.q[3] = kNaN;
        CHECK(fails_with(s, ErrorCode::CorruptObject));
        s = Spec{};
        s.pose_class = 16;
        s.s = kNaN;
        CHECK(fails_with(s, ErrorCode::CorruptObject));

        s = Spec{};
        s.pose_class = 16;
        s.s = 0;
        CHECK(fails_with(s, ErrorCode::CorruptObject));
        s.s = -0.0;   // minus noll är också noll
        CHECK(fails_with(s, ErrorCode::CorruptObject));

        // Nollkvaternion i klass 15 och 16.
        s = Spec{};
        s.pose_class = 15;
        s.q[3] = 0;
        CHECK(fails_with(s, ErrorCode::CorruptObject));
        s.pose_class = 16;
        s.s = 2;
        CHECK(fails_with(s, ErrorCode::CorruptObject));
        // Längden ska vara 1 inom 1e-6: en mycket liten eller stor kvaternion är
        // fel, inte en rotation.
        for (const double w : {1e-300, 1e-200, 0.5, 0.999998, 1.000002, 2.0, 1e200}) {
            s.q[3] = w;
            CHECK(fails_with(s, ErrorCode::CorruptObject));
        }
        FragmentStore t;
        for (const double w : {1.0, 0.9999995, 1.0000005}) {
            s.q[3] = w;
            CHECK(parse(single(s), t) == ErrorCode::Ok);
            CHECK(t.items.size() == 1 && fragment_view(t, 0).q[3] == w && fragment_view(t, 0).s == 2);
        }

        // Klass 14 har ingen kvaternion: den blir (0, 0, 0, 1) hur t än ser ut.
        s = Spec{};
        s.pose_class = 14;
        CHECK(parse(single(s), t) == ErrorCode::Ok);
        CHECK(t.items.size() == 1 && fragment_view(t, 0).q[3] == 1 && fragment_view(t, 0).s == 1);
    }

    // Poser, utseenden och geometrier som inte får vara null eller bakåtpekare.
    {
        Spec s;
        s.pose = Slot::Back;   // posen är aldrig en bakåtpekare
        CHECK(fails_with(s, ErrorCode::CorruptObject));
        s = Spec{};
        s.pose_class = 13;     // fel klass
        s.q[3] = 1;
        CHECK(fails_with(s, ErrorCode::CorruptObject));
        s = Spec{};
        s.app = Slot::Null;
        CHECK(fails_with(s, ErrorCode::CorruptObject));
        // Ids i ett ensamt fragment: 100 fragmentet, 101 posen, 102 utseendet, 103 materialet.
        Ids pose_id;
        pose_id.app = pose_id.material = pose_id.geometry = 101;
        s = Spec{};
        s.app = Slot::Back;    // en bakåtpekare till posen: fel klass
        CHECK(fails_with(single(s, pose_id), ErrorCode::CorruptObject));
        s = Spec{};
        s.app = Slot::Back;    // en bakåtpekare till ett id som inte finns
        CHECK(fails_with(single(s), ErrorCode::CorruptObject));
        s = Spec{};
        s.material = Slot::Back;   // en bakåtpekare till posen: fel klass
        CHECK(fails_with(single(s, pose_id), ErrorCode::CorruptObject));
        s = Spec{};
        s.material = Slot::Null;
        CHECK(fails_with(s, ErrorCode::CorruptObject));
        s = Spec{};
        s.geometry = Slot::Null;
        CHECK(fails_with(s, ErrorCode::CorruptObject));
        s = Spec{};
        s.geometry = Slot::Back;   // en bakåtpekare till posen: fel klass
        CHECK(fails_with(single(s, pose_id), ErrorCode::CorruptObject));

        // Extra-pekaren vid flags & 32 får vara null eller en klass 185, men inte
        // en bakåtpekare till fel klass.
        s = Spec{};
        s.flags = 32;
        s.extra = Slot::Null;
        FragmentStore t;
        CHECK(parse(single(s), t) == ErrorCode::Ok);
        s.extra = Slot::New;
        CHECK(parse(single(s), t) == ErrorCode::Ok);
        s.extra = Slot::Back;   // en bakåtpekare till materialet (103): fel klass
        Ids material_id;
        material_id.material = 103;
        CHECK(fails_with(single(s, material_id), ErrorCode::CorruptObject));
        // Utan flags & 32 finns ingen extra-pekare att läsa.
        s = Spec{};
        s.flags = 1;
        s.extra = Slot::New;
        CHECK(parse(single(s), t) == ErrorCode::Ok);
        // En trasig klass 185 (e = 2) är ett fel.
        {
            ByteWriter v;
            v.i32(1);
            v.obj(13);
            v.f64(1);
            v.i32(0);
            v.i32(0);
            v.i32(0);
            v.i32(0);
            v.obj(14);
            for (int i = 0; i < 3; ++i) v.f64(0);
            v.obj(55);
            v.i32(32);
            v.obj(54);
            for (int i = 0; i < 14; ++i) v.f32(0);
            v.obj(185);
            v.str("n");
            v.str("j");
            v.i32(1);
            v.str("a");
            v.i32(2);   // varken 0 eller 1
            v.str("b");
            v.obj(93);
            for (int i = 0; i < 6; ++i) v.f32(0);
            for (int i = 0; i < 3; ++i) v.f64(0);
            v.f32(0);
            v.i32(0);
            v.i32(1);
            v.pad(8);
            CHECK(fails_with(v.data, ErrorCode::CorruptObject));
        }
    }

    // Icke-ändliga värden i materialet och i lådan.
    {
        Spec s;
        s.mat[5] = std::numeric_limits<float>::quiet_NaN();
        CHECK(fails_with(s, ErrorCode::CorruptObject));
        s = Spec{};
        s.mat[13] = std::numeric_limits<float>::infinity();
        CHECK(fails_with(s, ErrorCode::CorruptObject));
        s = Spec{};
        s.bbox[2] = std::numeric_limits<float>::quiet_NaN();
        CHECK(fails_with(s, ErrorCode::CorruptObject));
        s = Spec{};
        s.bbox[5] = -std::numeric_limits<float>::infinity();
        CHECK(fails_with(s, ErrorCode::CorruptObject));
        s = Spec{};
        FragmentStore t;
        CHECK(parse(single(s), t) == ErrorCode::Ok);
    }

    // Antalet fragment: för stort, negativt, och rymt precis.
    {
        Bytes d = single(Spec{});
        Bytes big = d;
        big[0] = 0xE8;   // n = 1000 (0x3E8) ryms inte i resten
        big[1] = 0x03;
        CHECK(fails_with(big, ErrorCode::CorruptObject));
        Bytes negative = d;
        negative[3] = 0x80;
        CHECK(fails_with(negative, ErrorCode::CorruptObject));
        Bytes two = d;
        two[0] = 2;   // ett fragment saknas
        CHECK(fails_with(two, ErrorCode::CorruptObject));
        Bytes zero = d;
        zero[0] = 0;   // ett fragment är över
        CHECK(fails_with(zero, ErrorCode::CorruptObject));
        // Ett antal som är 2^31 - 1 allokerar ingenting.
        Bytes huge = d;
        huge[0] = 0xFF;
        huge[1] = 0xFF;
        huge[2] = 0xFF;
        huge[3] = 0x7F;
        CHECK(fails_with(huge, ErrorCode::CorruptObject));
    }

    // Roten ska vara ett nytt objekt av klass 13.
    {
        ByteWriter w;
        w.i32(1);
        w.obj(14);   // en pose som rot
        for (int i = 0; i < 3; ++i) w.f64(0);
        w.pad(8);
        CHECK(fails_with(w.data, ErrorCode::CorruptObject));

        ByteWriter n;
        n.i32(1);
        n.null_ptr();   // null rot
        n.pad(8);
        CHECK(fails_with(n.data, ErrorCode::CorruptObject));

        ByteWriter b;
        b.i32(2);
        Ids ids, out;
        fragment(b, Spec{}, ids, out);
        b.ref(100);   // en bakåtpekare till det första fragmentet som rot
        b.pad(8);
        CHECK(fails_with(b.data, ErrorCode::CorruptObject));
    }

    // Byte kvar efter slutet, och en utfyllnad som inte är noll.
    {
        Bytes d = single(Spec{});
        Bytes extra = d;
        extra.push_back(1);   // en byte efter slutet
        CHECK(fails_with(extra, ErrorCode::CorruptObject));
        Bytes more = d;
        for (int i = 0; i < 8; ++i) more.push_back(0);   // en hel extra åtta
        CHECK(fails_with(more, ErrorCode::CorruptObject));

        // Ett fragment som slutar på 4 mod 8 följs av en int32 0 som fyller ut.
        ByteWriter w;
        w.i32(1);
        Ids out;
        Spec s;
        s.pose_class = 14;
        s.flags = 1;
        fragment(w, s, Ids{}, out);
        if (w.data.size() % 8 == 4) {
            Bytes bad = w.data;
            bad.push_back(1);   // utfyllnaden är inte noll
            bad.push_back(0);
            bad.push_back(0);
            bad.push_back(0);
            CHECK(fails_with(bad, ErrorCode::CorruptObject));
        } else {
            Bytes bad = w.data;
            bad.push_back(0);
            CHECK(fails_with(bad, ErrorCode::CorruptObject));
        }
    }

    // Varje avhuggen längd av ett enda fragment, med och utan flags & 32.
    for (const int32_t flags : {1, 33}) {
        Spec s;
        s.flags = flags;
        s.extra = Slot::New;
        const Bytes d = single(s);
        for (size_t len = 0; len < d.size(); ++len) {
            FragmentStore t;
            std::string detail;
            if (parse_fragments(d.data(), len, t, nullptr, &detail) != ErrorCode::CorruptObject ||
                detail.empty()) {
                std::printf("  single fragment cut at %zu\n", len);
                ++fails;
            }
        }
    }

    // Felkoderna har en text, och detail anger byten: avhugget i början ger byte 0..4.
    {
        FragmentStore t;
        std::string detail;
        const Bytes d = single(Spec{});
        CHECK(parse_fragments(d.data(), 2, t, nullptr, &detail) == ErrorCode::CorruptObject);
        CHECK(detail.find("LcOpNwdFragments at byte 0") == 0);
    }

    std::printf("test_fragments: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
