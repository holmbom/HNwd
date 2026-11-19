#include "hierarchy.h"

#include "byte_reader.h"
#include "partition_props.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <iterator>
#include <memory>
#include <unordered_map>
#include <utility>

namespace HNwd::detail {

// ---------------------------------------------------------------------------
// Strängarna.

bool StringPool::add(std::string_view s, uint32_t& index) {
    size_t hash = 0;
    if (dedupe_) {
        hash = std::hash<std::string_view>()(s);
        const uint32_t found = table_.find(hash, [&](uint32_t v) { return get(v) == s; });
        if (found != kNone) {
            ++found_;
            index = found;
            return true;
        }
        if (table_.gave_up() || (++looked_up_ == kDedupeSample && found_ < kDedupeSample / 8)) {
            dedupe_ = false;
            table_.clear();
        }
    }
    if (s.size() > UINT32_MAX - chars_.size() || ends_.size() >= kNone - 1) return false;
    chars_.insert(chars_.end(), s.begin(), s.end());
    ends_.push_back(static_cast<uint32_t>(chars_.size()));
    index = static_cast<uint32_t>(ends_.size() - 1);
    if (dedupe_) {
        table_.insert(hash, index);
        if (table_.gave_up()) {
            dedupe_ = false;
            table_.clear();
        }
    }
    return true;
}

std::string_view StringPool::get(uint32_t index) const {
    if (index >= ends_.size()) return {};
    const uint32_t begin = index == 0 ? 0 : ends_[index - 1];
    return std::string_view(chars_.data() + begin, ends_[index] - begin);
}

void StringPool::dedupe(bool on) {
    dedupe_ = on;
    looked_up_ = 0;
    found_ = 0;
    table_.clear();
    if (!on) return;
    for (uint32_t i = 0; i < ends_.size() && !table_.gave_up(); ++i) {
        // En dubblett som redan finns behåller sitt eget index; den första vinner.
        const std::string_view s = get(i);
        const size_t hash = std::hash<std::string_view>()(s);
        if (table_.find(hash, [&](uint32_t v) { return get(v) == s; }) == kNone)
            table_.insert(hash, i);
    }
    if (table_.gave_up()) {
        dedupe_ = false;
        table_.clear();
    }
}

// ---------------------------------------------------------------------------
// Trädet.

namespace {

// Formerna i Hierarchy::transforms.
constexpr uint32_t kForm40 = 0, kForm41 = 1, kForm41Shared = 2, kForm39 = 3;
constexpr uint32_t kFormShift = 30;
constexpr uint32_t kOffsetMask = (uint32_t(1) << kFormShift) - 1;

// Värdet för `node` i en gles lista sorterad efter nod, eller `missing`.
uint32_t sparse_value(const std::vector<NodeValue>& list, uint32_t node, uint32_t missing) {
    const auto it = std::lower_bound(list.begin(), list.end(), node,
                                     [](const NodeValue& a, uint32_t n) { return a.node < n; });
    return it != list.end() && it->node == node ? it->value : missing;
}

} // namespace

int32_t Hierarchy::flags(uint32_t node) const {
    return static_cast<int32_t>(sparse_value(node_flags, node, 0));
}

uint32_t Hierarchy::partition(uint32_t node) const { return sparse_value(node_partitions, node, kNone); }

int32_t Hierarchy::transform_class(uint32_t index) const {
    if (index >= transforms.size()) return 0;
    switch (transforms[index] >> kFormShift) {
        case kForm40: return 40;
        case kForm39: return 39;
        default: return 41;
    }
}

bool Hierarchy::transform(uint32_t index, StoredTransform& out) const {
    if (index >= transforms.size()) return false;
    const uint32_t form = transforms[index] >> kFormShift;
    const size_t at = transforms[index] & kOffsetMask;
    const size_t need = form == kForm40 ? 3 : form == kForm41 ? 7 : form == kForm41Shared ? 4 : 13;
    if (at > transform_values.size() || need > transform_values.size() - at) return false;
    const double* v = transform_values.data() + at;
    StoredTransform t;
    t.source_class = transform_class(index);
    if (form == kForm39) {
        for (int k = 0; k < 9; ++k) t.m[k] = v[k];
        v += 9;
    }
    for (int k = 0; k < 3; ++k) t.t[k] = v[k];
    if (form == kForm41) {
        for (int k = 0; k < 4; ++k) t.q[k] = v[3 + k];
    } else if (form == kForm41Shared) {
        uint64_t q_at = 0;
        std::memcpy(&q_at, &v[3], sizeof q_at);
        if (q_at > transform_values.size() || 4 > transform_values.size() - q_at) return false;
        for (int k = 0; k < 4; ++k) t.q[k] = transform_values[static_cast<size_t>(q_at) + k];
    } else if (form == kForm39) {
        t.reverses = static_cast<int32_t>(v[3]);
    }
    out = t;
    return true;
}

size_t Hierarchy::memory_bytes() const {
    size_t n = strings.memory_bytes() + names.capacity() * sizeof(StoredName) +
               properties.capacity() * sizeof(StoredProperty) + tabs.capacity() * sizeof(StoredTab) +
               (transforms.capacity() + node_transforms.capacity()) * sizeof(uint32_t) +
               transform_values.capacity() * sizeof(double) +
               nodes.capacity() * sizeof(StoredNode) +
               (node_tabs.capacity() + tab_begin.capacity() + children.capacity() + top.capacity()) *
                   sizeof(uint32_t) +
               root_counts.capacity() * sizeof(int32_t) +
               (node_flags.capacity() + node_partitions.capacity()) * sizeof(NodeValue) +
               partitions.capacity() * sizeof(PartitionTail) + lights.capacity() * sizeof(PartitionLight);
    for (const PartitionTail& p : partitions)
        n += p.light_refs.capacity() * sizeof(uint32_t) + p.loader.capacity() + p.label.capacity() +
             p.source_path.capacity();
    return n;
}

// ---------------------------------------------------------------------------
// Skelettet.

namespace {

void put_varint(std::vector<uint8_t>& out, uint64_t v) {
    while (v >= 0x80) {
        out.push_back(static_cast<uint8_t>(v | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<uint8_t>(v));
}

// Läser ett varint ur `p` som slutar före `end`. false om det inte ryms.
bool get_varint(const uint8_t*& p, const uint8_t* end, uint64_t& v) {
    v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (p == end) return false;
        const uint8_t b = *p++;
        v |= static_cast<uint64_t>(b & 0x7f) << shift;
        if (!(b & 0x80)) return true;
    }
    return false;
}

uint64_t zigzag(int64_t d) { return (static_cast<uint64_t>(d) << 1) ^ static_cast<uint64_t>(d >> 63); }
int64_t unzigzag(uint64_t v) { return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1); }

// Klasserna som en bakåtpekare oftast har; var och en har sitt eget förra
// värde i ett block av PackedRefs, och alla andra delar ett.
constexpr int32_t kRefClasses[] = {22, 31, 32, 39, 40, 41, 52, 53, 66, 70, 76, 79, 86, 92, 185, 200};
constexpr uint32_t kRefCodes = sizeof(kRefClasses) / sizeof(kRefClasses[0]);

uint32_t ref_code(int64_t cls) {
    for (uint32_t k = 0; k < kRefCodes; ++k)
        if (kRefClasses[k] == cls) return k;
    return kRefCodes;
}

} // namespace

void PackedRows::push(const uint32_t (&row)[kWidth]) {
    if (count_ % kBlock == 0) {
        block_at_.push_back(bytes_.size());
        std::fill(std::begin(last_), std::end(last_), 0u);
    }
    for (size_t k = 0; k < kWidth; ++k) {
        put_varint(bytes_, zigzag(static_cast<int64_t>(row[k]) - last_[k]));
        last_[k] = row[k];
    }
    ++count_;
}

bool PackedRows::get(size_t index, uint32_t (&row)[kWidth]) const {
    if (index >= count_) return false;
    const uint8_t* p = bytes_.data() + block_at_[index / kBlock];
    const uint8_t* end = bytes_.data() + bytes_.size();
    int64_t v[kWidth] = {};
    for (size_t i = 0; i <= index % kBlock; ++i) {
        for (size_t k = 0; k < kWidth; ++k) {
            uint64_t d = 0;
            if (!get_varint(p, end, d)) return false;
            v[k] += unzigzag(d);
        }
    }
    for (size_t k = 0; k < kWidth; ++k) row[k] = static_cast<uint32_t>(v[k]);
    return true;
}

void PackedRefs::add_stream(const std::vector<IdRef>& refs) {
    stream_begin_.push_back(blocks_.size());
    uint32_t last[kRefCodes + 1] = {};
    int32_t last_id = 0, last_cls = 0;
    for (size_t i = 0; i < refs.size(); ++i) {
        const IdRef& r = refs[i];
        if (i % kBlock == 0) {
            blocks_.push_back(Block{r.id, 0, bytes_.size()});
            std::fill(std::begin(last), std::end(last), 0u);
            last_id = r.id;
            last_cls = 0;
        }
        const bool new_cls = r.cls != last_cls;
        put_varint(bytes_, (static_cast<uint64_t>(static_cast<int64_t>(r.id) - last_id) << 1) |
                               (new_cls ? 1u : 0u));
        if (new_cls) put_varint(bytes_, zigzag(r.cls));
        const uint32_t code = ref_code(r.cls);
        put_varint(bytes_, zigzag(static_cast<int64_t>(r.value) - last[code]));
        last[code] = r.value;
        last_id = r.id;
        last_cls = r.cls;
        ++blocks_.back().count;
    }
    count_ += refs.size();
}

bool PackedRefs::list(uint32_t stream, std::vector<IdRef>& out) const {
    out.clear();
    if (stream >= stream_begin_.size()) return false;
    const size_t last_block = stream + 1 < stream_begin_.size() ? stream_begin_[stream + 1] : blocks_.size();
    const uint8_t* end = bytes_.data() + bytes_.size();
    for (size_t k = stream_begin_[stream]; k < last_block; ++k) {
        const Block& b = blocks_[k];
        const uint8_t* p = bytes_.data() + b.at;
        uint32_t last[kRefCodes + 1] = {};
        int64_t cur = b.first_id, c = 0;
        for (uint32_t i = 0; i < b.count; ++i) {
            uint64_t head = 0, raw = 0;
            if (!get_varint(p, end, head)) return false;
            cur += static_cast<int64_t>(head >> 1);
            if (head & 1) {
                if (!get_varint(p, end, raw)) return false;
                c = unzigzag(raw);
            }
            if (!get_varint(p, end, raw)) return false;
            const uint32_t code = ref_code(c);
            last[code] = static_cast<uint32_t>(static_cast<int64_t>(last[code]) + unzigzag(raw));
            out.push_back(IdRef{static_cast<int32_t>(cur), static_cast<int32_t>(c), last[code]});
        }
    }
    return true;
}

bool PackedRefs::find(uint32_t stream, int32_t id, int32_t& cls, uint32_t& value) const {
    if (stream >= stream_begin_.size()) return false;
    const size_t first = stream_begin_[stream];
    const size_t last_block = stream + 1 < stream_begin_.size() ? stream_begin_[stream + 1] : blocks_.size();
    // Det sista blocket som börjar på eller före id.
    const auto it = std::upper_bound(blocks_.begin() + static_cast<std::ptrdiff_t>(first),
                                     blocks_.begin() + static_cast<std::ptrdiff_t>(last_block), id,
                                     [](int32_t v, const Block& b) { return v < b.first_id; });
    if (it == blocks_.begin() + static_cast<std::ptrdiff_t>(first)) return false;
    const Block& b = *(it - 1);
    const uint8_t* p = bytes_.data() + b.at;
    const uint8_t* end = bytes_.data() + bytes_.size();
    uint32_t last[kRefCodes + 1] = {};
    int64_t cur = b.first_id, c = 0;
    for (uint32_t k = 0; k < b.count; ++k) {
        uint64_t head = 0, raw = 0;
        if (!get_varint(p, end, head)) return false;
        cur += static_cast<int64_t>(head >> 1);
        if (head & 1) {
            if (!get_varint(p, end, raw)) return false;
            c = unzigzag(raw);
        }
        if (!get_varint(p, end, raw)) return false;
        const uint32_t code = ref_code(c);
        const uint32_t v = static_cast<uint32_t>(static_cast<int64_t>(last[code]) + unzigzag(raw));
        last[code] = v;
        if (cur == id) {
            cls = static_cast<int32_t>(c);
            value = v;
            return true;
        }
        if (cur > id) return false;
    }
    return false;
}

bool HierarchySkeleton::location(uint32_t node, NodeLocation& out) const {
    uint32_t row[PackedRows::kWidth] = {};
    if (!locations.get(node, row)) return false;
    out.attr_pos = row[0];
    out.attr_next_id = static_cast<int32_t>(row[1]);
    out.tab_count = row[2];
    return true;
}

size_t HierarchySkeleton::checkpoint_bytes() const {
    size_t n = 0;
    for (const std::vector<InflateStream::Checkpoint>& list : checkpoints) {
        n += list.size() * sizeof(InflateStream::Checkpoint);
        for (const InflateStream::Checkpoint& cp : list) n += cp.window.size();
    }
    return n;
}

size_t HierarchySkeleton::memory_bytes() const {
    return locations.memory_bytes() + roots.capacity() * sizeof(RootLocation) +
           first_root.capacity() * sizeof(uint32_t) + refs.memory_bytes() +
           checkpoints.capacity() * sizeof(std::vector<InflateStream::Checkpoint>) + checkpoint_bytes();
}

namespace {

using Ptr = ObjectReader::Ptr;
using PropStreams = std::vector<std::vector<uint8_t>>;
using Checkpoints = std::vector<InflateStream::Checkpoint>;

constexpr const char* kWhat = "LcOpNwdLogicalHierarchy";
constexpr int32_t kFirstId = 100;

// Fönstret när en ström läses från början till slut.
constexpr size_t kWindowBytes = size_t(1) << 20;
// Utan skelett sparas kontrollpunkter bara för förloppet, och kastas direkt.
constexpr uint64_t kProgressCheckpointEvery = uint64_t(1) << 20;
// Förloppsfunktionen anropas efter så här många steg i tolkningen.
constexpr uint32_t kProgressSteps = 4096;

std::string props_what(size_t stream) {
    return "LcOaPartitionProps stream " + std::to_string(stream);
}

enum class Mode : uint8_t {
    Full,        // allt lagras
    Structure,   // noderna och namnparen lagras, flikarna läses förbi
    Region,      // en nods attribut läses om; bara flikarna lagras
};

// En ström som packas upp medan den läses, från början.
struct Inflated {
    InflateStream z;
    std::unique_ptr<StreamWindow> window;
    std::unique_ptr<ObjectReader> reader;
    std::string what;
    StreamSpan span;

