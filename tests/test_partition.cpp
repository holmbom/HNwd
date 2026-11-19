// Klass 32, partitionsnoden: paketet LcOpNwdPartition, och noden i trädet i en
// fil med flera modeller.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "container.h"
#include "fragment_view.h"
#include "fragments.h"
#include "guid_store.h"
#include "hierarchy.h"
#include "mapped_file.h"
#include "partition.h"
#include "partition_props.h"
#include "test_data.h"
#include "test_support.h"

using namespace HNwd::detail;
using testdata::Bytes;
using testdata::ByteWriter;

namespace {

constexpr uint64_t kBig = uint64_t(1) << 23;
constexpr const char* kStreamName = "LcOpNwdPartition at byte ";
constexpr const char* kTreeName = "LcOpNwdLogicalHierarchy at byte ";

// ---------------------------------------------------------------------------
// Syntetiska strömmar.

// Vad en partition ska innehålla.
struct Spec {
    std::string name = "Model.ifc";
    std::string loader = "LcNwcLoaderPlugin:test";
    std::string path = "C:\\src\\Model.ifc";
    int attributes = 1;   // nya objekt av klass 31
    int lights = 0;       // nya objekt av klass 27
    enum Extra { None, BackLight, Share185 } extra = None;
    int32_t unit_code = 2;
    int32_t element = 7;
    int32_t back_target = 0;   // BackLight utan egna ljus: id för ett ljus i en tidigare svans
};

// Var markörerna ligger i strömmen.
struct Offsets {
    size_t version = 0, count = 0, light_version = 0, light_marker = 0, loader_marker = 0,
           unit_marker = 0, path_marker0 = 0, path_marker1 = 0, path_marker2 = 0, end_one = 0,
           element = 0, attributes = 0;
};

void name_pair(ByteWriter& w, const char* display, const char* internal) {
    w.obj(52);
    w.str(display);
    w.str(internal);
}

// Ett ljus (klass 27) med sin klass 185, eller med en bakåtpekare till en
// klass 185 om `shared_185` ges. Returnerar klass 185:s id.
int32_t light(ByteWriter& w, int index, Offsets* o, const int32_t* shared_185 = nullptr) {
    w.obj(27);
    if (o) o->light_version = w.data.size();
    w.i32(1);
    int32_t id185;
    if (shared_185) {
        w.ref(*shared_185);
        id185 = *shared_185;
    } else {
        id185 = w.obj(185);
        w.str("{\"version\":2}");
        w.null_str();
        w.i32(0);
    }
    if (o) o->light_marker = w.data.size();
    w.i32(0);
    for (int i = 0; i < 12; ++i) w.f64(i + 0.5 + index);
    return id185;
}

// Svansen, med [1] och utfyllnaden sist. Returnerar id för det första nya
// ljuset, eller 0 om det inte finns något.
int32_t tail(ByteWriter& w, const Spec& s, Offsets* o) {
    if (o) o->version = w.data.size();
    w.i32(2);
    if (o) o->count = w.data.size();
    w.i32(s.lights + (s.extra != Spec::None ? 1 : 0));
    int32_t first_light = s.back_target, first_185 = 0, first_new = 0;
    for (int i = 0; i < s.lights; ++i) {
        const int32_t id = w.next_id;
        const int32_t id185 = light(w, i, i == 0 ? o : nullptr);
        if (i == 0) {
            first_light = first_new = id;
            first_185 = id185;
        }
    }
    if (s.extra == Spec::BackLight) w.ref(first_light);
    if (s.extra == Spec::Share185) light(w, s.lights, nullptr, &first_185);

    w.f64(0.1);
    w.f64(0.2);
    w.f64(0.3);
    if (o) o->loader_marker = w.data.size();
    w.i32(0);
    w.str(s.loader);
    w.i32(448);
    w.i32(s.unit_code);
    if (o) o->unit_marker = w.data.size();
    w.i32(1);
    for (const double v : {0.0, 0.0, 1.0, 0.0, 1.0, 0.0}) w.f64(v);
    if (o) o->path_marker0 = w.data.size();
    w.i32(0);
    if (o) o->path_marker1 = w.data.size();
    w.i32(-1);
    if (o) o->path_marker2 = w.data.size();
    w.i32(1);
    w.str(s.path);
    for (const double v : {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0}) w.f64(v);
    if (o) o->end_one = w.data.size();
    w.i32(1);
    w.pad(8);
    return first_new;
}

// Ett attribut av klass 31.
void material(ByteWriter& w) {
    w.obj(31);
    w.null_str();
    for (int i = 0; i < 15; ++i) w.f64(i >= 1 && i <= 6 ? 1.0 : 0.0);
}

// Huvudet i en partitionsnod efter pekaren: [i32 0][str namn][ptr 52][flags][X][Y].
void head(ByteWriter& w, const Spec& s, Offsets* o) {
    w.i32(0);
    w.str(s.name);
    name_pair(w, "File", "LcOaPartition");
    w.i32(0);
    if (o) o->element = w.data.size();
    w.i32(s.element);
    if (o) o->attributes = w.data.size();
    w.i32(s.attributes);
    for (int i = 0; i < s.attributes; ++i) material(w);
}

// Hela strömmen LcOpNwdPartition.
Bytes stream(const Spec& s, Offsets* o = nullptr) {
    ByteWriter w;
    w.obj(32);
    head(w, s, o);
    tail(w, s, o);
    return w.data;
}

ErrorCode parse(const Bytes& b, Partition& p, std::string* detail = nullptr) {
    return parse_partition_stream(b.data(), b.size(), p, detail);
}

// Ger strömmen exakt `code`, ett tomt resultat och ett meddelande som inleds
// med paketets namn och, om `rule` ges, innehåller regeln?
bool fails_with(const Bytes& b, ErrorCode code, const char* rule = nullptr) {
    Partition p;
    std::string detail;
    const ErrorCode e = parse(b, p, &detail);
    bool ok = e == code && detail.rfind(kStreamName, 0) == 0 && p.name.empty() &&
              p.lights.empty() && p.tail.loader.empty() && p.attribute_count == 0;
    if (rule) ok = ok && detail.find(rule) != std::string::npos;
    if (!ok) std::printf("  got %d \"%s\"\n", static_cast<int>(e), detail.c_str());
    return ok;
}

void put32(Bytes& b, size_t at, int32_t v) { std::memcpy(b.data() + at, &v, 4); }

int test_synthetic_stream() {
    int fails = 0;

    // En partition med två ljus: alla fält kommer tillbaka.
    {
        Spec s;
        s.lights = 2;
        const Bytes b = stream(s);
        Partition p;
        std::string detail;
        CHECK(parse(b, p, &detail) == ErrorCode::Ok);
        CHECK(detail.empty());
        CHECK(p.name == "Model.ifc");
        CHECK(p.has_user_name && p.user_display == "File" && p.user_internal == "LcOaPartition");
        CHECK(p.flags == 0 && p.element == 7 && p.attribute_count == 1);
        CHECK(p.lights.size() == 2);
        if (p.lights.size() == 2) {
            CHECK(p.lights[0].version == 1 && p.lights[1].version == 1);
            for (int i = 0; i < 12; ++i) {
                CHECK(p.lights[0].values[i] == i + 0.5);
                CHECK(p.lights[1].values[i] == i + 1.5);
            }
        }
        const PartitionTail& t = p.tail;
        CHECK(t.light_refs.size() == 2 && t.light_refs[0] == 0 && t.light_refs[1] == 1);
        CHECK(t.colour[0] == 0.1 && t.colour[1] == 0.2 && t.colour[2] == 0.3);
        CHECK(t.loader == "LcNwcLoaderPlugin:test");
        CHECK(t.format_version == 448 && t.unit_code == 2);
        CHECK(t.a[0] == 0 && t.a[1] == 0 && t.a[2] == 1);
        CHECK(t.b[0] == 0 && t.b[1] == 1 && t.b[2] == 0);
        CHECK(t.source_path == "C:\\src\\Model.ifc");
        CHECK(t.c[0] == 1 && t.c[1] == 2 && t.c[2] == 3);
        CHECK(t.d[0] == 4 && t.d[1] == 5 && t.d[2] == 6 && t.d[3] == 7);
        // Utan detail-pekare går det lika bra.
        CHECK(parse_partition_stream(b.data(), b.size(), p, nullptr) == ErrorCode::Ok);
    }

    // Antalet attribut och ljus, och strängar i alla längder, så att
    // 64-bitarsvärdena hamnar på båda sidor om en 8-byte-gräns.
    for (int attributes = 0; attributes <= 2; ++attributes) {
        for (int lights = 0; lights <= 2; ++lights) {
            for (size_t len = 1; len <= 9; ++len) {
                Spec s;
                s.attributes = attributes;
                s.lights = lights;
                s.name = std::string(len, 'n');
                s.path = std::string(len + 3, 'p');
                s.loader = std::string(len + 1, 'l');
                Partition p;
                CHECK(parse(stream(s), p) == ErrorCode::Ok);
                CHECK(p.attribute_count == static_cast<uint32_t>(attributes));
                CHECK(p.lights.size() == static_cast<size_t>(lights));
                CHECK(p.name == s.name && p.tail.source_path == s.path &&
                      p.tail.loader == s.loader);
            }
        }
    }

    // Null-strängar och ett null namnpar är tomma i resultatet och inga fel.
    {
        ByteWriter w;
        w.obj(32);
        w.i32(0);
        w.null_str();
        w.null_ptr();
        w.i32(0);
        w.i32(0);
        w.i32(0);
        w.i32(2);
        w.i32(0);
        for (int i = 0; i < 3; ++i) w.f64(0);
        w.i32(0);
        w.null_str();
        w.i32(448);
        w.i32(0);
        w.i32(1);
        for (int i = 0; i < 6; ++i) w.f64(0);
        w.i32(0);
        w.i32(-1);
        w.i32(1);
        w.null_str();
        for (int i = 0; i < 7; ++i) w.f64(0);
        w.i32(1);
        w.pad(8);
        Partition p;
        CHECK(parse(w.data, p) == ErrorCode::Ok);
        CHECK(p.name.empty() && !p.has_user_name && p.tail.loader.empty() &&
              p.tail.source_path.empty());
    }

    // Bakåtpekare i svansen: till ett ljus, och till klass 185 i ett nytt ljus.
    {
        Spec s;
        s.lights = 1;
        s.extra = Spec::BackLight;
        Partition p;
        CHECK(parse(stream(s), p) == ErrorCode::Ok);
        CHECK(p.lights.size() == 1);
        CHECK(p.tail.light_refs.size() == 2 && p.tail.light_refs[0] == 0 &&
              p.tail.light_refs[1] == 0);

        Spec t;
        t.lights = 1;
        t.extra = Spec::Share185;
        CHECK(parse(stream(t), p) == ErrorCode::Ok);
        CHECK(p.lights.size() == 2 && p.tail.light_refs.size() == 2 && p.tail.light_refs[1] == 1);

        // En bakåtpekare när inget ljus finns: id:t är okänt.
        Spec u;
        u.lights = 0;
        u.extra = Spec::BackLight;
        CHECK(fails_with(stream(u), ErrorCode::CorruptObject, "back-reference to an unknown id"));
    }

    // Avhuggen på varje längd.
    {
        Spec s;
        s.lights = 2;
        const Bytes b = stream(s);
        for (size_t len = 0; len < b.size(); ++len) {
            const Bytes cut(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(len));
            Partition p;
            std::string detail;
            const ErrorCode e = parse(cut, p, &detail);
            if (e == ErrorCode::Ok || detail.rfind(kStreamName, 0) != 0) {
                std::printf("  cut at %zu: %d \"%s\"\n", len, static_cast<int>(e), detail.c_str());
                ++fails;
            }
        }
    }

    // Fel markörer och antal.
    {
        Spec s;
        s.lights = 1;
        Offsets o;
        const Bytes good = stream(s, &o);
        struct Case {
            size_t at;
            int32_t value;
            const char* rule;
        };
        const Case cases[] = {
            {o.version, 3, "partition tail does not start with 2"},
            {o.light_version, 2, "class-27 version is not 1"},
            {o.light_marker, 1, "class-27 marker after the material is not 0"},
            {o.loader_marker, 1, "marker before the loader is not 0"},
            {o.unit_marker, 0, "marker after the unit code is not 1"},
            {o.path_marker0, 2, "marker before the label is not 0 or 1"},
            {o.path_marker2, 2, "marker before the source path is not 1"},
            {o.end_one, 2, "partition tail does not end with 1"},
        };
        for (const Case& c : cases) {
            Bytes b = good;
            put32(b, c.at, c.value);
            CHECK(fails_with(b, ErrorCode::CorruptObject, c.rule));
        }

        Bytes b = good;
        put32(b, o.element, -1);
        CHECK(fails_with(b, ErrorCode::CorruptObject, "element index is negative"));
        for (const int32_t n : {0x40000000, -1}) {
            b = good;
            put32(b, o.count, n);
            CHECK(fails_with(b, ErrorCode::CorruptObject, "count is negative"));
            b = good;
            put32(b, o.attributes, n);
            CHECK(fails_with(b, ErrorCode::CorruptObject, "count is negative"));
        }
        // Ett ljusantal som inte stämmer med det som står där.
        for (const int32_t n : {0, 2, 3}) {
            b = good;
            put32(b, o.count, n);
            CHECK(fails_with(b, ErrorCode::CorruptObject));
        }
        // Ett attributantal som är för litet eller för stort.
        for (const int32_t n : {0, 2}) {
            b = good;
            put32(b, o.attributes, n);
            CHECK(fails_with(b, ErrorCode::CorruptObject));
        }
    }

    // Slutet: utfyllnad som inte är noll, och data efter slutet.
    {
        const Bytes good = stream(Spec{});
        Bytes b = good;
        b.back() = 1;
        CHECK(fails_with(b, ErrorCode::CorruptObject, "partition tail pad is not zero"));
        for (const size_t extra : {size_t(8), size_t(4), size_t(1)}) {
            b = good;
            b.insert(b.end(), extra, 0);
            CHECK(fails_with(b, ErrorCode::CorruptObject));
        }
        b = good;
        b.push_back(1);
        CHECK(fails_with(b, ErrorCode::CorruptObject));
    }

    // Fel klass och fel pekare i början.
    {
        ByteWriter w;
        w.obj(53);
        CHECK(fails_with(w.data, ErrorCode::CorruptObject, "class that this slot does not allow"));
        Bytes b = stream(Spec{});
        put32(b, 0, 101);
        CHECK(fails_with(b, ErrorCode::CorruptObject, "pointer is neither 0, 1 nor the next id"));
        put32(b, 0, 0);
        CHECK(fails_with(b, ErrorCode::CorruptObject, "null pointer in a slot"));
        b = stream(Spec{});
        put32(b, 8, 1);
        CHECK(fails_with(b, ErrorCode::CorruptObject, "class-32 body does not start with 0"));
        CHECK(fails_with(Bytes{}, ErrorCode::CorruptObject));
    }

    // Attribut som strömmen inte kan läsa: en tom plats och en annan klass.
    for (const bool null_slot : {true, false}) {
        ByteWriter w;
        w.obj(32);
        w.i32(0);
        w.str("N");
        name_pair(w, "File", "LcOaPartition");
        w.i32(0);
        w.i32(0);
        w.i32(1);
        if (null_slot) w.null_ptr();
        else w.obj(86);
        w.pad(8);
        CHECK(fails_with(w.data, ErrorCode::Unsupported,
                         null_slot ? "null attribute slot" : "attribute class is not read"));
    }

    // Pekarna i svansen.
    {
        Spec s;
        s.lights = 1;
        Offsets o;
        const Bytes good = stream(s, &o);
        // Ljusets pekare ligger direkt efter antalet: [id][27].
        Bytes b = good;
        put32(b, o.count + 4, 999);
        CHECK(fails_with(b, ErrorCode::CorruptObject, "pointer is neither 0, 1 nor the next id"));
        b = good;
        put32(b, o.count + 8, 28);
        CHECK(fails_with(b, ErrorCode::CorruptObject, "class that this slot does not allow"));
        b = good;
        put32(b, o.count + 4, 0);
        CHECK(fails_with(b, ErrorCode::CorruptObject, "null pointer in a slot"));
        // En bakåtpekare till klass 52 där ett ljus ska stå.
        b = good;
        put32(b, o.count + 4, 1);
        put32(b, o.count + 8, 101);
        CHECK(fails_with(b, ErrorCode::CorruptObject, "back-reference to a class"));
    }
    return fails;
}

// ---------------------------------------------------------------------------
// Klass 32 i trädet.

// En grupp utan attribut och barn.
void leafless_group(ByteWriter& w, const char* name) {
    w.obj(53);
    w.str(name);
    name_pair(w, name, "Lc");
    w.i32(0);
    w.i32(0);
    w.i32(0);
    w.i32(0);
}

// En toppnod av klass 32 med `children` barn och en svans. Returnerar id för
// svansens första nya ljus, eller 0.
int32_t top32(ByteWriter& w, Spec s, int children) {
    s.element = 0;   // GUID-tabellen är tom i de syntetiska träden
    w.obj(32);
    head(w, s, nullptr);
    w.i32(children);
    for (int i = 0; i < children; ++i) leafless_group(w, "Child");
    return tail(w, s, nullptr);
}

// Ett träd med k = 1 och noll rötter; anroparen skriver toppnoderna.
template <typename Body>
Bytes tree(int tops, Body body) {
    ByteWriter w;
    w.i32(1);
    w.i32(0);
    w.implicit_root();
    w.i32(tops);
    body(w);
    w.i32(1);
    w.pad(8);
    return w.data;
}

Spec with_attributes(Spec s, int attributes) {
    s.attributes = attributes;
    return s;
}

int test_synthetic_tree() {
    int fails = 0;

    // Två partitioner: den första med ett ljus, den andra med en bakåtpekare till det.
    {
        const Bytes t = tree(2, [](ByteWriter& w) {
            Spec a;
            a.name = "A.ifc";
            a.lights = 1;
            const int32_t light_id = top32(w, a, 1);
            Spec b;
            b.back_target = light_id;
            b.name = "B.ifc";
            b.path = "C:\\src\\B.ifc";
            b.extra = Spec::BackLight;
            b.unit_code = 0;
            top32(w, b, 0);
        });
        Hierarchy h;
        std::string detail;
        const ErrorCode e = parse_hierarchy(t.data(), t.size(), nullptr, 0, kBig, h, &detail);
        if (e != ErrorCode::Ok) std::printf("  got %d \"%s\"\n", static_cast<int>(e), detail.c_str());
        CHECK(e == ErrorCode::Ok);
        CHECK(h.top.size() == 2);
        CHECK(h.nodes.size() == 3);   // A, barnet, B
        CHECK(h.partitions.size() == 2 && h.lights.size() == 1);
        CHECK(h.expanded_total == 4);   // roten, A, barnet, B
        if (h.top.size() == 2 && h.partitions.size() == 2) {
            const StoredNode& a = h.nodes[h.top[0]];
            const StoredNode& b = h.nodes[h.top[1]];
            CHECK(a.cls == 32 && b.cls == 32);
            CHECK(h.partition(h.top[0]) == 0 && h.partition(h.top[1]) == 1);
            CHECK(h.strings.get(a.name) == "A.ifc" && h.strings.get(b.name) == "B.ifc");
            CHECK(h.child_count(h.top[0]) == 1 && h.child_count(h.top[1]) == 0);
            CHECK(a.expanded == 2 && b.expanded == 1 && a.height == 2 && b.height == 1);
            CHECK(h.nodes[h.children[a.child_begin]].cls == 53);
            CHECK(h.partition(h.children[a.child_begin]) == kNone);
            CHECK(h.partitions[0].light_refs.size() == 1 && h.partitions[0].light_refs[0] == 0);
            CHECK(h.partitions[1].light_refs.size() == 1 && h.partitions[1].light_refs[0] == 0);
            CHECK(h.partitions[0].unit_code == 2 && h.partitions[1].unit_code == 0);
            CHECK(h.partitions[1].source_path == "C:\\src\\B.ifc");
            CHECK(h.partitions[0].loader == "LcNwcLoaderPlugin:test");
            CHECK(h.lights[0].version == 1 && h.lights[0].values[0] == 0.5);
        }
    }

    // En grupp som toppnod före en partition: båda klasserna får stå där.
    {
        const Bytes t = tree(2, [](ByteWriter& w) {
            leafless_group(w, "G");
            top32(w, Spec{}, 0);
        });
        Hierarchy h;
        CHECK(parse_hierarchy(t.data(), t.size(), nullptr, 0, kBig, h) == ErrorCode::Ok);
        CHECK(h.top.size() == 2);
        if (h.top.size() == 2) {
            CHECK(h.nodes[h.top[0]].cls == 53 && h.nodes[h.top[1]].cls == 32);
            CHECK(h.partition(h.top[0]) == kNone && h.partitions.size() == 1);
        }
    }

    // Brott mot reglerna.
    {
        Hierarchy h;
        std::string detail;
        auto parse_tree = [&](const Bytes& t) {
            return parse_hierarchy(t.data(), t.size(), nullptr, 0, kBig, h, &detail);
        };

        // Den andra toppnodens pekare har ett id för mycket.
        Bytes t = tree(2, [](ByteWriter& w) {
            top32(w, Spec{}, 0);
            w.next_id += 1;
            top32(w, Spec{}, 0);
        });
        CHECK(parse_tree(t) == ErrorCode::CorruptObject);
        CHECK(detail.rfind(kTreeName, 0) == 0 &&
              detail.find("pointer is neither 0, 1 nor the next id") != std::string::npos);
        CHECK(h.nodes.empty() && h.partitions.empty() && h.lights.empty());

        // Svansen saknas, eller [1] sist i den.
        t = tree(1, [](ByteWriter& w) {
            w.obj(32);
            head(w, Spec{}, nullptr);
            w.i32(0);
        });
        CHECK(parse_tree(t) == ErrorCode::CorruptObject);

        // Kroppen börjar inte med 0.
        t = tree(1, [](ByteWriter& w) { top32(w, Spec{}, 0); });
        Hierarchy ok;
        CHECK(parse_hierarchy(t.data(), t.size(), nullptr, 0, kBig, ok) == ErrorCode::Ok);
        put32(t, 16 + 8, 1);   // [i32 0] efter pekaren
        CHECK(parse_tree(t) == ErrorCode::CorruptObject);
        CHECK(detail.find("class-32 body does not start with 0") != std::string::npos);

        // Trädets eget slut: [1] efter den sista svansen, och bara det.
        t = tree(1, [](ByteWriter& w) { top32(w, Spec{}, 0); });
        t.insert(t.end(), 8, 0);
        CHECK(parse_tree(t) == ErrorCode::CorruptObject);
        t = tree(1, [](ByteWriter& w) { top32(w, Spec{}, 0); });
        put32(t, t.size() - 8, 0);
        CHECK(parse_tree(t) == ErrorCode::CorruptObject);

        // Ett attribut av null-typ tar en egenskapsrot, och här finns ingen.
        t = tree(1, [](ByteWriter& w) {
            w.obj(32);
            w.i32(0);
            w.str("N");
            name_pair(w, "File", "LcOaPartition");
            w.i32(0);
            w.i32(0);
            w.i32(1);
            w.null_ptr();
            w.i32(0);
            tail(w, Spec{}, nullptr);
        });
        CHECK(parse_tree(t) == ErrorCode::CorruptLink);

        // Attribut av klass 31 på noden fungerar som på en grupp.
        t = tree(1, [](ByteWriter& w) { top32(w, with_attributes(Spec{}, 2), 0); });
        CHECK(parse_tree(t) == ErrorCode::Ok);
        CHECK(h.nodes.size() == 1 && h.nodes[0].material != kNone);
    }
    return fails;
}

// ---------------------------------------------------------------------------
// Exempelfilerna.

// Paketets uppackade ström.
bool package_stream(const MappedFile& file, const Container& c, const char* name, Bytes& out) {
    const DbCacheEntry* e = find_entry(c.entries, name);
    StreamSpan span;
    return e && last_stream(*e, file.size(), span) == ErrorCode::Ok &&
           inflate_span(file.data(), file.size(), span, kDefaultMaxStreamBytes, out) ==
               ErrorCode::Ok;
}

// Allt en fil ger som har med trädet och partitionen att göra.
struct Loaded {
    const char* path = nullptr;
    Bytes tree;
    Bytes partition_stream;
    std::vector<Bytes> props;
    std::vector<Guid16> guids;
    Hierarchy h;
    Partition partition;
    FragmentStore fragments;
};

bool load(const char* path, Loaded& L) {
    L.path = path;
    MappedFile file;
    if (file.open(path) != ErrorCode::Ok) {
        std::printf("  FAIL — %s: öppnas inte\n", path);
        return false;
    }
    Container c;
    Bytes data;
    std::string detail;
    const DbCacheEntry* pe = nullptr;
    ErrorCode e = read_container(file.data(), file.size(), c, &detail);
    if (e == ErrorCode::Ok) pe = find_entry(c.entries, "LcOaPartitionProps");
    if (e != ErrorCode::Ok || !pe || !package_stream(file, c, "LcOpNwdLogicalHierarchy", L.tree) ||
        !package_stream(file, c, "LcOpNwdPartition", L.partition_stream) ||
        !package_stream(file, c, "LcOpGuidStore", data) ||
        parse_guid_store(data.data(), data.size(), L.guids, &detail) != ErrorCode::Ok ||
        read_partition_props(file.data(), file.size(), *pe, kDefaultMaxStreamBytes, L.props,
                             &detail) != ErrorCode::Ok ||
        !package_stream(file, c, "LcOpNwdFragments", data) ||
        parse_fragments(data.data(), data.size(), L.fragments, nullptr, &detail) !=
            ErrorCode::Ok) {
        std::printf("  FAIL — %s: paketen läses inte %s\n", path, detail.c_str());
        return false;
    }
    e = parse_hierarchy(L.tree.data(), L.tree.size(), &L.props,
                        static_cast<uint32_t>(L.guids.size()), kBig, L.h, &detail);
    if (e != ErrorCode::Ok) {
        std::printf("  FAIL — %s: trädet: %s %s\n", path, error_message(e), detail.c_str());
        return false;
    }
    e = parse_partition_stream(L.partition_stream.data(), L.partition_stream.size(), L.partition,
                               &detail);
    if (e != ErrorCode::Ok) {
        std::printf("  FAIL — %s: partitionen: %s %s\n", path, error_message(e), detail.c_str());
        return false;
    }
    return true;
}

// Preorder över det expanderade trädet från `node`. Delade delträd gås igenom
// varje gång de förekommer.
void preorder(const Hierarchy& h, uint32_t node, std::vector<uint32_t>& out) {
    struct Frame {
        uint32_t node, next;
    };
    std::vector<Frame> stack{{node, 0}};
    out.push_back(node);
    while (!stack.empty()) {
        Frame& f = stack.back();
        const StoredNode& n = h.nodes[f.node];
        if (f.next == h.child_count(f.node)) {
            stack.pop_back();
            continue;
        }
        const uint32_t child = h.children[n.child_begin + f.next++];
        out.push_back(child);
        stack.push_back({child, 0});
    }
}

// Fragmentens seq mot det expanderade trädet: den underförstådda roten är
// index 0, sedan kommer toppnoderna i preorder, och en klass-32-nod räknas som en nod.
int check_seq(const Loaded& L) {
    int fails = 0;
    std::vector<uint8_t> leaf{0};
    std::vector<uint32_t> order;
    for (const uint32_t top : L.h.top) preorder(L.h, top, order);
    for (const uint32_t n : order) leaf.push_back(L.h.nodes[n].cls == 22 ? 1 : 0);
    CHECK(leaf.size() == L.h.expanded_total);

    size_t bad = 0;
    for (const FragmentView& f : fragment_views(L.fragments))
        if (f.seq < 0 || static_cast<size_t>(f.seq) >= leaf.size() || !leaf[static_cast<size_t>(f.seq)])
            ++bad;
    if (bad) std::printf("  FAIL — %s: %zu fragment pekar inte på ett löv\n", L.path, bad);
    CHECK(bad == 0);
    return fails;
}

bool starts_with(std::string_view s, std::string_view prefix) { return s.rfind(prefix, 0) == 0; }
bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

// Två träd lika i preorder: samma klass, namn, flaggor, antal flikar och barn
// i varje nod, om noden är ett element (indexen skiljer: .nwd har en egen
// GUID-tabell), och lika många lagrade noder.
int same_subtree(const Hierarchy& a, uint32_t na, const Hierarchy& b, uint32_t nb,
                 const char* what) {
    int fails = 0;
    std::vector<uint32_t> oa, ob;
    preorder(a, na, oa);
    preorder(b, nb, ob);
    CHECK(oa.size() == ob.size());
    CHECK(a.nodes[na].expanded == b.nodes[nb].expanded);

    size_t differing = 0;
    for (size_t i = 0; i < oa.size() && i < ob.size(); ++i) {
        const StoredNode& x = a.nodes[oa[i]];
        const StoredNode& y = b.nodes[ob[i]];
        if (x.cls != y.cls || (x.name == kNone) != (y.name == kNone) ||
            a.strings.get(x.name) != b.strings.get(y.name) || a.flags(oa[i]) != b.flags(ob[i]) ||
            (x.element == 0) != (y.element == 0) || a.tab_count(oa[i]) != b.tab_count(ob[i]) ||
            a.child_count(oa[i]) != b.child_count(ob[i]))
            ++differing;
    }
    if (differing) std::printf("  FAIL — %s: %zu noder skiljer i preorder\n", what, differing);
    CHECK(differing == 0);

    // Antalet lagrade noder: de som går att nå från noden.
    auto distinct = [](const std::vector<uint32_t>& order, size_t total) {
        std::vector<uint8_t> seen(total, 0);
        size_t n = 0;
        for (const uint32_t i : order)
            if (!seen[i]) {
                seen[i] = 1;
                ++n;
            }
        return n;
    };
    CHECK(distinct(oa, a.nodes.size()) == distinct(ob, b.nodes.size()));
    return fails;
}

// I filen med flera modeller: poserna med skala (klass 16) är exakt
// fragmenten i modellerna med enhetskod 0, alla med skalan 1000 som räknar om
// meter till dokumentets millimeter. Modellen för ett fragment ges av seq: den
// underförstådda roten är 0, och varje toppnod täcker sina `expanded` platser
// i följd.
int check_scaled_poses(const Loaded& doc) {
    int fails = 0;
    const Hierarchy& h = doc.h;
    std::vector<uint64_t> begin;   // varje toppnods första seq
    uint64_t next = 1;
    for (const uint32_t t : h.top) {
        begin.push_back(next);
        next += h.nodes[t].expanded;
    }
    CHECK(next == h.expanded_total);

    // Enhetskoden för det fragment som har `seq`, eller -1 utanför toppnoderna.
    auto unit_of = [&](int32_t seq) -> int32_t {
        if (seq < 1 || static_cast<uint64_t>(seq) >= next) return -1;
        size_t k = begin.size();
        while (k > 0 && begin[k - 1] > static_cast<uint64_t>(seq)) --k;
        const uint32_t partition = h.partition(h.top[k - 1]);
        return partition < h.partitions.size() ? h.partitions[partition].unit_code : -1;
    };

    uint32_t scaled = 0, wrong = 0, metre_models = 0;
    for (const FragmentView& f : fragment_views(doc.fragments)) {
        const int32_t unit = unit_of(f.seq);
        if (unit == 0) ++metre_models;
        if (f.pose_class != 16) {
            // Ett fragment i en metermodell utan skala vore inte omräknat.
            if (unit == 0) ++wrong;
            continue;
        }
        ++scaled;
        if (unit != 0 || std::fabs(f.s - 1000.0) > 1e-9) ++wrong;
    }
    if (wrong) std::printf("  FAIL — %u fragment bryter regeln om skalan 1000\n", wrong);
    CHECK(wrong == 0);
    CHECK(scaled == testdata::kSampleNwdScaledPoses);
    CHECK(metre_models == testdata::kSampleNwdScaledPoses);
    return fails;
}

// De tio filerna.
int test_samples() {
    int fails = 0;
    std::vector<Loaded> nwc(std::size(testdata::kSampleNwc));
    bool all = true;
    for (size_t i = 0; i < nwc.size(); ++i) {
        if (!testdata::exists(testdata::kSampleNwc[i])) {
            std::printf("  SKIP — %s saknas\n", testdata::kSampleNwc[i]);
            ++g_skipped;
            all = false;
            continue;
        }
        if (!load(testdata::kSampleNwc[i], nwc[i])) {
            ++fails;
            all = false;
            continue;
        }
        const Loaded& L = nwc[i];
        const PartitionTail& t = L.partition.tail;
        // Partitionen: ett .ifc-namn, loadern, versionen och källsökvägen.
        CHECK(ends_with(L.partition.name, ".ifc"));
        CHECK(starts_with(t.loader, "LcNwcLoaderPlugin:"));
        CHECK(t.format_version == testdata::kSampleVersion);
        CHECK(ends_with(t.source_path, L.partition.name));
        CHECK(L.partition.lights.empty() && t.light_refs.empty());
        CHECK(L.partition.attribute_count == 1);
        CHECK(L.partition.has_user_name && L.partition.user_internal == "LcOaPartition");
        CHECK(L.h.partitions.empty() && L.h.top.size() == 1 && L.h.nodes[L.h.top[0]].cls == 53);
        // Enhetskoden är belagd och har en längd.
        CHECK(t.unit_code == testdata::kFacts[i].unit_code);
        double metres = 0;
        CHECK(metres_per_unit(t.unit_code, metres) && metres > 0);
        fails += check_seq(L);
    }

    if (!testdata::exists(testdata::kSampleNwd)) {
        std::printf("  SKIP — %s saknas\n", testdata::kSampleNwd);
        ++g_skipped;
        return fails;
    }
    Loaded doc;
    if (!load(testdata::kSampleNwd, doc)) return fails + 1;
    const Hierarchy& h = doc.h;

    // Dokumentets egen partition.
    CHECK(doc.partition.name == "Navisworks.nwd");
    CHECK(doc.partition.attribute_count == 0);
    CHECK(doc.partition.lights.size() == 2 && doc.partition.tail.light_refs.size() == 2);
    CHECK(!starts_with(doc.partition.tail.loader, "LcNwcLoaderPlugin:") &&
          !doc.partition.tail.loader.empty());
    CHECK(ends_with(doc.partition.tail.source_path, "Navisworks.nwd"));
    CHECK(doc.partition.tail.format_version == testdata::kSampleVersion);
    CHECK(doc.partition.tail.unit_code ==
          testdata::kFacts[std::size(testdata::kFacts) - 1].unit_code);

    // Trädet: nio toppnoder av klass 32, alla egenskapsrötter förbrukade.
    const testdata::SampleCounts& s = testdata::kSamples[std::size(testdata::kSamples) - 1];
    CHECK(h.top.size() == 9 && h.top.size() == s.top_nodes);
    CHECK(h.partitions.size() == 9);
    for (const uint32_t t : h.top) {
        CHECK(h.nodes[t].cls == 32);
        CHECK(h.child_count(t) == 1);
        CHECK(h.partition(t) < h.partitions.size());
    }
    int64_t roots = 0;
    for (const int32_t n : h.root_counts) roots += n;
    CHECK(roots == 824 && h.null_slots == 824 && h.null_slots == s.property_roots);
    CHECK(doc.props.size() == 1 && h.root_counts.size() == 1);

    // (a) Pekarna: nio klass-32-pekare. Den första ligger på byte 16, efter
    // toppnodsantalet 9 på byte 12; de åtta andra hittas här i byten som
    // [id][32][0] med växande id, och trädläsaren har läst var och en direkt efter
    // den föregåendes svans.
    {
        int32_t tops, first_id, first_cls;
        std::memcpy(&tops, doc.tree.data() + 12, 4);
        std::memcpy(&first_id, doc.tree.data() + 16, 4);
        std::memcpy(&first_cls, doc.tree.data() + 20, 4);
        CHECK(tops == 9 && first_id == 101 && first_cls == 32);

        size_t pointers = 0;
        int32_t previous = first_id;
        bool increasing = true;
        for (size_t i = 24; i + 12 <= doc.tree.size(); i += 4) {
            int32_t id, cls, zero;
            std::memcpy(&id, doc.tree.data() + i, 4);
            std::memcpy(&cls, doc.tree.data() + i + 4, 4);
            std::memcpy(&zero, doc.tree.data() + i + 8, 4);
            if (cls == 32 && zero == 0 && id > 101) {
                ++pointers;
                if (id <= previous) increasing = false;
                previous = id;
            }
        }
        CHECK(pointers == 8 && increasing);
    }

    // (b) Slutet: strömmen är en multipel av 8 och slutar på två gånger [1][0]:
    // den sista svansens slut och trädets eget slut.
    {
        const Bytes& t = doc.tree;
        CHECK(t.size() % 8 == 0 && t.size() >= 16);
        int32_t last[4];
        std::memcpy(last, t.data() + t.size() - 16, 16);
        CHECK(last[0] == 1 && last[1] == 0 && last[2] == 1 && last[3] == 0);

        // Slutet är exakt: åtta byte till, eller ett annat sista tal, ger fel.
        Hierarchy tmp;
        Bytes b = t;
        b.insert(b.end(), 8, 0);
        CHECK(parse_hierarchy(b.data(), b.size(), &doc.props,
                              static_cast<uint32_t>(doc.guids.size()), kBig, tmp) ==
              ErrorCode::CorruptObject);
        b = t;
        put32(b, b.size() - 8, 0);
        CHECK(parse_hierarchy(b.data(), b.size(), &doc.props,
                              static_cast<uint32_t>(doc.guids.size()), kBig, tmp) ==
              ErrorCode::CorruptObject);
        // Utan egenskapsströmmar blir rötterna över.
        CHECK(parse_hierarchy(t.data(), t.size(), nullptr, static_cast<uint32_t>(doc.guids.size()),
                              kBig, tmp) == ErrorCode::CorruptLink);
    }

    // (d) Varje partitions delträd är lika med motsvarande .nwc-träd, och
    // partitionens svans är lika med den i .nwc-filens LcOpNwdPartition.
    if (all) {
        size_t stored = h.nodes.size();
        std::vector<int> used(h.top.size(), 0);
        for (const Loaded& L : nwc) {
            size_t match = h.top.size();
            for (size_t k = 0; k < h.top.size(); ++k)
                if (h.strings.get(h.nodes[h.top[k]].name) == L.partition.name) match = k;
            CHECK(match < h.top.size());
            if (match == h.top.size()) continue;
            ++used[match];

            const StoredNode& top = h.nodes[h.top[match]];
            const uint32_t child = h.children[top.child_begin];
            fails += same_subtree(h, child, L.h, L.h.top[0], L.path);
            CHECK(top.expanded == 1 + L.h.nodes[L.h.top[0]].expanded);
            CHECK(h.flags(h.top[match]) == L.partition.flags);
            CHECK((top.element == 0) == (L.partition.element == 0));
            CHECK(top.material != kNone && L.partition.attribute_count == 1);

            const PartitionTail& a = h.partitions[h.partition(h.top[match])];
            const PartitionTail& b = L.partition.tail;
            CHECK(a.loader == b.loader && a.source_path == b.source_path);
            CHECK(a.format_version == b.format_version && a.unit_code == b.unit_code);
            CHECK(std::memcmp(a.a, b.a, sizeof a.a) == 0);
            stored -= L.h.nodes.size();
        }
        for (const int u : used) CHECK(u == 1);
        // Varje nod i .nwd är en nod i någon .nwc, eller en av de nio partitionerna.
        CHECK(stored == h.top.size());

        // Ordningen i .nwd är .nwc-filernas.
        for (size_t k = 0; k < h.top.size() && k < nwc.size(); ++k)
            CHECK(h.strings.get(h.nodes[h.top[k]].name) == nwc[k].partition.name);

        // De summerade talen.
        uint64_t fragments = 0, roots_sum = 0;
        for (const Loaded& L : nwc) {
            fragments += L.fragments.items.size();
            for (const int32_t n : L.h.root_counts) roots_sum += static_cast<uint64_t>(n);
        }
        CHECK(fragments == doc.fragments.items.size());
        CHECK(roots_sum == h.null_slots);
    }

    // Ljusen: de två i trädets första partition är de två i dokumentets egen.
    CHECK(h.lights.size() == 2);
    CHECK(h.partitions[0].light_refs.size() == 2);
    for (size_t k = 1; k < h.partitions.size(); ++k) CHECK(h.partitions[k].light_refs.empty());
    if (h.lights.size() == 2 && doc.partition.lights.size() == 2) {
        for (size_t k = 0; k < 2; ++k) {
            CHECK(h.lights[k].version == doc.partition.lights[k].version);
            CHECK(std::memcmp(h.lights[k].values, doc.partition.lights[k].values,
                              sizeof h.lights[k].values) == 0);
        }
    }

    // Fälten b och c är dokumentets i varje partition i trädet; den första
    // partitionen har också dokumentets färg och rektangel d.
    for (const PartitionTail& t : h.partitions) {
        CHECK(std::memcmp(t.b, doc.partition.tail.b, sizeof t.b) == 0);
        CHECK(std::memcmp(t.c, doc.partition.tail.c, sizeof t.c) == 0);
    }
    CHECK(std::memcmp(h.partitions[0].colour, doc.partition.tail.colour, sizeof h.partitions[0].colour) == 0);
    CHECK(std::memcmp(h.partitions[0].d, doc.partition.tail.d, sizeof h.partitions[0].d) == 0);

    // Fragmentens seq pekar på löv.
    fails += check_seq(doc);
    fails += check_scaled_poses(doc);
    return fails;
}

// Metres per enhet för varje kod.
int test_units() {
    int fails = 0;
    double m = -1;
    CHECK(metres_per_unit(0, m) && m == 1.0);
    CHECK(metres_per_unit(1, m) && m == 0.01);
    CHECK(metres_per_unit(2, m) && m == 0.001);
    CHECK(metres_per_unit(3, m) && m == 0.3048);
    // Kvoten mellan två koder är skalan i en klass 39 på partitionen: kod 1
    // under kod 2 är 10, under kod 3 0,01 / 0,3048.
    double inner = 0, outer = 0;
    CHECK(metres_per_unit(1, inner) && metres_per_unit(2, outer) &&
          std::fabs(inner / outer - 10.0) < 1e-12);
    CHECK(metres_per_unit(1, inner) && metres_per_unit(3, outer) &&
          std::fabs(inner / outer - 0.0328083989501312) < 1e-12);
    // En kod som inte är belagd ger false och lämnar värdet orört.
    for (const int32_t code : {4, -1, 5, 100}) {
        m = -1;
        CHECK(!metres_per_unit(code, m) && m == -1);
    }
    return fails;
}

} // namespace

int test_partition() {
    int fails = 0;
    fails += test_synthetic_stream();
    fails += test_synthetic_tree();
    fails += test_units();
    fails += test_samples();
    std::printf("test_partition: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
