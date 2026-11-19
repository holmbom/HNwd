// Paketet LcOpNwdSpatialHierarchy: trädet över fragmenten.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <iterator>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "container.h"
#include "fragment_view.h"
#include "fragments.h"
#include "mapped_file.h"
#include "object_reader.h"
#include "spatial.h"
#include "test_data.h"
#include "test_support.h"

using namespace HNwd::detail;
using testdata::Bytes;
using testdata::ByteWriter;

namespace {

constexpr const char* kName = "LcOpNwdSpatialHierarchy at byte ";

// ---------------------------------------------------------------------------
// Syntetiska strömmar.

void inner(ByteWriter& w, int32_t k) {
    w.i32(5);
    w.i32(k);
}

void leaf(ByteWriter& w, std::initializer_list<int32_t> items) {
    w.i32(2);
    w.i32(static_cast<int32_t>(items.size()));
    for (const int32_t f : items) {
        w.i32(1);
        w.i32(f);
    }
}

void single(ByteWriter& w, int32_t f) {
    w.i32(4);
    w.i32(f);
}

ErrorCode parse(const Bytes& b, uint32_t count, SpatialTree& t, std::string* detail = nullptr) {
    return parse_spatial_hierarchy(b.data(), b.size(), count, t, detail);
}

// Ger strömmen exakt `code`, ett tomt resultat och ett meddelande som inleds
// med paketets namn och, om de ges, innehåller byten och regeln?
bool fails_with(const Bytes& b, uint32_t count, ErrorCode code, const char* rule = nullptr,
                long at = -1) {
    SpatialTree t;
    t.nodes.resize(3);   // ska ersättas
    std::string detail;
    const ErrorCode e = parse(b, count, t, &detail);
    bool ok = e == code && detail.rfind(kName, 0) == 0 && t.nodes.empty() &&
              t.children.empty() && t.items.empty();
    if (rule) ok = ok && detail.find(rule) != std::string::npos;
    if (at >= 0) ok = ok && detail.rfind(kName + std::to_string(at) + ":", 0) == 0;
    if (!ok) std::printf("  got %d \"%s\"\n", static_cast<int>(e), detail.c_str());
    return ok;
}

// Trädets egna samband: barnen pekar på sin förälder, löven har inga barn, en
// inre nod har inga egna poster och dess delträd är barnens i följd.
bool consistent(const SpatialTree& t) {
    if (t.nodes.empty() || t.nodes[0].parent != UINT32_MAX) return false;
    for (uint32_t i = 0; i < t.nodes.size(); ++i) {
        const SpatialTree::Node& n = t.nodes[i];
        if (n.item_begin > t.items.size() || n.item_count > t.items.size() - n.item_begin)
            return false;
        if (n.child_count == 0) {
            if (n.item_count == 0) return false;
            continue;
        }
        if (n.child_begin > t.children.size() ||
            n.child_count > t.children.size() - n.child_begin)
            return false;
        uint32_t next = n.item_begin;
        for (uint32_t c = 0; c < n.child_count; ++c) {
            const uint32_t child = t.children[n.child_begin + c];
            if (child <= i || child >= t.nodes.size() || t.nodes[child].parent != i) return false;
            if (t.nodes[child].item_begin != next) return false;
            next += t.nodes[child].item_count;
        }
        if (next != n.item_begin + n.item_count) return false;
    }
    return true;
}

int test_synthetic() {
    int fails = 0;

    // Alla tre nodformerna:
    //   0 inre(2)
    //     1 löv (0, 2)
    //     2 inre(2)
    //       3 ensamt löv (1)
    //       4 löv (3)
    {
        ByteWriter w;
        inner(w, 2);
        leaf(w, {0, 2});
        inner(w, 2);
        single(w, 1);
        leaf(w, {3});
        SpatialTree t;
        std::string detail = "x";
        CHECK(parse(w.data, 4, t, &detail) == ErrorCode::Ok);
        CHECK(detail.empty());
        CHECK(t.nodes.size() == 5 && t.children.size() == 4 && t.items.size() == 4);
        CHECK(consistent(t));
        if (t.nodes.size() == 5 && t.children.size() == 4 && t.items.size() == 4) {
            CHECK(t.items[0] == 0 && t.items[1] == 2 && t.items[2] == 1 && t.items[3] == 3);
            CHECK(t.nodes[0].child_count == 2 && t.nodes[0].item_count == 4);
            CHECK(t.children[t.nodes[0].child_begin] == 1);
            CHECK(t.children[t.nodes[0].child_begin + 1] == 2);
            CHECK(t.nodes[1].parent == 0 && t.nodes[1].child_count == 0);
            CHECK(t.nodes[1].item_begin == 0 && t.nodes[1].item_count == 2);
            CHECK(t.nodes[2].parent == 0 && t.nodes[2].child_count == 2);
            CHECK(t.nodes[2].item_begin == 2 && t.nodes[2].item_count == 2);
            CHECK(t.children[t.nodes[2].child_begin] == 3);
            CHECK(t.children[t.nodes[2].child_begin + 1] == 4);
            CHECK(t.nodes[3].parent == 2 && t.nodes[3].item_begin == 2 &&
                  t.nodes[3].item_count == 1);
            CHECK(t.nodes[4].parent == 2 && t.nodes[4].item_begin == 3 &&
                  t.nodes[4].item_count == 1);
        }
        // Utan detail-pekare går det lika bra.
        CHECK(parse(w.data, 4, t) == ErrorCode::Ok && t.nodes.size() == 5);
    }

    // Roten får vara ett löv, av båda formerna.
    {
        ByteWriter a;
        leaf(a, {1, 0});
        SpatialTree t;
        CHECK(parse(a.data, 2, t) == ErrorCode::Ok);
        CHECK(t.nodes.size() == 1 && t.children.empty() && t.items.size() == 2 && consistent(t));
        ByteWriter b;
        single(b, 0);
        CHECK(parse(b.data, 1, t) == ErrorCode::Ok);
        CHECK(t.nodes.size() == 1 && t.items.size() == 1 && t.items[0] == 0 && consistent(t));
    }

    // Okända taggar: en nodtagg och en posttagg. Grammatiken är hel, så en
    // annan tagg är ett trasigt paket.
    {
        for (const int32_t tag : {0, 1, 3, 6, -1}) {
            ByteWriter w;
            inner(w, 2);
            single(w, 0);
            w.i32(tag);
            w.i32(1);
            CHECK(fails_with(w.data, 2, ErrorCode::CorruptPackage, "node tag", 16));
        }
        for (const int32_t tag : {0, 2, 4, 5}) {
            ByteWriter w;
            w.i32(2);
            w.i32(2);
            w.i32(1);
            w.i32(0);
            w.i32(tag);
            w.i32(1);
            CHECK(fails_with(w.data, 2, ErrorCode::CorruptPackage, "leaf entry tag", 16));
        }
    }

    // Barnplatserna räknas mot strömmen sammanlagt: en inre nod som lovar
    // så många barn som resten rymmer lämnar ingen plats åt ett barn som
    // själv lovar fler. Annars kunde varje nivå allokera en gång strömmens
    // storlek.
    {
        const size_t size = size_t(1) << 20;
        Bytes data(size, 0);
        size_t pos = 0;
        for (int d = 0; d < 256; ++d) {
            const int32_t tag = 5;
            const int32_t k = static_cast<int32_t>((size - pos - 8) / 8);
            std::memcpy(&data[pos], &tag, 4);
            std::memcpy(&data[pos + 4], &k, 4);
            pos += 8;
        }
        CHECK(fails_with(data, 1, ErrorCode::CorruptPackage, "does not fit in the rest of the stream", 12));
        // Ett löv vars poster tar platsen för syskon som ännu inte är lästa.
        ByteWriter w;
        inner(w, 2);
        w.i32(2);
        w.i32(2);
        w.i32(1);
        w.i32(0);
        w.i32(1);
        w.i32(1);
        CHECK(fails_with(w.data, 2, ErrorCode::CorruptPackage, "does not fit in the rest of the stream", 12));
    }

    // Antal som inte är positiva, eller som inte ryms.
    {
        for (const int32_t k : {0, -1, INT32_MIN}) {
            ByteWriter w;
            inner(w, k);
            single(w, 0);
            CHECK(fails_with(w.data, 1, ErrorCode::CorruptPackage, "is not positive", 4));
            ByteWriter v;
            v.i32(2);
            v.i32(k);
            v.i32(1);
            v.i32(0);
            CHECK(fails_with(v.data, 1, ErrorCode::CorruptPackage, "is not positive", 4));
        }
        for (const int32_t k : {3, 0x40000000, INT32_MAX}) {
            ByteWriter w;
            inner(w, k);
            single(w, 0);
            single(w, 1);
            CHECK(fails_with(w.data, 2, ErrorCode::CorruptPackage, "does not fit", 4));
            ByteWriter v;
            v.i32(2);
            v.i32(k);
            for (int i = 0; i < 2; ++i) {
                v.i32(1);
                v.i32(i);
            }
            CHECK(fails_with(v.data, 2, ErrorCode::CorruptPackage, "does not fit", 4));
        }
    }

    // Byte efter roten, och en ström som är avhuggen på varje längd.
    {
        ByteWriter w;
        inner(w, 2);
        leaf(w, {0, 2});
        inner(w, 2);
        single(w, 1);
        leaf(w, {3});
        for (const size_t extra : {size_t(1), size_t(4), size_t(8)}) {
            Bytes b = w.data;
            b.insert(b.end(), extra, 0);
            CHECK(fails_with(b, 4, ErrorCode::CorruptPackage, "after the root node",
                             static_cast<long>(w.data.size())));
        }
        Bytes b = w.data;
        single(w, 4);   // en hel nod till efter roten
        CHECK(fails_with(w.data, 5, ErrorCode::CorruptPackage, "after the root node"));
        for (size_t len = 0; len < b.size(); ++len) {
            const Bytes cut(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(len));
            CHECK(fails_with(cut, 4, ErrorCode::CorruptPackage));
        }
    }

    // Fragmenten: utanför, dubbletter och saknade.
    {
        ByteWriter w;
        inner(w, 2);
        leaf(w, {0, 1});
        single(w, 4);
        CHECK(fails_with(w.data, 4, ErrorCode::CorruptLink, "outside", 36));
        ByteWriter n;
        leaf(n, {0, -1});
        CHECK(fails_with(n.data, 4, ErrorCode::CorruptLink, "outside", 20));
        ByteWriter d;
        inner(d, 2);
        leaf(d, {0, 1});
        single(d, 1);
        CHECK(fails_with(d.data, 2, ErrorCode::CorruptLink, "more than one place", 36));
        ByteWriter e;
        leaf(e, {2, 2});
        CHECK(fails_with(e.data, 3, ErrorCode::CorruptLink, "more than one place", 20));
        ByteWriter m;
        inner(m, 2);
        leaf(m, {0, 2});
        single(m, 3);
        CHECK(fails_with(m.data, 5, ErrorCode::CorruptLink, "fragment 1 of 5 is in no leaf",
                         static_cast<long>(m.data.size())));
        // Inga fragment alls: varje index är utanför.
        ByteWriter z;
        single(z, 0);
        CHECK(fails_with(z.data, 0, ErrorCode::CorruptLink, "outside"));
    }

    // Djupet: en kedja av inre noder med ett löv sist på djupet `depth`.
    {
        auto chain = [](int depth) {
            ByteWriter w;
            for (int i = 0; i < depth; ++i) inner(w, 1);
            single(w, 0);
            return w.data;
        };
        SpatialTree t;
        CHECK(parse(chain(kMaxNesting), 1, t) == ErrorCode::Ok);
        CHECK(t.nodes.size() == static_cast<size_t>(kMaxNesting) + 1 && consistent(t));
        CHECK(fails_with(chain(kMaxNesting + 1), 1, ErrorCode::LimitExceeded, "depth",
                         static_cast<long>(8 * (kMaxNesting + 1))));
    }

    // Ett brett träd med stora löv: 200 000 fragment i blandad ordning, i 40
    // löv om 5 000 under fem inre noder om åtta.
    {
        constexpr uint32_t kCount = 200000;
        std::vector<int32_t> order(kCount);
        std::iota(order.begin(), order.end(), 0);
        std::mt19937 rng(7);
        std::shuffle(order.begin(), order.end(), rng);
        ByteWriter w;
        inner(w, 5);
        size_t next = 0;
        for (int i = 0; i < 5; ++i) {
            inner(w, 8);
            for (int j = 0; j < 8; ++j) {
                w.i32(2);
                w.i32(5000);
                for (int e = 0; e < 5000; ++e) {
                    w.i32(1);
                    w.i32(order[next++]);
                }
            }
        }
        SpatialTree t;
        CHECK(parse(w.data, kCount, t) == ErrorCode::Ok);
        CHECK(t.nodes.size() == 46 && t.items.size() == kCount && consistent(t));
        bool same = t.items.size() == kCount;
        for (size_t i = 0; same && i < kCount; ++i) same = t.items[i] == static_cast<uint32_t>(order[i]);
        CHECK(same);
    }
    return fails;
}

// ---------------------------------------------------------------------------
// Exempelfilerna.

bool package_stream(const MappedFile& file, const Container& c, const char* name, Bytes& out) {
    const DbCacheEntry* e = find_entry(c.entries, name);
    StreamSpan span;
    return e && last_stream(*e, file.size(), span) == ErrorCode::Ok &&
           inflate_span(file.data(), file.size(), span, kDefaultMaxStreamBytes, out) ==
               ErrorCode::Ok;
}

// Summan över grupperna av antalet fragment gånger diagonalen i lådan kring
// deras flytt t. Grupperna är följder i `items` med längderna i `sizes`.
double weighted_diagonal(const std::vector<FragmentView>& frags, const std::vector<uint32_t>& items,
                         const std::vector<uint32_t>& sizes) {
    double sum = 0;
    size_t at = 0;
    for (const uint32_t n : sizes) {
        double lo[3], hi[3];
        for (int a = 0; a < 3; ++a) {
            lo[a] = INFINITY;
            hi[a] = -INFINITY;
        }
        for (uint32_t i = 0; i < n; ++i) {
            const FragmentView& f = frags[items[at + i]];
            for (int a = 0; a < 3; ++a) {
                lo[a] = std::min(lo[a], f.t[a]);
                hi[a] = std::max(hi[a], f.t[a]);
            }
        }
        at += n;
        const double dx = hi[0] - lo[0], dy = hi[1] - lo[1], dz = hi[2] - lo[2];
        sum += n * std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    return sum;
}

int test_sample(const testdata::SampleFacts& facts, uint32_t expected_fragments) {
    int fails = 0;
    SKIP_IF_MISSING(facts.path);

    MappedFile file;
    Container c;
    Bytes spatial, frag_stream;
    FragmentStore frags;
    std::string detail;
    if (file.open(facts.path) != ErrorCode::Ok ||
        read_container(file.data(), file.size(), c, &detail) != ErrorCode::Ok ||
        !package_stream(file, c, "LcOpNwdSpatialHierarchy", spatial) ||
        !package_stream(file, c, "LcOpNwdFragments", frag_stream) ||
        parse_fragments(frag_stream.data(), frag_stream.size(), frags, nullptr, &detail) !=
            ErrorCode::Ok) {
        std::printf("  FAIL — %s: paketen läses inte %s\n", facts.path, detail.c_str());
        return fails + 1;
    }
    const uint32_t count = static_cast<uint32_t>(frags.items.size());
    CHECK(count == expected_fragments);

    SpatialTree t;
    const ErrorCode e = parse_spatial_hierarchy(spatial.data(), spatial.size(), count, t, &detail);
    if (e != ErrorCode::Ok) {
        std::printf("  FAIL — %s: %s %s\n", facts.path, error_message(e), detail.c_str());
        return fails + 1;
    }
    CHECK(consistent(t));

    // Inre noder, löv, djup och största löv. Föräldern kommer före barnet i
    // preorder, så djupet räknas i en följd.
    uint32_t inner_nodes = 0, leaves = 0, max_depth = 0, max_leaf = 0, tag4 = 0;
    std::vector<uint32_t> depth(t.nodes.size(), 0), sizes;
    for (uint32_t i = 0; i < t.nodes.size(); ++i) {
        const SpatialTree::Node& n = t.nodes[i];
        if (i > 0) depth[i] = depth[n.parent] + 1;
        max_depth = std::max(max_depth, depth[i]);
        if (n.child_count > 0) {
            ++inner_nodes;
        } else {
            ++leaves;
            sizes.push_back(n.item_count);
            max_leaf = std::max(max_leaf, n.item_count);
            if (n.item_count == 1) ++tag4;
        }
    }
    CHECK(inner_nodes == facts.spatial_inner);
    CHECK(leaves == facts.spatial_leaves);
    CHECK(max_depth == facts.spatial_depth);
    CHECK(max_leaf == facts.spatial_max_leaf);

    // Posterna är en permutation av 0 .. N-1, prövat här oberoende av tolken.
    std::vector<uint8_t> seen(count, 0);
    bool permutation = t.items.size() == count;
    for (const uint32_t f : t.items) {
        if (f >= count || seen[f]) permutation = false;
        else seen[f] = 1;
    }
    CHECK(permutation);

    // Löven är rumsligt samlade: deras diagonaler är mindre än för lika stora
    // grupper i slumpad ordning. Med minst 20 löv är kvoten under 0,25 (i
    // exempelfilerna 0,03–0,11); med några få stora löv täcker varje löv en
    // stor del av modellen (0,41 med 10 löv, 0,55 med 3), och kvoten prövas
    // bara mot 1.
    std::vector<uint32_t> shuffled = t.items;
    std::mt19937 rng(1);
    std::shuffle(shuffled.begin(), shuffled.end(), rng);
    const std::vector<FragmentView> views = fragment_views(frags);
    const double leaf_sum = weighted_diagonal(views, t.items, sizes);
    const double random_sum = weighted_diagonal(views, shuffled, sizes);
    const double ratio = random_sum > 0 ? leaf_sum / random_sum : 1.0;
    CHECK(ratio < (leaves >= 20 ? 0.25 : 1.0));

    std::printf("  %s: %u inre, %u löv (%u med ett fragment), djup %u, största löv %u, "
                "diagonal mot slump %.3f\n",
                facts.path, inner_nodes, leaves, tag4, max_depth, max_leaf, ratio);
    return fails;
}

} // namespace

int test_spatial() {
    int fails = 0;
    fails += test_synthetic();
    static_assert(std::size(testdata::kFacts) == std::size(testdata::kSamples));
    for (size_t i = 0; i < std::size(testdata::kFacts); ++i)
        fails += test_sample(testdata::kFacts[i], testdata::kSamples[i].fragments);
    std::printf("test_spatial: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