    void close() {
        reader.reset();
        window.reset();
    }
};

// Läser trädet. Noderna och länkarna i trädet (som kan bära noder) läses med
// en egen stack av ramar; övriga objekt har bara namnpar och klass 185 inuti
// sig och läses direkt. Djupet begränsas av ObjectReader till kMaxNesting.
//
// Samma tolkare läser i tre lägen (Mode): hela trädet, skelettet, och en
// enskild nods attribut i efterhand.
class Parser {
public:
    Parser(Mode mode, Hierarchy& h, uint32_t guid_count, uint64_t max_nodes, bool transforms = true)
        : h_(h), mode_(mode), store_nodes_(mode != Mode::Region), store_tabs_(mode != Mode::Structure),
          keep_transforms_(transforms), guid_count_(guid_count), max_nodes_(max_nodes) {
        frames_.reserve(kMaxFrames);
    }

    // Hela trädet ur en buffert, med egenskapsströmmarna i minnet.
    ErrorCode run_buffer(const uint8_t* data, size_t size, const PropStreams* props,
                         std::string& detail) {
        props_buf_ = props;
        ObjectReader r(data, size, kWhat);
        main_ = &r;
        h_.strings.dedupe(true);
        const bool ok = start_top(r) && drive();
        if (ok) h_.non_utf8_strings += r.non_utf8_strings();
        finish();
        return result(ok, detail);
    }

    // Hela trädet ur filen, strömmarna uppackade medan de läses.
    ErrorCode run_file(const uint8_t* file, size_t size, const HierarchyStreams& streams,
                       HierarchySkeleton* skeleton, const HierarchyProgress& progress,
                       uint64_t every, std::string& detail) {
        file_ = file;
        size_ = size;
        streams_ = &streams;
        skeleton_ = skeleton;
        recording_ = skeleton != nullptr && mode_ == Mode::Structure;
        every_ = every;
        if (progress) progress_ = &progress;
        total_ = streams.tree.length;
        for (const StreamSpan& s : streams.props) total_ += s.length;

        const size_t tables = 1 + streams.props.size();
        if (recording_) {
            skeleton_->checkpoints.assign(tables, Checkpoints());
            table_refs_.assign(tables, std::vector<IdRef>());
        }
        Checkpoints* sink = recording_ ? &skeleton_->checkpoints[0] : progress_ ? &scratch_ : nullptr;
        tree_sink_ = sink;
        h_.strings.dedupe(true);
        bool ok = open_sequential(tree_, streams.tree, kWhat, sink,
                                  recording_ ? every_ : kProgressCheckpointEvery);
        if (ok) {
            main_ = tree_.reader.get();
            ok = start_top(*main_) && drive() && close_sequential(tree_);
        }
        if (ok) h_.non_utf8_strings += main_->non_utf8_strings();
        finish();
        if (ok && recording_) ok = build_skeleton();
        if (ok && progress_ && !(*progress_)(total_, total_))
            ok = fail_text(ErrorCode::Cancelled,
                           std::string(kWhat) + " at byte " + std::to_string(main_->pos()) +
                               ": cancelled through the progress function");
        if (!ok && !main_) {
            detail = det_;
            return err_;
        }
        return result(ok, detail);
    }

