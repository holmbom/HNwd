// Document: allt utom trädet och det rumsliga indexet. Trädets funktioner
// ligger i tree.cpp, indexet i spatial_index.cpp och läsaren per tråd i
// geometry_reader.cpp.

#include "document_impl.h"

#include <new>

#include "inflate.h"
#include "partition.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <utility>

namespace HNwd {

// ---------------------------------------------------------------------------
// Status.

const char* status_text(Status status) {
    switch (status) {
    case Status::Ok: return "ok";
    case Status::FileNotFound: return "the file was not found";
    case Status::FileOpenFailed: return "the file could not be opened";
    case Status::MmapFailed: return "the file could not be mapped into memory";
    case Status::NotNavisworks: return "the file is not in the Navisworks format";
    case Status::NoDirectory: return "the file has no directory over its packages";
    case Status::Corrupt: return "the file breaks the format";
    case Status::Unsupported: return "valid, but not decoded by this library";
    case Status::LimitExceeded: return "a limit in Options was reached";
    case Status::Missing: return "the file does not have the package the call needs";
    case Status::NotLoaded: return "the call needs a load that has not been done";
    case Status::Cancelled: return "the caller cancelled through the progress function";
    case Status::OutOfRange: return "an index outside what exists";
    }
    return "unknown status";
}

Status to_status(detail::ErrorCode code) {
    using detail::ErrorCode;
    switch (code) {
    case ErrorCode::Ok: return Status::Ok;
    case ErrorCode::FileNotFound: return Status::FileNotFound;
    case ErrorCode::FileOpenFailed: return Status::FileOpenFailed;
    case ErrorCode::MmapFailed: return Status::MmapFailed;
    case ErrorCode::NotLcUStream:
    case ErrorCode::CorruptHeader: return Status::NotNavisworks;
    case ErrorCode::NoDirectory: return Status::NoDirectory;
    case ErrorCode::CorruptDirectory:
    case ErrorCode::CorruptStream:
    case ErrorCode::CorruptObject:
    case ErrorCode::CorruptPackage:
    case ErrorCode::CorruptGeometry:
    case ErrorCode::CorruptLink:
    case ErrorCode::OutOfRange: return Status::Corrupt;
    case ErrorCode::StreamTooLarge:
    case ErrorCode::LimitExceeded: return Status::LimitExceeded;
    case ErrorCode::Unsupported: return Status::Unsupported;
    case ErrorCode::PackageMissing: return Status::Missing;
    case ErrorCode::NotLoaded: return Status::NotLoaded;
    case ErrorCode::Cancelled: return Status::Cancelled;
    case ErrorCode::kCount: break;
    }
    return Status::Corrupt;
}

Status Document::Impl::fail(detail::ErrorCode code, const std::string& text) {
    last_error = text.empty() ? detail::error_message(code) : text;
    return to_status(code);
}

Status Document::Impl::fail(Status status, const char* text) {
    last_error = (text && *text) ? text : status_text(status);
    return status;
}

Status Document::Impl::ok() {
    last_error.clear();
    return Status::Ok;
}

// Varje fragments seq ska vara en plats i trädet med ett löv (klass 22).
Status Document::Impl::check_seq_leaves(const detail::FragmentStore& store,
                                         const TreeData& tree_data) {
    const Occurrences& occurrences = tree_data.occurrences;
    const detail::Hierarchy& hierarchy = tree_data.hierarchy;
    const std::vector<detail::FragmentStore::Item>& items = store.items;
    for (size_t i = 0; i < items.size(); ++i) {
        const uint32_t seq = static_cast<uint32_t>(items[i].seq);   // aldrig negativ efter load_fragments()
        if (seq >= occurrences.size())
            return fail(detail::ErrorCode::CorruptLink,
                        "fragment " + std::to_string(i) + ": seq " + std::to_string(seq) +
                            " is outside the tree of " + std::to_string(occurrences.size()) +
                            " nodes");
        const uint32_t stored = occurrences.stored(seq);
        if (stored == kNone || stored >= hierarchy.nodes.size() ||
            hierarchy.nodes[stored].cls != 22)
            return fail(detail::ErrorCode::CorruptLink,
                        "fragment " + std::to_string(i) + ": seq " + std::to_string(seq) +
                            " is not a leaf (class 22) in the tree");
    }
    return ok();
}

// ---------------------------------------------------------------------------
// Hjälp.

std::string at_byte(const detail::DbCacheEntry& e) {
    return std::string(e.leaf()) + " at byte " + std::to_string(e.offset);
}

namespace {

// Paketets form ska vara (0, 0): flags 1 och en zlib-ström på hela längden.
// Ger strömmens plats i filen.
detail::ErrorCode whole_stream(const detail::MappedFile& file, const detail::DbCacheEntry& e,
                               detail::StreamSpan& span, std::string& why) {
    using detail::ErrorCode;
    if (e.flags != 1 || e.a != 0 || e.b != 0) {
        why = at_byte(e) + ": expected flags 1 and form (0, 0), found flags " +
              std::to_string(e.flags) + " and form (" + std::to_string(e.a) + ", " +
              std::to_string(e.b) + ")";
        return ErrorCode::CorruptPackage;
    }
    const ErrorCode ec = detail::last_stream(e, file.size(), span);
    if (ec != ErrorCode::Ok) {
        why = at_byte(e) + ": the package lies outside the file";
        return ec;
    }
    if (span.offset > file.size() || span.length > file.size() - span.offset) {
        why = at_byte(e) + ": the package lies outside the file";
        return ErrorCode::OutOfRange;
    }
    return ErrorCode::Ok;
}

} // namespace

// Packar upp ett paket av formen (0, 0): flags 1 och en zlib-ström på hela
// längden. `why` får texten vid fel.
detail::ErrorCode unpack_package(const detail::MappedFile& file, const detail::DbCacheEntry& e,
                                 size_t max_stream, std::vector<uint8_t>& out,
                                 std::string& why) {
    detail::StreamSpan span;
    detail::ErrorCode ec = whole_stream(file, e, span, why);
    if (ec != detail::ErrorCode::Ok) return ec;
    std::string inner;
    ec = detail::inflate_span(file.data(), file.size(), span, max_stream, out, &inner);
    if (ec != detail::ErrorCode::Ok) why = std::string(e.leaf()) + ": " + inner;
    return ec;
}

detail::PoseD to_pose_d(const Pose& pose) {
    detail::PoseD p;
    for (int k = 0; k < 3; ++k) p.t[k] = pose.t[k];
    for (int k = 0; k < 4; ++k) p.q[k] = pose.q[k];
    p.s = pose.s;
    p.affine = pose.affine;
    for (int k = 0; k < 9; ++k) p.m[k] = pose.m[k];
    return p;
}

Pose to_pose(const detail::PoseD& pose) {
    Pose p;
    for (int k = 0; k < 3; ++k) p.t[k] = pose.t[k];
    for (int k = 0; k < 4; ++k) p.q[k] = pose.q[k];
    p.s = pose.s;
    p.affine = pose.affine;
    for (int k = 0; k < 9; ++k) p.m[k] = pose.m[k];
    return p;
}

void to_mesh(const detail::RecordData& rd, Mesh& out) {
    out.kind = rd.text     ? MeshKind::Text
               : rd.points ? MeshKind::Points
               : rd.lines  ? MeshKind::Lines
                           : MeshKind::Triangles;
    out.text = rd.text_utf8;
    out.text_angle = rd.text_angle;
    for (int i = 0; i < 3; ++i) out.text_axis[i] = rd.text_axis[i];
    out.text_font = rd.text_font;
    out.positions.assign(rd.positions.begin(), rd.positions.end());
    out.normals.assign(rd.normals.begin(), rd.normals.end());
    out.uvs.assign(rd.uvs.begin(), rd.uvs.end());
    out.colors.assign(rd.colors.begin(), rd.colors.end());
    out.indices.assign(rd.indices.begin(), rd.indices.end());
    for (int i = 0; i < 3; ++i) {
        out.bounds.min[i] = rd.bbox[i];
        out.bounds.max[i] = rd.bbox[3 + i];
    }
}

namespace {

// Ger Impl för ett Document, och skapar den om dokumentet har flyttats ifrån.
template <class T>
T& ensure(std::unique_ptr<T>& p) {
    if (!p) p = std::make_unique<T>();
    return *p;
}

} // namespace

// ---------------------------------------------------------------------------
// Livscykel.

Document::Document() : impl_(std::make_unique<Impl>()) {}
Document::~Document() = default;
Document::Document(Document&&) noexcept = default;
Document& Document::operator=(Document&&) noexcept = default;

void Document::close() {
    if (impl_) *impl_ = Impl{};
}

bool Document::is_open() const { return impl_ && impl_->is_open(); }

Status Document::open(const char* path, const Options& options) try {
    Impl& s = ensure(impl_);
    s = Impl{};
    if (options.normal_bits != 0 && options.normal_bits != 8 && options.normal_bits != 16)
        return s.fail(Status::OutOfRange, "Options::normal_bits is not 0, 8 or 16");
    s.options = options;

    if (!path || !*path) return s.fail(Status::FileNotFound, "the path is empty");
    auto file = std::make_shared<detail::MappedFile>();
    detail::ErrorCode ec = file->open(path);
    if (ec != detail::ErrorCode::Ok) {
        const std::string message = std::string(detail::error_message(ec)) + ": " + path;
        s = Impl{};
        return s.fail(ec, message);
    }
    s.file = std::move(file);

    std::string why;
    ec = detail::read_container(s.file->data(), s.file->size(), s.container, &why);
    if (ec != detail::ErrorCode::Ok) {
        s = Impl{};
        return s.fail(ec, why);
    }

    // Utan "double" i rubriken är flyttalen i poser och matriser 32 bitar,
    // och det avkodas inte.
    if (!s.container.data_section.is_double || !s.container.directory_section.is_double) {
        s = Impl{};
        return s.fail(Status::Unsupported,
                      "the file stores 32-bit floating point values, which is not decoded");
    }

    // Paketen läsaren använder. Ett namn som förekommer två gånger går inte att
    // välja mellan.
    struct Slot {
        const char* leaf;
        const detail::DbCacheEntry** entry;
    };
    const Slot slots[] = {
        {"LcOpNwdLogicalHierarchy", &s.hierarchy_entry},
        {"LcOpNwdFragments", &s.fragments_entry},
        {"LcOpNwdGeometry", &s.geometry_entry},
        {"LcOpGuidStore", &s.guids_entry},
        {"LcOaPartitionProps", &s.props_entry},
        {"LcOpNwdPartition", &s.partition_entry},
        {"LcOpXRefTable", &s.xref_entry},
        {"LcOpDBCache", &s.source_entry},
        {"LcOpNwdSpatialHierarchy", &s.spatial_entry},
    };
    for (const Slot& slot : slots) {
        int count = 0;
        for (const detail::DbCacheEntry& e : s.container.entries) {
            if (e.leaf() != slot.leaf) continue;
            if (!*slot.entry) *slot.entry = &e;
            ++count;
        }
        if (count > 1) {
            const std::string message =
                std::string("the directory has ") + std::to_string(count) + " packages named " +
                slot.leaf + "; the package to read cannot be chosen";
            s = Impl{};
            return s.fail(detail::ErrorCode::CorruptDirectory, message);
        }
    }
    return s.ok();
} catch (const std::bad_alloc&) {
    return out_of_memory(ensure(impl_));
}

// ---------------------------------------------------------------------------
// Katalogen.

size_t Document::entry_count() const { return is_open() ? impl_->container.entries.size() : 0; }

bool Document::entry(size_t i, DirectoryEntry& out) const {
    if (!is_open() || i >= impl_->container.entries.size()) return false;
    const detail::DbCacheEntry& e = impl_->container.entries[i];
    out.name = e.name;
    out.leaf = e.leaf();
    out.flags = e.flags;
    out.offset = e.offset;
    out.length = e.length;
    out.a = e.a;
    out.b = e.b;
    return true;
}

bool Document::has(Package package) const {
    if (!is_open()) return false;
    const Impl& s = *impl_;
    switch (package) {
    case Package::Hierarchy: return s.hierarchy_entry != nullptr;
    case Package::Fragments: return s.fragments_entry != nullptr;
    case Package::Geometry: return s.geometry_entry != nullptr;
    case Package::GuidStore: return s.guids_entry != nullptr;
    case Package::PartitionProps: return s.props_entry != nullptr;
    case Package::Partition: return s.partition_entry != nullptr;
    case Package::XRefTable: return s.xref_entry != nullptr;
    case Package::SourceHeader: return s.source_entry != nullptr;
    }
    return false;
}

int Document::version() const {
    return is_open() ? impl_->container.directory_section.version : -1;
}

const char* Document::last_error() const { return impl_ ? impl_->last_error.c_str() : ""; }

// ---------------------------------------------------------------------------
// Geometrin.

Status Document::load_geometry(const Progress& progress) try {
    Impl& s = ensure(impl_);
    if (!is_open()) return s.fail(Status::NotLoaded, "no file is open");
    if (s.geometry) return s.ok();
    if (!s.geometry_entry)
        return s.fail(detail::ErrorCode::PackageMissing,
                      "the file has no LcOpNwdGeometry package");

    const detail::DbCacheEntry& e = *s.geometry_entry;
    if (e.flags != 1 || e.a != 4)
        return s.fail(detail::ErrorCode::CorruptPackage,
                      at_byte(e) + ": expected flags 1 and form (4, b), found flags " +
                          std::to_string(e.flags) + " and form (" + std::to_string(e.a) + ", " +
                          std::to_string(e.b) + ")");

    detail::SegmentLimits limits;
    limits.max_stream = s.options.max_stream_bytes;
    limits.cache_bytes = s.options.chunk_cache_bytes;
    limits.max_record = s.options.max_record_bytes;
    limits.normal_bits = s.options.normal_bits;
    limits.layout = s.options.measure_chunks ? detail::ChunkLayout::Scan : detail::ChunkLayout::Fixed;

    auto layout = std::make_shared<detail::GeometryLayout>();
    std::string why;
    detail::ErrorCode ec = layout->open(s.file->data(), s.file->size(), e, limits, &why);
    if (ec != detail::ErrorCode::Ok) return s.fail(ec, why);
    ec = layout->decide_normal_bits(progress, &why);
    if (ec != detail::ErrorCode::Ok) return s.fail(ec, why);

    s.cursor = detail::GeometryCursor(*layout, s.file->data(), s.file->size(),
                                      s.options.chunk_cache_bytes);
    s.geometry = std::move(layout);
    return s.ok();
} catch (const std::bad_alloc&) {
    return out_of_memory(ensure(impl_));
}

uint32_t Document::record_count() const {
    return impl_ && impl_->geometry ? impl_->geometry->record_count() : 0;
}

int Document::normal_bits() const {
    return impl_ && impl_->geometry ? impl_->geometry->normal_bits() : 0;
}

Status Document::record_mesh(uint32_t record, Mesh& out) try {
    Impl& s = ensure(impl_);
    if (!is_open() || !s.geometry)
        return s.fail(Status::NotLoaded, "the geometry has not been loaded: call load_geometry()");
    if (record < 1 || record > s.geometry->record_count())
        return s.fail(Status::OutOfRange,
                      ("record " + std::to_string(record) + " is outside 1.." +
                       std::to_string(s.geometry->record_count()))
                          .c_str());

    std::string why;
    const detail::ErrorCode ec = s.cursor.parse(record - 1, s.record, &why);
    if (ec != detail::ErrorCode::Ok) return s.fail(ec, why);
    to_mesh(s.record, out);
    return s.ok();
} catch (const std::bad_alloc&) {
    return out_of_memory(ensure(impl_));
}

// ---------------------------------------------------------------------------
// Placeringen.

Status Document::load_fragments(const Progress& progress) try {
    Impl& s = ensure(impl_);
    if (!is_open()) return s.fail(Status::NotLoaded, "no file is open");
    if (s.fragments) return s.ok();
    if (!s.fragments_entry)
        return s.fail(detail::ErrorCode::PackageMissing,
                      "the file has no LcOpNwdFragments package");
    if (!s.geometry_entry)
        return s.fail(detail::ErrorCode::PackageMissing,
                      "the fragments need the LcOpNwdGeometry package, which the file does not "
                      "have");
    if (const Status g = load_geometry(progress); g != Status::Ok) return g;

    auto store = std::make_shared<detail::FragmentStore>();
    {
        std::string why;
        detail::StreamSpan span;
        detail::ErrorCode ec = whole_stream(*s.file, *s.fragments_entry, span, why);
        if (ec != detail::ErrorCode::Ok) return s.fail(ec, why);

        detail::InflateStream stream;
        ec = stream.open(s.file->data() + span.offset, static_cast<size_t>(span.length),
                         s.options.max_stream_bytes);
        if (ec != detail::ErrorCode::Ok)
            return s.fail(ec, at_byte(*s.fragments_entry) + ": " + detail::error_message(ec));
        ec = detail::parse_fragments(stream, *store, progress, nullptr, &why,
                                     detail::kStreamWindowBytes,
                                     s.options.max_material_json_bytes);
        if (ec != detail::ErrorCode::Ok) return s.fail(ec, why);
        if (!stream.finished() || stream.consumed() != span.length)
            return s.fail(detail::ErrorCode::CorruptStream,
                          at_byte(*s.fragments_entry) + ": the zlib stream ends at " +
                              std::to_string(stream.consumed()) + " of " +
                              std::to_string(span.length) + " bytes");
    }   // strömmen och dess fönster släpps här

    const uint32_t records = s.geometry->record_count();
    const std::vector<detail::FragmentStore::Item>& items = store->items;
    int64_t previous_seq = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        const detail::FragmentStore::Item& it = items[i];
        if (it.seq < 0 || it.seq < previous_seq || it.primitives < 0 ||
            store->geomrefs[it.geomref].record < 1 ||
            static_cast<uint32_t>(store->geomrefs[it.geomref].record) > records) {
            const std::string prefix = "LcOpNwdFragments: fragment " + std::to_string(i) + ": ";
            const int32_t record = store->geomrefs[it.geomref].record;
            if (it.seq < 0)
                return s.fail(detail::ErrorCode::CorruptLink, prefix + "seq is negative");
            if (it.seq < previous_seq)
                return s.fail(detail::ErrorCode::CorruptLink,
                              prefix + "seq " + std::to_string(it.seq) +
                                  " is lower than the previous fragment's seq " +
                                  std::to_string(previous_seq));
            if (it.primitives < 0)
                return s.fail(detail::ErrorCode::CorruptLink,
                              prefix + "the primitive count is negative");
            return s.fail(detail::ErrorCode::CorruptLink,
                          prefix + "record " + std::to_string(record) + " is outside 1.." +
                              std::to_string(records));
        }
        previous_seq = it.seq;
    }
    for (size_t i = 0; i < store->looks.size(); ++i) {
        const uint32_t m = store->looks[i].material;
        if (m != detail::kNoMaterial && m >= store->materials.size())
            return s.fail(detail::ErrorCode::CorruptLink,
                          "LcOpNwdFragments: look " + std::to_string(i) + ": material " +
                              std::to_string(m) + " is outside the " +
                              std::to_string(store->materials.size()) + " materials");
    }

