// Document: det rumsliga indexet över fragmenten, ur filens paket
// LcOpNwdSpatialHierarchy eller byggt ur fragmentens lådor.

#include "document_impl.h"

#include <new>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace HNwd {

namespace {

// Högst så många fragment i ett löv i det egna indexet.
constexpr uint32_t kLeafItems = 32;
// Förloppet rapporteras så här tätt, räknat i fragment.
constexpr uint32_t kSpatialProgressEvery = 65536;

// Ger Impl för ett Document, och skapar den om dokumentet har flyttats ifrån.
template <class T>
T& ensure(std::unique_ptr<T>& p) {
    if (!p) p = std::make_unique<T>();
    return *p;
}

// Förloppet över två lika stora steg: trädet, sedan lådorna. `done` räknas i
// fragment inom steget.
class Steps {
public:
    Steps(const Progress& progress, uint64_t fragments) : progress_(progress), n_(fragments) {}
    bool report(int step, uint64_t done) const {
        if (!progress_) return true;
        return progress_(static_cast<uint64_t>(step) * n_ + done, 2 * n_);
    }

private:
    const Progress& progress_;
    uint64_t n_;
};

detail::ErrorCode cancelled(std::string& why, uint64_t done, uint64_t total) {
    why = "LcOpNwdSpatialHierarchy: building the index was cancelled after " +
          std::to_string(done) + " of " + std::to_string(total) + " steps";
    return detail::ErrorCode::Cancelled;
}

// Det egna indexet: fragmenten delas vid medianen av lådornas mittpunkter
// längs den längsta axeln, tills ett löv har högst kLeafItems. Noderna läggs
// i preorder med en explicit stack. Mittpunkterna räknas i världen och hålls
// som float: de bestämmer bara delningen, inte lådorna.
detail::ErrorCode build_own(const detail::FragmentStore& store, const Steps& steps,
                            detail::SpatialTree& tree, std::string& why) {
    const uint32_t n = static_cast<uint32_t>(store.size());
    std::vector<float> centers(size_t(n) * 3);
    for (uint32_t i = 0; i < n; ++i) {
        if (i % kSpatialProgressEvery == 0 && !steps.report(0, i))
            return cancelled(why, i, 2 * uint64_t(n));
        detail::PoseD pose;
        store.pose(i, pose);
        double lo[3], hi[3];
        detail::pose_box(pose, store.geomrefs[store.items[i].geomref].bbox, lo, hi);
        for (int k = 0; k < 3; ++k) centers[size_t(i) * 3 + k] = static_cast<float>(0.5 * (lo[k] + hi[k]));
    }

    tree.items.resize(n);
    for (uint32_t i = 0; i < n; ++i) tree.items[i] = i;
    // Ett binärt träd med löv om minst kLeafItems / 2 har färre än
    // 4·n / kLeafItems + 1 noder.
    tree.nodes.reserve(size_t(4) * n / kLeafItems + 1);
    tree.children.reserve(size_t(4) * n / kLeafItems + 1);

    struct Job {
        uint32_t begin, count, parent, slot;   // slot: platsen i children, kNone för roten
    };
    std::vector<Job> stack;
    stack.push_back({0, n, kNone, kNone});
    while (!stack.empty()) {
        const Job job = stack.back();
        stack.pop_back();

        const uint32_t index = static_cast<uint32_t>(tree.nodes.size());
        detail::SpatialTree::Node node;
        node.parent = job.parent;
        node.item_begin = job.begin;
        node.item_count = job.count;
        if (job.slot != kNone) tree.children[job.slot] = index;
        if (job.count <= kLeafItems) {
            tree.nodes.push_back(node);
            continue;
        }

        // Den längsta axeln bland mittpunkterna i delen.
        uint32_t* first = tree.items.data() + job.begin;
        uint32_t* last = first + job.count;
        float lo[3], hi[3];
        for (int k = 0; k < 3; ++k) lo[k] = hi[k] = centers[size_t(*first) * 3 + k];
        for (const uint32_t* it = first; it != last; ++it)
            for (int k = 0; k < 3; ++k) {
                const float c = centers[size_t(*it) * 3 + k];
                lo[k] = std::min(lo[k], c);
                hi[k] = std::max(hi[k], c);
            }
        int axis = 0;
        for (int k = 1; k < 3; ++k)
            if (hi[k] - lo[k] > hi[axis] - lo[axis]) axis = k;

        const uint32_t half = job.count / 2;
        // Lika mittpunkter ordnas efter fragmentindex, så att indexet blir
        // detsamma varje gång.
        std::nth_element(first, first + half, last, [&](uint32_t a, uint32_t b) {
            const float ca = centers[size_t(a) * 3 + axis], cb = centers[size_t(b) * 3 + axis];
            return ca < cb || (ca == cb && a < b);
        });

        node.child_begin = static_cast<uint32_t>(tree.children.size());
        node.child_count = 2;
        tree.children.push_back(kNone);
        tree.children.push_back(kNone);
        tree.nodes.push_back(node);
        // Det högra läggs först, så att det vänstra delträdet får numren närmast
        // efter noden: preorder.
        stack.push_back({job.begin + half, job.count - half, index, node.child_begin + 1});
        stack.push_back({job.begin, half, index, node.child_begin});
    }
    return detail::ErrorCode::Ok;
}

// Varje nods världslåda och primitivsumma: löven ur sina fragment, de inre
// noderna ur sina barn. Noderna är i preorder, så ett barn har alltid högre
// index än sin förälder och samlas in baklänges.
detail::ErrorCode fill_bounds(const detail::FragmentStore& store, const Steps& steps,
                              SpatialIndex& out, std::string& why) {
    const detail::SpatialTree& tree = out.tree;
    const size_t count = tree.nodes.size();
    BoxD empty;
    for (int k = 0; k < 3; ++k) {
        empty.min[k] = INFINITY;
        empty.max[k] = -INFINITY;
    }
    out.bounds.assign(count, empty);
    out.primitives.assign(count, 0);

    const uint64_t total = 2 * uint64_t(store.size());
    uint64_t done = 0, next_report = 0;
    for (size_t i = 0; i < count; ++i) {
        const detail::SpatialTree::Node& node = tree.nodes[i];
        if (node.child_count != 0) continue;
        BoxD& box = out.bounds[i];
        uint64_t prims = 0;
        for (uint32_t k = 0; k < node.item_count; ++k) {
            const uint32_t f = tree.items[node.item_begin + k];
            detail::PoseD pose;
            store.pose(f, pose);
            double lo[3], hi[3];
            detail::pose_box(pose, store.geomrefs[store.items[f].geomref].bbox, lo, hi);
            for (int a = 0; a < 3; ++a) {
                box.min[a] = std::min(box.min[a], lo[a]);
                box.max[a] = std::max(box.max[a], hi[a]);
            }
            prims += static_cast<uint32_t>(store.items[f].primitives);
        }
        out.primitives[i] = prims;
        done += node.item_count;
        if (done >= next_report) {
            if (!steps.report(1, done)) return cancelled(why, store.size() + done, total);
            next_report = done + kSpatialProgressEvery;
        }
    }
    for (size_t i = count; i-- > 1;) {
        const uint32_t parent = tree.nodes[i].parent;
        BoxD& p = out.bounds[parent];
        const BoxD& c = out.bounds[i];
        for (int a = 0; a < 3; ++a) {
            p.min[a] = std::min(p.min[a], c.min[a]);
            p.max[a] = std::max(p.max[a], c.max[a]);
        }
        out.primitives[parent] += out.primitives[i];
    }
    if (!steps.report(2, 0)) return cancelled(why, total, total);
    return detail::ErrorCode::Ok;
}

} // namespace

