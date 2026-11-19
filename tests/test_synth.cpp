// Den syntetiska filen (tools/synth_writer.h): skriven till en temporärfil
// genom samma kod som nwdsynth, öppnad och läst med hela det publika API:t.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "../tools/pose_check.h"
#include "../tools/synth_writer.h"
#include "HNwd.h"
#include "container.h"
#include "hierarchy.h"
#include "mapped_file.h"
#include "tab_reader.h"
#include "test_data.h"
#include "test_support.h"

using HNwd::Document;
using HNwd::Status;

namespace {

namespace hd = HNwd::detail;

// Tar bort filen när den går ur räckvidd. Dokument som öppnat den ska
// deklareras efter den, så att de stängs först.
struct Removed {
    std::string path;
    ~Removed() { testdata::remove_utf8(path); }
};

// Läser allt i filen och prövar det mot det som skrevs.
int check_file(const std::string& path, const synth::Options& o, const synth::Result& r) {
    int fails = 0;
    Document doc;
    CHECK(doc.open(path.c_str()) == Status::Ok);
    if (!doc.is_open()) {
        std::printf("  open: %s\n", doc.last_error());
        return fails;
    }
    HNwd::Unit unit;
    CHECK(doc.length_unit(unit) == Status::Ok);
    CHECK(unit.code == o.unit);

    CHECK(doc.load_fragments() == Status::Ok);
    CHECK(doc.fragment_count() == o.fragments);
    CHECK(doc.record_count() == o.records);

    CHECK(doc.load_spatial() == Status::Ok);
    CHECK(doc.spatial_from_file() == o.spatial);
    CHECK(doc.spatial_node_count() > 0);
    if (const uint32_t* items = doc.spatial_items()) {
        std::vector<uint8_t> seen(doc.fragment_count(), 0);
        bool permutation = true;
        for (size_t i = 0; i < doc.fragment_count(); ++i) {
            if (items[i] >= seen.size() || seen[items[i]]) permutation = false;
            else seen[items[i]] = 1;
        }
        CHECK(permutation);
    } else {
        CHECK(false);
    }

    // Varje fragments geometri, med lådan och antalet prövade av läsaren.
    HNwd::Mesh mesh;
    size_t bad = 0;
    uint64_t primitives = 0;
    for (size_t i = 0; i < doc.fragment_count(); ++i) {
        if (doc.fragment_mesh(i, mesh) != Status::Ok) {
            if (bad++ == 0) std::printf("  fragment %zu: %s\n", i, doc.last_error());
            continue;
        }
        primitives += mesh.indices.size() / (mesh.kind == HNwd::MeshKind::Lines ? 2 : 3);
    }
    CHECK(bad == 0);
    CHECK(primitives > 0);

    // Trädet i båda lägena: Structure först, sedan Full med flikarna.
    CHECK(doc.load_tree(HNwd::TreeContent::Structure) == Status::Ok);
    CHECK(doc.node_count() == o.nodes + 1);
    CHECK(!doc.has_tabs());
    CHECK(doc.guid_count() == r.leaves);
    HNwd::NodeProperties props;
    {
        HNwd::PropertyReader reader;
        CHECK(reader.open(doc) == Status::Ok);
        HNwd::Fragment f;
        CHECK(doc.fragment(0, f));
        CHECK(reader.read(f.seq, props) == Status::Ok);
        // Lövet har fliken i trädet, och det första lövet också en rot.
        CHECK(props.tabs.size() == 2);
    }
    CHECK(doc.load_tree(HNwd::TreeContent::Full) == Status::Ok);
    CHECK(doc.has_tabs());
    HNwd::Fragment f;
    CHECK(doc.fragment(0, f));
    CHECK(doc.tab_count(f.seq) == 2);
    HNwd::Node node;
    CHECK(doc.node(f.seq, node));
    CHECK(node.kind == HNwd::NodeKind::Leaf);
    CHECK(doc.model_count() == 1);

    // Poserna följer trädets transformer.
    HNwd::detail::MappedFile file;
    CHECK(file.open(path.c_str()) == HNwd::detail::ErrorCode::Ok);
    posecheck::Input in;
    std::string why;
    CHECK(posecheck::load(file.data(), file.size(), in, why) == HNwd::detail::ErrorCode::Ok);
    posecheck::Report report;
    posecheck::check(in, report);
    CHECK(report.unmatched == 0);
    CHECK(report.fragments == o.fragments);
    for (int slot = 0; slot < posecheck::kClassCount; ++slot)
        CHECK(report.stats[slot][0][0].count == r.poses[slot]);
    return fails;
}

// Postens form som den skrevs innan storlekarna och segmentantalet fanns:
// facit för att förvalen ger samma poster.
void reference_shape(uint64_t seed, uint64_t r, double per_metre, bool& lines, uint32_t& primitives,
                     std::vector<float>& positions) {
    const uint64_t h = synth::hash(seed, 1, r);
    lines = (h % 100) < 15;
    const uint32_t segments = 1 + static_cast<uint32_t>((h >> 8) % 12);
    const double length = (0.5 + 4.5 * synth::unit_random(synth::hash(seed, 2, r))) * per_metre;
    const double width = (0.1 + 0.9 * synth::unit_random(synth::hash(seed, 3, r))) * per_metre;
    positions.clear();
    if (lines) {
        primitives = segments;
        for (uint32_t i = 0; i <= segments; ++i) {
            positions.push_back(static_cast<float>(-length / 2 + length * i / segments));
            positions.push_back(static_cast<float>(width / 2 * std::sin(static_cast<double>(i))));
            positions.push_back(0.0f);
        }
    } else {
        primitives = 2 * segments;
        for (uint32_t i = 0; i <= segments; ++i) {
            const float x = static_cast<float>(-length / 2 + length * i / segments);
            const float z = static_cast<float>(0.05 * per_metre * std::sin(static_cast<double>(i)));
            for (const double y : {-width / 2, width / 2}) {
                positions.push_back(x);
                positions.push_back(static_cast<float>(y));
                positions.push_back(z);
            }
        }
    }
}

// Diagonalen på en låda (min xyz, max xyz) i meter, när längderna är i enheter
// om `per_metre` per meter.
double box_diagonal(const float lo[3], const float hi[3], double per_metre) {
    double sum = 0;
    for (int k = 0; k < 3; ++k) {
        const double d = static_cast<double>(hi[k]) - static_cast<double>(lo[k]);
        sum += d * d;
    }
    return std::sqrt(sum) / per_metre;
}

// Antalet segment som post r ska ha med högst `max_segments` ur fröet.
uint32_t expected_segments(uint64_t seed, uint64_t r, uint32_t max_segments) {
    return 1 + static_cast<uint32_t>((synth::hash(seed, 1, r) >> 8) % max_segments);
}

// Förvalen: varje post är den som skrevs förut.
int check_default_shapes() {
    int fails = 0;
    synth::Options o;
    o.seed = 7;
    CHECK(o.min_size == 0 && o.max_size == 0 && o.max_segments == 12);
    synth::RecordShape shape;
    size_t differing = 0;
    for (const double per_metre : {1.0, 1000.0}) {
        for (uint64_t r = 1; r <= 3000; ++r) {
            bool lines = false;
            uint32_t primitives = 0;
            std::vector<float> positions;
            reference_shape(o.seed, r, per_metre, lines, primitives, positions);
            synth::record_shape(o, r, per_metre, shape);
            if (shape.lines != lines || shape.primitives != primitives || shape.positions != positions)
                ++differing;
        }
    }
    CHECK(differing == 0);
    return fails;
}

// Storlekarna och segmentantalet: varje posts diagonal ligger inom intervallet
// och fördelar sig över det, varje post har så många segment som fröet ger, och
// filen med dem går att läsa helt.
int check_sizes_and_detail() {
    int fails = 0;
    synth::Options o;
    o.fragments = 1500;
    o.records = 500;
    o.nodes = 1200;
    o.seed = 3;
    o.min_size = 0.02;
    o.max_size = 40;
    o.max_segments = 100;
    constexpr double kPerMetre = 1000;   // enhetskod 2: millimeter

    // Formerna direkt: diagonalen är exakt den dragna storleken, och segmenten
    // följer fröet.
    {
        synth::RecordShape shape;
        double smallest = 1e300, largest = 0;
        uint32_t most_segments = 0;
        size_t outside = 0, wrong_count = 0;
        for (uint64_t r = 1; r <= o.records; ++r) {
            synth::record_shape(o, r, kPerMetre, shape);
            const double d = box_diagonal(shape.bbox, shape.bbox + 3, kPerMetre);
            if (!(d >= o.min_size * (1 - 1e-5) && d <= o.max_size * (1 + 1e-5))) ++outside;
            smallest = std::min(smallest, d);
            largest = std::max(largest, d);
            const uint32_t segments = expected_segments(o.seed, r, o.max_segments);
            if (shape.primitives != (shape.lines ? segments : 2 * segments)) ++wrong_count;
            most_segments = std::max(most_segments, segments);
        }
        CHECK(outside == 0);
        CHECK(wrong_count == 0);
        // Log-likformigt: båda ändarna av intervallet används, knappt en
        // tredjedel av det i logaritmen vid var ände.
        CHECK(smallest < 0.2);
        CHECK(largest > 4);
        CHECK(most_segments > 12);
    }

    // Filen: varje fragments postlåda ligger inom intervallet, antalet
    // primitiver når över förvalets tak och allt går att läsa (check_file
    // avkodar varje post och prövar dess låda och antal mot fragmentet).
    {
        Removed file{testdata::tmp_path("hnwd_test_synth_sizes.nwd")};
        synth::Result r;
        std::string why;
        CHECK(synth::write(file.path, o, r, why));
        if (!why.empty()) std::printf("  write: %s\n", why.c_str());
        fails += check_file(file.path, o, r);

        Document doc;
        CHECK(doc.open(file.path.c_str()) == Status::Ok);
        CHECK(doc.load_fragments() == Status::Ok);
        size_t outside = 0;
        uint32_t most_primitives = 0;
        for (size_t i = 0; i < doc.fragment_count(); ++i) {
            HNwd::Fragment f;
            CHECK(doc.fragment(i, f));
            const double d = box_diagonal(f.bounds.min, f.bounds.max, kPerMetre);
            if (!(d >= o.min_size * (1 - 1e-5) && d <= o.max_size * (1 + 1e-5))) ++outside;
            most_primitives = std::max(most_primitives, f.primitives);
            CHECK(f.primitives <= 2 * o.max_segments);
        }
        CHECK(outside == 0);
        CHECK(most_primitives > 24);
    }

    // Ett segment per post: en linje eller två trianglar.
    {
        synth::Options one = o;
        one.max_segments = 1;
        synth::RecordShape shape;
        size_t wrong = 0;
        for (uint64_t r = 1; r <= one.records; ++r) {
            synth::record_shape(one, r, kPerMetre, shape);
            if (shape.primitives != (shape.lines ? 1u : 2u)) ++wrong;
        }
        CHECK(wrong == 0);
    }
    return fails;
}

// --- Egenskaperna: flera strömmar, delade flikar och upprepade delträd ----------

void append_double(std::string& s, double d) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%a|", d);
    s += buf;
}