    // Är trädet redan läst är det här fragmenten som kommer sist.
    if (s.tree)
        if (const Status t = s.check_seq_leaves(*store, *s.tree); t != Status::Ok) return t;

    s.fragments = std::move(store);
    return s.ok();
} catch (const std::bad_alloc&) {
    return out_of_memory(ensure(impl_));
}

size_t Document::fragment_count() const {
    return impl_ && impl_->fragments ? impl_->fragments->size() : 0;
}

bool Document::fragment(size_t i, Fragment& out) const {
    if (!impl_ || !impl_->fragments || i >= impl_->fragments->size()) return false;
    const detail::FragmentStore& st = *impl_->fragments;
    const detail::FragmentStore::Item& it = st.items[i];
    const detail::GeometryRefData& g = st.geomrefs[it.geomref];
    Fragment f;
    f.record = static_cast<uint32_t>(g.record);
    f.seq = static_cast<uint32_t>(it.seq);
    f.primitives = static_cast<uint32_t>(it.primitives);
    f.look = it.look;
    detail::PoseD pose;
    st.pose(i, pose);
    f.pose = to_pose(pose);
    for (int k = 0; k < 3; ++k) {
        f.bounds.min[k] = g.bbox[k];
        f.bounds.max[k] = g.bbox[3 + k];
    }
    out = f;
    return true;
}

