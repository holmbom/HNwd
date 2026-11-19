#pragma once
#include "container.h"
#include "dbcache.h"
#include "inflate.h"
#include "nwd_error.h"
#include "object_reader.h"
#include "partition.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace HNwd::detail {

// Paketet LcOpNwdLogicalHierarchy: objektträdet med namn, egenskaper,
// material och placering. En objektström:
//
//   [int32 k][k × int32 rotantal]  [100]  [int32 n] n × PTR  [int32 1]   nollor upp till 8
//
// k >= 1 är antalet strömmar i LcOaPartitionProps och rotantalen hur många
// rötter var och en har. [100] är den underförstådda roten. De n pekarna är
// toppnoderna. Klasserna:
//
//   53 grupp     [str namn][ptr 52][i32 flags][i32 X][i32 Y] Y × ATTR [i32 c] c × BARN
//   22 löv       som 53, utan c och barn
//   32 partition en modell i en fil med flera:
//              [i32 0] och sedan en kropp som klass 53 har, med modellens träd som
//              barn, och sist en svans (PartitionTail i partition.h). Står som
//              toppnod eller som barn till en nod av klass 32. Svansen skapar
//              nya objekt av klass 27 och 185, som tar id:n i trädets ström
//   52 namnpar   [str visningsnamn][str internt namn]
//   86 flik      [str namn][ptr 52][i32 flags][i32 n] n × ( [ptr 52 nyckel][i32 typ][värde] )
//   66 element-id [str null][ptr 52 nyckel][i32 flags] PAD8 [i64 värde i 1 .. 2^31-1]
//   70 text-id   [str värde][ptr 52 nyckel][i32 flags]
//                flags i 66 och 70 varierar (0, 0x00020000, 0x01000000) utan att
//                layouten ändras; de lagras i fliken
//   76 text      [str null][ptr 52 nyckel][i32 0][str värde]
//   92           HUVUD [i32 96][48 byte] — sedd en gång; innehållet är inte känt
//   31 material  HUVUD [14 × f64]
//   40 flytt     HUVUD [3 × f64 t]
//   41 flytt och rotation   HUVUD [3 × f64 t][4 × f64 q (x, y, z, w)]
//   39 matris    HUVUD [9 × f64][3 × f64 t][i32 speglar]
//   79 länkar    HUVUD [i32 n] n × ( [str namn][str adress][PTR][i32 p][p × 3 f64] )
//   200          HUVUD [ptr 185]
//   HUVUD     := [str namn][ptr 52 eller null][i32 flags]     som klass 86 börjar
//   185          se skip_class185()
//
// Var klasserna får stå:
//   toppnod   53 eller 32, aldrig null
//   BARN      22 eller 53, och under en partition också 32; aldrig null; får
//             vara en bakåtpekare, så trädet är
//             en riktad acyklisk graf och ett delträd kan sitta på flera ställen
//   ptr 52    aldrig null i en flik eller som nyckel; en NODS namnpar får
//             vara null
//   ATTR      86, 31, 40, 41, 39, 79, 200, 66, 70, 76, 92 — eller null. En null-plats tar
//             nästa rot ur LcOaPartitionProps: den n-te null-platsen i
//             byteordning hör till den n-te roten i ström 0, sedan ström 1 …
//   rot i LcOaPartitionProps   ett NYTT objekt av klass 86, 79, 66, 70, 76 eller 92
//   PTR i klass 79   klass 52 eller null i LcOaPartitionProps; i trädet vilken
//             som helst av klasserna ovan utom 66, eller null
//   ptr 185 i klass 200   aldrig null
//
// Värdet i en flik efter typ:
//   0 inget · 1, 6, 7, 10, 11 f64 · 2, 3 i32 · 4, 9 sträng · 5 i64 (unix-sekunder)
//   8 ptr 52 · 12 tre f64 · 13 två f64 · allt annat är fel
//
// X är elementindex: 0 om noden inte är ett element, annars pekar X-1 in i
// GUID-tabellen. X ska ligga i 0 .. antalet GUID.

