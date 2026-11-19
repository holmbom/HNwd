#pragma once
#include "hierarchy.h"
#include "nwd_error.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace HNwd::detail {

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
// Resultatet är detsamma som parse_hierarchy() i läget Full ger för noden:
// samma flikar i samma ordning, med samma egenskaper.
//
// Filen, `streams`, `h` och `skeleton` ska gälla så länge läsaren gör det.
// Läsaren håller några uppackningar på några hundra KB var, och är inte
// trådsäker; en tråd per läsare.
class TabReader {
public:
    TabReader(const uint8_t* file, size_t size, const HierarchyStreams& streams, const Hierarchy& h,
              const HierarchySkeleton& skeleton);
    ~TabReader();
    TabReader(const TabReader&) = delete;
    TabReader& operator=(const TabReader&) = delete;

    // Flikarna för den lagrade noden `stored_node` (index i Hierarchy::nodes).
    // `out` ersätts. OutOfRange: noden finns inte, eller en position ligger
    // utanför sin ström. CorruptLink: skelettet stämmer inte med strömmen.
    // Övriga fel som parse_hierarchy(), med strömmens namn och byte i `detail`.
    ErrorCode read(uint32_t stored_node, NodeTabs& out, std::string* detail = nullptr);

private:
    class Source;
    std::unique_ptr<Source> source_;
    const Hierarchy& h_;
    const HierarchySkeleton& skeleton_;
};

// En enda nod, med en läsare som bara lever under anropet.
ErrorCode read_node_tabs(const uint8_t* file, size_t size, const HierarchyStreams& streams,
                         const Hierarchy& h, const HierarchySkeleton& skeleton,
                         uint32_t stored_node, NodeTabs& out, std::string* detail = nullptr);

} // namespace HNwd::detail