    // En lagrad nods attribut, läsa om ur `source`. Flikarna hamnar i h_ och
    // deras ordning i region_tabs().
    ErrorCode run_region(const Hierarchy& src, const HierarchySkeleton& sk, uint32_t node,
                         ObjectSource& source, std::string& detail) {
        src_ = &src;
        sk_ = &sk;
        source_ = &source;
        NodeLocation loc;
        if (!sk.location(node, loc)) {
            detail = std::string(kWhat) + ": the skeleton has no location for node " + std::to_string(node);
            return ErrorCode::CorruptLink;
        }
        ErrorCode code = ErrorCode::Ok;
        std::string why;
        ObjectReader* r = source.open(0, 0, loc.attr_pos, loc.attr_next_id, resolver(0), code, why);
        if (!r) {
            detail = why;
            return code;
        }
        main_ = r;
        // Nodens första rot: den första vars null-plats står efter attributens början.
        region_next_root_ = static_cast<size_t>(
            std::lower_bound(sk.roots.begin(), sk.roots.end(), loc.attr_pos,
                             [](const RootLocation& a, uint32_t p) { return a.slot < p; }) -
            sk.roots.begin());

        Frame f;
        f.kind = Frame::Node;
        f.r = r;
        f.cls = src.nodes[node].cls;
        f.region_root = true;
        frames_.push_back(f);
        bool ok = begin_attributes(frames_.back()) && drive() && import_shared_tabs();
        if (ok && tab_stack_.size() != loc.tab_count)
            ok = fail_text(ErrorCode::CorruptLink,
                           std::string(kWhat) + " at byte " + std::to_string(loc.attr_pos) + ": node " +
                               std::to_string(node) + " has " + std::to_string(tab_stack_.size()) +
                               " tabs, but the skeleton counted " + std::to_string(loc.tab_count));
        return result(ok, detail);
    }

    const std::vector<uint32_t>& region_tabs() const { return tab_stack_; }

private:
    // Efter tolkningen av hela trädet: barnlistorna i nodernas ordning, och
    // tabellerna för dubbletter släpps.
    void finish() {
        h_.strings.dedupe(false);
        rotations_.clear();
        order_children();
    }

    // --- Ramarna --------------------------------------------------------------

    static constexpr size_t kMaxFrames = static_cast<size_t>(kMaxNesting) + 8;

    struct Frame {
        enum Kind : uint8_t { Top, Node, Link } kind = Top;
        enum Phase : uint8_t { Head, Attrs, Children, Items } phase = Head;
        bool region_root = false;   // noden vars attribut läses om
        bool waiting = false;       // en plats lade en ram ovanpå; svaret kommer i result_
        bool region_open = false;   // attributen räknas som ett område för skelettet
        ObjectReader* r = nullptr;
        int32_t id = 0;
        int32_t cls = 0;
        int32_t i = 0;
        int32_t n = 0;
        uint64_t expanded = 1;
        uint32_t height = 0;
        size_t tab_base = 0;
        size_t child_base = 0;
        uint32_t index = kNone;   // nodens index i Hierarchy::nodes, när det är givet
        uint32_t transform = kNone;   // nodens första placering
        int32_t flags = 0;
        uint32_t tabs = 0;        // flikarna i Structure, räknade
        StoredNode node;
        NodeLocation loc;
    };

    enum class Slot : uint8_t { Done, Pushed, Failed };

    bool drive() {
        while (!frames_.empty()) {
            Frame& f = frames_.back();
            bool ok = false;
            switch (f.kind) {
                case Frame::Top: ok = top_step(f); break;
                case Frame::Node: ok = node_step(f); break;
                case Frame::Link: ok = link_step(f); break;
            }
            if (!ok) return false;
            if (progress_ && ++ticks_ >= kProgressSteps) {
                ticks_ = 0;
                if (!report()) return false;
            }
        }
        return true;
    }

    // En ram ovanpå de andra. Ramarna ligger i en vektor med reserverat utrymme,
    // så en referens till en ram gäller medan andra läggs till.
    bool push(const Frame& f) {
        if (frames_.size() >= kMaxFrames)
            return err(ErrorCode::LimitExceeded, "nesting deeper than the limit");
        frames_.push_back(f);
        return true;
    }

    // Den översta ramen är klar: dess objekt lämnas till ramen under.
    bool complete(const Frame& f, uint32_t value) {
        if (!f.r->finish(f.id, value)) return false;
        Ptr p;
        p.kind = Ptr::New;
        p.id = f.id;
        p.cls = f.cls;
        p.value = value;
        frames_.pop_back();
        result_ = p;
        return true;
    }

    // Läser en pekarplats. Ett nytt objekt som är en nod, eller en länk i
    // trädet, får en egen ram (Pushed); övriga läses direkt (Done).
    // `may_partition`: platsen får ta en ny klass 32.
    Slot begin_slot(ObjectReader& r, std::initializer_list<int32_t> allowed, bool allow_null,
                    bool allow_back, bool props, bool may_partition, Ptr& p) {
        const size_t at = r.pos();
        slot_at_ = at;
        if (!r.ptr(p, allowed, allow_null, allow_back)) return Slot::Failed;
        if (p.kind == Ptr::Back) {
            note_back(r, p);
            return Slot::Done;
        }
        if (p.kind != Ptr::New) return Slot::Done;
        if (p.cls == 32 && !may_partition) {
            err(ErrorCode::Unsupported,
                "partition node (class 32) is only decoded at the top level or under a partition", at);
            return Slot::Failed;
        }
        if (p.cls == 53 || p.cls == 22 || p.cls == 32 || (p.cls == 79 && !props)) {
            Frame f;
            f.kind = p.cls == 79 ? Frame::Link : Frame::Node;
            f.r = &r;
            f.id = p.id;
            f.cls = p.cls;
            return push(f) ? Slot::Pushed : Slot::Failed;
        }
        uint32_t value = 0;
        if (!body(r, p.cls, value) || !r.finish(p.id, value)) return Slot::Failed;
        p.value = value;
        return Slot::Done;
    }

    // En plats som aldrig tar en nod eller en länk i trädet.
    bool inline_slot(ObjectReader& r, std::initializer_list<int32_t> allowed, bool allow_null,
                     bool allow_back, bool props, Ptr& p) {
        const Slot s = begin_slot(r, allowed, allow_null, allow_back, props, false, p);
        if (s == Slot::Pushed) return err(ErrorCode::CorruptObject, "internal error: a leaf slot holds a node");
        return s == Slot::Done;
    }

    // --- Toppen ---------------------------------------------------------------

    bool start_top(ObjectReader& r) {
        Frame f;
        f.kind = Frame::Top;
        f.r = &r;
        return push(f);
    }

    bool top_step(Frame& f) {
        ObjectReader& r = *f.r;
        if (f.phase == Frame::Head) {
            int32_t k;
            if (!r.count(4, k)) return false;
            if (k < 1) return r.fail("header count k is below 1");
            for (int32_t i = 0; i < k; ++i) {
                int32_t n;
                if (!r.count(0, n)) return false;
                h_.root_counts.push_back(n);
                unread_ += static_cast<uint64_t>(n);
            }
            if (!r.implicit_root()) return false;

            // En toppnod tar minst [1][id].
            int32_t tops;
            if (!r.count(8, tops)) return false;
            f.n = tops;
            f.expanded = 1;   // den underförstådda roten
            f.height = 0;
            if (f.expanded > max_nodes_) return err(ErrorCode::LimitExceeded, "expanded tree exceeds the node limit");
            f.phase = Frame::Children;
            return true;
        }

        if (f.waiting) {
            f.waiting = false;
            if (!top_child(f, result_)) return false;
        }
        while (f.i < f.n) {
            Ptr p;
            const Slot s = begin_slot(r, {53, 32}, false, true, false, true, p);
            if (s == Slot::Failed) return false;
            if (s == Slot::Pushed) {
                f.waiting = true;
                return true;
            }
            if (!top_child(f, p)) return false;
        }
        if (f.height + 1 > static_cast<uint32_t>(kMaxNesting))
            return err(ErrorCode::LimitExceeded, "expanded tree is higher than the nesting limit");
        h_.expanded_total = f.expanded;

        int32_t tail;
        if (!r.i32(tail)) return false;
        if (tail != 1) return r.fail("hierarchy tail is not 1");
        if (!finish_roots()) return false;
        if (!r.end()) return false;
        frames_.pop_back();
        return true;
    }

    bool top_child(Frame& f, const Ptr& p) {
        if (!add_child(p.value, f.expanded, f.height)) return false;
        h_.top.push_back(p.value);
        ++f.i;
        return true;
    }

