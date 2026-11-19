#pragma once
#include "hierarchy.h"
#include "nwd_error.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace HNwd::detail {

// Vad en TabReader har gjort sedan den skapades.
struct TabReaderStats {
    uint64_t reads = 0;        // read() som lyckades
    uint64_t node_hits = 0;    // av dem besvarade ur cachen
    uint64_t tab_hits = 0;     // delade flikar ur cachen
    uint64_t opens[3] = {};    // läsare öppnade per markör: 0 nodens attribut, 1 rötter, 2 delade flikar
    uint64_t restarts = 0;     // öppningar som började om från en kontrollpunkt eller strömmens början
    uint64_t inflated = 0;     // byte som uppackningarna gått fram, överhoppade inräknade
};

// Läser lagrade noders flikar och egenskaper ur filen i efterhand, enligt
// skelettet som parse_hierarchy() ger i läget Structure.
//
// En nods attribut läses om från sin position i trädets ström. Strömmen packas
// upp från närmaste kontrollpunkt före positionen, eller vidare från där läsaren
// stod om det är närmare; en nod längre fram i strömmen än den förra blir
// därför billig. Bakåtpekare till objekt före attributen slås upp i
// skelettets lista: ett namnpar kopieras ur `h`, och en flik som en annan nod
// äger läses om från sin egen position. En null-plats tar nästa egenskapsrot,
// som läses ur sin ström på samma sätt.
//
// Med ett tak över 0 håller läsaren en cache: de tolkade flikar som flera noder
// delar, och flikarna för de noder som läses med `keep`. Det som ligger där
// läses inte ur filen igen. Äldst använda kastas först när taket nås.
//
// Resultatet är detsamma som parse_hierarchy() i läget Full ger för noden:
// samma flikar i samma ordning, med samma egenskaper.
//
// Filen, `streams`, `h` och `skeleton` ska gälla så länge läsaren gör det.
// Läsaren håller några uppackningar på några hundra KB var, och är inte
// trådsäker; en tråd per läsare.
class TabReader {
public:
    // `cache_bytes`: taket för cachen över delade flikar och noder; 0 utan cache.
    TabReader(const uint8_t* file, size_t size, const HierarchyStreams& streams, const Hierarchy& h,
              const HierarchySkeleton& skeleton, size_t cache_bytes = 0);
    ~TabReader();
    TabReader(const TabReader&) = delete;
    TabReader& operator=(const TabReader&) = delete;

    // Flikarna för den lagrade noden `stored_node` (index i Hierarchy::nodes).
    // `out` ersätts. OutOfRange: noden finns inte, eller en position ligger
    // utanför sin ström. CorruptLink: skelettet stämmer inte med strömmen.
    // Övriga fel som parse_hierarchy(), med strömmens namn och byte i `detail`.
    //
    // `keep`: nodens flikar läggs i cachen, för en nod som sitter på flera
    // ställen i trädet. En nod som ligger i cachen läses inte ur filen.
    ErrorCode read(uint32_t stored_node, NodeTabs& out, std::string* detail = nullptr,
                   bool keep = false);

    size_t cached_bytes() const;   // byte i cachen; högst taket efter varje anrop
    TabReaderStats stats() const;

private:
    class Source;
    class Cache;
    std::unique_ptr<Source> source_;
    std::unique_ptr<Cache> cache_;   // nullptr när taket är 0
    const Hierarchy& h_;
    const HierarchySkeleton& skeleton_;
    uint64_t reads_ = 0;
    uint64_t node_hits_ = 0;
};

// En enda nod, med en läsare som bara lever under anropet.
ErrorCode read_node_tabs(const uint8_t* file, size_t size, const HierarchyStreams& streams,
                         const Hierarchy& h, const HierarchySkeleton& skeleton,
                         uint32_t stored_node, NodeTabs& out, std::string* detail = nullptr);

} // namespace HNwd::detail
