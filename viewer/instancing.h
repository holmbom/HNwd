#pragma once

// Instanser för upprepad geometri: fragment som visar samma geometripost,
// grupperade så att posten kan ritas en gång med en placering per fragment.
// Ren std och HNwd — inga fönstertyper — så att samma kod går att använda
// utanför visaren.
//
// Ritanropen är begränsade, så bara poster som många fragment visar blir
// grupper; resten ritas som vanligt ur bitarna. Det som går att avgöra utan att
// avkoda geometrin avgörs här: postens antal fragment, fragmentets utseende och
// om dess placering går att skriva som vridning och skala per axel. Vilken
// sorts geometri en post har (trianglar, linjer, punkter, text) står först i
// den avkodade posten, så en grupp kan hamna på en post som inte är trianglar;
// InstancingSettings::accept_record låter anroparen avgöra det med postens
// mesh innan gruppen räknas.

#include "model.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <stop_token>
#include <vector>

namespace nwdview {

// Byte per instans på grafikkortet.
inline constexpr size_t kInstanceBytes = 52;

// En placering i ramverkets konvention: världen = origo + at + R · (skala ·
// läget), där läget är prototypens koordinat och R = Rz(−yaw) · Rx(pitch) ·
// Ry(roll). Vinklarna är i grader; yaw räknas medurs ovanifrån som en kompass
// (prototypens +y pekar mot bäringen yaw), pitch lyfter +y, roll sänker +x.
// Skalan verkar före vridningen. `at` är i världen, i meter, i double.
struct InstancePose {
    double at[3] = {0, 0, 0};
    float yaw = 0, pitch = 0, roll = 0;
    float scale[3] = {1, 1, 1};
};

// Posen som en placering. Prototypens läge för postens punkt v är
// (v − center) · metres_per_unit: postens lägen relativt `center` (postens
// lådas mitt, i postens enhet) skalade till meter. Då gäller för varje punkt v
//
//   world_point(pose, v) · metres_per_unit
//       = at + R · (scale · ((v − center) · metres_per_unit))
//
// till ungefär sex värdesiffror (vinklarna och skalorna är float). `scale` är
// posens skala utan metres_per_unit; `at` är postens läge för `center` i meter.
//
// Posens linjära del M (s · R(q), eller matrisen M) ska vara en vridning gånger
// en skala per axel, M = R · diag(S): matrisens tre kolumner står då
// vinkelrätt mot varandra. Kolumnerna får avvika från vinkelrätt med högst
// 1e-5 i cosinus; R görs om till en exakt vridning. Svaret är false när
// kolumnerna inte är vinkelräta (skjuvning), när en kolumn har längden noll,
// när något tal inte är ändligt eller inte ryms som float, och när
// metres_per_unit inte är positivt och ändligt; `out` är då orörd.
//
// Spegling: en pose med negativ determinant ger en NEGATIV skala på den tredje
// axeln (scale[2] < 0), R är alltid en riktig vridning (determinant +1) och de
// två första skalorna är positiva. Ramverket tål det: en negativ skala vänder
// vindningen, och dess program räknar med det för framsidan och normalen.
// Visarens program lyser ytor lika från båda sidor och gallrar inga baksidor,
// så vindningen spelar ingen roll i visaren. Prototypen byggs ur postens egen
// mesh (Document::record_mesh): GeometryReader::fragment_mesh vänder
// trianglarnas ordning för ett fragment med spegling, och en prototyp läst
// genom ett sådant fragment skulle därför vändas två gånger.
//
// Pitch ligger i [−90, 90] och yaw och roll i [−180, 180]. Vid pitch ±90, där
// yaw och roll ger samma vridning, är roll 0.
bool instance_pose(const HNwd::Pose& pose, double metres_per_unit, const float center[3],
                   InstancePose& out);

// En grupp: en geometripost som minst InstancingSettings::min_count fragment
// visar och som går att rita som instanser.
struct InstanceGroup {
    uint32_t record = 0;      // Fragment::record, 1-baserat
    uint32_t triangles = 0;   // postens primitiver (Fragment::primitives), en gång
    float max_size = 0;       // det största fragmentets diagonal i världen, meter
    // Gruppens fragmentindex i dokumentet, i växande ordning. Varje sådant
    // fragment har instance_pose() == true, ett ogenomskinligt utseende utan
    // bild, och en ändlig världslåda. Fragment med samma post som inte uppfyller
    // det ingår inte.
    std::vector<uint32_t> fragments;
};

struct InstancingSettings {
    uint32_t min_count = 16;           // minst så många instansierbara fragment per post; minst 2
    uint32_t max_triangles = 65'536;   // poster med fler primitiver tas inte med
    size_t max_groups = 4'096;         // fler grupper: de som sparar mest behålls
    // Arbetstrådar över fragmenten (1 till 32). Resultatet är detsamma oavsett
    // antal.
    unsigned threads = 1;
    // Avgör om en post som uppfyller de andra villkoren får bli en grupp, till
    // exempel genom att avkoda den och se att den är trianglar och saknar
    // hörnfärger med alfa. Anropas i den här tråden, efter att fragmenten
    // räknats, för kandidaterna i ordningen de sparar mest (post i växande ordning
    // vid lika), tills max_groups grupper tagits emot; en post som avvisas tas inte
    // med och ersätts av nästa. Tom funktion: alla kandidater tas emot.
    std::function<bool(uint32_t record)> accept_record;
};

struct InstancePlan {
    std::vector<InstanceGroup> groups;   // de som sparar mest först: primitiver × (antal − 1), post vid lika
    uint64_t fragments = 0;              // summan av gruppernas fragment
    // Uppskattat sparat grafikminne: för varje grupp primitiverna × (antal − 1)
    // × (kGpuBytesPerVertex + kIndexBytesPerTriangle), ett hörn per primitiv,
    // minus kInstanceBytes per instans. Summan av grupperna, aldrig under 0.
    uint64_t saved_bytes = 0;
};

// Grupperar fragmenten efter Fragment::record. En post blir en grupp när minst
// min_count av dess fragment går att instansiera, den har högst max_triangles
// primitiver och accept_record tar emot den. Ett fragment går att instansiera
// när
//
// - posten finns (Fragment::record är 1 till record_count) och har primitiver,
// - utseendet är ogenomskinligt (genomskinligheten ger alfa minst 0,996, som
//   bitarnas ritning räknar) och dess material saknar bild (Material::has_texture),
//   eller utseendet inte finns,
// - världslådan är ändlig, och
// - posen går att skriva som en placering (instance_pose() med fragmentets
//   postlåda som center).
//
// Fragment som inte går att instansiera står kvar utanför gruppen. Antalet
// primitiver i en grupp är det som står på fragmenten. Per fragment räknas
// bara det som Fragment och dess utseende säger; hörnfärger och geometrins
// sort avgörs inte här utan genom accept_record.
//
// Fragmenten läses i `threads` trådar och resultatet är detsamma oavsett antal.
// Svaret är false när `stop` begärs (out är då tomt). En modell utan fragment
// ger en tom plan.
bool plan_instancing(const Model& m, const InstancingSettings& s, std::stop_token stop,
                     InstancePlan& out);

} // namespace nwdview