    // Räknar in ett barn i föräldrans storlek och höjd, i 64 bitar utan att slå runt.
    bool add_child(uint32_t index, uint64_t& expanded, uint32_t& height) {
        const StoredNode& child = h_.nodes[index];
        if (child.expanded > max_nodes_ - expanded)
            return err(ErrorCode::LimitExceeded, "expanded tree exceeds the node limit");
        expanded += child.expanded;
        if (child.height > height) height = child.height;
        return true;
    }

    // --- Noderna ----------------------------------------------------------------

    // Klass 53, 22 och 32. Flikar och barn hamnar i följd i sina listor först när
    // noden är färdig: noder som ligger inuti den skriver sina egna under tiden.
    // Klass 32 har en int32 0 först, därefter kroppen en klass 53 har, och sist
    // svansen.
    bool node_step(Frame& f) {
        ObjectReader& r = *f.r;
        switch (f.phase) {
            case Frame::Head: {
                StoredNode& n = f.node;
                n.cls = static_cast<uint8_t>(f.cls);
                if (f.cls == 32) {
                    int32_t zero;
                    if (!r.i32(zero)) return false;
                    if (zero != 0) return r.fail("class-32 body does not start with 0");
                }
                int32_t x;
                if (!pooled(r, n.name, store_nodes_) || !username(r, n.user, true)) return false;
                if (!r.i32(f.flags) || !r.i32(x)) return false;
                if (x < 0 || static_cast<uint32_t>(x) > guid_count_)
                    return r.fail("element index is outside the GUID table");
                n.element = x;
                return begin_attributes(f);
            }
            case Frame::Attrs: return node_attributes(f);
            case Frame::Children: return node_children(f);
            case Frame::Items: break;
        }
        return err(ErrorCode::CorruptObject, "internal error: node frame in an unknown phase");
    }

    // Antalet attribut. Positionen och nästa id här är det skelettet sparar.
    bool begin_attributes(Frame& f) {
        ObjectReader& r = *f.r;
        // Positionen lagras bara i skelettet; utan det får strömmen vara
        // längre än 32 bitar räcker till.
        if (recording_ && !pos32(r.pos(), f.loc.attr_pos)) return false;
        f.loc.attr_next_id = r.next_id();
        int32_t attributes;
        if (!r.count(4, attributes)) return false;
        f.n = attributes;
        f.i = 0;
        f.tab_base = tab_stack_.size();
        if (recording_) {
            region_starts_.push_back(f.loc.attr_next_id);
            f.region_open = true;
        }
        f.phase = Frame::Attrs;
        return true;
    }

    bool node_attributes(Frame& f) {
        ObjectReader& r = *f.r;
        if (f.waiting) {
            f.waiting = false;
            if (!attribute(f, result_)) return false;
        }
        while (f.i < f.n) {
            Ptr p;
            const Slot s = begin_slot(r, {86, 31, 40, 41, 39, 79, 200, 66, 70, 76, 92}, true, true,
                                      false, false, p);
            if (s == Slot::Failed) return false;
            if (s == Slot::Pushed) {
                f.waiting = true;
                return true;
            }
            if (!attribute(f, p)) return false;
        }
        if (f.region_open) {
            region_starts_.pop_back();
            f.region_open = false;
        }
        if (f.region_root) {
            frames_.pop_back();
            return true;
        }

        f.expanded = 1;
        f.height = 0;
        if (store_nodes_ && f.expanded > max_nodes_)
            return err(ErrorCode::LimitExceeded, "expanded tree exceeds the node limit");
        f.child_base = child_stack_.size();
        f.i = 0;
        f.n = 0;
        if (f.cls == 53 || f.cls == 32) {
            int32_t children;
            // Ett barn tar minst [1][id].
            if (!r.count(8, children)) return false;
            f.n = children;
        }
        if (store_nodes_ && !assign_node(f)) return false;
        f.phase = Frame::Children;
        return true;
    }

    // Noden får sitt index när attributen är lästa, före barnen: flikarna och
    // skelettets plats läggs i samma ordning. Barnlistan läggs först när
    // barnen är lästa, och sorteras i nodernas ordning sist (order_children).
    bool assign_node(Frame& f) {
        if (h_.nodes.size() >= kNone - 1) return err(ErrorCode::LimitExceeded, "more stored nodes than an index can address");
        f.index = static_cast<uint32_t>(h_.nodes.size());
        if (mode_ == Mode::Full) {
            const size_t count = tab_stack_.size() - f.tab_base;
            if (count >= UINT32_MAX - h_.node_tabs.size())
                return err(ErrorCode::LimitExceeded, "more node tabs than an index can address");
            if (h_.tab_begin.empty()) h_.tab_begin.push_back(0);
            h_.node_tabs.insert(h_.node_tabs.end(),
                                tab_stack_.begin() + static_cast<std::ptrdiff_t>(f.tab_base),
                                tab_stack_.end());
            tab_stack_.resize(f.tab_base);
            h_.tab_begin.push_back(static_cast<uint32_t>(h_.node_tabs.size()));
        }
        if (f.flags != 0) h_.node_flags.push_back(NodeValue{f.index, static_cast<uint32_t>(f.flags)});
        h_.nodes.push_back(f.node);
        if (keep_transforms_) h_.node_transforms.push_back(f.transform);
        child_spans_.push_back(0);
        if (recording_)
            skeleton_->locations.push({f.loc.attr_pos, static_cast<uint32_t>(f.loc.attr_next_id), f.tabs});
        return true;
    }

    // Barnlistorna i nodernas ordning, så att en nods antal är nästa nods
    // början minus dess egen. Under tolkningen står varje nods början i
    // listan som den lades, och antalet i child_spans_.
    void order_children() {
        std::vector<uint32_t> ordered;
        ordered.reserve(h_.children.size());
        for (size_t i = 0; i < h_.nodes.size(); ++i) {
            StoredNode& n = h_.nodes[i];
            const uint32_t begin = n.child_begin;
            n.child_begin = static_cast<uint32_t>(ordered.size());
            ordered.insert(ordered.end(), h_.children.begin() + begin,
                           h_.children.begin() + begin + child_spans_[i]);
        }
        h_.children = std::move(ordered);
        child_spans_ = std::vector<uint32_t>();
        std::sort(h_.node_partitions.begin(), h_.node_partitions.end(),
                  [](const NodeValue& a, const NodeValue& b) { return a.node < b.node; });
    }

    // Ett attribut, läst eller taget som rot.
    bool attribute(Frame& f, const Ptr& p) {
        int32_t attr_class = p.cls;
        uint32_t attr_value = p.value;
        if (p.kind == Ptr::Null && !take_root(attr_class, attr_value)) return false;
        switch (attr_class) {
            case 86:
            case 66:
            case 70:
            case 76:
                if (mode_ == Mode::Structure) {
                    ++f.tabs;
                } else if (mode_ == Mode::Full) {
                    tab_stack_.push_back(attr_value);
                } else if (f.region_root) {
                    // En flik före området läses om från sin egen position när
                    // attributen är lästa, alla sådana i strömmens ordning.
                    if (p.kind == Ptr::Back && p.id < f.loc.attr_next_id) {
                        SharedTab s;
                        s.index = tab_stack_.size();
                        s.cls = attr_class;
                        s.id = p.id;
                        s.pos = attr_value;
                        shared_tabs_.push_back(s);
                    }
                    tab_stack_.push_back(attr_value);
                }
                break;
            case 31:
                if (store_nodes_ && f.node.material == kNone) f.node.material = attr_value;
                break;
            case 40:
            case 41:
            case 39:
                if (store_nodes_ && keep_transforms_ && f.transform == kNone) f.transform = attr_value;
                break;
            default: break;
        }
        ++f.i;
        return true;
    }

    bool node_children(Frame& f) {
        ObjectReader& r = *f.r;
        if (f.waiting) {
            f.waiting = false;
            if (!node_child(f, result_)) return false;
        }
        while (f.i < f.n) {
            Ptr p;
            // Under en partition får det sitta partitioner: en fil sammansatt av
            // filer som själva är sammansatta.
            const Slot s = f.cls == 32 ? begin_slot(r, {22, 53, 32}, false, true, false, true, p)
                                       : begin_slot(r, {22, 53}, false, true, false, false, p);
            if (s == Slot::Failed) return false;
            if (s == Slot::Pushed) {
                f.waiting = true;
                return true;
            }
            if (!node_child(f, p)) return false;
        }
        if (store_nodes_ && f.height + 1 > static_cast<uint32_t>(kMaxNesting))
            return err(ErrorCode::LimitExceeded, "expanded tree is higher than the nesting limit");

        if (f.cls == 32) {
            PartitionTail tail;
            if (!read_partition_tail(r, h_.lights, tail)) return false;
            if (store_nodes_)
                h_.node_partitions.push_back(
                    NodeValue{f.index, static_cast<uint32_t>(h_.partitions.size())});
            h_.partitions.push_back(std::move(tail));
        }
        if (!store_nodes_) {
            tab_stack_.resize(f.tab_base);
            return complete(f, 0);
        }

        if (f.expanded > UINT32_MAX)
            return err(ErrorCode::LimitExceeded, "a node has more expanded nodes under it than 2^32 - 1");
        const size_t count = child_stack_.size() - f.child_base;
        if (count > UINT32_MAX - h_.children.size())
            return err(ErrorCode::LimitExceeded, "more child links than an index can address");
        StoredNode& n = h_.nodes[f.index];
        n.expanded = static_cast<uint32_t>(f.expanded);
        n.height = static_cast<uint16_t>(f.height + 1);
        n.child_begin = static_cast<uint32_t>(h_.children.size());
        child_spans_[f.index] = static_cast<uint32_t>(count);
        h_.children.insert(h_.children.end(),
                           child_stack_.begin() + static_cast<std::ptrdiff_t>(f.child_base),
                           child_stack_.end());
        child_stack_.resize(f.child_base);
        return complete(f, f.index);
    }