size_t SpatialIndex::memory_bytes() const {
    return sizeof(*this) + tree.nodes.capacity() * sizeof(detail::SpatialTree::Node) +
           tree.children.capacity() * sizeof(uint32_t) + tree.items.capacity() * sizeof(uint32_t) +
           bounds.capacity() * sizeof(BoxD) + primitives.capacity() * sizeof(uint64_t);
}

detail::ErrorCode build_spatial_index(const detail::MappedFile& file,
                                      const detail::DbCacheEntry* spatial, size_t max_stream,
                                      const detail::FragmentStore& store, SpatialSource source,
                                      const Progress& progress, SpatialIndex& out,
                                      std::string& why) {
    out = SpatialIndex{};
    const uint64_t n = store.size();
    if (n == 0) return detail::ErrorCode::Ok;   // inga fragment: inga noder
    const Steps steps(progress, n);
    if (!steps.report(0, 0)) return cancelled(why, 0, 2 * n);

    if (source == SpatialSource::File && spatial) {
        std::vector<uint8_t> bytes;
        detail::ErrorCode ec = unpack_package(file, *spatial, max_stream, bytes, why);
        if (ec != detail::ErrorCode::Ok) return ec;
        ec = detail::parse_spatial_hierarchy(bytes.data(), bytes.size(),
                                             static_cast<uint32_t>(n), out.tree, &why);
        // Ett paket som finns men inte går att tolka är ett fel; det egna
        // indexet byggs bara när paketet saknas eller anroparen ber om det.
        if (ec != detail::ErrorCode::Ok) return ec;
        out.from_file = true;
    }
    if (!out.from_file) {
        out.tree = detail::SpatialTree{};
        const detail::ErrorCode ec = build_own(store, steps, out.tree, why);
        if (ec != detail::ErrorCode::Ok) return ec;
    }
    if (!steps.report(1, 0)) return cancelled(why, n, 2 * n);
    return fill_bounds(store, steps, out, why);
}

