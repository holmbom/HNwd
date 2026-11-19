// Prövar sambandet mellan objektträdets transformer och fragmentens poser.
// Delas av verktyget nwdcheck och testerna.
//
// Sambandet som prövas, för varje fragment:
//
//   pose = T(toppnod) · … · T(löv) · flytt(c)
//
// där T(nod) är nodens första transform (klass 40, 41 eller 39) längs vägen
// från trädets rot till fragmentets löv (Fragment::seq), och c är
// geometrins mittpunkt ur klass 93. En nod utan transform bidrar inte.
// Längdenheten står i trädet: en partition (klass 32) vars enhet skiljer sig
// från den omgivande har en transform vars linjära del är kvoten mellan
// enheterna gånger enhetsmatrisen. Den kontrolleras för sig (PartitionCheck).
//
// Två frågor går inte att avgöra ur filen själv, och prövas därför båda vägarna:
// - om de nio talen i klass 39 är matrisens rader eller kolumner, och
// - om de tolv talen i poseklass 17 är matrisens kolumner (som FragmentStore
//   läser dem) eller rader.
//
// Posen jämförs i två delar: punkten i geometrins mitt (lådans mitt i postens
// egna koordinater) och den linjära delen. Båda ska stämma inom kTolerance
// gånger storleken (minst 1).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "container.h"
#include "dbcache.h"
#include "fragments.h"
#include "guid_store.h"
#include "hierarchy.h"
#include "inflate.h"
#include "partition.h"
#include "pose.h"

namespace posecheck {

namespace hd = HNwd::detail;

// Relativ tolerans för punkten och den linjära delen.
inline constexpr double kTolerance = 1e-6;

// Poseklasserna i rapporten, i den här ordningen.
inline constexpr int kClassCount = 5;
inline constexpr uint8_t kClasses[kClassCount] = {0, 14, 15, 16, 17};   // 0 är null

inline int class_slot(uint8_t pose_class) {
    for (int i = 0; i < kClassCount; ++i)
        if (kClasses[i] == pose_class) return i;
    return -1;
}

// En affin avbildning: x → l · x + t, med l radvis.
struct Affine {
    double l[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    double t[3] = {0, 0, 0};
};

// a ∘ b: först b, sedan a.
inline Affine compose(const Affine& a, const Affine& b) {
    Affine r;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            double s = 0;
            for (int k = 0; k < 3; ++k) s += a.l[3 * i + k] * b.l[3 * k + j];
            r.l[3 * i + j] = s;
        }
        r.t[i] = a.l[3 * i] * b.t[0] + a.l[3 * i + 1] * b.t[1] + a.l[3 * i + 2] * b.t[2] + a.t[i];
    }
    return r;
}

inline void transpose(double m[9]) {
    std::swap(m[1], m[3]);
    std::swap(m[2], m[6]);
    std::swap(m[5], m[7]);
}

// Hur de nio talen i klass 39 läses.
enum class Order39 : uint8_t { Rows = 0, Columns = 1 };
// Hur de tolv talen i poseklass 17 läses.
enum class Order17 : uint8_t { Columns = 0, Rows = 1 };

// En transform i trädet som avbildning. Klass 39 läses i ordningen `order`.
inline Affine transform_of(const hd::StoredTransform& t, Order39 order) {
    Affine a;
    for (int k = 0; k < 3; ++k) a.t[k] = t.t[k];
    if (t.source_class == 41) {
        hd::PoseD p;
        for (int k = 0; k < 4; ++k) p.q[k] = t.q[k];
        hd::pose_linear(p, a.l);
    } else if (t.source_class == 39) {
        for (int k = 0; k < 9; ++k) a.l[k] = t.m[k];
        if (order == Order39::Columns) transpose(a.l);
    }
    return a;
}

// Det som läses ur filen.
struct Input {
    hd::FragmentStore store;
    std::vector<hd::FragmentExtra> extra;   // mittpunkten c per fragment
    hd::Hierarchy tree;
    // Trädet expanderat i preorder; index är Fragment::seq. [0] är roten.
    std::vector<uint32_t> stored;   // lagrad nod; kNone för roten
    std::vector<uint32_t> parent;   // förälderns index; kNone för roten
    bool has_doc_unit = false;      // filen har paketet LcOpNwdPartition
    int32_t doc_unit = -1;          // dess enhetskod
};

