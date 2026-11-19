#pragma once

// Urvalet: vilka bitar som byggs, i vilken ordning, vilka som visas, och vilka
// som släpps när minnesbudgeten är nådd. Ren std — kameran är en
// perspektivkamera i världen — så att samma kod går att använda utanför
// visaren.
//
// En bit b mäts i px som P_b = s_max(b) · K / d(öga, b.låda), med K =
// pixels_per_radian() och d avståndet till lådans närmaste punkt. Bitens
// största fragment kan inte synas större än så, och inget fragment i biten
// ligger närmare än lådan (lod_tree.h), så varje fragment som syns minst N px
// ligger i en bit som mäts minst N px. Mätningen vandrar detaljträdet uppifrån
// och beskär en nod vars största fragment inte når min_pixels, så kostnaden
// växer med antalet bitar som behövs och inte med modellen.
//
// Tre trösklar styr:
//  - min_pixels: det som garantin gäller när allt ryms.
//  - threshold() (N_eff): minnets tröskel. Den höjs när det som behövs vid
//    min_pixels inte ryms i budgeten, högst till max_pixels, och byggs sedan
//    bara för bitar som mäts minst N_eff.
//  - ritningens tröskel, som anroparen ger shown_changes(): bitar som är
//    byggda men mäts under den parkeras, och visas igen utan ny uppbyggnad.
//
// Förutsägelsen: update() kan få en andra kamera, ögat om en stund längs
// rörelsen. Bitar som inte behövs nu men mäts minst N_eff för den kameran byggs
// efter allt som behövs nu, tar bara plats som annars är ledig och som inte
// något annat behöver, och räknas varken som hål, i latensen eller i minnets
// tröskel.
//
// Minnet räknas i RAM och på grafikkortet var för sig (PartBytes) och har en
// budget för vardera. En bit får plats bara om båda ryms; det som släpps
// räknas av i båda.
//
// En bit byggs till den storlek som behövs, inte alltid hel. Mäts bit b till
// P_b px är s_behövs = N_eff · s_max(b) / P_b (utan närhetsvikt N_eff · d / K)
// storleken på ett fragment som är precis N_eff px på lådans avstånd: ett
// fragment i biten som är minst N_eff px är minst så stort, eftersom inget
// fragment ligger närmare än lådan. Ett byggjobb får därför minsta
// fragmentstorleken s_behövs / 2 (Job::min_size) och bygger bara bitens
// fragment som är minst så stora, ett prefix av dem (LodTree::prefix_at_least).
// Är prefixet hela biten, och för en fast bit, är min_size 0. Minnet ett jobb
// behöver, och som N_eff räknas med, är prefixets. En byggd bit byggs om när
// dess största fragment som inte byggdes är minst s_behövs; den gamla visas
// tills den nya tas emot. Garantin: varje fragment som mäts minst N_eff
// ligger i en bit som är byggd så att fragmentet är med, eller som väntar på
// ett jobb som gör det, när urvalet står still.

#include "cells.h"
#include "lod_tree.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace nwdview {

// En perspektivkamera i världen, z upp, utan roll: ögat, kompassriktningen
// blicken går mot (0 = norr = +y, medurs) och hur högt ögat står över målet,
// synfältet på höjden och skärmens storlek i px.
//
// `near_radius` > 0 (gångläget): en låda inom så många meter från ögat räknas
// som synlig också bakom och bredvid blicken, så att detaljen runt
// betraktaren finns när han vänder sig om.
struct ViewFrustum {
    double eye[3] = {0, 0, 0};
    double forward[3] = {0, 1, 0};
    double right[3] = {1, 0, 0};
    double up[3] = {0, 0, 1};
    double tan_half_fov = 0.414;
    double width = 1, height = 1;   // px
    double near_radius = 0;         // meter; 0 = av

    static ViewFrustum orbit(const double eye[3], double azimuth_deg, double elevation_deg,
                             double fov_deg, double width, double height);

    // Px per radian: bildhöjden delad med 2 · tan(halva synfältet). Ett
    // föremål med storleken s meter på avståndet d meter är ungefär
    // s · pixels_per_radian() / d px. 0 när kameran saknar storlek eller
    // synfält.
    double pixels_per_radian() const;

