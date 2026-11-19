#pragma once

// Ritbudgeten: ritningens tröskel, den lägsta storlek i px som en bit måste ha
// för att ritas. Ren std — ingen fönstertyp — så att samma kod går att
// använda utanför visaren.
//
// Ritningens tröskel är minst minnets tröskel och styrs av vad som kostar per
// bild: trianglar och ritanrop. Anroparen räknar belastningen vid en given
// tröskel (`load_at`: de visade bitarnas trianglar och ritanrop när bara det
// som mäts minst så många px ritas), och regulatorn svarar med den lägsta
// tröskeln vars belastning ryms inom de gällande taken.
//
// De gällande taken börjar på `start_scale` gånger inställningarnas
// max_triangles och max_draws (klämt till [golvet, 1]) och flyttas av den
// uppmätta bildtiden:
//  - Bildtiden jämnas ut med medianen av de fem senaste proven, så att en
//    enstaka lång bild inte slår igenom. Ett nytt värde räknas först när det
//    finns tre prov.
//  - Över målet (target_ms) sänks båda taken med samma faktor, målet delat
//    med den utjämnade bildtiden men inte under en halv, och de utjämnade
//    proven glöms så att nästa värde mäter bilderna efter ändringen.
//  - Under 70 % av målet höjs båda taken med 15 % vid varje anpassning, aldrig
//    över inställningarnas tak, och de utjämnade proven glöms så att nästa
//    anpassning mäter bilderna efter höjningen. Från start_scale 0,2 till
//    inställningarnas tak tar det tolv höjningar, alltså ungefär tre
//    bilder per höjning när grafikkortet orkar.
//  - Taken sänks aldrig under en tjugondel av inställningarnas tak (golvet).
//  - Mellan 70 % och 100 % av målet står de kvar.
//  - En bildtid 0 är okänd: ingenting läggs till i proven och taken står kvar.
//
// Svaret har hysteres så att tröskeln inte fladdrar: den höjs genast när
// belastningen vid den nuvarande tröskeln är över taken, och sänks först när
// belastningen vid en tröskel minst en tiondel lägre ryms inom nio tiondelar
// av taken; då sänks den dit där nio tiondelar ryms.
//
// reduce_quality() säger när regulatorn inte räcker: den utjämnade
// bildtiden är över målet trots att tröskeln står på max_pixels eller taken
// står på sitt golv. Anroparen sänker då det som regulatorn inte styr
// (kantutjämningen). Den faller tillbaka först när den utjämnade bildtiden är
// under 85 % av målet.
//
// Med `deterministic` används bara inställningarnas tak, hela, oavsett
// start_scale: bildtiden läses inte, taken, proven och reduce_quality()
// lämnas orörda, och samma belastning ger samma svar. Det enda som skrivs är
// pixels().

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace nwdview {

// Det som ritas vid en tröskel: trianglar och ritanrop per bild.
struct DrawLoad {
    uint64_t triangles = 0;
    uint32_t draws = 0;
};

struct GovernorSettings {
    uint64_t max_triangles = 60'000'000;   // tak för trianglar per bild
    uint32_t max_draws = 800;              // tak för ritanrop per bild; de utfärdas i UI-tråden
    double target_ms = 1000.0 / 45;        // målet för bildtiden
    double min_pixels = 3, max_pixels = 32;   // svarets intervall
    double start_scale = 0.2;   // takens andel när regleringen börjar; höjs medan bildtiden håller
};

class DrawGovernor {
public:
    // Ogiltiga värden rättas: tak till minst 1, målet till minst 1e-3 ms,
    // min_pixels till minst 1e-6 och max_pixels till minst min_pixels. Takens
    // andel i början, start_scale, rättas till intervallet från golvet (en
    // tjugondel) till 1; ett tal som inte är ett tal blir golvet.
    explicit DrawGovernor(const GovernorSettings& s);

    // Ritningens tröskel i px: den lägsta inom [min_pixels, max_pixels] vars
    // belastning ryms inom de gällande taken, och max_pixels när ingen gör
    // det. `load_at(px)` ger belastningen vid tröskeln px och ska avta (eller
    // vara lika) när px växer; den anropas högst ungefär femton gånger per
    // svar och två gånger när belastningen ligger stilla. `frame_ms` är den
    // uppmätta bildtiden, 0 när den är okänd.
    double next(const std::function<DrawLoad(double pixels)>& load_at, double frame_ms,
                bool deterministic);

    // Det senaste svaret; min_pixels före det första.
    double pixels() const { return pixels_; }

    // Sant när regulatorn inte räcker och kantutjämningen bör sänkas.
    bool reduce_quality() const { return reduce_; }

    // De gällande taken: inställningarnas tak gånger taktalet, aldrig under
    // golvet (en tjugondel) och aldrig under 1.
    uint64_t triangle_cap() const;
    uint32_t draw_cap() const;
    // Den senast räknade utjämnade bildtiden, ms; 0 innan tre prov getts.
    double smoothed_ms() const { return smoothed_ms_; }

private:
    bool fits(const DrawLoad& l, double factor) const;
    double search(const std::function<DrawLoad(double)>& load_at, double factor, double lo,
                  double hi, bool lo_unfit, bool hi_fit) const;
    bool add_sample(double ms);
    void adapt();

    static constexpr size_t kWindow = 5;
    static constexpr size_t kMinSamples = 3;

    GovernorSettings s_;
    double pixels_ = 0;
    double scale_ = 1;                 // de gällande takens del av inställningarnas; börjar på start_scale
    double smoothed_ms_ = 0;
    bool reduce_ = false;
    std::array<double, kWindow> samples_{};
    size_t sample_count_ = 0, sample_next_ = 0;
};

} // namespace nwdview