inline constexpr uint32_t kNone = UINT32_MAX;

// En tabell med öppen adressering över tal, för att hitta dubbletter under en
// inläsning. Varje plats håller talet och 32 bitar av dess hash, blandad så
// att varje bit i anroparens hash påverkar platsen; posterna själva jämförs
// genom en funktion som anroparen ger. Tabellen växer med det som läggs in,
// aldrig efter ett tal ur filen.
//
// Ingen sökning går längre än kMaxProbe platser. Når en sökning eller en
// inläggning dit ger tabellen upp: den töms, find() ger sedan alltid kNone
// och insert() lägger inte in något, tills clear(). Att inte hitta en
// dubblett är alltid tillåtet, så tiden är linjär i antalet poster vad
// hasharna än är.
class DedupeTable {
public:
    static constexpr size_t kMaxProbe = 128;

    // Talet för en post som är lika med den som `hash` och `equal` beskriver,
    // eller kNone. `equal(v)` säger om det lagrade talet v är lika.
    template <class Equal>
    uint32_t find(size_t hash, Equal equal) {
        if (slots_.empty()) return kNone;
        const uint32_t h = mix(hash);
        const size_t mask = slots_.size() - 1;
        size_t i = h & mask;
        for (size_t probes = 0; probes < kMaxProbe; ++probes, i = (i + 1) & mask) {
            const uint64_t slot = slots_[i];
            if (slot == kEmpty) return kNone;
            const uint32_t v = static_cast<uint32_t>(slot >> 32);
            if (static_cast<uint32_t>(slot) == h && equal(v)) return v;
        }
        give_up();
        return kNone;
    }
    // Lägger in `value` med hashen `hash`.
    void insert(size_t hash, uint32_t value) {
        if (gave_up_) return;
        if ((count_ + 1) * 2 > slots_.size() && !grow()) return give_up();
        if (!place((static_cast<uint64_t>(value) << 32) | mix(hash))) return give_up();
        ++count_;
    }
    // Sann när tabellen har gett upp (se ovan).
    bool gave_up() const { return gave_up_; }
    void clear() {
        slots_ = std::vector<uint64_t>();
        count_ = 0;
        gave_up_ = false;
    }
    size_t memory_bytes() const { return slots_.capacity() * sizeof(uint64_t); }

private:
    static constexpr uint64_t kEmpty = UINT64_MAX;

    // Blandningen fmix64 över alla 64 bitar; de låga 32 blir kvar.
    static uint32_t mix(size_t hash) {
        uint64_t x = static_cast<uint64_t>(hash);
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33;
        x *= 0xc4ceb9fe1a85ec53ULL;
        x ^= x >> 33;
        return static_cast<uint32_t>(x);
    }
    void give_up() {
        slots_ = std::vector<uint64_t>();
        count_ = 0;
        gave_up_ = true;
    }
    // false om platsen inte nås inom kMaxProbe.
    bool place(uint64_t slot) {
        const size_t mask = slots_.size() - 1;
        size_t i = static_cast<uint32_t>(slot) & mask;
        for (size_t probes = 0; probes < kMaxProbe; ++probes, i = (i + 1) & mask) {
            if (slots_[i] == kEmpty) {
                slots_[i] = slot;
                return true;
            }
        }
        return false;
    }
    bool grow() {
        std::vector<uint64_t> old = std::move(slots_);
        slots_.assign(old.empty() ? 64 : old.size() * 2, kEmpty);
        for (const uint64_t slot : old)
            if (slot != kEmpty && !place(slot)) return false;
        return true;
    }

    // En plats: talet i de höga 32 bitarna, hashen i de låga. Talet tas ut med
    // en skiftning, så att det alltid är ett eget 32-bitarsvärde och aldrig
    // läses tillsammans med hashen.
    std::vector<uint64_t> slots_;
    size_t count_ = 0;
    bool gave_up_ = false;
};

