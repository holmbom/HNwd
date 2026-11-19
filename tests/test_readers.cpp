// GeometryReader: fyra trådar med var sin läsare över samma dokument ger samma
// geometri som en tråd, också när dokumentet stängs medan de läser. Och
// load_tree() medan fyra trådar gör const-uppslag och läser geometri på samma
// dokument: trådarna får samma svar som utan samtidighet, och trädets anrop
// svarar "inte inläst" eller ur ett färdigt träd.

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "HNwd.h"
#include "test_data.h"
#include "test_support.h"

using HNwd::Document;
using HNwd::GeometryReader;
using HNwd::Status;

namespace {

// FNV-1a över en meshs byte och dess status.
uint64_t mesh_hash(Status st, const HNwd::Mesh& m) {
    uint64_t h = 1469598103934665603ull;
    auto add = [&](const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    };
    const uint8_t s = static_cast<uint8_t>(st);
    add(&s, 1);
    if (st != Status::Ok) return h;
    const uint8_t kind = static_cast<uint8_t>(m.kind);
    add(&kind, 1);
    add(m.positions.data(), m.positions.size() * sizeof(float));
    add(m.normals.data(), m.normals.size() * sizeof(float));
    add(m.uvs.data(), m.uvs.size() * sizeof(float));
    add(m.colors.data(), m.colors.size() * sizeof(uint32_t));
    add(m.indices.data(), m.indices.size() * sizeof(uint32_t));
    return h;
}

// Vad en tråd fick: summan av hashvärdena och antalet fragment per status.
struct Result {
    uint64_t sum = 0;
    size_t ok = 0, other = 0;
};

// Läser fragmenten i, i + step, i + 2·step …, framlänges eller baklänges.
void read_part(GeometryReader& reader, size_t count, size_t first, size_t step, bool backwards,
               Result& out) {
    HNwd::Mesh mesh;
    std::vector<size_t> order;
    for (size_t i = first; i < count; i += step) order.push_back(i);
    if (backwards) std::reverse(order.begin(), order.end());
    for (const size_t i : order) {
        const Status st = reader.fragment_mesh(i, mesh);
        out.sum += mesh_hash(st, mesh) * (i + 1);
        if (st == Status::Ok)
            ++out.ok;
        else
            ++out.other;
    }
}

// ---------------------------------------------------------------------------
// load_tree() samtidigt med const-uppslag och läsare.

// FNV-1a som går att mata bit för bit.
struct Hash {
    uint64_t h = 1469598103934665603ull;
    void add(const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    }
    template <class T>
    void value(const T& v) {
        add(&v, sizeof v);
    }
    void text(std::string_view s) {
        value(s.size());
        add(s.data(), s.size());
    }
};

constexpr size_t kTreeThreads = 4;
// Geometrin läses för vart 64:e av trådens fragment, så att ett varv är kort
// och många varv hinner gå medan trädet läses.
constexpr size_t kMeshEvery = 64;
// Noderna som jämförs: var 97:e.
constexpr uint32_t kNodeStep = 97;

// Const-uppslagen för trådens fragment (i ≡ t mod kTreeThreads): fragmentet,
// dess utseende och material och världslådan, och sedan det rumsliga
// indexet och geometrins layout.
uint64_t lookup_pass(const Document& doc, size_t t) {
    Hash h;
    const size_t count = doc.fragment_count();
    h.value(count);
    for (size_t i = t; i < count; i += kTreeThreads) {
        HNwd::Fragment f;
        const bool ok = doc.fragment(i, f);
        h.value(ok);
        if (!ok) continue;
        h.value(f.record);
        h.value(f.seq);
        h.value(f.primitives);
        h.value(f.look);
        h.value(f.pose.t);
        h.value(f.pose.q);
        h.value(f.pose.s);
        h.value(f.pose.m);
        h.value(f.bounds);
        HNwd::Look l;
        if (doc.look(f.look, l)) {
            h.value(l.diffuse);
            h.value(l.transparency);
            h.value(l.material);
            HNwd::Material m;
            if (l.material != HNwd::kNone && doc.material(l.material, m)) {
                h.value(m.has_color);
                h.value(m.base_color);
                h.value(m.texture_size);
                h.text(m.texture_name);
                h.text(m.json);
            }
        }
        HNwd::BoxD box;
        if (doc.fragment_world_box(i, box)) h.value(box);
    }
    for (uint32_t i = static_cast<uint32_t>(t); i < doc.spatial_node_count();
         i += kTreeThreads) {
        HNwd::SpatialNode n;
        if (!doc.spatial_node(i, n)) continue;
        h.value(n.parent);
        h.value(n.item_begin);
        h.value(n.item_count);
        h.value(n.primitives);
        h.value(n.bounds);
        if (n.child_count > 0) h.value(doc.spatial_child(i, 0));
    }
    if (const uint32_t* items = doc.spatial_items()) h.value(items[t]);
    h.value(doc.record_count());
    h.value(doc.normal_bits());
    return h.h;
}

// Geometrin för vart kMeshEvery:e av trådens fragment, genom trådens läsare.
uint64_t mesh_pass(GeometryReader& reader, size_t count, size_t t) {
    HNwd::Mesh mesh;
    uint64_t sum = 0;
    for (size_t i = t; i < count; i += kTreeThreads * kMeshEvery)
        sum += mesh_hash(reader.fragment_mesh(i, mesh), mesh) * (i + 1);
    return sum;
}

// En nod som trädets anrop ger den, med fliken som PropertyReader ger.
uint64_t node_hash(const Document& doc, uint32_t i, bool& ok) {
    Hash h;
    HNwd::Node n;
    ok = doc.node(i, n);
    if (!ok) return 0;
    h.value(n.parent);
    h.value(n.child_count);
    h.value(n.subtree_size);
    h.value(n.kind);
    h.value(n.element);
    h.text(n.name);
    h.text(n.display);
    h.text(n.internal);
    h.text(n.material);
    h.value(doc.first_child(i));
    h.value(doc.next_sibling(i));
    return h.h;
}

uint64_t model_hash(const HNwd::Model& m) {
    Hash h;
    h.text(m.name);
    h.text(m.source);
    h.text(m.loader);
    h.value(m.unit.code);
    h.value(m.node);
    h.value(m.first_fragment);
    h.value(m.fragment_count);
    return h.h;
}

uint64_t properties_hash(const HNwd::NodeProperties& p) {
    Hash h;
    h.value(p.tabs.size());
    for (const HNwd::Tab& t : p.tabs) {
        h.text(t.name);
        h.text(t.internal);
        h.value(t.property_count);
    }
    for (const HNwd::Property& q : p.properties) {
        h.text(q.name);
        h.text(q.internal);
        h.value(q.raw_type);
        h.value(q.type);
        h.value(q.d);
        h.value(q.i);
        h.text(q.text);
    }
    return h.h;
}

// Facit ur ett dokument där allt är läst utan samtidighet.
struct TreeFacit {
    std::vector<uint64_t> lookups;   // per tråd
    std::vector<uint64_t> meshes;    // per tråd
    size_t nodes = 0;
    std::vector<uint64_t> node_hashes;   // nod 0, kNodeStep, 2·kNodeStep …
    std::vector<uint64_t> models;
    std::vector<uint32_t> leaves;        // node_of_fragment för vart kNodeStep:e fragment
    std::vector<uint32_t> parents;       // parent() för samma noder som node_hashes
    size_t guids = 0;
    uint8_t first_guid[16] = {};
    std::vector<uint64_t> properties;    // PropertyReader för samma noder
};

// Vad en tråd såg medan trädet lästes.
struct TreeSeen {
    uint64_t passes = 0;
    uint64_t wrong = 0;          // svar som varken är "inte inläst" eller facit
    uint64_t not_loaded = 0;     // uppslag i trädet som gav "inte inläst"
    uint64_t loaded = 0;         // uppslag i trädet som gav facit
    uint64_t went_back = 0;      // "inte inläst" efter att trädet setts
    uint64_t reader_reads = 0;   // noder lästa med PropertyReader
    std::string first_wrong;
};

// Det en tråd har sett publiceras: trädet och GUID-tabellen, som load_tree()
// publicerar var för sig (tabellen först).
struct Seen {
    bool tree = false;
    bool guids = false;
};

// Trädets anrop: varje svar är "inte inläst" eller facit, och efter att ett
// träd setts kommer "inte inläst" aldrig igen. Samma för GUID-tabellen.
void tree_pass(const Document& doc, const TreeFacit& facit, size_t t, Seen& seen,
               TreeSeen& out) {
    auto wrong = [&](const char* what) {
        if (out.wrong++ == 0) out.first_wrong = what;
    };
    auto observe = [&](bool& flag, bool loaded, bool matches, const char* what) {
        if (!loaded) {
            ++out.not_loaded;
            if (flag) ++out.went_back;
            return;
        }
        if (!matches) {
            wrong(what);
            return;
        }
        ++out.loaded;
        flag = true;
    };

    const size_t nodes = doc.node_count();
    observe(seen.tree, nodes != 0, nodes == facit.nodes, "node_count");
    observe(seen.tree, doc.has_tree(), true, "has_tree");
    for (size_t k = t; k < facit.node_hashes.size(); k += kTreeThreads) {
        const uint32_t i = static_cast<uint32_t>(k * kNodeStep);
        // node_hash gör flera anrop; har node() svarat är trädet publicerat,
        // och det tas aldrig bort, så resten svarar ur samma noder.
        bool ok = false;
        const uint64_t h = node_hash(doc, i, ok);
        observe(seen.tree, ok, h == facit.node_hashes[k], "node");
        const uint32_t p = doc.parent(i);
        // Roten har ingen förälder också när trädet är läst.
        if (i == 0) {
            if (p != HNwd::kNone) wrong("parent of the root");
        } else {
            observe(seen.tree, p != HNwd::kNone, p == facit.parents[k], "parent");
        }
    }
    for (size_t k = t; k < facit.leaves.size(); k += kTreeThreads) {
        const uint32_t leaf = doc.node_of_fragment(k * kNodeStep);
        observe(seen.tree, leaf != HNwd::kNone, leaf == facit.leaves[k], "node_of_fragment");
    }
    const size_t models = doc.model_count();
    observe(seen.tree, models != 0, models == facit.models.size(), "model_count");
    for (size_t i = 0; i < facit.models.size(); ++i) {
        HNwd::Model m;
        const bool ok = doc.model(i, m);
        observe(seen.tree, ok, ok && model_hash(m) == facit.models[i], "model");
    }
    const size_t guids = doc.guid_count();
    observe(seen.guids, guids != 0, guids == facit.guids, "guid_count");
    uint8_t g[16];
    const bool has_guid = doc.guid(0, g);
    observe(seen.guids, has_guid, has_guid && std::memcmp(g, facit.first_guid, 16) == 0, "guid");
}

// Trådens arbete: varv efter varv tills huvudtråden säger till, och ett
// sista varv efter det.
void tree_worker(const Document& doc, GeometryReader& reader, const TreeFacit& facit, size_t t,
                 std::atomic<int>& started, const std::atomic<bool>& done, TreeSeen& out) {
    started.fetch_add(1);
    Seen seen_tree;
    HNwd::PropertyReader props;
    HNwd::NodeProperties p;
    const size_t count = doc.fragment_count();
    for (;;) {
        const bool last = done.load(std::memory_order_acquire);
        tree_pass(doc, facit, t, seen_tree, out);
        if (lookup_pass(doc, t) != facit.lookups[t] && out.wrong++ == 0)
            out.first_wrong = "lookups";
        if (mesh_pass(reader, count, t) != facit.meshes[t] && out.wrong++ == 0)
            out.first_wrong = "meshes";
        // En läsare av egenskaper öppnas så snart ett träd är publicerat.
        if (!props.is_open()) {
            const Status st = props.open(doc);
            if (st != Status::Ok && st != Status::NotLoaded && out.wrong++ == 0)
                out.first_wrong = "PropertyReader::open";
        }
        if (props.is_open()) {
            for (size_t k = t; k < facit.properties.size(); k += kTreeThreads * 8) {
                const uint32_t i = static_cast<uint32_t>(k * kNodeStep);
                if (props.read(i, p) != Status::Ok || properties_hash(p) != facit.properties[k]) {
                    if (out.wrong++ == 0) out.first_wrong = "PropertyReader::read";
                }
                ++out.reader_reads;
            }
        }
        ++out.passes;
        if (last) break;
    }
}

// Ett varv: dokumentet med fragment och rumsligt index men utan träd och
// GUID-tabell; fyra trådar arbetar medan huvudtråden läser trädet med
// Structure och sedan med Full.
int concurrent_tree_round(const char* path, const TreeFacit& facit, bool verbose) {
    int fails = 0;
    Document doc;
    CHECK(doc.open(path) == Status::Ok);
    CHECK(doc.load_spatial() == Status::Ok);
    CHECK(!doc.has_tree() && doc.guid_count() == 0);

    std::vector<GeometryReader> readers(kTreeThreads);
    for (GeometryReader& r : readers) CHECK(r.open(doc, size_t(4) << 20) == Status::Ok);

    std::atomic<int> started{0};
    std::atomic<bool> done{false};
    std::vector<TreeSeen> seen(kTreeThreads);
    std::vector<std::thread> threads;
    for (size_t t = 0; t < kTreeThreads; ++t)
        threads.emplace_back([&, t] {
            tree_worker(doc, readers[t], facit, t, started, done, seen[t]);
        });
    while (started.load() < static_cast<int>(kTreeThreads)) std::this_thread::yield();

    const Status structure = doc.load_tree(HNwd::TreeContent::Structure);
    const std::string structure_error = doc.last_error();
    const bool structure_tabs = doc.has_tabs();
    const Status full = doc.load_tree(HNwd::TreeContent::Full);
    const std::string full_error = doc.last_error();
    done.store(true, std::memory_order_release);
    for (std::thread& th : threads) th.join();

    CHECK(structure == Status::Ok && structure_error.empty() && !structure_tabs);
    CHECK(full == Status::Ok && full_error.empty() && doc.has_tabs());
    if (structure != Status::Ok || full != Status::Ok)
        std::printf("  FAIL — load_tree: %s / %s\n", structure_error.c_str(), full_error.c_str());

    TreeSeen total;
    for (const TreeSeen& s : seen) {
        CHECK(s.wrong == 0 && s.went_back == 0 && s.passes >= 1);
        if (s.wrong)
            std::printf("  FAIL — %llu wrong answers, the first from %s\n",
                        static_cast<unsigned long long>(s.wrong), s.first_wrong.c_str());
        total.passes += s.passes;
        total.not_loaded += s.not_loaded;
        total.loaded += s.loaded;
        total.reader_reads += s.reader_reads;
    }

    // Efteråt: samma svar som facit, i den här tråden.
    Seen seen_tree{true, true};
    TreeSeen after;
    for (size_t t = 0; t < kTreeThreads; ++t) {
        tree_pass(doc, facit, t, seen_tree, after);
        CHECK(lookup_pass(doc, t) == facit.lookups[t]);
    }
    CHECK(after.wrong == 0 && after.not_loaded == 0);

    if (verbose)
        std::printf("  load_tree beside %zu threads: %llu passes, %llu lookups not loaded, "
                    "%llu loaded, %llu property reads\n",
                    kTreeThreads, static_cast<unsigned long long>(total.passes),
                    static_cast<unsigned long long>(total.not_loaded),
                    static_cast<unsigned long long>(total.loaded),
                    static_cast<unsigned long long>(total.reader_reads));
    return fails;
}

// Facit och `rounds` varv.
int concurrent_tree(const char* path, int rounds) {
    int fails = 0;
    Document ref;
    CHECK(ref.open(path) == Status::Ok);
    CHECK(ref.load_spatial() == Status::Ok);
    CHECK(ref.load_tree(HNwd::TreeContent::Full) == Status::Ok);
    if (fails) return fails;

    TreeFacit facit;
    {
        GeometryReader reader;
        CHECK(reader.open(ref) == Status::Ok);
        for (size_t t = 0; t < kTreeThreads; ++t) {
            facit.lookups.push_back(lookup_pass(ref, t));
            facit.meshes.push_back(mesh_pass(reader, ref.fragment_count(), t));
        }
    }
    facit.nodes = ref.node_count();
    HNwd::PropertyReader props;
    CHECK(props.open(ref) == Status::Ok);
    HNwd::NodeProperties p;
    for (uint32_t i = 0; i < facit.nodes; i += kNodeStep) {
        bool ok = false;
        facit.node_hashes.push_back(node_hash(ref, i, ok));
        CHECK(ok);
        facit.parents.push_back(ref.parent(i));
        CHECK(props.read(i, p) == Status::Ok);
        facit.properties.push_back(properties_hash(p));
    }
    for (size_t f = 0; f < ref.fragment_count(); f += kNodeStep)
        facit.leaves.push_back(ref.node_of_fragment(f));
    for (size_t i = 0; i < ref.model_count(); ++i) {
        HNwd::Model m;
        CHECK(ref.model(i, m));
        facit.models.push_back(model_hash(m));
    }
    CHECK(facit.models.size() == 9);
    facit.guids = ref.guid_count();
    CHECK(facit.guids > 0 && ref.guid(0, facit.first_guid));
    if (fails) return fails;

    for (int r = 0; r < rounds && fails == 0; ++r)
        fails += concurrent_tree_round(path, facit, r == 0);
    return fails;
}

} // namespace

