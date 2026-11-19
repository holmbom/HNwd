#pragma once

// Gångläget: en kamera i första person som räknar nästa läge ur tangenterna,
// musens rörelse och tidssteget. Ren std — ingen fönstertyp — så att samma
// kod kan driva en annan vy.
//
// Världen är högerhänt med z upp, i meter och i double. Riktningen är en
// kompassriktning (`yaw`, grader: 0 = norr = +y, 90 = öster = +x) och en
// lutning (`pitch`, grader uppåt från horisonten). Rörelsen följer blickens
// vågräta del, så att man går på golvet också när man tittar upp eller ner,
// och integreras med verklig tid: samma fart oberoende av bildfrekvensen.

#include <cstdint>
#include <functional>
#include <optional>

namespace nwdview {

// Tangenterna som hålls nere under tidssteget.
struct WalkKeys {
    bool forward = false, back = false, left = false, right = false;
    bool turn_left = false, turn_right = false;   // kompassriktningen
    bool up = false, down = false;   // höjden
    bool run = false;
};

// Det som hänt sedan förra steget.
struct WalkInput {
    WalkKeys keys;
    double look_dx = 0, look_dy = 0;   // musens rörelse i px: höger och nedåt positivt
    int wheel = 0;                   // hjulsteg: positivt ökar farten
    double seconds = 0;                // tidssteget
};

struct WalkSettings {
    double degrees_per_pixel = 0.15;   // musens vridning
    double turn_rate = 60;             // svängtangenternas vridning, grader per sekund
    double max_pitch = 89;
    double run_factor = 3;             // Shift
    double climb_factor = 0.5;         // höjdändringens fart som andel av gångfarten
    double speed_step = 1.25;          // ett hjulsteg multiplicerar farten
    double min_speed = 0.1, max_speed = 500;   // m/s
    double max_rise = 4;               // m/s: hur fort ögat följer en högre yta
    double max_fall = 6;               // m/s: hur fort ögat sjunker mot en lägre yta
    double min_eye_height = 0.2;       // höjdtangenternas gräns nedåt med markföljning
    double probe_reach = 100;          // meter under ögat där en yta söks
};

// Avståndet från punkten rakt ned till närmaste yta, högst `reach` meter;
// tomt när ingen yta finns där.
using GroundProbe = std::function<std::optional<double>(const double from[3], double reach)>;

struct WalkState {
    double eye[3] = {0, 0, 0};
    double yaw = 0;                    // kompassriktningen, grader
    double pitch = 0;                  // grader uppåt
    double speed = 1.5;               // gångfarten, m/s
    bool follow_ground = false;        // håll ögat `eye_height` över ytan under
    // Ögats höjd över ytan med markföljning, meter; höjdtangenterna ändrar den.
    double eye_height = 1.7;
    // Ytans avstånd under ögat vid senaste steget; tomt utan yta eller utan
    // markföljning.
    std::optional<double> ground_below;
};

// Blickens riktning, enhetsvektor.
void walk_forward(const WalkState& s, double out[3]);

// Ett läge ur ett öga och en punkt att titta mot; false när de sammanfaller.
bool walk_look_at(WalkState& s, const double eye[3], const double target[3]);

// Nästa läge. Svarar om kameran flyttade eller vreds. `probe` frågas bara med
// markföljning.
bool walk_step(WalkState& s, const WalkInput& in, const WalkSettings& settings,
               const GroundProbe& probe);

// Farten efter `steps` hjulsteg, inom inställningarnas gränser.
double walk_speed_after(double speed, int steps, const WalkSettings& settings);

} // namespace nwdview
