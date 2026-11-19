// Det rumsliga indexet i Document: filens och det egna, i alla tio
// exempelfilerna. Nodernas lådor rymmer barnens och fragmentens, primitiverna
// summeras, och varje fragment står i exakt ett löv.

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "HNwd.h"
#include "test_data.h"
#include "test_support.h"

using HNwd::Document;
using HNwd::kNone;
using HNwd::SpatialNode;
using HNwd::SpatialSource;
using HNwd::Status;

namespace {

bool inside(const HNwd::BoxD& inner, const HNwd::BoxD& outer) {
    for (int k = 0; k < 3; ++k)
        if (inner.min[k] < outer.min[k] || inner.max[k] > outer.max[k]) return false;
    return true;
}

bool touches(const HNwd::BoxD& a, const HNwd::BoxD& b, int k, bool low) {
    return low ? a.min[k] == b.min[k] : a.max[k] == b.max[k];
}

// Prövar indexet i ett dokument med inlästa fragment. `max_leaf` 0: inget tak.
int check_index(const Document& doc, const char* path, uint32_t max_leaf, uint32_t& leaves,
                uint32_t& depth) {
    int fails = 0;
    const size_t n = doc.fragment_count();
    const size_t nodes = doc.spatial_node_count();
    const uint32_t* items = doc.spatial_items();
    CHECK(nodes > 0 && items != nullptr);
    if (nodes == 0 || !items) return fails;

    std::vector<HNwd::BoxD> world(n);
    std::vector<uint32_t> prims(n);
    uint64_t all_prims = 0;
    for (size_t i = 0; i < n; ++i) {
        CHECK(doc.fragment_world_box(i, world[i]));
        HNwd::Fragment f;
        doc.fragment(i, f);
        prims[i] = f.primitives;
        all_prims += f.primitives;
    }

    // Varje fragment en gång.
    std::vector<uint8_t> seen(n, 0);
    size_t twice = 0;
    for (size_t k = 0; k < n; ++k) {
        if (items[k] >= n || seen[items[k]]++) ++twice;
    }
    CHECK(twice == 0);

    std::vector<uint32_t> level(nodes, 0);
    leaves = 0;
    depth = 0;
    size_t bad_box = 0, bad_prims = 0, bad_link = 0, bad_tight = 0, big_leaf = 0;
    for (uint32_t i = 0; i < nodes; ++i) {
        SpatialNode node;
        CHECK(doc.spatial_node(i, node));
        if (i == 0) {
            CHECK(node.parent == kNone && node.item_begin == 0 && node.item_count == n);
            CHECK(node.primitives == all_prims);
        } else {
            if (node.parent >= i) ++bad_link;   // preorder: föräldern kommer först
            else level[i] = level[node.parent] + 1;
        }
        if (level[i] > depth) depth = level[i];
        if (node.item_begin > n || node.item_count > n - node.item_begin) {
            ++bad_link;
            continue;
        }

        if (node.child_count == 0) {
            ++leaves;
            if (max_leaf && node.item_count > max_leaf) ++big_leaf;
            uint64_t sum = 0;
            bool tight[6] = {};
            for (uint32_t k = 0; k < node.item_count; ++k) {
                const uint32_t f = items[node.item_begin + k];
                if (f >= n) continue;
                sum += prims[f];
                if (!inside(world[f], node.bounds)) ++bad_box;
                for (int a = 0; a < 3; ++a) {
                    tight[a] = tight[a] || touches(world[f], node.bounds, a, true);
                    tight[3 + a] = tight[3 + a] || touches(world[f], node.bounds, a, false);
                }
            }
            if (sum != node.primitives) ++bad_prims;
            for (const bool t : tight)
                if (!t) ++bad_tight;
            continue;
        }

        // En inre nod: barnen ligger i följd och täcker nodens fragment.
        uint64_t sum = 0;
        uint32_t next = node.item_begin;
        for (uint32_t k = 0; k < node.child_count; ++k) {
            const uint32_t c = doc.spatial_child(i, k);
            SpatialNode child;
            if (c == kNone || c <= i || !doc.spatial_node(c, child) || child.parent != i ||
                child.item_begin != next) {
                ++bad_link;
                break;
            }
            next += child.item_count;
            sum += child.primitives;
            if (!inside(child.bounds, node.bounds)) ++bad_box;
        }
        if (next != node.item_begin + node.item_count) ++bad_link;
        if (sum != node.primitives) ++bad_prims;
        CHECK(doc.spatial_child(i, node.child_count) == kNone);
    }
    SpatialNode outside;
    CHECK(!doc.spatial_node(static_cast<uint32_t>(nodes), outside));
    CHECK(doc.spatial_child(static_cast<uint32_t>(nodes), 0) == kNone);
    if (bad_box || bad_prims || bad_link || bad_tight || big_leaf) {
        std::printf("  FAIL — %s: boxes %zu, primitives %zu, links %zu, loose %zu, big leaves %zu\n",
                    path, bad_box, bad_prims, bad_link, bad_tight, big_leaf);
        ++fails;
    }
    return fails;
}

int test_sample(const testdata::SampleFacts& facts) {
    SKIP_IF_MISSING(facts.path);
    int fails = 0;
    Document doc;
    CHECK(doc.open(facts.path) == Status::Ok);
    CHECK(doc.spatial_node_count() == 0 && doc.spatial_items() == nullptr && !doc.spatial_from_file());

    // En källa som inte är File eller Own.
    for (const int bad : {2, 255}) {
        CHECK(doc.load_spatial({}, static_cast<SpatialSource>(bad)) == Status::OutOfRange);
        CHECK(doc.spatial_node_count() == 0 && std::string(doc.last_error()).find("File nor Own") !=
                                                   std::string::npos);
    }

    // Filens index: lika många inre noder och löv som paketet har.
    const size_t before = doc.memory_bytes();
    CHECK(doc.load_spatial() == Status::Ok);   // läser fragmenten själv
    CHECK(doc.spatial_from_file());
    CHECK(doc.memory_bytes() > before);
    uint32_t leaves = 0, depth = 0;
    fails += check_index(doc, facts.path, 0, leaves, depth);
    CHECK(leaves == facts.spatial_leaves);
    CHECK(doc.spatial_node_count() == facts.spatial_inner + facts.spatial_leaves);
    CHECK(depth == facts.spatial_depth);
    CHECK(doc.load_spatial() == Status::Ok && doc.spatial_from_file());   // en gång till: samma

    // Det egna indexet, tvingat: samma struktur, löv om högst 32. Filens
    // index hålls kvar, så pekaren ur spatial_items() gäller fortfarande och
    // minnet räknar båda.
    const uint32_t* file_items = doc.spatial_items();
    const std::vector<uint32_t> file_copy(file_items, file_items + doc.fragment_count());
    const size_t with_file = doc.memory_bytes();
    CHECK(doc.load_spatial({}, SpatialSource::Own) == Status::Ok);
    CHECK(!doc.spatial_from_file());
    CHECK(doc.spatial_items() != file_items);
    CHECK(std::equal(file_copy.begin(), file_copy.end(), file_items));
    CHECK(doc.memory_bytes() > with_file);
    fails += check_index(doc, facts.path, 32, leaves, depth);
    const size_t n = doc.fragment_count();
    CHECK(leaves >= (n + 31) / 32);
    std::printf("  %s: own index %zu nodes, %u leaves, depth %u\n", facts.path,
                doc.spatial_node_count(), leaves, depth);

    // Tillbaka till filens: samma index som förut, inte ett nytt.
    const size_t with_both = doc.memory_bytes();
    CHECK(doc.load_spatial({}, SpatialSource::File) == Status::Ok && doc.spatial_from_file());
    CHECK(doc.spatial_items() == file_items && doc.memory_bytes() == with_both);
    return fails;
}

} // namespace

int test_spatial_index() {
    int fails = 0;
    for (const testdata::SampleFacts& f : testdata::kFacts) fails += test_sample(f);
    std::printf("test_spatial_index: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