size_t Document::look_count() const {
    return impl_ && impl_->fragments ? impl_->fragments->looks.size() : 0;
}

bool Document::look(size_t i, Look& out) const {
    if (!impl_ || !impl_->fragments || i >= impl_->fragments->looks.size()) return false;
    const detail::LookData& ld = impl_->fragments->looks[i];
    Look l;
    for (int k = 0; k < 3; ++k) {
        l.ambient[k] = ld.v[k];
        l.diffuse[k] = ld.v[3 + k];
        l.specular[k] = ld.v[6 + k];
        l.emissive[k] = ld.v[9 + k];
    }
    l.shininess = ld.v[12];
    l.transparency = ld.v[13];
    l.material = ld.material == detail::kNoMaterial ? kNone : ld.material;
    out = l;
    return true;
}

size_t Document::material_count() const {
    return impl_ && impl_->fragments ? impl_->fragments->materials.size() : 0;
}

bool Document::material(size_t i, Material& out) const {
    if (!impl_ || !impl_->fragments || i >= impl_->fragments->materials.size()) return false;
    to_material(impl_->fragments->materials[i], out);
    return true;
}

void to_material(const detail::MaterialData& m, Material& out) {
    const detail::MaterialTexture& t = m.texture;
    out = Material{};
    out.json = m.json;
    out.has_color = t.has_color;
    for (int k = 0; k < 4; ++k) out.base_color[k] = t.color[k];
    out.has_texture = !t.bitmap.empty();
    out.texture_name = t.bitmap;
    if (t.file != detail::kNoMaterial && t.file < m.files.size()) {
        const detail::MaterialFile& f = m.files[t.file];
        if (f.embedded) {
            out.texture_type = f.type;
            out.texture_bytes = f.bytes.empty() ? nullptr : f.bytes.data();
            out.texture_size = f.bytes.size();
        }
    }
    out.u_scale = t.u_scale;
    out.v_scale = t.v_scale;
    out.u_offset = t.u_offset;
    out.v_offset = t.v_offset;
    out.w_angle = t.w_angle;
    out.real_scale_x = t.real_scale_x;
    out.real_scale_y = t.real_scale_y;
    out.real_units_x = t.real_units_x;
    out.real_units_y = t.real_units_y;
    out.u_repeat = t.u_repeat;
    out.v_repeat = t.v_repeat;
}

