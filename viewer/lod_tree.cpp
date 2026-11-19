#include "lod_tree.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>
#include <queue>
#include <system_error>
#include <thread>
#include <utility>

namespace nwdview {

size_t LodTree::memory_bytes() const {
    return nodes.capacity() * sizeof(LodNode) + children.capacity() * sizeof(uint32_t) +
           tiles.capacity() * sizeof(Tile) + frags.capacity() * sizeof(uint32_t) +
           sizes.capacity() * sizeof(float) + prefix_primitives.capacity() * sizeof(uint32_t) +
           tile_of.capacity() * sizeof(uint32_t);
}

TilePrefix LodTree::prefix_at_least(uint32_t tile, float min_size) const {
    TilePrefix p;
    if (tile >= tiles.size()) return p;
    const Tile& t = tiles[tile];
    p.fragments = t.frag_count;
    p.primitives = t.primitives;
    if (!(min_size > t.min_size)) return p;
    const size_t begin = t.frag_begin, end = begin + t.frag_count;
    if (t.frag_count == 0 || end > sizes.size()) {
        p.omitted = min_size;
        return p;
    }
    const auto first = sizes.begin() + static_cast<std::ptrdiff_t>(begin);
    const auto last = sizes.begin() + static_cast<std::ptrdiff_t>(end);
    const auto cut = std::partition_point(first, last, [min_size](float s) { return s >= min_size; });
    const size_t count = static_cast<size_t>(cut - first);
    p.fragments = static_cast<uint32_t>(count);
    p.omitted = cut == last ? 0.0f : *cut;
    if (count == t.frag_count) return p;
    if (count == 0) {
        p.primitives = 0;
    } else if (prefix_primitives.size() == frags.size() && end <= prefix_primitives.size()) {
        p.primitives = std::min<uint64_t>(prefix_primitives[begin + count - 1], t.primitives);
    } else {
        p.primitives = t.primitives * count / t.frag_count;
    }
    return p;
}

namespace {

// Golvet för ett fragments storlek: punkter och degenererade lådor.
constexpr float kMinSize = 1e-6f;

// Hur ofta långa slingor frågar efter avbrott.
constexpr uint32_t kStopMask = 0x3FFF;

constexpr unsigned kMaxThreads = 32;

// Diagonalen på en fragmentlåda som float, avrundad uppåt så att storleken
// aldrig understiger den verkliga, och minst kMinSize. -1 när lådan inte är
// ändlig.
float size_of_box(const Box& b) {
    if (!b.valid()) return -1.0f;
    for (int a = 0; a < 3; ++a)
        if (!std::isfinite(b.min[a]) || !std::isfinite(b.max[a])) return -1.0f;
    const double d = b.diagonal();
    if (!std::isfinite(d)) return -1.0f;
    float f = static_cast<float>(d);
    if (static_cast<double>(f) < d) f = std::nextafter(f, std::numeric_limits<float>::infinity());
    if (!std::isfinite(f)) return -1.0f;
    return std::max(f, kMinSize);
}

// Kör body(lo, hi) för följderna [edges[t], edges[t + 1]), en tråd vardera;
// den första i den här tråden. Kan en tråd inte startas körs dess följd här.
template <class F>
void run_chunks(const std::vector<size_t>& edges, F&& body) {
    const size_t chunks = edges.size() - 1;
    std::vector<std::thread> pool;
    pool.reserve(chunks);
    for (size_t t = 1; t < chunks; ++t) {
        try {
            pool.emplace_back([&body, &edges, t] { body(edges[t], edges[t + 1]); });
        } catch (const std::system_error&) {
            body(edges[t], edges[t + 1]);
        }
    }
    body(edges[0], edges[1]);
    for (std::thread& th : pool) th.join();
}

// Gränserna för `threads` följder över `weights.size()` element, med ungefär
// lika stor summa vikt i varje. Resultatet beror bara på vikterna och antalet
// trådar.
std::vector<size_t> split_by_weight(const std::vector<uint64_t>& weights, unsigned threads) {
    const size_t n = weights.size();
    std::vector<size_t> edges{0};
    if (threads <= 1 || n < 2) {
        edges.push_back(n);
        return edges;
    }
    uint64_t total = 0;
    for (const uint64_t w : weights) total += w;
    uint64_t running = 0;
    size_t i = 0;
    for (unsigned t = 1; t < threads; ++t) {
        const uint64_t target = total / threads * t;
        while (i < n && running < target) running += weights[i++];
        if (i > edges.back()) edges.push_back(i);
    }
    if (edges.back() != n) edges.push_back(n);
    return edges;
}

struct Range {
    uint32_t begin = 0, count = 0;   // platser i ids
};

} // namespace

bool plan_lod_tree(const Model& m, const LodTreeSettings& settings, std::stop_token stop,
                   LodTree& out, LodTreeStats& stats) {
    const auto start = std::chrono::steady_clock::now();
    out = LodTree{};
    stats = LodTreeStats{};
    const auto finish = [&] {
        stats.seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    };

    const size_t n = m.fragment_count;
    if (m.nodes.empty() || n == 0 || m.items == nullptr || m.nodes[0].item_count == 0) {
        finish();
        return true;
    }
    const double alpha = std::isfinite(settings.alpha) ? std::max(0.0, settings.alpha) : 1.0 / 32;
    const uint32_t leaf_cap = std::max<uint32_t>(1, settings.max_leaf_fragments);
    const uint64_t tile_cap = std::max<uint64_t>(1, settings.max_tile_triangles);
    const unsigned threads = std::clamp(settings.threads, 1u, kMaxThreads);
    const HNwd::Document& doc = *m.doc;
    const std::vector<uint8_t>* const skip = settings.skip;
    constexpr uint32_t kNone = HNwd::kNone;

    std::vector<LodNode>& nodes = out.nodes;
    std::vector<uint32_t>& children = out.children;

    // --- Skelettet: modellens rumsliga träd utan tomma noder. ---
    std::vector<Range> range;   // per nod: dess fragment i ids
    {
        nodes.reserve(m.nodes.size());
        range.reserve(m.nodes.size());
        nodes.emplace_back();
        range.push_back({m.nodes[0].item_begin,
                         static_cast<uint32_t>(std::min<size_t>(m.nodes[0].item_count, n))});
        std::vector<std::pair<uint32_t, uint32_t>> stack{{0u, 0u}};   // (rumslig nod, nod)
        while (!stack.empty()) {
            const auto [spatial, node] = stack.back();
            stack.pop_back();
            const SpatialCell& sc = m.nodes[spatial];
            if (sc.child_count == 0) continue;
            const uint32_t first = static_cast<uint32_t>(children.size());
            uint32_t added = 0;
            for (uint32_t k = 0; k < sc.child_count; ++k) {
                const uint32_t c = m.children[sc.first_child + k];
                if (c >= m.nodes.size() || m.nodes[c].item_count == 0) continue;
                const SpatialCell& cc = m.nodes[c];
                const uint32_t index = static_cast<uint32_t>(nodes.size());
                nodes.emplace_back();
                nodes[index].parent = node;
                const size_t begin = std::min<size_t>(cc.item_begin, n);
                range.push_back({static_cast<uint32_t>(begin),
                                 static_cast<uint32_t>(std::min<size_t>(cc.item_count, n - begin))});
                children.push_back(index);
                stack.emplace_back(c, index);
                ++added;
            }
            nodes[node].first_child = first;
            nodes[node].child_count = added;
        }
    }

    const auto leaves_of = [&] {
        std::vector<uint32_t> leaves;
        for (uint32_t i = 0; i < nodes.size(); ++i)
            if (nodes[i].child_count == 0) leaves.push_back(i);
        return leaves;
    };
    const auto leaf_edges = [&](const std::vector<uint32_t>& leaves) {
        std::vector<uint64_t> weights(leaves.size());
        for (size_t i = 0; i < leaves.size(); ++i) weights[i] = range[leaves[i]].count + 1;
        return split_by_weight(weights, threads);
    };

    // --- Fragmentens storlek, primitiver och lövens lådor, i trådar. ---
    std::vector<uint32_t> ids(m.items, m.items + n);   // fragmentindex i lövordning
    std::vector<float> sz(n, -1.0f);                   // diagonalen; -1 utan låda
    std::vector<uint32_t> prims(n, 0);
    {
        const std::vector<uint32_t> leaves = leaves_of();
        run_chunks(leaf_edges(leaves), [&](size_t lo, size_t hi) {
            uint32_t counter = 0;
            for (size_t li = lo; li < hi; ++li) {
                const uint32_t node = leaves[li];
                Box bounds;
                for (uint32_t k = range[node].begin; k < range[node].begin + range[node].count;
                     ++k) {
                    if ((++counter & kStopMask) == 0 && stop.stop_requested()) return;
                    if (skip && ids[k] < skip->size() && (*skip)[ids[k]] != 0) continue;
                    HNwd::Fragment f;
                    if (!doc.fragment(ids[k], f)) continue;
                    prims[k] = f.primitives;
                    Box b;
                    if (!fragment_box(m, ids[k], b)) continue;
                    const float s = size_of_box(b);
                    if (s < 0) continue;
                    sz[k] = s;
                    bounds.add(b);
                }
                nodes[node].bounds = bounds;
            }
        });
        if (stop.stop_requested()) return false;
    }

    // --- Stora löv delas vid medianen. ---
    {
        const std::vector<uint32_t> leaves = leaves_of();
        std::vector<Box> boxes;
        std::vector<uint32_t> perm, gather_ids, gather_prims;
        std::vector<float> gather_sz;
        struct Job {
            uint32_t node, lo, hi;
        };
        std::vector<Job> work;
        for (const uint32_t leaf : leaves) {
            if (range[leaf].count <= leaf_cap) continue;
            if (stop.stop_requested()) return false;
            const uint32_t base = range[leaf].begin, count = range[leaf].count;
            boxes.assign(count, Box{});
            for (uint32_t i = 0; i < count; ++i) {
                if ((i & kStopMask) == 0 && stop.stop_requested()) return false;
                Box b;
                if (sz[base + i] >= 0 && fragment_box(m, ids[base + i], b)) boxes[i] = b;
            }
            perm.resize(count);
            std::iota(perm.begin(), perm.end(), 0u);
            work.assign(1, Job{leaf, 0, count});
            while (!work.empty()) {
                const Job j = work.back();
                work.pop_back();
                Box bb;
                for (uint32_t i = j.lo; i < j.hi; ++i) bb.add(boxes[perm[i]]);
                nodes[j.node].bounds = bb;
                range[j.node] = {base + j.lo, j.hi - j.lo};
                if (j.hi - j.lo <= leaf_cap) continue;
                int axis = 0;
                if (bb.valid()) {
                    double longest = -1;
                    for (int a = 0; a < 3; ++a)
                        if (bb.max[a] - bb.min[a] > longest) {
                            longest = bb.max[a] - bb.min[a];
                            axis = a;
                        }
                }
                const uint32_t mid = j.lo + (j.hi - j.lo) / 2;
                // Utan låda ligger fragmentet vid lådans mitt längs axeln.
                const double fallback = bb.valid() ? (bb.min[axis] + bb.max[axis]) / 2 : 0.0;
                const auto centre = [&](uint32_t i) {
                    const Box& b = boxes[i];
                    return b.valid() ? (b.min[axis] + b.max[axis]) / 2 : fallback;
                };
                std::nth_element(perm.begin() + j.lo, perm.begin() + mid, perm.begin() + j.hi,
                                 [&](uint32_t x, uint32_t y) {
                                     const double cx = centre(x), cy = centre(y);
                                     if (cx != cy) return cx < cy;
                                     return x < y;
                                 });
                const uint32_t first = static_cast<uint32_t>(children.size());
                for (int half = 0; half < 2; ++half) {
                    const uint32_t index = static_cast<uint32_t>(nodes.size());
                    nodes.emplace_back();
                    nodes[index].parent = j.node;
                    range.push_back({});
                    children.push_back(index);
                }
                nodes[j.node].first_child = first;
                nodes[j.node].child_count = 2;
                ++stats.split_nodes;
                const uint32_t left = children[first], right = children[first + 1];
                work.push_back({right, mid, j.hi});
                work.push_back({left, j.lo, mid});
            }
            // Fragmenten i den nya ordningen.
            gather_ids.resize(count);
            gather_prims.resize(count);
            gather_sz.resize(count);
            for (uint32_t i = 0; i < count; ++i) {
                gather_ids[i] = ids[base + perm[i]];
                gather_prims[i] = prims[base + perm[i]];
                gather_sz[i] = sz[base + perm[i]];
            }
            std::copy(gather_ids.begin(), gather_ids.end(), ids.begin() + base);
            std::copy(gather_prims.begin(), gather_prims.end(), prims.begin() + base);
            std::copy(gather_sz.begin(), gather_sz.end(), sz.begin() + base);
        }
    }

    // --- Nodernas lådor, nerifrån och upp. ---
    for (size_t i = nodes.size(); i-- > 1;) nodes[nodes[i].parent].bounds.add(nodes[i].bounds);
    std::vector<double> diagonal(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i) diagonal[i] = nodes[i].bounds.diagonal();

    // --- Placeringen: den översta nod där fragmentet är stort nog. ---
    std::vector<uint32_t> node_of(n, kNone);
    const std::vector<uint32_t> leaves = leaves_of();
    {
        run_chunks(leaf_edges(leaves), [&](size_t lo, size_t hi) {
            uint32_t counter = 0;
            for (size_t li = lo; li < hi; ++li) {
                const uint32_t leaf = leaves[li];
                for (uint32_t k = range[leaf].begin; k < range[leaf].begin + range[leaf].count;
                     ++k) {
                    if ((++counter & kStopMask) == 0 && stop.stop_requested()) return;
                    if (sz[k] < 0) continue;
                    const double s = sz[k];
                    uint32_t j = leaf;
                    for (;;) {
                        const uint32_t p = nodes[j].parent;
                        if (p == kNone || !(alpha * diagonal[p] <= s)) break;
                        j = p;
                    }
                    node_of[k] = j;
                }
            }
        });
        if (stop.stop_requested()) return false;
    }

    // --- Noder med för få trianglar lämnar dem till föräldern. ---
    {
        std::vector<uint64_t> own(nodes.size(), 0);
        for (size_t k = 0; k < n; ++k)
            if (node_of[k] != kNone) own[node_of[k]] += prims[k];
        std::vector<uint32_t> final_of(nodes.size());
        std::vector<uint8_t> keep(nodes.size(), 1);
        for (size_t i = nodes.size(); i-- > 1;) {
            if (own[i] >= settings.min_tile_triangles) continue;
            keep[i] = 0;
            own[nodes[i].parent] += own[i];
        }
        for (uint32_t i = 0; i < nodes.size(); ++i)
            final_of[i] = keep[i] ? i : final_of[nodes[i].parent];
        for (const uint32_t leaf : leaves)
            for (uint32_t k = range[leaf].begin; k < range[leaf].begin + range[leaf].count; ++k) {
                if (node_of[k] == kNone) continue;
                node_of[k] = final_of[node_of[k]];
                if (node_of[k] != leaf) ++stats.moved_up;
            }
    }

    // --- Nodernas fragment i följd, i rumslig ordning. ---
    std::vector<uint32_t> offset(nodes.size() + 1, 0);
    for (size_t k = 0; k < n; ++k)
        if (node_of[k] != kNone) ++offset[node_of[k] + 1];
    for (size_t i = 0; i < nodes.size(); ++i) offset[i + 1] += offset[i];
    const size_t placed = offset[nodes.size()];
    std::vector<uint32_t> grouped(placed);   // platser i ids
    {
        std::vector<uint32_t> cursor(offset.begin(), offset.end() - 1);
        for (size_t k = 0; k < n; ++k)
            if (node_of[k] != kNone) grouped[cursor[node_of[k]]++] = static_cast<uint32_t>(k);
    }
    std::vector<uint32_t>().swap(node_of);
    if (stop.stop_requested()) return false;

    // --- Bitarna: en nods fragment delas efter taket. ---
    std::vector<Tile>& tiles = out.tiles;
    for (uint32_t i = 0; i < nodes.size(); ++i) {
        nodes[i].first_tile = static_cast<uint32_t>(tiles.size());
        Tile cur;
        cur.node = i;
        for (uint32_t g = offset[i]; g < offset[i + 1]; ++g) {
            const uint64_t p = prims[grouped[g]];
            if (cur.frag_count > 0 && cur.primitives + p > tile_cap) {
                tiles.push_back(cur);
                cur = Tile{};
                cur.node = i;
            }
            if (cur.frag_count == 0) cur.frag_begin = g;
            ++cur.frag_count;
            cur.primitives += p;
        }
        if (cur.frag_count > 0) tiles.push_back(cur);
        nodes[i].tile_count = static_cast<uint32_t>(tiles.size()) - nodes[i].first_tile;
    }
    if (stop.stop_requested()) return false;

    // --- Fragmenten ordnas störst först i varje bit; bitarnas lådor; tile_of. ---
    out.frags.resize(placed);
    out.sizes.resize(placed);
    out.prefix_primitives.resize(placed);
    out.tile_of.assign(n, kNone);
    {
        struct Entry {
            float size;
            uint32_t id, primitives;
        };
        std::vector<uint64_t> weights(tiles.size());
        for (size_t t = 0; t < tiles.size(); ++t) weights[t] = tiles[t].frag_count + 1;
        run_chunks(split_by_weight(weights, threads), [&](size_t lo, size_t hi) {
            std::vector<Entry> order;
            uint32_t counter = 0;
            for (size_t t = lo; t < hi; ++t) {
                if ((++counter & 0xFF) == 0 && stop.stop_requested()) return;
                Tile& tile = tiles[t];
                order.clear();
                for (uint32_t g = tile.frag_begin; g < tile.frag_begin + tile.frag_count; ++g)
                    order.push_back({sz[grouped[g]], ids[grouped[g]], prims[grouped[g]]});
                std::sort(order.begin(), order.end(), [](const Entry& a, const Entry& b) {
                    if (a.size != b.size) return a.size > b.size;
                    return a.id < b.id;
                });
                Box bounds;
                uint64_t running = 0;
                for (uint32_t k = 0; k < tile.frag_count; ++k) {
                    const uint32_t g = tile.frag_begin + k;
                    out.frags[g] = order[k].id;
                    out.sizes[g] = order[k].size;
                    running += order[k].primitives;
                    out.prefix_primitives[g] = static_cast<uint32_t>(
                        std::min<uint64_t>(running, std::numeric_limits<uint32_t>::max()));
                    out.tile_of[order[k].id] = static_cast<uint32_t>(t);
                    Box b;
                    if (fragment_box(m, order[k].id, b)) bounds.add(b);
                }
                tile.bounds = bounds;
                tile.max_size = order.front().size;
                tile.min_size = order.back().size;
            }
        });
        if (stop.stop_requested()) return false;
    }
    std::vector<uint32_t>().swap(grouped);
    std::vector<uint32_t>().swap(ids);
    std::vector<uint32_t>().swap(prims);
    std::vector<float>().swap(sz);

    // --- max_size nerifrån och upp. ---
    for (LodNode& node : nodes)
        for (uint32_t t = node.first_tile; t < node.first_tile + node.tile_count; ++t)
            node.max_size = std::max(node.max_size, tiles[t].max_size);
    for (size_t i = nodes.size(); i-- > 1;)
        nodes[nodes[i].parent].max_size = std::max(nodes[nodes[i].parent].max_size, nodes[i].max_size);

    // --- De fasta bitarna: de största tills andelen är förbrukad. ---
    if (settings.pinned_bytes > 0 && !tiles.empty()) {
        std::vector<uint32_t> by_size(tiles.size());
        std::iota(by_size.begin(), by_size.end(), 0u);
        std::sort(by_size.begin(), by_size.end(), [&](uint32_t a, uint32_t b) {
            if (tiles[a].max_size != tiles[b].max_size) return tiles[a].max_size > tiles[b].max_size;
            return a < b;
        });
        double used = 0;
        const double limit = static_cast<double>(settings.pinned_bytes);
        for (const uint32_t t : by_size) {
            const double cost =
                static_cast<double>(tiles[t].primitives) * settings.bytes_per_primitive;
            if (used + cost > limit) break;
            used += cost;
            tiles[t].pinned = true;
            ++stats.pinned_tiles;
            stats.pinned_primitives += tiles[t].primitives;
        }
    }
    finish();
    return true;
}

std::vector<NodeBox> tree_boxes(const LodTree& t, size_t max_boxes) {
    std::vector<NodeBox> boxes;
    if (t.nodes.empty() || max_boxes == 0 || !t.nodes[0].bounds.valid()) return boxes;

    struct Open {
        double diagonal;
        uint32_t node;
        bool operator<(const Open& o) const {
            if (diagonal != o.diagonal) return diagonal < o.diagonal;
            return node > o.node;
        }
    };
    std::priority_queue<Open> open;
    std::vector<uint32_t> done;
    const auto place = [&](uint32_t node) {
        if (t.nodes[node].child_count > 0) open.push({t.nodes[node].bounds.diagonal(), node});
        else done.push_back(node);
    };
    place(0);
    size_t frontier = 1;
    while (!open.empty()) {
        const uint32_t node = open.top().node;
        open.pop();
        const LodNode& n = t.nodes[node];
        size_t valid = 0;
        for (uint32_t k = 0; k < n.child_count; ++k)
            if (t.nodes[t.children[n.first_child + k]].bounds.valid()) ++valid;
        if (valid == 0 || frontier - 1 + valid > max_boxes) {
            done.push_back(node);
            continue;
        }
        frontier = frontier - 1 + valid;
        for (uint32_t k = 0; k < n.child_count; ++k) {
            const uint32_t c = t.children[n.first_child + k];
            if (t.nodes[c].bounds.valid()) place(c);
        }
    }
    while (!open.empty()) {
        done.push_back(open.top().node);
        open.pop();
    }
    std::sort(done.begin(), done.end());
    boxes.reserve(done.size());
    for (const uint32_t node : done) boxes.push_back({t.nodes[node].bounds, node});
    return boxes;
}

} // namespace nwdview
