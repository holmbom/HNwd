#pragma once

// Detaljträdet: modellens fragment i ett träd av bitar som byggs, visas och
// släpps var för sig. Ren std och HNwd — inga fönstertyper — så att samma kod
// går att använda utanför visaren.
//
// Varje fragment med en låda hör till exakt en bit (Tile), och varje bit hör
// till en nod. Ett fragment med diagonalen s placeras i den översta nod på
// vägen från sitt löv till roten vars diagonal D uppfyller s >= alpha * D, och
// annars i lövet. Stora saker ligger då i stora noder och små i små: en nods
// egna fragment ligger inom ungefär en oktav. Bitens fragment står med de
// största först, och bitens låda är fragmentens egen.
//
// Garantin som urvalet bygger på: för en bit b med största fragment s_max och
// lådan B, och ett fragment f i b med diagonalen s_f och lådan F, gäller
// s_f <= s_max och avstånd(öga, F) >= avstånd(öga, B). Fragmentets vinkelstorlek
// s_f / avstånd(öga, F) är alltså aldrig större än bitens s_max / avstånd(öga,
// B). Varje fragment som syns minst N px ligger i en bit som mäts minst N px,
// hur fragmenten än grupperas.

#include "model.h"

#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <vector>

namespace nwdview {

// En bit: fragmenten LodTree::frags[frag_begin, frag_begin + frag_count),
// störst först.
struct Tile {
    Box bounds;                         // fragmentens låda, meter
    float max_size = 0, min_size = 0;   // fragmentens diagonal, meter
    uint32_t node = 0;
    uint32_t frag_begin = 0, frag_count = 0;   // LodTree::frags, störst först
    uint64_t primitives = 0;
    bool pinned = false;                // grundbilden: byggs först, släpps aldrig
};

// En nod i detaljträdet.
struct LodNode {
    Box bounds;                         // lådan kring delträdets fragment, meter
    float max_size = 0;                 // största fragmentet i delträdet; 0 utan fragment
    uint32_t parent = HNwd::kNone;
    uint32_t first_child = 0, child_count = 0;   // LodTree::children
    uint32_t first_tile = 0, tile_count = 0;     // LodTree::tiles
};

// Prefixet av en bits fragment som är minst en viss storlek: eftersom
// fragmenten står störst först är det de första `fragments`.
struct TilePrefix {
    uint32_t fragments = 0;     // antalet fragment i prefixet
    uint64_t primitives = 0;    // deras primitiver
    float omitted = 0;          // det största fragmentet som inte är med; 0 när alla är med
};

// Trädet. Noderna står så att en förälder kommer före sina barn, med roten
// som [0]; en nods barn ligger i följd i `children`, och en nods bitar ligger
// i följd i `tiles`. En nods bitar hör till noden (Tile::node). En nod kan
// sakna fragment i delträdet: den har då en ogiltig låda och max_size 0.
//
// Fragmenten i bitarna står i `frags`, bit för bit i samma
// ordning som bitarna, och `sizes` har deras storlek på samma platser. Ett
// fragment utan en ändlig låda ligger i ingen bit. `prefix_primitives` har på
// samma platser summan av primitiverna i bitens fragment från det första till
// och med det här (mättad vid 2^32 − 1), så att primitiverna i ett prefix är
// ett uppslag.
struct LodTree {
    std::vector<LodNode> nodes;         // [0] roten, föräldern före barnen
    std::vector<uint32_t> children;
    std::vector<Tile> tiles;
    std::vector<uint32_t> frags;        // fragmentindex i dokumentet
    std::vector<float> sizes;           // diagonalen per post i frags, meter
    std::vector<uint32_t> prefix_primitives;   // per post i frags: primitiverna från bitens första
    std::vector<uint32_t> tile_of;      // per fragment i dokumentet; HNwd::kNone utan låda
    size_t memory_bytes() const;        // det vektorerna håller