// Allt i flikarna och egenskaperna som text, så att två läsningar kan jämföras.
std::string signature(const HNwd::NodeProperties& p) {
    std::string s;
    size_t k = 0;
    for (const HNwd::Tab& t : p.tabs) {
        s += "T:";
        s += t.name;
        s += '|';
        s += t.internal;
        s += '|' + std::to_string(t.property_count) + '\n';
        for (uint32_t j = 0; j < t.property_count && k < p.properties.size(); ++j, ++k) {
            const HNwd::Property& q = p.properties[k];
            s += " P:";
            s += q.name;
            s += '|';
            s += q.internal;
            s += '|' + std::to_string(q.raw_type) + '|' + std::to_string(static_cast<int>(q.type)) + '|';
            for (const double d : q.d) append_double(s, d);
            s += std::to_string(q.i) + '|';
            s += q.text;
            s += '\n';
        }
    }
    s += "#" + std::to_string(p.properties.size()) + "/" + std::to_string(k);
    return s;
}

// Samma sak ur Document::tab() och property() (bara med Full).
std::string doc_signature(const Document& doc, uint32_t node) {
    HNwd::NodeProperties p;
    const size_t tabs = doc.tab_count(node);
    for (size_t t = 0; t < tabs; ++t) {
        HNwd::Tab tab;
        if (!doc.tab(node, t, tab)) return "tab missing";
        p.tabs.push_back(tab);
        for (size_t j = 0; j < tab.property_count; ++j) {
            HNwd::Property q;
            if (!doc.property(node, t, j, q)) return "property missing";
            p.properties.push_back(q);
        }
    }
    return signature(p);
}