    // Avståndet från ögat till lådans närmaste punkt, meter; 0 inuti lådan och
    // oändligt för en ogiltig låda.
    double distance(const Box& b) const;

    // Når lådan in i bilden? Falskt när hela lådan ligger utanför något av de
    // fyra sidoplanen eller bakom ögats plan, annars sant. En låda som ligger
    // inuti en annan syns alltså bara när den yttre gör det. near_radius
    // räknas inte här.
    bool sees(const Box& b) const;

    // Hur många px `size` meter vid punkten blir i bild, på punktens djup;
    // 0 bakom ögat.
    double pixels_at(const double p[3], double size) const;
};

class TileScheduler {
public:
    struct Settings {
        double min_pixels = 3;          // garantins tröskel
        double max_pixels = 32;         // högst så här högt höjs minnets tröskel
        double keep_factor = 0.5;       // byggt behålls före annat ned till tröskeln × detta
        // Byte för bitarnas meshar, i RAM och på grafikkortet.
        PartBytes budget{size_t(1500) << 20, size_t(1500) << 20};
        // En gemensam budget: budget.ram gäller RAM och grafikkort tillsammans
        // och budget.gpu används inte. För ett grafikkort som delar minne med
        // processorn.
        bool shared = false;
        // Ett byggjobb bygger ned till s_behövs / size_margin: marginalen innan
        // biten måste byggas om när ögat kommer närmare. Minst 1; en mindre
        // marginal tar mindre minne och bygger om oftare.
        double size_margin = 2;
    };

    // En ny modell: inget byggt, ingenting mätt. Trädet som ges här är det som
    // update() får därefter.
    void reset(const LodTree& tree, const Settings& s);

    // Mäter om med kameran som den är: trädet vandras uppifrån, bitar som
    // mäts minst min_pixels blir kandidater (störst först), och varje byggd
    // eller påbörjad bit mäts om. `now` är tiden i sekunder, för latensen.
    // Räknar om minnets tröskel, och kandidatlistan och listan av det som
    // får släppas.
    //
    // En låda inom near_radius räknas oavsett riktning, med halva vikten när
    // den ligger helt bakom ögats plan. En låda som varken når in i bilden
    // eller ligger inom near_radius mäts 0.
    //
    // Minnets tröskel räknas så att bitarna som mäts minst N_eff, och de fasta
    // bitarna, ryms i budgeten; en byggd bit räknas med sin verkliga storlek
    // och annars med uppskattningen för det prefix som N_eff ger (en byggd
    // bit som då skulle byggas om räknas med det större av de två). Tröskeln
    // är den lägsta, högst en procent över, där det ryms. Mellan två bitars P
    // är samma bitar med, men prefixen krymper när tröskeln växer: den hittas
    // med en binärsökning över bitarnas P och sedan en halvering mellan de två
    // närmaste. Varje prövning är en sökning över varje kandidats fragment.
    // Hysteres: tröskeln höjs genast när det inte ryms, och sänks först när
    // det som behövs vid den lägre tröskeln ryms i nio tiondelar av budgeten.
    void update(const LodTree& tree, const ViewFrustum& view, double now) {
        update(tree, view, nullptr, now);
    }

    // Som ovan, med `ahead`: kameran om en stund längs rörelsen (får vara
    // nullptr). En bit som inte behövs för `view` men mäts minst N_eff för
    // `ahead` är en förhandskandidat. Den byggs efter allt som behövs nu, och
    // bara om platsen finns utan att släppa något annat än det som varken
    // syns eller väntas av `ahead`. Den räknas inte som hål eller i
    // latensen och höjer inte N_eff. En byggd bit som `ahead` ser släpps efter
    // de bitar som ingen av kamerorna ser. Mäts på samma sätt som för `view`,
    // med `ahead`s egen near_radius. Fasta bitar är aldrig förhandskandidater.
    void update(const LodTree& tree, const ViewFrustum& view, const ViewFrustum* ahead,
                double now);

