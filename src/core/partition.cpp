#include "partition.h"

namespace HNwd::detail {

namespace {

constexpr const char* kWhat = "LcOpNwdPartition";

// En markör med ett känt värde.
bool marker(ObjectReader& r, int32_t expected, const char* rule) {
    int32_t v;
    if (!r.i32(v)) return false;
    if (v != expected) return r.fail(rule);
    return true;
}

bool f64s(ObjectReader& r, double* out, int n) {
    for (int i = 0; i < n; ++i)
        if (!r.f64(out[i])) return false;
    return true;
}

// Klass 27: se PartitionLight.
bool read_light(ObjectReader& r, PartitionLight& light) {
    if (!r.i32(light.version)) return false;
    if (light.version != 1) return r.fail("class-27 version is not 1");

    ObjectReader::Ptr p;
    if (!r.ptr(p, {185}, true, true)) return false;
    if (p.kind == ObjectReader::Ptr::New) {
        if (!skip_class185(r) || !r.finish(p.id, 0)) return false;
    }
    if (!marker(r, 0, "class-27 marker after the material is not 0")) return false;
    return f64s(r, light.values, 12);
}

} // namespace

bool metres_per_unit(int32_t unit_code, double& out) {
    switch (unit_code) {
    case 0: out = 1.0; return true;
    case 1: out = 0.01; return true;
    case 2: out = 0.001; return true;
    case 3: out = 0.3048; return true;
    default: return false;
    }
}

bool read_partition_tail(ObjectReader& r, std::vector<PartitionLight>& lights,
                         PartitionTail& out) {
    out = PartitionTail{};
    if (!marker(r, 2, "partition tail does not start with 2")) return false;

    int32_t n;
    // En pekare tar minst [1][id].
    if (!r.count(8, n)) return false;
    // Genom ett fönster är antalet bara prövat mot strömmens tak: listan växer
    // med pekarna som läses i stället för att reserveras efter antalet.
    for (int32_t i = 0; i < n; ++i) {
        ObjectReader::Ptr p;
        if (!r.ptr(p, {27}, false, true)) return false;
        if (p.kind == ObjectReader::Ptr::New) {
            PartitionLight light;
            if (!read_light(r, light)) return false;
            p.value = static_cast<uint32_t>(lights.size());
            if (!r.finish(p.id, p.value)) return false;
            lights.push_back(light);
        }
        out.light_refs.push_back(p.value);
    }

    if (!f64s(r, out.colour, 3)) return false;
    if (!marker(r, 0, "partition tail marker before the loader is not 0")) return false;
    if (!r.str(out.loader)) return false;
    if (!r.i32(out.format_version) || !r.i32(out.unit_code)) return false;
    if (!marker(r, 1, "partition tail marker after the unit code is not 1")) return false;
    if (!f64s(r, out.a, 3) || !f64s(r, out.b, 3)) return false;
    // Ett tal, 0 eller 1, och en text som får saknas; sedan 1 och källsökvägen.
    int32_t has_label;
    if (!r.i32(has_label)) return false;
    if (has_label != 0 && has_label != 1)
        return r.fail("partition tail marker before the label is not 0 or 1");
    if (!r.str(out.label)) return false;
    if (!marker(r, 1, "partition tail marker before the source path is not 1")) return false;
    if (!r.str(out.source_path)) return false;
    if (!f64s(r, out.c, 3) || !f64s(r, out.d, 4)) return false;

    // Svansen slutar på [i32 1] och nollor upp till en multipel av 8.
    if (!marker(r, 1, "partition tail does not end with 1")) return false;
    if (r.pos() % 8 != 0 && !marker(r, 0, "partition tail pad is not zero")) return false;
    return true;
}

namespace {

// Läser ett standalone-objekt av klass 32. `unsupported` får texten om felet är
// en klass eller en plats som inte läses här.
bool read_stream(ObjectReader& r, Partition& out, std::string& unsupported) {
    ObjectReader::Ptr p;
    if (!r.ptr(p, {32}, false, false)) return false;
    if (!marker(r, 0, "class-32 body does not start with 0")) return false;

    if (!r.str(out.name)) return false;

    if (!r.ptr(p, {52}, true, false)) return false;
    if (p.kind == ObjectReader::Ptr::New) {
        if (!r.str(out.user_display) || !r.str(out.user_internal) || !r.finish(p.id, 0))
            return false;
        out.has_user_name = true;
    }

    int32_t attributes;
    if (!r.i32(out.flags) || !r.i32(out.element)) return false;
    if (out.element < 0) return r.fail("element index is negative");
    // Ett attribut tar minst en nollpekare.
    if (!r.count(4, attributes)) return false;
    for (int32_t i = 0; i < attributes; ++i) {
        const size_t at = r.pos();
        if (!r.ptr(p, {86, 31, 40, 41, 39, 79, 200}, true, false)) return false;
        if (p.kind == ObjectReader::Ptr::Null || p.cls != 31) {
            unsupported = std::string(kWhat) + " at byte " + std::to_string(at) +
                          (p.kind == ObjectReader::Ptr::Null
                               ? ": null attribute slot needs a property root, and the stream has none"
                               : ": attribute class is not read in a standalone partition");
            return false;
        }
        std::string name;
        double d;
        if (!r.str(name)) return false;
        for (int k = 0; k < 15; ++k)
            if (!r.f64(d)) return false;
        if (!r.finish(p.id, 0)) return false;
    }
    out.attribute_count = static_cast<uint32_t>(attributes);

    if (!read_partition_tail(r, out.lights, out.tail)) return false;
    if (!r.finish(100, 0)) return false;
    return r.end();
}

} // namespace

ErrorCode parse_partition_stream(const uint8_t* data, size_t size, Partition& out,
                                 std::string* detail) {
    out = Partition{};
    if (detail) detail->clear();

    ObjectReader r(data, size, kWhat);
    std::string unsupported;
    if (read_stream(r, out, unsupported)) return ErrorCode::Ok;

    ErrorCode code;
    if (!unsupported.empty()) {
        code = ErrorCode::Unsupported;
        if (detail) *detail = unsupported;
    } else {
        code = r.ok() ? ErrorCode::CorruptObject : r.code();
        if (detail) *detail = r.detail();
    }
    out = Partition{};
    return code;
}

} // namespace HNwd::detail
