#pragma once
#include "container.h"
#include "dbcache.h"
#include "geometry.h"
#include "nwd_error.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace HNwd::detail {

// Hur chunkarnas uppackade storlekar blir kända.
//
//   Fixed  varje chunk utom den sista packas upp till exakt paketets prefix P
//          (65536) byte, och den sista till resten. Chunk i börjar på i·P i
//          den uppackade följden, så en post nås utan att något annat packas
//          upp. Varje uppackning prövas mot den storleken.
//   Scan   storlekarna mäts: open packar upp varje chunk en gång. För ett
//          paket som inte följer regeln i Fixed.
enum class ChunkLayout : uint8_t { Fixed, Scan };

struct SegmentLimits {
    size_t max_stream = kDefaultMaxStreamBytes;       // tak för en uppackad chunk
    size_t cache_bytes = size_t(256) * 1024 * 1024;   // tak för uppackade chunkar i minnet
    // Tak för en posts längd. En post kan vara mycket större tolkad än lagrad
    // (ett index på en byte kan stå för 64 vertexar), så taket är det som
    // begränsar minnet för en enskild post.
    size_t max_record = size_t(16) * 1024 * 1024;
    int normal_bits = 0;                              // 8 eller 16 tvingar bredden; 0 avgör den ur posterna
    ChunkLayout layout = ChunkLayout::Fixed;
};

// Paketet LcOpNwdGeometry: geometriposterna, packade i chunkar.
//
//   [int32 P = 65536] CHUNK{ch} TOC
//
// Chunkarna är zlib-strömmar. Uppackade och lagda efter varandra är de en
// följd av poster, och innehållsförteckningen ger varje posts längd. En post
// kan börja i en chunk och sluta i en senare. Summan av postlängderna kallas
// `total` nedan.
//
// Layouten är det som är känt om paketet: var chunkarna ligger i filen, var
// de börjar i den uppackade följden, var varje post börjar, och normalernas
// bredd. Efter open() och decide_normal_bits() ändras den inte, och flera
// trådar får läsa den samtidigt, var och en genom sin egen GeometryCursor.
// Minnet är fast: en position per post och en per chunk; ingenting växer med
// det som läses.
//
// Filen ska vara mappad så länge layouten används.
class GeometryLayout {
public:
    // Läser innehållsförteckningen och delar paketet i chunkar.
    //
    // Fixed packar inte upp någon chunk, och kräver (ch − 1)·P < total ≤ ch·P.
    // Scan packar upp varje chunk en gång och mäter den.
    //
    // CorruptPackage om paketet inte har formen (4, b) inuti filen, om
    // prefixet inte är 65536, om chunkarnas längder inte går ihop med
    // paketets, i Fixed om total ligger utanför intervallet ovan, och i Scan
    // om chunkarna ger mer eller mindre än total. OutOfRange om
    // limits.normal_bits inte är 0, 8 eller 16. CorruptGeometry om
    // innehållsförteckningen bryter sin grammatik. Fel från uppackningen
    // lämnas vidare. Efter ett fel är layouten tom.
    ErrorCode open(const uint8_t* file, size_t size, const DbCacheEntry& entry,
                   const SegmentLimits& limits, std::string* detail = nullptr);

    // Palettnormalernas bredd står inte i filen: den är den bredd posterna
    // går att tolka med. Posterna prövas framifrån, och den första som bara
    // tolkas med den ena bredden avgör. Tolkas alla med båda blir den 16. En
    // post som inte går att tolka med någon av bredderna, eller som är längre
    // än limits.max_record, hoppas över: den ger sitt fel först när den
    // efterfrågas. Med limits.normal_bits satt görs ingen prövning.
    //
    // `progress(done, total)` anropas med antalet prövade poster och
    // record_count(); svarar den false avbryts prövningen med Cancelled, och
    // bredden är då fortfarande inte avgjord. Görs en gång; ett anrop till
    // när den är avgjord ändrar ingenting. Fel i paketet lämnas vidare.
    ErrorCode decide_normal_bits(const std::function<bool(uint64_t, uint64_t)>& progress,
                                 std::string* detail = nullptr);