    // De påbörjade bitar (också ombyggnader) som vid senaste update() föll under
    // N_eff / 4, inte är fasta, inte ligger inom `view.near_radius` och inte
    // väntas av `ahead`.
    // De räknas nu som avbrutna på samma sätt som invalidate() gör med ett
    // pågående jobb: jobbet tas emot av tile_done() som ett jobb som inte
    // gav något och biten märks inte som misslyckad, så den byggs igen när
    // den behövs. Anroparen avbryter jobben. Bitens reserverade byte släpps
    // först när jobbet tas emot. Tömmer listan: varje bit kommer en gång.
    std::vector<uint32_t> take_cancellations();

    // Ett byggjobb: bygg bitens fragment som är minst `min_size` meter i
    // diagonal (BuildLimits::min_size); 0 är hela biten.
    struct Job {
        uint32_t tile = 0;
        float min_size = 0;
    };

    // Nästa jobb, de fasta bitarna först och sedan de som mäts minst N_eff,
    // viktigast först, högst `max_jobs`. Ett jobb är en bit som inte är byggd,
    // eller en byggd bit som ska byggas om till en mindre fragmentstorlek
    // (se överst); en ombyggnad har samma vikt som biten och ligger i samma
    // ordning som de andra. De räknas som påbörjade. Den gamla formen av en
    // bit som byggs om räknas som byggd och visad tills tile_done() tar emot
    // den nya, som ersätter den; en ombyggnad reserverar bara det som den nya
    // formen tar mer än den gamla. Får en bit plats bara genom att andra
    // släpps, läggs de i `evict` (sist, utan att tömma den): de är redan
    // borträknade och ska tas bort ur scenen. Släpps i den här ordningen:
    // byggda bitar utanför bilden (minst närhetsmätning först, och sist de som
    // `ahead` ser), sedan bitar i bild under N_eff × keep_factor, sedan bitar i
    // bild under N_eff, sedan bitar i bild med lägre P än den som väntar. Fasta
    // bitar, bitar med minst lika hög P som den som väntar och bitar som
    // byggs om släpps aldrig.
    std::vector<Job> next_build_jobs(size_t max_jobs, std::vector<uint32_t>& evict);

    // Ett bygge är klart. `min_size` är storleken jobbet byggdes till (Job::
    // min_size) och `bytes` det byggda; `ok` falskt för ett jobb som inte gav
    // något (ett fel), och biten prövas då inte igen (en ombyggnad som
    // misslyckas lämnar den gamla formen kvar och byggs inte om igen). Svarar
    // om den nya formen är kvar (räknas som byggd, och ersätter en gammal): den
    // ryms inte när budgeten inte går att frigöra, i RAM eller på grafikkortet,
    // och en bit som byggs om behåller då sin gamla form. En bit som tas emot
    // är inte visad; shown_changes() avgör när den visas, och en bit som
    // byggs om är visad som förut. Det som måste släppas läggs i `evict`. Ett
    // jobb för en bit som ogiltigförklarats eller avbrutits under tiden svarar
    // false och räknas inte som ett fel. `now` är tiden för latensen.
    bool tile_done(uint32_t tile, float min_size, PartBytes bytes, bool ok, double now,
                   std::vector<uint32_t>& evict);

    // Storleken den byggda biten byggdes till (Job::min_size); 0 är hela
    // biten. Meningslös för en bit som inte är byggd.
    float built_min_size(uint32_t tile) const;

    // Minne som kommit till en byggd bit efter bygget (plockningens sökträd):
    // det räknas till biten och släpps med den. En bit som inte är byggd
    // påverkas inte. Det kan lägga resident över budgeten; nästa bit som
    // behöver plats släpper då mer.
    void add_bytes(uint32_t tile, PartBytes extra);

    // Bitens byggda form gäller inte längre: den är borträknad och byggs om
    // när den behövs, också om bygget misslyckats. Svarar om den var byggd och
    // ska tas bort ur scenen.
    bool invalidate(uint32_t tile);

    // Byggda bitar som ska visas (mäts minst draw_pixels och är inte visade)
    // respektive parkeras (visade men mäts under draw_pixels · 0,8; hysteres),
    // högst `max_changes` tillsammans, viktigast först: de som visas största
    // P först, de som parkeras minsta P först, växelvis. Tömmer `show` och
    // `park` och räknar biterna som de säger som visade respektive inte
    // visade. Bitar utanför bilden mäts 0 och parkeras. En fast bit visas
    // så snart den är byggd, före alla andra, och parkeras aldrig.
    void shown_changes(double draw_pixels, size_t max_changes, std::vector<uint32_t>& show,
                       std::vector<uint32_t>& park);

