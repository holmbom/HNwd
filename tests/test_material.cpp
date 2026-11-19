// Klass 185: materialets JSON med sina filer, och JSON-läsaren.

#include <cstdio>
#include <string>
#include <vector>

#include "alloc_probe.h"
#include "byte_reader.h"
#include "material.h"
#include "object_reader.h"
#include "test_support.h"

using namespace HNwd::detail;
using testdata::Bytes;
using testdata::ByteWriter;

namespace {

// Ett material med grundtillgång och inkopplad bild, i formen filerna har:
// sektionerna ligger i tillgångens "properties".
const char kFull[] =
    R"({"version":2,"userassets":["0"],"materials":{)"
    R"("0":{"tag":"Generic","definition":"Generic","InstanceIdentifier":"x","keywords":["a"],)"
    R"("categories":[],"unknown":[1,2,{"x":null}],"properties":{"strings":{},)"
    R"("uris":{"thumbnail":{"values":["t.png"]}},"scalars":{"generic_glossiness":{"units":"","values":[0.2]}},)"
    R"("colors":{"generic_diffuse":{"IsConnectionEnabled":true,"connections":["1"],)"
    R"("values":[{"r":0.25,"g":0.5,"b":0.75,"a":1}]}},"textures":{},"references":{}}},)"
    R"("1":{"tag":"UnifiedBitmap","definition":"UnifiedBitmap","properties":{)"
    R"("uris":{"thumbnail":{"values":[]},"unifiedbitmap_Bitmap":{"values":["bilder/tak.jpg"]}},)"
    R"("scalars":{"texture_UScale":{"units":"","values":[2.5]},"texture_VScale":{"values":[0.5]},)"
    R"("texture_UOffset":{"values":[-1]},"texture_VOffset":{"values":[1e-1]},)"
    R"("texture_WAngle":{"values":[90]},)"
    R"("texture_RealWorldScaleX":{"units":"mm","values":[1200]},)"
    R"("texture_RealWorldScaleY":{"units":"in","values":[48]}},)"
    R"("booleans":{"texture_URepeat":{"values":[false]},"texture_VRepeat":{"values":[true]}}}}}})";

// En tillgång med `props` som innehållet i "properties".
std::string asset(const std::string& definition, const std::string& props) {
    return R"({"tag":")" + definition + R"(","definition":")" + definition +
           R"(","properties":{)" + props + "}}";
}

// Hela texten: `users` är listan i "userassets", `assets` innehållet i "materials".
std::string material(const std::string& users, const std::string& assets) {
    return R"({"version":2,"userassets":[)" + users + R"(],"materials":{)" + assets + "}}";
}

// Grundtillgången "0" med generic_diffuse = `diffuse`, och tillgångarna i `rest`.
std::string generic(const std::string& diffuse, const std::string& rest = "") {
    return material(R"("0")", R"("0":)" + asset("Generic", R"("colors":{"generic_diffuse":)" +
                                                               diffuse + "}") +
                                  (rest.empty() ? "" : "," + rest));
}

// Grundtillgången kopplad till bilden "1" med `props` i dess "properties".
std::string bitmap(const std::string& props) {
    return generic(R"({"connections":["1"]})", R"("1":)" + asset("UnifiedBitmap", props));
}

std::vector<MaterialFile> files_with(const char* name) {
    std::vector<MaterialFile> files(2);
    files[0].name = "annan.png";
    files[1].name = name;
    files[1].embedded = true;
    files[1].type = "jpg";
    return files;
}

struct Parsed {
    bool ok;
    MaterialTexture t;
    size_t at;
    std::string rule;
};

Parsed run(const std::string& json, const std::vector<MaterialFile>& files = {}) {
    Parsed p;
    p.t.u_scale = 99;   // ska ersättas
    p.ok = parse_material_json(json, files, p.t, p.at, p.rule, kDefaultMaxMaterialJsonBytes);
    return p;
}

bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// Texten ska ge ett fel vid tecknet `at` med regeln `rule`.
bool fails_at(const std::string& json, size_t at, const char* rule) {
    const Parsed p = run(json);
    const bool good = !p.ok && p.at == at && contains(p.rule, rule) && p.t.u_scale == 1 &&
                      p.t.bitmap.empty() && !p.t.has_color;
    if (!good)
        std::printf("  json '%s': ok %d, at %zu, rule '%s'\n", json.c_str(), p.ok ? 1 : 0, p.at,
                    p.rule.c_str());
    return good;
}

// En klass-185-kropp: json, en sträng, och filerna.
struct File {
    std::string name;
    int32_t flag = 0;
    std::string kind = "navis";
    std::string type = "jpg";
    int64_t len = -2;   // -2: byten nedan
    Bytes bytes;
};

Bytes body(const std::string& json, const std::vector<File>& files, bool pad_bad = false) {
    ByteWriter w;
    w.str(json);
    w.str("");
    w.i32(static_cast<int32_t>(files.size()));
    for (const File& f : files) {
        w.str(f.name);
        w.i32(f.flag);
        w.str(f.kind);
        if (f.flag == 1) {
            w.str(f.type);
            w.i64a(f.len == -2 ? static_cast<int64_t>(f.bytes.size()) : f.len);
            w.raw(f.bytes);
            if (pad_bad && w.data.size() % 4 != 0) w.u8(1);
            w.pad(4);
        }
    }
    w.pad(8);
    return w.data;
}

struct Read {
    bool ok;
    MaterialData data;
    std::string detail;
};

Read read(const Bytes& b) {
    Read r;
    ObjectReader reader(b.data(), b.size(), "test");
    r.ok = read_class185(reader, r.data, kDefaultMaxMaterialJsonBytes) && reader.end();
    r.detail = reader.detail();
    return r;
}

Bytes fake_image(size_t n) {
    Bytes b(n);
    for (size_t i = 0; i < n; ++i) b[i] = static_cast<uint8_t>(0xF0 + i * 7);
    return b;
}

} // namespace