// Det expanderade trädet i preorder som lagrade noder, utan den
// underförstådda roten: en nod så många gånger den sitter i trädet.
void expanded_order(const hd::Hierarchy& h, std::vector<uint32_t>& order) {
    struct Frame {
        uint32_t node, next, count;
    };
    std::vector<Frame> stack;
    for (const uint32_t top : h.top) {
        order.push_back(top);
        stack.push_back({top, 0, h.child_count(top)});
        while (!stack.empty()) {
            Frame& f = stack.back();
            if (f.next == f.count) {
                stack.pop_back();
                continue;
            }
            const uint32_t child = h.children[h.nodes[f.node].child_begin + f.next];
            ++f.next;
            order.push_back(child);
            stack.push_back({child, 0, h.child_count(child)});
        }
    }
}

// Filen genom core: strömmarnas rotantal, och vad TabReader packar upp när
// noderna läses i lagrad ordning och i expanderad ordning med förvald cache.
int check_core_reads(const std::string& path, const synth::Options& o, const synth::Result& r) {
    int fails = 0;
    hd::MappedFile file;
    hd::Container container;
    std::string detail;
    CHECK(file.open(path.c_str()) == hd::ErrorCode::Ok);
    if (!file.is_open()) return fails;
    CHECK(hd::read_container(file.data(), file.size(), container, &detail) == hd::ErrorCode::Ok);
    const hd::DbCacheEntry* tree = hd::find_entry(container.entries, "LcOpNwdLogicalHierarchy");
    const hd::DbCacheEntry* props = hd::find_entry(container.entries, "LcOaPartitionProps");
    CHECK(tree != nullptr && props != nullptr);
    if (!tree || !props) return fails;
    hd::HierarchyStreams streams;
    CHECK(hd::locate_hierarchy(file.data(), file.size(), *tree, props, hd::kDefaultMaxStreamBytes, streams,
                               &detail) == hd::ErrorCode::Ok);
    CHECK(streams.props.size() == o.prop_streams);

    hd::Hierarchy h;
    hd::HierarchySkeleton skeleton;
    const hd::ErrorCode parsed =
        hd::parse_hierarchy(file.data(), file.size(), streams, static_cast<uint32_t>(r.leaves),
                            uint64_t(1) << 23, hd::HierarchyContent::Structure, h, &skeleton, {}, &detail);
    CHECK(parsed == hd::ErrorCode::Ok);
    if (parsed != hd::ErrorCode::Ok) {
        std::printf("  parse_hierarchy: %s %s\n", hd::error_message(parsed), detail.c_str());
        return fails;
    }

    // Rötterna fördelade i följd, lika många som möjligt i varje ström.
    const uint64_t k = o.prop_streams;
    CHECK(h.root_counts.size() == k);
    uint64_t counted = 0;
    for (size_t s = 0; s < h.root_counts.size(); ++s) {
        const uint64_t want = r.property_roots / k + (s < r.property_roots % k ? 1 : 0);
        CHECK(static_cast<uint64_t>(h.root_counts[s]) == want);
        counted += static_cast<uint64_t>(h.root_counts[s]);
    }
    CHECK(counted == r.property_roots);
    CHECK(h.nodes.size() == o.nodes + (o.instances > 0 ? 1 : 0));
    CHECK(h.top.size() == (o.instances > 0 ? 2u : 1u));
    CHECK(h.expanded_total == r.expanded_nodes);

    std::vector<uint32_t> order;
    expanded_order(h, order);
    CHECK(order.size() + 1 == r.expanded_nodes);
    std::vector<uint8_t> copies(h.nodes.size(), 0);   // förekomster, högst 2
    for (const uint32_t n : order)
        if (copies[n] < 2) ++copies[n];

    constexpr size_t kCache = size_t(16) << 20;
    hd::NodeTabs tabs;

    // Varje lagrad nod en gång, stigande.
    hd::TabReaderStats sequential;
    {
        hd::TabReader reader(file.data(), file.size(), streams, h, skeleton, kCache);
        size_t failed = 0;
        for (size_t i = 0; i < h.nodes.size(); ++i)
            if (reader.read(static_cast<uint32_t>(i), tabs, &detail) != hd::ErrorCode::Ok) ++failed;
        CHECK(failed == 0);
        CHECK(reader.cached_bytes() <= kCache);
        sequential = reader.stats();
    }
    CHECK(sequential.reads == h.nodes.size());
    CHECK(sequential.inflated > 0);
    CHECK(sequential.opens[2] == 0);

    // Varje förekomst i expanderad ordning, med en nod som sitter på flera ställen läst med keep.
    hd::TabReaderStats expanded;
    {
        hd::TabReader reader(file.data(), file.size(), streams, h, skeleton, kCache);
        size_t failed = 0;
        for (const uint32_t n : order)
            if (reader.read(n, tabs, &detail, copies[n] > 1) != hd::ErrorCode::Ok) ++failed;
        CHECK(failed == 0);
        CHECK(reader.cached_bytes() <= kCache);
        expanded = reader.stats();
    }
    CHECK(expanded.reads == order.size());
    CHECK(expanded.node_hits == order.size() - h.nodes.size());
    CHECK(expanded.opens[2] == 0);
    CHECK(static_cast<double>(expanded.inflated) <=
          1.1 * static_cast<double>(sequential.inflated) + static_cast<double>(size_t(1) << 20));
    std::printf("  core: %zu stored nodes, %zu occurrences; inflated %llu in stored order, %llu in "
                "expanded order; %llu node hits, %llu shared tab hits\n",
                h.nodes.size(), order.size(), static_cast<unsigned long long>(sequential.inflated),
                static_cast<unsigned long long>(expanded.inflated),
                static_cast<unsigned long long>(expanded.node_hits),
                static_cast<unsigned long long>(expanded.tab_hits));
    return fails;
}