// Alla strängar i trädet i en buffert. Index kNone är en null-sträng.
//
// Med dedupe() påslaget ger en sträng som redan finns i poolen samma index
// igen, så att namn, material och värden som upprepas lagras en gång.
// Tabellen för det hålls bara medan dedupe är på. Är färre än var åttonde av
// de första kDedupeSample strängarna en dubblett slås det av igen: sökningen
// kostar då mer tid än den sparar minne. Det slås också av när tabellen ger
// upp (DedupeTable::kMaxProbe); strängarna lagras då var för sig.
class StringPool {
public:
    static constexpr uint32_t kDedupeSample = 65536;

    // Lägger till strängen och ger dess index i `index`. false om poolen inte
    // rymmer den: mer än 4 GB tecken eller fler än 2^32 - 1 strängar.
    bool add(std::string_view s, uint32_t& index);
    std::string_view get(uint32_t index) const;   // tom för kNone
    size_t count() const { return ends_.size(); }
    void dedupe(bool on);
    // Släpper det som växte med under inläsningen.
    void shrink_to_fit() {
        chars_.shrink_to_fit();
        ends_.shrink_to_fit();
    }
    // Byte som poolen håller.
    size_t memory_bytes() const {
        return chars_.capacity() + ends_.capacity() * sizeof(uint32_t) + table_.memory_bytes();
    }

private:
    std::vector<char> chars_;
    std::vector<uint32_t> ends_;   // var varje sträng slutar; den börjar där den förra slutade
    bool dedupe_ = false;
    uint32_t looked_up_ = 0;   // strängar som sökts sedan dedupe slogs på
    uint32_t found_ = 0;       // av dem dubbletter
    DedupeTable table_;
};

// Klass 52.
struct StoredName {
    uint32_t display = kNone;    // visningsnamn
    uint32_t internal = kNone;   // internt namn
};

struct StoredProperty {
    uint32_t key = kNone;   // index i Hierarchy::names
    int32_t type = 0;       // värdetypen som den står i filen
    double d[3] = {};       // typ 1, 6, 7, 10, 11 (d[0]), 12 (tre) och 13 (två)
    int64_t i = 0;          // typ 2, 3 och 5
    uint32_t text = kNone;  // typ 4 och 9: sträng. Typ 8: index i Hierarchy::names
};

// Klass 86, eller klass 66 som en flik med en egenskap av typ 2, eller klass
// 70 eller 76 som en flik med en egenskap av typ 4.
struct StoredTab {
    uint32_t name = kNone;      // sträng; null om fliken bara har sitt namnpar
    uint32_t user = kNone;      // index i Hierarchy::names
    int32_t flags = 0;
    uint32_t prop_begin = 0;    // i Hierarchy::properties
    uint32_t prop_count = 0;
    int32_t source_class = 86;  // 86, 66, 70 eller 76
};

// Klass 40, 41 eller 39 på en nod, så som Hierarchy::transform() lämnar ut den.
struct StoredTransform {
    int32_t source_class = 40;
    double t[3] = {};
    double q[4] = {0, 0, 0, 1};                    // klass 41, som den står i filen
    double m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};     // klass 39, i filens ordning: kolumnvis
    int32_t reverses = 0;                          // klass 39
};

// En nod så som den är lagrad: en gång, hur många ställen den än sitter på.
//
// Noderna numreras i den ordning tolkningen når deras barnlista, alltså när
// nodens attribut är lästa. För ett träd utan delade delträd och utan noder i
// länkar är det trädets preorder. Barnlistorna ligger i samma ordning, så en
// nods antal barn är nästa nods början minus dess egen
// (Hierarchy::child_count()); likaså flikarna i Full (Hierarchy::tab_begin).
struct StoredNode {
    uint32_t name = kNone;          // sträng; null om noden bara har sitt namnpar
    uint32_t user = kNone;          // index i Hierarchy::names; kNone om noden saknar namnpar
    int32_t element = 0;            // X
    uint32_t material = kNone;      // sträng: namnet i nodens första klass 31
    uint32_t child_begin = 0;       // i Hierarchy::children
    uint32_t expanded = 1;          // noden och allt under den, med delade delträd räknade varje gång
    uint16_t height = 1;            // nivåer från noden till dess djupaste löv
    uint8_t cls = 0;                // 53, 22 eller 32
};

