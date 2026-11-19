#include "spatial.h"

#include "object_reader.h"

#include <cstring>
#include <utility>

namespace HNwd::detail {

namespace {

constexpr const char* kPackage = "LcOpNwdSpatialHierarchy";

constexpr int32_t kTagInner = 5;
constexpr int32_t kTagLeaf = 2;
constexpr int32_t kTagSingle = 4;
constexpr int32_t kTagItem = 1;

// Den minsta nod som finns: [4][f].
constexpr size_t kMinNodeBytes = 8;
// En post i ett löv: [1][f].
constexpr size_t kItemBytes = 8;

class Parser {
public:
    Parser(const uint8_t* data, size_t size, uint32_t fragment_count, SpatialTree& tree,
           std::string* detail)
        : data_(data), size_(size), fragment_count_(fragment_count), tree_(tree),
          detail_(detail), seen_(fragment_count, false) {}

    ErrorCode run() {
        // Varje nod tar minst 8 byte, så antalet noder är högst size / 8.
        if (size_ / kMinNodeBytes >= UINT32_MAX)
            return fail(ErrorCode::LimitExceeded, 0, "the stream holds more nodes than an index can address");
        const size_t most = size_ / kItemBytes;
        tree_.items.reserve(fragment_count_ < most ? fragment_count_ : most);

        if (const ErrorCode e = node(UINT32_MAX, 0); e != ErrorCode::Ok) return e;
        while (!stack_.empty()) {
            Frame& f = stack_.back();
            SpatialTree::Node& n = tree_.nodes[f.node];
            if (f.next == n.child_count) {
                n.item_count = static_cast<uint32_t>(tree_.items.size()) - n.item_begin;
                stack_.pop_back();
                continue;
            }
            const uint32_t parent = f.node;
            const uint32_t slot = n.child_begin + f.next++;
            --promised_;   // barnet läses nu
            tree_.children[slot] = static_cast<uint32_t>(tree_.nodes.size());
            // node() kan lägga till en ram: `f` och `n` gäller inte efter den.
            if (const ErrorCode e = node(parent, static_cast<uint32_t>(stack_.size()));
                e != ErrorCode::Ok)
                return e;
        }

        if (pos_ != size_)
            return fail(ErrorCode::CorruptPackage, pos_,
                        std::to_string(size_ - pos_) + " byte after the root node");
        if (tree_.items.size() != fragment_count_) {
            uint32_t missing = 0;
            while (missing < fragment_count_ && seen_[missing]) ++missing;
            return fail(ErrorCode::CorruptLink, pos_,
                        "fragment " + std::to_string(missing) + " of " +
                            std::to_string(fragment_count_) + " is in no leaf");
        }
        return ErrorCode::Ok;
    }

private:
    struct Frame {
        uint32_t node;   // index i nodes
        uint32_t next;   // nästa barn att läsa
    };

    ErrorCode fail(ErrorCode code, size_t at, const std::string& rule) {
        if (detail_) *detail_ = std::string(kPackage) + " at byte " + std::to_string(at) + ": " + rule;
        return code;
    }

    bool read(int32_t& v) {
        if (size_ - pos_ < 4) return false;
        std::memcpy(&v, data_ + pos_, 4);
        pos_ += 4;
        return true;
    }

    ErrorCode truncated() { return fail(ErrorCode::CorruptPackage, pos_, "the stream ends inside a node"); }

    // Ryms `count` poster om `bytes` byte var, utöver de barn som redan har
    // en plats men inte är lästa, i resten av strömmen?
    bool fits(int32_t count, size_t bytes) const {
        const size_t room = (size_ - pos_) / bytes;
        return promised_ <= room && static_cast<size_t>(count) <= room - promised_;
    }

    // Ett fragmentindex ur ett löv, läst vid `at`.
    ErrorCode item(int32_t f, size_t at) {
        if (f < 0 || static_cast<uint32_t>(f) >= fragment_count_)
            return fail(ErrorCode::CorruptLink, at,
                        "fragment index " + std::to_string(f) + " is outside 0.." +
                            std::to_string(fragment_count_) + "-1");
        if (seen_[static_cast<uint32_t>(f)])
            return fail(ErrorCode::CorruptLink, at,
                        "fragment " + std::to_string(f) + " stands in more than one place");
        seen_[static_cast<uint32_t>(f)] = true;
        tree_.items.push_back(static_cast<uint32_t>(f));
        return ErrorCode::Ok;
    }

