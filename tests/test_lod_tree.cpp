// Detaljträdet (viewer/lod_tree.h): bitarna, placeringen, delningen av stora
// löv, de fasta bitarna och trådarna, på en syntetisk fil med spridda storlekar
// och på exempelfilerna. Ren std och HNwd — ingen fönstertyp — så det prövas
// utan visaren.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <stop_token>
#include <string>
#include <vector>

#include "../tools/synth_writer.h"
#include "HNwd.h"
#include "lod_tree.h"
#include "model.h"
#include "test_data.h"
#include "test_support.h"

using namespace nwdview;

namespace {

// Tar bort filen när den går ur räckvidd. Modellen som öppnat den ska
// deklareras efter den, så att den stängs först.
struct Removed {
    std::string path;
    ~Removed() { testdata::remove_utf8(path); }
};

bool contains(const Box& outer, const Box& inner) {
    for (int a = 0; a < 3; ++a)
        if (inner.min[a] < outer.min[a] || inner.max[a] > outer.max[a]) return false;
    return true;
}

bool same_box(const Box& a, const Box& b) {
    for (int k = 0; k < 3; ++k)
        if (a.min[k] != b.min[k] || a.max[k] != b.max[k]) return false;
    return true;
}

bool finite_box(const Box& b) {
    if (!b.valid()) return false;
    for (int k = 0; k < 3; ++k)
        if (!std::isfinite(b.min[k]) || !std::isfinite(b.max[k])) return false;
    return true;
}

bool same_tree(const LodTree& a, const LodTree& b) {
    if (a.nodes.size() != b.nodes.size() || a.tiles.size() != b.tiles.size()) return false;
    if (a.children != b.children || a.frags != b.frags || a.sizes != b.sizes ||
        a.prefix_primitives != b.prefix_primitives ||
        a.tile_of != b.tile_of)
        return false;
    for (size_t i = 0; i < a.nodes.size(); ++i) {
        const LodNode &x = a.nodes[i], &y = b.nodes[i];
        if (!same_box(x.bounds, y.bounds) || x.max_size != y.max_size || x.parent != y.parent ||
            x.first_child != y.first_child || x.child_count != y.child_count ||
            x.first_tile != y.first_tile || x.tile_count != y.tile_count)
            return false;
    }
    for (size_t i = 0; i < a.tiles.size(); ++i) {
        const Tile &x = a.tiles[i], &y = b.tiles[i];
        if (!same_box(x.bounds, y.bounds) || x.max_size != y.max_size ||
            x.min_size != y.min_size || x.node != y.node || x.frag_begin != y.frag_begin ||
            x.frag_count != y.frag_count || x.primitives != y.primitives || x.pinned != y.pinned)
            return false;
    }
    return true;
}

bool same_stats(const LodTreeStats& a, const LodTreeStats& b) {
    return a.split_nodes == b.split_nodes && a.pinned_tiles == b.pinned_tiles &&
           a.moved_up == b.moved_up && a.pinned_primitives == b.pinned_primitives;
}

// Allt som trädet lovar, prövat mot fragmentens egna lådor. exact_placement
// kräver att inga fragment lämnats uppåt för att en nod hade för få trianglar
// (min_tile_triangles 0): varje fragment står då i den översta nod där det är
// stort nog, eller i sitt löv.
int check_tree(const Model& m, const LodTree& t, const LodTreeSettings& s,
               const LodTreeStats& stats, bool exact_placement,
               const std::vector<uint8_t>* skip = nullptr) {
    int fails = 0;
    const size_t n = m.fragment_count;
    constexpr uint32_t kNone = HNwd::kNone;

    std::vector<Box> boxes(n);
    std::vector<uint8_t> has_box(n, 0);
    size_t with_box = 0;
    for (size_t f = 0; f < n; ++f) {
        Box b;
        const bool skipped = skip && f < skip->size() && (*skip)[f] != 0;
        if (!skipped && fragment_box(m, f, b) && finite_box(b)) {
            boxes[f] = b;
            has_box[f] = 1;
            ++with_box;
        }
    }
    CHECK(t.tile_of.size() == n);
    CHECK(t.frags.size() == t.sizes.size());
    CHECK(t.frags.size() == with_box);
    CHECK(t.memory_bytes() >= t.nodes.size() * sizeof(LodNode));
    if (t.nodes.empty()) return fails + 1;
    CHECK(t.nodes[0].parent == kNone);
    CHECK(t.children.size() + 1 == t.nodes.size());

    // Trädets form: föräldern före barnen, varje nod barn till exakt en.
    std::vector<uint32_t> parents(t.nodes.size(), 0);
    for (uint32_t i = 0; i < t.nodes.size(); ++i) {
        const LodNode& node = t.nodes[i];
        if (i > 0) CHECK(node.parent < i);
        if (static_cast<size_t>(node.first_child) + node.child_count > t.children.size()) {
            ++fails;
            continue;
        }
        for (uint32_t k = 0; k < node.child_count; ++k) {
            const uint32_t c = t.children[node.first_child + k];
            if (c <= i || c >= t.nodes.size()) {
                ++fails;
                continue;
            }
            CHECK(t.nodes[c].parent == i);
            ++parents[c];
        }
    }
    for (size_t i = 1; i < t.nodes.size(); ++i) CHECK(parents[i] == 1);

    // Bitarna, nod för nod.
    uint32_t next_tile = 0, next_frag = 0;
    std::vector<uint32_t> times(n, 0);
    std::vector<float> own_max(t.nodes.size(), 0.0f);
    std::vector<uint64_t> own_fragments(t.nodes.size(), 0);
    for (uint32_t i = 0; i < t.nodes.size(); ++i) {
        const LodNode& node = t.nodes[i];
        CHECK(node.first_tile == next_tile);
        next_tile += node.tile_count;
        if (static_cast<size_t>(node.first_tile) + node.tile_count > t.tiles.size()) {
            ++fails;
            continue;
        }
        for (uint32_t ti = node.first_tile; ti < node.first_tile + node.tile_count; ++ti) {
            const Tile& tile = t.tiles[ti];
            CHECK(tile.node == i);
            CHECK(tile.frag_begin == next_frag);
            CHECK(tile.frag_count >= 1);
            next_frag += tile.frag_count;
            if (static_cast<size_t>(tile.frag_begin) + tile.frag_count > t.frags.size()) {
                ++fails;
                continue;
            }
            own_fragments[i] += tile.frag_count;
            own_max[i] = std::max(own_max[i], tile.max_size);
            uint64_t primitives = 0;
            Box united;
            for (uint32_t g = tile.frag_begin; g < tile.frag_begin + tile.frag_count; ++g) {
                const uint32_t f = t.frags[g];
                if (f >= n || !has_box[f]) {
                    ++fails;
                    continue;
                }
                ++times[f];
                CHECK(t.tile_of[f] == ti);
                HNwd::Fragment fragment;
                CHECK(m.doc->fragment(f, fragment));
                primitives += fragment.primitives;
                united.add(boxes[f]);
                // Storleken är diagonalen, avrundad uppåt som float.
                const double want = std::max(boxes[f].diagonal(), static_cast<double>(1e-6f));
                CHECK(static_cast<double>(t.sizes[g]) >= want);
                CHECK(static_cast<double>(t.sizes[g]) <= want * (1 + 1e-6));
                if (g > tile.frag_begin) {
                    CHECK(t.sizes[g - 1] >= t.sizes[g]);
                    if (t.sizes[g - 1] == t.sizes[g]) CHECK(t.frags[g - 1] < t.frags[g]);
                }
                // Placeringen: fragmentet står i noden där det är stort nog, och
                // inte i föräldern.
                if (exact_placement) {
                    if (node.child_count > 0)
                        CHECK(s.alpha * node.bounds.diagonal() <= static_cast<double>(t.sizes[g]));
                    if (node.parent != kNone)
                        CHECK(!(s.alpha * t.nodes[node.parent].bounds.diagonal() <=
                                static_cast<double>(t.sizes[g])));
                }
            }
            CHECK(tile.primitives == primitives);
            CHECK(tile.primitives <= s.max_tile_triangles || tile.frag_count == 1);
            CHECK(same_box(tile.bounds, united));
            CHECK(contains(node.bounds, tile.bounds));
            CHECK(tile.max_size == t.sizes[tile.frag_begin]);
            CHECK(tile.min_size == t.sizes[tile.frag_begin + tile.frag_count - 1]);
            CHECK(tile.min_size <= tile.max_size);
        }
    }
    CHECK(next_tile == t.tiles.size());
    CHECK(next_frag == t.frags.size());
    for (size_t f = 0; f < n; ++f) {
        if (has_box[f]) {
            CHECK(times[f] == 1);
        } else {
            CHECK(times[f] == 0);
            CHECK(t.tile_of[f] == kNone);
        }
    }

    // Noderna: max_size och lådan rymmer delträdet, och löven är inte för stora.
    for (uint32_t i = 0; i < t.nodes.size(); ++i) {
        const LodNode& node = t.nodes[i];
        float largest = own_max[i];
        if (static_cast<size_t>(node.first_child) + node.child_count > t.children.size()) continue;
        for (uint32_t k = 0; k < node.child_count; ++k) {
            const LodNode& child = t.nodes[t.children[node.first_child + k]];
            largest = std::max(largest, child.max_size);
            CHECK(contains(node.bounds, child.bounds));
        }
        CHECK(node.max_size == largest);
        if (node.child_count == 0) CHECK(own_fragments[i] <= s.max_leaf_fragments);
    }

    // De fasta bitarna: de största, så många som andelen ryms, och ingen
    // fler: den första som inte är fast skulle inte ha fått plats.
    uint32_t pinned = 0;
    uint64_t pinned_primitives = 0;
    double used = 0;
    for (const Tile& tile : t.tiles) {
        if (!tile.pinned) continue;
        ++pinned;
        pinned_primitives += tile.primitives;
        used += static_cast<double>(tile.primitives) * s.bytes_per_primitive;
    }
    CHECK(pinned == stats.pinned_tiles);
    CHECK(pinned_primitives == stats.pinned_primitives);
    CHECK(used <= static_cast<double>(s.pinned_bytes));
    if (s.pinned_bytes == 0) CHECK(pinned == 0);
    std::vector<uint32_t> by_size(t.tiles.size());
    std::iota(by_size.begin(), by_size.end(), 0u);
    std::sort(by_size.begin(), by_size.end(), [&t](uint32_t a, uint32_t b) {
        if (t.tiles[a].max_size != t.tiles[b].max_size)
            return t.tiles[a].max_size > t.tiles[b].max_size;
        return a < b;
    });
    bool in_prefix = true;
    for (const uint32_t ti : by_size) {
        if (t.tiles[ti].pinned) {
            if (!in_prefix) ++fails;   // en fast bit efter en som inte är det
            continue;
        }
        if (in_prefix && s.pinned_bytes > 0) {
            CHECK(used + static_cast<double>(t.tiles[ti].primitives) * s.bytes_per_primitive >
                  static_cast<double>(s.pinned_bytes));
        }
        in_prefix = false;
    }
    CHECK(stats.moved_up <= n);

    // Prefixsummorna: per post i frags, primitiverna från bitens första
    // fragment till och med det här, och prefix_at_least() mot en rak
    // genomgång av bitens storlekar.
    CHECK(t.prefix_primitives.size() == t.frags.size());
    CHECK(t.memory_bytes() >= t.prefix_primitives.size() * sizeof(uint32_t));
    if (t.prefix_primitives.size() == t.frags.size()) {
        size_t bad_sums = 0, bad_prefixes = 0, checked_cuts = 0;
        for (uint32_t ti = 0; ti < t.tiles.size(); ++ti) {
            const Tile& tile = t.tiles[ti];
            uint64_t running = 0;
            std::vector<uint64_t> sums(tile.frag_count);
            for (uint32_t k = 0; k < tile.frag_count; ++k) {
                HNwd::Fragment f;
                if (!m.doc->fragment(t.frags[tile.frag_begin + k], f)) {
                    ++fails;
                    continue;
                }
                running += f.primitives;
                sums[k] = running;
                if (t.prefix_primitives[tile.frag_begin + k] != running) ++bad_sums;
            }
            CHECK(running == tile.primitives);

            // Gränser vid bitens egna storlekar, strax över och under, och
            // långt utanför.
            std::vector<float> cuts = {0.0f, 1e9f, tile.min_size, tile.max_size};
            for (uint32_t k = 0; k < tile.frag_count; k += 1 + tile.frag_count / 8) {
                const float s = t.sizes[tile.frag_begin + k];
                cuts.push_back(s);
                cuts.push_back(std::nextafter(s, 1e9f));
                cuts.push_back(std::nextafter(s, 0.0f));
            }
            for (const float cut : cuts) {
                uint32_t count = 0;
                while (count < tile.frag_count && t.sizes[tile.frag_begin + count] >= cut) ++count;
                const TilePrefix p = t.prefix_at_least(ti, cut);
                const uint64_t want = count == 0 ? 0 : sums[count - 1];
                const float omitted = count < tile.frag_count ? t.sizes[tile.frag_begin + count] : 0.0f;
                if (p.fragments != count || p.primitives != want || p.omitted != omitted) ++bad_prefixes;
                ++checked_cuts;
            }
        }
        CHECK(bad_sums == 0);
        CHECK(bad_prefixes == 0);
        CHECK(t.tiles.empty() || checked_cuts > 0);
    }
    // En bit utanför trädet har ett tomt prefix.
    {
        const TilePrefix p = t.prefix_at_least(static_cast<uint32_t>(t.tiles.size()), 1.0f);
        CHECK(p.fragments == 0 && p.primitives == 0 && p.omitted == 0);
    }
    return fails;
}

// En översikt av noderna: högst så många, inga släkt med varandra.
int check_boxes(const LodTree& t) {
    int fails = 0;
    constexpr uint32_t kNone = HNwd::kNone;
    CHECK(tree_boxes(t, 0).empty());
    CHECK(tree_boxes(LodTree{}, 10).empty());
    size_t valid_leaves = 0;
    for (const LodNode& node : t.nodes)
        if (node.child_count == 0 && node.bounds.valid()) ++valid_leaves;
    for (const size_t limit : {size_t(1), size_t(2), size_t(5), size_t(40), size_t(1) << 40}) {
        const std::vector<NodeBox> boxes = tree_boxes(t, limit);
        CHECK(!boxes.empty());
        CHECK(boxes.size() <= limit);
        std::vector<uint8_t> in(t.nodes.size(), 0);
        for (size_t k = 0; k < boxes.size(); ++k) {
            if (boxes[k].node >= t.nodes.size()) {
                ++fails;
                continue;
            }
            if (k > 0) CHECK(boxes[k - 1].node < boxes[k].node);
            CHECK(boxes[k].bounds.valid());
            CHECK(same_box(boxes[k].bounds, t.nodes[boxes[k].node].bounds));
            in[boxes[k].node] = 1;
        }
        for (const NodeBox& b : boxes)
            for (uint32_t p = b.node >= t.nodes.size() ? kNone : t.nodes[b.node].parent;
                 p != kNone; p = t.nodes[p].parent)
                CHECK(!in[p]);
        if (limit >= t.nodes.size()) CHECK(boxes.size() == valid_leaves);
    }
    return fails;
}

struct Fixture {
    Removed file;
    LoadResult loaded;
};

// Skriver en syntetisk fil och läser den.
bool write_and_load(Fixture& fx, const std::string& name, const synth::Options& o) {
    fx.file.path = testdata::tmp_path(name);
    synth::Result written;
    std::string why;
    if (!synth::write(fx.file.path, o, written, why)) {
        std::printf("  write: %s\n", why.c_str());
        return false;
    }
    fx.loaded = load_model(fx.file.path, {});
    if (!fx.loaded.model) std::printf("  load_model: %s\n", fx.loaded.error.c_str());
    return fx.loaded.model != nullptr;
}

// Planerar med ett antal trådar.
bool plan(const Model& m, LodTreeSettings s, unsigned threads, LodTree& tree,
          LodTreeStats& stats) {
    s.threads = threads;
    return plan_lod_tree(m, s, std::stop_token{}, tree, stats);
}

int check_synthetic() {
    int fails = 0;
    synth::Options o;
    o.fragments = 3000;
    o.records = 700;
    o.nodes = 1500;
    o.seed = 11;
    o.min_size = 0.02;
    o.max_size = 40;
    Fixture fx;
    if (!write_and_load(fx, "hnwd_test_lod_tree.nwd", o)) return fails + 1;
    const Model& model = *fx.loaded.model;
    CHECK(model.fragment_count == o.fragments);

    // Placeringen utan golv: delade löv, små bitar, fasta bitar.
    {
        LodTreeSettings s;
        s.alpha = 0.25;
        s.max_leaf_fragments = 4;
        s.max_tile_triangles = 120;
        s.min_tile_triangles = 0;
        s.pinned_bytes = 100'000;
        LodTree one, four;
        LodTreeStats one_stats, four_stats;
        CHECK(plan(model, s, 1, one, one_stats));
        CHECK(plan(model, s, 4, four, four_stats));
        fails += check_tree(model, one, s, one_stats, true);
        CHECK(one_stats.split_nodes > 0);
        CHECK(one_stats.pinned_tiles > 0);
        CHECK(one_stats.seconds >= 0);
        // Samma träd oavsett antalet trådar.
        CHECK(same_tree(one, four));
        CHECK(same_stats(one_stats, four_stats));
        fails += check_boxes(one);
    }
    // Standardgolven: nodernas få trianglar lämnas uppåt.
    {
        LodTreeSettings s;
        s.max_leaf_fragments = 8;
        s.max_tile_triangles = 200;
        s.min_tile_triangles = 50;
        LodTree one, three;
        LodTreeStats one_stats, three_stats;
        CHECK(plan(model, s, 1, one, one_stats));
        CHECK(plan(model, s, 3, three, three_stats));
        fails += check_tree(model, one, s, one_stats, false);
        CHECK(same_tree(one, three));
        CHECK(same_stats(one_stats, three_stats));
        // Golvet ändrar vilka bitar noderna har, inte noderna eller fragmenten.
        LodTreeSettings none = s;
        none.min_tile_triangles = 0;
        LodTree loose;
        LodTreeStats loose_stats;
        CHECK(plan(model, none, 1, loose, loose_stats));
        CHECK(one.nodes.size() == loose.nodes.size());
        CHECK(one.frags.size() == loose.frags.size());
        CHECK(loose_stats.moved_up <= one_stats.moved_up);
    }
    // Förvalen.
    {
        LodTreeSettings s;
        LodTree tree;
        LodTreeStats stats;
        CHECK(plan(model, s, 2, tree, stats));
        fails += check_tree(model, tree, s, stats, false);
    }
    // Fragment som hoppas över står i ingen bit; alla andra i exakt en.
    {
        const size_t n = model.fragment_count;
        std::vector<uint8_t> skip(n, 0), none(n, 0), all(n, 1), shorter(n / 2, 0);
        size_t flagged = 0;
        for (size_t i = 0; i < n; ++i)
            if (i % 3 == 1 || i % 7 == 0) {
                skip[i] = 1;
                ++flagged;
                if (i < shorter.size()) shorter[i] = 1;
            }
        CHECK(flagged > 0 && flagged < n);
        LodTreeSettings s;
        s.alpha = 0.25;
        s.max_leaf_fragments = 4;
        s.max_tile_triangles = 120;
        s.min_tile_triangles = 0;
        s.pinned_bytes = 100'000;
        LodTree plain, cleared, one, four, partial, nothing;
        LodTreeStats plain_stats, cleared_stats, one_stats, four_stats, partial_stats,
            nothing_stats;
        CHECK(plan(model, s, 1, plain, plain_stats));
        // Utan flaggor, och utan vektor, är trädet detsamma som förut.
        s.skip = &none;
        CHECK(plan(model, s, 3, cleared, cleared_stats));
        CHECK(same_tree(plain, cleared));
        CHECK(same_stats(plain_stats, cleared_stats));
        s.skip = &skip;
        CHECK(plan(model, s, 1, one, one_stats));
        CHECK(plan(model, s, 4, four, four_stats));
        fails += check_tree(model, one, s, one_stats, true, &skip);
        CHECK(same_tree(one, four));
        CHECK(same_stats(one_stats, four_stats));
        size_t flagged_in_plain = 0;
        for (size_t i = 0; i < n; ++i)
            if (skip[i] && plain.tile_of[i] != HNwd::kNone) ++flagged_in_plain;
        CHECK(flagged_in_plain > 0);
        CHECK(one.frags.size() + flagged_in_plain == plain.frags.size());
        for (size_t i = 0; i < n; ++i) {
            if (skip[i]) CHECK(one.tile_of[i] == HNwd::kNone);
            else CHECK(one.tile_of[i] != HNwd::kNone);
        }
        // En vektor som är kortare än fragmenten hoppar bara över sina egna.
        s.skip = &shorter;
        CHECK(plan(model, s, 2, partial, partial_stats));
        fails += check_tree(model, partial, s, partial_stats, true, &shorter);
        for (size_t i = shorter.size(); i < n; ++i) CHECK(partial.tile_of[i] != HNwd::kNone);
        // Allt överhoppat ger noder utan bitar.
        s.skip = &all;
        CHECK(plan(model, s, 2, nothing, nothing_stats));
        CHECK(nothing.tiles.empty() && nothing.frags.empty());
        CHECK(nothing.tile_of.size() == n);
        for (size_t i = 0; i < n; ++i) CHECK(nothing.tile_of[i] == HNwd::kNone);
        fails += check_tree(model, nothing, s, nothing_stats, false, &all);
    }
    // Ett avbrott före planeringen.
    {
        std::stop_source source;
        source.request_stop();
        LodTree tree;
        LodTreeStats stats;
        CHECK(!plan_lod_tree(model, LodTreeSettings{}, source.get_token(), tree, stats));
    }
    // Ett löv delas i halvor, och ensamma fragment över taket blir egna bitar.
    {
        LodTreeSettings s;
        s.max_leaf_fragments = 1;
        s.max_tile_triangles = 1;
        s.min_tile_triangles = 0;
        LodTree tree;
        LodTreeStats stats;
        CHECK(plan(model, s, 2, tree, stats));
        fails += check_tree(model, tree, s, stats, true);
        for (const Tile& tile : tree.tiles) CHECK(tile.frag_count >= 1);
    }
    return fails;
}

// En modell utan fragment ger ett tomt träd.
int check_empty() {
    int fails = 0;
    Model empty;
    LodTree tree;
    LodTreeStats stats;
    tree.nodes.resize(3);
    CHECK(plan_lod_tree(empty, LodTreeSettings{}, std::stop_token{}, tree, stats));
    CHECK(tree.nodes.empty() && tree.tiles.empty() && tree.frags.empty() && tree.tile_of.empty());
    CHECK(stats.split_nodes == 0 && stats.pinned_tiles == 0 && stats.moved_up == 0);
    return fails;
}

// Exempelfilen med standardinställningar, med golv och utan, i en och fyra
// trådar.
int check_sample(const char* path) {
    int fails = 0;
    SKIP_IF_MISSING(path);
    const LoadResult loaded = load_model(path, {});
    CHECK(loaded.model != nullptr);
    if (!loaded.model) {
        std::printf("  load_model %s: %s\n", path, loaded.error.c_str());
        return fails;
    }
    const Model& model = *loaded.model;
    {
        LodTreeSettings s;
        s.pinned_bytes = size_t(4) << 20;
        LodTree one, four;
        LodTreeStats one_stats, four_stats;
        CHECK(plan(model, s, 1, one, one_stats));
        CHECK(plan(model, s, 4, four, four_stats));
        fails += check_tree(model, one, s, one_stats, false);
        CHECK(same_tree(one, four));
        CHECK(same_stats(one_stats, four_stats));
        fails += check_boxes(one);
    }
    {
        LodTreeSettings s;
        s.alpha = 1.0 / 16;
        s.max_leaf_fragments = 16;
        s.max_tile_triangles = 3000;
        s.min_tile_triangles = 0;
        LodTree tree;
        LodTreeStats stats;
        CHECK(plan(model, s, 2, tree, stats));
        fails += check_tree(model, tree, s, stats, true);
    }
    return fails;
}

} // namespace

int test_lod_tree() {
    int fails = 0;
    fails += check_synthetic();
    fails += check_empty();
    fails += check_sample(testdata::kSampleNwc[1]);
    fails += check_sample(testdata::kSampleNwc[5]);
    fails += check_sample(testdata::kSampleNwd);
    std::printf("test_lod_tree: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