// Ett värde för en nod, i en gles lista sorterad efter nod.
struct NodeValue {
    uint32_t node = 0;
    uint32_t value = 0;
};

struct Hierarchy {
    StringPool strings;
    std::vector<StoredName> names;
    std::vector<StoredProperty> properties;
    std::vector<StoredTab> tabs;
    std::vector<StoredNode> nodes;
    std::vector<uint32_t> node_tabs;   // flikarna per nod, i följd; index i tabs
    // Full: per nod var dess flikar börjar i node_tabs, och sist antalet; tom i
    // Structure, där antalet står i skelettet (HierarchySkeleton::location).
    std::vector<uint32_t> tab_begin;
    std::vector<uint32_t> children;    // barnen per nod, i följd; index i nodes
    std::vector<uint32_t> top;         // toppnoderna; index i nodes
    std::vector<int32_t> root_counts;  // ur huvudet
    uint64_t expanded_total = 0;       // alla noder med den underförstådda roten
    uint64_t null_slots = 0;           // tomma attributplatser, alltså förbrukade rötter
    size_t non_utf8_strings = 0;       // strängar som inte är giltig UTF-8
    std::vector<PartitionTail> partitions;   // svansen i varje klass-32-nod, i läsordning
    std::vector<PartitionLight> lights;      // klass 27-objekten i partitionssvansarna
    // Nodernas flaggor där de inte är 0 (värdet är int32-talet som bitar), och
    // partitionernas svansar (index i partitions), båda sorterade efter nod.
    std::vector<NodeValue> node_flags;
    std::vector<NodeValue> node_partitions;

    // Placeringarna, varje klass 40, 41 och 39 som lästs, i läsordning. Ett ord
    // per transform: formen i de två översta bitarna och var talen börjar i
    // transform_values i de övriga. Formerna och talen:
    //   0  klass 40   t (3)
    //   1  klass 41   t (3), q (4)
    //   2  klass 41   t (3), och positionen för en lika rotation q som redan
    //                 står i transform_values (1, som heltalets bitar)
    //   3  klass 39   m (9), t (3), speglingen (1, som tal)
    std::vector<uint32_t> transforms;
    std::vector<double> transform_values;
    // Per nod: index i transforms för nodens första, eller kNone. Tom när
    // placeringarna inte lagras.
    std::vector<uint32_t> node_transforms;

    uint32_t child_count(uint32_t node) const {
        const uint32_t end = node + 1 < nodes.size() ? nodes[node + 1].child_begin
                                                     : static_cast<uint32_t>(children.size());
        return end - nodes[node].child_begin;
    }
    // Full: nodens antal flikar. 0 i Structure.
    uint32_t tab_count(uint32_t node) const {
        return node + 1 < tab_begin.size() ? tab_begin[node + 1] - tab_begin[node] : 0;
    }
    int32_t flags(uint32_t node) const;
    uint32_t partition(uint32_t node) const;   // index i partitions; kNone utom för klass 32

    // Nodens första transform (index i transforms), eller kNone.
    uint32_t node_transform(uint32_t node) const {
        return node < node_transforms.size() ? node_transforms[node] : kNone;
    }
    size_t transform_count() const { return transforms.size(); }
    int32_t transform_class(uint32_t index) const;   // 40, 41 eller 39; 0 för ett index utanför
    // false för ett index utanför.
    bool transform(uint32_t index, StoredTransform& out) const;

    // Byte som trädet håller, räknat ur vektorernas kapacitet.
    size_t memory_bytes() const;
};

