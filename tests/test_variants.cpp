// Former som exempelfilerna inte har: placering med full matris, fler flaggor på utseendet, partitioner under partitioner, ljus
// utan beskrivning, id och text som egna klasser, och attribut med namnpar.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "HNwd.h"
#include "alloc_probe.h"
#include "byte_reader.h"
#include "fragment_view.h"
#include "fragments.h"
#include "hierarchy.h"
#include "object_reader.h"
#include "partition.h"
#include "test_support.h"

using namespace HNwd::detail;
using testdata::Bytes;
using testdata::ByteWriter;

namespace {

void pair(ByteWriter& w, const char* display, const char* internal) {
    w.obj(52);
    w.str(display);
    w.str(internal);
}

// Början på ett fragment fram till posen.
void fragment_head(ByteWriter& w, int32_t a, int32_t seq) {
    w.obj(13);
    w.f64(1.0);
    w.i32(a);
    w.i32(4);
    w.i32(0);
    w.i32(seq);
}

void material54(ByteWriter& w, float base) {
    w.obj(54);
    for (int i = 0; i < 14; ++i) w.f32(base + static_cast<float>(i));
}

int32_t geometry93(ByteWriter& w, int32_t record) {
    const int32_t id = w.obj(93);
    for (const float v : {-1.f, -2.f, -3.f, 1.f, 2.f, 3.f}) w.f32(v);
    for (const double v : {100.0, 200.0, 300.0}) w.f64(v);
    w.f32(0.5f);
    w.i32(7);
    w.i32(record);
    return id;
}

// Två fragment: ett med matrispose och utseendeflaggorna 1 + 32 + 64, ett med
// vanlig flytt och flaggorna 1 + 16. `column_end` och `matrix_end` är talen
// som ska vara 0 och 1, `flags` det första utseendets flaggor, `value` talet
// efter flaggan 64, och `scale_z` matrisens tredje kolumn.
Bytes fragment_stream(float column_end = 0, float matrix_end = 1, int32_t flags = 97,
                      int32_t value = 1, float scale_z = 4) {
    ByteWriter w;
    w.i32(2);

    fragment_head(w, 3, 5);
    w.obj(17);
    // Kolumnerna (0, 2, 0), (-3, 0, 0) och (0, 0, scale_z), var och en följd av 0.
    for (const float v : {0.f, 2.f, 0.f, column_end}) w.f32(v);
    for (const float v : {-3.f, 0.f, 0.f, 0.f}) w.f32(v);
    for (const float v : {0.f, 0.f, scale_z, 0.f}) w.f32(v);
    w.f32(matrix_end);
    for (const double v : {10.0, 20.0, 30.0}) w.f64(v);
    w.obj(55);
    w.i32(flags);
    material54(w, 0.0f);
    if (flags & 32) w.null_ptr();
    if (flags & 64) w.i32(value);
    const int32_t geometry = geometry93(w, 1);

    fragment_head(w, 9, 6);
    w.obj(14);
    for (const double v : {1.0, 2.0, 3.0}) w.f64(v);
    w.obj(55);
    w.i32(17);
    material54(w, 100.0f);
    w.ref(geometry);

    w.pad(8);
    return w.data;
}

// En partitionssvans. `label` nullptr ger formen utan text.
void tail(ByteWriter& w, int lights, const char* label, int32_t label_marker = -1) {
    w.i32(2);
    w.i32(lights);
    for (int i = 0; i < lights; ++i) {
        w.obj(27);
        w.i32(1);
        w.null_ptr();   // ljuset saknar beskrivning
        w.i32(0);
        for (int k = 0; k < 12; ++k) w.f64(0.7);
    }
    for (const double v : {0.3, 0.3, 0.3}) w.f64(v);
    w.i32(0);
    w.str("LcNwcLoaderPlugin:test");
    w.i32(451);
    w.i32(2);
    w.i32(1);
    for (const double v : {0.0, 0.0, 1.0, 0.0, 1.0, 0.0}) w.f64(v);
    w.i32(label_marker >= 0 ? label_marker : (label ? 1 : 0));
    if (label)
        w.str(label);
    else
        w.null_str();
    w.i32(1);
    w.str("C:\\src\\model.src");
    for (const double v : {0.0, 1.0, 0.0, 1.0, 2.0, 3.0, 4.0}) w.f64(v);
    w.i32(1);
    w.pad(8);
}

struct TreeOptions {
    int32_t size92 = 96;       // talet i klass 92
    int32_t flags76 = 0;       // flaggorna i klass 76
    int32_t label_marker = -1; // -1: 0 eller 1 efter om texten finns
    bool partition_under_group = false;
};

// Trädet: en partition med en partition under sig, som har ett material med
// namnpar, en grupp med fyra attribut (ett element-id direkt och tre tomma
// platser) och ett löv.
Bytes tree_stream(const TreeOptions& o = {}) {
    ByteWriter w;
    w.i32(1);
    w.i32(3);
    w.implicit_root();
    w.i32(1);

    w.obj(32);
    w.i32(0);
    w.str("Outer");
    pair(w, "File", "LcOaPartition");
    w.i32(0);
    w.i32(0);
    w.i32(0);
    w.i32(1);

    w.obj(32);
    w.i32(0);
    w.str("Inner");
    pair(w, "File", "LcOaPartition");
    w.i32(0);
    w.i32(0);
    w.i32(1);
    w.obj(31);
    w.str("Steel");
    pair(w, "Material", "material_key");
    w.i32(0);
    for (int i = 0; i < 14; ++i) w.f64(i < 6 ? 1.0 : 0.0);
    w.i32(1);

    w.obj(53);
    w.str("G");
    pair(w, "Group", "LcGroup");
    w.i32(0);
    w.i32(0);
    w.i32(4);
    w.obj(66);
    w.null_str();
    pair(w, "Id", "number_id_key");
    w.i32(0x00020000);
    w.i64a(77);
    w.null_ptr();
    w.null_ptr();
    w.null_ptr();
    w.i32(1);

    if (o.partition_under_group) {
        w.obj(32);
        w.i32(0);
        w.str("Deep");
        pair(w, "File", "LcOaPartition");
        w.i32(0);
        w.i32(0);
        w.i32(0);
        w.i32(0);
        tail(w, 0, nullptr);
    } else {
        w.obj(22);
        w.str("L");
        pair(w, "Leaf", "LcLeaf");
        w.i32(0);
        w.i32(0);
        w.i32(0);
    }

    tail(w, 1, "label", o.label_marker);   // Inner
    tail(w, 0, nullptr);                   // Outer
    w.i32(1);
    w.pad(8);
    return w.data;
}

// Egenskapsströmmen: ett id som text, en text under en nyckel, och klass 92.
Bytes props_stream(const TreeOptions& o = {}) {
    ByteWriter w;
    w.obj(70);
    w.str("0abcdefghijklmnopqrstu");
    pair(w, "TextId", "text_id_key");
    w.i32(0x01000000);

    w.obj(76);
    w.null_str();
    pair(w, "Description", "description_key");
    w.i32(o.flags76);
    w.str("hello");

    w.obj(92);
    w.null_str();
    w.null_ptr();
    w.i32(0);
    w.i32(o.size92);
    w.zeros(48);

    w.pad(8);
    return w.data;
}

ErrorCode parse_tree(const TreeOptions& o, Hierarchy& h, std::string* detail = nullptr) {
    const Bytes tree = tree_stream(o);
    const std::vector<Bytes> props = {props_stream(o)};
    return parse_hierarchy(tree.data(), tree.size(), &props, 0, 1000, h, detail);
}

bool close_to(double a, double b) { return std::fabs(a - b) < 1e-9; }

} // namespace