// Filen genom det publika API:t: trädet i båda lägena, och PropertyReader i
// Structure lika med Full för varje nod i det expanderade trädet.
int check_properties(const std::string& path, const synth::Options& o, const synth::Result& r) {
    int fails = 0;
    CHECK(r.leaves + r.groups == o.nodes);
    uint64_t refs = 0;
    if (o.common_tab) refs += r.leaves - 1;
    if (o.shared_tabs) refs += r.leaves - (r.leaves + o.shared_tabs - 1) / o.shared_tabs;
    CHECK(r.shared_tab_refs == refs);
    CHECK(r.expanded_nodes >= o.nodes + 1 + (o.instances > 0 ? 1 + o.instances : 0));

    Document st, full;
    const bool loaded = st.open(path.c_str()) == Status::Ok && full.open(path.c_str()) == Status::Ok &&
                        st.load_tree(HNwd::TreeContent::Structure) == Status::Ok &&
                        full.load_tree(HNwd::TreeContent::Full) == Status::Ok;
    CHECK(loaded);
    if (!loaded) {
        std::printf("  open: %s / %s\n", st.last_error(), full.last_error());
        return fails;
    }
    const size_t n = full.node_count();
    CHECK(n == r.expanded_nodes && st.node_count() == n);
    CHECK(full.guid_count() == r.leaves);

    // Huvudträdet, och den andra toppnoden med sina barn.
    const uint32_t first = full.first_child(0);
    const uint32_t second = first == HNwd::kNone ? HNwd::kNone : full.next_sibling(first);
    HNwd::Node top;
    CHECK(first != HNwd::kNone && full.node(first, top) && top.subtree_size == o.nodes);
    if (o.instances > 0) {
        CHECK(second != HNwd::kNone && full.node(second, top) && top.child_count == o.instances);
        CHECK(second == HNwd::kNone || full.next_sibling(second) == HNwd::kNone);
    } else {
        CHECK(second == HNwd::kNone);
    }

    HNwd::PropertyReader reader;
    CHECK(reader.open(st) == Status::Ok);
    const size_t base =
        static_cast<size_t>((o.common_tab ? 1 : 0) + (o.shared_tabs ? 1 : 0) + 1);   // flikar utan rot
    size_t leaves = 0, rooted = 0, differing = 0, wrong = 0;
    HNwd::NodeProperties props;
    for (uint32_t i = 0; i < n; ++i) {
        if (reader.read(i, props) != Status::Ok || signature(props) != doc_signature(full, i)) {
            if (differing++ < 3) std::printf("  node %u: %s\n", i, reader.last_error());
        }
        HNwd::Node node;
        if (!full.node(i, node)) {
            ++wrong;
            continue;
        }
        const size_t tab_count = full.tab_count(i);
        if (node.kind != HNwd::NodeKind::Leaf) {
            if (tab_count != 0) ++wrong;
            continue;
        }
        // Flikarna i attributens ordning: gemensam, grupp, egen, och sist en rot.
        ++leaves;
        if (tab_count == base + 1) ++rooted;
        if (tab_count != base && tab_count != base + 1) {
            ++wrong;
            continue;
        }
        size_t at = 0;
        HNwd::Tab tab;
        const auto named = [&](const char* name) { return full.tab(i, at++, tab) && tab.name == name; };
        if (!((!o.common_tab || named("Common")) && (!o.shared_tabs || named("Shared")) && named("Item")))
            ++wrong;
    }
    CHECK(differing == 0);
    CHECK(wrong == 0);
    CHECK(leaves >= r.leaves && (o.instances > 0 || leaves == r.leaves));
    CHECK(rooted > 0 && rooted < leaves);
    fails += check_core_reads(path, o, r);
    return fails;
}

