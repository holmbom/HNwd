#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

#include "container.h"
#include "hierarchy.h"
#include "mapped_file.h"
#include "tab_reader.h"
#include "test_data.h"
#include "test_support.h"

using namespace HNwd::detail;
using testdata::Bytes;
using testdata::ByteWriter;

namespace {

using Streams = std::vector<Bytes>;

constexpr uint64_t kBig = uint64_t(1) << 23;

// --- Byggstenar ---------------------------------------------------------------

void header(ByteWriter& w, std::initializer_list<int32_t> counts, int32_t tops) {
    w.i32(static_cast<int32_t>(counts.size()));
    for (const int32_t c : counts) w.i32(c);
    w.implicit_root();
    w.i32(tops);
}

void finish(ByteWriter& w) {
    w.i32(1);
    w.pad(8);
}

int32_t name_pair(ByteWriter& w, const char* display, const char* internal = "Lc") {
    const int32_t id = w.obj(52);
    w.str(display);
    w.str(internal);
    return id;
}

int32_t node_head(ByteWriter& w, int32_t cls, const char* name, int32_t y) {
    const int32_t id = w.obj(cls);
    w.str(name);
    name_pair(w, name);
    w.i32(0);
    w.i32(0);
    w.i32(y);
    return id;
}

// En flik (86) med egenskaper av typ 2 under sitt eget namnpar. `user` får
// namnparets id.
int32_t tab_with_ints(ByteWriter& w, const char* name, std::initializer_list<int32_t> values,
                      int32_t* user = nullptr) {
    const int32_t id = w.obj(86);
    w.str(name);
    const int32_t u = name_pair(w, name, "LcTab");
    if (user) *user = u;
    w.i32(0);
    w.i32(static_cast<int32_t>(values.size()));
    for (const int32_t v : values) {
        w.ref(u);
        w.i32(2);
        w.i32(v);
    }
    return id;
}

// Trädet och egenskapsströmmarna som lagrade zlib-strömmar i en fil.
struct TreeFile {
    Bytes file;
    HierarchyStreams streams;
    size_t block = 0;
};

TreeFile tree_file(const Bytes& tree, const Streams& props, size_t block) {
    TreeFile f;
    f.block = block;
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
    f.streams.has_props = true;
    for (const Bytes& s : props) f.streams.props.push_back(add(s));
    f.file.insert(f.file.end(), 16, 0xEE);
    return f;
}

// Var uppackad byte `i` i en ström ligger i filen: efter zlib-huvudet har
// varje lagrat block ett eget huvud om 5 byte.
size_t file_byte(const TreeFile& f, const StreamSpan& span, size_t i) {
    return static_cast<size_t>(span.offset) + 2 + (i / f.block + 1) * 5 + i;
}

// Skelettet med nodens plats ersatt av `loc`.
HierarchySkeleton with_location(const HierarchySkeleton& sk, uint32_t node, const NodeLocation& loc) {
    HierarchySkeleton out = sk;
    out.locations = PackedRows();
    for (uint32_t i = 0; i < sk.locations.size(); ++i) {
        NodeLocation l;
        sk.location(i, l);
        if (i == node) l = loc;
        out.locations.push({l.attr_pos, static_cast<uint32_t>(l.attr_next_id), l.tab_count});
    }
    return out;
}

// Nodens plats i skelettet.
NodeLocation location_of(const HierarchySkeleton& sk, uint32_t node) {
    NodeLocation l;
    sk.location(node, l);
    return l;
}

// --- Jämförelsen ----------------------------------------------------------------

bool same_text(const StringPool& a, uint32_t ia, const StringPool& b, uint32_t ib) {
    return (ia == kNone) == (ib == kNone) && a.get(ia) == b.get(ib);
}

bool same_name(const Hierarchy& h, uint32_t a, const NodeTabs& t, uint32_t b) {
    if ((a == kNone) != (b == kNone)) return false;
    if (a == kNone) return true;
    if (a >= h.names.size() || b >= t.names.size()) return false;
    return same_text(h.strings, h.names[a].display, t.strings, t.names[b].display) &&
           same_text(h.strings, h.names[a].internal, t.strings, t.names[b].internal);
}

// Två f64 med samma bitar; NaN är då lika med samma NaN.
bool same_bits(double a, double b) {
    return std::memcmp(&a, &b, sizeof a) == 0;
}

// Nodens flikar och egenskaper ur Full lika med de omlästa?
bool same_tabs(const Hierarchy& h, uint32_t node, const NodeTabs& t) {
    const uint32_t count = h.tab_count(node);
    if (count != t.tabs.size()) return false;
    for (uint32_t j = 0; j < count; ++j) {
        const StoredTab& a = h.tabs[h.node_tabs[h.tab_begin[node] + j]];
        const StoredTab& b = t.tabs[j];
        if (a.source_class != b.source_class || a.flags != b.flags || a.prop_count != b.prop_count ||
            !same_text(h.strings, a.name, t.strings, b.name) || !same_name(h, a.user, t, b.user))
            return false;
        if (b.prop_begin > t.properties.size() || b.prop_count > t.properties.size() - b.prop_begin)
            return false;
        for (uint32_t k = 0; k < a.prop_count; ++k) {
            const StoredProperty& p = h.properties[a.prop_begin + k];
            const StoredProperty& q = t.properties[b.prop_begin + k];
            if (p.type != q.type || p.i != q.i || !same_bits(p.d[0], q.d[0]) ||
                !same_bits(p.d[1], q.d[1]) || !same_bits(p.d[2], q.d[2]) ||
                !same_name(h, p.key, t, q.key))
                return false;
            const bool text = p.type == 4 || p.type == 9;
            if (text && !same_text(h.strings, p.text, t.strings, q.text)) return false;
            if (p.type == 8 && !same_name(h, p.text, t, q.text)) return false;
            if (!text && p.type != 8 && (p.text != kNone || q.text != kNone)) return false;
        }
    }
    return true;
}

std::string tab_name(const NodeTabs& t, size_t j) {
    const StoredTab& tab = t.tabs[j];
    return std::string(t.strings.get(tab.name));
}

// --- Den syntetiska grafen ------------------------------------------------------------

// Toppnoden äger fliken T1. A har en egen flik T2 (namnparet och nyckeln är
// bakåtpekare till T1:s) och tar rot 0. B delar T1 och T2 genom bakåtpekare. C
// har inga flikar. D har en länk vars mål är en ny flik T3; E delar T3. F tar
// rot 1, vars nyckel pekar bakåt på ett namnpar i rot 0. G har en länk till en
// ny nod H som tar rot 2, och tar själv rot 3 (ström 1).
struct Graph {
    Bytes tree;
    Streams props;
    size_t t2_type = 0;     // byten där T2:s egenskapstyp står i trädet
    size_t p1_value = 0;    // byten där P1:s första värde står i ström 0
};

Graph graph() {
    Graph g;
    ByteWriter w;
    header(w, {2, 2}, 1);
    node_head(w, 53, "Top", 1);
    int32_t t1_user = 0;
    const int32_t t1 = tab_with_ints(w, "T1", {1, 2}, &t1_user);
    w.i32(7);   // barnen

    // A: T2 och en null-plats.
    node_head(w, 22, "A", 2);
    const int32_t t2 = w.obj(86);
    w.str("T2");
    w.ref(t1_user);
    w.i32(3);
    w.i32(2);
    w.ref(t1_user);
    g.t2_type = w.data.size();
    w.i32(4);
    w.str("text");
    name_pair(w, "Key8", "LcKey8");
    w.i32(8);
    w.ref(t1_user);
    w.null_ptr();

    // B: T1 och T2 genom bakåtpekare.
    node_head(w, 22, "B", 2);
    w.ref(t1);
    w.ref(t2);

    // C: inga attribut.
    node_head(w, 22, "C", 0);

    // D: en länk till en ny flik T3.
    node_head(w, 22, "D", 1);
    w.obj(79);
    w.str("L");
    w.i32(0);
    w.i32(0);
    w.i32(1);
    w.str("n");
    w.str("u");
    const int32_t t3 = tab_with_ints(w, "T3", {33});
    w.i32(0);

    // E: T3 genom en bakåtpekare.
    node_head(w, 22, "E", 1);
    w.ref(t3);

    // F: en null-plats.
    node_head(w, 22, "F", 1);
    w.null_ptr();

    // G: en länk till noden H med en null-plats, och en egen null-plats.
    node_head(w, 22, "G", 2);
    w.obj(79);
    w.str("L");
    w.i32(0);
    w.i32(0);
    w.i32(1);
    w.str("n");
    w.str("u");
    node_head(w, 22, "H", 1);
    w.null_ptr();
    w.i32(0);
    w.null_ptr();
    finish(w);
    g.tree = w.data;

    // Ström 0: P1 med nyckeln K, P2 vars nyckel pekar bakåt på K.
    ByteWriter s0;
    int32_t k = 0;
    s0.obj(86);
    s0.str("P1");
    k = name_pair(s0, "K", "LcK");
    s0.i32(0);
    s0.i32(1);
    s0.ref(k);
    s0.i32(1);
    g.p1_value = s0.data.size();
    s0.f64(1.5);
    s0.obj(86);
    s0.str("P2");
    s0.ref(k);
    s0.i32(0);
    s0.i32(2);
    s0.ref(k);
    s0.i32(12);
    s0.f64(1);
    s0.f64(2);
    s0.f64(3);
    s0.ref(k);
    s0.i32(5);
    s0.i64a(1700000000);
    s0.pad(8);
    g.props.push_back(s0.data);

    // Ström 1: Q1 (tas av H) och ett element-id Q2 (tas av G).
    ByteWriter s1;
    tab_with_ints(s1, "Q1", {7});
    s1.obj(66);
    s1.null_str();
    name_pair(s1, "Id", "LcRevitId");
    s1.i32(0x01000000);
    s1.i64a(4242);
    s1.pad(8);
    g.props.push_back(s1.data);
    return g;
}

struct Parsed {
    Hierarchy full;
    Hierarchy structure;
    HierarchySkeleton skeleton;
};

bool parse_both(const TreeFile& f, uint64_t every, Parsed& p) {
    std::string detail;
    const ErrorCode a = parse_hierarchy(f.file.data(), f.file.size(), f.streams, 0, kBig,
                                        HierarchyContent::Full, p.full, nullptr, {}, &detail);
    if (a != ErrorCode::Ok) std::printf("  Full: %s %s\n", error_message(a), detail.c_str());
    const ErrorCode b =
        parse_hierarchy(f.file.data(), f.file.size(), f.streams, 0, kBig, HierarchyContent::Structure,
                        p.structure, &p.skeleton, {}, &detail, every);
    if (b != ErrorCode::Ok) std::printf("  Structure: %s %s\n", error_message(b), detail.c_str());
    return a == ErrorCode::Ok && b == ErrorCode::Ok;
}

// Den lagrade noden med namnet `name`.
uint32_t node_named(const Hierarchy& h, const char* name) {
    for (size_t i = 0; i < h.nodes.size(); ++i)
        if (h.strings.get(h.nodes[i].name) == name) return static_cast<uint32_t>(i);
    return kNone;
}

// Varje nod, i ordningen `order`, med en läsare som återanvänds.
int check_every_node(const TreeFile& f, const Parsed& p, const std::vector<uint32_t>& order) {
    int fails = 0;
    TabReader reader(f.file.data(), f.file.size(), f.streams, p.structure, p.skeleton);
    for (const uint32_t i : order) {
        NodeTabs t;
        std::string detail;
        const ErrorCode e = reader.read(i, t, &detail);
        if (e != ErrorCode::Ok || !same_tabs(p.full, i, t)) {
            std::printf("  node %u: %s %s\n", i, error_message(e), detail.c_str());
            ++fails;
        }
    }
    return fails;
}

// --- Exempelfilerna -----------------------------------------------------------------

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
    return tree && locate_hierarchy(s.file.data(), s.file.size(), *tree, props,
                                    kDefaultMaxStreamBytes, s.streams, &detail) == ErrorCode::Ok;
}

