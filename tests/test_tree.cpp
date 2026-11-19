// HNwd::Document: trädet. Syntetiska filer med träd, egenskapspaket, GUID-tabell,
// fragment och geometri, deras fel, och exempelfilerna.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "HNwd.h"
#include "path_utf8.h"
#include "test_data.h"
#include "test_support.h"

using HNwd::Document;
using HNwd::kNone;
using HNwd::Node;
using HNwd::NodeKind;
using HNwd::Status;
using HNwd::ValueType;
using testdata::Bytes;
using testdata::ByteWriter;

namespace {

using Streams = std::vector<Bytes>;

// Fragment i i dokumentet; ett tomt fragment utanför.
HNwd::Fragment fragment_at(const Document& doc, size_t i) {
    HNwd::Fragment f;
    doc.fragment(i, f);
    return f;
}

std::vector<HNwd::Fragment> all_fragments(const Document& doc) {
    std::vector<HNwd::Fragment> out(doc.fragment_count());
    for (size_t i = 0; i < out.size(); ++i) doc.fragment(i, out[i]);
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
    Opened(const char* name, const Bytes& bytes, const HNwd::Options& options = {})
        : file(name, bytes), status(doc.open(file.path.c_str(), options)) {}
};

bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// ---------------------------------------------------------------------------
// Geometriposterna: R1 (två trianglar) och L1 (ett linjesegment med färg).

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

// Geometripaketet med R1 och L1 i en chunk.
Bytes geometry_package(int32_t& b) {
    const Bytes a = r1(), c = l1();
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

// Ett fragment per seq, alla på post 1 utan pose.
Bytes fragment_stream(const std::vector<int32_t>& seqs) {
    ByteWriter w;
    w.i32(static_cast<int32_t>(seqs.size()));
    for (const int32_t seq : seqs) {
        w.obj(13);
        w.f64(1.5);
        w.i32(2);
        w.i32(4);
        w.i32(7);
        w.i32(seq);
        w.null_ptr();
        w.obj(55);
        w.i32(1);
        w.obj(54);
        const float mat[14] = {0.125f, 0.125f, 0.125f, 0.5f, 0.5f, 0.5f, 0.5f,
                               0.5f,   0.5f,   0,      0,    0,    32,   0.75f};
        for (const float v : mat) w.f32(v);
        w.obj(93);
        for (const float v : {0.f, 0.f, 0.f, 1.f, 1.f, 0.f}) w.f32(v);
        for (int i = 0; i < 3; ++i) w.f64(0.5);
        w.f32(2.5f);
        w.i32(7);
        w.i32(1);
    }
    w.pad(8);
    return w.data;
}

// ---------------------------------------------------------------------------
// Trädet. Preorder: 0 roten, 1 A, 2 L1, 3 L2, 4 B, 5 A, 6 L1, 7 L2. A är en
// toppnod och B:s enda barn, så delträdet A sitter på två ställen.

int32_t pair(ByteWriter& w, const char* display, const char* internal) {
    const int32_t id = w.obj(52);
    w.str(display);
    w.str(internal);
    return id;
}

void material(ByteWriter& w, const char* name) {
    w.obj(31);
    w.str(name);
    for (int i = 0; i < 15; ++i) w.f64(i);
}

// En flik med en egenskap av varje värdetyp 0..13.
void item_tab(ByteWriter& w) {
    w.obj(86);
    w.str("Item");
    const int32_t user = pair(w, "Item Display", "LcItem");
    w.i32(2);
    w.i32(14);
    const int32_t key = pair(w, "key0", "lc_key0");
    w.i32(0);                          // 0: inget
    w.ref(key);
    w.i32(1);
    w.f64(2.5);                        // 1
    w.ref(user);
    w.i32(2);
    w.i32(-7);                         // 2
    w.ref(key);
    w.i32(3);
    w.i32(9);                          // 3
    pair(w, "key4", "lc_key4");
    w.i32(4);
    w.str("hello");                    // 4
    w.ref(key);
    w.i32(5);
    w.i64a(1700000000);                // 5
    w.ref(key);
    w.i32(6);
    w.f64(6.5);                        // 6
    w.ref(key);
    w.i32(7);
    w.f64(7.5);                        // 7
    w.ref(key);
    w.i32(8);
    pair(w, "Named Display", "named"); // 8: ett namnpar
    w.ref(key);
    w.i32(9);
    w.str("nine");                     // 9
    w.ref(key);
    w.i32(10);
    w.f64(10.5);                       // 10
    w.ref(key);
    w.i32(11);
    w.f64(11.5);                       // 11
    w.ref(key);
    w.i32(12);
    w.f64(1);
    w.f64(2);
    w.f64(3);                          // 12
    w.ref(key);
    w.i32(13);
    w.f64(4);
    w.f64(5);                          // 13
}

Bytes standard_tree(int32_t tail = 1) {
    ByteWriter w;
    w.i32(1);   // en egenskapsström
    w.i32(2);   // med två rötter
    w.implicit_root();
    w.i32(2);   // två toppnoder

    // A: eget namn, namnpar, tre attributplatser (en flik och två null), två barn.
    const int32_t a = w.obj(53);
    w.str("A");
    pair(w, "A Display", "LcA");
    w.i32(0);
    w.i32(0);
    w.i32(3);
    item_tab(w);
    w.null_ptr();
    w.null_ptr();
    w.i32(2);
    // L1: ett löv utan namnpar, med eget material och element 1.
    w.obj(22);
    w.str("L1");
    w.null_ptr();
    w.i32(0);
    w.i32(1);
    w.i32(1);
    material(w, "Brick");
    // L2: ett löv utan eget namn, med namnpar och element 2.
    w.obj(22);
    w.null_str();
    pair(w, "L2 Display", "LcL2");
    w.i32(0);
    w.i32(2);
    w.i32(0);

    // B: med eget material, och A som enda barn.
    w.obj(53);
    w.str("B");
    pair(w, "B Display", "LcB");
    w.i32(0);
    w.i32(0);
    w.i32(1);
    material(w, "Wood");
    w.i32(1);
    w.ref(a);

    w.i32(tail);
    w.pad(8);
    return w.data;
}

// Egenskapsströmmen: en flik utan eget namn och ett element-id.
Streams standard_props() {
    ByteWriter s;
    s.obj(86);
    s.null_str();
    const int32_t user = pair(s, "P0 Display", "LcP0");
    s.i32(0);
    s.i32(1);
    s.ref(user);
    s.i32(3);
    s.i32(5);

    s.obj(66);
    s.null_str();
    pair(s, "Id", "LcRevitId");
    s.i32(0x01000000);
    s.i64a(12345);
    s.pad(8);
    return {s.data};
}

Bytes props_package(const Streams& streams, int32_t& b) {
    ByteWriter toc;
    toc.i32(static_cast<int32_t>(streams.size()));
    for (const Bytes& s : streams) toc.i32(static_cast<int32_t>(testdata::zlib_store(s).size()));
    toc.pad(8);
    return testdata::prefixed(0, streams, toc.data, b);
}

Bytes guid_table() {
    ByteWriter w;
    w.i32(1);
    w.i32(2);
    for (uint8_t i = 0; i < 32; ++i) w.u8(i);
    return w.data;
}

// ---------------------------------------------------------------------------
// Hela filer.

struct Spec {
    Bytes tree;                                  // tom: standard_tree()
    Streams props = standard_props();
    std::vector<int32_t> seqs = {2, 3, 6, 7};    // fragmentens seq: löven i trädet
    bool tree_package = true, props_package = true, guids = true, fragments = true;
    int32_t tree_flags = 1;
    int32_t guid_flags = 1;
    bool break_tree_stream = false;              // sista byten i trädets ström är fel
};

Bytes build(const Spec& sp) {
    using testdata::Package;
    using testdata::zlib_store;
    std::vector<Package> pk;
    if (sp.tree_package) {
        Bytes z = zlib_store(sp.tree.empty() ? standard_tree() : sp.tree);
        if (sp.break_tree_stream) z.back() ^= 0x55;
        pk.push_back({"cache\\LcOpNwdLogicalHierarchy", sp.tree_flags, 0, 0, z});
    }
    if (sp.fragments) pk.push_back({"cache\\LcOpNwdFragments", 1, 0, 0, zlib_store(fragment_stream(sp.seqs))});
    {
        int32_t b = 0;
        const Bytes g = geometry_package(b);
        pk.push_back({"cache\\LcOpNwdGeometry", 1, 4, b, g});
    }
    if (sp.guids) pk.push_back({"cache\\LcOpGuidStore", sp.guid_flags, 0, 0, zlib_store(guid_table())});
    if (sp.props_package) {
        int32_t b = 0;
        const Bytes p = props_package(sp.props, b);
        pk.push_back({"cache\\LcOaPartitionProps", 1, 4, b, p});
    }
    return testdata::build_file(pk, true);
}

// Ett träd utan GUID och egenskaper: en toppnod G med ett löv (ett litet
// träd), eller inga toppnoder alls.
Bytes small_tree(bool with_top) {
    ByteWriter w;
    w.i32(1);
    w.i32(0);
    w.implicit_root();
    if (!with_top) {
        w.i32(0);
    } else {
        w.i32(1);
        w.obj(53);
        w.str("G");
        w.null_ptr();
        w.i32(0);
        w.i32(0);
        w.i32(0);
        w.i32(1);
        w.obj(22);
        w.str("leaf");
        w.null_ptr();
        w.i32(0);
        w.i32(0);
        w.i32(0);
    }
    w.i32(1);
    w.pad(8);
    return w.data;
}

// ---------------------------------------------------------------------------
// Kontroller som gäller varje träd.

ValueType expected_type(int32_t raw) {
    switch (raw) {
    case 1: case 6: case 7: case 10: case 11: return ValueType::Double;
    case 2: case 3: return ValueType::Int;
    case 4: case 9: return ValueType::String;
    case 5: return ValueType::Time;
    case 8: return ValueType::Name;
    case 12: return ValueType::Point3;
    case 13: return ValueType::Point2;
    default: return ValueType::None;
    }
}

struct TreeStats {
    uint64_t nodes = 0, leaves = 0, groups = 0, partitions = 0;
    uint64_t tabs = 0, properties = 0;
    uint64_t unnamed = 0, with_material = 0;
    uint32_t top_nodes = 0;
};

// Går igenom hela trädet genom första barn och nästa syskon, och prövar att allt
// hänger ihop. `label` säger vilken fil.
int check_tree(const Document& doc, const char* label, TreeStats& st) {
    int fails = 0;
    const size_t count = doc.node_count();
    CHECK(doc.has_tree() && count >= 1);
    if (count < 1) return fails;
    const uint32_t n = static_cast<uint32_t>(count);

    Node root;
    CHECK(doc.node(0, root));
    CHECK(root.kind == NodeKind::Root && root.parent == kNone && root.subtree_size == n);
    CHECK(root.name.empty() && root.display.empty() && root.internal.empty() && root.material.empty());
    CHECK(root.element == 0);
    CHECK(doc.next_sibling(0) == kNone);
    CHECK(doc.tab_count(0) == 0);
    HNwd::Tab tab;
    HNwd::Property prop;
    CHECK(!doc.tab(0, 0, tab) && !doc.property(0, 0, 0, prop));
    st.top_nodes = root.child_count;

    // Roten har toppnoderna som barn, och deras delträd fyller resten.
    uint64_t sum = 1;
    for (uint32_t c = doc.first_child(0); c != kNone; c = doc.next_sibling(c)) {
        Node cn;
        if (!doc.node(c, cn)) {
            ++fails;
            break;
        }
        sum += cn.subtree_size;
        CHECK(cn.parent == 0);
    }
    CHECK(sum == n);
    CHECK((root.child_count == 0) == (doc.first_child(0) == kNone));

    uint64_t bad = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const int before = fails;
        Node node;
        if (!doc.node(i, node)) {
            ++fails;
            continue;
        }
        ++st.nodes;
        if (i > 0) {
            CHECK(node.parent < i);
            CHECK(node.kind != NodeKind::Root);
            CHECK(node.kind != NodeKind::Partition || node.parent == 0);
            CHECK(node.element >= 0 && static_cast<size_t>(node.element) <= doc.guid_count());
            if (node.name.empty()) ++st.unnamed;
            if (!node.material.empty()) ++st.with_material;
            switch (node.kind) {
            case NodeKind::Leaf: ++st.leaves; break;
            case NodeKind::Partition: ++st.partitions; break;
            default: ++st.groups; break;
            }
            Node parent;
            CHECK(doc.node(node.parent, parent));
            CHECK(parent.kind != NodeKind::Leaf);
        }
        CHECK(node.subtree_size >= 1 && static_cast<uint64_t>(i) + node.subtree_size <= n);
        if (node.kind == NodeKind::Leaf) CHECK(node.child_count == 0 && node.subtree_size == 1);

        // Barnen: första barn är i + 1, vart nästa börjar där det förra slutar, och
        // det sista slutar där delträdet slutar.
        uint32_t seen = 0;
        uint64_t expected = static_cast<uint64_t>(i) + 1;
        for (uint32_t c = doc.first_child(i); c != kNone; c = doc.next_sibling(c)) {
            if (c != expected || seen >= node.child_count) {
                ++fails;
                break;
            }
            Node cn;
            if (!doc.node(c, cn)) {
                ++fails;
                break;
            }
            CHECK(cn.parent == i);
            ++seen;
            expected = static_cast<uint64_t>(c) + cn.subtree_size;
        }
        CHECK(seen == node.child_count);
        CHECK(expected == static_cast<uint64_t>(i) + node.subtree_size);
        CHECK((node.child_count == 0) == (doc.first_child(i) == kNone));
        if (node.child_count > 0) CHECK(doc.first_child(i) == i + 1);

        // Flikarna och egenskaperna.
        const size_t tabs = doc.tab_count(i);
        for (size_t t = 0; t < tabs; ++t) {
            HNwd::Tab tb;
            if (!doc.tab(i, t, tb)) {
                ++fails;
                continue;
            }
            ++st.tabs;
            for (size_t p = 0; p < tb.property_count; ++p) {
                HNwd::Property pr;
                if (!doc.property(i, t, p, pr)) {
                    ++fails;
                    continue;
                }
                ++st.properties;
                CHECK(pr.type == expected_type(pr.raw_type));
                CHECK(pr.raw_type >= 0 && pr.raw_type <= 13);
            }
            CHECK(!doc.property(i, t, tb.property_count, prop));
        }
        CHECK(!doc.tab(i, tabs, tab));
        CHECK(!doc.property(i, tabs, 0, prop));

        if (fails != before && ++bad <= 3)
            std::printf("  FAIL — %s: node %u (parent %u, children %u, subtree %u)\n", label, i,
                        node.parent, node.child_count, node.subtree_size);
    }

    // Utanför trädet.
    Node tmp;
    CHECK(!doc.node(n, tmp) && !doc.node(kNone, tmp));
    CHECK(doc.first_child(n) == kNone && doc.first_child(kNone) == kNone);
    CHECK(doc.next_sibling(n) == kNone && doc.next_sibling(kNone) == kNone);
    CHECK(doc.tab_count(n) == 0 && doc.tab_count(kNone) == 0);
    CHECK(!doc.tab(n, 0, tab) && !doc.property(n, 0, 0, prop));
    return fails;
}

// ---------------------------------------------------------------------------
// Den syntetiska filen.

struct Expect {
    uint32_t parent, children, subtree;
    NodeKind kind;
    const char *name, *display, *internal, *material;
    int32_t element;
    uint32_t first_child, next_sibling;
};

const Expect kExpected[8] = {
    {kNone, 2, 8, NodeKind::Root, "", "", "", "", 0, 1, kNone},
    {0, 2, 3, NodeKind::Group, "A", "A Display", "LcA", "", 0, 2, 4},
    {1, 0, 1, NodeKind::Leaf, "L1", "", "", "Brick", 1, kNone, 3},
    {1, 0, 1, NodeKind::Leaf, "L2 Display", "L2 Display", "LcL2", "", 2, kNone, kNone},
    {0, 1, 4, NodeKind::Group, "B", "B Display", "LcB", "Wood", 0, 5, kNone},
    {4, 2, 3, NodeKind::Group, "A", "A Display", "LcA", "Wood", 0, 6, kNone},
    {5, 0, 1, NodeKind::Leaf, "L1", "", "", "Brick", 1, kNone, 7},
    {5, 0, 1, NodeKind::Leaf, "L2 Display", "L2 Display", "LcL2", "Wood", 2, kNone, kNone},
};

struct ExpectProp {
    const char *name, *internal;
    int32_t raw;
    ValueType type;
    double d0, d1, d2;
    int64_t i;
    const char* text;
};

const ExpectProp kItem[14] = {
    {"key0", "lc_key0", 0, ValueType::None, 0, 0, 0, 0, ""},
    {"key0", "lc_key0", 1, ValueType::Double, 2.5, 0, 0, 0, ""},
    {"Item Display", "LcItem", 2, ValueType::Int, 0, 0, 0, -7, ""},
    {"key0", "lc_key0", 3, ValueType::Int, 0, 0, 0, 9, ""},
    {"key4", "lc_key4", 4, ValueType::String, 0, 0, 0, 0, "hello"},
    {"key0", "lc_key0", 5, ValueType::Time, 0, 0, 0, 1700000000, ""},
    {"key0", "lc_key0", 6, ValueType::Double, 6.5, 0, 0, 0, ""},
    {"key0", "lc_key0", 7, ValueType::Double, 7.5, 0, 0, 0, ""},
    {"key0", "lc_key0", 8, ValueType::Name, 0, 0, 0, 0, "Named Display"},
    {"key0", "lc_key0", 9, ValueType::String, 0, 0, 0, 0, "nine"},
    {"key0", "lc_key0", 10, ValueType::Double, 10.5, 0, 0, 0, ""},
    {"key0", "lc_key0", 11, ValueType::Double, 11.5, 0, 0, 0, ""},
    {"key0", "lc_key0", 12, ValueType::Point3, 1, 2, 3, 0, ""},
    {"key0", "lc_key0", 13, ValueType::Point2, 4, 5, 0, 0, ""},
};

// Flikarna på ett A: fliken i trädet, flikens ur egenskapspaketet och elementets id.
int check_a_tabs(const Document& doc, uint32_t node) {
    int fails = 0;
    CHECK(doc.tab_count(node) == 3);
    HNwd::Tab t;
    CHECK(doc.tab(node, 0, t));
    CHECK(t.name == "Item" && t.internal == "LcItem" && t.property_count == 14);   // eget namn går före visningsnamnet
    for (size_t p = 0; p < 14; ++p) {
        HNwd::Property pr;
        CHECK(doc.property(node, 0, p, pr));
        const ExpectProp& e = kItem[p];
        CHECK(pr.name == e.name && pr.internal == e.internal);
        CHECK(pr.raw_type == e.raw && pr.type == e.type);
        CHECK(pr.d[0] == e.d0 && pr.d[1] == e.d1 && pr.d[2] == e.d2);
        CHECK(pr.i == e.i && pr.text == e.text);
    }
    HNwd::Property pr;
    CHECK(!doc.property(node, 0, 14, pr));

    CHECK(doc.tab(node, 1, t));   // utan eget namn: visningsnamnet
    CHECK(t.name == "P0 Display" && t.internal == "LcP0" && t.property_count == 1);
    CHECK(doc.property(node, 1, 0, pr));
    CHECK(pr.name == "P0 Display" && pr.internal == "LcP0" && pr.raw_type == 3 &&
          pr.type == ValueType::Int && pr.i == 5);

    CHECK(doc.tab(node, 2, t));   // elementets id
    CHECK(t.name == "Id" && t.internal == "LcRevitId" && t.property_count == 1);
    CHECK(doc.property(node, 2, 0, pr));
    CHECK(pr.name == "Id" && pr.internal == "LcRevitId" && pr.raw_type == 2 &&
          pr.type == ValueType::Int && pr.i == 12345);
    CHECK(!doc.tab(node, 3, t));
    return fails;
}

int test_standard() {
    int fails = 0;
    Opened o("hnwd_test_tree_standard.nwd", build(Spec{}));
    Document& doc = o.doc;
    CHECK(o.status == Status::Ok);
    if (!doc.is_open()) return fails + 1;

    // Före load_tree.
    Node node;
    HNwd::Tab tab;
    HNwd::Property prop;
    CHECK(!doc.has_tree() && doc.node_count() == 0);
    CHECK(!doc.node(0, node) && !doc.node(1, node));
    CHECK(doc.first_child(0) == kNone && doc.next_sibling(1) == kNone);
    CHECK(doc.tab_count(1) == 0 && !doc.tab(1, 0, tab) && !doc.property(1, 0, 0, prop));

    CHECK(doc.load_tree() == Status::Ok);
    CHECK(*doc.last_error() == 0);
    CHECK(doc.has_tree() && doc.node_count() == 8);
    CHECK(doc.guid_count() == 2);   // GUID-tabellen lästes med

    for (uint32_t i = 0; i < 8; ++i) {
        const Expect& e = kExpected[i];
        CHECK(doc.node(i, node));
        CHECK(node.parent == e.parent && node.child_count == e.children &&
              node.subtree_size == e.subtree && node.kind == e.kind);
        CHECK(node.name == e.name && node.display == e.display && node.internal == e.internal);
        CHECK(node.material == e.material && node.element == e.element);
        CHECK(doc.first_child(i) == e.first_child);
        CHECK(doc.next_sibling(i) == e.next_sibling);
    }
    CHECK(!doc.node(8, node));
    CHECK(doc.first_child(8) == kNone && doc.next_sibling(8) == kNone);

    // Elementet pekar in i GUID-tabellen.
    uint8_t g[16];
    CHECK(doc.node(2, node) && node.element == 1 && doc.guid(static_cast<size_t>(node.element) - 1, g));
    CHECK(g[0] == 0 && g[15] == 15);
    CHECK(doc.node(7, node) && node.element == 2 && doc.guid(static_cast<size_t>(node.element) - 1, g));
    CHECK(g[0] == 16 && g[15] == 31);

    // Flikar: A på båda ställena har tre, ingen annan nod har någon.
    CHECK(check_a_tabs(doc, 1) == 0);
    CHECK(check_a_tabs(doc, 5) == 0);
    for (const uint32_t i : {0u, 2u, 3u, 4u, 6u, 7u}) CHECK(doc.tab_count(i) == 0 && !doc.tab(i, 0, tab));
    CHECK(!doc.property(2, 0, 0, prop));

    TreeStats st;
    fails += check_tree(doc, "standard", st);
    CHECK(st.nodes == 8 && st.leaves == 4 && st.groups == 3 && st.partitions == 0);
    CHECK(st.tabs == 6 && st.properties == 32 && st.top_nodes == 2);

    // Strängvyerna gäller till close(), också efter ett nytt load_tree().
    CHECK(doc.node(1, node));
    const char* name_ptr = node.name.data();
    CHECK(doc.load_tree() == Status::Ok && *doc.last_error() == 0);
    CHECK(doc.node_count() == 8 && doc.node(1, node) && node.name.data() == name_ptr);

    // Fragmenten efter trädet: varje seq är ett löv.
    CHECK(doc.load_fragments() == Status::Ok && doc.fragment_count() == 4);
    CHECK(fragment_at(doc, 0).seq == 2 && fragment_at(doc, 3).seq == 7);
    for (size_t i = 0; i < doc.fragment_count(); ++i)
        CHECK(doc.node(fragment_at(doc, i).seq, node) && node.kind == NodeKind::Leaf);

    // Stäng och öppna igen.
    doc.close();
    CHECK(!doc.has_tree() && doc.node_count() == 0);
    CHECK(!doc.node(1, node) && doc.first_child(0) == kNone && doc.next_sibling(1) == kNone);
    CHECK(doc.tab_count(1) == 0 && !doc.tab(1, 0, tab) && !doc.property(1, 0, 0, prop));
    CHECK(doc.load_tree() == Status::NotLoaded && *doc.last_error() != 0);
    CHECK(doc.open(o.file.path.c_str()) == Status::Ok);
    CHECK(!doc.has_tree() && doc.node_count() == 0);   // inget är läst
    CHECK(doc.load_tree() == Status::Ok && doc.node_count() == 8);
    CHECK(doc.node(5, node) && node.material == "Wood");
    return fails;
}

// ---------------------------------------------------------------------------
// seq mot trädet, i båda ordningarna.

int test_seq() {
    int fails = 0;

    {   // Löv: båda ordningarna går.
        Opened o("hnwd_test_tree_seq_ok.nwd", build(Spec{}));
        CHECK(o.doc.load_fragments() == Status::Ok);
        CHECK(o.doc.load_tree() == Status::Ok && o.doc.has_tree() && o.doc.fragment_count() == 4);
        Opened p("hnwd_test_tree_seq_ok2.nwd", build(Spec{}));
        CHECK(p.doc.load_tree() == Status::Ok);
        CHECK(p.doc.load_fragments() == Status::Ok && p.doc.fragment_count() == 4);
    }

    // En seq som pekar på en grupp (1 är A), på roten (0) och utanför trädet (8, 99).
    for (const int32_t bad : {1, 0, 8, 99}) {
        Spec sp;
        sp.seqs = {2, 3, 6, 7, bad};
        std::sort(sp.seqs.begin(), sp.seqs.end());   // seq avtar aldrig
        const std::string text = "seq " + std::to_string(bad);

        {   // Fragmenten först: trädet är det som kommer sist.
            Opened o("hnwd_test_tree_seq_a.nwd", build(sp));
            CHECK(o.doc.load_fragments() == Status::Ok);
            CHECK(o.doc.load_tree() == Status::Corrupt);
            CHECK(contains(o.doc.last_error(), text.c_str()));
            CHECK(!o.doc.has_tree() && o.doc.node_count() == 0);   // trädet är inte läst
            CHECK(o.doc.fragment_count() == sp.seqs.size());       // fragmenten är det
            Node n;
            CHECK(!o.doc.node(1, n));
            CHECK(o.doc.load_tree() == Status::Corrupt);           // samma fel igen
        }
        {   // Trädet först: fragmenten är det som kommer sist.
            Opened o("hnwd_test_tree_seq_b.nwd", build(sp));
            CHECK(o.doc.load_tree() == Status::Ok);
            CHECK(o.doc.load_fragments() == Status::Corrupt);
            CHECK(contains(o.doc.last_error(), text.c_str()));
            CHECK(o.doc.has_tree() && o.doc.node_count() == 8);    // trädet är läst
            CHECK(o.doc.fragment_count() == 0 && all_fragments(o.doc).empty());
            CHECK(o.doc.load_fragments() == Status::Corrupt);
            Node n;
            CHECK(o.doc.node(7, n) && n.kind == NodeKind::Leaf);
        }
    }
    return fails;
}

// ---------------------------------------------------------------------------
// Fel.

int test_errors() {
    int fails = 0;

    {   // Utan trädpaket.
        Spec sp;
        sp.tree_package = false;
        Opened o("hnwd_test_tree_nopackage.nwd", build(sp));
        CHECK(o.status == Status::Ok);
        CHECK(o.doc.load_tree() == Status::Missing && contains(o.doc.last_error(), "LcOpNwdLogicalHierarchy"));
        CHECK(!o.doc.has_tree() && o.doc.node_count() == 0);
    }
    {   // Taket i Options: exakt antalet går, ett mindre ger LimitExceeded.
        HNwd::Options limit;
        limit.max_tree_nodes = 8;
        Opened ok("hnwd_test_tree_limit_ok.nwd", build(Spec{}), limit);
        CHECK(ok.doc.load_tree() == Status::Ok && ok.doc.node_count() == 8);

        limit.max_tree_nodes = 7;
        Opened o("hnwd_test_tree_limit.nwd", build(Spec{}), limit);
        CHECK(o.status == Status::Ok);
        CHECK(o.doc.load_tree() == Status::LimitExceeded && *o.doc.last_error() != 0);
        CHECK(!o.doc.has_tree() && o.doc.node_count() == 0);
        limit.max_tree_nodes = 5;
        Opened small("hnwd_test_tree_limit_small.nwd", build(Spec{}), limit);
        CHECK(small.doc.load_tree() == Status::LimitExceeded);
    }
    {   // Trädet är avklippt.
        Bytes cut = standard_tree();
        cut.resize(cut.size() - 16);
        Spec sp;
        sp.tree = cut;
        Opened o("hnwd_test_tree_cut.nwd", build(sp));
        CHECK(o.doc.load_tree() == Status::Corrupt && contains(o.doc.last_error(), "LcOpNwdLogicalHierarchy"));
        CHECK(!o.doc.has_tree() && o.doc.node_count() == 0);
        CHECK(o.doc.load_tree() == Status::Corrupt);
    }
    {   // Svansen är inte 1.
        Spec sp;
        sp.tree = standard_tree(2);
        Opened o("hnwd_test_tree_tail.nwd", build(sp));
        CHECK(o.doc.load_tree() == Status::Corrupt && *o.doc.last_error() != 0);
    }
    {   // En trasig ström: kontrollsumman stämmer inte.
        Spec sp;
        sp.break_tree_stream = true;
        Opened o("hnwd_test_tree_zlib.nwd", build(sp));
        CHECK(o.doc.load_tree() == Status::Corrupt && contains(o.doc.last_error(), "LcOpNwdLogicalHierarchy"));
        CHECK(!o.doc.has_tree());
    }
    {   // Fel form: råa byte i stället för zlib.
        Spec sp;
        sp.tree_flags = 3;
        Opened o("hnwd_test_tree_form.nwd", build(sp));
        CHECK(o.doc.load_tree() == Status::Corrupt && contains(o.doc.last_error(), "LcOpNwdLogicalHierarchy"));
    }
    {   // Noderna har element men filen har ingen GUID-tabell.
        Spec sp;
        sp.guids = false;
        Opened o("hnwd_test_tree_noguid.nwd", build(sp));
        CHECK(o.doc.load_tree() == Status::Corrupt && contains(o.doc.last_error(), "GUID"));
        CHECK(!o.doc.has_tree() && o.doc.guid_count() == 0);
    }
    {   // GUID-tabellen går inte att läsa.
        Spec sp;
        sp.guid_flags = 3;
        Opened o("hnwd_test_tree_badguid.nwd", build(sp));
        CHECK(o.doc.load_tree() == Status::Corrupt && contains(o.doc.last_error(), "LcOpGuidStore"));
        CHECK(!o.doc.has_tree());
    }
    {   // Huvudet nämner egenskapsrötter men filen har inget egenskapspaket.
        Spec sp;
        sp.props_package = false;
        Opened o("hnwd_test_tree_noprops.nwd", build(sp));
        CHECK(o.doc.load_tree() == Status::Corrupt && contains(o.doc.last_error(), "LcOaPartitionProps"));
        CHECK(!o.doc.has_tree());
    }
    {   // En egenskapsström är avklippt.
        Spec sp;
        sp.props[0].resize(sp.props[0].size() - 16);
        Opened o("hnwd_test_tree_cutprops.nwd", build(sp));
        CHECK(o.doc.load_tree() == Status::Corrupt && contains(o.doc.last_error(), "LcOaPartitionProps"));
        CHECK(!o.doc.has_tree());
    }
    {   // Ingen fil öppen.
        Document doc;
        CHECK(doc.load_tree() == Status::NotLoaded && *doc.last_error() != 0);
        CHECK(!doc.has_tree() && doc.node_count() == 0);
    }
    return fails;
}

// ---------------------------------------------------------------------------
// Små träd utan GUID-tabell och egenskaper.

int test_small() {
    int fails = 0;
    Node node;
    {   // En toppnod med ett löv.
        Spec sp;
        sp.tree = small_tree(true);
        sp.guids = false;
        sp.props_package = false;
        sp.fragments = false;
        Opened o("hnwd_test_tree_small.nwd", build(sp));
        CHECK(o.status == Status::Ok);
        CHECK(o.doc.load_tree() == Status::Ok && *o.doc.last_error() == 0);
        CHECK(o.doc.node_count() == 3);
        CHECK(o.doc.node(1, node) && node.name == "G" && node.display.empty() && node.kind == NodeKind::Group);
        CHECK(node.child_count == 1 && node.subtree_size == 2 && node.parent == 0);
        CHECK(o.doc.node(2, node) && node.name == "leaf" && node.kind == NodeKind::Leaf && node.parent == 1);
        CHECK(o.doc.first_child(1) == 2 && o.doc.next_sibling(1) == kNone);
        TreeStats st;
        fails += check_tree(o.doc, "small", st);
        CHECK(o.doc.guid_count() == 0);
    }
    {   // Inga toppnoder: bara roten.
        Spec sp;
        sp.tree = small_tree(false);
        sp.guids = false;
        sp.props_package = false;
        sp.fragments = false;
        Opened o("hnwd_test_tree_empty.nwd", build(sp));
        CHECK(o.doc.load_tree() == Status::Ok && o.doc.node_count() == 1);
        CHECK(o.doc.node(0, node) && node.kind == NodeKind::Root && node.child_count == 0 &&
              node.subtree_size == 1);
        CHECK(o.doc.first_child(0) == kNone && o.doc.next_sibling(0) == kNone);
        CHECK(!o.doc.node(1, node));
    }
    return fails;
}

// ---------------------------------------------------------------------------
// Exempelfilerna.

// Antalet noder i trädet i var och en av de nio .nwc-filerna, med roten.
constexpr uint64_t kSampleNodes[9] = {7330, 1100, 7345, 2448, 821, 294, 509, 842, 3037};

// Varje fragments seq ska vara ett löv, och elementen ska ha sin GUID.
int check_fragments_against_tree(const Document& doc, const char* label) {
    int fails = 0;
    uint64_t bad = 0;
    for (size_t i = 0; i < doc.fragment_count(); ++i) {
        Node n;
        const bool ok = doc.node(fragment_at(doc, i).seq, n) && n.kind == NodeKind::Leaf;
        if (!ok && bad++ < 3) std::printf("  FAIL — %s: fragment %zu: seq is not a leaf\n", label, i);
    }
    CHECK(bad == 0);
    return fails;
}

int check_nwc(const testdata::SampleCounts& s, uint64_t expected_nodes, size_t& nodes_out) {
    int fails = 0;
    Document doc;
    if (doc.open(s.path) != Status::Ok) {
        std::printf("  FAIL — %s: open: %s\n", s.path, doc.last_error());
        return 1;
    }
    if (doc.load_tree() != Status::Ok) {
        std::printf("  FAIL — %s: load_tree: %s\n", s.path, doc.last_error());
        return 1;
    }
    nodes_out = doc.node_count();
    CHECK(doc.node_count() == expected_nodes);
    CHECK(doc.guid_count() == s.guids);

    TreeStats st;
    fails += check_tree(doc, s.path, st);
    CHECK(st.top_nodes == s.top_nodes);
    CHECK(st.partitions == 0 && st.leaves > 0);
    Node top;
    CHECK(doc.node(doc.first_child(0), top) && top.kind == NodeKind::Group && top.parent == 0);

    // Fragmenten efter trädet.
    if (doc.load_fragments() != Status::Ok) {
        std::printf("  FAIL — %s: load_fragments after load_tree: %s\n", s.path, doc.last_error());
        ++fails;
    } else {
        CHECK(doc.fragment_count() == s.fragments);
        fails += check_fragments_against_tree(doc, s.path);
    }

    // Fragmenten före trädet.
    Document other;
    if (other.open(s.path) != Status::Ok || other.load_fragments() != Status::Ok) {
        std::printf("  FAIL — %s: load_fragments: %s\n", s.path, other.last_error());
        return fails + 1;
    }
    if (other.load_tree() != Status::Ok) {
        std::printf("  FAIL — %s: load_tree after load_fragments: %s\n", s.path, other.last_error());
        ++fails;
    } else {
        CHECK(other.node_count() == expected_nodes);
        fails += check_fragments_against_tree(other, s.path);
    }

    std::printf("  %s\n    nodes %llu (groups %llu, leaves %llu), top nodes %u, tabs %llu, properties "
                "%llu, unnamed %llu, with material %llu, fragments %zu\n",
                s.path, static_cast<unsigned long long>(st.nodes),
                static_cast<unsigned long long>(st.groups), static_cast<unsigned long long>(st.leaves),
                st.top_nodes, static_cast<unsigned long long>(st.tabs),
                static_cast<unsigned long long>(st.properties),
                static_cast<unsigned long long>(st.unnamed),
                static_cast<unsigned long long>(st.with_material), doc.fragment_count());
    return fails;
}

// .nwd med nio modeller: trädet är rotens nio Partition-barn med var sin .nwc:s
// träd under sig, om klass 32 är avkodad.
int check_nwd(const size_t nwc_nodes[9], bool all_nwc) {
    int fails = 0;
    Document doc;
    if (doc.open(testdata::kSampleNwd) != Status::Ok) {
        std::printf("  FAIL — %s: open: %s\n", testdata::kSampleNwd, doc.last_error());
        return 1;
    }
    const Status status = doc.load_tree();
    if (status != Status::Ok) {
        std::printf("  FAIL — %s: load_tree: %s\n", testdata::kSampleNwd, doc.last_error());
        return 1;
    }

    TreeStats st;
    fails += check_tree(doc, testdata::kSampleNwd, st);
    CHECK(st.top_nodes == 9 && st.partitions == 9);
    for (uint32_t c = doc.first_child(0); c != kNone; c = doc.next_sibling(c)) {
        Node n;
        CHECK(doc.node(c, n) && n.kind == NodeKind::Partition && n.parent == 0);
    }
    if (doc.load_fragments() != Status::Ok) {
        std::printf("  FAIL — %s: load_fragments after load_tree: %s\n", testdata::kSampleNwd,
                    doc.last_error());
        ++fails;
    } else {
        CHECK(doc.fragment_count() == 56247);
        fails += check_fragments_against_tree(doc, testdata::kSampleNwd);
    }
    if (all_nwc) {
        size_t expected = 1 + 9;
        for (int i = 0; i < 9; ++i) expected += nwc_nodes[i] - 1;
        CHECK(doc.node_count() == expected);
    } else {
        std::printf("  SKIP — en .nwc saknas, summan av deras träd kan inte prövas\n");
        ++g_skipped;
    }
    std::printf("  %s\n    nodes %zu (partitions %llu, groups %llu, leaves %llu), tabs %llu, "
                "properties %llu\n",
                testdata::kSampleNwd, doc.node_count(), static_cast<unsigned long long>(st.partitions),
                static_cast<unsigned long long>(st.groups), static_cast<unsigned long long>(st.leaves),
                static_cast<unsigned long long>(st.tabs),
                static_cast<unsigned long long>(st.properties));
    return fails;
}

// Ett träd där varje grupp har samma delträd tre gånger: N_k har N_{k-1} som
// alla barnen, det första nytt och de andra genom bakåtpekare, och N_0 är ett
// löv. De flesta platser har en annan lagrad nod än platsen före, så trädet
// prövar förekomsterna med en lagrad nod per plats. Full och Structure ger
// föräldrar, delträd och namn som väntat.
int test_shared_deep() {
    int fails = 0;
    constexpr int kLevels = 5;
    constexpr uint32_t kChildren = 3;
    ByteWriter w;
    w.i32(1);
    w.i32(0);
    w.implicit_root();
    w.i32(1);
    auto head = [&](int32_t cls, const char* name) {
        const int32_t id = w.obj(cls);
        w.str(name);
        w.null_ptr();
        w.i32(0);
        w.i32(0);
        w.i32(0);
        return id;
    };
    // Toppnoden N_k har N_{k-1} som första barn, och så vidare ned till N_0;
    // sedan, inifrån och ut, varje nods övriga barn som bakåtpekare.
    std::vector<int32_t> ids(kLevels + 1, 0);
    for (int k = kLevels; k >= 1; --k) {
        ids[k] = head(53, k == kLevels ? "Top" : "N");
        w.i32(static_cast<int32_t>(kChildren));
    }
    ids[0] = head(22, "L");
    for (int k = 0; k < kLevels; ++k)
        for (uint32_t c = 1; c < kChildren; ++c) w.ref(ids[k]);
    w.i32(1);
    w.pad(8);

    // Väntat i preorder: förälder och delträdets storlek.
    struct Expect {
        uint32_t parent, subtree;
        int level;
    };
    std::vector<Expect> expected{{kNone, 0, -1}};
    {
        // Platsen läggs till, sedan barnen; barnen är lika. Delträdet på nivå
        // l har (3^(l+1) - 1) / 2 platser.
        std::vector<std::pair<int, uint32_t>> todo{{kLevels, 0}};
        while (!todo.empty()) {
            const auto [level, parent] = todo.back();
            todo.pop_back();
            const uint32_t index = static_cast<uint32_t>(expected.size());
            uint32_t subtree = 1;
            for (int l = 0; l < level; ++l) subtree = subtree * kChildren + 1;
            expected.push_back({parent, subtree, level});
            if (level > 0)
                for (uint32_t c = 0; c < kChildren; ++c) todo.push_back({level - 1, index});
        }
        expected[0].subtree = static_cast<uint32_t>(expected.size());
    }

    Spec sp;
    sp.tree = w.data;
    sp.props_package = false;
    sp.props.clear();
    sp.fragments = false;
    sp.guids = false;
    for (const HNwd::TreeContent content : {HNwd::TreeContent::Full, HNwd::TreeContent::Structure}) {
        Opened o("hnwd_test_tree_shared_deep.nwd", build(sp));
        CHECK(o.status == Status::Ok);
        if (o.status != Status::Ok) return fails;
        CHECK(o.doc.load_tree(content) == Status::Ok);
        CHECK(o.doc.node_count() == expected.size());
        if (o.doc.node_count() != expected.size()) return fails;
        TreeStats st;
        fails += check_tree(o.doc, "shared_deep", st);
        size_t bad = 0;
        for (uint32_t i = 1; i < expected.size(); ++i) {
            Node node;
            const Expect& e = expected[i];
            const bool ok = o.doc.node(i, node) && node.parent == e.parent && o.doc.parent(i) == e.parent &&
                            node.subtree_size == e.subtree && node.child_count == (e.level > 0 ? kChildren : 0u) &&
                            node.name == (e.level == kLevels ? "Top" : e.level == 0 ? "L" : "N") &&
                            node.kind == (e.level == 0 ? NodeKind::Leaf : NodeKind::Group);
            bad += !ok;
        }
        CHECK(bad == 0);
    }
    return fails;
}

int test_samples() {
    int fails = 0;
    size_t nwc_nodes[9] = {};
    bool all_nwc = true;
    for (size_t i = 0; i < 9; ++i) {
        const testdata::SampleCounts& s = testdata::kSamples[i];
        if (!testdata::exists(s.path)) {
            std::printf("  SKIP — %s saknas\n", s.path);
            ++g_skipped;
            all_nwc = false;
            continue;
        }
        fails += check_nwc(s, kSampleNodes[i], nwc_nodes[i]);
    }

    if (!testdata::exists(testdata::kSampleNwd)) {
        std::printf("  SKIP — %s saknas\n", testdata::kSampleNwd);
        ++g_skipped;
    } else {
        fails += check_nwd(nwc_nodes, all_nwc);
    }
    return fails;
}

// ---------------------------------------------------------------------------
// TreeContent::Structure mot Full.

// Två dokument med samma fil, det ena läst med Full och det andra med
// Structure: samma noder i allt, men Structure har inga flikar.
int compare_structure(const Document& full, const Document& st, const char* label) {
    int fails = 0;
    CHECK(full.has_tree() && st.has_tree());
    CHECK(full.has_tabs() && !st.has_tabs());
    CHECK(full.node_count() == st.node_count());
    const uint32_t n = static_cast<uint32_t>(std::min(full.node_count(), st.node_count()));
    uint64_t bad = 0;
    for (uint32_t i = 0; i <= n; ++i) {   // också ett index utanför
        const int before = fails;
        Node a, b;
        const bool ha = full.node(i, a), hb = st.node(i, b);
        CHECK(ha == hb);
        if (ha && hb) {
            CHECK(a.parent == b.parent && a.child_count == b.child_count &&
                  a.subtree_size == b.subtree_size && a.kind == b.kind && a.element == b.element);
            CHECK(a.name == b.name && a.display == b.display && a.internal == b.internal &&
                  a.material == b.material);
            CHECK(full.parent(i) == a.parent && st.parent(i) == b.parent);
        }
        CHECK(full.first_child(i) == st.first_child(i));
        CHECK(full.next_sibling(i) == st.next_sibling(i));
        CHECK(st.tab_count(i) == 0);
        HNwd::Tab tab;
        HNwd::Property prop;
        CHECK(!st.tab(i, 0, tab) && !st.property(i, 0, 0, prop));
        if (fails != before && ++bad <= 3) std::printf("  FAIL — %s: node %u differs\n", label, i);
    }
    CHECK(full.parent(n) == kNone && st.parent(n) == kNone && st.parent(0) == kNone);
    return fails;
}

int test_structure_synthetic() {
    int fails = 0;
    {   // Standardfilen: allt utom flikarna som i Full.
        Opened f("hnwd_test_tree_st_full.nwd", build(Spec{}));
        Opened s("hnwd_test_tree_st_structure.nwd", build(Spec{}));
        CHECK(f.doc.load_tree(HNwd::TreeContent::Full) == Status::Ok);
        CHECK(s.doc.load_tree(HNwd::TreeContent::Structure) == Status::Ok && *s.doc.last_error() == 0);
        fails += compare_structure(f.doc, s.doc, "standard");
        // A har tre flikar i filen, men Structure ger inga.
        CHECK(f.doc.tab_count(1) == 3 && s.doc.tab_count(1) == 0);
        TreeStats st;
        fails += check_tree(s.doc, "standard structure", st);
        CHECK(st.nodes == 8 && st.tabs == 0 && st.properties == 0);

        // Fragmenten efter Structure: seq prövas mot trädet, och fragmentet ger sitt löv.
        CHECK(s.doc.load_fragments() == Status::Ok);
        for (size_t i = 0; i < s.doc.fragment_count(); ++i)
            CHECK(s.doc.node_of_fragment(i) == fragment_at(s.doc, i).seq);
        CHECK(s.doc.node_of_fragment(s.doc.fragment_count()) == kNone);
        CHECK(s.doc.node_of_fragment(2) == 6 && s.doc.parent(6) == 5 && s.doc.parent(5) == 4 &&
              s.doc.parent(4) == 0 && s.doc.parent(0) == kNone);
        // Utan partition i trädet och utan partitionspaket: ingen modell.
        CHECK(s.doc.model_count() == 0);
        HNwd::Model m;
        CHECK(!s.doc.model(0, m));

        // PropertyReader: flikarna ur filen i Structure, ur minnet i Full, lika
        // med Document i Full för varje nod.
        HNwd::PropertyReader rs, rf;
        CHECK(rs.open(s.doc) == Status::Ok && rf.open(f.doc) == Status::Ok);
        for (uint32_t i = 0; i < 8; ++i) {
            HNwd::NodeProperties ps, pf;
            CHECK(rs.read(i, ps) == Status::Ok && rf.read(i, pf) == Status::Ok);
            CHECK(ps.tabs.size() == f.doc.tab_count(i) && pf.tabs.size() == ps.tabs.size());
            size_t k = 0;
            for (size_t t = 0; t < ps.tabs.size() && t < pf.tabs.size(); ++t) {
                HNwd::Tab tab;
                CHECK(f.doc.tab(i, t, tab));
                for (const HNwd::Tab* x : {&ps.tabs[t], &pf.tabs[t]})
                    CHECK(x->name == tab.name && x->internal == tab.internal &&
                          x->property_count == tab.property_count);
                for (size_t p = 0; p < tab.property_count; ++p, ++k) {
                    HNwd::Property e;
                    CHECK(f.doc.property(i, t, p, e) && k < ps.properties.size() &&
                          k < pf.properties.size());
                    if (k >= ps.properties.size() || k >= pf.properties.size()) break;
                    for (const HNwd::Property* x : {&ps.properties[k], &pf.properties[k]})
                        CHECK(x->name == e.name && x->internal == e.internal &&
                              x->raw_type == e.raw_type && x->type == e.type && x->d[0] == e.d[0] &&
                              x->d[1] == e.d[1] && x->d[2] == e.d[2] && x->i == e.i &&
                              x->text == e.text);
                }
            }
            CHECK(k == ps.properties.size() && k == pf.properties.size());
        }
        HNwd::NodeProperties a;
        CHECK(rs.read(5, a) == Status::Ok && a.tabs.size() == 3 && a.properties.size() == 16);
        if (a.tabs.size() == 3 && a.properties.size() == 16) {
            CHECK(a.tabs[0].name == "Item" && a.tabs[1].name == "P0 Display" && a.tabs[2].name == "Id");
            CHECK(a.properties[4].text == "hello" && a.properties[8].text == "Named Display");
            CHECK(a.properties[15].i == 12345);
        }
        CHECK(rs.read(8, a) == Status::OutOfRange && a.tabs.empty());
        // Läsaren läser vidare när dokumentet är stängt.
        s.doc.close();
        CHECK(rs.read(1, a) == Status::Ok && a.tabs.size() == 3 && a.properties[0].name == "key0");
    }
    {   // seq som inte är ett löv ger fel också i Structure, i båda ordningarna.
        Spec sp;
        sp.seqs = {1, 2, 3, 6, 7};
        Opened a("hnwd_test_tree_st_seq_a.nwd", build(sp));
        CHECK(a.doc.load_fragments() == Status::Ok);
        CHECK(a.doc.load_tree(HNwd::TreeContent::Structure) == Status::Corrupt);
        CHECK(contains(a.doc.last_error(), "seq 1") && !a.doc.has_tree());
        Opened b("hnwd_test_tree_st_seq_b.nwd", build(sp));
        CHECK(b.doc.load_tree(HNwd::TreeContent::Structure) == Status::Ok);
        CHECK(b.doc.load_fragments() == Status::Corrupt && b.doc.has_tree());
    }
    {   // Fel ger samma Status i båda lägena.
        Spec sp;
        sp.props_package = false;
        Opened o("hnwd_test_tree_st_noprops.nwd", build(sp));
        CHECK(o.doc.load_tree(HNwd::TreeContent::Structure) == Status::Corrupt);
        CHECK(contains(o.doc.last_error(), "LcOaPartitionProps") && !o.doc.has_tree());
        HNwd::Options limit;
        limit.max_tree_nodes = 7;
        Opened l("hnwd_test_tree_st_limit.nwd", build(Spec{}), limit);
        CHECK(l.doc.load_tree(HNwd::TreeContent::Structure) == Status::LimitExceeded);
        Document none;
        CHECK(none.load_tree(HNwd::TreeContent::Structure) == Status::NotLoaded);
        CHECK(none.load_tree(static_cast<HNwd::TreeContent>(7)) == Status::NotLoaded);
        Opened bad("hnwd_test_tree_st_badcontent.nwd", build(Spec{}));
        CHECK(bad.doc.load_tree(static_cast<HNwd::TreeContent>(7)) == Status::OutOfRange);
    }
    {   // Små träd.
        for (const bool top : {true, false}) {
            Spec sp;
            sp.tree = small_tree(top);
            sp.guids = false;
            sp.props_package = false;
            sp.fragments = false;
            Opened f("hnwd_test_tree_st_small_f.nwd", build(sp));
            Opened s("hnwd_test_tree_st_small_s.nwd", build(sp));
            CHECK(f.doc.load_tree() == Status::Ok);
            CHECK(s.doc.load_tree(HNwd::TreeContent::Structure) == Status::Ok);
            fails += compare_structure(f.doc, s.doc, "small");
        }
    }
    return fails;
}

// Avbrott, och ett andra load_tree() med annat innehåll.
int test_repeat_and_cancel() {
    int fails = 0;
    const auto stop = [](uint64_t, uint64_t) { return false; };
    for (const HNwd::TreeContent c : {HNwd::TreeContent::Full, HNwd::TreeContent::Structure}) {
        Opened o("hnwd_test_tree_cancel.nwd", build(Spec{}));
        uint64_t calls = 0, last_done = 0, last_total = 0;
        CHECK(o.doc.load_tree(c, stop) == Status::Cancelled && *o.doc.last_error() != 0);
        CHECK(!o.doc.has_tree() && o.doc.node_count() == 0);
        // Dokumentet går att läsa igen, och förloppet når slutet.
        CHECK(o.doc.load_tree(c, [&](uint64_t done, uint64_t total) {
            ++calls;
            last_done = done;
            last_total = total;
            return true;
        }) == Status::Ok);
        CHECK(calls >= 1 && last_done == last_total && last_total > 0);
        CHECK(o.doc.node_count() == 8 && o.doc.has_tabs() == (c == HNwd::TreeContent::Full));
    }

    {   // Structure, sedan Structure och Full.
        Opened o("hnwd_test_tree_repeat.nwd", build(Spec{}));
        CHECK(o.doc.load_fragments() == Status::Ok);
        CHECK(o.doc.load_tree(HNwd::TreeContent::Structure) == Status::Ok);
        Node node;
        CHECK(o.doc.node(1, node) && node.name == "A");
        const std::string_view name = node.name;
        const size_t structure_bytes = o.doc.memory_bytes();
        CHECK(o.doc.load_tree(HNwd::TreeContent::Structure) == Status::Ok);   // redan uppfyllt
        CHECK(o.doc.node(1, node) && node.name.data() == name.data());
        CHECK(o.doc.memory_bytes() == structure_bytes);

        // Full avbrutet: Structure står kvar.
        CHECK(o.doc.load_tree(HNwd::TreeContent::Full, stop) == Status::Cancelled);
        CHECK(o.doc.has_tree() && !o.doc.has_tabs() && o.doc.node_count() == 8);
        CHECK(o.doc.node(1, node) && node.name.data() == name.data());

        // Full läser om; vyn ur Structure gäller fortfarande, och båda träden räknas.
        CHECK(o.doc.load_tree(HNwd::TreeContent::Full) == Status::Ok);
        CHECK(o.doc.has_tabs() && o.doc.tab_count(1) == 3 && check_a_tabs(o.doc, 5) == 0);
        CHECK(name == "A");
        CHECK(o.doc.memory_bytes() > structure_bytes);
        CHECK(o.doc.node(1, node) && node.name == "A");
        const char* full_name = node.name.data();
        const size_t full_bytes = o.doc.memory_bytes();
        // load_tree() och Structure efter Full: redan uppfyllt.
        CHECK(o.doc.load_tree(HNwd::TreeContent::Structure) == Status::Ok && o.doc.has_tabs());
        CHECK(o.doc.load_tree() == Status::Ok && o.doc.memory_bytes() == full_bytes);
        CHECK(o.doc.node(1, node) && node.name.data() == full_name);
        // close() släpper allt.
        o.doc.close();
        CHECK(!o.doc.has_tree() && !o.doc.has_tabs() && o.doc.memory_bytes() < structure_bytes);
    }
    return fails;
}

// En sökväg utan katalog.
std::string_view file_name(std::string_view path) {
    const size_t cut = path.find_last_of("/\\");
    return cut == std::string_view::npos ? path : path.substr(cut + 1);
}

// Modellerna: intervallen ligger i följd utan glapp, och varje fragment i ett
// intervall sitter i modellens delträd.
int check_models(const Document& doc, const char* label) {
    int fails = 0;
    uint64_t next = 0;
    for (size_t i = 0; i < doc.model_count(); ++i) {
        HNwd::Model m;
        if (!doc.model(i, m)) {
            ++fails;
            continue;
        }
        Node n;
        CHECK(doc.node(m.node, n));
        CHECK(m.first_fragment == next);
        next = uint64_t(m.first_fragment) + m.fragment_count;
        uint64_t bad = 0;
        for (uint32_t f = m.first_fragment; f < m.first_fragment + m.fragment_count; ++f) {
            const uint32_t leaf = doc.node_of_fragment(f);
            if (leaf < m.node || leaf >= m.node + n.subtree_size) ++bad;
            // Vägen uppåt når modellens nod.
            uint32_t up = leaf;
            while (up != kNone && up != m.node) up = doc.parent(up);
            if (up != m.node) ++bad;
        }
        CHECK(bad == 0);
        if (bad) std::printf("  FAIL — %s: model %zu has %llu foreign fragments\n", label, i,
                             static_cast<unsigned long long>(bad));
    }
    CHECK(next == doc.fragment_count());
    HNwd::Model m;
    CHECK(!doc.model(doc.model_count(), m));
    return fails;
}

// Den kod en exempelfil har i kFacts.
int32_t facts_unit(const char* path) {
    for (const testdata::SampleFacts& f : testdata::kFacts)
        if (std::strcmp(f.path, path) == 0) return f.unit_code;
    return -1;
}

int test_structure_samples() {
    int fails = 0;
    for (const testdata::SampleCounts& sc : testdata::kSamples) {
        if (!testdata::exists(sc.path)) {
            std::printf("  SKIP — %s saknas\n", sc.path);
            ++g_skipped;
            continue;
        }
        const bool nwd = std::strcmp(sc.path, testdata::kSampleNwd) == 0;
        Document full, st;
        if (full.open(sc.path) != Status::Ok || st.open(sc.path) != Status::Ok) {
            std::printf("  FAIL — %s: open\n", sc.path);
            ++fails;
            continue;
        }
        const size_t base = st.memory_bytes();
        CHECK(full.load_tree(HNwd::TreeContent::Full) == Status::Ok);
        CHECK(st.load_tree(HNwd::TreeContent::Structure) == Status::Ok);
        if (!full.has_tree() || !st.has_tree()) {
            std::printf("  FAIL — %s: load_tree: %s / %s\n", sc.path, full.last_error(), st.last_error());
            ++fails;
            continue;
        }
        const size_t full_tree = full.memory_bytes() - base;
        const size_t st_tree = st.memory_bytes() - base;
        fails += compare_structure(full, st, sc.path);

        // Modellerna kräver fragmenten.
        CHECK(st.model_count() == 0);
        CHECK(st.load_fragments() == Status::Ok && full.load_fragments() == Status::Ok);
        CHECK(st.model_count() == (nwd ? 9u : 1u) && full.model_count() == st.model_count());
        fails += check_models(st, sc.path);
        for (size_t i = 0; i < st.model_count(); ++i) {
            HNwd::Model a, b;
            CHECK(st.model(i, a) && full.model(i, b));
            CHECK(a.name == b.name && a.source == b.source && a.loader == b.loader &&
                  a.unit.code == b.unit.code && a.node == b.node &&
                  a.first_fragment == b.first_fragment && a.fragment_count == b.fragment_count);
        }
        for (size_t i = 0; i < st.fragment_count(); ++i)
            if (st.node_of_fragment(i) != fragment_at(st, i).seq) {
                ++fails;
                break;
            }

        HNwd::Unit own;
        CHECK(st.length_unit(own) == Status::Ok && own.code == facts_unit(sc.path));
        if (!nwd) {
            // En modell: roten, alla fragment, och filens egen partition.
            HNwd::Model m;
            CHECK(st.model(0, m) && m.node == 0 && m.first_fragment == 0 &&
                  m.fragment_count == st.fragment_count());
            CHECK(m.unit.code == own.code && m.unit.unit == own.unit &&
                  m.unit.metres_per_unit == own.metres_per_unit);
            CHECK(!m.name.empty() && !m.loader.empty());
        } else {
            // Nio partitioner, i .nwc-filernas ordning, med deras enheter. Modellerna
            // med enhetskod 0 är de vars poser har skalan 1000.
            uint64_t metre_fragments = 0;
            for (size_t i = 0; i < st.model_count(); ++i) {
                HNwd::Model m;
                Node n;
                CHECK(st.model(i, m) && st.node(m.node, n) && n.kind == NodeKind::Partition);
                CHECK(n.parent == 0 && m.name == n.name);
                CHECK(m.unit.code == testdata::kFacts[i].unit_code);
                CHECK(m.unit.unit != HNwd::LengthUnit::Unknown && m.unit.metres_per_unit > 0);
                CHECK(file_name(m.source).size() > 4 &&
                      file_name(testdata::kSampleNwc[i]).substr(0, 6) ==
                          file_name(m.source).substr(0, 6));
                if (m.unit.code == 0) metre_fragments += m.fragment_count;
                std::printf("    model %zu: node %u, unit %d, fragments %u..+%u, %.*s\n", i, m.node,
                            m.unit.code, m.first_fragment, m.fragment_count,
                            static_cast<int>(file_name(m.source).size()), file_name(m.source).data());
            }
            CHECK(metre_fragments == testdata::kSampleNwdScaledPoses);
        }

        std::printf("  %s: tree memory Full %zu B, Structure %zu B (%.1f B per node)\n", sc.path,
                    full_tree, st_tree,
                    static_cast<double>(st_tree) / static_cast<double>(st.node_count()));
        if (nwd)
            std::printf("  %s: memory_bytes() after load_tree and load_fragments: Full %zu B, "
                        "Structure %zu B\n",
                        sc.path, full.memory_bytes(), st.memory_bytes());
    }
    return fails;
}

} // namespace

int test_tree() {
    int fails = 0;
    fails += test_standard();
    fails += test_seq();
    fails += test_errors();
    fails += test_small();
    fails += test_shared_deep();
    fails += test_samples();
    fails += test_structure_synthetic();
    fails += test_repeat_and_cancel();
    fails += test_structure_samples();
    std::printf("test_tree: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