// Förvalen: en ström, inga delade flikar, ingen andra toppnod.
int check_default_layout() {
    int fails = 0;
    synth::Options o;
    o.fragments = 400;
    o.records = 100;
    o.nodes = 300;
    o.seed = 5;
    CHECK(o.prop_streams == 1 && o.shared_tabs == 0 && !o.common_tab && o.instances == 0);
    Removed file{testdata::tmp_path("hnwd_test_synth_layout.nwd")};
    synth::Result r;
    std::string why;
    CHECK(synth::write(file.path, o, r, why));
    CHECK(r.expanded_nodes == o.nodes + 1);
    CHECK(r.shared_tab_refs == 0);
    fails += check_properties(file.path, o, r);
    // Filen byte för byte: längden och en FNV-1a-summa över hela innehållet.
    {
        HNwd::detail::MappedFile mapped;
        CHECK(mapped.open(file.path.c_str()) == HNwd::detail::ErrorCode::Ok);
        CHECK(mapped.is_open() && mapped.size() == 140734);
        uint64_t sum = 1469598103934665603ull;
        for (size_t i = 0; mapped.is_open() && i < mapped.size(); ++i) {
            sum ^= mapped.data()[i];
            sum *= 1099511628211ull;
        }
        CHECK(sum == 0x907bd33940eb0163ull);
    }
    return fails;
}

