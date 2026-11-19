// nwddump — visar hur en fil är uppbyggd: sektionerna, katalogen och varje
// pakets sista ström uppackad.
//
//   nwddump <fil> [--hex N] [--partition] [--spatial] [--units] [--elements]
//
// --hex N skriver de N första byten av varje uppackad ström (förval 64, 0 = inga).
// --partition packar upp LcOpNwdPartition i stället för att lista strömmarna:
// huvudet i klass 32, var svansen börjar och slutar, svansen som hexdump med
// heltal, flyttal och strängar bredvid, och vad parse_partition_stream() säger.
// --spatial tolkar LcOpNwdSpatialHierarchy mot antalet fragment: noder, löv,
// djup, barnfördelning, de största löven och om posterna är en permutation.
// --units skriver enhetskoden i filens egen partition (LcOpNwdPartition) och
// vad den är i meter.
// --elements tolkar de små paketen (vyerna, ljusen, bakgrunden, rutnätet,
// materialen, filens identitet, loggen …) och skriver vad de innehåller; ett
// paket utan tolk skrivs som "not decoded" med sin storlek.
// --spatial, --units och --elements går att ge tillsammans, och ersätter listan
// över strömmarna.

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "byte_reader.h"
#include "container.h"
#include "elements.h"
#include "guid_store.h"
#include "inflate.h"
#include "mapped_file.h"
#include "object_reader.h"
#include "partition.h"
#include "spatial.h"
#include "tool_main.h"

using namespace HNwd::detail;

