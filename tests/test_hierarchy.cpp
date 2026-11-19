#include <chrono>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

#include "container.h"
#include "hierarchy.h"
#include "mapped_file.h"
#include "partition_props.h"
#include "test_data.h"
#include "test_support.h"

using namespace HNwd::detail;
using testdata::Bytes;
using testdata::ByteWriter;

namespace {

using Streams = std::vector<Bytes>;

constexpr uint64_t kBig = uint64_t(1) << 23;
constexpr const char* kTreeName = "LcOpNwdLogicalHierarchy at byte ";
constexpr const char* kStream0Name = "LcOaPartitionProps stream 0 at byte ";

// --- Byggstenar ---------------------------------------------------------------

// Huvudet: k, rotantalen, den underförstådda roten och antalet toppnoder.
void header(ByteWriter& w, std::initializer_list<int32_t> counts, int32_t tops) {
    w.i32(static_cast<int32_t>(counts.size()));
    for (const int32_t c : counts) w.i32(c);
    w.implicit_root();
    w.i32(tops);
}

void finish(ByteWriter& w, int32_t tail = 1) {
    w.i32(tail);
    w.pad(8);
}

// Ett klass-52-objekt.
int32_t name_pair(ByteWriter& w, const char* display, const char* internal = "Lc") {
    const int32_t id = w.obj(52);
    w.str(display);
    w.str(internal);
    return id;
}

// En nod till och med Y. Namnparet är ett nytt klass-52-objekt.
int32_t node_head(ByteWriter& w, int32_t cls, const char* name, int32_t x, int32_t y) {
    const int32_t id = w.obj(cls);
    w.str(name);
    name_pair(w, name);
    w.i32(0);
    w.i32(x);
    w.i32(y);
    return id;
}

// En grupp utan attribut; anroparen skriver barnen.
int32_t group(ByteWriter& w, const char* name, int32_t children) {
    const int32_t id = node_head(w, 53, name, 0, 0);
    w.i32(children);
    return id;
}

int32_t leaf(ByteWriter& w, const char* name) { return node_head(w, 22, name, 0, 0); }

// Klass 185 med en inbäddad fil.
void class185(ByteWriter& w) {
    w.str("name");
    w.str("json");
    w.i32(1);
    w.str("a");
    w.i32(1);
    w.str("b");
    w.str("file");
    w.i64a(3);
    w.hex("010203");
    w.pad(4);
}

// En flik (86) vars egenskaper alla är av typ 2.
int32_t tab_with_ints(ByteWriter& w, const char* name, std::initializer_list<int32_t> values) {
    const int32_t id = w.obj(86);
    w.str(name);
    const int32_t user = name_pair(w, name);
    w.i32(0);
    w.i32(static_cast<int32_t>(values.size()));
    for (const int32_t v : values) {
        w.ref(user);
        w.i32(2);
        w.i32(v);
    }
    return id;
}

// Ett träd med en toppnod som `body` skriver (huvudet och svansen sköts här).
template <class Body>
Bytes tree_of(std::initializer_list<int32_t> counts, Body body, int32_t tail = 1) {
    ByteWriter w;
    header(w, counts, 1);
    body(w);
    finish(w, tail);
    return w.data;
}

// En egenskapsström: rötterna som `body` skriver, och utfyllnaden.
template <class Body>
Bytes stream_of(Body body) {
    ByteWriter w;
    body(w);
    w.pad(8);
    return w.data;
}

// En ström med `count` flikar som rötter.
Bytes tabs_stream(int count) {
    return stream_of([&](ByteWriter& w) {
        for (int i = 0; i < count; ++i) tab_with_ints(w, "T", {i});
    });
}

// En nod med `nulls` null-platser och inga barn.
void node_with_nulls(ByteWriter& w, int32_t nulls) {
    node_head(w, 53, "G", 0, nulls);
    for (int32_t i = 0; i < nulls; ++i) w.null_ptr();
    w.i32(0);
}

ErrorCode parse(const Bytes& tree, const Streams* props, uint32_t guids, uint64_t max,
                Hierarchy& h, std::string* detail = nullptr) {
    return parse_hierarchy(tree.data(), tree.size(), props, guids, max, h, detail);
}

// Ger trädet exakt `code`, ett tomt resultat och ett meddelande som inleds med `prefix`?
bool fails_with(const Bytes& tree, const Streams* props, uint32_t guids, uint64_t max,
                ErrorCode code, const char* prefix = kTreeName) {
    Hierarchy h;
    std::string detail;
    const ErrorCode e = parse(tree, props, guids, max, h, &detail);
    const bool ok = e == code && detail.rfind(prefix, 0) == 0 && h.nodes.empty() &&
                    h.top.empty() && h.expanded_total == 0 && h.root_counts.empty();
    if (!ok) std::printf("  got %d \"%s\"\n", static_cast<int>(e), detail.c_str());
    return ok;
}

bool fails_with(const Bytes& tree, ErrorCode code, uint32_t guids = 0, uint64_t max = kBig) {
    return fails_with(tree, nullptr, guids, max, code);
}

// En klass-66-rot.
Bytes root66(bool null_name, int32_t flags, int64_t value) {
    return stream_of([&](ByteWriter& w) {
        w.obj(66);
        if (null_name) w.null_str(); else w.str("x");
        name_pair(w, "Id", "LcRevitId");
        w.i32(flags);
        w.i64a(value);
    });
}

// --- Grafen med alla attributklasser och värdetyper --------------------------------

struct Full {
    Bytes tree;
    Streams props;
};

Full full_graph() {
    Full f;
    ByteWriter w;
    header(w, {3, 2}, 1);

    // Toppnoden: en grupp med tio attribut och ett barn.
    w.obj(53);
    w.str("Model");
    const int32_t model_user = name_pair(w, "Model Display", "LcModel");
    w.i32(5);    // flags
    w.i32(1);    // X
    w.i32(10);   // Y

    // 1. Klass 86 med en egenskap av varje värdetyp.
    const int32_t item = w.obj(86);
    w.str("Item");
    const int32_t item_user = name_pair(w, "Item", "LcItem");
    w.i32(2);
    w.i32(14);
    const int32_t key = name_pair(w, "key0", "lc_key0");
    w.i32(0);                       // 0: inget
    w.ref(key);
    w.i32(1);
    w.f64(2.5);                     // 1: f64
    w.ref(item_user);
    w.i32(2);
    w.i32(-7);                      // 2: i32
    w.ref(key);
    w.i32(3);
    w.i32(9);                       // 3: i32
    name_pair(w, "key4", "lc_key4");
    w.i32(4);
    w.str("hello");                 // 4: sträng
    w.ref(key);
    w.i32(5);
    w.i64a(1700000000);             // 5: i64
    w.ref(key);
    w.i32(6);
    w.f64(6.5);                     // 6: f64
    w.ref(key);
    w.i32(7);
    w.f64(7.5);                     // 7: f64
    w.ref(key);
    w.i32(8);
    name_pair(w, "Named", "named"); // 8: klass 52
    w.ref(key);
    w.i32(9);
    w.null_str();                   // 9: null-sträng
    w.ref(key);
    w.i32(10);
    w.f64(10.5);                    // 10: f64
    w.ref(key);
    w.i32(11);
    w.f64(11.5);                    // 11: f64
    w.ref(key);
    w.i32(12);
    w.f64(1);
    w.f64(2);
    w.f64(3);                       // 12: tre f64
    w.ref(key);
    w.i32(13);
    w.f64(4);
    w.f64(5);                       // 13: två f64

    // 2. Klass 31.
    const int32_t mat = w.obj(31);
    w.str("Mat");
    for (int i = 0; i < 15; ++i) w.f64(i);

    // 3. Klass 40.
    w.obj(40);
    w.null_str();
    w.i32(0);
    w.i32(0);
    w.f64(1);
    w.f64(2);
    w.f64(3);

    // 4. Klass 41.
    const int32_t xf41 = w.obj(41);
    w.str("x");
    w.i32(0);
    w.i32(0);
    for (const double v : {4.0, 5.0, 6.0, 0.0, 0.0, 0.0, 1.0}) w.f64(v);

    // 5. Klass 39.
    w.obj(39);
    w.str("m");
    w.i32(0);
    w.i32(0);
    for (int i = 1; i <= 9; ++i) w.f64(i);
    w.f64(7);
    w.f64(8);
    w.f64(9);
    w.i32(1);

    // 6. Klass 79 med tre länkar: null, en bakåtpekare till klass 52 och ett nytt löv.
    w.obj(79);
    w.str("L");
    w.i32(0);
    w.i32(0);
    w.i32(3);
    w.str("n1");
    w.str("u1");
    w.null_ptr();
    w.i32(1);
    w.f64(1);
    w.f64(2);
    w.f64(3);
    w.str("n2");
    w.str("u2");
    w.ref(model_user);
    w.i32(0);
    w.str("n3");
    w.str("u3");
    leaf(w, "Linked");
    w.i32(0);

    // 7. Klass 200 med en klass 185.
    w.obj(200);
    w.str("e");
    w.i32(0);
    w.i32(0);
    w.obj(185);
    class185(w);

    // 8-10. Tre null-platser: de tre rötterna i ström 0.
    w.null_ptr();
    w.null_ptr();
    w.null_ptr();

    // Ett barn: ett löv med tre bakåtpekare och två null-platser (ström 1).
    w.i32(1);
    w.obj(22);
    w.str("Leaf");
    w.ref(model_user);
    w.i32(0);
    w.i32(2);   // X
    w.i32(5);   // Y
    w.ref(item);
    w.ref(mat);
    w.ref(xf41);
    w.null_ptr();
    w.null_ptr();
    finish(w);
    f.tree = w.data;

    // Ström 0: en flik, en länk och ett element-id.
    f.props.push_back(stream_of([](ByteWriter& s) {
        s.obj(86);
        s.str("T0");
        const int32_t user = name_pair(s, "T0", "LcT0");
        s.i32(0);
        s.i32(1);
        s.ref(user);
        s.i32(2);
        s.i32(5);

        s.obj(79);
        s.str("links");
        s.i32(0);
        s.i32(0);
        s.i32(1);
        s.str("name");
        s.str("url");
        name_pair(s, "cat", "c");
        s.i32(0);

        s.obj(66);
        s.null_str();
        name_pair(s, "Id", "LcRevitId");
        s.i32(0x01000000);
        s.i64a(12345);
    }));
    // Ström 1: två flikar.
    f.props.push_back(stream_of([](ByteWriter& s) {
        tab_with_ints(s, "S1", {});
        tab_with_ints(s, "S2", {1, 2});
    }));
    return f;
}

std::string text(const Hierarchy& h, uint32_t index) { return std::string(h.strings.get(index)); }

// Alla längder under den hela är fel.
int check_all_cuts(const Full& f, uint32_t guids) {
    int fails = 0;
    for (size_t len = 0; len < f.tree.size(); ++len) {
        Bytes cut(f.tree.begin(), f.tree.begin() + static_cast<std::ptrdiff_t>(len));
        Hierarchy h;
        std::string detail;
        if (parse(cut, &f.props, guids, kBig, h, &detail) == ErrorCode::Ok || detail.empty()) {
            std::printf("  tree cut at %zu was not an error\n", len);
            ++fails;
        }
    }
    for (size_t s = 0; s < f.props.size(); ++s) {
        for (size_t len = 0; len < f.props[s].size(); ++len) {
            Streams cut = f.props;
            cut[s].resize(len);
            Hierarchy h;
            std::string detail;
            const ErrorCode e = parse(f.tree, &cut, guids, kBig, h, &detail);
            if (e != ErrorCode::CorruptObject || detail.rfind("LcOaPartitionProps stream ", 0) != 0) {
                std::printf("  stream %zu cut at %zu gave %d \"%s\"\n", s, len,
                            static_cast<int>(e), detail.c_str());
                ++fails;
            }
        }
    }
    return fails;
}

// --- read_partition_props ------------------------------------------------------------

struct PropsPackage {
    Bytes file;
    DbCacheEntry entry;
};

// Ett paket i formen (4, b) mitt i en fil.
PropsPackage package(int32_t prefix, const Streams& streams, const Bytes& toc) {
    PropsPackage p;
    int32_t b = 0;
    const Bytes pkg = testdata::prefixed(prefix, streams, toc, b);
    p.file.assign(100, 0xEE);
    p.file.insert(p.file.end(), pkg.begin(), pkg.end());
    p.file.insert(p.file.end(), 20, 0xEE);
    p.entry.name = "dir\\LcOaPartitionProps";
    p.entry.flags = 1;
    p.entry.offset = 100;
    p.entry.length = pkg.size();
    p.entry.a = 4;
    p.entry.b = b;
    return p;
}

int32_t zsize(const Bytes& s) { return static_cast<int32_t>(testdata::zlib_store(s).size()); }

// [int32 k][längder] nollor upp till 8.
Bytes ztoc(int32_t k, const std::vector<int32_t>& lengths) {
    ByteWriter w;
    w.i32(k);
    for (const int32_t l : lengths) w.i32(l);
    w.pad(8);
    return w.data;
}

bool props_fail(const PropsPackage& p, ErrorCode code, size_t max = size_t(1) << 20) {
    Streams out;
    out.push_back(Bytes{1});   // ska tömmas
    std::string detail;
    const ErrorCode e = read_partition_props(p.file.data(), p.file.size(), p.entry, max, out, &detail);
    // Fel i paketets ram namnger paketet; fel från uppackningen kommer som de är.
    const bool framing = code == ErrorCode::CorruptPackage || code == ErrorCode::OutOfRange;
    const bool ok = e == code && out.empty() && !detail.empty() &&
                    (!framing || detail.rfind("LcOaPartitionProps at byte ", 0) == 0);
    if (!ok) std::printf("  props got %d \"%s\"\n", static_cast<int>(e), detail.c_str());
    return ok;
}

// --- Trädet läst ur filen ---------------------------------------------------------

// Trädet och egenskapsströmmarna som zlib-strömmar i en fil, med lagrade block
// om högst `block` byte (varje blockgräns kan bli en kontrollpunkt).
struct TreeFile {
    Bytes file;
    HierarchyStreams streams;
};

TreeFile tree_file(const Bytes& tree, const Streams* props, size_t block = 65535) {
    TreeFile f;
    f.file.assign(16, 0xEE);
    auto add = [&](const Bytes& s) {
        const Bytes z = testdata::zlib_store(s, block);
        StreamSpan span;
        span.offset = f.file.size();
        span.length = z.size();
        f.file.insert(f.file.end(), z.begin(), z.end());
        return span;
    };
    f.streams.tree = add(tree);
    if (props) {
        f.streams.has_props = true;
        for (const Bytes& s : *props) f.streams.props.push_back(add(s));
    }
    f.file.insert(f.file.end(), 16, 0xEE);
    return f;
}

ErrorCode parse_file(const TreeFile& f, HierarchyContent content, Hierarchy& h,
                     HierarchySkeleton* sk = nullptr, std::string* detail = nullptr,
                     uint64_t every = kCheckpointEvery, uint32_t guids = 3, uint64_t max = kBig,
                     const HierarchyProgress& progress = {}) {
    return parse_hierarchy(f.file.data(), f.file.size(), f.streams, guids, max, content, h, sk,
                           progress, detail, every);
}

bool same_text(const StringPool& a, uint32_t ia, const StringPool& b, uint32_t ib) {
    return (ia == kNone) == (ib == kNone) && a.get(ia) == b.get(ib);
}

bool same_names(const Hierarchy& a, const Hierarchy& b) {
    if (a.names.size() != b.names.size()) return false;
    for (size_t i = 0; i < a.names.size(); ++i)
        if (!same_text(a.strings, a.names[i].display, b.strings, b.names[i].display) ||
            !same_text(a.strings, a.names[i].internal, b.strings, b.names[i].internal))
            return false;
    return true;
}

// Transform `i` i `h`; klass 0 om den saknas.
StoredTransform transform_at(const Hierarchy& h, uint32_t i) {
    StoredTransform t;
    if (!h.transform(i, t)) t.source_class = 0;
    return t;
}

bool same_transform(const StoredTransform& a, const StoredTransform& b) {
    if (a.source_class != b.source_class || a.reverses != b.reverses) return false;
    for (int i = 0; i < 3; ++i)
        if (a.t[i] != b.t[i]) return false;
    for (int i = 0; i < 4; ++i)
        if (a.q[i] != b.q[i]) return false;
    for (int i = 0; i < 9; ++i)
        if (a.m[i] != b.m[i]) return false;
    return true;
}

// Strukturen: allt utom flikarna. Strängindex jämförs genom texten, eftersom
// Structure inte lägger flikarnas strängar i poolen. Med skelettet jämförs
// också antalet flikar per nod.
int same_structure(const Hierarchy& full, const Hierarchy& s, const HierarchySkeleton* sk = nullptr) {
    int fails = 0;
    CHECK(full.nodes.size() == s.nodes.size());
    CHECK(full.children == s.children);
    CHECK(full.top == s.top);
    CHECK(full.root_counts == s.root_counts);
    CHECK(full.expanded_total == s.expanded_total);
    CHECK(full.null_slots == s.null_slots);
    CHECK(full.non_utf8_strings == s.non_utf8_strings);
    CHECK(same_names(full, s));
    CHECK(full.transforms.size() == s.transforms.size());
    CHECK(full.partitions.size() == s.partitions.size());
    CHECK(full.lights.size() == s.lights.size());
    CHECK(s.tabs.empty() && s.properties.empty() && s.node_tabs.empty() && s.tab_begin.empty());
    CHECK(full.node_transforms == s.node_transforms && full.node_flags.size() == s.node_flags.size());
    if (sk) CHECK(sk->locations.size() == s.nodes.size());
    if (fails) return fails;
    for (uint32_t i = 0; i < full.transforms.size(); ++i)
        if (!same_transform(transform_at(full, i), transform_at(s, i))) {
            std::printf("  transform %u differs\n", static_cast<unsigned>(i));
            return fails + 1;
        }
    for (size_t i = 0; i < full.partitions.size(); ++i) {
        const PartitionTail& a = full.partitions[i];
        const PartitionTail& b = s.partitions[i];
        CHECK(a.light_refs == b.light_refs && a.loader == b.loader && a.source_path == b.source_path &&
              a.unit_code == b.unit_code && a.format_version == b.format_version && a.label == b.label);
    }
    size_t bad = 0;
    for (uint32_t i = 0; i < full.nodes.size(); ++i) {
        const StoredNode& a = full.nodes[i];
        const StoredNode& b = s.nodes[i];
        NodeLocation loc;
        const bool tabs = !sk || (sk->location(i, loc) && loc.tab_count == full.tab_count(i));
        const bool same = a.cls == b.cls && same_text(full.strings, a.name, s.strings, b.name) &&
                          a.user == b.user && full.flags(i) == s.flags(i) && a.element == b.element &&
                          tabs && a.child_begin == b.child_begin &&
                          full.child_count(i) == s.child_count(i) &&
                          same_text(full.strings, a.material, s.strings, b.material) &&
                          full.node_transform(i) == s.node_transform(i) && a.expanded == b.expanded &&
                          a.height == b.height && full.partition(i) == s.partition(i);
        if (!same && bad++ < 5)
            std::printf("  node %u differs between Full and Structure\n", static_cast<unsigned>(i));
    }
    CHECK(bad == 0);
    return fails;
}

// Två tolkningar i läget Full: exakt samma innehåll.
int same_full(const Hierarchy& a, const Hierarchy& b) {
    int fails = 0;
    CHECK(a.strings.count() == b.strings.count());
    if (a.strings.count() == b.strings.count()) {
        size_t bad = 0;
        for (size_t i = 0; i < a.strings.count(); ++i)
            bad += a.strings.get(static_cast<uint32_t>(i)) != b.strings.get(static_cast<uint32_t>(i));
        CHECK(bad == 0);
    }
    CHECK(same_names(a, b));
    CHECK(a.nodes.size() == b.nodes.size() && a.tabs.size() == b.tabs.size() &&
          a.properties.size() == b.properties.size() && a.transforms.size() == b.transforms.size());
    CHECK(a.node_tabs == b.node_tabs && a.tab_begin == b.tab_begin && a.children == b.children &&
          a.top == b.top);
    CHECK(a.node_transforms == b.node_transforms);
    CHECK(a.root_counts == b.root_counts && a.expanded_total == b.expanded_total &&
          a.null_slots == b.null_slots && a.non_utf8_strings == b.non_utf8_strings);
    CHECK(a.partitions.size() == b.partitions.size() && a.lights.size() == b.lights.size());
    if (fails) return fails;
    size_t bad = 0;
    for (uint32_t i = 0; i < a.nodes.size(); ++i) {
        const StoredNode& x = a.nodes[i];
        const StoredNode& y = b.nodes[i];
        bad += !(x.cls == y.cls && x.name == y.name && x.user == y.user && a.flags(i) == b.flags(i) &&
                 x.element == y.element && x.child_begin == y.child_begin &&
                 x.material == y.material && x.expanded == y.expanded && x.height == y.height &&
                 a.partition(i) == b.partition(i));
    }
    for (size_t i = 0; i < a.tabs.size(); ++i) {
        const StoredTab& x = a.tabs[i];
        const StoredTab& y = b.tabs[i];
        bad += !(x.name == y.name && x.user == y.user && x.flags == y.flags &&
                 x.prop_begin == y.prop_begin && x.prop_count == y.prop_count &&
                 x.source_class == y.source_class);
    }
    for (size_t i = 0; i < a.properties.size(); ++i) {
        const StoredProperty& x = a.properties[i];
        const StoredProperty& y = b.properties[i];
        bad += !(x.key == y.key && x.type == y.type && x.d[0] == y.d[0] && x.d[1] == y.d[1] &&
                 x.d[2] == y.d[2] && x.i == y.i && x.text == y.text);
    }
    for (uint32_t i = 0; i < a.transforms.size(); ++i)
        bad += !same_transform(transform_at(a, i), transform_at(b, i));
    CHECK(bad == 0);
    return fails;
}

// Skelettets form: en plats per nod, en rot per null-plats, sorterade id per ström.
int check_skeleton(const Hierarchy& h, const HierarchySkeleton& sk, size_t streams) {
    int fails = 0;
    CHECK(sk.locations.size() == h.nodes.size());
    CHECK(sk.roots.size() == h.null_slots);
    CHECK(sk.first_root.size() == h.root_counts.size());
    CHECK(sk.checkpoints.size() == 1 + streams);
    CHECK(sk.refs.stream_count() == 1 + streams);
    // Varje ström i stigande id, och varje post hittas med find(). Ett namnpar
    // pekar in i names; en flik i trädet in i listan över flikarnas positioner.
    // En rot pekas aldrig bakåt på, så egenskapsströmmarna har inga flikar.
    size_t listed = 0;
    for (uint32_t t = 0; t < sk.refs.stream_count(); ++t) {
        std::vector<IdRef> list;
        CHECK(sk.refs.list(t, list));
        listed += list.size();
        for (size_t i = 0; i < list.size(); ++i) {
            if (i > 0) CHECK(list[i - 1].id < list[i].id);
            int32_t cls = 0;
            uint32_t value = 0;
            CHECK(sk.refs.find(t, list[i].id, cls, value) && cls == list[i].cls && value == list[i].value);
            const bool tab = cls == 86 || cls == 66 || cls == 70 || cls == 76;
            if (cls == 52) CHECK(value < h.names.size());
            if (tab) CHECK(t == 0);
        }
        // Ett id som inte är en post hittas inte.
        int32_t cls = 0;
        uint32_t value = 0;
        if (!list.empty()) CHECK(!sk.refs.find(t, list.back().id + 1, cls, value));
        CHECK(!sk.refs.find(t, 99, cls, value));
    }
    CHECK(listed == sk.refs.size());
    // Null-platserna står i stigande ordning i trädet.
    for (size_t i = 1; i < sk.roots.size(); ++i) CHECK(sk.roots[i - 1].slot < sk.roots[i].slot);
    return fails;
}

// Ett exempel ur filen: trädet och strömmarna.
struct Sample {
    MappedFile file;
    Container container;
    HierarchyStreams streams;
};

bool load_sample(const char* path, Sample& s) {
    std::string detail;
    if (s.file.open(path) != ErrorCode::Ok ||
        read_container(s.file.data(), s.file.size(), s.container, &detail) != ErrorCode::Ok)
        return false;
    const DbCacheEntry* tree = find_entry(s.container.entries, "LcOpNwdLogicalHierarchy");
    const DbCacheEntry* props = find_entry(s.container.entries, "LcOaPartitionProps");
    if (!tree) return false;
    const ErrorCode e = locate_hierarchy(s.file.data(), s.file.size(), *tree, props,
                                         kDefaultMaxStreamBytes, s.streams, &detail);
    if (e != ErrorCode::Ok) std::printf("  locate: %s %s\n", error_message(e), detail.c_str());
    return e == ErrorCode::Ok;
}

// Den gamla vägen: allt uppackat i minnet.
ErrorCode parse_sample_buffer(const Sample& s, Hierarchy& h, std::string& detail) {
    std::vector<uint8_t> tree;
    ErrorCode e = inflate_span(s.file.data(), s.file.size(), s.streams.tree, kDefaultMaxStreamBytes,
                               tree, &detail);
    if (e != ErrorCode::Ok) return e;
    Streams props;
    for (const StreamSpan& span : s.streams.props) {
        props.emplace_back();
        e = inflate_span(s.file.data(), s.file.size(), span, kDefaultMaxStreamBytes, props.back(),
                         &detail);
        if (e != ErrorCode::Ok) return e;
    }
    return parse_hierarchy(tree.data(), tree.size(), s.streams.has_props ? &props : nullptr,
                           UINT32_MAX, kBig, h, &detail);
}

double seconds_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

} // namespace

