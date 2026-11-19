// De små paketen: vyn, ljusen, bakgrunden, filens identitet, loggen och de
// tomma paketen. Syntetiska strömmar per tolk, och alla exempelfilerna.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "HNwd.h"
#include "alloc_probe.h"
#include "container.h"
#include "elements.h"
#include "guid_store.h"
#include "mapped_file.h"
#include "partition.h"
#include "test_data.h"
#include "test_support.h"

using namespace HNwd::detail;
using testdata::Bytes;
using testdata::ByteWriter;

namespace {

bool starts_with(const std::string& s, const char* prefix) {
    return s.compare(0, std::strlen(prefix), prefix) == 0;
}

// En GUID med byten 0, 1, … 15 plus `base`.
void guid_bytes(ByteWriter& w, uint8_t base) {
    for (int i = 0; i < 16; ++i) w.u8(static_cast<uint8_t>(base + i));
}

bool guid_is(const Guid16& g, uint8_t base) {
    for (int i = 0; i < 16; ++i)
        if (g[static_cast<size_t>(i)] != static_cast<uint8_t>(base + i)) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Syntetiska strömmar.

constexpr const char* kLightJson = "{\"version\":2,\"userassets\":[],\"materials\":{}}";
constexpr const char* kSkyJson =
    "{\"version\":2,\"userassets\":[\"0\"],\"materials\":{\"0\":{\"definition\":\"SunAndSky\"}}}";

int test_empty_pair() {
    int fails = 0;
    std::string detail;
    ByteWriter w;
    w.i32(0);
    w.i32(0);
    CHECK(parse_empty_pair(w.data.data(), w.data.size(), "LcOpZoneElement", &detail) == ErrorCode::Ok);
    CHECK(detail.empty());
    // Avhuggen.
    CHECK(parse_empty_pair(w.data.data(), 4, "LcOpZoneElement", &detail) == ErrorCode::CorruptPackage);
    CHECK(starts_with(detail, "LcOpZoneElement at byte 4: "));
    // Innehåll: en form som inte är avkodad.
    ByteWriter v;
    v.i32(0);
    v.i32(3);
    CHECK(parse_empty_pair(v.data.data(), v.data.size(), "LcOpCommonSchemas", &detail) ==
          ErrorCode::Unsupported);
    CHECK(starts_with(detail, "LcOpCommonSchemas at byte 8: "));
    // Något efter slutet.
    w.i32(0);
    w.i32(0);
    CHECK(parse_empty_pair(w.data.data(), w.data.size(), "LcOpZoneElement", &detail) ==
          ErrorCode::CorruptPackage);
    return fails;
}

int test_stats() {
    int fails = 0;
    std::string detail, text;
    bool non_utf8 = true;
    ByteWriter w;
    w.str("line one\nline two");   // 17 byte: utfyllnad till 4 och sedan till 8
    w.pad(8);
    CHECK(parse_stats(w.data.data(), w.data.size(), text, non_utf8, &detail) == ErrorCode::Ok);
    CHECK(text == "line one\nline two" && !non_utf8 && detail.empty());
    // Avhuggen inne i texten.
    CHECK(parse_stats(w.data.data(), 10, text, non_utf8, &detail) == ErrorCode::CorruptPackage);
    CHECK(starts_with(detail, "LcOpNwdStats at byte "));
    // Utfyllnaden till 8 är inte noll.
    Bytes bad = w.data;
    CHECK(bad.size() == 24);
    if (bad.size() == 24) bad[23] = 1;
    CHECK(parse_stats(bad.data(), bad.size(), text, non_utf8, &detail) == ErrorCode::CorruptPackage);
    // Ogiltig UTF-8 är inget fel men sägs.
    ByteWriter u;
    u.str(std::string("a\xff", 2));
    u.pad(8);
    CHECK(parse_stats(u.data.data(), u.data.size(), text, non_utf8, &detail) == ErrorCode::Ok);
    CHECK(non_utf8);
    return fails;
}

Bytes scene_props(int32_t second_zero) {
    ByteWriter w;
    w.str("C:\\dir\\file.nwc");
    guid_bytes(w, 0x10);
    w.i32(0);
    w.i32(second_zero);
    w.pad(8);
    return w.data;
}

int test_scene_props() {
    int fails = 0;
    std::string detail;
    SceneProps sp;
    const Bytes b = scene_props(0);
    CHECK(parse_scene_props(b.data(), b.size(), sp, &detail) == ErrorCode::Ok);
    CHECK(sp.path == "C:\\dir\\file.nwc" && !sp.path_null && guid_is(sp.guid, 0x10));
    CHECK(parse_scene_props(b.data(), 24, sp, &detail) == ErrorCode::CorruptPackage);
    const Bytes other = scene_props(5);
    CHECK(parse_scene_props(other.data(), other.size(), sp, &detail) == ErrorCode::Unsupported);
    CHECK(starts_with(detail, "LcOpOdyScenePropsChunk at byte "));
    return fails;
}

// En FileInfo; `minus` är det andra av de två -1, och `third` den tredje
// prefixsträngen (null om nullptr).
Bytes file_info(bool first_null, int32_t minus, const char* third = nullptr) {
    ByteWriter w;
    if (first_null) w.null_str();
    else w.str("x");
    w.i32(0);
    guid_bytes(w, 0x20);
    w.i32(0);
    w.str("G");
    w.str("model.nwc");
    w.i32(0);
    guid_bytes(w, 0x40);
    w.i32(0);
    w.str("cache");
    w.i32(0);
    w.i32(0);
    w.i32(-1);
    w.i32(minus);
    if (third) w.str(third);
    else w.null_str();
    guid_bytes(w, 0x40);
    w.str("cache");
    w.i32(0);
    guid_bytes(w, 0x20);
    w.zeros(16);
    w.pad(8);
    return w.data;
}

int test_file_info() {
    int fails = 0;
    std::string detail;
    FileInfo fi;
    const Bytes b = file_info(true, -1);
    CHECK(parse_file_info(b.data(), b.size(), fi, &detail) == ErrorCode::Ok);
    CHECK(guid_is(fi.file_guid, 0x20) && fi.tag == "G" && fi.name == "model.nwc");
    CHECK(guid_is(fi.scene_guid, 0x40) && fi.prefix == "cache");
    CHECK(fi.prefix_third.empty() && fi.prefix_third_null);
    CHECK(guid_is(fi.scene_guid_again, 0x40) && fi.prefix_again == "cache");
    CHECK(guid_is(fi.guid_c, 0x20) && fi.guid_d == Guid16{});
    CHECK(parse_file_info(b.data(), b.size() - 12, fi, &detail) == ErrorCode::CorruptPackage);
    const Bytes m = file_info(true, 2);
    CHECK(parse_file_info(m.data(), m.size(), fi, &detail) == ErrorCode::Unsupported);
    CHECK(starts_with(detail, "LcOpOdyFileInfoChunk at byte "));
    const Bytes n = file_info(false, -1);
    CHECK(parse_file_info(n.data(), n.size(), fi, &detail) == ErrorCode::Unsupported);
    // Den tredje prefixsträngen med text: GUID:erna efter den hamnar på en
    // annan position, och de nollor som är egna fält står kvar.
    const Bytes t = file_info(true, -1, "Modell 7");
    CHECK(parse_file_info(t.data(), t.size(), fi, &detail) == ErrorCode::Ok);
    CHECK(fi.prefix_third == "Modell 7" && !fi.prefix_third_null);
    CHECK(guid_is(fi.scene_guid_again, 0x40) && guid_is(fi.guid_c, 0x20));
    return fails;
}

Bytes xref(const std::string& json) {
    ByteWriter w;
    w.str(json);
    w.pad(8);
    return w.data;
}

int test_xref() {
    int fails = 0;
    std::string detail;
    XRefTable x;
    const Bytes ok = xref("{\"Type\":\"XRefTable\",\"Version\":1,"
                          "\"SavedFilename\":\"C:\\\\a\\\\b\\u00e5.nwd\",\"Table\":[ ]}");
    CHECK(parse_xref_table(ok.data(), ok.size(), x, &detail) == ErrorCode::Ok);
    CHECK(x.version == 1 && x.saved_filename == "C:\\a\\b\xc3\xa5.nwd");
    // Nycklarna i en annan ordning, med blanksteg.
    const Bytes order = xref(" { \"Table\" : [] , \"SavedFilename\":\"\\ud83d\\ude00\", "
                             "\"Version\": 1.0, \"Type\":\"XRefTable\" } ");
    CHECK(parse_xref_table(order.data(), order.size(), x, &detail) == ErrorCode::Ok);
    CHECK(x.saved_filename == "\xf0\x9f\x98\x80");
    // En tabell med poster, en annan version, en okänd nyckel och en som saknas.
    const char* unsupported[] = {
        "{\"Type\":\"XRefTable\",\"Version\":1,\"SavedFilename\":\"\",\"Table\":[{}]}",
        "{\"Type\":\"XRefTable\",\"Version\":2,\"SavedFilename\":\"\",\"Table\":[]}",
        "{\"Type\":\"Other\",\"Version\":1,\"SavedFilename\":\"\",\"Table\":[]}",
        "{\"Type\":\"XRefTable\",\"Version\":1,\"SavedFilename\":\"\",\"Table\":[],\"X\":0}",
        "{\"Type\":\"XRefTable\",\"Version\":1,\"Table\":[]}",
    };
    for (const char* j : unsupported) {
        const Bytes b = xref(j);
        CHECK(parse_xref_table(b.data(), b.size(), x, &detail) == ErrorCode::Unsupported);
        CHECK(starts_with(detail, "LcOpXRefTable at byte "));
    }
    // JSON som inte går att tolka, och fel slags värde.
    const char* corrupt[] = {
        "{\"Type\":\"XRefTable\",\"Version\":1,\"SavedFilename\":\"\",\"Table\":[]",
        "{\"Type\":\"XRefTable\",\"Version\":1,\"SavedFilename\":\"\\q\",\"Table\":[]}",
        "{\"Type\":\"XRefTable\",\"Version\":\"1\",\"SavedFilename\":\"\",\"Table\":[]}",
        "{\"Type\":\"XRefTable\",\"Version\":1,\"SavedFilename\":\"\",\"Table\":{}}",
        "{\"Type\":\"XRefTable\",\"Type\":\"XRefTable\",\"Version\":1,\"SavedFilename\":\"\",\"Table\":[]}",
        "{\"Type\":\"XRefTable\",\"Version\":1,\"SavedFilename\":\"\\ud800\",\"Table\":[]} x",
    };
    for (const char* j : corrupt) {
        const Bytes b = xref(j);
        CHECK(parse_xref_table(b.data(), b.size(), x, &detail) == ErrorCode::CorruptPackage);
        CHECK(starts_with(detail, "LcOpXRefTable at byte "));
    }
    // Avhuggen.
    CHECK(parse_xref_table(ok.data(), 20, x, &detail) == ErrorCode::CorruptPackage);
    return fails;
}

// Snittplanen sist i en vy, med påhittade värden: planet `on` påslaget, och
// `count` i stället för 6.
void write_clip(ByteWriter& w, int on = 0, int32_t count = 6) {
    for (int32_t i = 0; i < 6; ++i) w.i32(10 + i);
    for (const double v : {0.5, 1.5, 2.5}) w.f64(v);
    w.i32(count);
    for (int k = 0; k < 6; ++k) {
        w.i32(k == on ? 1 : 0);
        w.i32(20 + k);
        for (const double v : {1.0 * k, 2.0 * k, 3.0 * k}) w.f64(v);
        for (const double v : {0.0, 0.0, 1.0}) w.f64(v);
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

// En vy i form 9, 843, 895 eller 1919, skriven där `w` står; `more` byte efter
// snittplanen.
void write_view(ByteWriter& w, int32_t form, int32_t projection, size_t more = 0) {
    w.i32(form);
    w.i32(projection);
    for (const double v : {1.0, 2.0, 3.0}) w.f64(v);
    for (const double v : {0.0, 0.0, 0.0, 1.0}) w.f64(v);
    for (const double v : {1.5, 0.75, 4.0, 40.0, 0.0, 0.0}) w.f64(v);
    if (form != 9) {
        for (const double v : {300.0, 1800.0, 1800.0, 150.0}) w.f64(v);
        w.str("someone");
        for (const double v : {0.0, 0.0, 3000.0, 1.0}) w.f64(v);
        w.i32(1);
        for (const double v : {0.0, 9800.0, 50000.0, 0.0}) w.f64(v);
    }
    if (form == 9 || form == 843) {
        w.f64(12.5);
    } else {
        for (const double v : {0.0, 0.0, 1.0, 20.0, 30.0, 0.5}) w.f64(v);
    }
    const int ints = form == 9 ? 2 : 6;
    for (int i = 0; i < ints; ++i) {
        if (form == 1919 && i == 4) w.f64(0.25);
        w.i32(i);
    }
    write_clip(w, 2);
    for (size_t i = 0; i < more; ++i) w.u8(1);
}

// Vyn som ett eget paket.
Bytes view(int32_t form, size_t more = 0, int32_t projection = 0) {
    ByteWriter w;
    write_view(w, form, projection, more);
    w.pad(8);
    return w.data;
}

// Snittplanen som write_clip() skrev dem, med planet `on` påslaget.
bool clip_is(const ViewClip& c, int on) {
    bool ok = c.head[0] == 10 && c.head[5] == 15 && c.head_values[2] == 2.5;
    for (int k = 0; k < 6; ++k) {
        const ClipPlane& p = c.planes[k];
        ok = ok && p.enabled == (k == on ? 1 : 0) && p.mode == 20 + k && p.point[1] == 2.0 * k &&
             p.normal[2] == 1 && p.tangent[0] == 1;
    }
    return ok && c.flags_a[1] == 2 && c.values_a[5] == 6 && c.flags_b[0] == 3 &&
           c.values_b[0] == 7 && c.rotation[2] == 0.6 && c.rotation[3] == 0.8;
}

int test_view_clip() {
    int fails = 0;
    std::string detail;
    ViewElement v;
    // Från jämn 8 byte är snittplanen 680 byte, med PAD8 i det första planet.
    ByteWriter w;
    write_clip(w);
    CHECK(w.data.size() == kViewRestBytes);
    CHECK(w.data[60] == 0 && w.data[56] == 20);

    // Ett annat antal plan än 6, och en strömbrytare som varken är 0 eller 1.
    ByteWriter c;
    write_view(c, 9, 0);
    const size_t clip_at = c.data.size() - kViewRestBytes;
    CHECK(clip_at % 8 == 0);
    c.pad(8);
    CHECK(parse_view_element(c.data.data(), c.data.size(), v, &detail) == ErrorCode::Ok);
    CHECK(clip_is(v.clip, 2));
    // Byten lämnas också ut som de står.
    CHECK(v.rest.size() == kViewRestBytes &&
          std::equal(v.rest.begin(), v.rest.end(), c.data.begin() + static_cast<std::ptrdiff_t>(clip_at)));
    Bytes count = c.data;
    count[clip_at + 48] = 7;
    CHECK(parse_view_element(count.data(), count.size(), v, &detail) == ErrorCode::Unsupported);
    CHECK(starts_with(detail, ("LcOpCurrentViewElement at byte " + std::to_string(clip_at + 52) + ": ").c_str()));
    Bytes on = c.data;
    on[clip_at + 52] = 2;
    CHECK(parse_view_element(on.data(), on.size(), v, &detail) == ErrorCode::Unsupported);
    // PAD8 i det första planet är inte noll.
    Bytes pad = c.data;
    pad[clip_at + 60] = 1;
    CHECK(parse_view_element(pad.data(), pad.size(), v, &detail) == ErrorCode::CorruptPackage);
    return fails;
}

int test_view() {
    int fails = 0;
    std::string detail;
    ViewElement v;
    const Bytes nine = view(9);
    CHECK(parse_view_element(nine.data(), nine.size(), v, &detail) == ErrorCode::Ok);
    CHECK(v.form == 9 && v.projection == 0 && v.position[0] == 1 && v.position[2] == 3 &&
          v.rotation[3] == 1);
    CHECK(v.u == 1.5 && v.angle == 0.75 && v.near_depth == 4 && v.far_depth == 40);
    CHECK(v.distance == 12.5 && v.tail_ints.size() == 2 && v.tail_ints[1] == 1);
    CHECK(v.avatar.empty() && clip_is(v.clip, 2));

    // Strängen i form 843 lägger flyttalen på 4 mod 8; PAD8 tar dem till 8.
    const Bytes big = view(843);
    CHECK(parse_view_element(big.data(), big.size(), v, &detail) == ErrorCode::Ok);
    CHECK(v.form == 843 && v.avatar == "someone" && v.avatar_values[1] == 1800);
    CHECK(v.after_avatar[2] == 3000 && v.avatar_flag == 1 && v.more[2] == 50000);
    CHECK(v.distance == 12.5 && v.tail_ints.size() == 6 && v.tail_ints[5] == 5);
    CHECK(clip_is(v.clip, 2));

    // Form 895: sex f64 i stället för distance.
    const Bytes f895 = view(895);
    CHECK(parse_view_element(f895.data(), f895.size(), v, &detail) == ErrorCode::Ok);
    CHECK(v.form == 895 && v.avatar == "someone" && v.more[1] == 9800);
    CHECK(v.extra[2] == 1 && v.extra[3] == 20 && v.extra[5] == 0.5 && v.distance == 0);
    CHECK(v.tail_ints.size() == 6 && v.tail_ints[4] == 4 && v.ortho_extra == 0);
    CHECK(clip_is(v.clip, 2));
    CHECK(f895.size() == big.size() + 40);

    // Form 1919: en f64 mellan det fjärde och femte heltalet, och projection 1.
    const Bytes f1919 = view(1919, 0, 1);
    CHECK(parse_view_element(f1919.data(), f1919.size(), v, &detail, "LcOpHomeViewElement") ==
          ErrorCode::Ok);
    CHECK(v.form == 1919 && v.projection == 1 && v.extra[4] == 30 && v.ortho_extra == 0.25);
    CHECK(v.tail_ints.size() == 6 && v.tail_ints[3] == 3 && v.tail_ints[4] == 4);
    CHECK(clip_is(v.clip, 2));

    const Bytes seven = view(7);
    CHECK(parse_view_element(seven.data(), seven.size(), v, &detail) == ErrorCode::Unsupported);
    CHECK(starts_with(detail, "LcOpCurrentViewElement at byte 4: "));
    CHECK(parse_view_element(seven.data(), seven.size(), v, &detail, "LcOpHomeViewElement") ==
          ErrorCode::Unsupported);
    CHECK(starts_with(detail, "LcOpHomeViewElement at byte 4: "));
    const Bytes longer = view(9, 8);
    CHECK(parse_view_element(longer.data(), longer.size(), v, &detail) == ErrorCode::Unsupported);
    CHECK(parse_view_element(nine.data(), 100, v, &detail) == ErrorCode::CorruptPackage);
    CHECK(parse_view_element(nine.data(), nine.size() - 8, v, &detail) == ErrorCode::CorruptPackage);
    // Projektionen är varken 0 eller 1.
    const Bytes proj = view(9, 0, 2);
    CHECK(parse_view_element(proj.data(), proj.size(), v, &detail) == ErrorCode::Unsupported);
    CHECK(starts_with(detail, "LcOpCurrentViewElement at byte 8: "));
    // PAD8 efter avatarens flagga är inte noll.
    Bytes bad_pad = big;
    // Form 843: 13 f64 från 8, fyra till från 112, strängen på 144–156, PAD8
    // och fyra f64 på 160–192, flaggan på 192 och utfyllnaden på 196.
    const size_t flag_pad = 196;
    CHECK(bad_pad[flag_pad - 4] == 1 && bad_pad[flag_pad] == 0);
    bad_pad[flag_pad] = 1;
    CHECK(parse_view_element(bad_pad.data(), bad_pad.size(), v, &detail) == ErrorCode::CorruptPackage);
    return fails;
}

int test_small_elements() {
    int fails = 0;
    std::string detail;

    ByteWriter h;
    h.f64(0.3);
    h.f64(1);
    HeadlightElement hl;
    CHECK(parse_headlight(h.data.data(), h.data.size(), hl, &detail) == ErrorCode::Ok);
    CHECK(hl.values[0] == 0.3 && hl.values[1] == 1);
    CHECK(parse_headlight(h.data.data(), 12, hl, &detail) == ErrorCode::CorruptPackage);
    CHECK(starts_with(detail, "LcOpHeadlightElement at byte 8: "));

    ByteWriter s;
    s.f64(1.0 / 6);
    double speed = 0;
    CHECK(parse_speed(s.data.data(), s.data.size(), speed, &detail) == ErrorCode::Ok);
    CHECK(speed == 1.0 / 6);
    s.f64(2);
    CHECK(parse_speed(s.data.data(), s.data.size(), speed, &detail) == ErrorCode::CorruptPackage);

    ByteWriter c;
    c.f64(1);
    for (int i = 0; i < 4; ++i) c.f64(i);
    c.i32(1);
    c.pad(8);
    CullingElement cu;
    CHECK(parse_culling(c.data.data(), c.data.size(), cu, &detail) == ErrorCode::Ok);
    CHECK(cu.first == 1 && cu.values[3] == 3 && cu.flag == 1);
    Bytes bad = c.data;
    CHECK(bad.size() == 48);
    if (bad.size() == 48) bad[47] = 7;   // utfyllnaden sist är inte noll
    CHECK(parse_culling(bad.data(), bad.size(), cu, &detail) == ErrorCode::CorruptPackage);

    ByteWriter b;
    b.i32(2);
    for (int k = 0; k < 7; ++k)
        for (int i = 0; i < 3; ++i) b.f64((k * 3 + i) / 255.0);
    b.i32(0);
    b.null_ptr();
    BackgroundElement bg;
    CHECK(b.data.size() == 184);
    CHECK(parse_background(b.data.data(), b.data.size(), bg, &detail) == ErrorCode::Ok);
    CHECK(bg.mode == 2 && bg.colours[6][2] == 20 / 255.0 && !bg.has_sky);
    CHECK(parse_background(b.data.data(), 176, bg, &detail) == ErrorCode::CorruptPackage);
    // Med en himmel: ett material av klass 185 i stället för null.
    ByteWriter sky;
    sky.i32(0);
    for (int k = 0; k < 21; ++k) sky.f64(k / 255.0);
    sky.i32(0);
    sky.obj(185);
    sky.str(kSkyJson);
    sky.null_str();
    sky.i32(0);
    sky.pad(8);
    CHECK(parse_background(sky.data.data(), sky.data.size(), bg, &detail) == ErrorCode::Ok);
    CHECK(bg.mode == 0 && bg.has_sky && bg.sky.json == kSkyJson && bg.sky.files.empty());
    // Nollan före pekaren är inte 0.
    Bytes marker = sky.data;
    marker[176] = 3;
    CHECK(parse_background(marker.data(), marker.size(), bg, &detail) == ErrorCode::Unsupported);
    CHECK(starts_with(detail, "LcOpBackgroundElement at byte 180: "));
    // En annan klass än 185.
    Bytes cls = sky.data;
    cls[184] = 186;
    CHECK(parse_background(cls.data(), cls.size(), bg, &detail) == ErrorCode::CorruptPackage);

    ByteWriter t;
    t.i32(3);
    t.i32(0);
    int32_t tool = 0;
    CHECK(parse_tool(t.data.data(), t.data.size(), tool, &detail) == ErrorCode::Ok && tool == 3);
    t.i32(5);
    CHECK(parse_tool(t.data.data(), t.data.size(), tool, &detail) == ErrorCode::CorruptPackage);

    ByteWriter n;
    for (const int32_t v : {7, 0, 9, 0}) n.i32(v);
    NextCommentId nc;
    CHECK(parse_next_comment_id(n.data.data(), n.data.size(), nc, &detail) == ErrorCode::Ok);
    CHECK(nc.values[0] == 7 && nc.values[1] == 9);
    Bytes hi = n.data;
    hi[12] = 1;
    CHECK(parse_next_comment_id(hi.data(), hi.size(), nc, &detail) == ErrorCode::Unsupported);
    CHECK(starts_with(detail, "LcOpNextCommentIdElement at byte 16: "));
    CHECK(parse_next_comment_id(n.data.data(), 12, nc, &detail) == ErrorCode::CorruptPackage);
    return fails;
}

// ---------------------------------------------------------------------------
// Sparade vyer.

// Två poster: en vy med två markeringar och två 89-objekt, och en mapp med
// en vy som pekar bakåt på det första objektet. Varianterna byter ut mappens
// slag, den första markeringens typ, det första elementets namn och det
// andra talet i FragMaterial.
Bytes saved_views(int32_t folder_kind = 4, int32_t redline_type = 4,
                  const char* element = "LcOpShadOverridesElement", int32_t fm = 0) {
    ByteWriter w;
    w.i32(2);
    w.i32(0);
    w.str("Vy 1");
    w.i32(0);
    guid_bytes(w, 0x10);
    w.i32(0);
    write_view(w, 895, 0);
    w.i32(2);
    w.i32(redline_type);
    w.str("Text");
    w.f64(-0.5);
    w.f64(0.25);
    w.i32(5);
    for (const double c : {0.0, 0.5, 1.0}) w.f64(c);
    w.i32(0x0F0F);
    w.i32(1);
    for (const double v : {-1.0, -0.5, 1.0, 0.75}) w.f64(v);
    w.i32(2);
    for (const double c : {0.25, 0.75, 0.0}) w.f64(c);
    w.i32(7);
    w.i32(2);
    w.obj(89);   // 100
    w.str(element);
    for (const int32_t v : {1, 42, 1, 43, 0}) w.i32(v);
    w.obj(89);   // 101
    w.str("LcOpShadFragMaterialElement");
    w.i32(0);
    w.i32(fm);
    // Mappen med en vy.
    w.i32(folder_kind);
    w.str("Mapp A");
    w.i32(0);
    guid_bytes(w, 0x20);
    w.i32(0);
    w.i32(1);
    w.i32(0);
    w.str("Vy 22");
    w.i32(0);
    guid_bytes(w, 0x30);
    w.i32(0);
    write_view(w, 1919, 1);
    w.i32(0);
    w.i32(1);
    w.ref(100);
    w.pad(8);
    return w.data;
}

int test_saved_views() {
    int fails = 0;
    std::string detail;
    SavedViews sv;
    const Bytes b = saved_views();
    CHECK(parse_saved_views(b.data(), b.size(), sv, &detail) == ErrorCode::Ok);
    CHECK(detail.empty());
    if (sv.items.size() == 3 && sv.views.size() == 2 && sv.states.size() == 2 &&
        sv.redlines.size() == 2 && sv.state_refs.size() == 3) {
        const SavedViews::Item& a = sv.items[0];
        CHECK(a.kind == 0 && a.name == "Vy 1" && guid_is(a.guid, 0x10) && a.parent == UINT32_MAX);
        CHECK(a.view == 0);
        const SavedViews::View& av = sv.views[0];
        CHECK(av.view.form == 895 && av.view.extra[3] == 20 && clip_is(av.view.clip, 2));
        CHECK(av.view.rest.empty());   // SNITT som byten står sparas inte i en sparad vy
        CHECK(av.redline_begin == 0 && av.redline_count == 2);
        const Redline& t = sv.redlines[0];
        CHECK(t.type == 4 && t.text == "Text" && t.points[0] == -0.5 && t.points[1] == 0.25);
        CHECK(t.points[2] == 0 && t.width == 5 && t.colour[2] == 1 && t.pattern == 0x0F0F);
        const Redline& l = sv.redlines[1];
        CHECK(l.type == 1 && l.text.empty() && l.points[3] == 0.75 && l.width == 2);
        CHECK(l.colour[1] == 0.75 && l.pattern == 7);
        CHECK(av.state_begin == 0 && av.state_count == 2 && sv.state_refs[0] == 0 &&
              sv.state_refs[1] == 1);
        CHECK(sv.states[0].element == "LcOpShadOverridesElement");
        CHECK(sv.states[0].indices == std::vector<int32_t>({42, 43}));
        CHECK(sv.states[1].element == "LcOpShadFragMaterialElement" && sv.states[1].indices.empty());
        const SavedViews::Item& folder = sv.items[1];
        CHECK(folder.kind == 4 && folder.name == "Mapp A" && folder.child_count == 1);
        CHECK(folder.parent == UINT32_MAX && folder.view == UINT32_MAX);
        const SavedViews::Item& c = sv.items[2];
        CHECK(c.kind == 0 && c.name == "Vy 22" && c.parent == 1 && guid_is(c.guid, 0x30));
        CHECK(c.view == 1);
        const SavedViews::View& cv = sv.views[1];
        CHECK(cv.view.form == 1919 && cv.view.projection == 1 && cv.view.ortho_extra == 0.25);
        CHECK(cv.redline_begin == 2 && cv.redline_count == 0);
        CHECK(cv.state_begin == 2 && cv.state_count == 1 && sv.state_refs[2] == 0);
    } else {
        CHECK(!"three items, two views and two states");
    }

    // Tomma mappar: minnet för det tolkade paketet följer strömmen. En mapp
    // är 36 byte i strömmen och bär ingen vy.
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
        SavedViews many;
        CHECK(parse_saved_views(w.data.data(), w.data.size(), many, &detail) == ErrorCode::Ok);
        CHECK(many.items.size() == static_cast<size_t>(n) && many.views.empty());
        size_t bytes = many.items.capacity() * sizeof(SavedViews::Item);
        for (const SavedViews::Item& it : many.items) bytes += it.name.capacity();
        if (bytes > 4 * w.data.size())
            std::printf("  %d empty folders: %zu bytes from a %zu-byte stream\n", n, bytes, w.data.size());
        CHECK(bytes <= 4 * w.data.size());
    }

    // Ett okänt slag, en okänd markering, ett okänt element och ett
    // FragMaterial som inte är tomt.
    const Bytes forms[] = {saved_views(5), saved_views(4, 2), saved_views(4, 4, "LcOpOther"),
                           saved_views(4, 4, "LcOpShadOverridesElement", 3)};
    for (const Bytes& f : forms) {
        CHECK(parse_saved_views(f.data(), f.size(), sv, &detail) == ErrorCode::Unsupported);
        CHECK(starts_with(detail, "LcOpSavedViewsElement at byte "));
    }
    // Avhuggen, och en bakåtpekare till ett okänt id.
    CHECK(parse_saved_views(b.data(), b.size() - 40, sv, &detail) == ErrorCode::CorruptObject);
    // Den sista platsen är [1][100]; [1][102] pekar på ett id som inte finns.
    Bytes back = b;
    size_t at = back.size();
    for (size_t k = back.size() - 5; k-- > 0;) {
        if (back[k] == 1 && back[k + 1] == 0 && back[k + 4] == 100 && back[k + 5] == 0) {
            at = k + 4;
            break;
        }
    }
    CHECK(at < back.size());
    if (at < back.size()) {
        back[at] = 102;
        CHECK(parse_saved_views(back.data(), back.size(), sv, &detail) == ErrorCode::CorruptObject);
    }

    // Tomt: [0] och nollor.
    ByteWriter e;
    e.i32(0);
    e.pad(8);
    CHECK(parse_saved_views(e.data.data(), e.data.size(), sv, &detail) == ErrorCode::Ok);
    CHECK(sv.items.empty() && sv.views.empty() && sv.redlines.empty() && sv.state_refs.empty() &&
          sv.states.empty());

    // Mappar i mappar djupare än taket.
    ByteWriter deep;
    deep.i32(1);
    for (int k = 0; k <= kMaxNesting + 1; ++k) {
        deep.i32(4);
        deep.str("M");
        deep.i32(0);
        guid_bytes(deep, 0);
        deep.i32(0);
        deep.i32(k <= kMaxNesting ? 1 : 0);
    }
    deep.pad(8);
    CHECK(parse_saved_views(deep.data.data(), deep.data.size(), sv, &detail) ==
          ErrorCode::LimitExceeded);
    return fails;
}

int test_overrides_and_texture_space() {
    int fails = 0;
    std::string detail;
    ByteWriter o;
    o.i32(2);
    for (const int32_t v : {5, 3, 9, 6}) o.i32(v);
    o.pad(8);
    std::vector<ShadOverride> ov;
    CHECK(parse_shad_overrides(o.data.data(), o.data.size(), ov, &detail) == ErrorCode::Ok);
    CHECK(ov.size() == 2 && ov[0].index == 5 && ov[1].index == 9 && ov[1].flags == 6);
    CHECK(parse_shad_overrides(o.data.data(), 16, ov, &detail) == ErrorCode::CorruptPackage);
    Bytes many = o.data;
    many[0] = 3;   // tre poster ryms inte
    CHECK(parse_shad_overrides(many.data(), many.size(), ov, &detail) == ErrorCode::CorruptPackage);

    ByteWriter t;
    t.i32(0);
    t.i32(2);
    for (int k = 0; k < 2; ++k) {
        t.i32(10 + k);
        t.i32(2);
        for (int i = 0; i < 16; ++i) t.f64(i + k);
    }
    std::vector<TextureSpace> ts;
    CHECK(parse_texture_space(t.data.data(), t.data.size(), ts, &detail) == ErrorCode::Ok);
    CHECK(ts.size() == 2 && ts[1].index == 11 && ts[1].kind == 2 && ts[1].values[15] == 16);
    Bytes first = t.data;
    first[0] = 1;
    CHECK(parse_texture_space(first.data(), first.size(), ts, &detail) == ErrorCode::Unsupported);
    CHECK(starts_with(detail, "LcOpTextureSpaceElement at byte 4: "));
    CHECK(parse_texture_space(t.data.data(), t.data.size() - 8, ts, &detail) ==
          ErrorCode::CorruptPackage);
    return fails;
}

int test_shared_nodes() {
    int fails = 0;
    std::string detail;
    ByteWriter w;
    w.i32(3);
    w.obj(102);   // 100
    w.str("Typsnitt A");
    w.f64(2.5);
    for (const int32_t v : {12, 0, 0, 0}) w.i32(v);
    w.obj(102);   // 101
    w.str("Typsnitt B");
    w.f64(9.25);
    for (const int32_t v : {12, 1, 2, 3}) w.i32(v);
    w.ref(100);
    w.pad(8);
    SharedNodes sn;
    CHECK(parse_shared_nodes(w.data.data(), w.data.size(), sn, &detail) == ErrorCode::Ok);
    CHECK(sn.entries == std::vector<uint32_t>({0, 1, 0}) && sn.fonts.size() == 2);
    if (sn.fonts.size() == 2) {
        CHECK(sn.fonts[0].name == "Typsnitt A" && sn.fonts[0].size == 2.5 && sn.fonts[0].values[0] == 12);
        CHECK(sn.fonts[1].name == "Typsnitt B" && sn.fonts[1].size == 9.25 && sn.fonts[1].values[3] == 3);
    }
    // En annan klass, och avhuggen.
    Bytes cls = w.data;
    cls[8] = 103;
    CHECK(parse_shared_nodes(cls.data(), cls.size(), sn, &detail) == ErrorCode::CorruptObject);
    CHECK(starts_with(detail, "LcOpNwdSharedNodes at byte "));
    CHECK(parse_shared_nodes(w.data.data(), w.data.size() - 8, sn, &detail) == ErrorCode::CorruptObject);
    // Tomt: [0] och nollor.
    ByteWriter e;
    e.i32(0);
    e.i32(0);
    CHECK(parse_shared_nodes(e.data.data(), e.data.size(), sn, &detail) == ErrorCode::Ok);
    CHECK(sn.entries.empty() && sn.fonts.empty());
    return fails;
}

// Två rutnätssystem: ett med två linjer och två nivåer, ett tomt. `level`
// är det förstas nivåindex.
Bytes grid(int32_t level) {
    ByteWriter w;
    w.i32(2);
    w.str("System 1");
    for (const double v : {10.0, 20.0, 0.0}) w.f64(v);
    for (const double v : {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 0.0}) w.f64(v);
    w.i32(2);
    w.str("A");
    w.f64(-2);
    w.f64(5);
    for (const int32_t v : {2, 0, 3}) w.i32(v);
    for (const double v : {0.0, 0.0, 0.0, 10.0}) w.f64(v);
    w.str("1");
    w.f64(-2);
    w.f64(6);
    for (const int32_t v : {2, 0, 3}) w.i32(v);
    for (const double v : {-2.0, 4.0, 12.0, 4.0}) w.f64(v);
    w.i32(2);
    w.str("Nivå A");
    w.f64(0);
    w.str("Nivå B");
    w.f64(3.5);
    w.i32(level);
    w.str("Tomt");
    for (int k = 0; k < 12; ++k) w.f64(k == 3 || k == 7 || k == 10 ? 1 : 0);
    w.i32(0);
    w.i32(0);
    w.i32(-1);
    w.i32(1);
    w.i32(6);
    w.pad(8);
    return w.data;
}

int test_grid() {
    int fails = 0;
    std::string detail;
    GridElement g;
    const Bytes b = grid(1);
    CHECK(parse_grid(b.data(), b.size(), g, &detail) == ErrorCode::Ok);
    CHECK(g.systems.size() == 2 && g.tail[0] == 1 && g.tail[1] == 6);
    if (g.systems.size() == 2) {
        const GridSystem& s = g.systems[0];
        CHECK(s.name == "System 1" && s.origin[1] == 20 && s.axes[1][1] == 1 && s.axes[2][1] == 1);
        CHECK(s.lines.size() == 2 && s.levels.size() == 2 && s.level == 1);
        if (s.lines.size() == 2) {
            CHECK(s.lines[0].label == "A" && s.lines[0].values[1] == 5 && s.lines[0].ints[2] == 3);
            CHECK(s.lines[1].label == "1" && s.lines[1].ends[0] == -2 && s.lines[1].ends[2] == 12);
        }
        if (s.levels.size() == 2) CHECK(s.levels[1].name == "Nivå B" && s.levels[1].elevation == 3.5);
        const GridSystem& e = g.systems[1];
        CHECK(e.name == "Tomt" && e.lines.empty() && e.levels.empty() && e.level == -1);
    }
    // Nivåindex utanför listan.
    for (const int32_t bad : {2, -2}) {
        const Bytes x = grid(bad);
        CHECK(parse_grid(x.data(), x.size(), g, &detail) == ErrorCode::Unsupported);
        CHECK(starts_with(detail, "LcOpGridElement at byte "));
    }
    CHECK(parse_grid(b.data(), b.size() - 16, g, &detail) == ErrorCode::CorruptPackage);
    return fails;
}

// Två material: det andra pekar bakåt på det förstas klass 185, och det
// tredje har ingen. `kind` är det förstas slag.
Bytes re_materials(int32_t kind) {
    ByteWriter w;
    w.i32(3);
    w.i32(kind);
    w.str("Betong");
    w.i32(0);
    guid_bytes(w, 0x70);
    w.i32(0);
    w.obj(185);   // 100
    w.str(kLightJson);
    w.null_str();
    w.i32(1);
    w.str("bild.png");
    w.i32(1);
    w.str("navis");
    w.str("png");
    w.i64a(3);
    w.text("abc");
    w.pad(4);
    w.i32(80);
    w.str("Betong 2");
    w.i32(0);
    guid_bytes(w, 0x80);
    w.i32(0);
    w.ref(100);
    w.i32(80);
    w.str("Utan");
    w.i32(0);
    guid_bytes(w, 0x90);
    w.i32(0);
    w.null_ptr();
    w.pad(8);
    return w.data;
}

int test_re_materials() {
    int fails = 0;
    std::string detail;
    ReMaterials m;
    const Bytes b = re_materials(80);
    CHECK(parse_re_materials(b.data(), b.size(), m, &detail) == ErrorCode::Ok);
    CHECK(m.entries.size() == 3 && m.materials.size() == 1);
    if (m.entries.size() == 3 && m.materials.size() == 1) {
        CHECK(m.entries[0].name == "Betong" && guid_is(m.entries[0].guid, 0x70));
        CHECK(m.entries[0].material == 0 && m.entries[1].material == 0);
        CHECK(m.entries[1].name == "Betong 2" && guid_is(m.entries[1].guid, 0x80));
        CHECK(m.entries[2].material == kNoMaterial);
        const MaterialData& d = m.materials[0];
        CHECK(d.json == kLightJson && d.files.size() == 1);
        if (d.files.size() == 1)
            CHECK(d.files[0].embedded && d.files[0].type == "png" && d.files[0].bytes.size() == 3);
    }
    const Bytes k = re_materials(81);
    CHECK(parse_re_materials(k.data(), k.size(), m, &detail) == ErrorCode::Unsupported);
    CHECK(starts_with(detail, "LcReMaterialElement at byte 8: "));
    CHECK(parse_re_materials(b.data(), b.size() - 24, m, &detail) == ErrorCode::CorruptObject);
    return fails;
}

// Vad clash() ska ändra, och var det står.
struct ClashSpec {
    int32_t second_root = 54;   // slaget på den andra roten
    int32_t test_marker = -1;   // det andra fasta talet i det andra testet
    int32_t rule_flag = 1;      // talet efter den första regeln
    double model_zero = 0;      // nollan före `up` i den första modellen
    size_t test_marker_at = 0;  // fylls i
};

// Ett huvud: [i32 slag] [str namn] [i32 0] [GUID] [i32 0].
void clash_head(ByteWriter& w, int32_t kind, const char* name, uint8_t guid) {
    w.i32(kind);
    w.str(name);
    w.i32(0);
    guid_bytes(w, guid);
    w.i32(0);
}

// En modell med påhittade värden; `name` får olika längd, så att talen efter
// inläsaren hamnar på olika lägen mod 8.
void clash_model(ByteWriter& w, const char* name, int32_t unit, double scale, double zero) {
    const std::string file = std::string(name) + ".nwd";
    const std::string path = "D:\\modeller\\" + file;
    w.str(file);
    w.str(path);
    w.str("cache");
    guid_bytes(w, 0x60);
    w.str(std::string(name) + ".nwc");
    w.str("inlasare");
    w.i32(0);
    w.i32(2);
    // En vridning på 90 grader kring z, gånger skalan, radvis.
    for (const double v : {0.0, scale, 0.0, -scale, 0.0, 0.0, 0.0, 0.0, scale}) w.f64(v);
    for (const double v : {10.0, 20.0, 0.0}) w.f64(v);
    w.i32(unit);
    w.i32(1);
    w.f64(zero);
    for (const double v : {0.0, 0.0, 1.0, -1.0, 0.0, 0.0}) w.f64(v);
    for (const int32_t v : {0, -1, -1, -1, 0, 0, 1}) w.i32(v);
    w.str(file);
    w.str(path);
    w.i64a(1700000000);
    w.i64a(4096);
    w.f64(0);
    for (const double v : {-1.0, 0.0, 1.0 / 4503599627370496.0}) w.f64(v);
    guid_bytes(w, 0x70);
}

Bytes clash(ClashSpec& spec) {
    ByteWriter w;
    w.i32(2);
    clash_head(w, 53, "Tester", 0x10);
    w.i32(2);
    clash_head(w, 50, "A mot B", 0x20);
    for (const int32_t v : {0, -1, 0, 0, -1, 0, 0, 1, 1}) w.i32(v);
    for (const double v : {0.25, 0.5, 0.0, 0.0, 0.0}) w.f64(v);
    w.i32(1);
    w.i32(0);
    clash_head(w, 50, "C", 0x30);
    w.i32(0);
    spec.test_marker_at = w.data.size();
    w.i32(spec.test_marker);
    for (const int32_t v : {0, 0, -1, 0, 0, 1, 1}) w.i32(v);
    for (const double v : {0.125, 1.0, 0.0, 0.0, 0.0}) w.f64(v);
    w.i32(1);
    w.i32(0);
    clash_head(w, spec.second_root, "Status", 0x40);
    w.i32(0);
    w.i32(2);
    w.str("regel_ett");
    w.i32(spec.rule_flag);
    w.str("RegelTvaPlugin");
    w.i32(1);
    w.i32(2);
    clash_model(w, "Hus", 2, 0.001 / 0.3048, spec.model_zero);
    clash_model(w, "Bro_7", 0, 1 / 0.3048, 0);
    w.pad(8);
    return w.data;
}

int test_clash() {
    int fails = 0;
    std::string detail;
    ClashElement ce;
    ClashSpec spec;
    const Bytes b = clash(spec);
    CHECK(parse_clash(b.data(), b.size(), ce, &detail) == ErrorCode::Ok);
    CHECK(detail.empty());
    CHECK(ce.roots.size() == 2 && ce.tests.size() == 2 && ce.rules.size() == 2 && ce.models.size() == 2);
    if (ce.roots.size() == 2 && ce.tests.size() == 2 && ce.rules.size() == 2 && ce.models.size() == 2) {
        CHECK(ce.roots[0].kind == 53 && ce.roots[0].name == "Tester" && guid_is(ce.roots[0].guid, 0x10));
        CHECK(ce.roots[0].test_begin == 0 && ce.roots[0].test_count == 2);
        CHECK(ce.roots[1].kind == 54 && ce.roots[1].test_count == 0 && guid_is(ce.roots[1].guid, 0x40));
        CHECK(ce.tests[0].name == "A mot B" && guid_is(ce.tests[0].guid, 0x20));
        CHECK(ce.tests[0].values[0] == 0.25 && ce.tests[0].values[1] == 0.5 && ce.tests[0].zero[2] == 0);
        CHECK(ce.tests[1].name == "C" && ce.tests[1].values[0] == 0.125);
        CHECK(ce.rules[0] == "regel_ett" && ce.rules[1] == "RegelTvaPlugin");
        for (const ClashModel& m : ce.models) {
            CHECK(m.prefix == "cache" && guid_is(m.source_guid, 0x60) && m.loader == "inlasare");
            CHECK(m.mode == 2 && m.offset[1] == 20 && m.up[2] == 1 && m.north[0] == -1);
            CHECK(m.file_again == m.file && m.path_again == m.path);
            CHECK(m.time == 1700000000 && m.size == 4096 && m.c[0] == -1 && guid_is(m.guid, 0x70));
            // Skalan i matrisen är enhetskvoten mot fot.
            double metres = 0;
            CHECK(metres_per_unit(m.unit_code, metres) &&
                  std::fabs(m.matrix[1] - metres / 0.3048) < 1e-15 && m.matrix[8] == m.matrix[1]);
        }
        CHECK(ce.models[0].file == "Hus.nwd" && ce.models[0].name == "Hus.nwc" && ce.models[0].unit_code == 2);
        CHECK(ce.models[1].path == "D:\\modeller\\Bro_7.nwd" && ce.models[1].unit_code == 0);
    }

    // Ett annat slag på en rot, ett annat fast tal i ett test eller efter en
    // regel, och en nolla som inte är 0: former som inte är avkodade.
    ClashSpec root;
    root.second_root = 56;
    const Bytes r = clash(root);
    CHECK(parse_clash(r.data(), r.size(), ce, &detail) == ErrorCode::Unsupported);
    CHECK(starts_with(detail, "LcOpClashElement at byte "));
    ClashSpec marker;
    marker.test_marker = 0;
    const Bytes m = clash(marker);
    CHECK(parse_clash(m.data(), m.size(), ce, &detail) == ErrorCode::Unsupported);
    CHECK(starts_with(detail, ("LcOpClashElement at byte " + std::to_string(marker.test_marker_at + 4) +
                               ": ").c_str()));
    ClashSpec rule;
    rule.rule_flag = 0;
    const Bytes ru = clash(rule);
    CHECK(parse_clash(ru.data(), ru.size(), ce, &detail) == ErrorCode::Unsupported);
    ClashSpec zero;
    zero.model_zero = 1;
    const Bytes z = clash(zero);
    CHECK(parse_clash(z.data(), z.size(), ce, &detail) == ErrorCode::Unsupported);
    // Avhuggen, och något efter slutet.
    CHECK(parse_clash(b.data(), b.size() - 16, ce, &detail) == ErrorCode::CorruptPackage);
    Bytes longer = b;
    longer.insert(longer.end(), 8, 1);
    CHECK(parse_clash(longer.data(), longer.size(), ce, &detail) == ErrorCode::CorruptPackage);
    // Ett antal som inte ryms.
    Bytes many = b;
    many[0] = 0xFF;
    many[1] = 0xFF;
    CHECK(parse_clash(many.data(), many.size(), ce, &detail) == ErrorCode::CorruptPackage);
    return fails;
}


// Två ljus; det andra delar det förstas material genom en bakåtpekare.
// `kind` är det andra ljusets tal och `version` dess klass-27-version.
Bytes lights(int32_t kind, int32_t version) {
    ByteWriter w;
    w.i32(2);
    // Ljus 1: nytt 27, nytt 185.
    w.i32(83);
    w.str("Light A");
    w.i32(0);
    guid_bytes(w, 0x50);
    w.i32(0);
    w.obj(27);   // 100
    w.i32(1);
    w.obj(185);  // 101
    w.str(kLightJson);
    w.null_str();
    w.i32(0);
    w.i32(0);
    for (int i = 0; i < 12; ++i) w.f64(i);
    for (const double v : {0.0, 0.0, 0.0, 0.0, 0.6, 0.8}) w.f64(v);
    // Ljus 2: nytt 27, bakåt till materialet.
    w.i32(kind);
    w.str("Light B");
    w.i32(0);
    guid_bytes(w, 0x60);
    w.i32(0);
    w.obj(27);   // 102
    w.i32(version);
    w.ref(101);
    w.i32(0);
    for (int i = 0; i < 12; ++i) w.f64(-i);
    for (const double v : {0.0, 0.0, 0.0, 1.0, 0.0, 0.0}) w.f64(v);
    w.pad(8);
    return w.data;
}

int test_lights() {
    int fails = 0;
    std::string detail;
    LightsElement l;
    const Bytes b = lights(83, 1);
    CHECK(parse_lights(b.data(), b.size(), l, &detail) == ErrorCode::Ok);
    if (l.entries.size() == 2 && l.lights.size() == 2 && l.materials.size() == 1) {
        CHECK(l.entries[0].name == "Light A" && guid_is(l.entries[0].guid, 0x50));
        CHECK(l.entries[1].name == "Light B" && guid_is(l.entries[1].guid, 0x60));
        CHECK(l.entries[0].light == 0 && l.entries[1].light == 1);
        CHECK(l.lights[0].material == 0 && l.lights[1].material == 0);
        CHECK(l.lights[0].values[11] == 11 && l.lights[1].values[11] == -11);
        CHECK(l.entries[0].direction[2] == 0.8 && l.entries[1].direction[0] == 1);
        CHECK(l.materials[0].json == kLightJson);
    } else {
        CHECK(!"two lights and one material");
    }
    const Bytes kind = lights(84, 1);
    CHECK(parse_lights(kind.data(), kind.size(), l, &detail) == ErrorCode::Unsupported);
    CHECK(starts_with(detail, "LcOpLightsElement at byte "));
    const Bytes version = lights(83, 2);
    CHECK(parse_lights(version.data(), version.size(), l, &detail) == ErrorCode::CorruptObject);
    CHECK(parse_lights(b.data(), b.size() - 16, l, &detail) == ErrorCode::CorruptObject);
    // Ett antal som inte ryms.
    Bytes many = b;
    many[0] = 0x40;
    CHECK(parse_lights(many.data(), many.size(), l, &detail) == ErrorCode::CorruptObject);
    return fails;
}

// ---------------------------------------------------------------------------
// Exempelfilerna.

// Paket som andra tolkar läser.
bool read_elsewhere(std::string_view leaf) {
    for (const char* name : {"LcOpNwdGeometry", "LcOaPartitionProps", "LcOpNwdLogicalHierarchy",
                             "LcOpNwdFragments", "LcOpGuidStore", "LcOpNwdPartition",
                             "LcOpNwdSpatialHierarchy", "LcOpDBCache"})
        if (leaf == name) return true;
    return false;
}

// Det som tolkas ur en exempelfil.
struct Sample {
    std::string prefix;   // paketnamnens prefix, utan bakstreck
    std::string stats;
    SceneProps scene;
    FileInfo info;
    XRefTable xref;
    ViewElement view;
    bool has_lights = false, has_background = false;
    LightsElement lights;
    BackgroundElement background;
    Partition partition;
    SharedNodes shared;
    SavedViews saved;
    Bytes serial, compress;
    std::vector<std::string> undecoded;   // paket utan tolk
};

bool unpack(const MappedFile& file, const DbCacheEntry& e, Bytes& out) {
    StreamSpan span;
    return last_stream(e, file.size(), span) == ErrorCode::Ok &&
           inflate_span(file.data(), file.size(), span, kDefaultMaxStreamBytes, out) ==
               ErrorCode::Ok;
}

// Tolkar varje litet paket i filen. Varje paket med en tolk ska tolkas till
// slut; de övriga hamnar i `undecoded`.
int load(const char* path, Sample& s) {
    int fails = 0;
    MappedFile file;
    Container c;
    std::string detail;
    if (file.open(path) != ErrorCode::Ok ||
        read_container(file.data(), file.size(), c, &detail) != ErrorCode::Ok) {
        std::printf("  FAIL — %s: öppnas inte\n", path);
        return 1;
    }
    for (const DbCacheEntry& e : c.entries) {
        const std::string_view leaf = e.leaf();
        if (leaf == "LcOpNwdPartition") {
            Bytes d;
            CHECK(unpack(file, e, d) &&
                  parse_partition_stream(d.data(), d.size(), s.partition, &detail) == ErrorCode::Ok);
        }
        if (read_elsewhere(leaf)) continue;
        // Prefixet: det som står före bakstrecket, för paket som har ett.
        if (e.name.size() > leaf.size()) {
            const std::string p = e.name.substr(0, e.name.size() - leaf.size() - 1);
            if (s.prefix.empty()) s.prefix = p;
            CHECK(p == s.prefix);
        }
        if (e.flags == 3) {
            CHECK(e.length == 40);
            Bytes raw(file.data() + e.offset, file.data() + e.offset + e.length);
            if (leaf == "LcOpNwdSerial") s.serial = raw;
            else if (leaf == "LcOpNwdGeometryCompress") s.compress = raw;
            else s.undecoded.emplace_back(leaf);
            continue;
        }
        Bytes d;
        if (!unpack(file, e, d)) {
            std::printf("  FAIL — %s: %.*s packas inte upp\n", path, static_cast<int>(leaf.size()),
                        leaf.data());
            ++fails;
            continue;
        }
        ErrorCode r = ErrorCode::Ok;
        if (leaf == "LcOpCommonSchemas" || leaf == "LcOpZoneElement" ||
            leaf == "LcOpSelectionSetsElement" || leaf == "LcOpPropertyOverrideElement" ||
            leaf == "LcOdpDBDatabaseLinksElement") {
            const std::string what(leaf);
            r = parse_empty_pair(d.data(), d.size(), what.c_str(), &detail);
        } else if (leaf == "LcOpNwdSharedNodes") {
            r = parse_shared_nodes(d.data(), d.size(), s.shared, &detail);
        } else if (leaf == "LcOpSavedViewsElement") {
            r = parse_saved_views(d.data(), d.size(), s.saved, &detail);
        } else if (leaf == "LcOpHomeViewElement") {
            ViewElement v;
            r = parse_view_element(d.data(), d.size(), v, &detail, "LcOpHomeViewElement");
        } else if (leaf == "LcOpToolElement") {
            int32_t tool;
            r = parse_tool(d.data(), d.size(), tool, &detail);
        } else if (leaf == "LcOpNextCommentIdElement") {
            NextCommentId n;
            r = parse_next_comment_id(d.data(), d.size(), n, &detail);
        } else if (leaf == "LcOpShadOverridesElement") {
            std::vector<ShadOverride> o;
            r = parse_shad_overrides(d.data(), d.size(), o, &detail);
        } else if (leaf == "LcOpTextureSpaceElement") {
            std::vector<TextureSpace> t;
            r = parse_texture_space(d.data(), d.size(), t, &detail);
        } else if (leaf == "LcOpGridElement") {
            GridElement g;
            r = parse_grid(d.data(), d.size(), g, &detail);
        } else if (leaf == "LcReMaterialElement") {
            ReMaterials m;
            r = parse_re_materials(d.data(), d.size(), m, &detail);
        } else if (leaf == "LcOpNwdStats") {
            bool non_utf8 = true;
            r = parse_stats(d.data(), d.size(), s.stats, non_utf8, &detail);
            CHECK(!non_utf8);
        } else if (leaf == "LcOpOdyScenePropsChunk") {
            r = parse_scene_props(d.data(), d.size(), s.scene, &detail);
        } else if (leaf == "LcOpOdyFileInfoChunk") {
            r = parse_file_info(d.data(), d.size(), s.info, &detail);
        } else if (leaf == "LcOpXRefTable") {
            r = parse_xref_table(d.data(), d.size(), s.xref, &detail);
        } else if (leaf == "LcOpCurrentViewElement") {
            r = parse_view_element(d.data(), d.size(), s.view, &detail);
        } else if (leaf == "LcOpHeadlightElement") {
            HeadlightElement h;
            r = parse_headlight(d.data(), d.size(), h, &detail);
        } else if (leaf == "LcOpSpeedElement") {
            double v;
            r = parse_speed(d.data(), d.size(), v, &detail);
        } else if (leaf == "LcOpCullingElement") {
            CullingElement cu;
            r = parse_culling(d.data(), d.size(), cu, &detail);
        } else if (leaf == "LcOpBackgroundElement") {
            r = parse_background(d.data(), d.size(), s.background, &detail);
            s.has_background = true;
        } else if (leaf == "LcOpLightsElement") {
            r = parse_lights(d.data(), d.size(), s.lights, &detail);
            s.has_lights = true;
        } else {
            s.undecoded.emplace_back(leaf);
        }
        if (r != ErrorCode::Ok) {
            std::printf("  FAIL — %s: %s %s\n", path, error_message(r), detail.c_str());
            ++fails;
        }
    }
    return fails;
}

// GUID enligt RFC 4122: varianten är 10, versionen 4 (slump) eller 5 (namn).
bool rfc_guid(const Guid16& g) {
    const int version = g[7] >> 4;   // tredje fältet, little-endian: hög byte sist
    return (g[8] & 0xC0) == 0x80 && (version == 4 || version == 5);
}

std::string lower_guid(const Guid16& g) {
    char text[37];
    guid_text(g, text);
    std::string s(text);
    for (char& ch : s)
        if (ch >= 'A' && ch <= 'F') ch = static_cast<char>(ch - 'A' + 'a');
    return s;
}

std::string base_name(const char* path) {
    const std::string p(path);
    const size_t slash = p.find_last_of("/\\");
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

// Blickriktningen R(q) · (0, 0, −1).
void forward(const double q[4], double f[3]) {
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    f[0] = -2 * (x * z + y * w);
    f[1] = -2 * (y * z - x * w);
    f[2] = -(1 - 2 * (x * x + y * y));
}

// Världslådan kring alla fragment.
bool world_box(const char* path, double mn[3], double mx[3]) {
    HNwd::Document doc;
    if (doc.open(path) != HNwd::Status::Ok || doc.load_fragments() != HNwd::Status::Ok) return false;
    for (int k = 0; k < 3; ++k) {
        mn[k] = HUGE_VAL;
        mx[k] = -HUGE_VAL;
    }
    for (size_t i = 0; i < doc.fragment_count(); ++i) {
        HNwd::BoxD b;
        if (!doc.fragment_world_box(i, b)) return false;
        for (int k = 0; k < 3; ++k) {
            mn[k] = std::fmin(mn[k], b.min[k]);
            mx[k] = std::fmax(mx[k], b.max[k]);
        }
    }
    return true;
}

// Djupet längs blickriktningen för varje vertex i modellen: minsta och största.
bool vertex_depths(const char* path, const double p[3], const double f[3], double& lo, double& hi) {
    HNwd::Document doc;
    if (doc.open(path) != HNwd::Status::Ok || doc.load_fragments() != HNwd::Status::Ok) return false;
    lo = HUGE_VAL;
    hi = -HUGE_VAL;
    HNwd::Mesh m;
    for (size_t i = 0; i < doc.fragment_count(); ++i) {
        HNwd::Fragment fr;
        if (!doc.fragment(i, fr) || doc.fragment_mesh(i, m) != HNwd::Status::Ok) return false;
        for (size_t k = 0; k + 2 < m.positions.size(); k += 3) {
            double w[3];
            HNwd::world_point(fr.pose, &m.positions[k], w);
            const double t = (w[0] - p[0]) * f[0] + (w[1] - p[1]) * f[1] + (w[2] - p[2]) * f[2];
            lo = std::fmin(lo, t);
            hi = std::fmax(hi, t);
        }
    }
    return true;
}

// Snittplanen i exemplen: inga plan påslagna, och samma värden i alla tio.
bool sample_clip(const ViewClip& c) {
    const int32_t head[6] = {2, 0, 0, 0, 2, 2};
    const int32_t modes[6] = {6, 7, 3, 2, 4, 5};
    bool ok = std::equal(head, head + 6, c.head) && c.head_values[0] == 1 &&
              c.head_values[1] == 0 && c.head_values[2] == 0;
    for (int k = 0; k < 6; ++k) {
        const ClipPlane& p = c.planes[k];
        ok = ok && p.enabled == 0 && p.mode == modes[k];
        for (int j = 0; j < 3; ++j)
            ok = ok && p.point[j] == 0 && p.normal[j] == (j == 1 ? 1 : 0) &&
                 p.tangent[j] == (j == 2 ? 1 : 0);
    }
    for (int j = 0; j < 6; ++j)
        ok = ok && c.values_a[j] == (j < 3 ? 1 : 0) && c.values_b[j] == (j < 3 ? 1 : 0);
    return ok && c.flags_a[0] == 0 && c.flags_a[1] == 0 && c.flags_b[0] == 0 &&
           c.flags_b[1] == 0 && c.rotation[0] == 0 && c.rotation[1] == 0 &&
           c.rotation[2] == 0 && c.rotation[3] == 1;
}

// Det som gäller i varje fil.
int check_common(const char* path, const Sample& s) {
    int fails = 0;
    // Bara GeometryCompress och Serial saknar tolk. SharedNodes och de sparade
    // vyerna är tomma.
    CHECK(s.undecoded.empty());
    CHECK(s.shared.entries.empty() && s.saved.items.empty());
    CHECK(s.serial.size() == 40 && s.compress.size() == 40);
    // De 16 första byten är lika i de två.
    CHECK(s.serial.size() == 40 && s.compress.size() == 40 &&
          std::memcmp(s.serial.data(), s.compress.data(), 16) == 0);

    // Sökvägen slutar på filens eget namn och är samma som i LcOpXRefTable.
    const std::string name = base_name(path);
    CHECK(s.scene.path.size() > name.size() &&
          s.scene.path.compare(s.scene.path.size() - name.size(), name.size(), name) == 0 &&
          s.scene.path[s.scene.path.size() - name.size() - 1] == '\\');
    CHECK(s.scene.path == s.xref.saved_filename && s.xref.version == 1);

    // FileInfo: prefixet är paketnamnens, och det som upprepas är lika.
    CHECK(s.info.prefix == s.prefix && s.info.prefix_again == s.prefix);
    CHECK(s.info.scene_guid_again == s.info.scene_guid);
    CHECK(s.info.tag == "G");
    CHECK(rfc_guid(s.info.file_guid) && rfc_guid(s.info.scene_guid) && rfc_guid(s.scene.guid) &&
          rfc_guid(s.info.guid_c));

    // Vyn: enhetskvaternion, och snittplanen lika i alla tio.
    const double* q = s.view.rotation;
    CHECK(std::fabs(std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]) - 1) < 1e-9);
    CHECK(s.view.angle == 0.785398);
    CHECK(s.view.zero[0] == 0 && s.view.zero[1] == 0);
    CHECK(sample_clip(s.view.clip));
    return fails;
}

int test_samples() {
    int fails = 0;
    std::vector<Sample> nwc(std::size(testdata::kSampleNwc));
    bool all = true;
    for (size_t i = 0; i < nwc.size(); ++i) {
        const char* path = testdata::kSampleNwc[i];
        if (!testdata::exists(path)) {
            std::printf("  SKIP — %s saknas\n", path);
            ++g_skipped;
            all = false;
            continue;
        }
        Sample& s = nwc[i];
        const int f = load(path, s);
        fails += f;
        if (f) {
            all = false;
            continue;
        }
        fails += check_common(path, s);
        // .nwc: prefixet "cache", namnet är filens, GUID:erna som upprepas.
        CHECK(s.prefix == "cache" && s.info.name == base_name(path));
        CHECK(s.info.scene_guid == s.scene.guid);
        CHECK(s.info.guid_c == s.info.file_guid && s.info.guid_d == Guid16{});
        CHECK(!s.has_lights && !s.has_background);
        CHECK(!s.stats.empty());

        // Vyn i form 9: identitetsrotationen, och läget plus avståndet längs
        // blickriktningen är mitten av fragmentens världslåda.
        const ViewElement& v = s.view;
        CHECK(v.form == 9 && v.tail_ints.size() == 2 && v.tail_ints[0] == 0 && v.tail_ints[1] == 0);
        CHECK(v.u == 1 && v.near_depth == 1 && v.far_depth == 10);
        double mn[3], mx[3], f3[3];
        if (world_box(path, mn, mx)) {
            forward(v.rotation, f3);
            const double diag = std::sqrt((mx[0] - mn[0]) * (mx[0] - mn[0]) +
                                          (mx[1] - mn[1]) * (mx[1] - mn[1]) +
                                          (mx[2] - mn[2]) * (mx[2] - mn[2]));
            for (int k = 0; k < 3; ++k) {
                const double target = v.position[k] + v.distance * f3[k];
                CHECK(std::fabs(target - (mn[k] + mx[k]) / 2) < 1e-9 * diag);
            }
        } else {
            CHECK(!"world box");
        }
        // Serial är samma i alla filer.
        CHECK(s.serial == nwc[0].serial);
    }

    const char* path = testdata::kSampleNwd;
    if (!testdata::exists(path)) {
        std::printf("  SKIP — %s saknas\n", path);
        ++g_skipped;
        return fails;
    }
    Sample d;
    const int f = load(path, d);
    if (f) return fails + f;
    fails += check_common(path, d);
    // .nwd: prefixet och namnet är den första modellens; GUID:erna är andra.
    CHECK(d.info.name == d.prefix);
    CHECK(d.info.scene_guid != d.scene.guid && d.info.guid_c != d.info.file_guid);
    CHECK(rfc_guid(d.info.guid_d));

    const ViewElement& v = d.view;
    CHECK(v.form == 843 && v.tail_ints.size() == 6 && v.avatar == "construction_worker");
    if (all) {
        CHECK(d.serial == nwc[0].serial);
        // Prefixet är den första modellens namn.
        CHECK(d.prefix == nwc[0].partition.name);
        // Avståndet är detsamma som i den första modellens vy.
        CHECK(std::fabs(v.distance - nwc[0].view.distance) < 1e-6);
        // Loggen är modellernas loggar efter varandra, var och en inledd av
        // "\n" <modellens namn> "\n\n".
        std::string joined;
        for (const Sample& s : nwc) joined += "\n" + s.partition.name + "\n\n" + s.stats;
        CHECK(d.stats == joined);
    }
    // Varje vertex i modellen ligger mellan near och far längs blickriktningen.
    double f3[3], lo = 0, hi = 0;
    forward(v.rotation, f3);
    if (vertex_depths(path, v.position, f3, lo, hi)) {
        CHECK(v.near_depth > 0 && v.near_depth < lo && hi < v.far_depth);
    } else {
        CHECK(!"vertex depths");
    }

    // Bakgrunden: sju färger i hela 255-delar.
    CHECK(d.has_background);
    for (const auto& c : d.background.colours)
        for (const double x : c) {
            CHECK(x >= 0 && x <= 1);
            CHECK(std::fabs(x * 255 - std::round(x * 255)) < 1e-9);
        }

    // Ljusen: GUID:en är ljusets VersionGUID, de tolv talen är ljusen i
    // filens egen partition, och riktningen är de tre sista.
    CHECK(d.has_lights && d.lights.entries.size() == 2 && d.lights.lights.size() == 2);
    CHECK(d.partition.lights.size() == d.lights.lights.size());
    for (size_t i = 0; i < d.lights.entries.size() && i < d.partition.lights.size(); ++i) {
        const LightsElement::Entry& e = d.lights.entries[i];
        CHECK(e.kind == 83 && e.light == i);
        const LightBody& b = d.lights.lights[e.light];
        CHECK(b.material != kNoMaterial && b.material < d.lights.materials.size());
        if (b.material < d.lights.materials.size()) {
            const std::string& json = d.lights.materials[b.material].json;
            CHECK(json.find("\"VersionGUID\":{\"values\":[\"" + lower_guid(e.guid) + "\"]}") !=
                  std::string::npos);
            // Talen 3–5 är ljusets "specularColor", med JSON-textens siffror.
            char spec[160];
            std::snprintf(spec, sizeof spec,
                          "\"specularColor\":{\"values\":[{\"r\":%.17g,\"g\":%.17g,\"b\":%.17g,",
                          b.values[3], b.values[4], b.values[5]);
            CHECK(json.find(spec) != std::string::npos);
        }
        for (int k = 0; k < 12; ++k) CHECK(b.values[k] == d.partition.lights[i].values[k]);
        for (int k = 0; k < 3; ++k) {
            CHECK(e.origin[k] == 0);
            CHECK(e.direction[k] == b.values[9 + k]);
            CHECK(b.values[3 + k] == b.values[6 + k]);
        }
        const double* r = e.direction;
        CHECK(std::fabs(r[0] * r[0] + r[1] * r[1] + r[2] * r[2] - 1) < 1e-9);
    }
    return fails;
}

// Ett antal ur filen som strömmen ryms för, följt av byte som inte går att
// tolka (0x7F): tolkningen faller på den första posten, och ingen behållare
// har dessförinnan fått plats efter antalet. Den största allokeringen är
// därför liten mot strömmen.
int test_counts_from_file() {
    int fails = 0;
    const int32_t n = 10000;
    auto junk = [](ByteWriter& w, size_t bytes) { w.data.insert(w.data.end(), bytes, 0x7F); };
    auto twelve_zeros = [](ByteWriter& w) {
        for (int k = 0; k < 12; ++k) w.f64(0);
    };
    struct Case {
        const char* what;
        Bytes stream;
        ErrorCode (*parse)(const Bytes&);
    };
    std::vector<Case> cases;
    {
        ByteWriter w;
        w.i32(n);
        junk(w, size_t(112) * n);
        cases.push_back({"grid systems", w.data, [](const Bytes& b) {
                             GridElement g;
                             return parse_grid(b.data(), b.size(), g);
                         }});
    }
    {
        ByteWriter w;
        w.i32(1);
        w.str("");
        twelve_zeros(w);
        w.i32(n);
        junk(w, size_t(64) * n);
        cases.push_back({"grid lines", w.data, [](const Bytes& b) {
                             GridElement g;
                             return parse_grid(b.data(), b.size(), g);
                         }});
    }
    {
        ByteWriter w;
        w.i32(1);
        w.str("");
        twelve_zeros(w);
        w.i32(0);
        w.i32(n);
        junk(w, size_t(12) * n);
        cases.push_back({"grid levels", w.data, [](const Bytes& b) {
                             GridElement g;
                             return parse_grid(b.data(), b.size(), g);
                         }});
    }
    {
        ByteWriter w;
        w.i32(n);
        junk(w, size_t(36) * n);
        cases.push_back({"clash roots", w.data, [](const Bytes& b) {
                             ClashElement c;
                             return parse_clash(b.data(), b.size(), c);
                         }});
    }
    {
        ByteWriter w;
        w.i32(0);
        w.i32(n);
        junk(w, size_t(8) * n);
        cases.push_back({"clash rules", w.data, [](const Bytes& b) {
                             ClashElement c;
                             return parse_clash(b.data(), b.size(), c);
                         }});
    }
    {
        ByteWriter w;
        w.i32(0);
        w.i32(0);
        w.i32(n);
        junk(w, size_t(304) * n);
        cases.push_back({"clash models", w.data, [](const Bytes& b) {
                             ClashElement c;
                             return parse_clash(b.data(), b.size(), c);
                         }});
    }
    {
        ByteWriter w;
        w.i32(n);
        junk(w, size_t(36) * n);
        cases.push_back({"scene materials", w.data, [](const Bytes& b) {
                             ReMaterials m;
                             return parse_re_materials(b.data(), b.size(), m);
                         }});
    }
    {
        ByteWriter w;
        w.i32(n);
        junk(w, size_t(88) * n);
        cases.push_back({"lights", w.data, [](const Bytes& b) {
                             LightsElement l;
                             return parse_lights(b.data(), b.size(), l);
                         }});
    }
    {
        ByteWriter w;
        w.i32(1);
        w.i32(0);
        w.str("V");
        w.i32(0);
        guid_bytes(w, 0);
        w.i32(0);
        write_view(w, 9, 0);
        w.i32(n);
        junk(w, size_t(48) * n);
        cases.push_back({"redlines", w.data, [](const Bytes& b) {
                             SavedViews sv;
                             return parse_saved_views(b.data(), b.size(), sv);
                         }});
    }
    for (const Case& c : cases) {
        alloc_probe::Scope probe;
        const ErrorCode e = c.parse(c.stream);
        const size_t largest = probe.largest();
        if (e == ErrorCode::Ok || largest >= c.stream.size() / 2)
            std::printf("  %s: %s, largest allocation %zu for a %zu-byte stream\n", c.what,
                        error_message(e), largest, c.stream.size());
        CHECK(e != ErrorCode::Ok);
        CHECK(largest < c.stream.size() / 2);
    }
    return fails;
}

} // namespace

int test_elements() {
    int fails = 0;
    fails += test_empty_pair();
    fails += test_stats();
    fails += test_scene_props();
    fails += test_file_info();
    fails += test_xref();
    fails += test_view();
    fails += test_view_clip();
    fails += test_small_elements();
    fails += test_lights();
    fails += test_saved_views();
    fails += test_overrides_and_texture_space();
    fails += test_shared_nodes();
    fails += test_grid();
    fails += test_re_materials();
    fails += test_clash();
    fails += test_counts_from_file();
    fails += test_samples();
    std::printf("test_elements: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
