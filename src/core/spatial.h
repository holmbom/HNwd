#pragma once
#include "nwd_error.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace HNwd::detail {

// Paketet LcOpNwdSpatialHierarchy: ett träd över fragmenten, där varje löv
// samlar fragment som ligger nära varandra. En ström av int32, utan
// objektpekare, utan lådor och utan utfyllnad:
//
//   NOD := [i32 5][i32 k] k × NOD                  inre nod, k >= 1
//        | [i32 2][i32 n] n × ([i32 1][i32 f])     löv med n >= 1 poster
//        | [i32 4][i32 f]                          löv med ett enda fragment
//
// f är ett fragmentindex, 0-baserat, i LcOpNwdFragments ordning. Strömmen är
// exakt en NOD, roten, och slutar där. Löven innehåller tillsammans varje
// fragment exakt en gång: posterna är en permutation av 0 .. N-1.
//
// I de filer som är prövade har en inre nod 2–7 barn. Roten får vara ett löv.

struct SpatialTree {
    // En nod i trädet. En inre nod har barn och inga egna poster; ett löv har
    // poster och inga barn. För en inre nod är item_begin/item_count hela
    // delträdets fragment, som ligger i följd i `items`.
    struct Node {
        uint32_t parent = UINT32_MAX;   // UINT32_MAX (kNone) för roten
        uint32_t child_begin = 0;       // index i `children`
        uint32_t child_count = 0;
        uint32_t item_begin = 0;        // index i `items`
        uint32_t item_count = 0;
    };
    std::vector<Node> nodes;          // preorder; [0] är roten
    std::vector<uint32_t> children;   // varje nods barn i följd; index i nodes
    std::vector<uint32_t> items;      // fragmentindex i lövordning; ett delträds fragment ligger i följd
};

// Tolkar paketets uppackade ström. `fragment_count` är antalet fragment i
// LcOpNwdFragments. Tolkningen använder en explicit stack, och varje antal
// prövas mot de byte som återstår innan något allokeras: k barn eller n
// poster ska rymmas i resten av strömmen tillsammans med de barn till andra
// noder som har en plats men ännu inte är lästa, så att barnplatserna
// sammanlagt aldrig är fler än strömmens längd / 8.
//
// Grammatiken ovan är hela formen: en annan tagg är ett trasigt paket.
//
// `out` ersätts, och är tomt efter ett fel. `detail` får formen
// "LcOpNwdSpatialHierarchy at byte <n>: <regel>".
//   CorruptPackage:  en nodtagg som inte är 5, 2 eller 4, en posttagg som inte
//                    är 1, k <= 0, n <= 0, ett antal som inte ryms i resten
//                    av strömmen, en avhuggen ström, eller byte efter roten
//   CorruptLink:     ett fragmentindex utanför 0 .. fragment_count-1, ett
//                    fragment som står två gånger, eller ett som saknas
//   LimitExceeded:   en nod djupare än kMaxNesting (roten har djupet 0), eller
//                    fler noder än ett uint32-index rymmer
ErrorCode parse_spatial_hierarchy(const uint8_t* data, size_t size, uint32_t fragment_count,
                                  SpatialTree& out, std::string* detail = nullptr);

} // namespace HNwd::detail