// Tolkar trädet. `out` ersätts.
//
// `props` är strömmarna ur LcOaPartitionProps (read_partition_props), eller
// nullptr om filen saknar paketet. Antalet strömmar ska vara k, varje ström
// ska ha exakt sitt rotantal och sluta med nollor upp till 8, och varje rot
// ska förbrukas av en null-plats.
//
// `guid_count` är GUID-tabellens storlek och `max_nodes` taket för
// expanded_total.
//
// Flikarna hamnar på noden i den ordning attributplatserna står. Länkar (79)
// och klass 200 läses och prövas men lämnas inte ut.
//
// Tolkningen har en egen stack för noderna: djupet kostar inte anroparens
// stack.
//
// CorruptObject: brott mot reglerna ovan. CorruptLink: rötterna och
// null-platserna går inte ihop, eller antalet strömmar är inte k.
// Unsupported: en nod av klass 32 på annat ställe än som toppnod eller under en
// nod av klass 32 (till exempel som mål för en länk); `detail` anger byten där dess pekare står.
// LimitExceeded: fler än `max_nodes` expanderade noder, eller ett träd högre
// än kMaxNesting; en nod med fler än 2^32 - 1 expanderade noder under sig;
// strängar över 4 GB eller placeringar över 8 GB.
//
// Strängar som upprepas lagras en gång (StringPool::dedupe), och likaså
// rotationerna i klass 41.
ErrorCode parse_hierarchy(const uint8_t* data, size_t size,
                          const std::vector<std::vector<uint8_t>>* props,
                          uint32_t guid_count, uint64_t max_nodes, Hierarchy& out,
                          std::string* detail = nullptr);

// ---------------------------------------------------------------------------
// Trädet läst direkt ur filen, och skelettet.

// Var trädets strömmar ligger i filen: LcOpNwdLogicalHierarchy (formen (0, 0))
// och strömmarna i LcOaPartitionProps.
struct HierarchyStreams {
    StreamSpan tree;
    bool has_props = false;           // filen har paketet LcOaPartitionProps
    std::vector<StreamSpan> props;    // dess k strömmar, i ordning
    uint64_t max_stream = kDefaultMaxStreamBytes;   // tak för VAR OCH EN uppackad ström
    // Största position i en uppackad ström som skelettet (Structure) kan
    // lagra; positionerna där har 32 bitar. Ett lägre värde prövar gränsen
    // på en liten ström. Full lagrar inga positioner och bryr sig inte om den.
    uint64_t max_position = UINT32_MAX;
};

// Fyller `out` ur katalogposterna. `props` är nullptr om filen saknar paketet.
// CorruptPackage: trädets paket har inte flaggan 1 och formen (0, 0), eller
// felen ur locate_partition_props(). OutOfRange: trädets paket ligger utanför
// filen.
ErrorCode locate_hierarchy(const uint8_t* file, size_t size, const DbCacheEntry& tree,
                           const DbCacheEntry* props, uint64_t max_stream, HierarchyStreams& out,
                           std::string* detail = nullptr);

// Full: allt som parse_hierarchy() ovan ger. Structure: skelettet — noderna
// med namn, klass, element, material, placering, barn och partitioner, och
// namnparen (Hierarchy::names) — men inga flikar: Hierarchy::tabs,
// properties, node_tabs och tab_begin är tomma, och antalet flikar en nod har
// står i skelettet (NodeLocation::tab_count). Strängarna i flikarna läses och
// prövas men lagras inte.
enum class HierarchyContent : uint8_t { Full, Structure };

// Var en lagrad nods attribut står: positionen för antalet attribut (Y) i
// trädets uppackade ström, och nästa id där. Och hur många flikar noden har.
struct NodeLocation {
    uint32_t attr_pos = 0;
    int32_t attr_next_id = 0;
    uint32_t tab_count = 0;
};

// En egenskapsrot: dess pekarplats i sin ström, rotens id, och var null-platsen
// som tar den står i trädets ström. Strömmen följer av rotens nummer och
// Hierarchy::root_counts; en nods första rot är den första vars null-plats står
// efter nodens attr_pos.
struct RootLocation {
    uint32_t pos = 0;
    int32_t next_id = 0;
    uint32_t slot = 0;
};