// Paketets enda ström (formen (0, 0)) eller dess sista, uppackad.
inline hd::ErrorCode unpack_last(const uint8_t* file, size_t size, const hd::DbCacheEntry& e,
                                 std::vector<uint8_t>& out, std::string& why) {
    hd::StreamSpan span;
    hd::ErrorCode ec = hd::last_stream(e, size, span);
    if (ec != hd::ErrorCode::Ok) {
        why = std::string(e.leaf()) + ": the package lies outside the file";
        return ec;
    }
    std::string inner;
    ec = hd::inflate_span(file, size, span, hd::kDefaultMaxStreamBytes, out, &inner);
    if (ec != hd::ErrorCode::Ok) why = std::string(e.leaf()) + ": " + inner;
    return ec;
}

// Expanderar trädet i preorder med en egen stack. false och `why` om en länk
// pekar utanför eller antalet inte blir det tolken räknade.
inline bool expand(const hd::Hierarchy& h, std::vector<uint32_t>& stored,
                   std::vector<uint32_t>& parent, std::string& why) {
    struct Frame {
        const uint32_t* next;
        const uint32_t* end;
        uint32_t occurrence;
    };
    stored.assign(1, hd::kNone);
    parent.assign(1, hd::kNone);
    stored.reserve(static_cast<size_t>(h.expanded_total));
    parent.reserve(static_cast<size_t>(h.expanded_total));
    std::vector<Frame> stack;
    stack.push_back({h.top.data(), h.top.data() + h.top.size(), 0});
    while (!stack.empty()) {
        Frame& f = stack.back();
        if (f.next == f.end) {
            stack.pop_back();
            continue;
        }
        const uint32_t s = *f.next++;
        const uint32_t p = f.occurrence;
        if (s >= h.nodes.size()) {
            why = "LcOpNwdLogicalHierarchy: a child link points at node " + std::to_string(s) +
                  " of " + std::to_string(h.nodes.size());
            return false;
        }
        if (stored.size() >= h.expanded_total || stored.size() >= hd::kNone) {
            why = "LcOpNwdLogicalHierarchy: the expansion exceeds the " +
                  std::to_string(h.expanded_total) + " nodes the parser counted";
            return false;
        }
        const uint32_t occurrence = static_cast<uint32_t>(stored.size());
        stored.push_back(s);
        parent.push_back(p);
        const hd::StoredNode& n = h.nodes[s];
        const uint32_t count = h.child_count(s);
        if (count == 0) continue;
        if (n.child_begin > h.children.size() || count > h.children.size() - n.child_begin) {
            why = "LcOpNwdLogicalHierarchy: the children of node " + std::to_string(s) +
                  " lie outside the child list";
            return false;
        }
        const uint32_t* first = h.children.data() + n.child_begin;
        stack.push_back({first, first + count, occurrence});
    }
    if (stored.size() != h.expanded_total) {
        why = "LcOpNwdLogicalHierarchy: the expansion has " + std::to_string(stored.size()) +
              " nodes but the parser counted " + std::to_string(h.expanded_total);
        return false;
    }
    return true;
}