// Delade flikar, flera egenskapsströmmar och upprepade delträd, var och en för
// sig och alla tillsammans, och en stor fil med allt.
int check_shared_and_repeated() {
    int fails = 0;
    synth::Options base;
    base.fragments = 600;
    base.records = 150;
    base.nodes = 1500;
    base.seed = 9;

    struct Variant {
        uint64_t streams, shared;
        bool common;
        uint64_t instances;
    };
    const Variant variants[] = {{3, 0, false, 0},   // bara flera strömmar
                                {1, 2, false, 0},   // varannan äger
                                {1, 5, false, 0},
                                {1, 0, true, 0},    // bara den gemensamma
                                {1, 0, false, 40},  // bara upprepningarna
                                {4, 3, true, 30}};  // allt
    int index = 0;
    for (const Variant& v : variants) {
        synth::Options o = base;
        o.prop_streams = v.streams;
        o.shared_tabs = v.shared;
        o.common_tab = v.common;
        o.instances = v.instances;
        Removed file{testdata::tmp_path("hnwd_test_synth_variant_" + std::to_string(index++) + ".nwd")};
        synth::Result r;
        std::string why;
        CHECK(synth::write(file.path, o, r, why));
        if (!why.empty()) std::printf("  write: %s\n", why.c_str());
        fails += check_properties(file.path, o, r);
    }

    // Stor nog för att trädets ström ska passera flera kontrollpunkter: fem
    // egenskapsströmmar, delade flikar, en gemensam flik och 200 upprepade delträd.
    synth::Options big;
    big.fragments = 3000;
    big.records = 400;
    big.nodes = 20000;
    big.seed = 11;
    big.prop_streams = 5;
    big.shared_tabs = 7;
    big.common_tab = true;
    big.instances = 200;
    Removed file{testdata::tmp_path("hnwd_test_synth_repeated.nwd")};
    synth::Result r;
    std::string why;
    CHECK(synth::write(file.path, big, r, why));
    if (!why.empty()) std::printf("  write: %s\n", why.c_str());
    CHECK(r.expanded_nodes > big.nodes + 1 + big.instances);
    fails += check_properties(file.path, big, r);
    return fails;
}