// ---------------------------------------------------------------------------
// Document.

Status Document::load_spatial(const Progress& progress, SpatialSource source) try {
    Impl& s = ensure(impl_);
    if (!is_open()) return s.fail(Status::NotLoaded, "no file is open");
    if (source != SpatialSource::File && source != SpatialSource::Own)
        return s.fail(Status::OutOfRange, "the spatial source is neither File nor Own");
    if (s.spatial && s.spatial_source == source) return s.ok();
    // Ett index som redan är byggt för källan används igen.
    std::shared_ptr<const SpatialIndex>& built = s.spatial_built[static_cast<size_t>(source)];
    if (!built) {
        if (const Status f = load_fragments(progress); f != Status::Ok) return f;
        auto index = std::make_shared<SpatialIndex>();
        std::string why;
        const detail::ErrorCode ec =
            build_spatial_index(*s.file, s.spatial_entry, s.options.max_stream_bytes, *s.fragments,
                                source, progress, *index, why);
        if (ec != detail::ErrorCode::Ok) return s.fail(ec, why);
        built = std::move(index);
    }
    s.spatial = built;
    s.spatial_source = source;
    return s.ok();
} catch (const std::bad_alloc&) {
    return out_of_memory(ensure(impl_));
}

bool Document::spatial_from_file() const {
    return impl_ && impl_->spatial && impl_->spatial->from_file;
}

size_t Document::spatial_node_count() const {
    return impl_ && impl_->spatial ? impl_->spatial->tree.nodes.size() : 0;
}

bool Document::spatial_node(uint32_t i, SpatialNode& out) const {
    if (!impl_ || !impl_->spatial || i >= impl_->spatial->tree.nodes.size()) return false;
    const SpatialIndex& x = *impl_->spatial;
    const detail::SpatialTree::Node& n = x.tree.nodes[i];
    SpatialNode r;
    r.parent = n.parent;
    r.child_count = n.child_count;
    r.item_begin = n.item_begin;
    r.item_count = n.item_count;
    r.primitives = x.primitives[i];
    r.bounds = x.bounds[i];
    out = r;
    return true;
}

uint32_t Document::spatial_child(uint32_t node, uint32_t k) const {
    if (!impl_ || !impl_->spatial || node >= impl_->spatial->tree.nodes.size()) return kNone;
    const detail::SpatialTree& t = impl_->spatial->tree;
    const detail::SpatialTree::Node& n = t.nodes[node];
    if (k >= n.child_count) return kNone;
    return t.children[n.child_begin + k];
}

const uint32_t* Document::spatial_items() const {
    if (!impl_ || !impl_->spatial || impl_->spatial->tree.items.empty()) return nullptr;
    return impl_->spatial->tree.items.data();
}

} // namespace HNwd
