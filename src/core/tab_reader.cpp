#include "tab_reader.h"

#include "byte_reader.h"
#include "inflate.h"

#include <algorithm>
#include <memory>
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
        if (best < 0) {
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
        if (const ErrorCode e = f.z->skip(pos - f.z->position(), &why); e != ErrorCode::Ok) {
            f.healthy = false;
            code = e;
            detail = at + why;
            return nullptr;
        }
        f.healthy = true;
        c.active = best;
        c.window = std::make_unique<StreamWindow>(*f.z, kWindowBytes);
        c.reader = std::make_unique<ObjectReader>(*c.window, c.what.c_str());
        c.reader->continue_ids(next_id, std::move(resolve));
        return c.reader.get();
    }

    // Släpper läsarna; uppackningarna står kvar där de var.
    void release_all() {
        for (Cursor& c : cursors_) release(c);
    }

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
        std::unique_ptr<StreamWindow> window;
        std::unique_ptr<ObjectReader> reader;
        std::string what;
    };

    static void release(Cursor& c) {
        if (c.active >= 0 && c.window && c.window->error() != ErrorCode::Ok)
            c.inflaters[c.active].healthy = false;
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
};

TabReader::TabReader(const uint8_t* file, size_t size, const HierarchyStreams& streams,
                     const Hierarchy& h, const HierarchySkeleton& skeleton)
    : source_(std::make_unique<Source>(file, size, streams, skeleton)), h_(h), skeleton_(skeleton) {}

TabReader::~TabReader() = default;

ErrorCode TabReader::read(uint32_t stored_node, NodeTabs& out, std::string* detail) {
    const ErrorCode e = parse_node_tabs(h_, skeleton_, stored_node, *source_, out, detail);
    source_->release_all();
    if (e != ErrorCode::Ok) out = NodeTabs{};
    return e;
}

ErrorCode read_node_tabs(const uint8_t* file, size_t size, const HierarchyStreams& streams,
                         const Hierarchy& h, const HierarchySkeleton& skeleton,
                         uint32_t stored_node, NodeTabs& out, std::string* detail) {
    TabReader reader(file, size, streams, h, skeleton);
    return reader.read(stored_node, out, detail);
}

} // namespace HNwd::detail