int test_hierarchy() {
    int fails = 0;

    // StringPool.
    {
        StringPool pool;
        CHECK(pool.count() == 0);
        uint32_t a = kNone, b = kNone, c = kNone;
        CHECK(pool.add("hello", a) && pool.add("", b) && pool.add(std::string_view("a\0b", 3), c));
        CHECK(a == 0 && b == 1 && c == 2 && pool.count() == 3);
        CHECK(pool.get(a) == "hello" && pool.get(b).empty() && pool.get(c).size() == 3);
        CHECK(pool.get(kNone).empty() && pool.get(99).empty());
        // Utan dedupe får en upprepad sträng ett nytt index, med dedupe samma.
        uint32_t d = kNone, e = kNone, f = kNone;
        CHECK(pool.add("hello", d) && d == 3);
        pool.dedupe(true);
        CHECK(pool.add("hello", e) && e == 0);
        CHECK(pool.add(std::string_view("a\0b", 3), f) && f == 2);
        CHECK(pool.add("ny", f) && f == 4 && pool.add("ny", e) && e == 4 && pool.count() == 5);
        pool.dedupe(false);
        CHECK(pool.add("ny", e) && e == 5);
        // Får nästan ingen sträng en dubblett slås dedupe av efter provet.
        StringPool unique;
        unique.dedupe(true);
        uint32_t k = kNone;
        bool added = true;
        for (uint32_t i = 0; i < StringPool::kDedupeSample + 1; ++i)
            added = added && unique.add(std::to_string(i), k);
        CHECK(added && unique.add("0", k) && k == StringPool::kDedupeSample + 1);
        // Dubblettabellen: lika hashar för olika poster ger aldrig en sökning
        // längre än kMaxProbe. Tabellen ger upp, och tiden förblir linjär.
        {
            DedupeTable t;
            const auto t0 = std::chrono::steady_clock::now();
            uint32_t found = 0;
            for (uint32_t i = 0; i < 200000; ++i) {
                if (t.find(12345, [](uint32_t) { return false; }) != kNone) ++found;
                t.insert(12345, i);
            }
            const double s = seconds_since(t0);
            CHECK(found == 0 && t.gave_up() && t.memory_bytes() == 0);
            CHECK(s < 1.0);
            t.clear();
            t.insert(12345, 7);
            CHECK(!t.gave_up() && t.find(12345, [](uint32_t v) { return v == 7; }) == 7);
            // Hashar som skiljer sig bara i de höga bitarna sprids över platserna.
            DedupeTable high;
            for (uint32_t i = 0; i < 100000; ++i) high.insert(static_cast<size_t>(i) << 40, i);
            CHECK(!high.gave_up() &&
                  high.find(static_cast<size_t>(77) << 40, [](uint32_t v) { return v == 77; }) == 77);
        }
        // Rader och bakåtpekare packade.
        PackedRows rows;
        const uint32_t values[][3] = {{0, 0, 0}, {5, 100, 2}, {3, 90, 0}, {UINT32_MAX, 7, 1}, {1u << 31, 1, 0}};
        for (int i = 0; i < 40; ++i) rows.push(values[i % 5]);
        bool same = rows.size() == 40;
        for (int i = 0; i < 40 && same; ++i) {
            uint32_t row[3] = {};
            same = rows.get(static_cast<size_t>(i), row) && row[0] == values[i % 5][0] &&
                   row[1] == values[i % 5][1] && row[2] == values[i % 5][2];
        }
        uint32_t row[3] = {};
        CHECK(same && !rows.get(40, row));
        PackedRefs refs;
        std::vector<IdRef> list;
        for (int32_t i = 0; i < 70; ++i)
            list.push_back(IdRef{100 + 3 * i, i % 7 == 0 ? 52 : i % 11 == 0 ? 999 : 86,
                                 static_cast<uint32_t>(i % 5 == 0 ? 1000 - i : 40 * i)});
        refs.add_stream(list);
        refs.add_stream({});
        std::vector<IdRef> back;
        CHECK(refs.stream_count() == 2 && refs.size() == 70 && refs.list(0, back) && back.size() == 70);
        same = back.size() == list.size();
        for (size_t i = 0; i < back.size() && same; ++i) {
            int32_t cls = 0;
            uint32_t value = 0;
            same = back[i].id == list[i].id && back[i].cls == list[i].cls && back[i].value == list[i].value &&
                   refs.find(0, list[i].id, cls, value) && cls == list[i].cls && value == list[i].value;
        }
        int32_t cls = 0;
        uint32_t value = 0;
        CHECK(same && !refs.find(0, 101, cls, value) && !refs.find(1, 100, cls, value) &&
              !refs.find(2, 100, cls, value) && refs.list(1, back) && back.empty());
    }

    // Grafen med alla attributklasser och värdetyper, två egenskapsströmmar.
    {
        const Full f = full_graph();
        Hierarchy h;
        std::string detail;
        CHECK(parse(f.tree, &f.props, 3, kBig, h, &detail) == ErrorCode::Ok);
        CHECK(detail.empty());
        CHECK(h.root_counts.size() == 2 && h.root_counts[0] == 3 && h.root_counts[1] == 2);
        CHECK(h.null_slots == 5);
        CHECK(h.expanded_total == 3);   // roten, toppnoden och dess barn
        CHECK(h.non_utf8_strings == 0);
        CHECK(h.nodes.size() == 3);     // den länkade noden räknas, men hänger inte i trädet
        CHECK(h.top.size() == 1);
        CHECK(h.transforms.size() == 3);

        if (h.nodes.size() == 3 && h.top.size() == 1 && h.transforms.size() == 3 &&
            h.tabs.size() == 5) {
            const StoredNode& model = h.nodes[h.top[0]];
            CHECK(model.cls == 53);
            CHECK(text(h, model.name) == "Model");
            CHECK(model.user < h.names.size());
            if (model.user < h.names.size()) {
                CHECK(text(h, h.names[model.user].display) == "Model Display");
                CHECK(text(h, h.names[model.user].internal) == "LcModel");
            }
            const uint32_t mi = h.top[0];
            CHECK(h.flags(mi) == 5 && model.element == 1);
            CHECK(h.child_count(mi) == 1 && model.expanded == 2 && model.height == 2);
            CHECK(h.tab_count(mi) == 3);
            CHECK(text(h, model.material) == "Mat");
            CHECK(h.node_transform(mi) < h.transforms.size());
            if (h.node_transform(mi) < h.transforms.size()) {
                const StoredTransform t = transform_at(h, h.node_transform(mi));
                CHECK(t.source_class == 40 && t.t[0] == 1 && t.t[1] == 2 && t.t[2] == 3);
            }

            const uint32_t ci = h.children[model.child_begin];
            const StoredNode& child = h.nodes[ci];
            CHECK(child.cls == 22 && text(h, child.name) == "Leaf");
            CHECK(child.user == model.user);   // bakåtpekare till samma namnpar
            CHECK(child.element == 2 && child.expanded == 1 && child.height == 1);
            CHECK(h.child_count(ci) == 0 && h.flags(ci) == 0 && h.partition(ci) == kNone);
            CHECK(h.tab_count(ci) == 3);
            CHECK(text(h, child.material) == "Mat");
            CHECK(h.node_transform(ci) < h.transforms.size());
            if (h.node_transform(ci) < h.transforms.size()) {
                const StoredTransform t = transform_at(h, h.node_transform(ci));
                CHECK(t.source_class == 41 && t.t[0] == 4 && t.t[1] == 5 && t.t[2] == 6);
                CHECK(t.q[0] == 0 && t.q[1] == 0 && t.q[2] == 0 && t.q[3] == 1);
            }
            const StoredTransform t2 = transform_at(h, 2);
            CHECK(t2.source_class == 39 && h.transform_class(2) == 39 && h.transform_class(3) == 0);
            CHECK(t2.m[0] == 1 && t2.m[8] == 9);
            CHECK(t2.t[0] == 7 && t2.t[2] == 9);
            CHECK(t2.reverses == 1);

            // Flikarna i den ordning attributplatserna står: egen flik, ström 0:s flik och
            // element-id (länken tar ingen flik).
            if (h.tab_count(mi) == 3) {
                const uint32_t tb = h.tab_begin[mi];
                const StoredTab& item = h.tabs[h.node_tabs[tb]];
                CHECK(item.source_class == 86 && text(h, item.name) == "Item");
                CHECK(text(h, h.names[item.user].internal) == "LcItem");
                CHECK(item.flags == 2 && item.prop_count == 14);
                const StoredTab& t0 = h.tabs[h.node_tabs[tb + 1]];
                CHECK(t0.source_class == 86 && text(h, t0.name) == "T0" && t0.prop_count == 1);
                CHECK(h.properties[t0.prop_begin].type == 2 && h.properties[t0.prop_begin].i == 5);
                const StoredTab& id = h.tabs[h.node_tabs[tb + 2]];
                CHECK(id.source_class == 66 && id.name == kNone && id.flags == 0x01000000);
                CHECK(id.prop_count == 1 && h.properties[id.prop_begin].type == 2);
                CHECK(h.properties[id.prop_begin].i == 12345);
                CHECK(text(h, h.names[id.user].internal) == "LcRevitId");
                CHECK(h.properties[id.prop_begin].key == id.user);

                // Egenskaperna i fliken Item, en av varje värdetyp.
                if (item.prop_count == 14) {
                    const StoredProperty* p = &h.properties[item.prop_begin];
                    CHECK(p[0].type == 0 && p[0].text == kNone);
                    CHECK(text(h, h.names[p[0].key].display) == "key0");
                    CHECK(p[1].type == 1 && p[1].d[0] == 2.5);
                    CHECK(p[2].type == 2 && p[2].i == -7 && p[2].key == item.user);
                    CHECK(p[3].type == 3 && p[3].i == 9 && p[3].key == p[0].key);
                    CHECK(p[4].type == 4 && text(h, p[4].text) == "hello");
                    CHECK(text(h, h.names[p[4].key].internal) == "lc_key4");
                    CHECK(p[5].type == 5 && p[5].i == 1700000000);
                    CHECK(p[6].type == 6 && p[6].d[0] == 6.5);
                    CHECK(p[7].type == 7 && p[7].d[0] == 7.5);
                    CHECK(p[8].type == 8 && p[8].text < h.names.size() &&
                          text(h, h.names[p[8].text].display) == "Named");
                    CHECK(p[9].type == 9 && p[9].text == kNone);
                    CHECK(p[10].type == 10 && p[10].d[0] == 10.5);
                    CHECK(p[11].type == 11 && p[11].d[0] == 11.5);
                    CHECK(p[12].type == 12 && p[12].d[0] == 1 && p[12].d[1] == 2 && p[12].d[2] == 3);
                    CHECK(p[13].type == 13 && p[13].d[0] == 4 && p[13].d[1] == 5);
                }
            }
            if (h.tab_count(ci) == 3) {
                const uint32_t cb = h.tab_begin[ci];
                CHECK(h.node_tabs[cb] == h.node_tabs[h.tab_begin[mi]]);   // delad flik
                const StoredTab& s1 = h.tabs[h.node_tabs[cb + 1]];
                const StoredTab& s2 = h.tabs[h.node_tabs[cb + 2]];
                CHECK(text(h, s1.name) == "S1" && s1.prop_count == 0);
                CHECK(text(h, s2.name) == "S2" && s2.prop_count == 2);
                CHECK(h.properties[s2.prop_begin + 1].i == 2);
            }

            // Den länkade noden ligger lagrad men är ingens barn.
            size_t linked = 0;
            for (const StoredNode& n : h.nodes) linked += text(h, n.name) == "Linked";
            CHECK(linked == 1);
        }

        fails += check_all_cuts(f, 3);
    }

    // Det delade delträdet: root, A, L1, L2, B, A, L1, L2.
    {
        ByteWriter w;
        header(w, {0}, 2);
        // A: grupp med ett flytt-attribut och två löv; L2 delar attributet.
        const int32_t a = node_head(w, 53, "A", 0, 1);
        const int32_t xf = w.obj(40);
        w.str("t");
        w.i32(0);
        w.i32(0);
        w.f64(1);
        w.f64(2);
        w.f64(3);
        w.i32(2);
        node_head(w, 22, "L1", 0, 0);
        node_head(w, 22, "L2", 0, 1);
        w.ref(xf);
        // B: grupp med A som barn genom en bakåtpekare.
        group(w, "B", 1);
        w.ref(a);
        finish(w);

        Hierarchy h;
        std::string detail;
        CHECK(parse(w.data, nullptr, 0, kBig, h, &detail) == ErrorCode::Ok);
        CHECK(h.expanded_total == 8);
        CHECK(h.nodes.size() == 4);   // A, L1, L2 och B; B delar A
        CHECK(h.top.size() == 2);
        if (h.top.size() == 2) {
            const StoredNode& na = h.nodes[h.top[0]];
            const StoredNode& nb = h.nodes[h.top[1]];
            CHECK(text(h, na.name) == "A" && na.expanded == 3 && na.height == 2);
            CHECK(text(h, nb.name) == "B" && nb.expanded == 4 && nb.height == 3);
            CHECK(h.child_count(h.top[0]) == 2 && h.child_count(h.top[1]) == 1);
            CHECK(h.children[nb.child_begin] == h.top[0]);   // B pekar på A
            CHECK(text(h, h.nodes[h.children[na.child_begin]].name) == "L1");
            CHECK(text(h, h.nodes[h.children[na.child_begin + 1]].name) == "L2");
            CHECK(h.node_transform(h.top[0]) < h.transforms.size());
            // Bakåtpekaren ger samma transform.
            CHECK(h.node_transform(h.children[na.child_begin + 1]) == h.node_transform(h.top[0]));
            // Noderna står i den ordning tolkningen når barnlistorna: A, L1, L2, B.
            CHECK(h.top[0] == 0 && h.top[1] == 3 && na.child_begin == 0 && nb.child_begin == 2);
        }
        // En ström som slutar mitt i ett objekt.
        for (size_t len = 0; len < w.data.size(); ++len) {
            Bytes cut(w.data.begin(), w.data.begin() + static_cast<std::ptrdiff_t>(len));
            CHECK(fails_with(cut, ErrorCode::CorruptObject));
        }
        // Exakt `max_nodes` går; en mindre gräns ger LimitExceeded.
        CHECK(parse(w.data, nullptr, 0, 8, h) == ErrorCode::Ok);
        CHECK(fails_with(w.data, nullptr, 0, 7, ErrorCode::LimitExceeded));
        CHECK(fails_with(w.data, nullptr, 0, 0, ErrorCode::LimitExceeded));
        CHECK(parse(w.data, nullptr, 0, 8, h) == ErrorCode::Ok);
    }

    // Gränserna: delade delträd som växer exponentiellt, höjd och nästling.
    {
        // N0 är ett löv i en grupp; N_k har N_{k-1} som båda barnen: 2^(k+1) - 1 noder.
        auto doubling = [](int levels) {
            ByteWriter w;
            header(w, {0}, levels + 1);
            int32_t previous = group(w, "N0", 0);
            for (int k = 1; k <= levels; ++k) {
                const int32_t id = group(w, "N", 2);
                w.ref(previous);
                w.ref(previous);
                previous = id;
            }
            finish(w);
            return w.data;
        };
        CHECK(fails_with(doubling(40), ErrorCode::LimitExceeded));
        CHECK(fails_with(doubling(24), ErrorCode::LimitExceeded));
        Hierarchy h;
        CHECK(parse(doubling(20), nullptr, 0, kBig, h) == ErrorCode::Ok);
        CHECK(h.nodes.size() == 21);
        // Räknat utan att slå runt: en nod rymmer 2^32 - 1 expanderade noder,
        // nästa nivå ger LimitExceeded, och så även långt bortom 64 bitar.
        CHECK(fails_with(doubling(70), nullptr, 0, std::numeric_limits<uint64_t>::max(),
                         ErrorCode::LimitExceeded));
        CHECK(fails_with(doubling(32), nullptr, 0, std::numeric_limits<uint64_t>::max(),
                         ErrorCode::LimitExceeded));
        CHECK(parse(doubling(31), nullptr, 0, std::numeric_limits<uint64_t>::max(), h) ==
              ErrorCode::Ok);
        CHECK(h.nodes.size() == 32);
        CHECK(!h.top.empty() && h.nodes[h.top.back()].expanded == UINT32_MAX);

        // G_k har G_{k-1} som enda barn genom en bakåtpekare: höjden växer utan nästling.
        auto tall = [](int height) {
            ByteWriter w;
            header(w, {0}, height);
            int32_t previous = group(w, "G", 0);
            for (int k = 2; k <= height; ++k) {
                const int32_t id = group(w, "G", 1);
                w.ref(previous);
                previous = id;
            }
            finish(w);
            return w.data;
        };
        CHECK(parse(tall(255), nullptr, 0, kBig, h) == ErrorCode::Ok);
        CHECK(!h.top.empty() && h.nodes[h.top.back()].height == 255);
        CHECK(fails_with(tall(256), ErrorCode::LimitExceeded));   // roten gör 257
        CHECK(fails_with(tall(300), ErrorCode::LimitExceeded));

        // Rent nästlade noder.
        auto nested = [](int depth) {
            ByteWriter w;
            header(w, {0}, 1);
            for (int i = 0; i < depth; ++i) group(w, "n", i + 1 < depth ? 1 : 0);
            finish(w);
            return w.data;
        };
        CHECK(parse(nested(250), nullptr, 0, kBig, h) == ErrorCode::Ok);
        CHECK(h.expanded_total == 251 && !h.top.empty() && h.nodes[h.top[0]].height == 250);
        CHECK(fails_with(nested(300), ErrorCode::LimitExceeded));
        CHECK(fails_with(nested(100000), ErrorCode::LimitExceeded));
    }

    // Huvudet och svansen.
    {
        ByteWriter k0;
        header(k0, {}, 0);
        finish(k0);
        CHECK(fails_with(k0.data, ErrorCode::CorruptObject));

        ByteWriter neg;
        neg.i32(-1);
        neg.implicit_root();
        neg.i32(0);
        finish(neg);
        CHECK(fails_with(neg.data, ErrorCode::CorruptObject));

        ByteWriter big;
        big.i32(1000000);   // k som inte ryms
        big.i32(0);
        big.pad(8);
        CHECK(fails_with(big.data, ErrorCode::CorruptObject));

        ByteWriter negative_count;
        negative_count.i32(1);
        negative_count.i32(-1);   // ett negativt rotantal
        negative_count.implicit_root();
        negative_count.i32(0);
        finish(negative_count);
        CHECK(fails_with(negative_count.data, ErrorCode::CorruptObject));

        ByteWriter wrong_root;
        wrong_root.i32(1);
        wrong_root.i32(0);
        wrong_root.i32(101);   // den underförstådda roten ska ha id 100
        wrong_root.i32(0);
        finish(wrong_root);
        CHECK(fails_with(wrong_root.data, ErrorCode::CorruptObject));

        ByteWriter tops;
        header(tops, {0}, 1000);   // fler toppnoder än som ryms
        finish(tops);
        CHECK(fails_with(tops.data, ErrorCode::CorruptObject));

        for (const int32_t tail : {0, 2, -1})
            CHECK(fails_with(tree_of({0}, [](ByteWriter& w) { group(w, "G", 0); }, tail),
                             ErrorCode::CorruptObject));

        // Byte kvar efter svansen, och en utfyllnad som inte är noll.
        Bytes ok = tree_of({0}, [](ByteWriter& w) { group(w, "G", 0); });
        Hierarchy h;
        CHECK(parse(ok, nullptr, 0, kBig, h) == ErrorCode::Ok);
        Bytes more = ok;
        for (int i = 0; i < 8; ++i) more.push_back(0);
        CHECK(fails_with(more, ErrorCode::CorruptObject));
        Bytes dirty = ok;
        dirty.push_back(1);
        CHECK(fails_with(dirty, ErrorCode::CorruptObject));
        if (ok.size() % 8 == 4) {
            Bytes bad = ok;
            bad[bad.size() - 1] = 1;   // utfyllnaden
            CHECK(fails_with(bad, ErrorCode::CorruptObject));
        }
        // Utan toppnoder är trädet bara roten.
        ByteWriter empty;
        header(empty, {0}, 0);
        finish(empty);
        CHECK(parse(empty.data, nullptr, 0, kBig, h) == ErrorCode::Ok);
        CHECK(h.expanded_total == 1 && h.nodes.empty() && h.top.empty());
    }

    // Klass 32 läses bara som toppnod (test_partition prövar kroppen och svansen).
    {
        // Efter pekaren ska en int32 0 stå; här står trädets sista tal.
        Bytes only = tree_of({1}, [](ByteWriter& w) { w.obj(32); });
        Hierarchy h;
        std::string detail;
        CHECK(parse(only, nullptr, 0, kBig, h, &detail) == ErrorCode::CorruptObject);
        CHECK(detail.find("class-32 body does not start with 0") != std::string::npos);
        CHECK(h.nodes.empty());
        // Med egenskapsströmmar och rötter i huvudet kommer felet ändå först.
        const Streams props = {tabs_stream(1)};
        CHECK(fails_with(only, &props, 0, kBig, ErrorCode::CorruptObject));

        // I en länk (79) i trädet: inte avkodad där.
        Bytes in_link = tree_of({0}, [](ByteWriter& v) {
            node_head(v, 53, "G", 0, 1);
            v.obj(79);
            v.str("L");
            v.i32(0);
            v.i32(0);
            v.i32(1);
            v.str("n");
            v.str("u");
            v.obj(32);
        });
        CHECK(fails_with(in_link, ErrorCode::Unsupported));
        CHECK(parse(in_link, nullptr, 0, kBig, h, &detail) == ErrorCode::Unsupported);
        CHECK(detail.find("partition node (class 32) is only decoded at the top level or under a partition") !=
              std::string::npos);

        // Som barn är en klass 32 ingen tillåten klass.
        Bytes as_child = tree_of({0}, [](ByteWriter& v) {
            group(v, "G", 1);
            v.obj(32);
        });
        CHECK(fails_with(as_child, ErrorCode::CorruptObject));
    }

    // Värdetyper som inte finns, och elementindex utanför GUID-tabellen.
    {
        for (const int32_t type : {14, 15, 99, -1, 0x7FFFFFFF}) {
            Bytes b = tree_of({0}, [&](ByteWriter& w) {
                node_head(w, 53, "G", 0, 1);
                w.obj(86);
                w.str("T");
                name_pair(w, "T");
                w.i32(0);
                w.i32(1);
                name_pair(w, "k");
                w.i32(type);
                w.i32(0);
                w.i32(0);   // c
            });
            CHECK(fails_with(b, ErrorCode::CorruptObject));
        }
        // Typ 8 med en null-pekare, och en nyckel som inte är klass 52.
        Bytes null8 = tree_of({0}, [](ByteWriter& w) {
            node_head(w, 53, "G", 0, 1);
            w.obj(86);
            w.str("T");
            name_pair(w, "T");
            w.i32(0);
            w.i32(1);
            name_pair(w, "k");
            w.i32(8);
            w.null_ptr();
            w.i32(0);
        });
        CHECK(fails_with(null8, ErrorCode::CorruptObject));
        Bytes bad_key = tree_of({0}, [](ByteWriter& w) {
            node_head(w, 53, "G", 0, 1);
            w.obj(86);
            w.str("T");
            name_pair(w, "T");
            w.i32(0);
            w.i32(1);
            w.obj(31);   // nyckeln ska vara klass 52
            w.str("m");
            for (int i = 0; i < 15; ++i) w.f64(0);
            w.i32(0);
            w.i32(0);
            w.i32(0);
        });
        CHECK(fails_with(bad_key, ErrorCode::CorruptObject));
        // Antalet egenskaper som inte ryms.
        Bytes many = tree_of({0}, [](ByteWriter& w) {
            node_head(w, 53, "G", 0, 1);
            w.obj(86);
            w.str("T");
            name_pair(w, "T");
            w.i32(0);
            w.i32(1000000);
            w.i32(0);
        });
        CHECK(fails_with(many, ErrorCode::CorruptObject));

        // X: 0 .. antalet GUID är giltigt.
        auto with_x = [](int32_t x) {
            return tree_of({0}, [&](ByteWriter& w) {
                node_head(w, 53, "G", x, 0);
                w.i32(0);
            });
        };
        Hierarchy h;
        CHECK(parse(with_x(0), nullptr, 2, kBig, h) == ErrorCode::Ok);
        CHECK(parse(with_x(2), nullptr, 2, kBig, h) == ErrorCode::Ok);
        CHECK(!h.nodes.empty() && h.nodes[0].element == 2);
        CHECK(fails_with(with_x(3), nullptr, 2, kBig, ErrorCode::CorruptObject));
        CHECK(fails_with(with_x(1), nullptr, 0, kBig, ErrorCode::CorruptObject));
        CHECK(fails_with(with_x(-1), nullptr, 5, kBig, ErrorCode::CorruptObject));
        CHECK(fails_with(with_x(INT32_MIN), nullptr, UINT32_MAX, kBig, ErrorCode::CorruptObject));
        CHECK(parse(with_x(INT32_MAX), nullptr, UINT32_MAX, kBig, h) == ErrorCode::Ok);
    }

    // Pekare som inte får vara null, och fel klass på varje platstyp.
    {
        // Toppnod: null, klass 22, klass 52, en bakåtpekare till okänt id.
        for (const int variant : {0, 1, 2, 3}) {
            ByteWriter w;
            header(w, {0}, 1);
            if (variant == 0) w.null_ptr();
            if (variant == 1) leaf(w, "L");
            if (variant == 2) name_pair(w, "N");
            if (variant == 3) w.ref(500);
            finish(w);
            CHECK(fails_with(w.data, ErrorCode::CorruptObject));
        }
        // Barn: null, klass 86, klass 52 (via bakåtpekare), noden själv (ofärdig), okänt id.
        for (const int variant : {0, 1, 2, 3, 4}) {
            ByteWriter w;
            header(w, {0}, 1);
            const int32_t self = node_head(w, 53, "G", 0, 0);
            w.i32(1);
            if (variant == 0) w.null_ptr();
            if (variant == 1) tab_with_ints(w, "T", {});
            if (variant == 2) w.ref(self + 1);   // namnparet: klass 52
            if (variant == 3) w.ref(self);       // en cykel
            if (variant == 4) w.ref(777);
            finish(w);
            CHECK(fails_with(w.data, ErrorCode::CorruptObject));
        }
        // Namnparet: null, fel klass, och i en flik.
        {
            Bytes null_user = tree_of({0}, [](ByteWriter& w) {
                node_head(w, 53, "G", 0, 1);
                w.obj(86);
                w.str("T");
                w.null_ptr();   // en flik har alltid ett namnpar
                w.i32(0);
                w.i32(0);
                w.i32(0);
            });
            CHECK(fails_with(null_user, ErrorCode::CorruptObject));

            Bytes wrong_user = tree_of({0}, [](ByteWriter& w) {
                w.obj(53);
                w.str("G");
                w.obj(31);   // namnparet ska vara klass 52
                w.str("m");
                for (int i = 0; i < 15; ++i) w.f64(0);
                w.i32(0);
                w.i32(0);
                w.i32(0);
                w.i32(0);
            });
            CHECK(fails_with(wrong_user, ErrorCode::CorruptObject));

            // En nod får sakna namnpar och namn.
            Bytes unnamed = tree_of({0}, [](ByteWriter& w) {
                w.obj(53);
                w.null_str();
                w.null_ptr();
                w.i32(0);
                w.i32(0);
                w.i32(0);
                w.i32(0);
            });
            Hierarchy h;
            CHECK(parse(unnamed, nullptr, 0, kBig, h) == ErrorCode::Ok);
            CHECK(h.nodes.size() == 1 && h.nodes[0].name == kNone && h.nodes[0].user == kNone);
            CHECK(h.names.empty());
        }
        // Attribut: klass 22, 53, 52 och en bakåtpekare till ett barn.
        for (const int32_t cls : {22, 53, 52, 66, 185}) {
            Bytes b = tree_of({0}, [&](ByteWriter& w) {
                node_head(w, 53, "G", 0, 1);
                w.obj(cls);
                w.i32(0);
            });
            CHECK(fails_with(b, ErrorCode::CorruptObject));
        }
        // Klass 200: pekaren är en klass 185 och aldrig null, och aldrig en annan klass.
        auto with_200 = [](int variant) {
            return tree_of({0}, [&](ByteWriter& w) {
                node_head(w, 53, "G", 0, 1);
                w.obj(200);
                w.str("e");
                w.i32(0);
                w.i32(0);
                if (variant == 0) w.null_ptr();
                if (variant == 1) tab_with_ints(w, "T", {});
                if (variant == 2) {
                    w.obj(185);
                    class185(w);
                }
                w.i32(0);
            });
        };
        CHECK(fails_with(with_200(0), ErrorCode::CorruptObject));
        CHECK(fails_with(with_200(1), ErrorCode::CorruptObject));
        Hierarchy h;
        CHECK(parse(with_200(2), nullptr, 0, kBig, h) == ErrorCode::Ok);

        // Klass 185 med ett flaggvärde som inte är 0 eller 1, och en fil som inte ryms.
        for (const int variant : {0, 1}) {
            Bytes b = tree_of({0}, [&](ByteWriter& w) {
                node_head(w, 53, "G", 0, 1);
                w.obj(200);
                w.str("e");
                w.i32(0);
                w.i32(0);
                w.obj(185);
                w.str("a");
                w.str("b");
                w.i32(1);
                w.str("c");
                w.i32(variant == 0 ? 2 : 1);
                w.str("d");
                if (variant == 1) {
                    w.str("f");
                    w.i64a(1000);   // 1000 byte ryms inte
                }
                w.i32(0);
            });
            CHECK(fails_with(b, ErrorCode::CorruptObject));
        }

        // Länkens pekare i trädet: klass 66 är inte tillåten, en bakåtpekare till klass 52 är det.
        auto with_link = [](int variant) {
            return tree_of({0}, [&](ByteWriter& w) {
                const int32_t g = node_head(w, 53, "G", 0, 1);
                w.obj(79);
                w.str("L");
                w.i32(0);
                w.i32(0);
                w.i32(1);
                w.str("n");
                w.str("u");
                if (variant == 0) w.ref(g + 1);           // klass 52: tillåtet
                if (variant == 1) tab_with_ints(w, "T", {});   // klass 86: tillåtet
                if (variant == 2) {                       // klass 66: inte tillåtet
                    w.obj(66);
                    w.null_str();
                    name_pair(w, "Id");
                    w.i32(0x01000000);
                    w.i64a(1);
                }
                if (variant == 3) w.ref(g);               // noden själv, ofärdig
                if (variant == 4) w.ref(999);
                w.i32(0);
                w.i32(0);   // c
            });
        };
        CHECK(parse(with_link(0), nullptr, 0, kBig, h) == ErrorCode::Ok);
        CHECK(parse(with_link(1), nullptr, 0, kBig, h) == ErrorCode::Ok);
        CHECK(h.tabs.size() == 1 && h.node_tabs.empty());   // länkens flik hör inte till noden
        CHECK(fails_with(with_link(2), ErrorCode::CorruptObject));
        CHECK(fails_with(with_link(3), ErrorCode::CorruptObject));
        CHECK(fails_with(with_link(4), ErrorCode::CorruptObject));
        // Punktantalet i en länk som inte ryms.
        Bytes points = tree_of({0}, [](ByteWriter& w) {
            node_head(w, 53, "G", 0, 1);
            w.obj(79);
            w.str("L");
            w.i32(0);
            w.i32(0);
            w.i32(1);
            w.str("n");
            w.str("u");
            w.null_ptr();
            w.i32(5000);
            w.i32(0);
        });
        CHECK(fails_with(points, ErrorCode::CorruptObject));
        // Och antalet länkar.
        Bytes links = tree_of({0}, [](ByteWriter& w) {
            node_head(w, 53, "G", 0, 1);
            w.obj(79);
            w.str("L");
            w.i32(0);
            w.i32(0);
            w.i32(100000);
            w.i32(0);
        });
        CHECK(fails_with(links, ErrorCode::CorruptObject));
        // Antalet attribut och antalet barn.
        Bytes attrs = tree_of({0}, [](ByteWriter& w) {
            node_head(w, 53, "G", 0, 100000);
            w.i32(0);
        });
        CHECK(fails_with(attrs, ErrorCode::CorruptObject));
        Bytes kids = tree_of({0}, [](ByteWriter& w) { group(w, "G", 100000); });
        CHECK(fails_with(kids, ErrorCode::CorruptObject));
        Bytes neg_attrs = tree_of({0}, [](ByteWriter& w) {
            node_head(w, 53, "G", 0, -1);
            w.i32(0);
        });
        CHECK(fails_with(neg_attrs, ErrorCode::CorruptObject));
    }

    // Egenskapsrötterna: koppling mellan null-platser, rötter och strömmar.
    {
        Hierarchy h;
        const Bytes one_slot = tree_of({1}, [](ByteWriter& w) { node_with_nulls(w, 1); });
        const Streams one = {tabs_stream(1)};
        CHECK(parse(one_slot, &one, 0, kBig, h) == ErrorCode::Ok);
        CHECK(h.null_slots == 1 && h.tabs.size() == 1 && h.node_tabs.size() == 1);

        // En null-plats utan rot kvar.
        CHECK(fails_with(tree_of({1}, [](ByteWriter& w) { node_with_nulls(w, 2); }), &one, 0, kBig,
                         ErrorCode::CorruptLink));
        const Streams empty_stream = {Bytes{}};
        CHECK(fails_with(tree_of({0}, [](ByteWriter& w) { node_with_nulls(w, 1); }),
                         &empty_stream, 0, kBig, ErrorCode::CorruptLink));
        // Rötter som blir över.
        const Streams two = {tabs_stream(2)};
        CHECK(fails_with(tree_of({2}, [](ByteWriter& w) { node_with_nulls(w, 1); }), &two, 0, kBig,
                         ErrorCode::CorruptLink));
        CHECK(fails_with(tree_of({1}, [](ByteWriter& w) { group(w, "G", 0); }), &one, 0, kBig,
                         ErrorCode::CorruptLink));
        // Fel antal strömmar: för få, för många, och ingen alls.
        const Streams short_list = {tabs_stream(1)};
        CHECK(fails_with(tree_of({1, 1}, [](ByteWriter& w) { node_with_nulls(w, 2); }), &short_list,
                         0, kBig, ErrorCode::CorruptLink));
        const Streams long_list = {tabs_stream(1), tabs_stream(1)};
        CHECK(fails_with(tree_of({1}, [](ByteWriter& w) { node_with_nulls(w, 1); }), &long_list, 0,
                         kBig, ErrorCode::CorruptLink));
        const Streams none;
        CHECK(fails_with(tree_of({1}, [](ByteWriter& w) { node_with_nulls(w, 1); }), &none, 0, kBig,
                         ErrorCode::CorruptLink));
        CHECK(fails_with(tree_of({0}, [](ByteWriter& w) { group(w, "G", 0); }), &none, 0, kBig,
                         ErrorCode::CorruptLink));
        // Rötter i huvudet men inget paket, och en null-plats utan paket.
        CHECK(fails_with(one_slot, nullptr, 0, kBig, ErrorCode::CorruptLink));
        CHECK(fails_with(tree_of({1}, [](ByteWriter& w) { group(w, "G", 0); }), nullptr, 0, kBig,
                         ErrorCode::CorruptLink));
        CHECK(fails_with(tree_of({0}, [](ByteWriter& w) { node_with_nulls(w, 1); }), nullptr, 0,
                         kBig, ErrorCode::CorruptLink));
        // Utan rötter behövs inget paket.
        CHECK(parse(tree_of({0}, [](ByteWriter& w) { group(w, "G", 0); }), nullptr, 0, kBig, h) ==
              ErrorCode::Ok);
        const Streams empty_one = {Bytes{}};
        CHECK(parse(tree_of({0}, [](ByteWriter& w) { group(w, "G", 0); }), &empty_one, 0, kBig, h) ==
              ErrorCode::Ok);

        // Strömmar utan rötter i mitten och sist: rötterna fördelas på de andra.
        const Streams spread = {tabs_stream(1), Bytes{}, tabs_stream(1), Bytes{}};
        CHECK(parse(tree_of({1, 0, 1, 0}, [](ByteWriter& w) { node_with_nulls(w, 2); }), &spread, 0,
                    kBig, h) == ErrorCode::Ok);
        CHECK(h.null_slots == 2 && h.root_counts.size() == 4);
        // En ström med noll rötter ska vara tom.
        const Streams padded = {tabs_stream(1), Bytes(8, 0)};
        CHECK(fails_with(tree_of({1, 0}, [](ByteWriter& w) { node_with_nulls(w, 1); }), &padded, 0,
                         kBig, ErrorCode::CorruptObject, "LcOaPartitionProps stream 1 at byte "));

        // En delad nod läses en gång och tar alltså sina rötter en gång.
        Bytes shared = tree_of({1}, [](ByteWriter& w) {
            group(w, "G", 2);
            const int32_t x = node_head(w, 22, "X", 0, 1);
            w.null_ptr();
            w.ref(x);
        });
        CHECK(parse(shared, &one, 0, kBig, h) == ErrorCode::Ok);
        CHECK(h.null_slots == 1 && h.nodes.size() == 2 && h.expanded_total == 4);
        CHECK(h.child_count(h.top[0]) == 2 &&
              h.children[h.nodes[h.top[0]].child_begin] ==
                  h.children[h.nodes[h.top[0]].child_begin + 1]);
        // Rötterna tas i byteordning över noder: två noder, en null-plats var.
        Bytes two_nodes = tree_of({1, 1}, [](ByteWriter& w) {
            group(w, "G", 2);
            node_head(w, 22, "X", 0, 1);
            w.null_ptr();
            node_head(w, 22, "Y", 0, 1);
            w.null_ptr();
        });
        const Streams ordered = {stream_of([](ByteWriter& s) { tab_with_ints(s, "first", {1}); }),
                                 stream_of([](ByteWriter& s) { tab_with_ints(s, "second", {2}); })};
        CHECK(parse(two_nodes, &ordered, 0, kBig, h) == ErrorCode::Ok);
        if (h.nodes.size() == 3) {
            const StoredNode& x = h.nodes[1];
            const StoredNode& y = h.nodes[2];
            CHECK(text(h, x.name) == "X" && text(h, y.name) == "Y");
            CHECK(h.tab_count(1) == 1 && h.tab_count(2) == 1);
            CHECK(text(h, h.tabs[h.node_tabs[h.tab_begin[1]]].name) == "first");
            CHECK(text(h, h.tabs[h.node_tabs[h.tab_begin[2]]].name) == "second");
        }
    }

    // Fel i egenskapsströmmarna: rotens klass, klass 66, utfyllnad, antal rötter.
    {
        Hierarchy h;
        const Bytes slot = tree_of({1}, [](ByteWriter& w) { node_with_nulls(w, 1); });

        auto fails_in_stream = [&](const Bytes& stream, ErrorCode code = ErrorCode::CorruptObject) {
            const Streams s = {stream};
            return fails_with(slot, &s, 0, kBig, code, kStream0Name);
        };
        // Rötter som inte är nya objekt av klass 86, 79 eller 66.
        CHECK(fails_in_stream(stream_of([](ByteWriter& w) { w.null_ptr(); })));
        CHECK(fails_in_stream(stream_of([](ByteWriter& w) { name_pair(w, "N"); })));
        CHECK(fails_in_stream(stream_of([](ByteWriter& w) { leaf(w, "L"); })));
        CHECK(fails_in_stream(stream_of([](ByteWriter& w) { w.ref(100); })));
        // Klass 66: rätt värden går, fel värde och namn gör det inte.
        for (const int64_t v : {int64_t(1), int64_t(INT32_MAX), int64_t(1234567)}) {
            const Streams s = {root66(true, 0x01000000, v)};
            CHECK(parse(slot, &s, 0, kBig, h) == ErrorCode::Ok);
            CHECK(h.properties.size() == 1 && h.properties[0].i == v);
        }
        // Flaggorna varierar mellan filer utan att layouten ändras: de lagras.
        for (const int32_t flags : {0, 0x00020000, 0x01000000}) {
            const Streams s = {root66(true, flags, 5)};
            CHECK(parse(slot, &s, 0, kBig, h) == ErrorCode::Ok);
            CHECK(h.tabs.size() == 1 && h.tabs[0].flags == flags && h.tabs[0].source_class == 66);
        }
        CHECK(fails_in_stream(root66(true, 0x01000000, 0)));
        CHECK(fails_in_stream(root66(true, 0x01000000, -1)));
        CHECK(fails_in_stream(root66(true, 0x01000000, int64_t(INT32_MAX) + 1)));
        CHECK(fails_in_stream(root66(true, 0x01000000, INT64_MIN)));
        CHECK(fails_in_stream(root66(false, 0x01000000, 5)));
        // Länkens pekare i en ström tar bara klass 52 eller null.
        CHECK(fails_in_stream(stream_of([](ByteWriter& w) {
            w.obj(79);
            w.str("L");
            w.i32(0);
            w.i32(0);
            w.i32(1);
            w.str("n");
            w.str("u");
            tab_with_ints(w, "T", {});
            w.i32(0);
        })));
        CHECK(fails_in_stream(stream_of([](ByteWriter& w) {
            w.obj(79);
            w.str("L");
            w.i32(0);
            w.i32(0);
            w.i32(1);
            w.str("n");
            w.str("u");
            leaf(w, "x");
            w.i32(0);
        })));
        // En flik med en okänd värdetyp i en ström.
        CHECK(fails_in_stream(stream_of([](ByteWriter& w) {
            w.obj(86);
            w.str("T");
            name_pair(w, "T");
            w.i32(0);
            w.i32(1);
            name_pair(w, "k");
            w.i32(14);
        })));
        // Utfyllnad som inte är noll, byte efter sista roten, och en saknad rot.
        CHECK(fails_in_stream(Bytes([] {
            Bytes b = tabs_stream(1);
            b.push_back(1);
            return b;
        }())));
        CHECK(fails_in_stream(Bytes([] {
            Bytes b = tabs_stream(1);
            b.insert(b.end(), 8, 0);
            return b;
        }())));
        CHECK(fails_in_stream(Bytes{}));
        {
            ByteWriter w;
            tab_with_ints(w, "T", {1});
            if (w.data.size() % 8 != 0) {
                Bytes b = w.data;
                b.resize((b.size() + 7) & ~size_t(7), 0);
                b.back() = 1;   // utfyllnaden är inte noll
                CHECK(fails_in_stream(b));
            }
        }
        // Två rötter i strömmen men bara en null-plats.
        const Streams extra = {tabs_stream(2)};
        CHECK(fails_with(slot, &extra, 0, kBig, ErrorCode::CorruptObject, kStream0Name));
        // Två null-platser men bara en rot i strömmen.
        const Bytes two_slots = tree_of({2}, [](ByteWriter& w) { node_with_nulls(w, 2); });
        const Streams one = {tabs_stream(1)};
        CHECK(fails_with(two_slots, &one, 0, kBig, ErrorCode::CorruptObject, kStream0Name));
        // Ett fel i ström 1 anger ström 1.
        const Bytes in_second = tree_of({1, 1}, [](ByteWriter& w) { node_with_nulls(w, 2); });
        const Streams second = {tabs_stream(1), stream_of([](ByteWriter& w) { w.null_ptr(); })};
        CHECK(fails_with(in_second, &second, 0, kBig, ErrorCode::CorruptObject,
                         "LcOaPartitionProps stream 1 at byte "));
    }

    // Strängar som inte är giltig UTF-8 är inget fel, men räknas ur trädet och ur strömmarna.
    {
        const Bytes tree = tree_of({1}, [](ByteWriter& w) {
            node_head(w, 53, "\xFF", 0, 1);   // namn och visningsnamn
            w.null_ptr();
            w.i32(0);
        });
        const Streams props = {stream_of([](ByteWriter& w) {
            w.obj(86);
            w.str("\xC3\x28");
            name_pair(w, "\xC3\x28", "ok");
            w.i32(0);
            w.i32(0);
        })};
        Hierarchy h;
        std::string detail;
        CHECK(parse(tree, &props, 0, kBig, h, &detail) == ErrorCode::Ok);
        CHECK(h.non_utf8_strings == 4);
        CHECK(h.nodes.size() == 1 && text(h, h.nodes[0].name) == "\xFF");
        CHECK(text(h, h.tabs[0].name) == "\xC3\x28");
    }

    // read_partition_props.
    {
        const Streams streams = {testdata::pattern(50), testdata::pattern(700)};
        const std::vector<int32_t> good = {zsize(streams[0]), zsize(streams[1])};

        // Rätt.
        {
            const PropsPackage p = package(0, streams, ztoc(2, good));
            Streams out = {Bytes{9}};
            std::string detail = "old";
            CHECK(read_partition_props(p.file.data(), p.file.size(), p.entry, 1 << 20, out,
                                       &detail) == ErrorCode::Ok);
            CHECK(detail.empty());
            CHECK(out == streams);
            CHECK(read_partition_props(p.file.data(), p.file.size(), p.entry, 1 << 20, out) ==
                  ErrorCode::Ok);
        }
        // Utan strömmar: bara innehållsförteckningen.
        {
            const PropsPackage p = package(0, {}, ztoc(0, {}));
            Streams out = {Bytes{9}};
            CHECK(read_partition_props(p.file.data(), p.file.size(), p.entry, 1 << 20, out) ==
                  ErrorCode::Ok);
            CHECK(out.empty());
        }
        // Prefixet ska vara 0.
        for (const int32_t prefix : {1, 7, 65536, -1})
            CHECK(props_fail(package(prefix, streams, ztoc(2, good)), ErrorCode::CorruptPackage));
        // Innehållsförteckningen av fel längd: för lång, för kort, utan längder, tom.
        {
            Bytes longer = ztoc(2, good);
            longer.insert(longer.end(), 8, 0);
            CHECK(props_fail(package(0, streams, longer), ErrorCode::CorruptPackage));
            CHECK(props_fail(package(0, streams, ztoc(2, {good[0]})), ErrorCode::CorruptPackage));
            Bytes four = ztoc(2, good);
            four.resize(12);   // utan utfyllnaden
            CHECK(props_fail(package(0, streams, four), ErrorCode::CorruptPackage));
            CHECK(props_fail(package(0, streams, Bytes{1, 0}), ErrorCode::CorruptPackage));
            CHECK(props_fail(package(0, streams, Bytes{}), ErrorCode::CorruptPackage));
        }
        // k negativt, och k som skulle kräva en förteckning större än den är.
        CHECK(props_fail(package(0, streams, ztoc(-1, {})), ErrorCode::CorruptPackage));
        CHECK(props_fail(package(0, streams, ztoc(INT32_MIN, {})), ErrorCode::CorruptPackage));
        CHECK(props_fail(package(0, streams, ztoc(INT32_MAX, good)), ErrorCode::CorruptPackage));
        // k som inte stämmer med längderna: för få, för många, summan fel.
        CHECK(props_fail(package(0, streams, ztoc(1, {good[0]})), ErrorCode::CorruptPackage));
        CHECK(props_fail(package(0, streams, ztoc(3, {good[0], good[1], 5})), ErrorCode::CorruptPackage));
        CHECK(props_fail(package(0, streams, ztoc(2, {good[0], good[1] + 1})), ErrorCode::CorruptPackage));
        CHECK(props_fail(package(0, streams, ztoc(2, {good[0] - 1, good[1]})), ErrorCode::CorruptPackage));
        // En längd som är 0 eller negativ.
        CHECK(props_fail(package(0, streams, ztoc(3, {good[0], good[1], 0})), ErrorCode::CorruptPackage));
        CHECK(props_fail(package(0, streams, ztoc(3, {good[0], good[1] + 5, -5})), ErrorCode::CorruptPackage));
        CHECK(props_fail(package(0, streams, ztoc(2, {0, good[0] + good[1]})), ErrorCode::CorruptPackage));
        // Gränserna sitter fel fast summan stämmer: strömmen tar inte slut där förteckningen säger.
        CHECK(props_fail(package(0, streams, ztoc(2, {good[0] + 10, good[1] - 10})), ErrorCode::CorruptStream));
        // Utfyllnaden i förteckningen är inte noll.
        {
            Bytes dirty = ztoc(2, good);
            dirty[12] = 1;
            CHECK(props_fail(package(0, streams, dirty), ErrorCode::CorruptPackage));
        }
        // Formen (0, 0), och ett paket utanför filen.
        {
            PropsPackage p = package(0, streams, ztoc(2, good));
            p.entry.a = 0;
            p.entry.b = 0;
            CHECK(props_fail(p, ErrorCode::CorruptPackage));
            p = package(0, streams, ztoc(2, good));
            p.entry.offset = p.file.size();
            CHECK(props_fail(p, ErrorCode::CorruptPackage));
        }
        // Strömmar över taket och trasiga strömmar lämnas vidare som de är.
        CHECK(props_fail(package(0, streams, ztoc(2, good)), ErrorCode::StreamTooLarge, 100));
        {
            PropsPackage p = package(0, streams, ztoc(2, good));
            p.file[100 + 4 + 2 + 5 + 3] ^= 0xFF;   // en byte i den första strömmens data
            CHECK(props_fail(p, ErrorCode::CorruptStream));
            p = package(0, streams, ztoc(2, good));
            p.file[p.entry.offset + p.entry.length - 3] ^= 0xFF;   // innehållsförteckningen
            CHECK(props_fail(p, ErrorCode::CorruptStream));
        }

        // Från paketet till trädet: strömmarna ur en syntetisk fil tolkas av parse_hierarchy.
        {
            const Full f = full_graph();
            std::vector<int32_t> lengths;
            for (const Bytes& s : f.props) lengths.push_back(zsize(s));
            const PropsPackage p = package(0, f.props, ztoc(2, lengths));
            Streams out;
            CHECK(read_partition_props(p.file.data(), p.file.size(), p.entry, 1 << 20, out) ==
                  ErrorCode::Ok);
            CHECK(out == f.props);
            Hierarchy h;
            CHECK(parse(f.tree, &out, 3, kBig, h) == ErrorCode::Ok);
            CHECK(h.null_slots == 5);
        }

        // locate_partition_props: samma strömmar, utan att någon packas upp.
        {
            const PropsPackage p = package(0, streams, ztoc(2, good));
            std::vector<StreamSpan> spans = {StreamSpan{}};
            std::string detail = "old";
            CHECK(locate_partition_props(p.file.data(), p.file.size(), p.entry, 1 << 20, spans,
                                         &detail) == ErrorCode::Ok);
            CHECK(detail.empty() && spans.size() == 2);
            for (size_t i = 0; i < spans.size() && i < streams.size(); ++i) {
                Bytes out;
                CHECK(inflate_span(p.file.data(), p.file.size(), spans[i], 1 << 20, out) == ErrorCode::Ok);
                CHECK(out == streams[i]);
            }
            // Felen i ramen är desamma; strömmarna packas inte upp, så taket gäller bara ZTOC.
            CHECK(locate_partition_props(p.file.data(), p.file.size(), p.entry, 100, spans) ==
                  ErrorCode::Ok);
            const PropsPackage bad = package(1, streams, ztoc(2, good));
            CHECK(locate_partition_props(bad.file.data(), bad.file.size(), bad.entry, 1 << 20, spans,
                                         &detail) == ErrorCode::CorruptPackage);
            CHECK(spans.empty() && detail.rfind("LcOaPartitionProps at byte ", 0) == 0);
        }
    }

    // Trädet ur filen, i läget Full: samma resultat som ur bufferten.
    {
        const Full f = full_graph();
        Hierarchy buffer;
        CHECK(parse(f.tree, &f.props, 3, kBig, buffer) == ErrorCode::Ok);
        for (const size_t block : {size_t(7), size_t(64), size_t(65535)}) {
            const TreeFile tf = tree_file(f.tree, &f.props, block);
            Hierarchy h;
            HierarchySkeleton sk;
            sk.locations.push({1, 2, 3});   // ska tömmas
            std::string detail = "old";
            CHECK(parse_file(tf, HierarchyContent::Full, h, &sk, &detail) == ErrorCode::Ok);
            CHECK(detail.empty());
            fails += same_full(buffer, h);
            CHECK(sk.locations.empty() && sk.refs.empty() && sk.checkpoints.empty());
        }
    }

    // Läget Structure: samma struktur som Full, inga flikar, och ett skelett.
    {
        const Full f = full_graph();
        Hierarchy full;
        CHECK(parse(f.tree, &f.props, 3, kBig, full) == ErrorCode::Ok);
        for (const uint64_t every : {uint64_t(1), uint64_t(100), kCheckpointEvery}) {
            const TreeFile tf = tree_file(f.tree, &f.props, 16);
            Hierarchy h;
            HierarchySkeleton sk;
            std::string detail;
            CHECK(parse_file(tf, HierarchyContent::Structure, h, &sk, &detail, every) == ErrorCode::Ok);
            fails += same_structure(full, h, &sk);
            fails += check_skeleton(h, sk, 2);
            NodeLocation top_loc;
            if (h.nodes.size() == 3 && sk.location(h.top[0], top_loc)) {
                CHECK(top_loc.tab_count == 3);   // egen flik, ström 0:s flik och element-id
                CHECK(text(h, h.nodes[h.top[0]].material) == "Mat");
            }
            // Kontrollpunkter i varje ström när de ligger tätt; ingen när de ligger glest.
            if (sk.checkpoints.size() == 3) {
                if (every == 1)
                    CHECK(!sk.checkpoints[0].empty() && !sk.checkpoints[1].empty());
                if (every == kCheckpointEvery)
                    CHECK(sk.checkpoints[0].empty() && sk.checkpoints[1].empty() &&
                          sk.checkpoints[2].empty());
                for (const auto& list : sk.checkpoints)
                    for (size_t i = 1; i < list.size(); ++i) CHECK(list[i - 1].out_pos < list[i].out_pos);
            }
            // Rötterna: ström 0 har tre, ström 1 två.
            CHECK(sk.first_root.size() == 2 && sk.first_root[0] == 0 && sk.first_root[1] == 3);
            CHECK(sk.memory_bytes() >= sk.checkpoint_bytes());
        }
    }

    // Strömmarnas fel genom fönstret: avhuggna och trasiga strömmar, spann som inte går ihop.
    {
        const Full f = full_graph();
        const TreeFile good = tree_file(f.tree, &f.props);
        for (const HierarchyContent content : {HierarchyContent::Full, HierarchyContent::Structure}) {
            Hierarchy h;
            HierarchySkeleton sk;
            std::string detail;
            // Trädet avhugget på varje längd (en hel zlib-ström av det som är kvar).
            size_t bad = 0;
            for (size_t len = 0; len < f.tree.size(); ++len) {
                const TreeFile cut = tree_file(Bytes(f.tree.begin(), f.tree.begin() + static_cast<std::ptrdiff_t>(len)),
                                               &f.props);
                if (parse_file(cut, content, h, &sk, &detail) == ErrorCode::Ok || detail.empty() ||
                    !h.nodes.empty() || !sk.locations.empty())
                    ++bad;
            }
            CHECK(bad == 0);
            // En egenskapsström avhuggen: felet anger strömmen.
            for (size_t s = 0; s < f.props.size(); ++s) {
                Streams cut = f.props;
                cut[s].resize(cut[s].size() / 2);
                const TreeFile tf = tree_file(f.tree, &cut);
                CHECK(parse_file(tf, content, h, &sk, &detail) == ErrorCode::CorruptObject);
                CHECK(detail.rfind(std::string("LcOaPartitionProps stream ") + std::to_string(s), 0) == 0);
            }
            // En trasig zlib-ström: kontrollsumman.
            TreeFile broken = good;
            broken.file[good.streams.tree.offset + good.streams.tree.length - 1] ^= 0xFF;
            CHECK(parse_file(broken, content, h, &sk, &detail) == ErrorCode::CorruptStream);
            CHECK(detail.rfind(kTreeName, 0) == 0);
            // Ett trasigt zlib-huvud.
            broken = good;
            broken.file[good.streams.tree.offset] = 0x00;
            CHECK(parse_file(broken, content, h, &sk, &detail) == ErrorCode::CorruptStream);
            // Spannet är längre än strömmen.
            broken = good;
            broken.streams.tree.length += 1;
            CHECK(parse_file(broken, content, h, &sk, &detail) == ErrorCode::CorruptStream);
            CHECK(detail.find("the zlib stream ends at") != std::string::npos);
            broken = good;
            broken.streams.props[1].length += 1;
            CHECK(parse_file(broken, content, h, &sk, &detail) == ErrorCode::CorruptStream);
            CHECK(detail.rfind("LcOaPartitionProps stream 1 at byte ", 0) == 0);
            // Utanför filen.
            broken = good;
            broken.streams.tree.length = broken.file.size();
            CHECK(parse_file(broken, content, h, &sk, &detail) == ErrorCode::OutOfRange);
            // Fel antal strömmar och inget paket, som ur bufferten.
            broken = good;
            broken.streams.props.pop_back();
            CHECK(parse_file(broken, content, h, &sk, &detail) == ErrorCode::CorruptLink);
            broken = good;
            broken.streams.has_props = false;
            broken.streams.props.clear();
            CHECK(parse_file(broken, content, h, &sk, &detail) == ErrorCode::CorruptLink);
            // Taket för en ström.
            broken = good;
            broken.streams.max_stream = 64;
            CHECK(parse_file(broken, content, h, &sk, &detail) == ErrorCode::StreamTooLarge);
            CHECK(h.nodes.empty() && sk.locations.empty());
        }
    }

    // Förloppet: stigande, aldrig över totalen, sist totalen; false avbryter.
    {
        // Många noder, så att förloppet hinner anropas under tolkningen.
        ByteWriter w;
        header(w, {0}, 1);
        group(w, "G", 20000);
        for (int i = 0; i < 20000; ++i) leaf(w, "L");
        finish(w);
        const TreeFile tf = tree_file(w.data, nullptr, 4096);
        for (const HierarchyContent content : {HierarchyContent::Full, HierarchyContent::Structure}) {
            uint64_t calls = 0, last = 0, total = 0;
            bool rising = true;
            const HierarchyProgress count = [&](uint64_t done, uint64_t t) {
                rising = rising && done >= last && done <= t;
                last = done;
                total = t;
                ++calls;
                return true;
            };
            Hierarchy h;
            HierarchySkeleton sk;
            std::string detail;
            CHECK(parse_file(tf, content, h, &sk, &detail, kCheckpointEvery, 0, kBig, count) == ErrorCode::Ok);
            CHECK(calls >= 2 && rising && last == total && total == tf.streams.tree.length);
            CHECK(h.expanded_total == 20002);

            uint64_t stop_calls = 0;
            const HierarchyProgress stop = [&](uint64_t, uint64_t) { return ++stop_calls < 2; };
            CHECK(parse_file(tf, content, h, &sk, &detail, kCheckpointEvery, 0, kBig, stop) ==
                  ErrorCode::Cancelled);
            CHECK(detail.rfind(kTreeName, 0) == 0 && h.nodes.empty() && sk.locations.empty());
        }
    }

    // Djupet kostar ingen stack: 255 nästlade noder går, och i en länk i ett
    // attribut, en nod i en länk i en nod … så djupt objekten får nästlas.
    {
        auto nested = [](int depth) {
            ByteWriter w;
            header(w, {0}, 1);
            for (int i = 0; i < depth; ++i) group(w, "n", i + 1 < depth ? 1 : 0);
            finish(w);
            return w.data;
        };
        for (const int depth : {255, 256, 300}) {
            const Bytes tree = nested(depth);
            Hierarchy full;
            const ErrorCode e = parse(tree, nullptr, 0, kBig, full);
            CHECK((e == ErrorCode::Ok) == (depth == 255));
            CHECK(e == ErrorCode::Ok || e == ErrorCode::LimitExceeded);
            const TreeFile tf = tree_file(tree, nullptr, 100);
            for (const HierarchyContent content : {HierarchyContent::Full, HierarchyContent::Structure}) {
                Hierarchy h;
                HierarchySkeleton sk;
                CHECK(parse_file(tf, content, h, &sk, nullptr, 64, 0) == e);
                if (e == ErrorCode::Ok) {
                    CHECK(h.expanded_total == 256 && h.nodes[h.top[0]].height == 255);
                    if (content == HierarchyContent::Structure) fails += same_structure(full, h, &sk);
                }
            }
        }
        // En kedja av länkar: nod → attribut 79 → ny nod → attribut 79 → …
        auto chain = [](int links) {
            ByteWriter w;
            header(w, {0}, 1);
            for (int i = 0; i < links; ++i) {
                node_head(w, i == 0 ? 53 : 22, "c", 0, 1);
                w.obj(79);
                w.str("L");
                w.i32(0);
                w.i32(0);
                w.i32(1);
                w.str("n");
                w.str("u");
            }
            node_head(w, 22, "end", 0, 0);
            for (int i = 0; i < links; ++i) {
                w.i32(0);   // punktantalet
                if (i == links - 1) w.i32(0);   // toppnodens barn
            }
            finish(w);
            return w.data;
        };
        for (const int links : {60, 200}) {
            const Bytes tree = chain(links);
            Hierarchy full;
            const ErrorCode e = parse(tree, nullptr, 0, kBig, full);
            CHECK((e == ErrorCode::Ok) == (links == 60));
            const TreeFile tf = tree_file(tree, nullptr);
            Hierarchy h;
            HierarchySkeleton sk;
            CHECK(parse_file(tf, HierarchyContent::Structure, h, &sk, nullptr, kCheckpointEvery, 0) == e);
            if (e == ErrorCode::Ok) {
                fails += same_structure(full, h, &sk);
                CHECK(h.nodes.size() == static_cast<size_t>(links) + 1);
            }
        }
    }

    // locate_hierarchy ur en hel fil.
    {
        const Full f = full_graph();
        std::vector<int32_t> lengths;
        for (const Bytes& s : f.props) lengths.push_back(zsize(s));
        int32_t b = 0;
        const Bytes props_pkg = testdata::prefixed(0, f.props, ztoc(2, lengths), b);
        const std::vector<testdata::Package> packages = {
            {"x\\LcOpNwdLogicalHierarchy", 1, 0, 0, testdata::zlib_store(f.tree)},
            {"x\\LcOaPartitionProps", 1, 4, b, props_pkg},
        };
        const Bytes file = testdata::build_file(packages, true);
        Container c;
        CHECK(read_container(file.data(), file.size(), c) == ErrorCode::Ok);
        const DbCacheEntry* tree = find_entry(c.entries, "LcOpNwdLogicalHierarchy");
        const DbCacheEntry* props = find_entry(c.entries, "LcOaPartitionProps");
        CHECK(tree && props);
        if (tree && props) {
            HierarchyStreams s;
            std::string detail;
            CHECK(locate_hierarchy(file.data(), file.size(), *tree, props, kDefaultMaxStreamBytes, s,
                                   &detail) == ErrorCode::Ok);
            CHECK(s.has_props && s.props.size() == 2);
            Hierarchy h;
            CHECK(parse_hierarchy(file.data(), file.size(), s, 3, kBig, HierarchyContent::Full, h,
                                  nullptr, {}, &detail) == ErrorCode::Ok);
            CHECK(h.null_slots == 5 && h.nodes.size() == 3);
            CHECK(locate_hierarchy(file.data(), file.size(), *tree, nullptr, kDefaultMaxStreamBytes,
                                   s) == ErrorCode::Ok);
            CHECK(!s.has_props && s.props.empty());
            DbCacheEntry wrong = *tree;
            wrong.a = 4;
            CHECK(locate_hierarchy(file.data(), file.size(), wrong, props, kDefaultMaxStreamBytes, s,
                                   &detail) == ErrorCode::CorruptPackage);
            CHECK(detail.rfind("LcOpNwdLogicalHierarchy at byte ", 0) == 0);
            wrong = *tree;
            wrong.length = file.size();
            CHECK(locate_hierarchy(file.data(), file.size(), wrong, props, kDefaultMaxStreamBytes, s) ==
                  ErrorCode::OutOfRange);
            CHECK(locate_hierarchy(file.data(), file.size(), *tree, tree, kDefaultMaxStreamBytes, s) ==
                  ErrorCode::CorruptPackage);
        }
    }

    // Exempelfilerna: ur filen lika med ur bufferten, och skelettet lika med Full nod för nod.
    for (const testdata::SampleCounts& sc : testdata::kSamples) {
        SKIP_IF_MISSING(sc.path);
        Sample s;
        if (!load_sample(sc.path, s)) {
            std::printf("  FAIL — %s kan inte öppnas\n", sc.path);
            ++fails;
            continue;
        }
        std::string detail;
        Hierarchy buffer;
        auto t0 = std::chrono::steady_clock::now();
        CHECK(parse_sample_buffer(s, buffer, detail) == ErrorCode::Ok);
        const double t_buffer = seconds_since(t0);

        Hierarchy full;
        t0 = std::chrono::steady_clock::now();
        CHECK(parse_hierarchy(s.file.data(), s.file.size(), s.streams, UINT32_MAX, kBig,
                              HierarchyContent::Full, full, nullptr, {}, &detail) == ErrorCode::Ok);
        const double t_full = seconds_since(t0);
        fails += same_full(buffer, full);

        Hierarchy structure;
        HierarchySkeleton sk;
        t0 = std::chrono::steady_clock::now();
        CHECK(parse_hierarchy(s.file.data(), s.file.size(), s.streams, UINT32_MAX, kBig,
                              HierarchyContent::Structure, structure, &sk, {}, &detail) ==
              ErrorCode::Ok);
        const double t_structure = seconds_since(t0);
        fails += same_structure(buffer, structure, &sk);
        fails += check_skeleton(structure, sk, s.streams.props.size());
        CHECK(structure.null_slots == sc.property_roots);

        const size_t nodes = structure.nodes.size();
        std::printf("  %s: %zu nodes, tree %llu B; buffer %.3f s, file %.3f s, structure %.3f s; "
                    "skeleton %.1f B/node (%zu roots, %zu refs), checkpoints %zu in %zu B\n",
                    sc.path, nodes, static_cast<unsigned long long>(s.streams.tree.length), t_buffer,
                    t_full, t_structure,
                    nodes ? static_cast<double>(sk.memory_bytes() - sk.checkpoint_bytes()) / nodes : 0.0,
                    sk.roots.size(), sk.refs.size(),
                    [&] {
                        size_t n = 0;
                        for (const auto& l : sk.checkpoints) n += l.size();
                        return n;
                    }(),
                    sk.checkpoint_bytes());
    }

    std::printf("test_hierarchy: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
