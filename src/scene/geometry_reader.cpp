// GeometryReader: geometrin för en tråd, med en egen cursor över dokumentets
// delade layout. Här ligger också kontrollen av ett fragments post, som
// Document::fragment_mesh() delar.

#include "document_impl.h"

#include <new>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <utility>

namespace HNwd {

namespace {

std::string box_text(const float* min, const float* max) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "(%.9g, %.9g, %.9g)-(%.9g, %.9g, %.9g)", min[0], min[1], min[2],
                  max[0], max[1], max[2]);
    return buf;
}

} // namespace

detail::ErrorCode read_fragment_mesh(const detail::FragmentStore& store, size_t fragment,
                                     detail::GeometryCursor& cursor, detail::RecordData& rd,
                                     Mesh& out, std::string& why) {
    const detail::FragmentStore::Item& it = store.items[fragment];
    const detail::GeometryRefData& g = store.geomrefs[it.geomref];
    const uint32_t record = static_cast<uint32_t>(g.record);   // prövad i load_fragments()
    const std::string prefix = "fragment " + std::to_string(fragment) + ": ";
    std::string inner;
    const detail::ErrorCode ec = cursor.parse(record - 1, rd, &inner);
    if (ec != detail::ErrorCode::Ok) {
        why = prefix + inner;
        return ec;
    }

    // Fragmentets låda och antal ska vara postens egna. En form har sin låda
    // uträknad, och den får skilja på avrundningen.
    float slack = 0.0f;
    if (!rd.box_exact) {
        for (int i = 0; i < 6; ++i) slack = std::max(slack, std::fabs(rd.bbox[i]));
        slack *= 4 * std::numeric_limits<float>::epsilon();
    }
    bool same_box = true;
    for (int i = 0; i < 3; ++i)
        same_box = same_box && std::fabs(rd.bbox[i] - g.bbox[i]) <= slack &&
                   std::fabs(rd.bbox[3 + i] - g.bbox[3 + i]) <= slack;
    if (!same_box) {
        why = prefix + "the box of record " + std::to_string(record) + " is " +
              box_text(rd.bbox, rd.bbox + 3) + " but the fragment says " +
              box_text(g.bbox, g.bbox + 3);
        return detail::ErrorCode::CorruptLink;
    }
    // En text bär inte sina glyfer, så dess antal går inte att pröva.
    if (!rd.text && rd.primitives != static_cast<uint32_t>(it.primitives)) {
        why = prefix + "record " + std::to_string(record) + " has " +
              std::to_string(rd.primitives) + " primitives but the fragment says " +
              std::to_string(it.primitives);
        return detail::ErrorCode::CorruptLink;
    }

    to_mesh(rd, out);

    // En spegling vänder framsidan: byt två hörn i varje triangel.
    detail::PoseD pose;
    store.pose(fragment, pose);
    if (detail::pose_mirrors(pose) && out.kind == MeshKind::Triangles) {
        for (size_t k = 0; k + 2 < out.indices.size(); k += 3)
            std::swap(out.indices[k + 1], out.indices[k + 2]);
    }
    return detail::ErrorCode::Ok;
}

// ---------------------------------------------------------------------------
// GeometryReader.

struct GeometryReader::Impl {
    // Cursorn pekar in i layouten och filen, så den ligger efter dem och
    // förstörs först.
    std::shared_ptr<const detail::MappedFile> file;
    std::shared_ptr<const detail::GeometryLayout> layout;
    std::shared_ptr<const detail::FragmentStore> fragments;
    detail::GeometryCursor cursor;
    detail::RecordData record;
    std::string last_error;

    Status fail(detail::ErrorCode code, const std::string& text) {
        last_error = text.empty() ? detail::error_message(code) : text;
        return to_status(code);
    }
    Status fail(Status status, const char* text) {
        last_error = (text && *text) ? text : status_text(status);
        return status;
    }
    Status ok() {
        last_error.clear();
        return Status::Ok;
    }
};

namespace {

// Ger Impl för en läsare, och skapar den om läsaren har flyttats ifrån.
template <class T>
T& ensure(std::unique_ptr<T>& p) {
    if (!p) p = std::make_unique<T>();
    return *p;
}

} // namespace

GeometryReader::GeometryReader() : impl_(std::make_unique<Impl>()) {}
GeometryReader::~GeometryReader() = default;
GeometryReader::GeometryReader(GeometryReader&&) noexcept = default;
GeometryReader& GeometryReader::operator=(GeometryReader&&) noexcept = default;

void GeometryReader::close() {
    if (impl_) *impl_ = Impl{};
}

bool GeometryReader::is_open() const { return impl_ && impl_->fragments != nullptr; }

const char* GeometryReader::last_error() const { return impl_ ? impl_->last_error.c_str() : ""; }

Status GeometryReader::open(const Document& doc, size_t cache_bytes) try {
    Impl& s = ensure(impl_);
    s = Impl{};
    const Document::Impl* d = doc.impl_.get();
    if (!d || !d->is_open() || !d->geometry || !d->fragments)
        return s.fail(Status::NotLoaded,
                      "the document's fragments have not been loaded: call load_fragments()");
    s.file = d->file;
    s.layout = d->geometry;
    s.fragments = d->fragments;
    s.cursor = detail::GeometryCursor(*s.layout, s.file->data(), s.file->size(), cache_bytes);
    return s.ok();
} catch (const std::bad_alloc&) {
    Impl& s = ensure(impl_);
    s = Impl{};
    return out_of_memory(s);
}

Status GeometryReader::fragment_mesh(size_t fragment, Mesh& out) try {
    Impl& s = ensure(impl_);
    if (!s.fragments) return s.fail(Status::NotLoaded, "the reader is not open");
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

Status GeometryReader::record_mesh(uint32_t record, Mesh& out) try {
    Impl& s = ensure(impl_);
    if (!s.layout) return s.fail(Status::NotLoaded, "the reader is not open");
    if (record < 1 || record > s.layout->record_count())
        return s.fail(Status::OutOfRange,
                      ("record " + std::to_string(record) + " is outside 1.." +
                       std::to_string(s.layout->record_count()))
                          .c_str());
    std::string why;
    const detail::ErrorCode ec = s.cursor.parse(record - 1, s.record, &why);
    if (ec != detail::ErrorCode::Ok) return s.fail(ec, why);
    to_mesh(s.record, out);
    return s.ok();
} catch (const std::bad_alloc&) {
    return out_of_memory(ensure(impl_));
}

} // namespace HNwd
