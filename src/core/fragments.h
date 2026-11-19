#pragma once
#include "inflate.h"
#include "material.h"
#include "nwd_error.h"
#include "pose.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace HNwd::detail {

// Paketet LcOpNwdFragments: var varje geometribit sitter och hur den ser ut.
// En objektström:
//
//   [int32 n] n × PTR    nollor upp till 8
//
// Varje rot är ett NYTT objekt av klass 13, aldrig null och aldrig en
// bakåtpekare.
//
//   13 fragment    [f64 D][i32 a][i32 b][i32 c][i32 seq][ptr pose][ptr utseende][ptr geometri]
//   14 pose        [3 × f64 t]
//   15 pose        [3 × f64 t][4 × f64 q (x, y, z, w)]
//   16 pose        klass 15 + [f64 s]
//   17 pose        [3 × (3 × f32, f32 0)][f32 1] PAD8 [3 × f64 t]
//   55 utseende    [i32 flags][ptr 54]  + [ptr 185 eller null] om flags & 32
//                                        + [i32 0 eller 1] om flags & 64
//   54 material    14 × f32
//   93 geometri    [6 × f32 bbox: min xyz, max xyz][3 × f64 c][f32 f][i32 key][i32 rec]
//
// Posen (14, 15, 16) är ett nytt objekt, aldrig en bakåtpekare. Den får vara
// null, och fragmentet har då ingen rotation eller skala. Utseendet (55),
// dess material (54) och geometrireferensen (93) är aldrig null men får vara
// bakåtpekare: flera fragment delar dem.
//
// Världskoordinat = s · R(q) · v + t, där R(q) är rotationsmatrisen för det
// normerade q. Klass 14 har ingen rotation och s = 1, klass 15 har s = 1.
//
// Klass 17 är en full matris, för en placering med olika skala per axel:
// världskoordinat = M · v + t. De tolv första talen är matrisens tre KOLUMNER,
// var och en följd av en nolla, och talet efter dem är 1 — en 4 × 4-matris
// kolumnvis, med flytten utbruten i dubbel precision.
//
// Utseendets flaggor: 1, 16, 32 och 64 förekommer. Bara 32 och 64 har ett
// eget fält; en annan bit är fel, eftersom den kunde betyda ett till.
//
// Postens positioner v är lagrade kring en mittpunkt, c i klass 93. Posens
// flytt t bär redan den: t är nodernas flytt i trädet tillämpad på c. Ett
// fragment utan pose hör till ett objekt utan flytt i trädet, och står i c:
// t = c.
//
// Klass 185 är materialet som JSON med sina filer (se material.h). Den får
// vara null eller en bakåtpekare: flera utseenden delar den. Varje klass-185-
// objekt sparas en gång i FragmentStore::materials.
//
// Ett utseende i tabellen är ett par: de 14 talen ur ett klass-54-objekt och
// ett klass-185-objekt eller inget. Två utseenden (klass 55) som har samma par
// får samma index, också när de är olika objekt i strömmen.

// Ett utseende: 14 flyttal i filens ordning och materialet.
struct LookData {
    float v[14];   // ambient 0–2, diffuse 3–5, specular 6–8, emissive 9–11, shininess 12, transparens 13
    uint32_t material = kNoMaterial;   // index i FragmentStore::materials; kNoMaterial om inget
};

// En geometrireferens (klass 93): posten och dess låda. Klass-93-objekt med
// samma post och samma låda (bit för bit) lagras en gång och delas av de
// fragment som pekar på dem; mittpunkten c behövs bara under tolkningen.
struct GeometryRefData {
    float bbox[6];    // geometrins låda i postens egna koordinater: min xyz, max xyz
    int32_t record;   // geometripostens nummer, 1-baserat, som det står
};

// Fält vars betydelse inte är känd. Bara för verktygen.
struct FragmentExtra {
    double d;
    int32_t b, c, key, appearance_flags;
    double center[3];
    float f;
};