int test_variants() {
    int fails = 0;

    // Partitionens svans genom ett fönster: antalet ljuspekare prövas bara mot
    // strömmens tak, så listan växer med det som läses och reserveras inte
    // efter antalet.
    {
        ByteWriter w;
        w.i32(2);
        w.i32(260000000);
        w.zeros(size_t(2) << 20);
        const Bytes z = testdata::zlib_store(w.data);
        InflateStream stream;
        stream.open(z.data(), z.size(), kDefaultMaxStreamBytes);
        StreamWindow window(stream, size_t(1) << 20);
        ObjectReader r(window, "tail");
        std::vector<PartitionLight> lights;
        PartitionTail tail;
        alloc_probe::Scope probe;
        const bool ok = read_partition_tail(r, lights, tail);
        const size_t largest = probe.largest();
        CHECK(!ok && r.code() == ErrorCode::CorruptObject);
        CHECK(tail.light_refs.capacity() < 1024);
        CHECK(largest < (size_t(16) << 20));
    }

    // Placeringen med full matris och utseendets flaggor.
    {
        const Bytes s = fragment_stream();
        FragmentStore t;
        std::vector<FragmentExtra> extra;
        std::string detail;
        const ErrorCode e = parse_fragments(s.data(), s.size(), t, &extra, &detail);
        if (e != ErrorCode::Ok) std::printf("  %s\n", detail.c_str());
        CHECK(e == ErrorCode::Ok);
        CHECK(t.items.size() == 2 && t.looks.size() == 2);
        if (t.items.size() == 2) {
            const FragmentView& f = fragment_view(t, 0);
            CHECK(f.pose_class == 17);
            CHECK(f.t[0] == 10 && f.t[1] == 20 && f.t[2] == 30);
            // Kolumnerna i filen blir kolumner i matrisen, som lagras radvis.
            const double want[9] = {0, -3, 0, 2, 0, 0, 0, 0, 4};
            for (int i = 0; i < 9; ++i) CHECK(f.m[i] == want[i]);
            CHECK(f.record == 1 && f.seq == 5 && f.a == 3 && f.look == 0);
            CHECK(extra[0].appearance_flags == 97);

            const FragmentView& g = fragment_view(t, 1);
            CHECK(g.pose_class == 14 && g.t[0] == 1 && g.t[2] == 3);
            CHECK(g.m[0] == 1 && g.m[4] == 1 && g.m[8] == 1 && g.m[1] == 0);
            CHECK(g.record == 1 && g.look == 1);   // geometrin delas, materialet är ett annat
            CHECK(extra[1].appearance_flags == 17);
        }

        auto corrupt = [&](const Bytes& b) {
            FragmentStore out;
            return parse_fragments(b.data(), b.size(), out) == ErrorCode::CorruptObject;
        };
        CHECK(corrupt(fragment_stream(1)));                 // en kolumn slutar inte med 0
        CHECK(corrupt(fragment_stream(0, 2)));              // matrisen slutar inte med 1
        CHECK(corrupt(fragment_stream(0, 1, 97 | 128)));    // en okänd flagga
        CHECK(corrupt(fragment_stream(0, 1, 97, 2)));       // talet efter flaggan 64
        CHECK(corrupt(fragment_stream(0, 1, 97, 1, 0)));    // determinanten 0
        CHECK(!corrupt(fragment_stream(0, 1, 97, 0)));
        CHECK(!corrupt(fragment_stream(0, 1, 1)));          // bara flaggan 1: inga extra fält
        CHECK(!corrupt(fragment_stream(0, 1, 33)));         // bara 32: pekaren, inget tal
        CHECK(!corrupt(fragment_stream(0, 1, 65)));         // bara 64: talet, ingen pekare

        // Varje avhuggen längd ger fel.
        bool all = true;
        for (size_t n = 0; n < s.size(); ++n) {
            FragmentStore out;
            all = all && parse_fragments(s.data(), n, out) != ErrorCode::Ok;
        }
        CHECK(all);
    }

    // En pose med matris i det publika API:t.
    {
        HNwd::Pose p;
        p.affine = true;
        const double m[9] = {0, -3, 0, 2, 0, 0, 0, 0, 4};
        for (int i = 0; i < 9; ++i) p.m[i] = m[i];
        p.t[0] = 10;
        p.t[1] = 20;
        p.t[2] = 30;
        // q och s gäller inte för en pose med matris.
        p.q[0] = 1;
        p.q[3] = 0;
        p.s = -5;

        const float v[3] = {1, 1, 1};
        double w[3];
        HNwd::world_point(p, v, w);
        CHECK(close_to(w[0], 7) && close_to(w[1], 22) && close_to(w[2], 34));
        CHECK(!HNwd::mirrors(p));

        const float nx[3] = {1, 0, 0};
        float n[3];
        HNwd::world_normal(p, nx, n);
        CHECK(close_to(n[0], 0) && close_to(n[1], 1) && close_to(n[2], 0));

        // Skjuvning: normalen ska stå vinkelrätt mot ytans riktningar efter
        // placeringen, vilket den inte gör om den bara följer matrisen.
        HNwd::Pose shear;
        shear.affine = true;
        shear.m[1] = 1;   // x' = x + y
        HNwd::world_normal(shear, nx, n);
        // Ytan med normalen x spänns av y och z; y blir (1, 1, 0).
        CHECK(std::fabs(n[0] * 1 + n[1] * 1) < 1e-6 && std::fabs(n[2]) < 1e-6);
        CHECK(close_to(n[0] * n[0] + n[1] * n[1] + n[2] * n[2], 1) ||
              std::fabs(n[0] * n[0] + n[1] * n[1] + n[2] * n[2] - 1) < 1e-6);
        CHECK(n[0] > 0);

        HNwd::Pose mirror;
        mirror.affine = true;
        mirror.m[0] = -1;
        CHECK(HNwd::mirrors(mirror));
        HNwd::world_normal(mirror, nx, n);
        CHECK(close_to(n[0], -1) && close_to(n[1], 0) && close_to(n[2], 0));
        double mw[3];
        HNwd::world_point(mirror, v, mw);
        CHECK(close_to(mw[0], -1) && close_to(mw[1], 1) && close_to(mw[2], 1));
    }

    // Trädet med partitioner under partitioner och de nya klasserna.
    {
        Hierarchy h;
        std::string detail;
        const ErrorCode e = parse_tree(TreeOptions{}, h, &detail);
        if (e != ErrorCode::Ok) std::printf("  %s\n", detail.c_str());
        CHECK(e == ErrorCode::Ok);
        CHECK(h.nodes.size() == 4 && h.top.size() == 1);
        CHECK(h.expanded_total == 5);
        CHECK(h.null_slots == 3);
        CHECK(h.partitions.size() == 2 && h.lights.size() == 1);
        if (e == ErrorCode::Ok && h.nodes.size() == 4 && h.partitions.size() == 2) {
            auto name = [&](const StoredNode& n) { return std::string(h.strings.get(n.name)); };
            const uint32_t oi = h.top[0];
            const StoredNode& outer = h.nodes[oi];
            CHECK(outer.cls == 32 && name(outer) == "Outer" && h.child_count(oi) == 1);
            const uint32_t ii = h.children[outer.child_begin];
            const StoredNode& inner = h.nodes[ii];
            CHECK(inner.cls == 32 && name(inner) == "Inner" && h.child_count(ii) == 1);
            CHECK(std::string(h.strings.get(inner.material)) == "Steel");
            const uint32_t gi = h.children[inner.child_begin];
            const StoredNode& group = h.nodes[gi];
            CHECK(group.cls == 53 && name(group) == "G" && h.child_count(gi) == 1);
            const StoredNode& leaf = h.nodes[h.children[group.child_begin]];
            CHECK(leaf.cls == 22 && name(leaf) == "L");

            // Inre partitionens svans lästes först: den har texten och ljuset.
            CHECK(h.partition(ii) == 0 && h.partition(oi) == 1);
            const PartitionTail& inner_tail = h.partitions[h.partition(ii)];
            const PartitionTail& outer_tail = h.partitions[h.partition(oi)];
            CHECK(inner_tail.label == "label" && inner_tail.light_refs.size() == 1);
            CHECK(inner_tail.format_version == 451);
            CHECK(inner_tail.source_path == "C:\\src\\model.src");
            CHECK(outer_tail.label.empty() && outer_tail.light_refs.empty());

            // Gruppens flikar i attributens ordning: element-id:t som stod
            // direkt, sedan id:t som text och texten ur egenskapsströmmen.
            // Klass 92 ger ingen flik.
            CHECK(h.tab_count(gi) == 3);
            if (h.tab_count(gi) == 3) {
                const StoredTab& id = h.tabs[h.node_tabs[h.tab_begin[gi]]];
                const StoredTab& guid = h.tabs[h.node_tabs[h.tab_begin[gi] + 1]];
                const StoredTab& text = h.tabs[h.node_tabs[h.tab_begin[gi] + 2]];
                CHECK(id.source_class == 66 && id.flags == 0x00020000 && id.prop_count == 1);
                CHECK(h.properties[id.prop_begin].type == 2 && h.properties[id.prop_begin].i == 77);
                CHECK(guid.source_class == 70 && guid.prop_count == 1);
                CHECK(h.properties[guid.prop_begin].type == 4);
                CHECK(std::string(h.strings.get(h.properties[guid.prop_begin].text)) ==
                      "0abcdefghijklmnopqrstu");
                CHECK(std::string(h.strings.get(h.names[guid.user].internal)) == "text_id_key");
                CHECK(text.source_class == 76 && text.prop_count == 1);
                CHECK(std::string(h.strings.get(h.properties[text.prop_begin].text)) == "hello");
            }
        }

        // Klass 92 i en annan storlek avkodas inte, och gissas inte.
        TreeOptions o;
        o.size92 = 95;
        CHECK(parse_tree(o, h, &detail) == ErrorCode::Unsupported);
        CHECK(detail.find("class 92") != std::string::npos);

        o = TreeOptions{};
        o.flags76 = 1;
        CHECK(parse_tree(o, h) == ErrorCode::CorruptObject);

        o = TreeOptions{};
        o.label_marker = 2;
        CHECK(parse_tree(o, h) == ErrorCode::CorruptObject);

        // En partition får sitta under en partition, inte under en grupp.
        o = TreeOptions{};
        o.partition_under_group = true;
        CHECK(parse_tree(o, h) != ErrorCode::Ok);

        // Varje avhuggen längd av trädet ger fel.
        const Bytes tree = tree_stream();
        const std::vector<Bytes> props = {props_stream()};
        bool all = true;
        for (size_t n = 0; n < tree.size(); n += 4)
            all = all && parse_hierarchy(tree.data(), n, &props, 0, 1000, h) != ErrorCode::Ok;
        CHECK(all);
    }

    std::printf("test_variants: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