// Val som inte går ger ett fel och ingen fil.
int check_impossible_choices() {
    int fails = 0;
    synth::Options o;
    o.fragments = 200;
    o.records = 50;
    o.nodes = 300;
    o.seed = 4;
    uint64_t roots = 0;
    {
        Removed file{testdata::tmp_path("hnwd_test_synth_roots.nwd")};
        synth::Result r;
        std::string why;
        CHECK(synth::write(file.path, o, r, why));
        roots = r.property_roots;
        CHECK(roots > 1);
    }
    struct Bad {
        const char* what;
        uint64_t nodes, streams, shared, instances;
    };
    const Bad cases[] = {
        {"no streams", 300, 0, 0, 0},
        {"more streams than roots", 300, roots + 1, 0, 0},
        {"every leaf its own owner", 300, 1, 1, 0},
        {"more instances than a count holds", 300, 1, 0, 0x7FFFFFF1},
        // 16 löv och två tomma grupper under toppnoden: ingen grupp har bara löv som barn.
        {"instances without a group of leaves", 19, 1, 0, 1},
    };
    for (const Bad& c : cases) {
        synth::Options bad = o;
        bad.nodes = c.nodes;
        bad.prop_streams = c.streams;
        bad.shared_tabs = c.shared;
        bad.instances = c.instances;
        synth::Result r;
        std::string why;
        const std::string path = testdata::tmp_path("hnwd_test_synth_bad.nwd");
        const bool written = synth::write(path, bad, r, why);
        if (written) std::printf("  FAIL — %s was written\n", c.what);
        CHECK(!written);
        CHECK(!why.empty());
        CHECK(!testdata::exists(path.c_str()));
    }
    return fails;
}

} // namespace