    bool node_child(Frame& f, const Ptr& p) {
        if (store_nodes_) {
            if (!add_child(p.value, f.expanded, f.height)) return false;
            child_stack_.push_back(p.value);
        }
        ++f.i;
        return true;
    }

    // --- Länkarna i trädet ------------------------------------------------------

    // Klass 79: länkar. De läses och prövas men lagras inte. I trädet kan en
    // länk peka på ett nytt objekt av nästan vilken klass som helst, också en
    // nod, och får därför en egen ram.
    bool link_step(Frame& f) {
        ObjectReader& r = *f.r;
        if (f.phase == Frame::Head) {
            int32_t n;
            if (!r.str(tmp_) || !attr_head(r)) return false;
            // En länk tar minst två strängar, en pekare och punktantalet.
            if (!r.count(16, n)) return false;
            f.n = n;
            f.i = 0;
            f.phase = Frame::Items;
            return true;
        }
        if (f.waiting) {
            f.waiting = false;
            if (!link_points(r)) return false;
            ++f.i;
        }
        while (f.i < f.n) {
            if (!r.str(tmp_) || !r.str(tmp_)) return false;
            Ptr p;
            const Slot s = begin_slot(r, {53, 22, 32, 52, 86, 31, 40, 41, 39, 79, 200, 185}, true,
                                      true, false, false, p);
            if (s == Slot::Failed) return false;
            if (s == Slot::Pushed) {
                f.waiting = true;
                return true;
            }
            if (!link_points(r)) return false;
            ++f.i;
        }
        return complete(f, 0);
    }

    bool link_points(ObjectReader& r) {
        int32_t points;
        if (!r.count(24, points)) return false;
        double d;
        for (int64_t j = 0; j < 3 * static_cast<int64_t>(points); ++j)
            if (!r.f64(d)) return false;
        return true;
    }

    // Klass 79 i en egenskapsström: pekarna tar bara klass 52 eller null.
    bool links_props(ObjectReader& r) {
        int32_t n;
        if (!r.str(tmp_) || !attr_head(r)) return false;
        if (!r.count(16, n)) return false;
        for (int32_t i = 0; i < n; ++i) {
            if (!r.str(tmp_) || !r.str(tmp_)) return false;
            Ptr p;
            if (!inline_slot(r, {52}, true, true, true, p)) return false;
            if (!link_points(r)) return false;
        }
        return true;
    }

    // --- Övriga objekt ------------------------------------------------------------

    bool body(ObjectReader& r, int32_t cls, uint32_t& value) {
        switch (cls) {
            case 52: return name_pair(r, value);
            case 86:
            case 66:
            case 70:
            case 76: return tab_object(r, cls, value);
            case 92: return opaque92(r);
            case 31: return material(r, value);
            case 40:
            case 41:
            case 39: return transform(r, cls, value);
            case 79: return links_props(r);
            case 200: return extra(r);
            case 185: return skip_class185(r);
            default: return r.fail("class has no body reader");
        }
    }

    // En flik (86, 66, 70 eller 76). I trädets ström är kroppen ett eget område
    // för skelettet, så att fliken kan läsas om från sin position om en
    // attributplats pekar bakåt på den; värdet blir då dess plats i den
    // listan.
    bool tab_object(ObjectReader& r, int32_t cls, uint32_t& value) {
        const bool tree_tab = recording_ && &r != props_reader_;
        uint32_t body = 0;
        if (tree_tab) {
            if (!pos32(r.pos(), body)) return false;
            region_starts_.push_back(r.next_id());
        }
        bool ok = false;
        switch (cls) {
            case 86: ok = tab(r, value); break;
            case 66: ok = element_id(r, value); break;
            case 70: ok = text_id(r, value); break;
            default: ok = text_value(r, value); break;
        }
        if (tree_tab) {
            region_starts_.pop_back();
            value = body;
        }
        return ok;
    }

    // En sträng in i poolen; null blir kNone. Utan `keep` läses den bara.
    bool pooled(ObjectReader& r, uint32_t& index, bool keep) {
        bool is_null = false;
        if (!r.str(tmp_, &is_null)) return false;
        index = kNone;
        return is_null || !keep || intern(h_.strings, tmp_, index);
    }

    // `s` in i `pool`. LimitExceeded om poolen är full.
    bool intern(StringPool& pool, std::string_view s, uint32_t& index) {
        if (pool.add(s, index)) return true;
        return err(ErrorCode::LimitExceeded, "the strings of the tree exceed 4 GB");
    }

    // Ett klass-52-objekt, som en ny pekare eller en bakåtpekare. En nod får
    // sakna namnpar (`allow_null`), då blir index kNone; på alla andra platser är
    // pekaren aldrig null.
    bool username(ObjectReader& r, uint32_t& index, bool allow_null = false) {
        Ptr p;
        if (!inline_slot(r, {52}, allow_null, true, false, p)) return false;
        index = p.kind == Ptr::Null ? kNone : p.value;
        return true;
    }

    bool name_pair(ObjectReader& r, uint32_t& value) {
        StoredName n;
        if (!pooled(r, n.display, true) || !pooled(r, n.internal, true)) return false;
        value = static_cast<uint32_t>(h_.names.size());
        h_.names.push_back(n);
        return true;
    }

    // Lägger fliken och dess egenskaper sist i listorna, om flikarna lagras.
    void add_tab(StoredTab& t, const StoredProperty* first, uint32_t count, uint32_t& value) {
        if (!store_tabs_) {
            value = 0;
            return;
        }
        t.prop_begin = static_cast<uint32_t>(h_.properties.size());
        t.prop_count = count;
        h_.properties.insert(h_.properties.end(), first, first + count);
        value = static_cast<uint32_t>(h_.tabs.size());
        h_.tabs.push_back(t);
    }

    // Klass 86: en flik. Dess egenskaper hamnar i följd, eftersom bara klass 52
    // kan ligga inuti den.
    bool tab(ObjectReader& r, uint32_t& value) {
        StoredTab t;
        t.source_class = 86;
        if (!pooled(r, t.name, store_tabs_) || !username(r, t.user) || !r.i32(t.flags)) return false;
        int32_t n;
        // En post tar minst en bakåtpekare på nyckeln och typen.
        if (!r.count(12, n)) return false;
        const uint32_t begin = static_cast<uint32_t>(h_.properties.size());
        for (int32_t i = 0; i < n; ++i) {
            StoredProperty pr;
            if (!username(r, pr.key) || !r.i32(pr.type)) return false;
            if (!property_value(r, pr)) return false;
            if (store_tabs_) h_.properties.push_back(pr);
        }
        if (!store_tabs_) {
            value = 0;
            return true;
        }
        t.prop_begin = begin;
        t.prop_count = static_cast<uint32_t>(n);
        value = static_cast<uint32_t>(h_.tabs.size());
        h_.tabs.push_back(t);
        return true;
    }

    bool property_value(ObjectReader& r, StoredProperty& pr) {
        switch (pr.type) {
            case 0: return true;
            case 1:
            case 6:
            case 7:
            case 10:
            case 11: return r.f64(pr.d[0]);
            case 2:
            case 3: {
                int32_t v;
                if (!r.i32(v)) return false;
                pr.i = v;
                return true;
            }
            case 4:
            case 9: return pooled(r, pr.text, store_tabs_);
            case 5: return r.i64(pr.i);
            case 8: return username(r, pr.text);
            case 12: return r.f64(pr.d[0]) && r.f64(pr.d[1]) && r.f64(pr.d[2]);
            case 13: return r.f64(pr.d[0]) && r.f64(pr.d[1]);
            default: return r.fail("property value type is unknown");
        }
    }