bool Document::fragment_world_box(size_t i, BoxD& out) const {
    if (!impl_ || !impl_->fragments || i >= impl_->fragments->size()) return false;
    const detail::FragmentStore& st = *impl_->fragments;
    detail::PoseD pose;
    st.pose(i, pose);
    BoxD box;
    detail::pose_box(pose, st.geomrefs[st.items[i].geomref].bbox, box.min, box.max);
    out = box;
    return true;
}

Status Document::fragment_mesh(size_t fragment, Mesh& out) try {
    Impl& s = ensure(impl_);
    if (!is_open() || !s.fragments)
        return s.fail(Status::NotLoaded,
                      "the fragments have not been loaded: call load_fragments()");
    if (fragment >= s.fragments->size())
        return s.fail(Status::OutOfRange,
                      ("fragment " + std::to_string(fragment) + " is outside 0.." +
                       std::to_string(s.fragments->size()))
                          .c_str());

    std::string why;
    const detail::ErrorCode ec =
        read_fragment_mesh(*s.fragments, fragment, s.cursor, s.record, out, why);
    if (ec != detail::ErrorCode::Ok) return s.fail(ec, why);
    return s.ok();
} catch (const std::bad_alloc&) {
    return out_of_memory(ensure(impl_));
}

// ---------------------------------------------------------------------------
// Enheten.