int test_synth() {
    int fails = 0;
    synth::Options o;
    o.fragments = 2000;
    o.records = 700;
    o.nodes = 1500;
    o.seed = 7;

    // Med det rumsliga trädet, i millimeter.
    {
        Removed file{testdata::tmp_path("hnwd_test_synth.nwd")};
        synth::Result r;
        std::string why;
        CHECK(synth::write(file.path, o, r, why));
        if (!why.empty()) std::printf("  write: %s\n", why.c_str());
        // Varje poseklass förekommer, och alla fragment hamnade i ett löv.
        for (const uint64_t n : r.poses) CHECK(n > 0);
        CHECK(r.poses[0] + r.poses[1] + r.poses[2] + r.poses[3] + r.poses[4] == o.fragments);
        CHECK(r.chunks >= 2);
        CHECK(r.leaves + r.groups == o.nodes);
        fails += check_file(file.path, o, r);
    }
    // Utan det rumsliga trädet, i meter: läsaren bygger ett eget index.
    {
        o.spatial = false;
        o.unit = 0;
        Removed file{testdata::tmp_path("hnwd_test_synth_own.nwd")};
        synth::Result r;
        std::string why;
        CHECK(synth::write(file.path, o, r, why));
        fails += check_file(file.path, o, r);
    }
    // Samma frö ger samma fil.
    {
        o.spatial = true;
        o.unit = 2;
        Removed a{testdata::tmp_path("hnwd_test_synth_a.nwd")}, b{testdata::tmp_path("hnwd_test_synth_b.nwd")};
        synth::Result ra, rb;
        std::string why;
        CHECK(synth::write(a.path, o, ra, why));
        CHECK(synth::write(b.path, o, rb, why));
        HNwd::detail::MappedFile fa, fb;
        CHECK(fa.open(a.path.c_str()) == HNwd::detail::ErrorCode::Ok);
        CHECK(fb.open(b.path.c_str()) == HNwd::detail::ErrorCode::Ok);
        CHECK(fa.size() == fb.size() && fa.size() == ra.file_bytes);
        CHECK(fa.is_open() && fb.is_open() && fa.size() == fb.size() && std::memcmp(fa.data(), fb.data(), fa.size()) == 0);
    }
    // Fel val ger ett fel och ingen fil.
    {
        synth::Options bad = o;
        bad.unit = 4;
        synth::Result r;
        std::string why;
        CHECK(!synth::write(testdata::tmp_path("hnwd_test_synth_bad.nwd"), bad, r, why));
        CHECK(!why.empty());
        CHECK(!testdata::exists(testdata::tmp_path("hnwd_test_synth_bad.nwd").c_str()));
    }
    // Förvalens poster, och storlekarna och segmentantalet.
    fails += check_default_shapes();
    fails += check_sizes_and_detail();
    // Förvalens fil, delade flikar, flera egenskapsströmmar och upprepade
    // delträd, och val som inte går.
    fails += check_default_layout();
    fails += check_shared_and_repeated();
    fails += check_impossible_choices();
    // Ogiltiga storlekar och segmentantal ger ett fel och ingen fil.
    {
        struct Bad {
            double min_size, max_size;
            uint32_t max_segments;
        };
        const Bad cases[] = {{5, 1, 12},   // omvänt intervall
                             {0, 5, 12},   // bara den ena änden
                             {1, 0, 12},
                             {-1, 5, 12},
                             {0.5, std::numeric_limits<double>::infinity(), 12},   // inte ändligt
                             {0, 0, 0},    // inget segment
                             {0, 0, synth::kMaxSegments + 1}};
        for (const Bad& c : cases) {
            synth::Options bad = o;
            bad.min_size = c.min_size;
            bad.max_size = c.max_size;
            bad.max_segments = c.max_segments;
            synth::Result r;
            std::string why;
            const std::string path = testdata::tmp_path("hnwd_test_synth_bad.nwd");
            CHECK(!synth::write(path, bad, r, why));
            CHECK(!why.empty());
            CHECK(!testdata::exists(path.c_str()));
        }
    }
    std::printf("test_synth: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