namespace {

void print_header(const char* label, const SectionHeader& h) {
    std::printf("%s @%" PRIu64 " (%u byte): %s\n", label, h.offset, h.length,
                h.line.c_str());
    std::printf("    format=%s encoding=%s marker=%s version=%d double=%s\n",
                h.format.c_str(), h.encoding.empty() ? "-" : h.encoding.c_str(),
                h.marker.empty() ? "-" : h.marker.c_str(), h.version,
                h.is_double ? "yes" : "no");
}

void hex_dump(const uint8_t* data, size_t size, size_t limit) {
    const size_t n = size < limit ? size : limit;
    for (size_t row = 0; row < n; row += 16) {
        std::printf("      %06zx ", row);
        for (size_t i = row; i < row + 16; ++i) {
            if (i < n) std::printf(" %02x", data[i]);
            else       std::printf("   ");
        }
        std::printf("  ");
        for (size_t i = row; i < row + 16 && i < n; ++i)
            std::putchar(data[i] >= 0x20 && data[i] <= 0x7E ? data[i] : '.');
        std::putchar('\n');
    }
}

// De första heltalen i en ström, som de ofta inleds med räknare och markörer.
void print_ints(const uint8_t* data, size_t size, size_t count) {
    ByteReader r(data, size);
    std::printf("      int32:");
    for (size_t i = 0; i < count; ++i) {
        int32_t v;
        if (!r.i32(v)) break;
        std::printf(" %" PRId32, v);
    }
    std::putchar('\n');
}

// Svansen i en partition som hexdump: varje rad är ett ord om 4 byte med
// offset, hex, int32 och, om ordet ligger på en jämn 8 byte, f64 över de åtta
// byten. En sträng som börjar på ordet skrivs med sin text, och byten i den
// hoppas över.
void dump_tail(const uint8_t* data, size_t begin, size_t end) {
    size_t i = begin;
    while (i + 4 <= end) {
        int32_t v;
        std::memcpy(&v, data + i, 4);
        std::printf("      %06zx  %02x %02x %02x %02x  %11" PRId32, i, data[i], data[i + 1],
                    data[i + 2], data[i + 3], v);
        // En sträng: längd, text av skrivbara tecken och nollor upp till 4.
        if (v >= 2 && static_cast<size_t>(v) <= end - i - 4) {
            bool text = true;
            for (int32_t k = 0; k < v; ++k) {
                const uint8_t c = data[i + 4 + static_cast<size_t>(k)];
                if (c < 0x20 || c > 0x7E) text = false;
            }
            if (text) {
                const size_t padded = (static_cast<size_t>(v) + 3) & ~size_t(3);
                std::printf("  str \"%.*s\"\n", static_cast<int>(v),
                            reinterpret_cast<const char*>(data) + i + 4);
                i += 4 + padded;
                continue;
            }
        }
        if (i % 8 == 0 && i + 8 <= end) {
            double d;
            std::memcpy(&d, data + i, 8);
            if (d > -1e12 && d < 1e12 && (d >= 1e-9 || d <= -1e-9))
                std::printf("  f64 %.17g", d);
        }
        std::putchar('\n');
        i += 4;
    }
}

// En pekare som läses av ObjectReader; skriver vad det blev.
bool tool_ptr(ObjectReader& r, ObjectReader::Ptr& p, std::initializer_list<int32_t> allowed,
              bool allow_null, const char* what) {
    const size_t at = r.pos();
    if (!r.ptr(p, allowed, allow_null, false)) {
        std::printf("    !! %s: %s\n", what, r.detail().c_str());
        return false;
    }
    std::printf("    %06zx %s: %s id %d class %d\n", at, what,
                p.kind == ObjectReader::Ptr::Null ? "null" : "new", p.id, p.cls);
    return true;
}

int dump_partition(const MappedFile& file, const Container& c) {
    const DbCacheEntry* e = find_entry(c.entries, "LcOpNwdPartition");
    if (!e) {
        std::printf("no LcOpNwdPartition package\n");
        return 1;
    }
    StreamSpan span;
    std::vector<uint8_t> data;
    if (last_stream(*e, file.size(), span) != ErrorCode::Ok ||
        inflate_span(file.data(), file.size(), span, kDefaultMaxStreamBytes, data) !=
            ErrorCode::Ok) {
        std::printf("!! LcOpNwdPartition does not unpack\n");
        return 1;
    }
    std::printf("\nLcOpNwdPartition: %zu byte\n", data.size());

    // Huvudet: [100][32] [i32 0] [str namn] PTR(52) [i32 flags] [i32 X] [i32 Y] Y × PTR.
    ObjectReader r(data.data(), data.size(), "LcOpNwdPartition");
    ObjectReader::Ptr p;
    std::string s, t;
    int32_t zero = -1, flags = 0, x = 0, y = 0;
    bool head = tool_ptr(r, p, {32}, false, "node") && r.i32(zero) && r.str(s);
    if (head) {
        std::printf("    [i32 %d] name \"%s\"\n", zero, s.c_str());
        head = tool_ptr(r, p, {52}, true, "name pair");
    }
    if (head && p.kind == ObjectReader::Ptr::New) {
        head = r.str(s) && r.str(t) && r.finish(p.id, 0);
        if (head) std::printf("    name pair \"%s\" / \"%s\"\n", s.c_str(), t.c_str());
    }
    head = head && r.i32(flags) && r.i32(x) && r.count(4, y);
    if (head) std::printf("    flags %d, X %d, Y %d\n", flags, x, y);
    for (int32_t i = 0; head && i < y; ++i) {
        head = tool_ptr(r, p, {86, 31, 40, 41, 39, 79, 200}, true, "attribute");
        if (head && p.kind == ObjectReader::Ptr::New && p.cls == 31) {
            double d;
            head = r.str(s);
            for (int k = 0; head && k < 15; ++k) head = r.f64(d);
            head = head && r.finish(p.id, 0);
        } else if (head) {
            std::printf("    attribute class %d is not read here\n", p.cls);
            head = false;
        }
    }
    if (!head) {
        std::printf("    !! header: %s\n", r.detail().c_str());
        return 1;
    }

    // Svansen slutar före [i32 1] och nollorna upp till 8 sist i strömmen.
    const size_t tail_begin = r.pos();
    size_t tail_end = data.size();
    while (tail_end > tail_begin && data[tail_end - 1] == 0) --tail_end;
    if (tail_end > tail_begin && data[tail_end - 1] == 1) --tail_end;
    else std::printf("    !! the stream does not end with [1] and zeros\n");
    tail_end = (tail_end + 3) & ~size_t(3);
    if (tail_end > data.size()) tail_end = data.size();
    std::printf("    tail begins at %zu, ends at %zu (%zu byte); [1] and the pad end the stream at %zu\n",
                tail_begin, tail_end, tail_end - tail_begin, data.size());
    std::printf("      offset  bytes        int32        f64 / string\n");
    dump_tail(data.data(), tail_begin, tail_end);

    Partition part;
    std::string detail;
    const ErrorCode pe = parse_partition_stream(data.data(), data.size(), part, &detail);
    std::printf("\nparse_partition_stream: %s %s\n", error_message(pe), detail.c_str());
    if (pe == ErrorCode::Ok) {
        const PartitionTail& tl = part.tail;
        std::printf("    name \"%s\", lights %zu, loader \"%s\", format %d, unit code %d\n"
                    "    source \"%s\"\n",
                    part.name.c_str(), part.lights.size(), tl.loader.c_str(), tl.format_version,
                    tl.unit_code, tl.source_path.c_str());
    }
    return pe == ErrorCode::Ok ? 0 : 1;
}

// Paketets sista ström uppackad; skriver felet och ger false om det inte går.
bool unpack(const MappedFile& file, const Container& c, const char* name,
            std::vector<uint8_t>& out) {
    const DbCacheEntry* e = find_entry(c.entries, name);
    if (!e) {
        std::printf("!! no %s package\n", name);
        return false;
    }
    StreamSpan span;
    ErrorCode ec = last_stream(*e, file.size(), span);
    if (ec == ErrorCode::Ok)
        ec = inflate_span(file.data(), file.size(), span, kDefaultMaxStreamBytes, out);
    if (ec != ErrorCode::Ok) {
        std::printf("!! %s does not unpack: %s\n", name, error_message(ec));
        return false;
    }
    return true;
}

int dump_spatial(const MappedFile& file, const Container& c) {
    std::vector<uint8_t> frags, data;
    if (!unpack(file, c, "LcOpNwdFragments", frags) ||
        !unpack(file, c, "LcOpNwdSpatialHierarchy", data))
        return 1;
    // Fragmentströmmen börjar med antalet fragment.
    ByteReader fr(frags.data(), frags.size());
    int32_t count = 0;
    if (!fr.i32(count) || count < 0) {
        std::printf("!! LcOpNwdFragments does not start with a fragment count\n");
        return 1;
    }
    std::printf("\nLcOpNwdSpatialHierarchy: %zu byte, %d fragments\n", data.size(), count);

    SpatialTree t;
    std::string detail;
    const ErrorCode e =
        parse_spatial_hierarchy(data.data(), data.size(), static_cast<uint32_t>(count), t, &detail);
    std::printf("    parse_spatial_hierarchy: %s %s\n", error_message(e), detail.c_str());
    if (e != ErrorCode::Ok) return 1;

    // Föräldern står före barnet i preorder, så djupet räknas i en följd.
    std::vector<uint32_t> depth(t.nodes.size(), 0), leaf_sizes;
    std::vector<uint64_t> fanout;   // antalet inre noder med k barn, index k
    uint32_t inner = 0, max_depth = 0, single = 0;
    for (size_t i = 0; i < t.nodes.size(); ++i) {
        const SpatialTree::Node& n = t.nodes[i];
        if (i > 0) depth[i] = depth[n.parent] + 1;
        max_depth = std::max(max_depth, depth[i]);
        if (n.child_count > 0) {
            ++inner;
            if (fanout.size() <= n.child_count) fanout.resize(n.child_count + size_t(1), 0);
            ++fanout[n.child_count];
        } else {
            leaf_sizes.push_back(n.item_count);
            if (n.item_count == 1) ++single;
        }
    }
    std::printf("    nodes %zu: inner %u, leaves %zu (with a single fragment %u), depth %u (root 0)\n",
                t.nodes.size(), inner, leaf_sizes.size(), single, max_depth);
    std::printf("    children per inner node (k:count):");
    for (size_t k = 0; k < fanout.size(); ++k)
        if (fanout[k]) std::printf(" %zu:%" PRIu64, k, fanout[k]);
    std::putchar('\n');

    std::sort(leaf_sizes.begin(), leaf_sizes.end(), std::greater<uint32_t>());
    std::printf("    largest leaves:");
    for (size_t i = 0; i < leaf_sizes.size() && i < 8; ++i) std::printf(" %u", leaf_sizes[i]);
    std::printf("; mean %.1f\n",
                leaf_sizes.empty() ? 0.0 : static_cast<double>(t.items.size()) / static_cast<double>(leaf_sizes.size()));

    // Permutationen prövas här en gång till, oberoende av tolken.
    std::vector<uint8_t> seen(static_cast<size_t>(count), 0);
    bool permutation = t.items.size() == static_cast<size_t>(count);
    for (const uint32_t f : t.items) {
        if (f >= static_cast<uint32_t>(count) || seen[f]) permutation = false;
        else seen[f] = 1;
    }
    std::printf("    entries %zu, a permutation of 0..%d: %s\n", t.items.size(), count - 1,
                permutation ? "yes" : "NO");
    return permutation ? 0 : 1;
}

int dump_units(const MappedFile& file, const Container& c) {
    std::vector<uint8_t> data;
    if (!unpack(file, c, "LcOpNwdPartition", data)) return 1;
    Partition part;
    std::string detail;
    const ErrorCode e = parse_partition_stream(data.data(), data.size(), part, &detail);
    if (e != ErrorCode::Ok) {
        std::printf("\n!! parse_partition_stream: %s %s\n", error_message(e), detail.c_str());
        return 1;
    }
    double metres = 0;
    if (metres_per_unit(part.tail.unit_code, metres))
        std::printf("\nunits: code %d, %.6g metres per unit\n", part.tail.unit_code, metres);
    else
        std::printf("\nunits: code %d, metres per unit unknown\n", part.tail.unit_code);
    return 0;
}


// Paket som andra tolkar läser; --elements går förbi dem.
bool read_elsewhere(std::string_view leaf) {
    for (const char* name : {"LcOpNwdGeometry", "LcOaPartitionProps", "LcOpNwdLogicalHierarchy",
                             "LcOpNwdFragments", "LcOpGuidStore", "LcOpNwdPartition",
                             "LcOpNwdSpatialHierarchy", "LcOpDBCache"})
        if (leaf == name) return true;
    return false;
}

void print_guid(const char* label, const Guid16& g) {
    char text[37];
    guid_text(g, text);
    std::printf("    %s %s\n", label, text);
}

void print_f64s(const char* label, const double* v, int n) {
    std::printf("    %s", label);
    for (int i = 0; i < n; ++i) std::printf(" %.17g", v[i]);
    std::putchar('\n');
}

// Snittplanen i en vy: de påslagna planen, och resten på en rad.
void print_clip(const ViewClip& c, const char* indent) {
    std::printf("%sclip head %d %d %d %d %d %d, %.17g %.17g %.17g; flags %d %d / %d %d\n", indent,
                c.head[0], c.head[1], c.head[2], c.head[3], c.head[4], c.head[5], c.head_values[0],
                c.head_values[1], c.head_values[2], c.flags_a[0], c.flags_a[1], c.flags_b[0],
                c.flags_b[1]);
    std::printf("%sclip modes", indent);
    for (const ClipPlane& p : c.planes) std::printf(" %d%s", p.mode, p.enabled ? "*" : "");
    std::printf(", rotation %.17g %.17g %.17g %.17g\n", c.rotation[0], c.rotation[1],
                c.rotation[2], c.rotation[3]);
    for (int k = 0; k < 6; ++k) {
        const ClipPlane& p = c.planes[k];
        if (!p.enabled) continue;
        std::printf("%splane %d: point %.17g %.17g %.17g, normal %.17g %.17g %.17g, tangent %.17g %.17g %.17g\n",
                    indent, k, p.point[0], p.point[1], p.point[2], p.normal[0], p.normal[1],
                    p.normal[2], p.tangent[0], p.tangent[1], p.tangent[2]);
    }
}

void print_view(const ViewElement& v) {
    std::printf("    form %d, projection %d\n", v.form, v.projection);
    print_f64s("position", v.position, 3);
    print_f64s("rotation", v.rotation, 4);
    std::printf("    u %.17g, angle %.17g, near %.17g, far %.17g\n", v.u, v.angle, v.near_depth,
                v.far_depth);
    print_f64s("zero", v.zero, 2);
    if (v.form != 9) {
        print_f64s("avatar values", v.avatar_values, 4);
        std::printf("    avatar \"%s\"\n", v.avatar.c_str());
        print_f64s("after avatar", v.after_avatar, 4);
        std::printf("    avatar flag %d\n", v.avatar_flag);
        print_f64s("more", v.more, 4);
    }
    if (v.form == 9 || v.form == 843) std::printf("    distance %.17g\n", v.distance);
    else print_f64s("extra", v.extra, 6);
    if (v.form == 1919) std::printf("    ortho extra %.17g\n", v.ortho_extra);
    std::printf("    ints");
    for (const int32_t k : v.tail_ints) std::printf(" %d", k);
    std::putchar('\n');
    print_clip(v.clip, "    ");
}

void print_material(const MaterialData& m) {
    std::printf("      json %zu byte, %zu files", m.json.size(), m.files.size());
    for (const MaterialFile& file : m.files)
        std::printf(" [\"%s\" %s %zu byte]", file.name.c_str(),
                    file.embedded ? file.type.c_str() : "not embedded", file.bytes.size());
    std::putchar('\n');
}

void print_saved_views(const SavedViews& sv) {
    std::printf("    %zu items, %zu class-89 objects\n", sv.items.size(), sv.states.size());
    for (size_t i = 0; i < sv.states.size(); ++i)
        std::printf("    state #%zu \"%s\", %zu indices\n", i, sv.states[i].element.c_str(),
                    sv.states[i].indices.size());
    // Djupet räknas ur föräldrarna; en förälder står före sina poster.
    std::vector<size_t> depth(sv.items.size(), 0);
    for (size_t i = 0; i < sv.items.size(); ++i) {
        const SavedViews::Item& it = sv.items[i];
        if (it.parent != UINT32_MAX) depth[i] = depth[it.parent] + 1;
        const std::string indent(2 * depth[i], ' ');
        char g[37];
        guid_text(it.guid, g);
        if (it.kind == 4) {
            std::printf("    %sfolder \"%s\" %s, %u items\n", indent.c_str(), it.name.c_str(), g,
                        it.child_count);
            continue;
        }
        const SavedViews::View& sv_view = sv.views[it.view];
        const ViewElement& v = sv_view.view;
        std::printf("    %sview \"%s\" %s, form %d, projection %d, %u redlines, states",
                    indent.c_str(), it.name.c_str(), g, v.form, v.projection, sv_view.redline_count);
        for (uint32_t k = 0; k < sv_view.state_count; ++k)
            std::printf(" #%u", sv.state_refs[sv_view.state_begin + k]);
        std::printf("\n    %s  position %.17g %.17g %.17g, rotation %.17g %.17g %.17g %.17g\n",
                    indent.c_str(), v.position[0], v.position[1], v.position[2], v.rotation[0],
                    v.rotation[1], v.rotation[2], v.rotation[3]);
        std::printf("    %s  u %.17g, angle %.17g, near %.17g, far %.17g\n", indent.c_str(), v.u,
                    v.angle, v.near_depth, v.far_depth);
        print_clip(v.clip, ("    " + indent + "  ").c_str());
        for (uint32_t k = 0; k < sv_view.redline_count; ++k) {
            const Redline& rl = sv.redlines[sv_view.redline_begin + k];
            std::printf("    %s  redline type %d", indent.c_str(), rl.type);
            if (rl.type == 4) std::printf(" \"%s\"", rl.text.c_str());
            std::printf(" points %.6g %.6g %.6g %.6g, width %d, colour %.6g %.6g %.6g, pattern %d\n",
                        rl.points[0], rl.points[1], rl.points[2], rl.points[3], rl.width,
                        rl.colour[0], rl.colour[1], rl.colour[2], rl.pattern);
        }
    }
}

void print_grid(const GridElement& g) {
    std::printf("    %zu systems, tail %d %d\n", g.systems.size(), g.tail[0], g.tail[1]);
    for (const GridSystem& s : g.systems) {
        std::printf("    system \"%s\": %zu lines, %zu levels, level %d\n", s.name.c_str(),
                    s.lines.size(), s.levels.size(), s.level);
        print_f64s("  origin", s.origin, 3);
        for (const auto& a : s.axes) print_f64s("  axis", a, 3);
        for (const GridLine& l : s.lines)
            std::printf("      line \"%s\" values %.17g %.17g, ints %d %d %d, ends %.17g %.17g %.17g %.17g\n",
                        l.label.c_str(), l.values[0], l.values[1], l.ints[0], l.ints[1], l.ints[2],
                        l.ends[0], l.ends[1], l.ends[2], l.ends[3]);
        for (const GridLevel& v : s.levels)
            std::printf("      level \"%s\" elevation %.17g\n", v.name.c_str(), v.elevation);
    }
}

void print_lights(const LightsElement& l) {
    std::printf("    %zu lights, %zu class-27 objects, %zu materials\n", l.entries.size(),
                l.lights.size(), l.materials.size());
    for (const LightsElement::Entry& en : l.entries) {
        std::printf("    light \"%s\", kind %d\n", en.name.c_str(), en.kind);
        print_guid("  guid", en.guid);
        const LightBody& body = l.lights[en.light];
        if (body.material == kNoMaterial)
            std::printf("      class 27 #%u, no material\n", en.light);
        else
            std::printf("      class 27 #%u, material #%u, json %zu byte\n", en.light,
                        body.material, l.materials[body.material].json.size());
        print_f64s("  values", body.values, 12);
        print_f64s("  origin", en.origin, 3);
        print_f64s("  direction", en.direction, 3);
    }
}

void print_clash(const ClashElement& ce) {
    std::printf("    %zu roots, %zu tests, %zu rules, %zu models\n", ce.roots.size(),
                ce.tests.size(), ce.rules.size(), ce.models.size());
    for (const ClashRoot& root : ce.roots) {
        char g[37];
        guid_text(root.guid, g);
        std::printf("    root \"%s\" kind %d %s, %u tests\n", root.name.c_str(), root.kind, g,
                    root.test_count);
        for (uint32_t k = 0; k < root.test_count; ++k) {
            const ClashTest& t = ce.tests[root.test_begin + k];
            guid_text(t.guid, g);
            std::printf("      test \"%s\" %s, values %.17g %.17g, zero %.17g %.17g %.17g\n",
                        t.name.c_str(), g, t.values[0], t.values[1], t.zero[0], t.zero[1], t.zero[2]);
        }
    }
    for (const std::string& rule : ce.rules) std::printf("    rule \"%s\"\n", rule.c_str());
    for (const ClashModel& m : ce.models) {
        std::printf("    model \"%s\"\n      path \"%s\"\n", m.file.c_str(), m.path.c_str());
        print_guid("  source guid", m.source_guid);
        std::printf("      prefix \"%s\", name \"%s\", loader \"%s\", mode %d, unit code %d\n",
                    m.prefix.c_str(), m.name.c_str(), m.loader.c_str(), m.mode, m.unit_code);
        print_f64s("  matrix", m.matrix, 9);
        print_f64s("  offset", m.offset, 3);
        print_f64s("  up", m.up, 3);
        print_f64s("  north", m.north, 3);
        std::printf("      again \"%s\" \"%s\", time %" PRId64 ", size %" PRId64 "\n",
                    m.file_again.c_str(), m.path_again.c_str(), m.time, m.size);
        print_f64s("  c", m.c, 3);
        print_guid("  guid", m.guid);
    }
}

// Ett paket tolkat; skriver innehållet eller felet. Ger 1 för ett fel som
// inte är Unsupported.
int dump_element(std::string_view leaf, const std::vector<uint8_t>& d) {
    std::string detail;
    ErrorCode e = ErrorCode::Ok;
    if (leaf == "LcOpCommonSchemas" || leaf == "LcOpZoneElement" ||
        leaf == "LcOpSelectionSetsElement" || leaf == "LcOpPropertyOverrideElement" ||
        leaf == "LcOdpDBDatabaseLinksElement") {
        const std::string what(leaf);
        e = parse_empty_pair(d.data(), d.size(), what.c_str(), &detail);
        if (e == ErrorCode::Ok) std::printf("    [0][0], empty\n");
    } else if (leaf == "LcOpToolElement") {
        int32_t tool = 0;
        e = parse_tool(d.data(), d.size(), tool, &detail);
        if (e == ErrorCode::Ok) std::printf("    tool %d\n", tool);
    } else if (leaf == "LcOpNextCommentIdElement") {
        NextCommentId n;
        e = parse_next_comment_id(d.data(), d.size(), n, &detail);
        if (e == ErrorCode::Ok) std::printf("    values %d %d\n", n.values[0], n.values[1]);
    } else if (leaf == "LcOpNwdSharedNodes") {
        SharedNodes sn;
        e = parse_shared_nodes(d.data(), d.size(), sn, &detail);
        if (e == ErrorCode::Ok) {
            std::printf("    %zu entries, %zu class-102 objects\n", sn.entries.size(), sn.fonts.size());
            for (const FontEntry& fe : sn.fonts)
                std::printf("    font \"%s\" size %.17g, values %d %d %d %d\n", fe.name.c_str(),
                            fe.size, fe.values[0], fe.values[1], fe.values[2], fe.values[3]);
        }
    } else if (leaf == "LcOpSavedViewsElement") {
        SavedViews sv;
        e = parse_saved_views(d.data(), d.size(), sv, &detail);
        if (e == ErrorCode::Ok) print_saved_views(sv);
    } else if (leaf == "LcOpHomeViewElement") {
        ViewElement v;
        e = parse_view_element(d.data(), d.size(), v, &detail, "LcOpHomeViewElement");
        if (e == ErrorCode::Ok) print_view(v);
    } else if (leaf == "LcOpShadOverridesElement") {
        std::vector<ShadOverride> o;
        e = parse_shad_overrides(d.data(), d.size(), o, &detail);
        if (e == ErrorCode::Ok) {
            std::printf("    %zu entries (index:flags):", o.size());
            for (const ShadOverride& x : o) std::printf(" %d:%d", x.index, x.flags);
            std::putchar('\n');
        }
    } else if (leaf == "LcOpTextureSpaceElement") {
        std::vector<TextureSpace> t;
        e = parse_texture_space(d.data(), d.size(), t, &detail);
        if (e == ErrorCode::Ok) {
            std::printf("    %zu entries\n", t.size());
            for (const TextureSpace& x : t) {
                std::printf("    index %d kind %d\n", x.index, x.kind);
                print_f64s("  values", x.values, 16);
            }
        }
    } else if (leaf == "LcOpGridElement") {
        GridElement g;
        e = parse_grid(d.data(), d.size(), g, &detail);
        if (e == ErrorCode::Ok) print_grid(g);
    } else if (leaf == "LcReMaterialElement") {
        ReMaterials m;
        e = parse_re_materials(d.data(), d.size(), m, &detail);
        if (e == ErrorCode::Ok) {
            std::printf("    %zu materials, %zu class-185 objects\n", m.entries.size(),
                        m.materials.size());
            for (const ReMaterials::Entry& en : m.entries) {
                std::printf("    material \"%s\"\n", en.name.c_str());
                print_guid("  guid", en.guid);
                if (en.material == kNoMaterial) std::printf("      no class 185\n");
                else print_material(m.materials[en.material]);
            }
        }
    } else if (leaf == "LcOpNwdStats") {
        std::string text;
        bool non_utf8 = false;
        e = parse_stats(d.data(), d.size(), text, non_utf8, &detail);
        if (e == ErrorCode::Ok) {
            size_t lines = 0;
            for (const char ch : text) lines += ch == '\n';
            std::printf("    text %zu byte, %zu lines%s\n", text.size(), lines,
                        non_utf8 ? ", NOT UTF-8" : "");
        }
    } else if (leaf == "LcOpOdyScenePropsChunk") {
        SceneProps sp;
        e = parse_scene_props(d.data(), d.size(), sp, &detail);
        if (e == ErrorCode::Ok) {
            std::printf("    path \"%s\"%s\n", sp.path.c_str(), sp.path_null ? " (null)" : "");
            print_guid("guid", sp.guid);
        }
    } else if (leaf == "LcOpOdyFileInfoChunk") {
        FileInfo fi;
        e = parse_file_info(d.data(), d.size(), fi, &detail);
        if (e == ErrorCode::Ok) {
            print_guid("file guid ", fi.file_guid);
            std::printf("    tag \"%s\", name \"%s\"\n", fi.tag.c_str(), fi.name.c_str());
            print_guid("scene guid", fi.scene_guid);
            std::printf("    prefix \"%s\"\n", fi.prefix.c_str());
            std::printf("    third prefix \"%s\"%s\n", fi.prefix_third.c_str(),
                        fi.prefix_third_null ? " (null)" : "");
            print_guid("scene guid", fi.scene_guid_again);
            std::printf("    prefix \"%s\"\n", fi.prefix_again.c_str());
            print_guid("guid c    ", fi.guid_c);
            print_guid("guid d    ", fi.guid_d);
        }
    } else if (leaf == "LcOpXRefTable") {
        XRefTable x;
        e = parse_xref_table(d.data(), d.size(), x, &detail);
        if (e == ErrorCode::Ok)
            std::printf("    version %d, saved filename \"%s\", table empty\n", x.version,
                        x.saved_filename.c_str());
    } else if (leaf == "LcOpCurrentViewElement") {
        ViewElement v;
        e = parse_view_element(d.data(), d.size(), v, &detail);
        if (e == ErrorCode::Ok) print_view(v);
    } else if (leaf == "LcOpHeadlightElement") {
        HeadlightElement h;
        e = parse_headlight(d.data(), d.size(), h, &detail);
        if (e == ErrorCode::Ok) print_f64s("values", h.values, 2);
    } else if (leaf == "LcOpSpeedElement") {
        double v = 0;
        e = parse_speed(d.data(), d.size(), v, &detail);
        if (e == ErrorCode::Ok) print_f64s("value", &v, 1);
    } else if (leaf == "LcOpCullingElement") {
        CullingElement cu;
        e = parse_culling(d.data(), d.size(), cu, &detail);
        if (e == ErrorCode::Ok) {
            print_f64s("first", &cu.first, 1);
            print_f64s("values", cu.values, 4);
            std::printf("    flag %d\n", cu.flag);
        }
    } else if (leaf == "LcOpBackgroundElement") {
        BackgroundElement b;
        e = parse_background(d.data(), d.size(), b, &detail);
        if (e == ErrorCode::Ok) {
            std::printf("    mode %d\n", b.mode);
            for (const auto& c : b.colours)
                std::printf("    colour %.6g %.6g %.6g  (x255: %.3f %.3f %.3f)\n", c[0], c[1],
                            c[2], c[0] * 255, c[1] * 255, c[2] * 255);
            if (b.has_sky) {
                std::printf("    sky, class 185:\n");
                print_material(b.sky);
            } else {
                std::printf("    no sky\n");
            }
        }
    } else if (leaf == "LcOpLightsElement") {
        LightsElement l;
        e = parse_lights(d.data(), d.size(), l, &detail);
        if (e == ErrorCode::Ok) print_lights(l);
    } else if (leaf == "LcOpClashElement") {
        ClashElement ce;
        e = parse_clash(d.data(), d.size(), ce, &detail);
        if (e == ErrorCode::Ok) print_clash(ce);
    } else {
        std::printf("    not decoded, %zu byte unpacked\n", d.size());
        return 0;
    }
    if (e == ErrorCode::Ok) return 0;
    std::printf("    %s %s: %s\n", e == ErrorCode::Unsupported ? "not decoded," : "!!",
                error_message(e), detail.c_str());
    return e == ErrorCode::Unsupported ? 0 : 1;
}

int dump_elements(const MappedFile& file, const Container& c) {
    int failures = 0;
    for (const DbCacheEntry& p : c.entries) {
        const std::string_view leaf = p.leaf();
        if (read_elsewhere(leaf)) continue;
        std::printf("\n%.*s:\n", static_cast<int>(leaf.size()), leaf.data());
        if (p.flags != 1) {
            if (p.offset <= file.size() && p.length <= file.size() - p.offset) {
                std::printf("    not decoded, raw, %" PRIu64 " byte\n", p.length);
                hex_dump(file.data() + p.offset, static_cast<size_t>(p.length), 64);
            } else {
                std::printf("    !! %s\n", error_message(ErrorCode::OutOfRange));
                ++failures;
            }
            continue;
        }
        if (p.a != 0) {
            std::printf("    not decoded, form (%" PRId32 ", %" PRId32 "), %" PRIu64 " byte\n", p.a,
                        p.b, p.length);
            continue;
        }
        StreamSpan span;
        std::vector<uint8_t> d;
        ErrorCode ec = last_stream(p, file.size(), span);
        if (ec == ErrorCode::Ok)
            ec = inflate_span(file.data(), file.size(), span, kDefaultMaxStreamBytes, d);
        if (ec != ErrorCode::Ok) {
            std::printf("    !! does not unpack: %s\n", error_message(ec));
            ++failures;
            continue;
        }
        failures += dump_element(leaf, d);
    }
    return failures ? 1 : 0;
}

} // namespace