    // De visade bitarna, närmast ögat först: avståndet från ögat till bitens
    // låda (ViewFrustum::distance), och bitens nummer vid lika avstånd. Tömmer
    // `out` först. Kostnaden följer antalet visade bitar.
    void shown_order(const ViewFrustum& view, std::vector<uint32_t>& out) const;

    double threshold() const { return threshold_; }   // minnets tröskel N_eff
    // Senaste mätningen: s_max·K/avstånd, med närhetsvikten; 0 för en bit som
    // inte mättes minst min_pixels i senaste update() och inte är byggd eller
    // påbörjad.
    double pixels(uint32_t tile) const;
    bool built(uint32_t tile) const;
    bool shown(uint32_t tile) const;
    // Ingenting byggs och ingenting mer önskas med kameran som den är.
    bool idle() const;
    // Bitar i bild som mäts minst tröskeln × factor och inte är byggda
    // (också påbörjade räknas), och byggda bitar vars största fragment som
    // inte byggdes är minst så stort i bild som tröskeln × factor.
    size_t holes(double factor) const;
    struct Latency {
        double p50 = 0, p95 = 0, max = 0;
        size_t count = 0;
    };
    // Från att en bit först behövs (mäts minst N_eff och är inte byggd) tills
    // tile_done() tar emot den, sekunder. En bit som slutar behövas innan den
    // är klar räknas inte.
    Latency latency() const;
    PartBytes resident() const { return resident_; }   // byggda bitar, med add_bytes
    PartBytes budget() const { return settings_.budget; }
    size_t pending() const { return pending_; }
    size_t built_count() const { return built_count_; }
    size_t shown_count() const { return shown_count_; }
    uint64_t evictions() const { return evictions_; }
    // Så många jobb har take_cancellations() gett sedan reset().
    uint64_t cancelled() const { return cancelled_; }

private:
    enum : uint8_t {
        kBuilt = 1,
        kPending = 2,
        kFailed = 4,
        kStale = 8,
        kShown = 16,
        kPinned = 32,
        kActive = 64,   // med i active_: byggd eller påbörjad
    };

    // Vad ett jobb bygger av en bit: minsta fragmentstorlek (0 = hela biten),
    // det största fragmentet som då inte är med (0 när alla är med) och
    // primitiverna i det som är med.
    struct Plan {
        float min_size = 0;
        float omitted = 0;
        uint64_t primitives = 0;
    };

    struct State {
        double pixels = 0;            // gäller när epoch == epoch_
        double ahead_pixels = 0;      // för `ahead`; gäller när ahead_epoch == epoch_
        double needed_since = -1;     // < 0: behövs inte
        uint64_t primitives = 0;      // hela bitens
        uint64_t known_primitives = 0;   // primitiverna i det som gav `known`
        PartBytes held;               // byggd: det byggda
        PartBytes reserve;            // påbörjad: det reserverade (en ombyggnad: bara tillskottet)
        PartBytes known;              // storleken när den byggts en gång
        Plan plan;                    // jobbet för den här mätningen; gäller när plan_epoch == epoch_
        Plan pend;                    // det påbörjade jobbet
        Plan have;                    // det byggda
        double need = 0;              // s_behövs, för plan
        float max_size = 0;           // bitens största fragment
        float potential = 0;          // som pixels, men utan hänsyn till vad som syns
        uint32_t epoch = 0;
        uint32_t ahead_epoch = 0;
        uint32_t needed_epoch = 0;
        uint32_t plan_epoch = 0;
        uint32_t refused_epoch = 0;   // fick inte plats i denna mätning
        uint32_t active_pos = 0;      // plats i active_ när byggd eller påbörjad
        uint32_t shown_pos = 0;       // plats i shown_ när visad
        uint8_t flags = 0;
    };