    // Bit `tile`s fragment med diagonal minst `min_size`: en binärsökning över
    // `sizes`, så kostnaden är logaritmisk i bitens fragment.
    // `min_size` som inte är över bitens minsta fragment (Tile::min_size) ger
    // hela biten. Har biten inga storlekar i `sizes` (ett träd som inte
    // planerats av plan_lod_tree) är alla fragment med och `omitted` är
    // min_size. Utan prefixsummor räknas primitiverna proportionellt mot
    // antalet fragment. En bit utanför trädet ger ett tomt prefix.
    TilePrefix prefix_at_least(uint32_t tile, float min_size) const;
};

// Inställningarna för plan_lod_tree(). Ett värde utanför det giltiga
// intervallet klipps: alpha till minst 0, max_leaf_fragments och
// max_tile_triangles till minst 1, threads till 1..32.
struct LodTreeSettings {
    double alpha = 1.0 / 32;            // en nod tar fragment med s >= alpha * nodens diagonal
    uint32_t max_leaf_fragments = 64;   // ett löv med fler delas vid medianen
    uint64_t max_tile_triangles = 32'768;   // tak per bit; ett ensamt fragment med fler blir en egen bit
    uint64_t min_tile_triangles = 2'048;    // en nod med färre egna trianglar lämnar dem till föräldern
    size_t pinned_bytes = 0;            // grundbildens del av budgeten
    double bytes_per_primitive = 72;    // uppskattningen före bygget
    unsigned threads = 1;
    // En flagga per fragment i dokumentet (0 eller 1). Ett fragment med flaggan
    // satt läggs i ingen bit: tile_of är HNwd::kNone för det, och det räknas inte
    // i nodernas lådor, max_size eller bitarnas tal, precis som ett fragment
    // utan ändlig låda. Ett fragment vars index ligger utanför vektorn hoppas
    // inte över. nullptr: inget hoppas över. Vektorn ska leva under anropet.
    const std::vector<uint8_t>* skip = nullptr;
};

// Vad planeringen gjorde. moved_up räknar fragment som ligger i en annan nod
// än det löv de låg i; split_nodes de delningar av stora löv som gjordes.
struct LodTreeStats {
    double seconds = 0;
    uint32_t split_nodes = 0, pinned_tiles = 0;
    uint64_t moved_up = 0, pinned_primitives = 0;
};

// Planerar trädet för modellen.
//
// 1. Fragmentens diagonal och lådor räknas, i `threads` trådar. En nod i
//    modellens rumsliga index utan fragment ger ingen nod.
// 2. Ett löv med fler än max_leaf_fragments fragment delas vid medianen av
//    fragmentens mitt längs lådans längsta axel, rekursivt, så att varje
//    halva har lika många fragment (högst en fler i den ena).
// 3. Nodernas lådor är unionen av fragmentens lådor, och fragmenten placeras
//    enligt alpha. En nod med färre än min_tile_triangles egna primitiver
//    lämnar dem till föräldern; roten behåller sina.
// 4. En nods egna fragment, i rumslig ordning, delas i bitar så att ingen får
//    fler än max_tile_triangles primitiver, utom en bit av ett ensamt
//    fragment. Inom varje bit står fragmenten störst först, jämna storlekar
//    efter fragmentindex, och prefix_primitives får sina summor.
// 5. max_size räknas nerifrån och upp.
// 6. De fasta bitarna märks: bitarna med störst max_size först, tills
//    summan av primitiver * bytes_per_primitive skulle överskrida
//    pinned_bytes; den första som inte ryms stoppar.
//
// Resultatet är detsamma oavsett antalet trådar. Planeringen använder ingen
// rekursion. Svaret är false när `stop` begärs (out och stats är då
// ofullständiga). En modell utan fragment ger ett tomt träd.
bool plan_lod_tree(const Model& m, const LodTreeSettings& s, std::stop_token stop,
                   LodTree& out, LodTreeStats& stats);

// En nod i trädet som en låda.
struct NodeBox {
    Box bounds;
    uint32_t node = 0;
};

// Högst `max_boxes` noder som tillsammans täcker trädet, för en översikt: roten
// delas i sina barn, och sedan den nod med störst diagonal som ryms, tills det
// inte går längre. Inga två av noderna är släkt med varandra. Noder utan låda
// tas inte med. Noderna står i växande nodordning.
std::vector<NodeBox> tree_boxes(const LodTree& t, size_t max_boxes);

} // namespace nwdview
