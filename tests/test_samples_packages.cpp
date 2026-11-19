#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "container.h"
#include "fragment_view.h"
#include "fragments.h"
#include "guid_store.h"
#include "hierarchy.h"
#include "mapped_file.h"
#include "partition_props.h"
#include "test_data.h"
#include "test_support.h"

using namespace HNwd::detail;

namespace {

constexpr uint64_t kMaxNodes = uint64_t(1) << 23;

// Paketets uppackade ström, för ett paket i formen (0, 0).
bool package_stream(const MappedFile& file, const Container& c, const char* name,
                    std::vector<uint8_t>& out) {
    const DbCacheEntry* e = find_entry(c.entries, name);
    StreamSpan span;
    return e && last_stream(*e, file.size(), span) == ErrorCode::Ok &&
           inflate_span(file.data(), file.size(), span, kDefaultMaxStreamBytes, out) ==
               ErrorCode::Ok;
}

// Fragmenten: antalet enligt tabellen, seq icke-avtagande och posten i 1..records.
int check_fragments(const testdata::SampleCounts& s, const std::vector<uint8_t>& data,
                    bool facts) {
    int fails = 0;
    FragmentStore table;
    std::vector<FragmentExtra> extra;
    std::string detail;
    const ErrorCode e = parse_fragments(data.data(), data.size(), table, &extra, &detail);
    if (e != ErrorCode::Ok) {
        std::printf("  FAIL — %s: fragment: %s %s\n", s.path, error_message(e), detail.c_str());
        return 1;
    }
    CHECK(table.items.size() == s.fragments);
    CHECK(extra.size() == table.items.size());

    int32_t previous = 0;
    size_t classes[3] = {}, no_pose = 0;
    size_t mirrored = 0;
    for (const FragmentView& f : fragment_views(table)) {
        CHECK(f.seq >= previous);
        previous = f.seq;
        CHECK(f.record >= 1 && static_cast<uint32_t>(f.record) <= s.records);
        CHECK(f.look < table.looks.size());
        CHECK(f.pose_class == 0 || (f.pose_class >= 14 && f.pose_class <= 16));
        if (f.pose_class == 0) ++no_pose;
        if (f.pose_class >= 14 && f.pose_class <= 16) ++classes[f.pose_class - 14];
        if (f.s < 0) ++mirrored;
    }
    if (facts)
        std::printf("  fragments %zu, looks %zu, pose classes 14:%zu 15:%zu 16:%zu, no pose %zu, mirrored %zu\n",
                    table.items.size(), table.looks.size(), classes[0], classes[1], classes[2],
                    no_pose, mirrored);
    return fails;
}

// Trädet med egenskapsströmmarna. Toppnoderna i en .nwd är klass 32.
int check_tree(const MappedFile& file, const Container& c, const testdata::SampleCounts& s,
               uint32_t guid_count, bool facts) {
    int fails = 0;
    std::vector<uint8_t> tree;
    CHECK(package_stream(file, c, "LcOpNwdLogicalHierarchy", tree));

    const DbCacheEntry* pe = find_entry(c.entries, "LcOaPartitionProps");
    CHECK(pe != nullptr);
    std::vector<std::vector<uint8_t>> props;
    std::string detail;
    if (pe) {
        const ErrorCode e = read_partition_props(file.data(), file.size(), *pe,
                                                 kDefaultMaxStreamBytes, props, &detail);
        if (e != ErrorCode::Ok) {
            std::printf("  FAIL — %s: props: %s %s\n", s.path, error_message(e), detail.c_str());
            return fails + 1;
        }
    }

    Hierarchy h;
    const ErrorCode e = parse_hierarchy(tree.data(), tree.size(), pe ? &props : nullptr,
                                        guid_count, kMaxNodes, h, &detail);

    if (e != ErrorCode::Ok) {
        std::printf("  FAIL — %s: tree: %s %s\n", s.path, error_message(e), detail.c_str());
        return fails + 1;
    }
    CHECK(detail.empty());
    int64_t roots = 0;
    for (const int32_t n : h.root_counts) roots += n;
    CHECK(roots == static_cast<int64_t>(s.property_roots));
    CHECK(h.null_slots == s.property_roots);
    CHECK(props.size() == h.root_counts.size());
    CHECK(h.top.size() == s.top_nodes);
    if (!h.top.empty()) CHECK(h.nodes[h.top[0]].cls == s.top_class);
    // Varje toppnod av klass 32 har en partitionssvans, och ingen annan nod har en.
    size_t partition_nodes = 0;
    for (uint32_t i = 0; i < h.nodes.size(); ++i) {
        const StoredNode& n = h.nodes[i];
        CHECK((n.cls == 32) == (h.partition(i) != kNone));
        if (n.cls == 32) {
            CHECK(h.partition(i) < h.partitions.size());
            ++partition_nodes;
        }
    }
    CHECK(partition_nodes == h.partitions.size());
    CHECK(partition_nodes == (s.top_class == 32 ? s.top_nodes : 0u));
    CHECK(h.expanded_total > 1);

    // Barnen och flikarna ligger i följd och ryms i sina listor.
    uint64_t children = 0, tabs = 0;
    CHECK(h.tab_begin.size() == h.nodes.size() + 1);
    for (uint32_t i = 0; i < h.nodes.size(); ++i) {
        CHECK(static_cast<uint64_t>(h.nodes[i].child_begin) + h.child_count(i) <= h.children.size());
        CHECK(static_cast<uint64_t>(h.tab_begin[i]) + h.tab_count(i) <= h.node_tabs.size());
        children += h.child_count(i);
        tabs += h.tab_count(i);
    }
    CHECK(children == h.children.size());
    CHECK(tabs == h.node_tabs.size());

    size_t tab66 = 0, materials = 0, no_user = 0, transform[3] = {};
    for (const StoredTab& t : h.tabs) tab66 += t.source_class == 66;
    for (const StoredNode& n : h.nodes) {
        materials += n.material != kNone;
        no_user += n.user == kNone;
    }
    for (uint32_t i = 0; i < h.transform_count(); ++i) {
        const int32_t cls = h.transform_class(i);
        if (cls >= 39 && cls <= 41) ++transform[cls - 39];
    }
    if (facts)
        std::printf("  tree: stored nodes %zu, expanded %llu, tabs %zu (class 66: %zu), properties %zu,"
                    " names %zu, nodes with material %zu, transforms 39:%zu 40:%zu 41:%zu,"
                    " nodes without name pair %zu, non-UTF-8 strings %zu\n",
                    h.nodes.size(), static_cast<unsigned long long>(h.expanded_total),
                    h.tabs.size(), tab66, h.properties.size(), h.names.size(), materials,
                    transform[0], transform[1], transform[2], no_user, h.non_utf8_strings);
    return fails;
}

int check_file(const testdata::SampleCounts& s) {
    int fails = 0;
    const bool facts = std::getenv("HNWD_FACTS") != nullptr;
    if (facts) std::printf("  %s\n", s.path);

    MappedFile file;
    CHECK(file.open(s.path) == ErrorCode::Ok);
    if (!file.is_open()) return fails;
    Container c;
    CHECK(read_container(file.data(), file.size(), c) == ErrorCode::Ok);

    std::vector<uint8_t> data;
    std::vector<Guid16> guids;
    if (package_stream(file, c, "LcOpGuidStore", data)) {
        std::string detail;
        const ErrorCode e = parse_guid_store(data.data(), data.size(), guids, &detail);
        CHECK(e == ErrorCode::Ok);
        CHECK(guids.size() == s.guids);
    } else {
        std::printf("  FAIL — %s: GUID-tabellen\n", s.path);
        ++fails;
    }

    if (package_stream(file, c, "LcOpNwdFragments", data)) {
        fails += check_fragments(s, data, facts);
    } else {
        std::printf("  FAIL — %s: fragmenten\n", s.path);
        ++fails;
    }

    fails += check_tree(file, c, s, static_cast<uint32_t>(guids.size()), facts);
    return fails;
}

} // namespace

int test_samples_packages() {
    SKIP_IF_MISSING(testdata::kSampleNwd);
    int fails = 0;
    for (const testdata::SampleCounts& s : testdata::kSamples) {
        if (!testdata::exists(s.path)) {
            std::printf("  SKIP — %s saknas\n", s.path);
            ++g_skipped;
            continue;
        }
        fails += check_file(s);
    }
    std::printf("test_samples_packages: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