// Läser fragmenten, trädet (utan flikar), GUID-tabellens storlek och filens
// egen partition. `out` ersätts.
inline hd::ErrorCode load(const uint8_t* file, size_t size, Input& out, std::string& why) {
    out = Input{};
    hd::Container c;
    hd::ErrorCode ec = hd::read_container(file, size, c, &why);
    if (ec != hd::ErrorCode::Ok) return ec;

    // Fragmenten, strömmande, med mittpunkterna.
    const hd::DbCacheEntry* fe = hd::find_entry(c.entries, "LcOpNwdFragments");
    if (!fe) {
        why = "the file has no LcOpNwdFragments package";
        return hd::ErrorCode::PackageMissing;
    }
    {
        hd::StreamSpan span;
        ec = hd::last_stream(*fe, size, span);
        if (ec != hd::ErrorCode::Ok) {
            why = "LcOpNwdFragments: the package lies outside the file";
            return ec;
        }
        hd::InflateStream stream;
        ec = stream.open(file + span.offset, static_cast<size_t>(span.length), hd::kDefaultMaxStreamBytes);
        if (ec != hd::ErrorCode::Ok) {
            why = "LcOpNwdFragments: the zlib stream does not open";
            return ec;
        }
        ec = hd::parse_fragments(stream, out.store, {}, &out.extra, &why);
        if (ec != hd::ErrorCode::Ok) return ec;
    }

    // GUID-tabellens storlek, som trädets elementindex prövas mot.
    uint32_t guid_count = 0;
    if (const hd::DbCacheEntry* ge = hd::find_entry(c.entries, "LcOpGuidStore")) {
        std::vector<uint8_t> bytes;
        ec = unpack_last(file, size, *ge, bytes, why);
        if (ec != hd::ErrorCode::Ok) return ec;
        std::vector<hd::Guid16> guids;
        ec = hd::parse_guid_store(bytes.data(), bytes.size(), guids, &why);
        if (ec != hd::ErrorCode::Ok) return ec;
        guid_count = static_cast<uint32_t>(std::min<size_t>(guids.size(), UINT32_MAX - 1));
    }

    // Trädet utan flikar: placeringen och partitionerna är allt som behövs.
    const hd::DbCacheEntry* te = hd::find_entry(c.entries, "LcOpNwdLogicalHierarchy");
    if (!te) {
        why = "the file has no LcOpNwdLogicalHierarchy package";
        return hd::ErrorCode::PackageMissing;
    }
    hd::HierarchyStreams streams;
    ec = hd::locate_hierarchy(file, size, *te, hd::find_entry(c.entries, "LcOaPartitionProps"),
                              hd::kDefaultMaxStreamBytes, streams, &why);
    if (ec != hd::ErrorCode::Ok) return ec;
    ec = hd::parse_hierarchy(file, size, streams, guid_count, uint64_t(1) << 31,
                             hd::HierarchyContent::Structure, out.tree, nullptr, {}, &why);
    if (ec != hd::ErrorCode::Ok) return ec;
    if (!expand(out.tree, out.stored, out.parent, why)) return hd::ErrorCode::CorruptLink;

    // Filens egen enhet.
    if (const hd::DbCacheEntry* pe = hd::find_entry(c.entries, "LcOpNwdPartition")) {
        std::vector<uint8_t> bytes;
        ec = unpack_last(file, size, *pe, bytes, why);
        if (ec != hd::ErrorCode::Ok) return ec;
        hd::Partition part;
        ec = hd::parse_partition_stream(bytes.data(), bytes.size(), part, &why);
        if (ec != hd::ErrorCode::Ok) return ec;
        out.has_doc_unit = true;
        out.doc_unit = part.tail.unit_code;
    }

    // Varje fragment ska sitta på en nod i trädet.
    for (size_t i = 0; i < out.store.size(); ++i) {
        const int32_t seq = out.store.items[i].seq;
        if (seq <= 0 || static_cast<size_t>(seq) >= out.stored.size()) {
            why = "fragment " + std::to_string(i) + ": seq " + std::to_string(seq) +
                  " is not a node in the tree of " + std::to_string(out.stored.size()) + " nodes";
            return hd::ErrorCode::CorruptLink;
        }
    }
    return hd::ErrorCode::Ok;
}

// ---------------------------------------------------------------------------
// Prövningen.

struct ClassStats {
    uint64_t count = 0;
    uint64_t match = 0;          // punkten och den linjära delen stämmer
    uint64_t point_match = 0;
    uint64_t linear_match = 0;
    double max_point = 0;        // största avvikelse i punkten, i filens enhet
    double max_linear = 0;       // största avvikelse i ett tal i den linjära delen
};

// En partition (klass 32) i trädet och dess enhet.
struct PartitionCheck {
    uint32_t node = 0;            // index i preorder
    int32_t unit_code = 0;        // partitionens kod
    int32_t outer_code = -1;      // den omgivande: närmaste partition ovanför, annars filens; -1 om ingen
    bool known = false;           // båda koderna är belagda
    double ratio = 0;             // meter per enhet här delat med den omgivandes
    int32_t transform_class = 0;  // nodens transform, 0 om ingen
    double deviation = 0;         // största avvikelse mellan transformens linjära del och ratio · I
    bool consistent = false;      // känd, och transformen är ratio · I
    uint64_t fragments = 0;       // fragment under partitionen
};

struct Report {
    uint64_t fragments = 0;
    uint64_t nodes = 0;               // trädet expanderat, roten inräknad
    uint64_t transforms[3] = {};      // klass 40, 41 och 39 i trädet
    uint64_t mirroring39 = 0;         // klass 39 med flaggan för spegling satt
    uint64_t asymmetric39 = 0;        // klass 39 vars matris inte är symmetrisk
    // Någon klass 39 med en osymmetrisk matris står på ett fragments väg: då
    // spelar ordningen roll.
    bool order39_matters = false;
    // [klass][ordning i klass 39][ordning i klass 17]; för andra klasser än 17
    // är de två sista lika.
    ClassStats stats[kClassCount][2][2];
    uint64_t unmatched = 0;           // fragment som inte stämmer med någon av tolkningarna
    uint64_t first_unmatched = 0;     // det första av dem
    std::vector<PartitionCheck> partitions;
    uint64_t fragments_scaled = 0;    // fragment under en partition vars kvot inte är 1
};