double seconds_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

// Ett träd med en toppnod och en nod av klass 22 per namn, utan attribut.
Bytes flat_tree(const std::vector<std::string>& names) {
    ByteWriter w;
    header(w, {0}, 1);
    w.obj(53);
    w.str("top");
    w.null_ptr();
    w.i32(0);
    w.i32(0);
    w.i32(0);
    w.i32(static_cast<int32_t>(names.size()));
    for (const std::string& n : names) {
        w.obj(22);
        w.str(n);
        w.null_ptr();
        w.i32(0);
        w.i32(0);
        w.i32(0);
    }
    finish(w);
    return w.data;
}

// Nodernas namn, sorterade.
std::vector<std::string> sorted_names(const Hierarchy& h) {
    std::vector<std::string> out;
    for (const StoredNode& n : h.nodes) out.emplace_back(h.strings.get(n.name));
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

int test_tab_reader() {
    int fails = 0;

    // Den syntetiska grafen: varje nod lika med Full, med tätt och glest mellan
    // kontrollpunkterna, i ordning, baklänges och en i taget.
    {
        const Graph g = graph();
        for (const size_t block : {size_t(8), size_t(64), size_t(65535)}) {
            for (const uint64_t every : {uint64_t(1), uint64_t(40), kCheckpointEvery}) {
                const TreeFile f = tree_file(g.tree, g.props, block);
                Parsed p;
                if (!parse_both(f, every, p)) {
                    ++fails;
                    continue;
                }
                CHECK(p.structure.nodes.size() == 9);   // Top, A … G och H
                std::vector<uint32_t> order(p.structure.nodes.size());
                std::iota(order.begin(), order.end(), 0u);
                fails += check_every_node(f, p, order);
                std::reverse(order.begin(), order.end());
                fails += check_every_node(f, p, order);
                for (const uint32_t i : order) {
                    NodeTabs t;
                    CHECK(read_node_tabs(f.file.data(), f.file.size(), f.streams, p.structure,
                                         p.skeleton, i, t) == ErrorCode::Ok);
                    CHECK(same_tabs(p.full, i, t));
                }
            }
        }

        // Vad noderna har, uttryckligen.
        const TreeFile f = tree_file(g.tree, g.props, 16);
        Parsed p;
        CHECK(parse_both(f, 1, p));
        auto tabs_of = [&](const char* name) {
            NodeTabs t;
            const uint32_t i = node_named(p.structure, name);
            std::string detail;
            if (i == kNone || read_node_tabs(f.file.data(), f.file.size(), f.streams, p.structure,
                                             p.skeleton, i, t, &detail) != ErrorCode::Ok)
                std::printf("  %s: %s\n", name, detail.c_str());
            return t;
        };
        const NodeTabs b = tabs_of("B");
        CHECK(b.tabs.size() == 2 && tab_name(b, 0) == "T1" && tab_name(b, 1) == "T2");
        if (b.tabs.size() == 2) {
            CHECK(b.tabs[0].prop_count == 2 && b.properties[b.tabs[0].prop_begin + 1].i == 2);
            const StoredTab& t2 = b.tabs[1];
            CHECK(t2.flags == 3 && t2.prop_count == 2);
            CHECK(t2.user < b.names.size() && b.strings.get(b.names[t2.user].display) == "T1");
            if (t2.prop_count == 2) {
                const StoredProperty& v = b.properties[t2.prop_begin];
                CHECK(v.type == 4 && b.strings.get(v.text) == "text");
                const StoredProperty& n = b.properties[t2.prop_begin + 1];
                CHECK(n.type == 8 && n.key < b.names.size() &&
                      b.strings.get(b.names[n.key].internal) == "LcKey8");
                CHECK(n.text < b.names.size() && b.strings.get(b.names[n.text].display) == "T1");
            }
        }
        CHECK(tabs_of("C").tabs.empty());
        CHECK(tabs_of("D").tabs.empty());   // länkens flik hör inte till noden
        const NodeTabs e = tabs_of("E");
        CHECK(e.tabs.size() == 1 && tab_name(e, 0) == "T3");
        const NodeTabs fn = tabs_of("F");
        CHECK(fn.tabs.size() == 1 && tab_name(fn, 0) == "P2");
        if (fn.tabs.size() == 1 && fn.tabs[0].prop_count == 2) {
            const StoredProperty& pt = fn.properties[fn.tabs[0].prop_begin];
            CHECK(pt.type == 12 && pt.d[2] == 3);
            CHECK(pt.key < fn.names.size() && fn.strings.get(fn.names[pt.key].display) == "K");
            CHECK(fn.properties[fn.tabs[0].prop_begin + 1].i == 1700000000);
        }
        const NodeTabs gn = tabs_of("G");
        CHECK(gn.tabs.size() == 1 && gn.tabs[0].source_class == 66);
        if (gn.tabs.size() == 1) CHECK(gn.properties[gn.tabs[0].prop_begin].i == 4242);
        const NodeTabs hn = tabs_of("H");
        CHECK(hn.tabs.size() == 1 && tab_name(hn, 0) == "Q1");

        // Skelettet: B:s bakåtpekare till T1 och T2 ger två flikar med position, E:s en.
        size_t tab_refs = 0;
        for (uint32_t t = 0; t < p.skeleton.refs.stream_count(); ++t) {
            std::vector<IdRef> list;
            CHECK(p.skeleton.refs.list(t, list));
            for (const IdRef& r : list) tab_refs += r.cls == 86 || r.cls == 66 || r.cls == 70 || r.cls == 76;
        }
        CHECK(tab_refs == 3);
        CHECK(p.skeleton.roots.size() == 4);
        CHECK(p.skeleton.first_root.size() == 2 && p.skeleton.first_root[1] == 2);

        // En nod som inte finns.
        NodeTabs t;
        std::string detail;
        CHECK(read_node_tabs(f.file.data(), f.file.size(), f.streams, p.structure, p.skeleton, 9, t,
                             &detail) == ErrorCode::OutOfRange);
        CHECK(!detail.empty() && t.tabs.empty());
    }

    // Fel i strömmen: fel kommer med strömmens namn och byte, och ingenting lämnas ut.
    {
        const Graph g = graph();
        const TreeFile good = tree_file(g.tree, g.props, 16);
        Parsed p;
        CHECK(parse_both(good, 1, p));
        const uint32_t a = node_named(p.structure, "A");
        const uint32_t b = node_named(p.structure, "B");
        const uint32_t fnode = node_named(p.structure, "F");
        auto read = [&](const TreeFile& f, uint32_t node, const HierarchySkeleton& sk,
                        const Hierarchy& h, std::string& detail) {
            NodeTabs t;
            t.tabs.resize(1);   // ska tömmas
            const ErrorCode e =
                read_node_tabs(f.file.data(), f.file.size(), f.streams, h, sk, node, t, &detail);
            if (e != ErrorCode::Ok && !t.tabs.empty()) return ErrorCode::Ok;   // fel: resultat kvar
            return e;
        };
        std::string detail;

        // T2:s egenskapstyp blir 99: A och B (som delar T2) ger fel; C går.
        TreeFile f = good;
        f.file[file_byte(f, f.streams.tree, g.t2_type)] = 99;
        CHECK(read(f, a, p.skeleton, p.structure, detail) == ErrorCode::CorruptObject);
        CHECK(detail.rfind("LcOpNwdLogicalHierarchy at byte ", 0) == 0);
        CHECK(detail.find("property value type is unknown") != std::string::npos);
        CHECK(read(f, b, p.skeleton, p.structure, detail) == ErrorCode::CorruptObject);
        CHECK(read(f, node_named(p.structure, "C"), p.skeleton, p.structure, detail) == ErrorCode::Ok);

        // Huvudet på det lagrade block där T2 ligger går inte ihop (LEN mot NLEN).
        f = good;
        f.file[static_cast<size_t>(f.streams.tree.offset) + 2 + (g.t2_type / 16) * (16 + 5) + 1] ^= 0xFF;
        CHECK(read(f, a, p.skeleton, p.structure, detail) == ErrorCode::CorruptStream);
        CHECK(detail.rfind("LcOpNwdLogicalHierarchy at byte ", 0) == 0);

        // En byte i ström 0 före F:s rot: utfyllnaden före P1:s värde är inte noll.
        f = good;
        f.file[file_byte(f, f.streams.props[0], g.p1_value)] = 1;
        if (g.p1_value % 8 == 4) {
            CHECK(read(f, a, p.skeleton, p.structure, detail) == ErrorCode::CorruptObject);
            CHECK(detail.rfind("LcOaPartitionProps stream 0 at byte ", 0) == 0);
        }
        // F:s rot ligger efter den förstörda byten och läses från sin egen position.
        CHECK(read(f, fnode, p.skeleton, p.structure, detail) == ErrorCode::Ok);

        // Ett skelett utan listan över bakåtpekare: B:s delade flikar går inte att lösa.
        HierarchySkeleton bare = p.skeleton;
        bare.refs = PackedRefs();
        CHECK(read(good, b, bare, p.structure, detail) == ErrorCode::CorruptObject);
        CHECK(detail.find("back-reference to an unknown id") != std::string::npos);
        // Ett skelett vars antal flikar inte stämmer med strömmen.
        NodeLocation loc = location_of(p.skeleton, b);
        ++loc.tab_count;
        const HierarchySkeleton wrong = with_location(p.skeleton, b, loc);
        CHECK(read(good, b, wrong, p.structure, detail) == ErrorCode::CorruptLink);
        // En position utanför strömmen.
        loc = location_of(p.skeleton, b);
        loc.attr_pos = 1u << 30;
        const HierarchySkeleton distant = with_location(p.skeleton, b, loc);
        CHECK(read(good, b, distant, p.structure, detail) == ErrorCode::OutOfRange);
        // Rötter som skelettet inte har.
        HierarchySkeleton no_roots = p.skeleton;
        no_roots.roots.clear();
        CHECK(read(good, fnode, no_roots, p.structure, detail) == ErrorCode::CorruptLink);
    }

    // Namn valda så att deras hash har bitarna 11..21 noll, och var fjärde en
    // dubblett så att dubblettsökningen står på efter sitt prov. Utan
    // blandning och tak i dubblettabellen hamnar alla i samma kluster och
    // tiden växer med kvadraten på antalet. Trädet läses på under en sekund i
    // båda formerna, och namnen står som de skrevs.
    {
        const size_t n = 60000;
        const std::hash<std::string_view> hasher;
        std::vector<std::string> names;
        names.reserve(n);
        char buf[32];
        for (uint64_t c = 0; names.size() < n; ++c) {
            const int len = std::snprintf(buf, sizeof buf, "n%011llx", static_cast<unsigned long long>(c));
            const std::string_view s(buf, static_cast<size_t>(len));
            if (((hasher(s) >> 11) & 0x7FF) == 0) names.emplace_back(s);
        }
        for (size_t i = 0; i < n; i += 4) names[i] = "dup";
        std::vector<std::string> expected = names;
        expected.push_back("top");
        std::sort(expected.begin(), expected.end());

        TreeFile f = tree_file(flat_tree(names), {}, 65535);
        f.streams.has_props = false;
        for (const HierarchyContent content : {HierarchyContent::Full, HierarchyContent::Structure}) {
            Hierarchy h;
            HierarchySkeleton sk;
            std::string detail;
            const auto t0 = std::chrono::steady_clock::now();
            const ErrorCode e = parse_hierarchy(f.file.data(), f.file.size(), f.streams, 0, kBig, content, h,
                                                content == HierarchyContent::Structure ? &sk : nullptr, {},
                                                &detail);
            const double s = seconds_since(t0);
            if (e != ErrorCode::Ok) std::printf("  flood: %s %s\n", error_message(e), detail.c_str());
            CHECK(e == ErrorCode::Ok);
            CHECK(sorted_names(h) == expected);
            if (s >= 1.0) std::printf("  flood: %zu names took %.2f s\n", n, s);
            CHECK(s < 1.0);
        }
    }

    // Positionerna i skelettet har 32 bitar. En ström längre än så går att
    // läsa i Full, som inte lagrar några; i Structure ger den LimitExceeded
    // med en text som säger det. Gränsen sänks här till 16 byte.
    {
        TreeFile f = tree_file(flat_tree({"a", "b"}), {}, 65535);
        f.streams.has_props = false;
        f.streams.max_position = 16;
        Hierarchy full, structure;
        HierarchySkeleton sk;
        std::string detail;
        const ErrorCode a = parse_hierarchy(f.file.data(), f.file.size(), f.streams, 0, kBig,
                                            HierarchyContent::Full, full, nullptr, {}, &detail);
        if (a != ErrorCode::Ok) std::printf("  max_position, Full: %s\n", detail.c_str());
        CHECK(a == ErrorCode::Ok && full.nodes.size() == 3);
        CHECK(parse_hierarchy(f.file.data(), f.file.size(), f.streams, 0, kBig, HierarchyContent::Structure,
                              structure, &sk, {}, &detail) == ErrorCode::LimitExceeded);
        CHECK(detail.find("over 4 GiB") != std::string::npos &&
              detail.find("TreeContent::Full") != std::string::npos);
        f.streams.max_position = UINT32_MAX;
        CHECK(parse_hierarchy(f.file.data(), f.file.size(), f.streams, 0, kBig, HierarchyContent::Structure,
                              structure, &sk, {}, &detail) == ErrorCode::Ok);
    }

    // Exempelfilerna: varje nod lika med Full.
    for (const testdata::SampleCounts& sc : testdata::kSamples) {
        SKIP_IF_MISSING(sc.path);
        Sample s;
        if (!load_sample(sc.path, s)) {
            std::printf("  FAIL — %s kan inte öppnas\n", sc.path);
            ++fails;
            continue;
        }
        Hierarchy full;
        Hierarchy structure;
        HierarchySkeleton sk;
        std::string detail;
        CHECK(parse_hierarchy(s.file.data(), s.file.size(), s.streams, UINT32_MAX, kBig,
                              HierarchyContent::Full, full, nullptr, {}, &detail) == ErrorCode::Ok);
        CHECK(parse_hierarchy(s.file.data(), s.file.size(), s.streams, UINT32_MAX, kBig,
                              HierarchyContent::Structure, structure, &sk, {}, &detail) ==
              ErrorCode::Ok);
        if (full.nodes.size() != structure.nodes.size() || structure.nodes.empty()) {
            ++fails;
            continue;
        }

        // Alla noder i strömmens ordning, med en läsare.
        std::vector<uint32_t> order(structure.nodes.size());
        std::iota(order.begin(), order.end(), 0u);
        std::vector<uint32_t> attr_pos(order.size());
        for (const uint32_t i : order) attr_pos[i] = location_of(sk, i).attr_pos;
        std::sort(order.begin(), order.end(),
                  [&](uint32_t x, uint32_t y) { return attr_pos[x] < attr_pos[y]; });
        size_t bad = 0, tabs = 0, props = 0;
        TabReader reader(s.file.data(), s.file.size(), s.streams, structure, sk);
        auto t0 = std::chrono::steady_clock::now();
        for (const uint32_t i : order) {
            NodeTabs t;
            const ErrorCode e = reader.read(i, t, &detail);
            if (e != ErrorCode::Ok || !same_tabs(full, i, t)) {
                if (bad++ < 5)
                    std::printf("  %s: node %u: %s %s\n", sc.path, i, error_message(e), detail.c_str());
            }
            tabs += t.tabs.size();
            props += t.properties.size();
        }
        const double t_all = seconds_since(t0);
        CHECK(bad == 0);
        CHECK(tabs == full.node_tabs.size());

        // Var 50:e nod för sig, utan en läsare som står nära: från närmaste kontrollpunkt.
        size_t single = 0;
        t0 = std::chrono::steady_clock::now();
        for (size_t k = 0; k < structure.nodes.size(); k += 50) {
            NodeTabs t;
            const uint32_t i = static_cast<uint32_t>(k);
            if (read_node_tabs(s.file.data(), s.file.size(), s.streams, structure, sk, i, t) !=
                    ErrorCode::Ok ||
                !same_tabs(full, i, t))
                ++bad;
            ++single;
        }
        const double t_single = seconds_since(t0);
        CHECK(bad == 0);
        std::printf("  %s: %zu nodes, %zu tabs, %zu properties: all in order %.3f s (%.1f us/node); "
                    "%zu single reads %.3f s (%.2f ms each)\n",
                    sc.path, structure.nodes.size(), tabs, props, t_all,
                    1e6 * t_all / static_cast<double>(structure.nodes.size()), single, t_single,
                    single ? 1e3 * t_single / static_cast<double>(single) : 0.0);
    }

    std::printf("test_tab_reader: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