Unit to_unit(int32_t code) {
    Unit out;
    out.code = code;
    LengthUnit unit = LengthUnit::Unknown;
    switch (code) {
    case 0: unit = LengthUnit::Metre; break;
    case 1: unit = LengthUnit::Centimetre; break;
    case 2: unit = LengthUnit::Millimetre; break;
    case 3: unit = LengthUnit::Foot; break;
    default: break;
    }
    double metres = 0;
    if (unit != LengthUnit::Unknown && detail::metres_per_unit(code, metres)) {
        out.unit = unit;
        out.metres_per_unit = metres;
    }
    return out;
}

Status Document::length_unit(Unit& out) try {
    Impl& s = ensure(impl_);
    out = Unit{};
    if (!is_open()) return s.fail(Status::NotLoaded, "no file is open");
    if (!s.partition_entry)
        return s.fail(detail::ErrorCode::PackageMissing,
                      "the file has no LcOpNwdPartition package");

    std::vector<uint8_t> bytes;
    std::string why;
    detail::ErrorCode ec =
        unpack_package(*s.file, *s.partition_entry, s.options.max_stream_bytes, bytes, why);
    if (ec != detail::ErrorCode::Ok) return s.fail(ec, why);
    detail::Partition partition;
    ec = detail::parse_partition_stream(bytes.data(), bytes.size(), partition, &why);
    if (ec != detail::ErrorCode::Ok) return s.fail(ec, why);

    const int32_t code = partition.tail.unit_code;
    const Unit unit = to_unit(code);
    out.code = code;
    if (unit.unit == LengthUnit::Unknown)
        return s.fail(Status::Unsupported,
                      ("LcOpNwdPartition: the length unit code " + std::to_string(code) +
                       " is not known")
                          .c_str());
    out = unit;
    return s.ok();
} catch (const std::bad_alloc&) {
    return out_of_memory(ensure(impl_));
}