// Ett objekt som en nods attribut (eller en rot, eller en flik i trädet) pekar
// bakåt på, fast det ligger före dem i strömmen. `value` är som i en
// fullständig tolkning, utom för flikar (86, 66, 70, 76) i trädet: där är det
// positionen där flikens kropp börjar, med nästa id id + 1.
struct IdRef {
    int32_t id = 0;
    int32_t cls = 0;
    uint32_t value = 0;
};

// Rader om tre 32-bitarstal i följd, packade: i block om kBlock rader står den
// första raden som den är och resten som skillnaden mot raden före, varje tal
// som varint med tecknet i lägsta biten. En rad läses med sitt index.
class PackedRows {
public:
    static constexpr size_t kWidth = 3;
    static constexpr size_t kBlock = 16;
    void push(const uint32_t (&row)[kWidth]);
    // false för ett index utanför.
    bool get(size_t index, uint32_t (&row)[kWidth]) const;
    size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }
    void shrink_to_fit() {
        bytes_.shrink_to_fit();
        block_at_.shrink_to_fit();
    }
    size_t memory_bytes() const { return bytes_.capacity() + block_at_.capacity() * sizeof(size_t); }

private:
    std::vector<uint8_t> bytes_;
    std::vector<size_t> block_at_;   // var varje block börjar i bytes_
    size_t count_ = 0;
    uint32_t last_[kWidth] = {};
};

// Bakåtpekarna per ström, sorterade efter id och packade: i block om kBlock
// poster står blockets första id för sig. Varje post är ett varint av
// skillnaden mot förra id, med en bit för att klassen skiljer sig från förra
// postens (då följer klassen), och värdet som skillnaden mot förra värdet med
// samma klass i blocket, med tecknet i lägsta biten.
class PackedRefs {
public:
    static constexpr size_t kBlock = 32;
    // Ströms poster, i stigande id utan dubbletter. Strömmarna läggs till i
    // ordning 0, 1, …
    void add_stream(const std::vector<IdRef>& refs);
    // Posten med `id` i ström `stream`. false om den saknas.
    bool find(uint32_t stream, int32_t id, int32_t& cls, uint32_t& value) const;
    // Alla poster i ström `stream`, i stigande id. false om strömmen saknas
    // eller posterna inte går att läsa.
    bool list(uint32_t stream, std::vector<IdRef>& out) const;
    size_t stream_count() const { return stream_begin_.size(); }
    size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }
    void shrink_to_fit() {
        bytes_.shrink_to_fit();
        blocks_.shrink_to_fit();
        stream_begin_.shrink_to_fit();
    }
    size_t memory_bytes() const {
        return bytes_.capacity() + blocks_.capacity() * sizeof(Block) +
               stream_begin_.capacity() * sizeof(size_t);
    }

private:
    struct Block {
        int32_t first_id;
        uint32_t count;
        size_t at;   // i bytes_
    };
    std::vector<uint8_t> bytes_;
    std::vector<Block> blocks_;
    std::vector<size_t> stream_begin_;   // per ström: dess första block; sist antalet block
    size_t count_ = 0;
};

// Det som behövs för att läsa en nods flikar i efterhand (read_node_tabs).
// Strömmarnas nummer: 0 är trädet, 1 + s egenskapsström s.
struct HierarchySkeleton {
    // Per lagrad nod, samma index som Hierarchy::nodes: (attr_pos,
    // attr_next_id, tab_count) ur NodeLocation.
    PackedRows locations;
    std::vector<RootLocation> roots;       // per egenskapsrot, i den ordning null-platserna tar dem
    std::vector<uint32_t> first_root;      // per egenskapsström: numret på dess första rot
    PackedRefs refs;                       // per ström
    // Per ström, i stigande position.
    std::vector<std::vector<InflateStream::Checkpoint>> checkpoints;

    // Var nodens attribut står. false för en nod utanför.
    bool location(uint32_t node, NodeLocation& out) const;

    // Byte som skelettet håller, kontrollpunkterna inräknade.
    size_t memory_bytes() const;
    size_t checkpoint_bytes() const;
};

// Ungefär så mycket uppackad data mellan två kontrollpunkter.
inline constexpr uint64_t kCheckpointEvery = uint64_t(1) << 20;