inline bool unit_ratio(int32_t inner, int32_t outer, double& ratio) {
    double a = 0, b = 0;
    if (!hd::metres_per_unit(inner, a) || !hd::metres_per_unit(outer, b)) return false;
    ratio = a / b;
    return true;
}

inline void check(const Input& in, Report& r) {
    r = Report{};
    const hd::Hierarchy& h = in.tree;
    r.fragments = in.store.size();
    r.nodes = in.stored.size();
    for (uint32_t k = 0; k < h.transform_count(); ++k) {
        hd::StoredTransform t;
        if (!h.transform(k, t)) continue;
        if (t.source_class == 40) ++r.transforms[0];
        if (t.source_class == 41) ++r.transforms[1];
        if (t.source_class == 39) {
            ++r.transforms[2];
            if (t.reverses != 0) ++r.mirroring39;
            const double* m = t.m;
            const double scale = std::max({1.0, std::fabs(m[0]), std::fabs(m[4]), std::fabs(m[8])});
            if (std::fabs(m[1] - m[3]) > kTolerance * scale || std::fabs(m[2] - m[6]) > kTolerance * scale ||
                std::fabs(m[5] - m[7]) > kTolerance * scale)
                ++r.asymmetric39;
        }
    }

    // Partitionerna, i preorder: den omgivande enheten är närmaste partition
    // ovanför, och den står före i listan.
    std::vector<uint32_t> partition_of(in.stored.size(), hd::kNone);   // närmaste partition, som index i r.partitions
    for (uint32_t o = 1; o < in.stored.size(); ++o) {
        const uint32_t up = partition_of[in.parent[o]];
        partition_of[o] = up;
        const hd::StoredNode& n = h.nodes[in.stored[o]];
        const uint32_t partition = h.partition(in.stored[o]);
        if (n.cls != 32 || partition >= h.partitions.size()) continue;
        PartitionCheck pc;
        pc.node = o;
        pc.unit_code = h.partitions[partition].unit_code;
        pc.outer_code = up != hd::kNone ? r.partitions[up].unit_code
                                        : (in.has_doc_unit ? in.doc_unit : -1);
        pc.known = pc.outer_code >= 0 && unit_ratio(pc.unit_code, pc.outer_code, pc.ratio);
        Affine a;
        hd::StoredTransform nt;
        const uint32_t nti = h.node_transform(in.stored[o]);
        if (nti != hd::kNone && h.transform(nti, nt)) {
            pc.transform_class = nt.source_class;
            a = transform_of(nt, Order39::Rows);
        }
        if (pc.known) {
            for (int k = 0; k < 9; ++k)
                pc.deviation = std::max(pc.deviation, std::fabs(a.l[k] - (k % 4 == 0 ? pc.ratio : 0.0)));
            pc.consistent = pc.deviation <= kTolerance * std::max(1.0, pc.ratio);
        }
        partition_of[o] = static_cast<uint32_t>(r.partitions.size());
        r.partitions.push_back(pc);
    }

    // Fragment för fragment. Fragmenten i ett löv står i följd, så det
    // sammansatta för senaste löv sparas.
    int32_t cached_seq = -1;
    Affine composed[2];
    bool has39_asym = false;
    std::vector<uint32_t> path;
    for (size_t i = 0; i < in.store.size(); ++i) {
        const hd::FragmentStore::Item& it = in.store.items[i];
        if (it.seq != cached_seq) {
            cached_seq = it.seq;
            path.clear();
            for (uint32_t o = static_cast<uint32_t>(it.seq); o != 0; o = in.parent[o]) path.push_back(o);
            has39_asym = false;
            for (int v = 0; v < 2; ++v) {
                composed[v] = Affine{};
                for (size_t k = path.size(); k-- > 0;) {
                    const uint32_t ti = h.node_transform(in.stored[path[k]]);
                    hd::StoredTransform t;
                    if (ti == hd::kNone || !h.transform(ti, t)) continue;
                    if (t.source_class == 39) {
                        const Affine rows = transform_of(t, Order39::Rows);
                        const Affine cols = transform_of(t, Order39::Columns);
                        for (int e = 0; e < 9; ++e)
                            if (std::fabs(rows.l[e] - cols.l[e]) >
                                kTolerance * std::max(1.0, std::fabs(rows.l[e])))
                                has39_asym = true;
                    }
                    composed[v] = compose(composed[v], transform_of(t, static_cast<Order39>(v)));
                }
            }
        }
        if (has39_asym) r.order39_matters = true;
        {
            // Partitionerna på vägen, och om någon av dem räknar om enheten.
            bool scaled = false;
            uint32_t p = partition_of[static_cast<uint32_t>(it.seq)];
            while (p != hd::kNone) {
                PartitionCheck& pc = r.partitions[p];
                ++pc.fragments;
                if (!pc.known || std::fabs(pc.ratio - 1) > kTolerance) scaled = true;
                p = partition_of[in.parent[pc.node]];
            }
            if (scaled) ++r.fragments_scaled;
        }

        const hd::GeometryRefData& g = in.store.geomrefs[it.geomref];
        const float mid[3] = {0.5f * (g.bbox[0] + g.bbox[3]), 0.5f * (g.bbox[1] + g.bbox[4]),
                              0.5f * (g.bbox[2] + g.bbox[5])};
        const double* c = in.extra[i].center;

        hd::PoseD pose;
        in.store.pose(i, pose);
        const int slot = class_slot(it.pose_class);
        if (slot < 0) continue;   // FragmentStore har bara de här klasserna
        bool any = false;
        for (int o17 = 0; o17 < 2; ++o17) {
            if (o17 == 1 && it.pose_class != 17) break;
            hd::PoseD p = pose;
            if (o17 == 1) transpose(p.m);
            double pl[9], pp[3];
            hd::pose_linear(p, pl);
            hd::pose_point(p, mid, pp);
            double lin_scale = 1, point_scale = 1;
            for (int k = 0; k < 9; ++k) lin_scale = std::max(lin_scale, std::fabs(pl[k]));
            for (int k = 0; k < 3; ++k) point_scale = std::max(point_scale, std::fabs(pp[k]));

            for (int v = 0; v < 2; ++v) {
                Affine shift;
                for (int k = 0; k < 3; ++k) shift.t[k] = c[k];
                const Affine a = compose(composed[v], shift);
                double dp = 0, dl = 0;
                for (int k = 0; k < 3; ++k) {
                    const double x = a.l[3 * k] * mid[0] + a.l[3 * k + 1] * mid[1] + a.l[3 * k + 2] * mid[2] + a.t[k];
                    dp = std::max(dp, std::fabs(x - pp[k]));
                }
                for (int k = 0; k < 9; ++k) dl = std::max(dl, std::fabs(a.l[k] - pl[k]));
                const bool point_ok = dp <= kTolerance * point_scale;
                const bool linear_ok = dl <= kTolerance * lin_scale;
                ClassStats& s = r.stats[slot][v][o17];
                ++s.count;
                if (point_ok) ++s.point_match;
                if (linear_ok) ++s.linear_match;
                if (point_ok && linear_ok) {
                    ++s.match;
                    any = true;
                }
                s.max_point = std::max(s.max_point, dp);
                s.max_linear = std::max(s.max_linear, dl);
            }
        }
        if (!any) {
            if (r.unmatched == 0) r.first_unmatched = i;
            ++r.unmatched;
        }
    }
}