// ---------------------------------------------------------------------------
// Minnet.

size_t Document::memory_bytes() const {
    if (!impl_) return 0;
    const Impl& s = *impl_;
    size_t n = sizeof(Document) + sizeof(Impl) + s.last_error.capacity();
    n += s.container.entries.capacity() * sizeof(detail::DbCacheEntry);
    for (const detail::DbCacheEntry& e : s.container.entries) n += e.name.capacity();
    if (s.geometry) {
        n += s.geometry->memory_bytes();
        // Cursorns uppackade chunkar och dess plats per chunk.
        n += s.cursor.cached_bytes() + size_t(s.geometry->chunk_count()) * sizeof(uint32_t);
    }
    n += (s.record.positions.capacity() + s.record.normals.capacity() + s.record.uvs.capacity()) *
             sizeof(float) +
         (s.record.colors.capacity() + s.record.indices.capacity()) * sizeof(uint32_t) +
         s.record.text_utf8.capacity();
    if (s.fragments) n += s.fragments->memory_bytes();
    for (const std::shared_ptr<const SpatialIndex>& x : s.spatial_built)
        if (x) n += x->memory_bytes();
    n += s.guids.capacity() * sizeof(detail::Guid16);
    if (s.tree) n += s.tree->memory_bytes();
    for (const std::shared_ptr<const TreeData>& t : s.retired_trees) n += t->memory_bytes();
    n += s.retired_trees.capacity() * sizeof(std::shared_ptr<const TreeData>);
    n += s.elements_memory_bytes();
    return n;
}

