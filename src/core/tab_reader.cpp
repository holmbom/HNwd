#include "tab_reader.h"

#include "byte_reader.h"
#include "inflate.h"

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace HNwd::detail {

namespace {

constexpr const char* kTreeWhat = "LcOpNwdLogicalHierarchy";

// Fönstret för en omläsning börjar tomt och växer bara med det som läses, så
// att uppackningen står strax efter det som lästs när läsaren släpps.
constexpr size_t kWindowBytes = 1;

constexpr int kCursors = 3;

// Uppackningar per läsare. Den som används för en läsning hamnar efter det
// lästa; de andra står kvar där tidigare läsningar slutade, och en läsning
// strax efter kan fortsätta med en av dem i stället för från en kontrollpunkt.
constexpr int kInflaters = 3;

} // namespace

class TabReader::Source : public ObjectSource {
public:
    Source(const uint8_t* file, size_t size, const HierarchyStreams& streams,
           const HierarchySkeleton& skeleton)
        : file_(file), size_(size), streams_(streams), skeleton_(skeleton) {}

    ObjectReader* open(int cursor, uint32_t stream, uint64_t pos, int32_t next_id,
                       ObjectReader::Resolver resolve, ErrorCode& code,
                       std::string& detail) override {
        if (cursor < 0 || cursor >= kCursors) {
            code = ErrorCode::OutOfRange;
            detail = std::string(kTreeWhat) + ": reader " + std::to_string(cursor) + " does not exist";
            return nullptr;
        }
        Cursor& c = cursors_[cursor];
        release(c);
        if (stream > streams_.props.size()) {
            code = ErrorCode::OutOfRange;
            detail = std::string(kTreeWhat) + ": stream " + std::to_string(stream) +
                     " is not among the " + std::to_string(1 + streams_.props.size()) +
                     " streams of the tree";
            return nullptr;
        }
        const StreamSpan& span = stream == 0 ? streams_.tree : streams_.props[stream - 1];
        c.what = stream == 0 ? std::string(kTreeWhat)
                             : "LcOaPartitionProps stream " + std::to_string(stream - 1);
        const std::string at = c.what + " at byte " + std::to_string(pos) + ": ";
        if (span.offset > size_ || span.length > size_ - span.offset) {
            code = ErrorCode::OutOfRange;
            detail = at + "the stream at file byte " + std::to_string(span.offset) +
                     " runs past the end of the file";
            return nullptr;
        }

        // Närmaste kontrollpunkt på eller före positionen.
        const InflateStream::Checkpoint* cp = nullptr;
        if (stream < skeleton_.checkpoints.size()) {
            const std::vector<InflateStream::Checkpoint>& list = skeleton_.checkpoints[stream];
            const auto it = std::upper_bound(
                list.begin(), list.end(), pos,
                [](uint64_t p, const InflateStream::Checkpoint& k) { return p < k.out_pos; });
            if (it != list.begin()) cp = &*(it - 1);
        }
        const uint64_t from = cp ? cp->out_pos : 0;

        // Den uppackning i strömmen som står närmast före positionen, om den står
        // efter kontrollpunkten; annars den som står längst bort (eller inte
        // används), om från kontrollpunkten.
        int best = -1;
        for (int k = 0; k < kInflaters; ++k) {
            const Inflater& f = c.inflaters[k];
            if (!f.z || !f.healthy || f.stream != stream) continue;
            const uint64_t p = f.z->position();
            if (p > pos || p < from) continue;
            if (best < 0 || p > c.inflaters[best].z->position()) best = k;
        }
        const bool restart = best < 0;
        if (restart) {
            best = 0;
            for (int k = 0; k < kInflaters; ++k) {
                const Inflater& f = c.inflaters[k];
                if (!f.z || !f.healthy) {
                    best = k;
                    break;
                }
                if (f.used < c.inflaters[best].used) best = k;
            }
            Inflater& f = c.inflaters[best];
            if (!f.z) f.z = std::make_unique<InflateStream>();
            f.healthy = false;
            ErrorCode e = f.z->open(file_ + span.offset, static_cast<size_t>(span.length),
                                    streams_.max_stream);
            if (e != ErrorCode::Ok) {
                code = e;
                detail = at + "the zlib stream cannot be opened: " + error_message(e);
                return nullptr;
            }
            if (cp) {
                e = f.z->resume(*cp);
                if (e != ErrorCode::Ok) {
                    code = e;
                    detail = at + "checkpoint at output byte " + std::to_string(cp->out_pos) +
                             " does not belong to the stream";
                    return nullptr;
                }
            }
            f.stream = stream;
        }

        Inflater& f = c.inflaters[best];
        f.used = ++clock_;
        std::string why;
        const uint64_t before = f.z->position();
        if (const ErrorCode e = f.z->skip(pos - before, &why); e != ErrorCode::Ok) {
            inflated_ += f.z->position() - before;
            f.healthy = false;
            code = e;
            detail = at + why;
            return nullptr;
        }
        f.healthy = true;
        c.active = best;
        c.start = before;
        ++opens_[cursor];
        if (restart) ++restarts_;
        c.window = std::make_unique<StreamWindow>(*f.z, kWindowBytes);
        c.reader = std::make_unique<ObjectReader>(*c.window, c.what.c_str());
        c.reader->continue_ids(next_id, std::move(resolve));
        return c.reader.get();
    }

    // Släpper läsarna; uppackningarna står kvar där de var.
    void release_all() {
        for (Cursor& c : cursors_) release(c);
    }

    uint64_t opens(int cursor) const { return opens_[cursor]; }
    uint64_t restarts() const { return restarts_; }
    uint64_t inflated() const { return inflated_; }

private:
    struct Inflater {
        std::unique_ptr<InflateStream> z;   // skapas när den behövs
        uint32_t stream = 0;
        bool healthy = false;               // kan läsas vidare
        uint64_t used = 0;                  // när den senast användes
    };

    struct Cursor {
        Inflater inflaters[kInflaters];
        int active = -1;
        uint64_t start = 0;   // där den aktiva uppackningen stod när läsaren öppnades
        std::unique_ptr<StreamWindow> window;
        std::unique_ptr<ObjectReader> reader;
        std::string what;
    };

    // Läsaren släpps; vad uppackningen gått fram sedan öppningen räknas in.
    void release(Cursor& c) {
        if (c.active >= 0) {
            Inflater& f = c.inflaters[c.active];
            inflated_ += f.z->position() - c.start;
            if (c.window && c.window->error() != ErrorCode::Ok) f.healthy = false;
        }
        c.active = -1;
        c.reader.reset();
        c.window.reset();
    }

    const uint8_t* file_;
    size_t size_;
    const HierarchyStreams& streams_;
    const HierarchySkeleton& skeleton_;
    Cursor cursors_[kCursors];
    uint64_t clock_ = 0;
    uint64_t opens_[kCursors] = {};
    uint64_t restarts_ = 0;
    uint64_t inflated_ = 0;
};

// Det läsaren minns: tolkade flikar ur trädets ström som flera noder delar
// (nyckel: flikens id) och flikarna för noder som lästs med `keep` (nyckel: den
// lagrade nodens index). Platserna ligger i en vektor och en länkad lista
// genom dem håller ordningen, så att den som användes längst sedan kastas
// först när summan över taket.
class TabReader::Cache : public SharedTabCache {
public:
    explicit Cache(size_t cap) : cap_(cap) {}

    const NodeTabs* find_tab(int32_t id) override {
        const NodeTabs* t = find(key(kTab, static_cast<uint32_t>(id)));
        if (t) ++tab_hits_;
        return t;
    }
    void keep_tab(int32_t id, NodeTabs&& tab) override {
        put(key(kTab, static_cast<uint32_t>(id)), std::move(tab));
    }

    const NodeTabs* find_node(uint32_t node) { return find(key(kNode, node)); }
    void keep_node(uint32_t node, const NodeTabs& tabs) { put(key(kNode, node), NodeTabs(tabs)); }

    size_t bytes() const { return bytes_; }
    uint64_t tab_hits() const { return tab_hits_; }

private:
    static constexpr uint32_t kNoSlot = UINT32_MAX;
    static constexpr uint64_t kTab = 0, kNode = 1;
    struct Slot {
        uint64_t ident = 0;
        NodeTabs value;
        size_t bytes = 0;
        uint32_t older = kNoSlot;
        uint32_t newer = kNoSlot;
    };

    // Vad en post tar utöver sina listor: platsen, och dess rad i tabellen
    // med sitt fack.
    static constexpr size_t kEntryOverhead = sizeof(Slot) + 48;

    // Slaget i de höga 32 bitarna, värdet i de låga.
    static uint64_t key(uint64_t kind, uint32_t value) { return (kind << 32) | value; }

    // Posten med nyckeln, flyttad sist i ordningen, eller nullptr.
    const NodeTabs* find(uint64_t k) {
        const auto it = slot_of_.find(k);
        if (it == slot_of_.end()) return nullptr;
        touch(it->second);
        return &slots_[it->second].value;
    }

    void put(uint64_t k, NodeTabs&& value) {
        const size_t cost = value.memory_bytes() + kEntryOverhead;
        if (cost > cap_) return;
        const auto it = slot_of_.find(k);
        if (it != slot_of_.end()) {
            touch(it->second);
            return;
        }
        uint32_t s;
        if (!free_.empty()) {
            s = free_.back();
            free_.pop_back();
        } else {
            slots_.emplace_back();
            s = static_cast<uint32_t>(slots_.size() - 1);
        }
        Slot& slot = slots_[s];
        slot.ident = k;
        slot.value = std::move(value);
        slot.bytes = cost;
        bytes_ += cost;
        slot_of_.emplace(k, s);
        touch(s);
        evict();
    }

    void unlink(uint32_t s) {
        Slot& slot = slots_[s];
        if (slot.older != kNoSlot) slots_[slot.older].newer = slot.newer;
        else if (oldest_ == s) oldest_ = slot.newer;
        if (slot.newer != kNoSlot) slots_[slot.newer].older = slot.older;
        else if (newest_ == s) newest_ = slot.older;
        slot.older = kNoSlot;
        slot.newer = kNoSlot;
    }

    void touch(uint32_t s) {
        if (newest_ == s) return;
        unlink(s);
        Slot& slot = slots_[s];
        slot.older = newest_;
        if (newest_ != kNoSlot) slots_[newest_].newer = s;
        newest_ = s;
        if (oldest_ == kNoSlot) oldest_ = s;
    }

    // Kastar de som användes längst sedan tills summan ryms under taket.
    void evict() {
        while (bytes_ > cap_ && oldest_ != kNoSlot) {
            const uint32_t s = oldest_;
            Slot& slot = slots_[s];
            unlink(s);
            bytes_ -= slot.bytes;
            slot_of_.erase(slot.ident);
            slot.value = NodeTabs{};
            slot.bytes = 0;
            free_.push_back(s);
        }
    }

    size_t cap_;
    size_t bytes_ = 0;
    uint64_t tab_hits_ = 0;
    std::vector<Slot> slots_;
    std::vector<uint32_t> free_;                      // lediga platser i slots_
    std::unordered_map<uint64_t, uint32_t> slot_of_;   // nyckel -> plats
    uint32_t oldest_ = kNoSlot;
    uint32_t newest_ = kNoSlot;
};

TabReader::TabReader(const uint8_t* file, size_t size, const HierarchyStreams& streams,
                     const Hierarchy& h, const HierarchySkeleton& skeleton, size_t cache_bytes)
    : source_(std::make_unique<Source>(file, size, streams, skeleton)),
      cache_(cache_bytes > 0 ? std::make_unique<Cache>(cache_bytes) : nullptr), h_(h),
      skeleton_(skeleton) {}

TabReader::~TabReader() = default;

ErrorCode TabReader::read(uint32_t stored_node, NodeTabs& out, std::string* detail, bool keep) {
    if (cache_) {
        if (const NodeTabs* hit = cache_->find_node(stored_node)) {
            out = *hit;
            if (detail) detail->clear();
            ++reads_;
            ++node_hits_;
            return ErrorCode::Ok;
        }
    }
    const ErrorCode e =
        parse_node_tabs(h_, skeleton_, stored_node, *source_, out, detail, cache_.get());
    source_->release_all();
    if (e != ErrorCode::Ok) {
        out = NodeTabs{};
        return e;
    }
    ++reads_;
    if (keep && cache_) cache_->keep_node(stored_node, out);
    return e;
}

size_t TabReader::cached_bytes() const { return cache_ ? cache_->bytes() : 0; }

TabReaderStats TabReader::stats() const {
    TabReaderStats s;
    s.reads = reads_;
    s.node_hits = node_hits_;
    s.tab_hits = cache_ ? cache_->tab_hits() : 0;
    for (int k = 0; k < kCursors; ++k) s.opens[k] = source_->opens(k);
    s.restarts = source_->restarts();
    s.inflated = source_->inflated();
    return s;
}

ErrorCode read_node_tabs(const uint8_t* file, size_t size, const HierarchyStreams& streams,
                         const Hierarchy& h, const HierarchySkeleton& skeleton,
                         uint32_t stored_node, NodeTabs& out, std::string* detail) {
    TabReader reader(file, size, streams, h, skeleton);
    return reader.read(stored_node, out, detail);
}

} // namespace HNwd::detail