    // Klass 66: elementets id som en flik med en egenskap av typ 2.
    bool element_id(ObjectReader& r, uint32_t& value) {
        bool is_null = false;
        if (!r.str(tmp_, &is_null)) return false;
        if (!is_null) return r.fail("class-66 name is not null");

        StoredTab t;
        t.source_class = 66;
        StoredProperty pr;
        pr.type = 2;
        int64_t id;
        if (!username(r, t.user) || !r.i32(t.flags)) return false;
        if (!r.i64(id)) return false;
        if (id < 1 || id > INT32_MAX) return r.fail("class-66 value is outside 1..2^31-1");

        pr.key = t.user;
        pr.i = id;
        add_tab(t, &pr, 1, value);
        return true;
    }

    // Klass 70: ett id som text, som en flik med en egenskap av typ 4.
    bool text_id(ObjectReader& r, uint32_t& value) {
        bool is_null = false;
        if (!r.str(tmp_, &is_null)) return false;
        if (is_null) return r.fail("class-70 value is null");

        StoredTab t;
        t.source_class = 70;
        StoredProperty pr;
        pr.type = 4;
        if (store_tabs_ && !intern(h_.strings, tmp_, pr.text)) return false;
        if (!username(r, t.user) || !r.i32(t.flags)) return false;

        pr.key = t.user;
        add_tab(t, &pr, 1, value);
        return true;
    }

    // Klass 76: en text under en nyckel, som en flik med en egenskap av typ 4.
    bool text_value(ObjectReader& r, uint32_t& value) {
        bool is_null = false;
        if (!r.str(tmp_, &is_null)) return false;
        if (!is_null) return r.fail("class-76 name is not null");

        StoredTab t;
        t.source_class = 76;
        StoredProperty pr;
        pr.type = 4;
        if (!username(r, t.user) || !r.i32(t.flags)) return false;
        if (t.flags != 0) return r.fail("class-76 flags are not 0");
        if (!r.str(tmp_, &is_null)) return false;
        if (is_null) return r.fail("class-76 value is null");
        if (store_tabs_ && !intern(h_.strings, tmp_, pr.text)) return false;

        pr.key = t.user;
        add_tab(t, &pr, 1, value);
        return true;
    }

    // Klass 92: huvudet, talet 96 och 48 byte. Bara den formen är sedd, och
    // dess innehåll är inte känt: en annan storlek ger Unsupported hellre än
    // en gissning om var objektet slutar. Ingenting lagras.
    bool opaque92(ObjectReader& r) {
        const size_t at = r.pos();
        int32_t size;
        if (!r.str(tmp_) || !attr_head(r) || !r.i32(size)) return false;
        if (size != 96)
            return err(ErrorCode::Unsupported, "class 92 with a size other than 96 is not decoded",
                       at);
        return r.skip_padded(48);
    }

    // Klass 31: namnet in i poolen (null blir en tom sträng, så att "har ett
    // material" skiljs från "har inget"), de femton flyttalen läses och lämnas.
    bool material(ObjectReader& r, uint32_t& value) {
        if (!r.str(tmp_)) return false;
        value = 0;
        if (store_nodes_ && !intern(h_.strings, tmp_, value)) return false;
        if (!attr_head(r)) return false;
        double d;
        for (int i = 0; i < 14; ++i)
            if (!r.f64(d)) return false;
        return true;
    }

    // Det attributen har gemensamt efter sitt namn: ett namnpar, som får
    // saknas, och flaggor. Ingetdera lagras.
    bool attr_head(ObjectReader& r) {
        uint32_t user;
        int32_t flags;
        return username(r, user, true) && r.i32(flags);
    }

    bool transform(ObjectReader& r, int32_t cls, uint32_t& value) {
        StoredTransform t;
        t.source_class = cls;
        if (!r.str(tmp_) || !attr_head(r)) return false;
        if (cls == 39) {
            for (double& v : t.m)
                if (!r.f64(v)) return false;
        }
        for (double& v : t.t)
            if (!r.f64(v)) return false;
        if (cls == 41) {
            for (double& v : t.q)
                if (!r.f64(v)) return false;
        }
        if (cls == 39 && !r.i32(t.reverses)) return false;
        value = 0;
        return !store_nodes_ || !keep_transforms_ || store_transform(t, value);
    }

    // Lägger transformen sist i Hierarchy::transforms med bara de tal klassen
    // har. En rotation som redan står där lagras som dess position.
    bool store_transform(const StoredTransform& t, uint32_t& value) {
        std::vector<double>& v = h_.transform_values;
        if (v.size() > kOffsetMask - 13 || h_.transforms.size() >= kNone - 1)
            return err(ErrorCode::LimitExceeded, "the transforms of the tree exceed 8 GB");
        const uint32_t at = static_cast<uint32_t>(v.size());
        uint32_t form = kForm40;
        if (t.source_class == 39) {
            form = kForm39;
            v.insert(v.end(), t.m, t.m + 9);
            v.insert(v.end(), t.t, t.t + 3);
            v.push_back(static_cast<double>(t.reverses));
        } else if (t.source_class == 41) {
            v.insert(v.end(), t.t, t.t + 3);
            const size_t hash = rotation_hash(t.q);
            const uint32_t found = rotations_.find(
                hash, [&](uint32_t p) { return std::memcmp(&v[p], t.q, sizeof t.q) == 0; });
            if (found != kNone) {
                form = kForm41Shared;
                double ref;
                const uint64_t q_at = found;
                std::memcpy(&ref, &q_at, sizeof ref);
                v.push_back(ref);
            } else {
                form = kForm41;
                v.insert(v.end(), t.q, t.q + 4);
                rotations_.insert(hash, at + 3);
            }
        } else {
            v.insert(v.end(), t.t, t.t + 3);
        }
        value = static_cast<uint32_t>(h_.transforms.size());
        h_.transforms.push_back((form << kFormShift) | at);
        return true;
    }

    static size_t rotation_hash(const double* q) {
        return std::hash<std::string_view>()(
            std::string_view(reinterpret_cast<const char*>(q), 4 * sizeof(double)));
    }

    // Klass 200: en klass 185 bakom en pekare som aldrig är null.
    bool extra(ObjectReader& r) {
        if (!r.str(tmp_) || !attr_head(r)) return false;
        Ptr p;
        return inline_slot(r, {185}, false, true, false, p);
    }

    // --- Egenskapsrötterna ---------------------------------------------------

    bool props_present() const { return streams_ ? streams_->has_props : props_buf_ != nullptr; }
    size_t stream_count() const {
        if (streams_) return streams_->props.size();
        return props_buf_ ? props_buf_->size() : 0;
    }

    // Strömmen som läses nu; läsaren skapas när den behövs. nullptr vid fel.
    ObjectReader* reader() {
        if (props_reader_) return props_reader_;
        if (streams_) {
            Checkpoints* sink = recording_ ? &skeleton_->checkpoints[1 + stream_] : nullptr;
            props_sink_ = sink;
            if (!open_sequential(props_in_, streams_->props[stream_], props_what(stream_), sink, every_))
                return nullptr;
            props_reader_ = props_in_.reader.get();
        } else {
            cur_what_ = props_what(stream_);
            const std::vector<uint8_t>& s = (*props_buf_)[stream_];
            cur_ = std::make_unique<ObjectReader>(s.data(), s.size(), cur_what_.c_str());
            props_reader_ = cur_.get();
        }
        return props_reader_;
    }

    // Antalet strömmar prövas mot huvudet första gången någon rot behövs.
    bool check_roots() {
        if (checked_) return true;
        checked_ = true;
        if (!props_present()) {
            if (unread_ > 0)
                return err(ErrorCode::CorruptLink,
                           "the header names " + std::to_string(unread_) +
                               " property roots, but the file has no LcOaPartitionProps package");
            return true;
        }
        if (stream_count() != h_.root_counts.size())
            return err(ErrorCode::CorruptLink,
                       "LcOaPartitionProps has " + std::to_string(stream_count()) +
                           " streams, but the hierarchy header names " +
                           std::to_string(h_.root_counts.size()));
        return end_streams();
    }

    // Avslutar strömmar vars alla rötter är lästa; en ström utan rötter avslutas
    // när den passeras.
    bool end_streams() {
        while (stream_ < stream_count() && taken_ == h_.root_counts[stream_]) {
            ObjectReader* s = reader();
            if (!s) return false;
            if (!s->end()) return fail_from(*s);
            if (streams_ && !close_sequential(props_in_)) return false;
            h_.non_utf8_strings += s->non_utf8_strings();
            if (streams_) props_done_ += streams_->props[stream_].length;
            ++stream_;
            taken_ = 0;
            props_reader_ = nullptr;
            props_sink_ = nullptr;
            cur_.reset();
            props_in_.close();
            seen_props_.clear();
        }
        return true;
    }