// ---------------------------------------------------------------------------
// GUID-tabellen, källan och XRef-tabellen.

Status Document::load_guids() try {
    Impl& s = ensure(impl_);
    if (!is_open()) return s.fail(Status::NotLoaded, "no file is open");
    if (s.guids_ready.load()) return s.ok();
    if (!s.guids_entry)
        return s.fail(detail::ErrorCode::PackageMissing, "the file has no LcOpGuidStore package");

    std::vector<uint8_t> bytes;
    std::string why;
    detail::ErrorCode ec =
        unpack_package(*s.file, *s.guids_entry, s.options.max_stream_bytes, bytes, why);
    if (ec != detail::ErrorCode::Ok) return s.fail(ec, why);

    std::vector<detail::Guid16> guids;
    ec = detail::parse_guid_store(bytes.data(), bytes.size(), guids, &why);
    if (ec != detail::ErrorCode::Ok) return s.fail(ec, why);

    // Tabellen fylls först och publiceras sedan: en annan tråd som ser
    // guids_ready ser hela tabellen.
    s.guids = std::move(guids);
    s.guids_ready.store(true);
    return s.ok();
} catch (const std::bad_alloc&) {
    return out_of_memory(ensure(impl_));
}

size_t Document::guid_count() const {
    return impl_ && impl_->guids_ready.load() ? impl_->guids.size() : 0;
}