// Utfallet för en fråga om ordningen: vilken tolkning som stämmer för alla.
enum class Verdict : uint8_t { NoData, Both, First, Second, Neither };

// Klass 17: kolumner (First) eller rader (Second), med klass 39 i ordningen
// `order39`.
inline Verdict verdict17(const Report& r, Order39 order39) {
    const int v = static_cast<int>(order39);
    const ClassStats& cols = r.stats[4][v][0];
    const ClassStats& rows = r.stats[4][v][1];
    if (cols.count == 0) return Verdict::NoData;
    const bool a = cols.match == cols.count, b = rows.match == rows.count;
    return a && b ? Verdict::Both : a ? Verdict::First : b ? Verdict::Second : Verdict::Neither;
}

// Klass 39: rader (First) eller kolumner (Second), över alla fragment och
// för klass 17 med dess bästa ordning.
inline Verdict verdict39(const Report& r) {
    if (!r.order39_matters) return Verdict::NoData;
    bool ok[2] = {true, true};
    for (int v = 0; v < 2; ++v) {
        for (int slot = 0; slot < kClassCount; ++slot) {
            const ClassStats& a = r.stats[slot][v][0];
            const ClassStats& b = r.stats[slot][v][1];
            const bool all = a.match == a.count || (kClasses[slot] == 17 && b.match == b.count);
            ok[v] = ok[v] && all;
        }
    }
    return ok[0] && ok[1] ? Verdict::Both : ok[0] ? Verdict::First : ok[1] ? Verdict::Second : Verdict::Neither;
}

} // namespace posecheck