    // Den n-te null-platsen tar den n-te roten.
    bool take_root(int32_t& cls, uint32_t& value) {
        if (mode_ == Mode::Region) return take_root_again(cls, value);
        // Null-platsen lästes alldeles nyss i trädet.
        const size_t slot = slot_at_;
        if (!check_roots()) return false;
        if (stream_ >= stream_count())
            return err(ErrorCode::CorruptLink, "null attribute slot " +
                                                   std::to_string(h_.null_slots + 1) +
                                                   " has no LcOaPartitionProps root");
        ObjectReader* s = reader();
        if (!s) return false;
        if (recording_) {
            RootLocation loc;
            if (!pos32(s->pos(), loc.pos) || !pos32(slot, loc.slot)) return false;
            loc.next_id = s->next_id();
            skeleton_->roots.push_back(loc);
            props_root_id_ = loc.next_id;
        }
        Ptr p;
        if (!inline_slot(*s, {86, 79, 66, 70, 76, 92}, false, false, true, p)) return fail_from(*s);
        cls = p.cls;
        value = p.value;
        ++h_.null_slots;
        ++taken_;
        --unread_;
        return end_streams();
    }

    // Efter svansen ska varje rot ha fyllt en null-plats.
    bool finish_roots() {
        if (!check_roots()) return false;
        if (unread_ > 0)
            return err(ErrorCode::CorruptLink, std::to_string(unread_) +
                                                   " LcOaPartitionProps roots fill no null attribute slot");
        return true;
    }

    // --- Strömmarna i filen ---------------------------------------------------

    // Öppnar strömmen i `span` från början, med läsaren `s.reader`.
    bool open_sequential(Inflated& s, const StreamSpan& span, std::string what, Checkpoints* sink,
                         uint64_t every) {
        s.close();
        s.what = std::move(what);
        s.span = span;
        if (span.offset > size_ || span.length > size_ - span.offset)
            return fail_text(ErrorCode::OutOfRange,
                             s.what + " at byte 0: the stream at file byte " +
                                 std::to_string(span.offset) + " runs past the end of the file");
        // Ett fel i zlib-huvudet kommer fram vid den första läsningen.
        s.z.open(file_ + span.offset, static_cast<size_t>(span.length), streams_->max_stream);
        if (sink) s.z.record_checkpoints(every, sink);
        s.window = std::make_unique<StreamWindow>(s.z, kWindowBytes);
        s.reader = std::make_unique<ObjectReader>(*s.window, s.what.c_str());
        return true;
    }

    // Strömmen är läst till slutet: den ska ta exakt sitt span i filen.
    bool close_sequential(Inflated& s) {
        if (s.z.consumed() != s.span.length)
            return fail_text(ErrorCode::CorruptStream,
                             s.what + " at byte " + std::to_string(s.reader->pos()) +
                                 ": the zlib stream ends at " + std::to_string(s.z.consumed()) +
                                 " of " + std::to_string(s.span.length) + " bytes");
        return true;
    }

    bool report() {
        if (tree_sink_ && !tree_sink_->empty()) {
            tree_done_ = tree_sink_->back().in_bit / 8;
            if (tree_sink_ == &scratch_) scratch_.clear();
        }
        uint64_t done = tree_done_ + props_done_;
        if (props_sink_ && !props_sink_->empty()) done += props_sink_->back().in_bit / 8;
        if (!(*progress_)(std::min(done, total_), total_))
            return err(ErrorCode::Cancelled, "cancelled through the progress function");
        return true;
    }

    // --- Skelettet --------------------------------------------------------------

    // En position som skelettet lagrar. Skelettet har 32 bitar för den.
    bool pos32(size_t pos, uint32_t& out) {
        const uint64_t most = streams_ ? std::min<uint64_t>(streams_->max_position, UINT32_MAX)
                                       : UINT32_MAX;
        if (pos > most)
            return err(ErrorCode::LimitExceeded,
                       "the stream is over 4 GiB unpacked, and TreeContent::Structure stores "
                       "positions in it in 32 bits; TreeContent::Full reads it");
        out = static_cast<uint32_t>(pos);
        return true;
    }

    // En bakåtpekare ut ur det innersta området (en nods attribut, en flik i
    // trädet, en rot) antecknas en gång per id.
    void note_back(const ObjectReader& r, const Ptr& p) {
        if (!recording_) return;
        uint32_t table = 0;
        int32_t start = 0;
        if (&r == props_reader_) {
            table = static_cast<uint32_t>(1 + stream_);
            start = props_root_id_;
        } else {
            if (region_starts_.empty()) return;
            start = region_starts_.back();
        }
        if (p.id >= start) return;
        std::vector<bool>& seen = table == 0 ? seen_tree_ : seen_props_;
        const size_t i = static_cast<size_t>(p.id - kFirstId);
        if (i >= seen.size()) seen.resize(std::max(i + 1, seen.size() * 2), false);
        if (seen[i]) return;
        seen[i] = true;
        table_refs_[table].push_back(IdRef{p.id, p.cls, p.value});
    }

    bool build_skeleton() {
        HierarchySkeleton& sk = *skeleton_;
        sk.first_root.clear();
        uint32_t first = 0;
        for (const int32_t n : h_.root_counts) {
            sk.first_root.push_back(first);
            first += static_cast<uint32_t>(n);
        }
        for (std::vector<IdRef>& list : table_refs_) {
            std::sort(list.begin(), list.end(), [](const IdRef& a, const IdRef& b) { return a.id < b.id; });
            sk.refs.add_stream(list);
            list = std::vector<IdRef>();
        }
        sk.locations.shrink_to_fit();
        sk.roots.shrink_to_fit();
        sk.refs.shrink_to_fit();
        return true;
    }

    // --- En nod läst om ---------------------------------------------------------

    ObjectReader::Resolver resolver(uint32_t table) {
        return [this, table](int32_t id, int32_t& cls, uint32_t& value) {
            return resolve(table, id, cls, value);
        };
    }

    // Ett id före läsarens början, ur skelettets lista. Ett namnpar kopieras in
    // i resultatet.
    bool resolve(uint32_t table, int32_t id, int32_t& cls, uint32_t& value) {
        if (!sk_->refs.find(table, id, cls, value)) return false;
        if (cls == 52) return import_name(value, value);
        return true;
    }

    bool import_name(uint32_t global, uint32_t& local) {
        if (global >= src_->names.size()) return false;
        const auto found = imported_names_.find(global);
        if (found != imported_names_.end()) {
            local = found->second;
            return true;
        }
        const StoredName& g = src_->names[global];
        StoredName n;
        if (g.display != kNone && !intern(h_.strings, src_->strings.get(g.display), n.display)) return false;
        if (g.internal != kNone && !intern(h_.strings, src_->strings.get(g.internal), n.internal))
            return false;
        local = static_cast<uint32_t>(h_.names.size());
        h_.names.push_back(n);
        imported_names_.emplace(global, local);
        return true;
    }

    // Flikarna före området som nodens attribut pekar bakåt på, var och en läst
    // från sin egen position i trädets ström, i stigande position så att
    // uppackningen bara går framåt. En flik som förekommer två gånger läses en gång.
    bool import_shared_tabs() {
        std::sort(shared_tabs_.begin(), shared_tabs_.end(),
                  [](const SharedTab& a, const SharedTab& b) { return a.pos < b.pos; });
        for (size_t k = 0; k < shared_tabs_.size(); ++k) {
            const SharedTab& s = shared_tabs_[k];
            if (k > 0 && shared_tabs_[k - 1].id == s.id) {
                tab_stack_[s.index] = tab_stack_[shared_tabs_[k - 1].index];
                continue;
            }
            ErrorCode code = ErrorCode::Ok;
            std::string why;
            ObjectReader* r = source_->open(2, 0, s.pos, s.id + 1, resolver(0), code, why);
            if (!r) return fail_text(code, why);
            uint32_t local = 0;
            bool ok = false;
            switch (s.cls) {
                case 86: ok = tab(*r, local); break;
                case 66: ok = element_id(*r, local); break;
                case 70: ok = text_id(*r, local); break;
                default: ok = text_value(*r, local); break;
            }
            if (!ok) return fail_from(*r);
            tab_stack_[s.index] = local;
        }
        return true;
    }

    // Nästa rot i ordningen, ur sin ström. En rot som börjar där den förra
    // slutade läses vidare med samma läsare.
    bool take_root_again(int32_t& cls, uint32_t& value) {
        const size_t index = region_next_root_++;
        if (index >= sk_->roots.size() || sk_->first_root.empty())
            return err(ErrorCode::CorruptLink, "null attribute slot has no property root in the skeleton");
        const RootLocation& loc = sk_->roots[index];
        // Strömmen: den sista vars första rot är högst index.
        const auto it = std::upper_bound(sk_->first_root.begin(), sk_->first_root.end(),
                                         static_cast<uint32_t>(index));
        const uint32_t stream = static_cast<uint32_t>(it - sk_->first_root.begin()) - 1;

        ObjectReader* s = props_reader_;
        if (!s || stream != props_stream_ || !s->ok() || s->pos() != loc.pos ||
            s->next_id() != loc.next_id) {
            ErrorCode code = ErrorCode::Ok;
            std::string why;
            s = source_->open(1, 1 + stream, loc.pos, loc.next_id, resolver(1 + stream), code, why);
            if (!s) {
                props_reader_ = nullptr;
                return fail_text(code, why);
            }
            props_reader_ = s;
            props_stream_ = stream;
        }
        Ptr p;
        if (!inline_slot(*s, {86, 79, 66, 70, 76, 92}, false, false, true, p)) return fail_from(*s);
        cls = p.cls;
        value = p.value;
        return true;
    }