    // Det som kan släppas, utanför bilden först och sedan minst på skärmen,
    // med summan av byten fram till varje plats. Byggs när det behövs och
    // gäller till nästa mätning.
    struct Victim {
        uint32_t tile;
        // 0 utanför bilden, 1 utanför bilden men väntad av `ahead`, 2 i bild
        // under tröskeln × keep_factor, 3 i bild.
        uint8_t group;
        // Grupp 0: potential; grupp 1: pixels för `ahead`; grupp 2 och 3: pixels.
        double key;
    };
    struct Victims {
        bool valid = false;
        std::vector<Victim> list;
        std::vector<PartBytes> prefix;   // prefix[k] = summan av byten för list[0..k), viktade
        size_t plain_end = 0;            // list[0, plain_end) ligger utanför bilden och ingen väntar den
        size_t hidden_end = 0;           // list[0, hidden_end) ligger utanför bilden
        size_t taken = 0;                // de första som redan släppts
    };

    bool wants(const State& s) const;
    double current_pixels(const State& s) const { return s.epoch == epoch_ ? s.pixels : 0; }
    double current_ahead(const State& s) const {
        return s.ahead_epoch == epoch_ ? s.ahead_pixels : 0;
    }
    // Uppskattat minne för ett jobb med så många primitiver.
    PartBytes estimate(const State& s, uint64_t primitives) const;
    // Vad ett jobb för bit `tile` bygger när det behövs minst `need` meter: ett
    // prefix av fragmenten, eller hela biten.
    Plan plan_for(const LodTree& tree, uint32_t tile, double need, bool pinned) const;
    // Minnet bit `tile` tar vid tröskeln `threshold`, för minnets tröskel.
    PartBytes demand_at(const LodTree& tree, uint32_t tile, double threshold) const;
    // Är en byggd bit en ombyggnad värd i den här mätningen?
    bool rebuild_wanted(const State& s) const;
    void set_plan(const LodTree& tree, uint32_t tile, double pixels);
    PartBytes weigh(PartBytes b) const;
    PartBytes weighed_budget(double fraction) const;
    void compute_threshold(const LodTree& tree);
    void build_victims();
    // `pixels` är det den som väntar mäts; med `ahead_job` får bara det som
    // ingen av kamerorna ser släppas.
    bool make_room(PartBytes need, double pixels, bool ahead_job, std::vector<uint32_t>& evict);
    void set_shown(uint32_t tile);
    void clear_shown(uint32_t tile);
    // Ordnar active_ efter flaggorna: med när byggd eller påbörjad.
    void sync_active(uint32_t tile);
    void drop_built(uint32_t tile);

    Settings settings_;
    std::vector<State> state_;
    std::vector<uint32_t> pinned_;      // de fasta bitarna, störst först
    std::vector<uint32_t> active_;      // byggda och påbörjade bitar
    std::vector<Box> bounds_;           // bitarnas lådor, för shown_order()
    std::vector<uint32_t> shown_;       // de visade bitarna
    std::vector<uint32_t> cand_;        // bitar som mäts minst min_pixels, störst P först
    size_t cand_pos_ = 0;               // alla före den här platsen önskas inte
    size_t needed_end_ = 0;             // cand_[0, needed_end_) mäts minst threshold_
    std::vector<uint32_t> ahead_;       // förhandskandidater, störst P för `ahead` först
    size_t ahead_pos_ = 0;              // alla före den här platsen önskas inte
    std::vector<uint32_t> cancel_;      // påbörjade bitar som take_cancellations() ger
    std::vector<uint32_t> waiting_;     // bitar med needed_since satt
    std::vector<double> latencies_;
    std::vector<uint32_t> walk_;        // vandringens stack
    Victims victims_;
    uint32_t epoch_ = 1;
    double threshold_ = 3;
    double ppr_ = 0;
    PartBytes resident_, reserved_;
    size_t pending_ = 0;
    size_t built_count_ = 0;
    size_t shown_count_ = 0;
    uint64_t evictions_ = 0;
    uint64_t cancelled_ = 0;
    // Byte per primitiv i de bitar som byggts, för uppskattningen.
    double built_ram_ = 0, built_gpu_ = 0, built_primitives_ = 0;
};

} // namespace nwdview