    uint32_t record_count() const;
    uint32_t chunk_count() const;
    uint64_t total_bytes() const;               // summan av postlängderna = alla chunkar uppackade
    ChunkLayout layout() const;
    const SegmentLimits& limits() const;

    uint64_t chunk_start(uint32_t i) const;     // var chunk i börjar i den uppackade följden; 0 utanför
    uint64_t chunk_size(uint32_t i) const;      // uppackad storlek; 0 utanför
    uint64_t record_start(uint32_t k) const;    // var post k börjar; total_bytes() för k = record_count()

    // Chunken som håller byte `pos` i den uppackade följden; pos < total_bytes().
    // O(1) i Fixed, O(log ch) i Scan.
    uint32_t chunk_of(uint64_t pos) const;

    int normal_bits() const;                    // 0 innan den är avgjord
    int64_t decided_by() const;                 // posten som avgjorde, -1 om ingen gjorde det

    // Minnet som layouten håller: positionerna per post och per chunk.
    size_t memory_bytes() const;

    // Packar upp chunk `i` till `out` och prövar att den blir chunk_size(i)
    // byte. CorruptPackage om den blir större eller mindre, OutOfRange om i
    // >= chunk_count(); övriga fel från uppackningen lämnas vidare. Rör inte
    // layouten och går att anropa från flera trådar samtidigt.
    ErrorCode inflate_chunk(const uint8_t* file, size_t size, uint32_t i,
                            std::vector<uint8_t>& out, std::string* detail = nullptr) const;

private:
    ErrorCode measure_chunks(std::string* detail);   // Scan: fyller chunk_start_

    const uint8_t* file_ = nullptr;
    size_t file_size_ = 0;
    uint64_t offset_ = 0;                       // paketets början i filen
    SegmentLimits limits_;
    uint64_t chunk_bytes_ = 0;                 // P, i Fixed
    std::vector<StreamSpan> spans_;             // var chunkarna ligger i filen
    std::vector<uint64_t> chunk_start_;         // Scan: chunk_count() + 1 värden; tom i Fixed
    std::vector<uint64_t> record_start_;        // record_count() + 1 värden; det sista är total_bytes()
    int normal_bits_ = 0;
    int64_t decided_by_ = -1;
};

// En läsares eget tillstånd: uppackade chunkar i en cache med tak, och
// bufferten för en post som går över en chunkgräns. Läser bara layouten, så
// flera cursorer kan arbeta samtidigt över samma layout, en per tråd. En
// cursor i sig är inte trådsäker.
//
// Uppackade chunkar ligger i cachen; de som användes längst sedan kastas
// först när taket nås, och packas upp igen om de behövs. Chunkarna en post
// behöver hålls alltid, också när de tillsammans är större än taket.
//
// Layouten ska vara öppnad innan cursorn skapas och leva lika länge som den.
class GeometryCursor {
public:
    GeometryCursor() = default;   // utan layout: varje post är OutOfRange
    GeometryCursor(const GeometryLayout& layout, const uint8_t* file, size_t size,
                   size_t cache_bytes);

    // Byten i post `k` (0-baserat). Pekaren gäller till nästa anrop på
    // cursorn.
    //
    // OutOfRange: k >= record_count(). LimitExceeded: posten är längre än
    // limits.max_record. CorruptPackage: en chunk ger en annan storlek än
    // layouten säger. Fel från uppackningen lämnas vidare.
    ErrorCode record_bytes(uint32_t k, const uint8_t*& data, size_t& size,
                           std::string* detail = nullptr);