int test_readers() {
    int fails = 0;

    // Utan inlästa fragment går en läsare inte att öppna.
    {
        Document empty;
        GeometryReader r;
        HNwd::Mesh m;
        CHECK(r.open(empty) == Status::NotLoaded && *r.last_error() != 0);
        CHECK(!r.is_open() && r.fragment_mesh(0, m) == Status::NotLoaded);
        CHECK(r.record_mesh(1, m) == Status::NotLoaded);
    }

    const char* path = testdata::kSampleNwd;
    SKIP_IF_MISSING(path);

    Document doc;
    CHECK(doc.open(path) == Status::Ok);
    CHECK(doc.load_fragments() == Status::Ok);
    const size_t count = doc.fragment_count();
    CHECK(count == 56247);

    // En tråd: facit.
    Result single;
    {
        GeometryReader r;
        CHECK(r.open(doc) == Status::Ok && r.is_open());
        read_part(r, count, 0, 1, false, single);
        HNwd::Mesh m;
        CHECK(r.fragment_mesh(count, m) == Status::OutOfRange);
        CHECK(r.record_mesh(0, m) == Status::OutOfRange);
        CHECK(r.record_mesh(1, m) == Status::Ok && *r.last_error() == 0);
    }
    CHECK(single.ok + single.other == count && single.ok > 0);

    // Dokumentets egen fragment_mesh ger samma.
    {
        Result own;
        HNwd::Mesh mesh;
        for (size_t i = 0; i < count; ++i) {
            const Status st = doc.fragment_mesh(i, mesh);
            own.sum += mesh_hash(st, mesh) * (i + 1);
        }
        CHECK(own.sum == single.sum);
    }

    constexpr size_t kThreads = 4;
    // Samma uppdelning i en tråd: ordningen och cachen ändrar ingenting.
    {
        Result seq;
        for (size_t t = 0; t < kThreads; ++t) {
            GeometryReader r;
            CHECK(r.open(doc, size_t(4) << 20) == Status::Ok);
            Result part;
            read_part(r, count, t, kThreads, t % 2 == 1, part);
            seq.sum += part.sum;
        }
        CHECK(seq.sum == single.sum);
    }

    // Fyra trådar, var och en med sin läsare och en liten cache, åt olika
    // håll. Läsarna öppnas här, och dokumentet stängs när trådarna har startat.
    std::vector<GeometryReader> readers(kThreads);
    for (GeometryReader& r : readers) CHECK(r.open(doc, size_t(4) << 20) == Status::Ok);
    std::vector<Result> parts(kThreads);
    std::vector<std::thread> threads;
    for (size_t t = 0; t < kThreads; ++t)
        threads.emplace_back([&, t] { read_part(readers[t], count, t, kThreads, t % 2 == 1, parts[t]); });
    doc.close();
    CHECK(!doc.is_open() && doc.fragment_count() == 0);
    for (std::thread& th : threads) th.join();

    Result together;
    for (const Result& p : parts) {
        together.sum += p.sum;
        together.ok += p.ok;
        together.other += p.other;
    }
    CHECK(together.sum == single.sum);
    CHECK(together.ok == single.ok && together.other == single.other);

    // En läsare går att flytta, och lever vidare utan dokumentet.
    {
        GeometryReader moved(std::move(readers[0]));
        HNwd::Mesh a;
        CHECK(moved.is_open() && moved.fragment_mesh(0, a) == Status::Ok);
        GeometryReader assigned;
        assigned = std::move(moved);
        HNwd::Mesh b;
        CHECK(assigned.fragment_mesh(0, b) == Status::Ok);
        CHECK(mesh_hash(Status::Ok, a) == mesh_hash(Status::Ok, b));
        assigned.close();
        CHECK(!assigned.is_open() && assigned.fragment_mesh(0, b) == Status::NotLoaded);
        // En läsare som flyttats ifrån går att öppna igen.
        CHECK(moved.open(doc) == Status::NotLoaded);
    }

    fails += concurrent_tree(path, 1);

    std::printf("test_readers: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
