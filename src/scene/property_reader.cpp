// PropertyReader: nodernas flikar och egenskaper för en tråd. Ur dokumentets
// minne när trädet är läst med TreeContent::Full, och ur filen genom
// TabReader när det är läst med Structure.

#include "document_impl.h"

#include <new>

#include "tab_reader.h"

#include <memory>
#include <string>
#include <utility>

namespace HNwd {

struct PropertyReader::Impl {
    // TabReader pekar in i filen och trädet, så den ligger efter dem och
    // förstörs först.
    std::shared_ptr<const detail::MappedFile> file;
    std::shared_ptr<const TreeData> tree;
    std::unique_ptr<detail::TabReader> tabs;   // bara i Structure
    detail::NodeTabs node_tabs;                // den senast lästa nodens flikar, i Structure
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

// De `tab_count` flikarna, med sina egenskaper ur `properties`, till `out`.
// `tab_at(j)` ger flik j. false om en fliks egenskaper ligger utanför
// `properties`.
template <class TabAt>
bool fill(const detail::StringPool& strings, const std::vector<detail::StoredName>& names,
          const std::vector<detail::StoredProperty>& properties, size_t tab_count, TabAt tab_at,
          NodeProperties& out) {
    size_t total = 0;
    for (size_t j = 0; j < tab_count; ++j) {
        const detail::StoredTab& t = tab_at(j);
        if (t.prop_begin > properties.size() || t.prop_count > properties.size() - t.prop_begin)
            return false;
        total += t.prop_count;
    }
    out.tabs.resize(tab_count);
    out.properties.resize(total);
    size_t k = 0;
    for (size_t j = 0; j < tab_count; ++j) {
        const detail::StoredTab& t = tab_at(j);
        to_tab(strings, names, t, out.tabs[j]);
        for (uint32_t p = 0; p < t.prop_count; ++p)
            to_property(strings, names, properties[t.prop_begin + p], out.properties[k++]);
    }
    return true;
}

} // namespace

PropertyReader::PropertyReader() : impl_(std::make_unique<Impl>()) {}
PropertyReader::~PropertyReader() = default;
PropertyReader::PropertyReader(PropertyReader&&) noexcept = default;
PropertyReader& PropertyReader::operator=(PropertyReader&&) noexcept = default;

void PropertyReader::close() {
    if (impl_) *impl_ = Impl{};
}

bool PropertyReader::is_open() const { return impl_ && impl_->tree != nullptr; }

const char* PropertyReader::last_error() const { return impl_ ? impl_->last_error.c_str() : ""; }

Status PropertyReader::open(const Document& doc) try {
    Impl& s = ensure(impl_);
    s = Impl{};
    const Document::Impl* d = doc.impl_.get();
    // Det publicerade trädet; ägarskapet tas ur trädet självt, så att
    // läsaren kan öppnas medan load_tree() går i en annan tråd.
    const TreeData* published = d ? d->tree_view.load() : nullptr;
    if (!d || !d->is_open() || !published)
        return s.fail(Status::NotLoaded, "the document's tree has not been loaded: call load_tree()");
    s.file = d->file;
    s.tree = published->shared_from_this();
    if (s.tree->content == TreeContent::Structure)
        s.tabs = std::make_unique<detail::TabReader>(s.file->data(), s.file->size(), s.tree->streams,
                                                     s.tree->hierarchy, s.tree->skeleton);
    return s.ok();
} catch (const std::bad_alloc&) {
    Impl& s = ensure(impl_);
    s = Impl{};
    return out_of_memory(s);
}

Status PropertyReader::read(uint32_t node, NodeProperties& out) try {
    Impl& s = ensure(impl_);
    out.tabs.clear();
    out.properties.clear();
    if (!s.tree) return s.fail(Status::NotLoaded, "the reader is not open");
    const TreeData& t = *s.tree;
    if (node >= t.occurrences.size())
        return s.fail(Status::OutOfRange, ("node " + std::to_string(node) + " is outside 0.." +
                                           std::to_string(t.occurrences.size()))
                                              .c_str());
    if (node == 0) return s.ok();   // roten har inga flikar

    const uint32_t stored = t.occurrences.stored(node);
    const detail::Hierarchy& h = t.hierarchy;
    if (stored >= h.nodes.size())
        return s.fail(Status::Corrupt,
                      ("internal error: node " + std::to_string(node) + " has no stored node").c_str());

    bool filled = false;
    if (t.content == TreeContent::Full) {
        const uint32_t begin = h.tab_begin[stored];
        filled = fill(
            h.strings, h.names, h.properties, h.tab_count(stored),
            [&](size_t j) -> const detail::StoredTab& { return h.tabs[h.node_tabs[begin + j]]; },
            out);
    } else {
        std::string why;
        const detail::ErrorCode ec = s.tabs->read(stored, s.node_tabs, &why);
        if (ec != detail::ErrorCode::Ok) return s.fail(ec, why);
        const detail::NodeTabs& nt = s.node_tabs;
        filled = fill(nt.strings, nt.names, nt.properties, nt.tabs.size(),
                      [&](size_t j) -> const detail::StoredTab& { return nt.tabs[j]; }, out);
    }
    if (!filled) {
        out.tabs.clear();
        out.properties.clear();
        return s.fail(Status::Corrupt, ("LcOpNwdLogicalHierarchy: internal error: the properties of "
                                        "node " + std::to_string(node) +
                                        " lie outside their list")
                                           .c_str());
    }
    return s.ok();
} catch (const std::bad_alloc&) {
    out.tabs.clear();
    out.properties.clear();
    return out_of_memory(ensure(impl_));
}

} // namespace HNwd