    // --- Felen ------------------------------------------------------------------

    ErrorCode result(bool ok, std::string& detail) {
        if (ok) return ErrorCode::Ok;
        if (err_ == ErrorCode::Ok) {
            err_ = main_->ok() ? ErrorCode::CorruptObject : main_->code();
            det_ = main_->detail();
        }
        detail = det_;
        return err_;
    }

    // Fel som inte kommer ur en läsare: gränser, partitioner och länkar. Byten är
    // trädströmmens aktuella position, eller `at` om den ges.
    bool err(ErrorCode code, const std::string& rule, size_t at) {
        return fail_text(code, std::string(kWhat) + " at byte " + std::to_string(at) + ": " + rule);
    }
    bool err(ErrorCode code, const std::string& rule) {
        return err(code, rule, main_ ? main_->pos() : 0);
    }

    bool fail_text(ErrorCode code, const std::string& text) {
        if (err_ == ErrorCode::Ok) {
            err_ = code;
            det_ = text;
        }
        return false;
    }

    // Felet i en annan läsare tas över som det är.
    bool fail_from(const ObjectReader& r) {
        if (err_ == ErrorCode::Ok) {
            err_ = r.ok() ? ErrorCode::CorruptObject : r.code();
            det_ = r.detail();
        }
        return false;
    }

    Hierarchy& h_;
    const Mode mode_;
    const bool store_nodes_;   // noder, material och placeringar lagras
    const bool store_tabs_;    // flikar, egenskaper och deras strängar lagras
    const bool keep_transforms_;   // placeringarna lagras
    uint32_t guid_count_;
    uint64_t max_nodes_;
    ObjectReader* main_ = nullptr;
    ErrorCode err_ = ErrorCode::Ok;
    std::string det_;
    std::string tmp_;
    std::vector<Frame> frames_;
    Ptr result_;
    std::vector<uint32_t> tab_stack_;
    std::vector<uint32_t> child_stack_;
    std::vector<uint32_t> child_spans_;   // per lagrad nod: antalet barn, till order_children()
    DedupeTable rotations_;               // klass 41: positionen för varje rotation i transform_values

    // Egenskapsrötterna.
    const PropStreams* props_buf_ = nullptr;
    uint64_t unread_ = 0;
    bool checked_ = false;
    size_t stream_ = 0;
    int32_t taken_ = 0;
    ObjectReader* props_reader_ = nullptr;   // läsaren i den aktuella egenskapsströmmen
    std::unique_ptr<ObjectReader> cur_;
    std::string cur_what_;

    // Filen.
    const uint8_t* file_ = nullptr;
    size_t size_ = 0;
    const HierarchyStreams* streams_ = nullptr;
    Inflated tree_;
    Inflated props_in_;
    uint64_t every_ = kCheckpointEvery;

    // Förloppet.
    const HierarchyProgress* progress_ = nullptr;
    uint32_t ticks_ = 0;
    uint64_t total_ = 0;
    uint64_t tree_done_ = 0;
    uint64_t props_done_ = 0;
    Checkpoints scratch_;
    Checkpoints* tree_sink_ = nullptr;
    Checkpoints* props_sink_ = nullptr;

    // Skelettet.
    HierarchySkeleton* skeleton_ = nullptr;
    bool recording_ = false;
    std::vector<int32_t> region_starts_;   // trädets områden, det innersta sist
    int32_t props_root_id_ = 0;            // id för roten som läses i egenskapsströmmen
    // Bakåtpekarna ut ur ett område per ström, medan tolkningen pågår.
    std::vector<std::vector<IdRef>> table_refs_;
    std::vector<bool> seen_tree_;
    std::vector<bool> seen_props_;

    // En nod läst om.
    const Hierarchy* src_ = nullptr;
    const HierarchySkeleton* sk_ = nullptr;
    ObjectSource* source_ = nullptr;
    size_t region_next_root_ = 0;
    size_t slot_at_ = 0;   // var den senaste pekarplatsen började
    uint32_t props_stream_ = 0;
    std::unordered_map<uint32_t, uint32_t> imported_names_;
    // En flik före området som en attributplats pekar på: platsen i tab_stack_,
    // klassen, id och kroppens position.
    struct SharedTab {
        size_t index;
        int32_t cls;
        int32_t id;
        uint32_t pos;
    };
    std::vector<SharedTab> shared_tabs_;
};

} // namespace

ErrorCode parse_hierarchy(const uint8_t* data, size_t size, const PropStreams* props,
                          uint32_t guid_count, uint64_t max_nodes, Hierarchy& out,
                          std::string* detail) {
    out = Hierarchy{};
    if (detail) detail->clear();

    std::string text;
    Parser parser(Mode::Full, out, guid_count, max_nodes);
    const ErrorCode e = parser.run_buffer(data, size, props, text);
    if (e != ErrorCode::Ok) {
        out = Hierarchy{};
        if (detail) *detail = text;
    }
    return e;
}

ErrorCode locate_hierarchy(const uint8_t* file, size_t size, const DbCacheEntry& tree,
                           const DbCacheEntry* props, uint64_t max_stream, HierarchyStreams& out,
                           std::string* detail) {
    out = HierarchyStreams{};
    out.max_stream = max_stream;
    if (detail) detail->clear();
    const std::string at = std::string(tree.leaf()) + " at byte " + std::to_string(tree.offset);
    if (tree.flags != 1 || tree.a != 0 || tree.b != 0) {
        if (detail)
            *detail = at + ": expected flags 1 and form (0, 0), found flags " +
                      std::to_string(tree.flags) + " and form (" + std::to_string(tree.a) + ", " +
                      std::to_string(tree.b) + ")";
        return ErrorCode::CorruptPackage;
    }
    if (last_stream(tree, size, out.tree) != ErrorCode::Ok) {
        if (detail) *detail = at + ": the package lies outside the file";
        return ErrorCode::OutOfRange;
    }
    if (props) {
        const size_t cap = static_cast<size_t>(std::min<uint64_t>(max_stream, SIZE_MAX));
        if (const ErrorCode e = locate_partition_props(file, size, *props, cap, out.props, detail);
            e != ErrorCode::Ok) {
            out = HierarchyStreams{};
            return e;
        }
        out.has_props = true;
    }
    return ErrorCode::Ok;
}

ErrorCode parse_hierarchy(const uint8_t* file, size_t size, const HierarchyStreams& streams,
                          uint32_t guid_count, uint64_t max_nodes, HierarchyContent content,
                          Hierarchy& out, HierarchySkeleton* skeleton,
                          const HierarchyProgress& progress, std::string* detail,
                          uint64_t checkpoint_every, bool transforms) {
    out = Hierarchy{};
    if (skeleton) *skeleton = HierarchySkeleton{};
    if (detail) detail->clear();

    std::string text;
    Parser parser(content == HierarchyContent::Full ? Mode::Full : Mode::Structure, out, guid_count,
                  max_nodes, transforms);
    const ErrorCode e = parser.run_file(file, size, streams,
                                        content == HierarchyContent::Structure ? skeleton : nullptr,
                                        progress, checkpoint_every, text);
    if (e != ErrorCode::Ok) {
        out = Hierarchy{};
        if (skeleton) *skeleton = HierarchySkeleton{};
        if (detail) *detail = text;
    }
    return e;
}

ErrorCode parse_node_tabs(const Hierarchy& h, const HierarchySkeleton& skeleton, uint32_t node,
                          ObjectSource& source, NodeTabs& out, std::string* detail) {
    out = NodeTabs{};
    if (detail) detail->clear();
    if (node >= h.nodes.size() || node >= skeleton.locations.size()) {
        if (detail)
            *detail = std::string(kWhat) + ": node " + std::to_string(node) + " is not among the " +
                      std::to_string(std::min(h.nodes.size(), skeleton.locations.size())) +
                      " stored nodes of the skeleton";
        return ErrorCode::OutOfRange;
    }

    Hierarchy scratch;
    std::string text;
    Parser parser(Mode::Region, scratch, UINT32_MAX, UINT64_MAX);
    const ErrorCode e = parser.run_region(h, skeleton, node, source, text);
    if (e != ErrorCode::Ok) {
        if (detail) *detail = text;
        return e;
    }
    out.strings = std::move(scratch.strings);
    out.names = std::move(scratch.names);
    out.properties = std::move(scratch.properties);
    out.tabs.reserve(parser.region_tabs().size());
    for (const uint32_t t : parser.region_tabs()) out.tabs.push_back(scratch.tabs[t]);
    return ErrorCode::Ok;
}

} // namespace HNwd::detail
