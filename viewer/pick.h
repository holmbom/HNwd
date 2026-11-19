#pragma once

// Plockning mot bitarnas geometri: strålen mot en bits trianglar, och bitarna
// som en stråle når i den ordning den når deras lådor. Ren std och HNwd — inga
// fönstertyper — så att samma kod går att använda utanför visaren och i en
// arbetstråd.
//
// Strålen är en början och en riktning i världen i meter. Riktningen behöver
// inte vara normerad: avstånden räknas längs strålen i meter (parametern gånger
// riktningens längd). En stråle med en riktning utan längd, eller med ett tal
// som inte är ändligt, träffar ingenting.
//
// Allt räknas i double. Hörnen i en del ligger som float relativt
// CellGeometry::origin; strålens början flyttas till samma ram före provet, så
// att stora världskoordinater inte kostar noggrannhet.

#include "cells.h"
#include "lod_tree.h"
#include "model.h"

#include <cstdint>
#include <vector>

namespace nwdview {

struct PickRay {
    double origin[3] = {0, 0, 0};
    double dir[3] = {0, 0, 1};   // behöver inte vara normerad
};

struct PickHit {
    bool hit = false;
    double distance = 0;          // längs strålen, i meter
    double point[3] = {0, 0, 0};  // världen, meter
    uint32_t fragment = HNwd::kNone;
};

// Hur stor del av varje del av en bit som ritas: de första `drawn[i]`
// trianglarna i del i (CellGeometry::parts[i]; en linjedel prövas aldrig). En
// tom lista är allt, och saknas en post för en del prövas hela delen. Ett tal över delens antal
// trianglar är hela delen. `clip` är snittet, eller nullptr utan.
struct PickLimits {
    std::vector<uint32_t> drawn;
    const ClipState* clip = nullptr;
};

// Närmaste träff mot trianglarna i bitens delar (ogenomskinligt, genomskinligt
// och delarna med bild; linjedelen prövas inte), framsida som baksida.
// Träffar på kanten och i hörnet räknas. En triangel som strålen ligger parallell med, och en
// utan yta, träffas inte. Träffen ska ligga längs strålen, på avståndet 0 eller
// längre, och högst `max_distance` bort; `max_distance` som är 0 eller negativt
// eller inte ändligt är obegränsat.
//
// En träff vars punkt är bortklippt av `limits.clip` (clipped_away) räknas
// inte; sökningen fortsätter med nästa träff längs strålen. Lika avstånd ger
// den lägsta delen och sedan den lägsta triangeln. Träffens fragment är det som
// delens `ranges` anger för triangeln. En triangel med ett index utanför
// delens hörn hoppas över.
//
// Genomgången är rak, en triangel i taget med ett tidigt avslag, och kostar
// tid i proportion till trianglarna som prövas; den ska köras i en arbetstråd.
// Funktionen läser bara `g` och `limits` och kan köras från flera trådar.
PickHit pick_geometry(const CellGeometry& g, const PickRay& ray, const PickLimits& limits,
                      double max_distance);

// En bit som strålen når, och avståndet längs strålen där den går in i lådan.
struct TileAlongRay {
    uint32_t tile = 0;     // index i LodTree::tiles
    double enter = 0;      // meter; 0 när strålen börjar i lådan
};

// De av de givna bitarna (index i LodTree::tiles) vars låda (Tile::bounds)
// strålen når inom `max_distance` meter, närmast först: växande `enter`, och
// vid lika avstånd bitens nummer. `max_distance` som är 0 eller negativt eller
// inte ändligt är obegränsat. Lådan är sluten: en stråle som tangerar en sida,
// ligger i ett sidoplan eller löper parallellt med en axel längs en kant
// träffar den. En bit bakom strålens början räknas inte, och en bit med en
// ogiltig låda eller ett index utanför trädet hoppas över. `out` töms först;
// en bit som står flera gånger i `tiles` ger lika många poster.
void tiles_along_ray(const LodTree& tree, const std::vector<uint32_t>& tiles, const PickRay& ray,
                     double max_distance, std::vector<TileAlongRay>& out);

} // namespace nwdview
