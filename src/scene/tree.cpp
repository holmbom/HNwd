// Document: trädet. Läser objektträdet, expanderar det i preorder och svarar
// på noder, flikar, egenskaper och modeller. Resten av Document ligger i
// document.cpp.

#include "document_impl.h"

#include <new>

#include "inflate.h"
#include "partition.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace HNwd {

namespace {

// Ger Impl för ett Document, och skapar den om dokumentet har flyttats ifrån.
template <class T>
T& ensure(std::unique_ptr<T>& p) {
    if (!p) p = std::make_unique<T>();
    return *p;
}

// Visningsnamnet och det interna namnet i ett namnpar (index i `names`).
// Tomma om indexet saknas.
std::string_view display_of(const detail::StringPool& strings,
                            const std::vector<detail::StoredName>& names, uint32_t pair) {
    return pair < names.size() ? strings.get(names[pair].display) : std::string_view();
}

std::string_view internal_of(const detail::StringPool& strings,
                             const std::vector<detail::StoredName>& names, uint32_t pair) {
    return pair < names.size() ? strings.get(names[pair].internal) : std::string_view();
}

// Indexet i Hierarchy::nodes för en förekomst. kNone för roten och för ett
// index utanför.
uint32_t stored_index(const TreeData& t, uint32_t index) {
    if (index == 0 || index >= t.occurrences.size()) return kNone;
    const uint32_t stored = t.occurrences.stored(index);
    return stored < t.hierarchy.nodes.size() ? stored : kNone;
}

// Den lagrade noden bakom en förekomst. nullptr för roten och för ett index
// utanför.
const detail::StoredNode* stored_of(const TreeData& t, uint32_t index) {
    const uint32_t stored = stored_index(t, index);
    return stored != kNone ? &t.hierarchy.nodes[stored] : nullptr;
}

uint32_t child_count_of(const TreeData& t, uint32_t index) {
    if (index == 0) return static_cast<uint32_t>(t.hierarchy.top.size());
    const uint32_t stored = stored_index(t, index);
    return stored != kNone ? t.hierarchy.child_count(stored) : 0;
}

uint32_t subtree_of(const TreeData& t, uint32_t index) {
    if (index == 0) return static_cast<uint32_t>(t.occurrences.size());
    const detail::StoredNode* n = stored_of(t, index);
    return n ? static_cast<uint32_t>(n->expanded) : 1;
}

// Nodens namn: det egna, annars visningsnamnet.
std::string_view name_of(const TreeData& t, const detail::StoredNode& n) {
    const detail::Hierarchy& h = t.hierarchy;
    return n.name != kNone ? h.strings.get(n.name) : display_of(h.strings, h.names, n.user);
}

// Nodens eget material, annars närmaste förfaders. Tom om ingen har något.
std::string_view material_of(const TreeData& t, uint32_t index) {
    for (uint32_t i = index; i != 0 && i < t.occurrences.size(); i = t.occurrences.parent(i)) {
        const detail::StoredNode* n = stored_of(t, i);
        if (n && n->material != kNone) return t.hierarchy.strings.get(n->material);
    }
    return {};
}

// Expanderar trädet i preorder: roten, sedan toppnoderna i ordning med sina
// delträd. En lagrad nod som sitter på flera ställen får en förekomst per
// ställe. Explicit stack, ingen rekursion.
//
// Ger false och en text om antalet inte blir det parsern räknade, eller om en
// länk pekar utanför sin lista.
bool expand(const detail::Hierarchy& h, Occurrences& out, std::string& why) {
    struct Frame {
        const uint32_t* next;   // nästa barn att lägga till
        const uint32_t* end;
        uint32_t occurrence;    // förekomsten som barnen hör till
    };

    out = Occurrences();
    out.reserve(static_cast<size_t>(h.expanded_total));
    out.push(kNone, kNone);   // roten

    std::vector<Frame> stack;
    stack.push_back({h.top.data(), h.top.data() + h.top.size(), 0});
    while (!stack.empty()) {
        Frame& frame = stack.back();
        if (frame.next == frame.end) {
            stack.pop_back();
            continue;
        }
        const uint32_t stored = *frame.next++;
        const uint32_t parent = frame.occurrence;
        if (stored >= h.nodes.size()) {
            why = "LcOpNwdLogicalHierarchy: internal error: a child link points at node " +
                  std::to_string(stored) + " of " + std::to_string(h.nodes.size());
            return false;
        }
        if (out.size() >= h.expanded_total) {
            why = "LcOpNwdLogicalHierarchy: internal error: the expansion exceeds the " +
                  std::to_string(h.expanded_total) + " nodes the parser counted";
            return false;
        }
        const uint32_t occurrence = static_cast<uint32_t>(out.size());
        out.push(stored, parent);

        const detail::StoredNode& n = h.nodes[stored];
        const uint32_t count = h.child_count(stored);
        if (count == 0) continue;
        if (n.child_begin > h.children.size() || count > h.children.size() - n.child_begin) {
            why = "LcOpNwdLogicalHierarchy: internal error: the children of node " +
                  std::to_string(stored) + " lie outside the child list";
            return false;
        }
        const uint32_t* first = h.children.data() + n.child_begin;
        stack.push_back({first, first + count, occurrence});   // `frame` gäller inte längre
    }

    if (out.size() != h.expanded_total) {
        why = "LcOpNwdLogicalHierarchy: internal error: the expansion has " +
              std::to_string(out.size()) + " nodes but the parser counted " +
              std::to_string(h.expanded_total);
        return false;
    }
    out.finish();
    return true;
}

// Modellerna: toppnoderna av klass 32 i trädets ordning. Har trädet ingen
// sådan och `own` inte är nullptr blir filens egen partition en modell med
// roten som nod. Ger false och en text om en nod av klass 32 saknar sin svans.
bool find_models(TreeData& t, std::string& why) {
    const detail::Hierarchy& h = t.hierarchy;
    t.models.clear();
    for (uint32_t i = 1; i < t.occurrences.size(); i += subtree_of(t, i)) {
        const detail::StoredNode* n = stored_of(t, i);
        if (!n) {
            why = "LcOpNwdLogicalHierarchy: internal error: top node " + std::to_string(i) +
                  " has no stored node";
            return false;
        }
        if (n->cls != 32) continue;
        const uint32_t partition = h.partition(t.occurrences.stored(i));
        if (partition >= h.partitions.size()) {
            why = "LcOpNwdLogicalHierarchy: internal error: the class 32 node at index " +
                  std::to_string(i) + " has no partition tail";
            return false;
        }
        const detail::PartitionTail& tail = h.partitions[partition];
        ModelEntry m;
        m.node = i;
        m.name = name_of(t, *n);
        m.source = tail.source_path;
        m.loader = tail.loader;
        m.unit_code = tail.unit_code;
        t.models.push_back(m);
    }
    if (t.models.empty() && t.has_own_partition) {
        const detail::Partition& p = t.own_partition;
        ModelEntry m;
        m.node = 0;
        m.name = p.name.empty() && p.has_user_name ? std::string_view(p.user_display)
                                                   : std::string_view(p.name);
        m.source = p.tail.source_path;
        m.loader = p.tail.loader;
        m.unit_code = p.tail.unit_code;
        t.models.push_back(m);
    }
    return true;
}

// Värdetypen i filen som publik typ, och värdet i `out`.
void fill_value(const detail::StringPool& strings, const std::vector<detail::StoredName>& names,
                const detail::StoredProperty& p, Property& out) {
    switch (p.type) {
    case 1:
    case 6:
    case 7:
    case 10:
    case 11:
        out.type = ValueType::Double;
        out.d[0] = p.d[0];
        break;
    case 2:
    case 3:
        out.type = ValueType::Int;
        out.i = p.i;
        break;
    case 4:
    case 9:
        out.type = ValueType::String;
        out.text = strings.get(p.text);
        break;
    case 5:
        out.type = ValueType::Time;
        out.i = p.i;
        break;
    case 8:
        out.type = ValueType::Name;
        out.text = display_of(strings, names, p.text);
        break;
    case 12:
        out.type = ValueType::Point3;
        out.d[0] = p.d[0];
        out.d[1] = p.d[1];
        out.d[2] = p.d[2];
        break;
    case 13:
        out.type = ValueType::Point2;
        out.d[0] = p.d[0];
        out.d[1] = p.d[1];
        break;
    default:
        out.type = ValueType::None;
        break;
    }
}

// Släpper det vektorerna växte med under tolkningen: trädet hålls länge, och
// utan det kostar en vektor upp till dubbla sin storlek.
void shrink(TreeData& t) {
    detail::Hierarchy& h = t.hierarchy;
    h.strings.shrink_to_fit();
    h.names.shrink_to_fit();
    h.properties.shrink_to_fit();
    h.tabs.shrink_to_fit();
    h.tab_begin.shrink_to_fit();
    h.transforms.shrink_to_fit();
    h.transform_values.shrink_to_fit();
    h.node_transforms.shrink_to_fit();
    h.node_flags.shrink_to_fit();
    h.node_partitions.shrink_to_fit();
    h.nodes.shrink_to_fit();
    h.node_tabs.shrink_to_fit();
    h.children.shrink_to_fit();
    h.top.shrink_to_fit();
    h.partitions.shrink_to_fit();
    h.lights.shrink_to_fit();
    detail::HierarchySkeleton& sk = t.skeleton;
    sk.locations.shrink_to_fit();
    sk.roots.shrink_to_fit();
    sk.first_root.shrink_to_fit();
    sk.refs.shrink_to_fit();
    for (std::vector<detail::InflateStream::Checkpoint>& list : sk.checkpoints) list.shrink_to_fit();
}

// Det publicerade trädet, eller nullptr om inget är läst. `impl` är ett
// Documents impl_. Ett anrop läser pekaren en gång och svarar ur det trädet,
// också om load_tree() i en annan tråd publicerar ett nytt under tiden.
template <class ImplPtr>
const TreeData* tree_of(const ImplPtr& impl) {
    return impl ? impl->tree_view.load() : nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// Delat med PropertyReader.

void to_tab(const detail::StringPool& strings, const std::vector<detail::StoredName>& names,
            const detail::StoredTab& t, Tab& out) {
    Tab result;
    result.internal = internal_of(strings, names, t.user);
    result.name = t.name != kNone ? strings.get(t.name) : display_of(strings, names, t.user);
    result.property_count = t.prop_count;
    out = result;
}

void to_property(const detail::StringPool& strings, const std::vector<detail::StoredName>& names,
                 const detail::StoredProperty& p, Property& out) {
    Property result;
    result.name = display_of(strings, names, p.key);
    result.internal = internal_of(strings, names, p.key);
    result.raw_type = p.type;
    fill_value(strings, names, p, result);
    out = result;
}

// ---------------------------------------------------------------------------
// Förekomsterna.

uint32_t Occurrences::stored(uint32_t index) const {
    if (index == 0 || index >= parent_.size()) return kNone;
    if (!stored_.empty()) return stored_[index];
    // Den sista serien som börjar på eller före förekomsten.
    const auto it = std::upper_bound(runs_.begin(), runs_.end(), index,
                                     [](uint32_t i, const Run& r) { return i < r.first; });
    if (it == runs_.begin()) return kNone;
    const Run& r = *(it - 1);
    return r.stored + (index - r.first);
}

void Occurrences::push(uint32_t stored, uint32_t parent) {
    const uint32_t index = static_cast<uint32_t>(parent_.size());
    parent_.push_back(parent);
    if (index == 0) return;   // roten
    if (runs_.empty() || runs_.back().stored + (index - runs_.back().first) != stored)
        runs_.push_back(Run{index, stored});
}

void Occurrences::finish() {
    parent_.shrink_to_fit();
    if (runs_.size() * sizeof(Run) <= parent_.size() * sizeof(uint32_t)) {
        runs_.shrink_to_fit();
        return;
    }
    std::vector<uint32_t> list(parent_.size(), kNone);
    for (uint32_t i = 1; i < list.size(); ++i) list[i] = stored(i);
    stored_ = std::move(list);
    runs_ = std::vector<Run>();
}

size_t TreeData::memory_bytes() const {
    size_t n = sizeof(TreeData) + hierarchy.memory_bytes() +
               occurrences.memory_bytes() +
               streams.props.capacity() * sizeof(detail::StreamSpan) + skeleton.memory_bytes() +
               models.capacity() * sizeof(ModelEntry);
    if (has_own_partition) {
        const detail::Partition& p = own_partition;
        n += p.name.capacity() + p.user_display.capacity() + p.user_internal.capacity() +
             p.lights.capacity() * sizeof(detail::PartitionLight) +
             p.tail.light_refs.capacity() * sizeof(uint32_t) + p.tail.loader.capacity() +
             p.tail.label.capacity() + p.tail.source_path.capacity();
    }
    return n;
}

// ---------------------------------------------------------------------------
// Läsningen.

Status Document::load_tree() { return load_tree(TreeContent::Full, {}); }

Status Document::load_tree(TreeContent content, const Progress& progress) try {
    Impl& s = ensure(impl_);
    if (!is_open()) return s.fail(Status::NotLoaded, "no file is open");
    if (content != TreeContent::Full && content != TreeContent::Structure)
        return s.fail(Status::OutOfRange, "the tree content is neither Full nor Structure");
    // Full har allt som Structure har.
    if (s.tree && (s.tree->content == TreeContent::Full || content == TreeContent::Structure))
        return s.ok();
    if (!s.hierarchy_entry)
        return s.fail(detail::ErrorCode::PackageMissing,
                      "the file has no LcOpNwdLogicalHierarchy package");

    // Trädets elementindex prövas mot GUID-tabellen. Utan paketet är antalet 0.
    uint32_t guid_count = 0;
    if (s.guids_entry) {
        if (const Status g = load_guids(); g != Status::Ok) return g;
        guid_count = static_cast<uint32_t>(
            s.guids.size() < UINT32_MAX ? s.guids.size() : UINT32_MAX - 1);
    }

    const bool structure = content == TreeContent::Structure;
    auto tree = std::make_shared<TreeData>();
    tree->content = content;
    {
        std::string why;
        detail::HierarchyStreams streams;
        detail::ErrorCode ec =
            detail::locate_hierarchy(s.file->data(), s.file->size(), *s.hierarchy_entry,
                                     s.props_entry, s.options.max_stream_bytes, streams, &why);
        if (ec != detail::ErrorCode::Ok) return s.fail(ec, why);

        // Strömmarna packas upp medan de tolkas och släpps efter hand.
        // Placeringarna lämnas inte ut genom Document, så de lagras inte.
        ec = detail::parse_hierarchy(
            s.file->data(), s.file->size(), streams, guid_count, s.options.max_tree_nodes,
            structure ? detail::HierarchyContent::Structure : detail::HierarchyContent::Full,
            tree->hierarchy, structure ? &tree->skeleton : nullptr, progress, &why,
            detail::kCheckpointEvery, false);
        if (ec != detail::ErrorCode::Ok) return s.fail(ec, why);
        if (structure) tree->streams = std::move(streams);
    }
    shrink(*tree);

    const detail::Hierarchy& hierarchy = tree->hierarchy;
    if (hierarchy.expanded_total >= kNone)
        return s.fail(detail::ErrorCode::LimitExceeded,
                      "LcOpNwdLogicalHierarchy: the expanded tree has " +
                          std::to_string(hierarchy.expanded_total) +
                          " nodes, more than a node index can address");

    {
        std::string why;
        if (!expand(hierarchy, tree->occurrences, why)) return s.fail(Status::Corrupt, why.c_str());
    }

    // Utan partitioner i trädet är filens egen partition den enda modellen.
    bool any_partition = false;
    for (const uint32_t top : hierarchy.top)
        any_partition = any_partition || (top < hierarchy.nodes.size() && hierarchy.nodes[top].cls == 32);
    if (!any_partition && s.partition_entry) {
        std::vector<uint8_t> bytes;
        std::string why;
        detail::ErrorCode ec =
            unpack_package(*s.file, *s.partition_entry, s.options.max_stream_bytes, bytes, why);
        if (ec != detail::ErrorCode::Ok) return s.fail(ec, why);
        ec = detail::parse_partition_stream(bytes.data(), bytes.size(), tree->own_partition, &why);
        if (ec != detail::ErrorCode::Ok) return s.fail(ec, why);
        tree->has_own_partition = true;
    }
    {
        std::string why;
        if (!find_models(*tree, why)) return s.fail(Status::Corrupt, why.c_str());
    }

    // Är fragmenten redan lästa är det trädet som kommer sist. Går det inte
    // ihop står det tidigare trädet kvar.
    if (s.fragments)
        if (const Status t = s.check_seq_leaves(*s.fragments, *tree); t != Status::Ok) return t;

    // Publiceringen. Platsen för det tidigare trädet reserveras först, så att
    // ingenting efter den kan misslyckas; trädet är färdigt och ändras inte
    // mer när pekaren publiceras.
    if (s.tree) s.retired_trees.reserve(s.retired_trees.size() + 1);
    const TreeData* view = tree.get();
    if (s.tree) s.retired_trees.push_back(std::move(s.tree));
    s.tree = std::move(tree);
    s.tree_view.store(view);
    return s.ok();
} catch (const std::bad_alloc&) {
    return out_of_memory(ensure(impl_));
}

// ---------------------------------------------------------------------------
// Noderna.

bool Document::has_tree() const { return tree_of(impl_) != nullptr; }

bool Document::has_tabs() const {
    const TreeData* t = tree_of(impl_);
    return t && t->content == TreeContent::Full;
}

size_t Document::node_count() const {
    const TreeData* t = tree_of(impl_);
    return t ? t->occurrences.size() : 0;
}

bool Document::node(uint32_t index, Node& out) const {
    const TreeData* t = tree_of(impl_);
    if (!t || index >= t->occurrences.size()) return false;
    const detail::Hierarchy& h = t->hierarchy;

    Node n;
    n.child_count = child_count_of(*t, index);
    n.subtree_size = subtree_of(*t, index);
    if (index == 0) {
        n.kind = NodeKind::Root;
        out = n;
        return true;
    }

    const detail::StoredNode* stored = stored_of(*t, index);
    if (!stored) return false;
    n.parent = t->occurrences.parent(index);
    n.element = stored->element;
    switch (stored->cls) {
    case 32: n.kind = NodeKind::Partition; break;
    case 22: n.kind = NodeKind::Leaf; break;
    default: n.kind = NodeKind::Group; break;
    }
    n.display = display_of(h.strings, h.names, stored->user);
    n.internal = internal_of(h.strings, h.names, stored->user);
    n.name = name_of(*t, *stored);
    n.material = material_of(*t, index);
    out = n;
    return true;
}

uint32_t Document::first_child(uint32_t index) const {
    const TreeData* t = tree_of(impl_);
    if (!t || index >= t->occurrences.size()) return kNone;
    return child_count_of(*t, index) > 0 ? index + 1 : kNone;
}

uint32_t Document::next_sibling(uint32_t index) const {
    const TreeData* t = tree_of(impl_);
    if (!t || index == 0 || index >= t->occurrences.size()) return kNone;
    const uint64_t next = static_cast<uint64_t>(index) + subtree_of(*t, index);
    if (next >= t->occurrences.size() ||
        t->occurrences.parent(static_cast<uint32_t>(next)) != t->occurrences.parent(index))
        return kNone;
    return static_cast<uint32_t>(next);
}

uint32_t Document::parent(uint32_t index) const {
    const TreeData* t = tree_of(impl_);
    if (!t || index == 0 || index >= t->occurrences.size()) return kNone;
    return t->occurrences.parent(index);
}

uint32_t Document::node_of_fragment(size_t fragment) const {
    const TreeData* t = tree_of(impl_);
    if (!t || !impl_->fragments || fragment >= impl_->fragments->size()) return kNone;
    // load_fragments() och load_tree() har prövat att seq är ett löv i trädet.
    return static_cast<uint32_t>(impl_->fragments->items[fragment].seq);
}

// ---------------------------------------------------------------------------
// Flikarna och egenskaperna.

size_t Document::tab_count(uint32_t node) const {
    const TreeData* t = tree_of(impl_);
    if (!t || t->content != TreeContent::Full) return 0;
    const uint32_t stored = stored_index(*t, node);
    return stored != kNone ? t->hierarchy.tab_count(stored) : 0;
}

bool Document::tab(uint32_t node, size_t tab, Tab& out) const {
    const TreeData* t = tree_of(impl_);
    if (!t || t->content != TreeContent::Full) return false;
    const detail::Hierarchy& h = t->hierarchy;
    const uint32_t stored = stored_index(*t, node);
    if (stored == kNone || tab >= h.tab_count(stored)) return false;
    to_tab(h.strings, h.names, h.tabs[h.node_tabs[h.tab_begin[stored] + tab]], out);
    return true;
}

bool Document::property(uint32_t node, size_t tab, size_t property, Property& out) const {
    const TreeData* t = tree_of(impl_);
    if (!t || t->content != TreeContent::Full) return false;
    const detail::Hierarchy& h = t->hierarchy;
    const uint32_t stored = stored_index(*t, node);
    if (stored == kNone || tab >= h.tab_count(stored)) return false;

    const detail::StoredTab& st = h.tabs[h.node_tabs[h.tab_begin[stored] + tab]];
    if (property >= st.prop_count) return false;
    to_property(h.strings, h.names, h.properties[st.prop_begin + property], out);
    return true;
}

// ---------------------------------------------------------------------------
// Modellerna.

size_t Document::model_count() const {
    const TreeData* t = tree_of(impl_);
    return t && impl_->fragments ? t->models.size() : 0;
}

bool Document::model(size_t i, Model& out) const {
    const TreeData* t = tree_of(impl_);
    if (!t || !impl_->fragments || i >= t->models.size()) return false;
    const ModelEntry& m = t->models[i];
    const std::vector<detail::FragmentStore::Item>& items = impl_->fragments->items;

    // seq avtar aldrig (load_fragments()), så delträdets fragment ligger i
    // följd: från det första med seq >= noden till det första bortom delträdet.
    const int64_t begin = m.node;
    const int64_t end = begin + subtree_of(*t, m.node);
    const auto by_seq = [](const detail::FragmentStore::Item& it, int64_t seq) {
        return it.seq < seq;
    };
    const auto first = std::lower_bound(items.begin(), items.end(), begin, by_seq);
    const auto last = std::lower_bound(first, items.end(), end, by_seq);
    const size_t first_index = static_cast<size_t>(first - items.begin());
    const size_t count = static_cast<size_t>(last - first);
    if (first_index > UINT32_MAX || count > UINT32_MAX - first_index) return false;

    Model result;
    result.name = m.name;
    result.source = m.source;
    result.loader = m.loader;
    result.unit = to_unit(m.unit_code);
    result.node = m.node;
    result.first_fragment = static_cast<uint32_t>(first_index);
    result.fragment_count = static_cast<uint32_t>(count);
    out = result;
    return true;
}

} // namespace HNwd