int test_material() {
    int fails = 0;

    // Hela materialet: färg, bildens namn, skalor, enheter och upprepning.
    {
        const Parsed p = run(kFull, files_with("bilder/tak.jpg"));
        CHECK(p.ok);
        CHECK(p.t.has_color);
        CHECK(p.t.color[0] == 0.25f && p.t.color[1] == 0.5f && p.t.color[2] == 0.75f &&
              p.t.color[3] == 1.0f);
        CHECK(p.t.bitmap == "bilder/tak.jpg");
        CHECK(p.t.file == 1);
        CHECK(p.t.u_scale == 2.5 && p.t.v_scale == 0.5);
        CHECK(p.t.u_offset == -1 && p.t.v_offset == 0.1);
        CHECK(p.t.w_angle == 90);
        CHECK(p.t.real_scale_x == 1200 && p.t.real_units_x == "mm");
        CHECK(p.t.real_scale_y == 48 && p.t.real_units_y == "in");
        CHECK(!p.t.u_repeat && p.t.v_repeat);

        // Namnet jämförs exakt: en annan skiftläge eller ingen fil gör bilden extern.
        const Parsed q = run(kFull, files_with("bilder/TAK.jpg"));
        CHECK(q.ok && q.t.bitmap == "bilder/tak.jpg" && q.t.file == kNoMaterial);
        const Parsed none = run(kFull);
        CHECK(none.ok && none.t.file == kNoMaterial);
    }

    // Material utan grundtillgång, utan färg eller utan bild är inget fel.
    {
        for (const char* json :
             {"{}", R"({"version":2})", R"({"materials":{}})",
              R"({"userassets":["0"],"materials":{"0":{"definition":"Ceramic"}}})",
              R"({"userassets":[],"materials":{"0":{"definition":"Generic"}}})",
              R"({"userassets":["0"],"materials":{"0":{"definition":"Generic"}}})",
              R"({"userassets":["0"],"materials":{"0":{}}})"}) {
            const Parsed p = run(json);
            CHECK(p.ok);
            CHECK(!p.t.has_color && p.t.bitmap.empty() && p.t.file == kNoMaterial);
            CHECK(p.t.u_scale == 1 && p.t.v_scale == 1 && p.t.u_offset == 0 && p.t.w_angle == 0);
            CHECK(p.t.u_repeat && p.t.v_repeat && p.t.color[0] == 1 && p.t.color[3] == 1);
        }
        // Färg utan "a", och ingen inkoppling.
        const Parsed c = run(generic(R"({"values":[{"r":1,"g":0,"b":0.5}]})"));
        CHECK(c.ok && c.t.has_color && c.t.color[0] == 1 && c.t.color[2] == 0.5f &&
              c.t.color[3] == 1 && c.t.bitmap.empty());
        // Tom "values": färgen står inte.
        const Parsed e = run(generic(R"({"values":[]})"));
        CHECK(e.ok && !e.t.has_color);
        // En inkoppling som inte är en bild.
        const Parsed n = run(generic(R"({"connections":["1"]})", R"("1":)" + asset("Noise", "")));
        CHECK(n.ok && n.t.bitmap.empty());
        // Grundtillgången är den första Generic i userassets; bilden den första UnifiedBitmap.
        const Parsed g = run(material(
            R"("2","0")",
            R"("2":)" + asset("Other", "") + R"(,"0":)" +
                asset("Generic", R"("colors":{"generic_diffuse":{"connections":["3","1"]}})") +
                R"(,"3":)" + asset("Checker", "") + R"(,"1":)" +
                asset("UnifiedBitmap",
                      R"("uris":{"unifiedbitmap_Bitmap":{"values":["x.png"]}},)"
                      R"("scalars":{"texture_UScale":3,"texture_RealWorldScaleX":{"values":[2]}},)"
                      R"("booleans":{"texture_VRepeat":false})")));
        CHECK(g.ok && g.t.bitmap == "x.png" && g.t.u_scale == 3 && g.t.real_scale_x == 2 &&
              g.t.real_units_x.empty() && !g.t.v_repeat && g.t.u_repeat);
        // En bild utan sökväg: inkopplad men utan namn.
        const Parsed b = run(bitmap(""));
        CHECK(b.ok && b.t.bitmap.empty() && b.t.file == kNoMaterial);
        // En tillgång utan "properties" har inga sektioner.
        const Parsed np = run(material(R"("0")", R"("0":{"definition":"Generic"})"));
        CHECK(np.ok && !np.t.has_color);

        // En sektion direkt i tillgången, utanför "properties", läses inte.
        const Parsed outside = run(material(
            R"("0")", R"("0":{"definition":"Generic","properties":{},)"
                      R"("colors":{"generic_diffuse":{"values":[{"r":0,"g":0,"b":0}]}}})"));
        CHECK(outside.ok && !outside.t.has_color);
        const Parsed outside_bmp = run(generic(
            R"({"connections":["1"]})",
            R"("1":{"definition":"UnifiedBitmap","properties":{},)"
            R"("uris":{"unifiedbitmap_Bitmap":{"values":["x.png"]}}})"));
        CHECK(outside_bmp.ok && outside_bmp.t.bitmap.empty());

        // En avstängd koppling följs inte: ingen bild, men färgen står.
        const std::string off = generic(
            R"({"IsConnectionEnabled":false,"connections":["1"],"values":[{"r":1,"g":1,"b":1}]})",
            R"("1":)" + asset("UnifiedBitmap", R"("uris":{"unifiedbitmap_Bitmap":{"values":["x.png"]}})"));
        const Parsed o = run(off, files_with("x.png"));
        CHECK(o.ok && o.t.has_color && o.t.bitmap.empty() && o.t.file == kNoMaterial);
        // Avstängd följs inte ens till en tillgång som saknas.
        CHECK(run(generic(R"({"IsConnectionEnabled":false,"connections":["9"]})")).ok);
        // Påslagen följs.
        const Parsed on = run(generic(
            R"({"IsConnectionEnabled":true,"connections":["1"]})",
            R"("1":)" + asset("UnifiedBitmap", R"("uris":{"unifiedbitmap_Bitmap":{"values":["x.png"]}})")));
        CHECK(on.ok && on.t.bitmap == "x.png");
    }

    // Strängarnas flyktsekvenser blir UTF-8.
    {
        const Parsed p = run(bitmap(
            R"("uris":{"unifiedbitmap_Bitmap":{"values":["aå😀\"\\\/\b\f\n\r\t"]}})"));
        CHECK(p.ok);
        CHECK(p.t.bitmap == "a\xC3\xA5\xF0\x9F\x98\x80\"\\/\b\f\n\r\t");
        // Byte över 0x7F som står som de är.
        const Parsed q = run(bitmap("\"uris\":{\"unifiedbitmap_Bitmap\":{\"values\":[\"\xC3\xA5.png\"]}}"));
        CHECK(q.ok && q.t.bitmap == "\xC3\xA5.png");
        // Tal i alla former, och blanktecken överallt.
        const Parsed r = run(" \t\r\n{ \"a\" : [ -0 , 0.5e+2 , 1E-2 , -12.25 , true , false , null ] } ");
        CHECK(r.ok);
    }

    // JSON som inte går att tolka: felet anger tecknet.
    {
        CHECK(fails_at("", 0, "unexpected end"));
        CHECK(fails_at("   ", 3, "unexpected end"));
        CHECK(fails_at("{", 1, "unexpected end"));
        CHECK(fails_at("{\"a\"", 4, "unexpected end"));
        CHECK(fails_at("{\"a\" 1}", 5, "expected ':'"));
        CHECK(fails_at("{\"a\":1,}", 7, "object key is not a string"));
        CHECK(fails_at("{\"a\":1 \"b\":2}", 7, "expected ',' or '}'"));
        CHECK(fails_at("{a:1}", 1, "object key is not a string"));
        CHECK(fails_at("[1,]", 3, "unexpected character"));
        CHECK(fails_at("[1 2]", 3, "expected ',' or ']'"));
        CHECK(fails_at("{\"a\":1,\"a\":2}", 7, "duplicate key"));
        CHECK(fails_at("{} x", 3, "text follows the value"));
        CHECK(fails_at("{}{}", 2, "text follows the value"));
        CHECK(fails_at("\"abc", 4, "string is not closed"));
        CHECK(fails_at("\"a\tb\"", 2, "control character"));
        CHECK(fails_at("\"\\x\"", 1, "unknown escape"));
        CHECK(fails_at("\"\\u12g4\"", 5, "non-hex digit"));
        CHECK(fails_at("\"\\u12", 3, "unexpected end"));
        CHECK(fails_at("\"\\udc00\"", 1, "lone low surrogate"));
        CHECK(fails_at("\"\\ud83d\"", 1, "without its pair"));
        CHECK(fails_at("\"\\ud83d\\u0041\"", 1, "without its pair"));
        CHECK(fails_at("01", 1, "text follows the value"));
        CHECK(fails_at("-", 1, "number has no digits"));
        CHECK(fails_at("1.", 2, "no digits after '.'"));
        CHECK(fails_at("1e", 2, "no digits in the exponent"));
        CHECK(fails_at("1e+", 3, "no digits in the exponent"));
        CHECK(fails_at("+1", 0, "unexpected character"));
        CHECK(fails_at("1e999", 0, "out of range"));
        CHECK(fails_at("tru", 0, "unexpected character"));
        CHECK(fails_at("nul", 0, "unexpected character"));
        CHECK(fails_at("{\"a\":1} // kommentar", 8, "text follows the value"));
        CHECK(fails_at("\"\xC3\"", 0, "not valid UTF-8"));
        CHECK(fails_at("\"\xED\xA0\x80\"", 0, "not valid UTF-8"));
        // Nästlingen har ett tak, och ett djup under det går.
        std::string deep(65, '[');
        deep += std::string(65, ']');
        CHECK(fails_at(deep, 64, "nesting deeper than the limit"));
        std::string ok_deep(64, '[');
        ok_deep += std::string(64, ']');
        const Parsed d = run(ok_deep);
        CHECK(!d.ok && contains(d.rule, "not an object"));   // tolkas, men är inget objekt
        std::string very_deep(100000, '[');
        CHECK(fails_at(very_deep, 64, "nesting deeper than the limit"));
    }

    // Ett objekt med många nycklar: varje nyckel slås upp och prövas mot de
    // andra utan att tiden växer med kvadraten. En dubblett anges vid sin
    // andra förekomst, den första i texten om flera står.
    {
        std::string json = "{";
        for (int i = 0; i < 30000; ++i) json += (i ? ",\"k" : "\"k") + std::to_string(i) + "\":0";
        const std::string unique = json + ",\"materials\":{}}";
        CHECK(run(unique).ok);
        const size_t second = json.size() + 1;
        CHECK(fails_at(json + ",\"k29999\":1,\"k5\":1}", second, "duplicate key"));
        CHECK(fails_at("{\"b\":1,\"a\":1,\"b\":2,\"a\":2}", 13, "duplicate key"));
    }

    // Texten och antalet värden har tak, givna av anroparen: högst
    // `max` tecken och högst max / 16 värden. Ett fel över dem är
    // LimitExceeded, och regeln namnger fältet i Options.
    {
        const size_t max = size_t(1) << 20;   // 65 536 värden
        std::string many = "[";
        for (int i = 0; i < 70000; ++i) many += i ? ",0" : "0";
        many += "]";
        MaterialTexture t;
        size_t at = 0;
        std::string rule;
        bool limit = false;
        CHECK(!parse_material_json(many, {}, t, at, rule, max, &limit) && limit &&
              contains(rule, "more values than the limit") &&
              contains(rule, "Options::max_material_json_bytes"));
        // Med förvalet, 4 MB, ryms både antalet och en text över 1 MB: felet
        // är då bara att texten inte är ett objekt.
        CHECK(!parse_material_json(many, {}, t, at, rule, kDefaultMaxMaterialJsonBytes, &limit) &&
              !limit && contains(rule, "not an object"));
        const std::string longest = "\"" + std::string(max + 1, 'x') + "\"";
        CHECK(!parse_material_json(longest, {}, t, at, rule, max, &limit) && limit &&
              contains(rule, "longer than the limit") &&
              contains(rule, "Options::max_material_json_bytes"));
        CHECK(!parse_material_json(longest, {}, t, at, rule, kDefaultMaxMaterialJsonBytes, &limit) &&
              !limit && contains(rule, "not an object"));
        const std::string big = "{\"version\":2,\"userassets\":[],\"materials\":{},\"x\":" + longest + "}";
        CHECK(parse_material_json(big, {}, t, at, rule, kDefaultMaxMaterialJsonBytes, &limit));
        CHECK(!parse_material_json("{\"a\":1,\"a\":2}", {}, t, at, rule, max, &limit) && !limit);

        ByteWriter w;
        w.str(many);
        w.str("");
        w.i32(0);
        w.pad(8);
        ObjectReader reader(w.data.data(), w.data.size(), "test");
        MaterialData out;
        CHECK(!read_class185(reader, out, max) && reader.code() == ErrorCode::LimitExceeded &&
              contains(reader.detail(), "more values than the limit") &&
              contains(reader.detail(), "Options::max_material_json_bytes"));
        ObjectReader again(w.data.data(), w.data.size(), "test");
        CHECK(!read_class185(again, out, kDefaultMaxMaterialJsonBytes) &&
              again.code() != ErrorCode::LimitExceeded);   // en lista är inget material
    }

    // Kända nycklar med fel slags värde, och pekare som inte finns.
    {
        CHECK(fails_at("[]", 0, "material json is not an object"));
        CHECK(fails_at("{\"materials\":[]}", 13, "\"materials\" is not an object"));
        CHECK(fails_at("{\"materials\":{},\"userassets\":\"0\"}", 29, "\"userassets\" is not an array"));
        CHECK(fails_at("{\"materials\":{},\"userassets\":[\"0\"]}", 30, "is not in \"materials\""));
        CHECK(fails_at("{\"materials\":{},\"userassets\":[0]}", 30, "reference is not a string"));
        CHECK(fails_at("{\"materials\":{\"0\":1},\"userassets\":[\"0\"]}", 18, "asset is not an object"));
        CHECK(fails_at("{\"materials\":{\"0\":{\"definition\":7}},\"userassets\":[\"0\"]}", 32,
                       "\"definition\" is not a string"));
        // "properties" som inte är ett objekt.
        CHECK(contains(run(material(R"("0")", R"("0":{"definition":"Generic","properties":[]})")).rule,
                       "\"properties\" is not an object"));
        CHECK(contains(run(generic(R"({"connections":["1"]})",
                                   R"("1":{"definition":"UnifiedBitmap","properties":"x"})")).rule,
                       "\"properties\" is not an object"));
        CHECK(contains(run(material(R"("0")", R"("0":)" + asset("Generic", R"("colors":1)"))).rule,
                       "asset section is not an object"));
        CHECK(contains(run(generic("2")).rule, "\"generic_diffuse\" is not an object"));
        CHECK(contains(run(generic(R"({"values":{}})")).rule, "\"values\" is not an array"));
        CHECK(contains(run(generic(R"({"values":[1]})")).rule, "value is not an object"));
        CHECK(contains(run(generic(R"({"values":[{"r":1,"g":1}]})")).rule, "lacks r, g or b"));
        CHECK(contains(run(generic(R"({"values":[{"r":1,"g":1,"b":"1"}]})")).rule,
                       "component is not a number"));
        CHECK(contains(run(generic(R"({"connections":"1"})")).rule, "\"connections\" is not an array"));
        CHECK(contains(run(generic(R"({"connections":["9"]})")).rule, "is not in \"materials\""));
        CHECK(contains(run(generic(R"({"IsConnectionEnabled":1,"connections":["1"]})")).rule,
                       "\"IsConnectionEnabled\" is not true or false"));

        CHECK(run(bitmap(R"("uris":{"unifiedbitmap_Bitmap":{"values":["a"]}})")).ok);
        CHECK(contains(run(bitmap(R"("uris":{"unifiedbitmap_Bitmap":"a"})")).rule, "is not an object"));
        CHECK(contains(run(bitmap(R"("uris":{"unifiedbitmap_Bitmap":{"values":[1]}})")).rule,
                       "file name is not a string"));
        CHECK(contains(run(bitmap(R"("scalars":{"texture_UScale":"2"})")).rule, "scalar is neither"));
        CHECK(contains(run(bitmap(R"("scalars":{"texture_UScale":{"values":[true]}})")).rule,
                       "scalar value is not a number"));
        CHECK(contains(run(bitmap(R"("scalars":{"texture_RealWorldScaleX":{"values":[1],"units":1}})")).rule,
                       "\"units\" is not a string"));
        CHECK(contains(run(bitmap(R"("booleans":{"texture_URepeat":1})")).rule, "boolean is neither"));
        CHECK(contains(run(bitmap(R"("booleans":{"texture_URepeat":{"values":[0]}})")).rule,
                       "boolean value is not true or false"));
        // Okända nycklar i en sektion hoppas över, vad de än innehåller.
        CHECK(run(bitmap(R"("scalars":{"other":"x"},"booleans":{"other":3})")).ok);
    }

    // Kroppen: filer med och utan inbäddade byte, med utfyllnad.
    {
        File a;
        a.name = "bilder/tak.jpg";
        a.flag = 1;
        a.bytes = fake_image(5);   // tre nollor fyller ut
        File b;
        b.name = "extern.png";
        b.flag = 0;
        b.kind = "";
        File c;
        c.name = "c.png";
        c.flag = 1;
        c.type = "png";
        c.bytes = fake_image(8);
        File d;
        d.name = "tom";
        d.flag = 1;
        d.type = "";
        const Bytes data = body(kFull, {b, a, c, d});
        const Read r = read(data);
        CHECK(r.ok);
        CHECK(r.detail.empty());
        CHECK(r.data.json == kFull);
        CHECK(r.data.files.size() == 4);
        if (r.data.files.size() == 4) {
            const MaterialFile& fb = r.data.files[0];
            CHECK(fb.name == "extern.png" && !fb.embedded && fb.bytes.empty() && fb.type.empty());
            const MaterialFile& fa = r.data.files[1];
            CHECK(fa.name == "bilder/tak.jpg" && fa.embedded && fa.kind == "navis" && fa.type == "jpg");
            CHECK(fa.bytes == fake_image(5));
            CHECK(r.data.files[2].bytes == fake_image(8) && r.data.files[2].type == "png");
            CHECK(r.data.files[3].embedded && r.data.files[3].bytes.empty());
        }
        CHECK(r.data.texture.file == 1 && r.data.texture.bitmap == "bilder/tak.jpg");
        CHECK(r.data.texture.u_scale == 2.5);

        // Varje avhuggen längd är ett fel.
        for (size_t len = 0; len + 8 < data.size(); ++len) {
            const Bytes cut(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(len));
            ObjectReader reader(cut.data(), cut.size(), "test");
            MaterialData out;
            if (read_class185(reader, out, kDefaultMaxMaterialJsonBytes)) {
                std::printf("  class 185 cut at %zu was read\n", len);
                ++fails;
            }
        }

        // Utfyllnaden efter byten ska vara noll.
        const Read pad = read(body("{}", {a}, true));
        CHECK(!pad.ok && contains(pad.detail, "pad after the bytes is not zero"));
        // Flaggan är 0 eller 1, och längden får inte vara negativ eller för stor.
        File bad = a;
        bad.flag = 2;
        CHECK(contains(read(body("{}", {bad})).detail, "flag is neither 0 nor 1"));
        bad = a;
        bad.len = -1;
        bad.bytes.clear();
        CHECK(contains(read(body("{}", {bad})).detail, "byte length is negative"));
        bad.len = 1000;
        CHECK(contains(read(body("{}", {bad})).detail, "passes the end"));
        bad.len = INT64_MAX;
        CHECK(contains(read(body("{}", {bad})).detail, "passes the end"));
        // Ett antal filer som inte ryms.
        {
            ByteWriter w;
            w.str("{}");
            w.str("");
            w.i32(1000);
            w.pad(8);
            ObjectReader reader(w.data.data(), w.data.size(), "test");
            MaterialData out;
            CHECK(!read_class185(reader, out, kDefaultMaxMaterialJsonBytes));
        }
        // Genom ett fönster prövas en längd och ett antal bara mot strömmens
        // tak. En inbäddad fil som säger sig vara 1,5 GB i en ström på 20 MB,
        // och 30 miljoner filer i samma ström, allokerar ändå bara efter det
        // som läses.
        for (const bool many_files : {false, true}) {
            ByteWriter w;
            w.str("{}");
            w.str("");
            if (many_files) {
                // Den första filens flagga blir -1 och är fel.
                w.i32(30000000);
                w.raw(Bytes(size_t(20) << 20, 0xFF));
            } else {
                w.i32(1);
                w.str("a.jpg");
                w.i32(1);
                w.str("navis");
                w.str("jpg");
                w.i64a(1500000000);
                w.zeros(size_t(20) << 20);
            }
            const Bytes z = testdata::zlib_store(w.data);
            InflateStream stream;
            stream.open(z.data(), z.size(), kDefaultMaxStreamBytes);
            StreamWindow window(stream, size_t(1) << 20);
            ObjectReader reader(window, "test");
            MaterialData out;
            alloc_probe::Scope probe;
            const bool ok = read_class185(reader, out, kDefaultMaxMaterialJsonBytes);
            const size_t largest = probe.largest();
            CHECK(!ok && reader.code() == ErrorCode::CorruptObject);
            CHECK(largest < (size_t(64) << 20));
            if (largest >= (size_t(64) << 20)) std::printf("  largest allocation %zu\n", largest);
        }

        // Trasig JSON ger läsarens fel med tecknet i texten.
        const Read j = read(body("{\"a\":}", {}));
        CHECK(!j.ok && j.detail.rfind("test at byte ", 0) == 0 &&
              contains(j.detail, "class-185 json at character 5: unexpected character"));
        CHECK(!read(body("", {})).ok);
        // Ett material utan bild går bra.
        const Read plain = read(body("{\"version\":2}", {}));
        CHECK(plain.ok && plain.data.files.empty() && plain.data.texture.bitmap.empty());
    }

    std::printf("test_material: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