// Fragmenten, kompakt: 24 byte per fragment, posens tal i en pool och det som
// delas (geometrireferenser, utseenden, material) en gång.
//
// Posens tal ligger i poolen efter klassen:
//
//   14     t (3)
//   15     t (3), q (4)
//   16     t (3), q (4), s
//   17     m radvis (9), t (3)
//   null   t (3), som är geometrins mittpunkt c
//
// Poolen är block om kPoseBlock tal; en pose ligger alltid inom ett block, så
// att poolen växer utan att flyttas.
struct FragmentStore {
    struct Item {
        uint32_t geomref;      // index i geomrefs
        uint32_t look;         // index i looks
        uint32_t pose;         // posens första tal i poolen (se pose_values())
        int32_t seq;           // preorder-index för det löv i trädet fragmentet hör till, som det står
        int32_t primitives;    // a: antal trianglar eller segment i posten, som det står
        uint8_t pose_class;    // 14, 15, 16 eller 17; 0 om posen är null
    };

    static constexpr uint32_t kPoseBlock = 65536;

    std::vector<Item> items;                       // i filens ordning
    std::vector<std::vector<double>> pose_blocks;  // poolen
    std::vector<GeometryRefData> geomrefs;         // ett per par av post och låda, i den ordning paren först förekommer
    std::vector<LookData> looks;                   // ett per par av klass 54 och klass 185, i den ordning paren först förekommer
    std::vector<MaterialData> materials;           // ett per klass-185-objekt under ett utseende

    size_t size() const { return items.size(); }

    // Fragment i:s pose. i < size().
    void pose(size_t i, PoseD& out) const;

    // Posens tal i poolen för fragment i, i ordningen ovan. i < size().
    const double* pose_values(size_t i) const {
        const uint32_t at = items[i].pose;
        return pose_blocks[at / kPoseBlock].data() + at % kPoseBlock;
    }

    // Antalet tal i poolen för en poseklass (0 för null).
    static uint32_t pose_size(uint8_t pose_class);

    // Minnet som lagret håller: vektorernas kapacitet och materialens text och byte.
    size_t memory_bytes() const;
};

// Tolkar paketet ur en uppackad buffert. `out` ersätts, och `extra` (om given)
// får en post per fragment.
//
// CorruptObject: brott mot objektströmmens regler, ett flyttal i en pose, en
// låda eller ett material som inte är ändligt, s == 0, en kvaternion vars
// längd avviker från 1 med mer än 1e-6, en matris i klass 17 utan sin form eller med determinanten 0,
// en okänd bit i utseendets flaggor, en klass 185 vars JSON inte går att
// tolka (se material.h), eller ett antal fragment som inte ryms (ett fragment
// tar minst 52 byte: utan pose, och med utseende och geometri som
// bakåtpekare). LimitExceeded: nästlingsdjupet, eller ett materials JSON över
// taket `max_json_bytes` (material.h). `detail` börjar med paketets namn.
// Efter ett fel är `out` och `extra` tomma.
ErrorCode parse_fragments(const uint8_t* data, size_t size, FragmentStore& out,
                          std::vector<FragmentExtra>* extra = nullptr,
                          std::string* detail = nullptr,
                          size_t max_json_bytes = kDefaultMaxMaterialJsonBytes);

// Samma tolkning direkt ur zlib-strömmen, genom ett fönster om minst
// `window_bytes`: den uppackade strömmen ligger aldrig hel i minnet.
// `stream` ska stå i strömmens början (efter open()); efter Ok är strömmen
// läst till sitt slut och Adler-32 prövad.
//
// Antalet fragment går inte att pröva mot strömmens längd förrän den är
// läst, så ingenting allokeras efter antalet: lagret växer med det som
// faktiskt läses.
//
// `progress(done, total)` anropas med antalet lästa fragment och antalet i
// strömmen, före det första, var kProgressEvery:e och efter det sista. Svarar
// den false avbryts tolkningen med Cancelled. Utöver felen ovan lämnas
// strömmens egna fel vidare (CorruptStream, StreamTooLarge).
inline constexpr size_t kStreamWindowBytes = size_t(16) << 20;
inline constexpr uint32_t kProgressEvery = 16384;

ErrorCode parse_fragments(InflateStream& stream, FragmentStore& out,
                          const std::function<bool(uint64_t, uint64_t)>& progress,
                          std::vector<FragmentExtra>* extra = nullptr,
                          std::string* detail = nullptr,
                          size_t window_bytes = kStreamWindowBytes,
                          size_t max_json_bytes = kDefaultMaxMaterialJsonBytes);

} // namespace HNwd::detail
