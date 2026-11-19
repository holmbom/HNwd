#pragma once

// Det ett Document håller. Delas av document.cpp (allt utom trädet och
// indexet), tree.cpp (trädet), spatial_index.cpp (det rumsliga indexet),
// geometry_reader.cpp (geometrin per läsare), property_reader.cpp
// (flikarna per läsare) och elements_api.cpp (de små paketen).

#include "HNwd.h"

#include "container.h"
#include "dbcache.h"
#include "elements.h"
#include "fragments.h"
#include "geometry.h"
#include "geometry_segment.h"
#include "guid_store.h"
#include "hierarchy.h"
#include "mapped_file.h"
#include "material.h"
#include "nwd_error.h"
#include "partition.h"
#include "pose.h"
#include "source_header.h"
#include "spatial.h"

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace HNwd {

// Den Status ett fel ur core blir:
//
//   NotLcUStream, CorruptHeader                          NotNavisworks
//   CorruptDirectory, CorruptStream, CorruptObject,
//   CorruptPackage, CorruptGeometry, CorruptLink         Corrupt
//   StreamTooLarge, LimitExceeded                        LimitExceeded
//   PackageMissing                                       Missing
//   övriga                                               koden med samma namn
Status to_status(detail::ErrorCode code);

// "<paket> at byte <n>", där n är paketets offset i filen.
std::string at_byte(const detail::DbCacheEntry& entry);

// Packar upp ett paket av formen (0, 0): flags 1 och en zlib-ström på hela
// längden. `why` får texten vid fel.
detail::ErrorCode unpack_package(const detail::MappedFile& file,
                                 const detail::DbCacheEntry& entry, size_t max_stream,
                                 std::vector<uint8_t>& out, std::string& why);

// Minnet tog slut mitt i ett anrop. Ger LimitExceeded med en text. Det som
// anropet höll på att läsa kan vara halvfärdigt, så dokumentet ska stängas.
template <class ImplT>
Status out_of_memory(ImplT& impl) {
    return impl.fail(Status::LimitExceeded, "out of memory; close the document");
}

// Den publika posen ur och till kärnans.
detail::PoseD to_pose_d(const Pose& pose);
Pose to_pose(const detail::PoseD& pose);

// En enhetskod ur en partitionssvans som publik Unit: koden i `code`, och
// LengthUnit::Unknown med metres_per_unit 0 för en kod som inte är känd.
Unit to_unit(int32_t code);

// En tolkad post som publik Mesh.
void to_mesh(const detail::RecordData& rd, Mesh& out);

// Ett material ur kärnans form som publikt Material. Vyerna och pekaren pekar
// in i `m`.
void to_material(const detail::MaterialData& m, Material& out);

// En sparad vy som load_views() håller: kameran och var vyns markeringar står.
struct SavedViewData {
    Camera camera;
    uint32_t redline_begin = 0;   // i ViewsData::redlines
    uint32_t redline_count = 0;
};

// Det load_views() har läst. Vyernas fält som inte lämnas ut (avatarblocket,
// heltalen och de 680 sista byten) töms när paketet är läst. Av de sparade
// vyerna hålls bara det som lämnas ut: posternas huvud, en kamera per vy och
// markeringarna; klass 89 och vyernas övriga fält släpps.
struct ViewsData {
    bool has_current = false, has_home = false;
    detail::ViewElement current, home;
    std::vector<detail::SavedViews::Item> items;   // Item::view är index i `views`
    std::vector<SavedViewData> views;
    std::vector<detail::Redline> redlines;
    std::vector<uint32_t> next_sibling;            // per post i `items`

    size_t memory_bytes() const;
};

// Tolkar fragmentets post genom `cursor` och prövar att postens låda och
// antal primitiver är fragmentets. Lämnar posten i `rd` och som Mesh i `out`,
// med triangelordningen vänd om posen speglar. `why` får texten vid fel,
// inledd med fragmentets nummer. Används av Document och GeometryReader.
detail::ErrorCode read_fragment_mesh(const detail::FragmentStore& store, size_t fragment,
                                     detail::GeometryCursor& cursor, detail::RecordData& rd,
                                     Mesh& out, std::string& why);

// Det expanderade trädet i preorder: en förekomst per plats, [0] är roten. En
// lagrad nod som sitter på flera ställen har en förekomst per ställe.
//
// Föräldern står per förekomst. Den lagrade noden står i serier: från seriens
// första förekomst och framåt växer den lagrade noden med ett för varje
// förekomst. Noderna är numrerade i den ordning tolkningen når dem, så ett träd
// utan delade delträd blir en enda serie. Skulle serierna ta mer plats än en
// lagrad nod per förekomst står den så i stället.
class Occurrences {
public:
    size_t size() const { return parent_.size(); }
    uint32_t parent(uint32_t index) const { return parent_[index]; }   // kNone för roten
    uint32_t stored(uint32_t index) const;                              // kNone för roten
    void reserve(size_t n) { parent_.reserve(n); }
    void push(uint32_t stored, uint32_t parent);
    // Sist: släpper det som växte med, och väljer serierna eller en lagrad nod
    // per förekomst efter vad som tar minst plats.
    void finish();
    size_t memory_bytes() const {
        return parent_.capacity() * sizeof(uint32_t) + runs_.capacity() * sizeof(Run) +
               stored_.capacity() * sizeof(uint32_t);
    }

private:
    struct Run {
        uint32_t first;    // förekomsten där serien börjar
        uint32_t stored;   // dess lagrade nod
    };
    std::vector<uint32_t> parent_;
    std::vector<Run> runs_;
    std::vector<uint32_t> stored_;   // per förekomst, när det tar mindre plats än serierna
};

// En modell i trädet (se Document::model()). Vyerna pekar in i samma TreeData.
struct ModelEntry {
    uint32_t node = 0;   // förekomsten: en toppnod, eller roten
    std::string_view name, source, loader;
    int32_t unit_code = 0;
};

// Det load_tree() har läst. Oföränderligt när det är publicerat; delas med
// PropertyReader, som håller det vid liv efter close(). Skapas med
// make_shared, så att shared_from_this() ger ägarskap ur den publicerade
// pekaren (se Document::Impl::tree_view).
struct TreeData : std::enable_shared_from_this<TreeData> {
    TreeContent content = TreeContent::Full;
    detail::Hierarchy hierarchy;
    Occurrences occurrences;
    // Bara i Structure: var strömmarna ligger och skelettet, som flikarna
    // läses om genom.
    detail::HierarchyStreams streams;
    detail::HierarchySkeleton skeleton;
    // Filens egen partition, när modellen tas ur den.
    bool has_own_partition = false;
    detail::Partition own_partition;
    std::vector<ModelEntry> models;

    size_t memory_bytes() const;
};

// Flikar och egenskaper ur kärnans form till den publika. `strings` och `names`
// är de som fliken och egenskapen pekar in i (ett Hierarchy eller ett NodeTabs).
void to_tab(const detail::StringPool& strings, const std::vector<detail::StoredName>& names,
            const detail::StoredTab& tab, Tab& out);
void to_property(const detail::StringPool& strings, const std::vector<detail::StoredName>& names,
                 const detail::StoredProperty& property, Property& out);

// Det rumsliga indexet, ur filen eller byggt. Oföränderligt när det är byggt.
struct SpatialIndex {
    detail::SpatialTree tree;          // noderna i preorder, barnen och fragmenten i lövordning
    std::vector<BoxD> bounds;          // per nod
    std::vector<uint64_t> primitives;  // per nod
    bool from_file = false;

    size_t memory_bytes() const;
};

// Bygger indexet för `store` (se Document::load_spatial). `spatial` är filens
// paket eller nullptr. `max_stream` är taket för den uppackade strömmen.
// Avbrott genom `progress` ger Cancelled. `why` får texten vid fel.
detail::ErrorCode build_spatial_index(const detail::MappedFile& file,
                                      const detail::DbCacheEntry* spatial,
                                      size_t max_stream, const detail::FragmentStore& store,
                                      SpatialSource source, const Progress& progress,
                                      SpatialIndex& out, std::string& why);

// Ett värde som en tråd publicerar och andra läser: skrivs med release och
// läses med acquire, så att allt som skrevs före publiceringen syns för den
// som ser värdet. Kopieras bara av open(), close() och flytt, som inte får gå
// samtidigt med något annat anrop.
template <class T>
struct Published {
    std::atomic<T> value{};

    Published() = default;
    Published(const Published& o) noexcept : value(o.value.load(std::memory_order_relaxed)) {}
    Published& operator=(const Published& o) noexcept {
        value.store(o.value.load(std::memory_order_relaxed), std::memory_order_relaxed);
        return *this;
    }

    T load() const { return value.load(std::memory_order_acquire); }
    void store(T v) { value.store(v, std::memory_order_release); }
};

struct Document::Impl {
    Options options;
    // Delas med läsarna; de håller det vid liv efter close().
    std::shared_ptr<const detail::MappedFile> file;
    detail::Container container;

    // Paketen läsaren använder, eller nullptr. Pekar in i container.entries.
    const detail::DbCacheEntry* hierarchy_entry = nullptr;
    const detail::DbCacheEntry* fragments_entry = nullptr;
    const detail::DbCacheEntry* geometry_entry = nullptr;
    const detail::DbCacheEntry* guids_entry = nullptr;
    const detail::DbCacheEntry* props_entry = nullptr;
    const detail::DbCacheEntry* partition_entry = nullptr;
    const detail::DbCacheEntry* xref_entry = nullptr;
    const detail::DbCacheEntry* source_entry = nullptr;
    const detail::DbCacheEntry* spatial_entry = nullptr;

    // Geometrin: layouten delas med läsarna; cursorn och posten är
    // dokumentets egna, för record_mesh() och fragment_mesh().
    std::shared_ptr<const detail::GeometryLayout> geometry;   // nullptr före load_geometry()
    detail::GeometryCursor cursor;
    detail::RecordData record;   // den senast tolkade posten

    // Placeringen; delas med läsarna. nullptr före load_fragments().
    std::shared_ptr<const detail::FragmentStore> fragments;

    // Det rumsliga indexet. nullptr före load_spatial(). `spatial` är det
    // som anropen svarar ur; `spatial_built` håller det index som byggts för
    // varje källa (index = SpatialSource) till close(), så att en pekare ur
    // spatial_items() gäller så länge dokumentet är öppet, också när ett
    // senare load_spatial() byter källa.
    std::shared_ptr<const SpatialIndex> spatial;
    SpatialSource spatial_source = SpatialSource::File;
    std::shared_ptr<const SpatialIndex> spatial_built[2];

    // GUID-tabellen. `guids` fylls innan `guids_ready` publiceras och ändras
    // sedan inte förrän close(); load_tree() kan läsa den medan andra trådar
    // frågar guid_count() och guid().
    Published<bool> guids_ready;
    std::vector<detail::Guid16> guids;

    // Källan.
    bool source_loaded = false;
    detail::SourceHeader source;

    // Trädet. load_tree() bygger ett nytt träd helt vid sidan av och
    // publicerar det sist genom `tree_view`; const-anropen läser bara
    // `tree_view`, och nullptr betyder att inget träd är läst. `tree` och
    // `retired_trees` äger träden och rörs bara av load_tree() och av de
    // anrop som inte får gå samtidigt med den. Ett publicerat träd lever
    // till close(), så en pekare ur `tree_view` gäller så länge dokumentet
    // är öppet.
    std::shared_ptr<const TreeData> tree;
    Published<const TreeData*> tree_view;
    // Träd som ett senare load_tree() ersatt; hålls kvar så att strängvyer ur
    // dem gäller till close().
    std::vector<std::shared_ptr<const TreeData>> retired_trees;

    // Vyerna, rutnäten, typsnitten och scenens material. Fylls av sitt
    // load_…(), som inte får gå samtidigt med något annat anrop, och ändras
    // sedan inte förrän close().
    bool views_loaded = false;
    ViewsData views;
    bool grids_loaded = false;
    detail::GridElement grids;
    bool fonts_loaded = false;
    detail::SharedNodes fonts;
    bool scene_materials_loaded = false;
    detail::ReMaterials scene_materials;
    // Minnet för de fyra ovan.
    size_t elements_memory_bytes() const;

    std::string last_error;

    bool is_open() const { return file && file->is_open(); }

    // Sätter last_error ur koden och detaljen och ger koden som Status.
    Status fail(detail::ErrorCode code, const std::string& detail);
    // Sätter last_error till en egen text.
    Status fail(Status status, const char* text);
    // Tömmer last_error och ger Ok.
    Status ok();

    // Prövar att varje fragments seq pekar på ett löv i `tree`. Anropas när
    // både fragmenten och trädet är lästa, av den som läste sist; load_tree()
    // prövar sitt träd innan det publiceras.
    Status check_seq_leaves(const detail::FragmentStore& store, const TreeData& tree_data);
};

} // namespace HNwd