bool Document::guid(size_t i, uint8_t out[16]) const {
    if (!impl_ || !impl_->guids_ready.load() || i >= impl_->guids.size()) return false;
    const detail::Guid16& g = impl_->guids[i];
    for (size_t k = 0; k < 16; ++k) out[k] = g[k];
    return true;
}

Status Document::file_source(Source& out) try {
    Impl& s = ensure(impl_);
    if (!is_open()) return s.fail(Status::NotLoaded, "no file is open");
    if (!s.source_entry)
        return s.fail(detail::ErrorCode::PackageMissing, "the file has no LcOpDBCache package");

    if (!s.source_loaded) {
        std::vector<uint8_t> bytes;
        std::string why;
        detail::ErrorCode ec =
            unpack_package(*s.file, *s.source_entry, s.options.max_stream_bytes, bytes, why);
        if (ec != detail::ErrorCode::Ok) return s.fail(ec, why);

        detail::SourceHeader header;
        ec = detail::parse_source_header(bytes.data(), bytes.size(), header, &why);
        if (ec != detail::ErrorCode::Ok) return s.fail(ec, why);
        s.source = std::move(header);
        s.source_loaded = true;
    }

    out.path = s.source.path;
    out.loader = s.source.loader;
    out.unix_time = s.source.unix_time;
    out.size = s.source.source_size;
    return s.ok();
} catch (const std::bad_alloc&) {
    return out_of_memory(ensure(impl_));
}

// [int32 längd][längd byte JSON-text] följt av nollor upp till 4 eller 8.
Status Document::xref_json(std::string& out) try {
    Impl& s = ensure(impl_);
    if (!is_open()) return s.fail(Status::NotLoaded, "no file is open");
    if (!s.xref_entry)
        return s.fail(detail::ErrorCode::PackageMissing, "the file has no LcOpXRefTable package");

    std::vector<uint8_t> bytes;
    std::string why;
    const detail::ErrorCode ec =
        unpack_package(*s.file, *s.xref_entry, s.options.max_stream_bytes, bytes, why);
    if (ec != detail::ErrorCode::Ok) return s.fail(ec, why);

    const std::string name = "LcOpXRefTable";
    if (bytes.size() < 4)
        return s.fail(detail::ErrorCode::CorruptPackage,
                      name + ": the stream is shorter than its length field");
    const uint32_t raw = static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
                         (static_cast<uint32_t>(bytes[2]) << 16) |
                         (static_cast<uint32_t>(bytes[3]) << 24);
    const int32_t length = static_cast<int32_t>(raw);
    if (length < 0 || static_cast<uint64_t>(length) > bytes.size() - 4)
        return s.fail(detail::ErrorCode::CorruptPackage,
                      name + " at byte 0: the text length " + std::to_string(length) +
                          " does not fit in the " + std::to_string(bytes.size()) +
                          " byte stream");
    const size_t end = 4 + static_cast<size_t>(length);
    if (bytes.size() - end > 7)
        return s.fail(detail::ErrorCode::CorruptPackage,
                      name + " at byte " + std::to_string(end) + ": " +
                          std::to_string(bytes.size() - end) +
                          " bytes follow the text, more than padding to 8");
    for (size_t i = end; i < bytes.size(); ++i)
        if (bytes[i] != 0)
            return s.fail(detail::ErrorCode::CorruptPackage,
                          name + " at byte " + std::to_string(i) +
                              ": the padding after the text is not zero");

    out.assign(reinterpret_cast<const char*>(bytes.data()) + 4, static_cast<size_t>(length));
    return s.ok();
} catch (const std::bad_alloc&) {
    return out_of_memory(ensure(impl_));
}

// ---------------------------------------------------------------------------
// Världen.

void world_point(const Pose& pose, const float v[3], double out[3]) {
    detail::pose_point(to_pose_d(pose), v, out);
}

void world_normal(const Pose& pose, const float n[3], float out[3]) {
    detail::pose_normal(to_pose_d(pose), n, out);
}

bool mirrors(const Pose& pose) { return detail::pose_mirrors(to_pose_d(pose)); }

} // namespace HNwd