    // Läser en nod vid pos_ och lägger den sist i nodes. En inre nod får sina
    // barnplatser i `children` och en ram på stacken; ett löv läses färdigt.
    ErrorCode node(uint32_t parent, uint32_t depth) {
        const size_t at = pos_;
        if (depth > static_cast<uint32_t>(kMaxNesting))
            return fail(ErrorCode::LimitExceeded, at,
                        "node depth exceeds " + std::to_string(kMaxNesting));
        int32_t tag;
        if (!read(tag)) return truncated();

        SpatialTree::Node n;
        n.parent = parent;
        n.item_begin = static_cast<uint32_t>(tree_.items.size());
        const uint32_t index = static_cast<uint32_t>(tree_.nodes.size());

        if (tag == kTagInner) {
            int32_t k;
            if (!read(k)) return truncated();
            if (k <= 0)
                return fail(ErrorCode::CorruptPackage, at + 4,
                            "inner node child count " + std::to_string(k) + " is not positive");
            // Varje barn tar minst kMinNodeBytes, också de barn till andra
            // noder som ännu inte är lästa: barnplatserna blir sammanlagt
            // aldrig fler än strömmen har plats för.
            if (!fits(k, kMinNodeBytes))
                return fail(ErrorCode::CorruptPackage, at + 4,
                            "inner node child count " + std::to_string(k) +
                                " does not fit in the rest of the stream");
            n.child_begin = static_cast<uint32_t>(tree_.children.size());
            n.child_count = static_cast<uint32_t>(k);
            tree_.children.resize(tree_.children.size() + static_cast<size_t>(k), UINT32_MAX);
            promised_ += static_cast<size_t>(k);
            tree_.nodes.push_back(n);
            stack_.push_back({index, 0});
            return ErrorCode::Ok;
        }

        if (tag == kTagLeaf) {
            int32_t count;
            if (!read(count)) return truncated();
            if (count <= 0)
                return fail(ErrorCode::CorruptPackage, at + 4,
                            "leaf entry count " + std::to_string(count) + " is not positive");
            if (!fits(count, kItemBytes))
                return fail(ErrorCode::CorruptPackage, at + 4,
                            "leaf entry count " + std::to_string(count) +
                                " does not fit in the rest of the stream");
            for (int32_t i = 0; i < count; ++i) {
                const size_t entry = pos_;
                // Antalet är prövat mot resten av strömmen: båda talen finns.
                int32_t item_tag = 0, f = 0;
                read(item_tag);
                read(f);
                if (item_tag != kTagItem)
                    return fail(ErrorCode::CorruptPackage, entry,
                                "leaf entry tag " + std::to_string(item_tag) + " is not 1");
                if (const ErrorCode e = item(f, entry + 4); e != ErrorCode::Ok) return e;
            }
            n.item_count = static_cast<uint32_t>(count);
            tree_.nodes.push_back(n);
            return ErrorCode::Ok;
        }

        if (tag == kTagSingle) {
            int32_t f;
            if (!read(f)) return truncated();
            if (const ErrorCode e = item(f, at + 4); e != ErrorCode::Ok) return e;
            n.item_count = 1;
            tree_.nodes.push_back(n);
            return ErrorCode::Ok;
        }

        return fail(ErrorCode::CorruptPackage, at, "node tag " + std::to_string(tag) + " is not 5, 2 or 4");
    }

    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
    uint32_t fragment_count_;
    SpatialTree& tree_;
    std::string* detail_;
    std::vector<bool> seen_;     // fragment som redan står i ett löv
    std::vector<Frame> stack_;   // de inre noder vars barn läses
    size_t promised_ = 0;        // barnplatser i `children` som ännu inte är lästa
};

} // namespace

ErrorCode parse_spatial_hierarchy(const uint8_t* data, size_t size, uint32_t fragment_count,
                                  SpatialTree& out, std::string* detail) {
    out = SpatialTree{};
    if (detail) detail->clear();
    SpatialTree tree;
    const ErrorCode e = Parser(data, size, fragment_count, tree, detail).run();
    if (e == ErrorCode::Ok) out = std::move(tree);
    return e;
}

} // namespace HNwd::detail