// Förloppet: `done` av `total` komprimerade byte. false avbryter.
using HierarchyProgress = std::function<bool(uint64_t done, uint64_t total)>;

// Som parse_hierarchy() ovan, men strömmarna packas upp medan de läses, med ett
// fönster i taget: trädet, och egenskapsströmmarna en i taget i den ordning
// rötterna tas. Varje ström ska ta exakt sitt span i filen; dess kontrollsumma
// prövas.
//
// `skeleton` fylls i läget Structure (och töms i Full). Då sparas en
// kontrollpunkt ungefär var `checkpoint_every` byte i varje ström.
// `progress` får vara tom. Utan `transforms` läses placeringarna och prövas,
// men lagras inte: Hierarchy::transforms och node_transforms är tomma.
//
// Samma fel som parse_hierarchy(), och dessutom strömmarnas egna
// (CorruptStream, StreamTooLarge, OutOfRange). Cancelled: `progress` gav
// false. LimitExceeded också om en position i en ström inte ryms i 32 bitar.
ErrorCode parse_hierarchy(const uint8_t* file, size_t size, const HierarchyStreams& streams,
                          uint32_t guid_count, uint64_t max_nodes, HierarchyContent content,
                          Hierarchy& out, HierarchySkeleton* skeleton,
                          const HierarchyProgress& progress, std::string* detail = nullptr,
                          uint64_t checkpoint_every = kCheckpointEvery, bool transforms = true);

// En nods flikar och egenskaper, läsa i efterhand. Index i `names` för
// StoredTab::user, StoredProperty::key och värden av typ 8; strängarna i
// `strings`.
struct NodeTabs {
    StringPool strings;
    std::vector<StoredName> names;
    std::vector<StoredProperty> properties;
    std::vector<StoredTab> tabs;   // nodens flikar i attributordning

    // Byte som listorna håller, räknat ur kapaciteten.
    size_t memory_bytes() const;
};

// Tolkade flikar ur trädets ström som flera noder delar, hos den som läser om
// noder. En flik lämnas som en NodeTabs med exakt en flik.
class SharedTabCache {
public:
    virtual ~SharedTabCache() = default;
    // Fliken med id `id` i trädets ström, eller nullptr. Pekaren gäller till
    // nästa anrop på cachen.
    virtual const NodeTabs* find_tab(int32_t id) = 0;
    // Fliken med id `id`. Cachen får låta bli att behålla den.
    virtual void keep_tab(int32_t id, NodeTabs&& tab) = 0;
};

// Det parse_node_tabs() läser genom: läsare som står mitt i en av trädets
// strömmar (0 trädet, 1 + s egenskapsström s).
class ObjectSource {
public:
    virtual ~ObjectSource() = default;
    // En läsare som står på `pos` i strömmen `stream`, med nästa id `next_id`;
    // bakåtpekare före det går genom `resolve`. Läsare med olika `cursor`
    // (0, 1 eller 2) lever samtidigt; en ny med samma nummer ersätter den förra.
    // nullptr vid fel, med `code` och `detail`.
    virtual ObjectReader* open(int cursor, uint32_t stream, uint64_t pos, int32_t next_id,
                               ObjectReader::Resolver resolve, ErrorCode& code,
                               std::string& detail) = 0;
};

// Läser den lagrade noden `node`:s flikar ur `source` enligt skelettet. `h` och
// `skeleton` är resultatet av parse_hierarchy() i läget Structure. `out`
// ersätts. Felen som parse_hierarchy(); CorruptLink om skelettet inte stämmer
// med strömmen, OutOfRange om noden inte finns.
//
// Med `cache` tas flikar som flera noder delar ur den i stället för ur
// strömmen, och de som läses läggs i den.
ErrorCode parse_node_tabs(const Hierarchy& h, const HierarchySkeleton& skeleton, uint32_t node,
                          ObjectSource& source, NodeTabs& out, std::string* detail = nullptr,
                          SharedTabCache* cache = nullptr);

} // namespace HNwd::detail