int tool_run(const std::vector<std::string>& args) {
    std::string path;
    size_t hex = 64;
    bool partition = false, spatial = false, units = false, elements = false;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--partition") {
            partition = true;
        } else if (args[i] == "--spatial") {
            spatial = true;
        } else if (args[i] == "--units") {
            units = true;
        } else if (args[i] == "--elements") {
            elements = true;
        } else if (args[i] == "--hex" && i + 1 < args.size()) {
            hex = static_cast<size_t>(std::strtoull(args[++i].c_str(), nullptr, 10));
        } else if (path.empty()) {
            path = args[i];
        } else {
            path.clear();
            break;
        }
    }
    if (path.empty()) {
        std::fprintf(stderr, "usage: nwddump <file> [--hex N] [--partition] [--spatial] [--units] [--elements]\n");
        return 2;
    }

    MappedFile file;
    if (const ErrorCode e = file.open(path.c_str()); e != ErrorCode::Ok) {
        std::fprintf(stderr, "error: %s\n", error_message(e));
        return 1;
    }
    std::printf("file: %zu byte\n", file.size());

    Container c;
    std::string detail;
    const ErrorCode e = read_container(file.data(), file.size(), c, &detail);
    if (e == ErrorCode::NotLcUStream) {
        std::fprintf(stderr, "error: %s\n", error_message(e));
        return 1;
    }
    print_header("section 0", c.data_section);
    if (e == ErrorCode::NoDirectory) {
        std::fprintf(stderr, "error: %s\n", error_message(e));
        return 1;
    }
    print_header("directory section", c.directory_section);
    std::printf("    directory stream @%" PRIu64 ", %" PRIu64 " byte compressed, "
                "%" PRIu64 " byte after it to the end of the file\n",
                c.directory_offset, c.directory_length,
                static_cast<uint64_t>(file.size()) - c.directory_offset -
                    c.directory_length);
    if (e != ErrorCode::Ok)
        std::printf("    !! %s: %s\n", error_message(e), detail.c_str());

    if (partition && e == ErrorCode::Ok) return dump_partition(file, c);
    if ((spatial || units || elements) && e == ErrorCode::Ok) {
        int result = 0;
        if (spatial && dump_spatial(file, c) != 0) result = 1;
        if (units && dump_units(file, c) != 0) result = 1;
        if (elements && dump_elements(file, c) != 0) result = 1;
        return result;
    }

    std::printf("\n%zu packages:\n", c.entries.size());
    std::printf("  %3s %12s %12s %5s %3s %10s  %s\n", "#", "offset", "length", "flags",
                "a", "b", "name");
    for (size_t i = 0; i < c.entries.size(); ++i) {
        const DbCacheEntry& p = c.entries[i];
        std::printf("  %3zu %12" PRIu64 " %12" PRIu64 " %5" PRId32 " %3" PRId32
                    " %10" PRId32 "  %s\n",
                    i, p.offset, p.length, p.flags, p.a, p.b, p.name.c_str());
    }

    std::printf("\nstreams:\n");
    std::vector<uint8_t> inflated;
    int failures = 0;
    for (size_t i = 0; i < c.entries.size(); ++i) {
        const DbCacheEntry& p = c.entries[i];
        std::printf("  %3zu %s\n", i, std::string(p.leaf()).c_str());

        if (p.flags != 1) {
            // Råa byte: inget att packa upp.
            if (p.offset <= file.size() && p.length <= file.size() - p.offset) {
                std::printf("      raw, %" PRIu64 " byte\n", p.length);
                hex_dump(file.data() + p.offset, static_cast<size_t>(p.length), hex);
            } else {
                std::printf("      !! %s\n", error_message(ErrorCode::OutOfRange));
                ++failures;
            }
            continue;
        }

        StreamSpan span;
        if (const ErrorCode se = last_stream(p, file.size(), span); se != ErrorCode::Ok) {
            std::printf("      !! %s (a=%" PRId32 " b=%" PRId32 ")\n", error_message(se),
                        p.a, p.b);
            ++failures;
            continue;
        }
        if (p.a == 4) {
            ByteReader prefix(file.data() + p.offset, 4);
            int32_t v = 0;
            prefix.i32(v);
            std::printf("      prefix int32 %" PRId32 ", %" PRIu64
                        " byte of streams before the last one\n",
                        v, p.length - 4 - span.length);
        }

        size_t consumed = 0;
        const ErrorCode ie =
            inflate_zlib(file.data() + span.offset, static_cast<size_t>(span.length),
                         inflated, kDefaultMaxStreamBytes, &consumed);
        if (ie != ErrorCode::Ok) {
            std::printf("      !! %s @%" PRIu64 "\n", error_message(ie), span.offset);
            ++failures;
            continue;
        }
        std::printf("      %s stream @%" PRIu64 ": %" PRIu64 " -> %zu byte%s\n",
                    p.a == 4 ? "last" : "only", span.offset, span.length,
                    inflated.size(),
                    consumed == span.length ? "" : "  !! compressed length differs");
        if (consumed != span.length) ++failures;
        print_ints(inflated.data(), inflated.size(), 12);
        hex_dump(inflated.data(), inflated.size(), hex);
    }

    if (e != ErrorCode::Ok || failures) {
        std::printf("\n%d stream(s) failed%s\n", failures,
                    e != ErrorCode::Ok ? ", directory does not cover the data section" : "");
        return 1;
    }
    return 0;
}