    // Tolkar post `k` med layoutens bredd. NotLoaded om bredden inte är
    // avgjord. En post som bara går att tolka med den andra bredden ger
    // CorruptGeometry.
    ErrorCode parse(uint32_t k, RecordData& out, std::string* detail = nullptr);

    size_t cached_bytes() const;     // minne som de uppackade chunkarna håller just nu
    uint64_t inflated_total() const; // byte som packats upp sammanlagt, omtagningar inräknade

private:
    static constexpr uint32_t kNoSlot = UINT32_MAX;

    struct Slot {
        uint32_t chunk = kNoSlot;
        std::vector<uint8_t> data;
        uint32_t older = kNoSlot;   // grannarna i cachens ordning, från den som
        uint32_t newer = kNoSlot;   // användes längst sedan till den senaste
    };

    ErrorCode load(uint32_t chunk, std::string* detail);
    void evict(uint32_t keep_from, uint32_t keep_to);
    void touch(uint32_t slot);
    void unlink(uint32_t slot);

    const GeometryLayout* layout_ = nullptr;
    const uint8_t* file_ = nullptr;
    size_t file_size_ = 0;
    size_t cache_bytes_ = 0;
    std::vector<uint32_t> slot_of_;   // per chunk: platsen i slots_, eller kNoSlot
    std::vector<Slot> slots_;
    std::vector<uint32_t> free_;      // lediga platser i slots_
    uint32_t oldest_ = kNoSlot;
    uint32_t newest_ = kNoSlot;
    size_t cached_bytes_ = 0;
    uint64_t inflated_total_ = 0;
    std::vector<uint8_t> scratch_;    // en post som går över en chunkgräns
};

// En layout och en cursor i ett, för den som läser från en tråd.
//
// Filen ska vara mappad så länge segmentet används. Inte trådsäkert.
class GeometrySegment {
public:
    // Som GeometryLayout::open. Efter ett fel är segmentet tomt.
    ErrorCode open(const uint8_t* file, size_t size, const DbCacheEntry& entry,
                   const SegmentLimits& limits, std::string* detail = nullptr);

    uint32_t record_count() const;
    uint32_t chunk_count() const;
    uint64_t total_bytes() const;   // summan av postlängderna = alla chunkar uppackade

    // Som GeometryCursor::record_bytes.
    ErrorCode record_bytes(uint32_t k, const uint8_t*& data, size_t& size,
                           std::string* detail = nullptr);

    // GeometryLayout::decide_normal_bits utan förlopp.
    ErrorCode resolve_normal_bits(std::string* detail = nullptr);
    int normal_bits() const;        // 0 innan den är avgjord
    int64_t decided_by() const;     // posten som avgjorde, -1 om ingen gjorde det

    // Tolkar post `k` (0-baserat) med den avgjorda bredden; avgör den först
    // om det inte är gjort.
    ErrorCode parse(uint32_t k, RecordData& out, std::string* detail = nullptr);

    // Packar upp varje chunk en gång och prövar att den ger den storlek
    // layouten säger. Chunkarna läggs inte i cachen.
    ErrorCode size_all(std::string* detail = nullptr);

    uint32_t sized_chunks() const;              // chunkar vars storlek är känd: alla efter open
    uint64_t chunk_size(uint32_t i) const;      // uppackad storlek; 0 utanför
    uint64_t chunk_start(uint32_t i) const;     // var chunken börjar uppackad; 0 utanför
    size_t cached_bytes() const;                // minne som de uppackade chunkarna håller just nu
    uint64_t inflated_total() const;            // byte som packats upp sammanlagt i cachen, omtagningar inräknade

    const GeometryLayout* layout() const;       // nullptr innan open lyckats

private:
    // På heapen, så att cursorns pekare till layouten håller när segmentet flyttas.
    std::unique_ptr<GeometryLayout> layout_;
    GeometryCursor cursor_;
    const uint8_t* file_ = nullptr;
    size_t file_size_ = 0;
};

} // namespace HNwd::detail
