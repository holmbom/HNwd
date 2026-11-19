#include "walk.h"

#include <algorithm>
#include <cmath>

namespace nwdview {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180;

double wrap_degrees(double a) {
    a = std::fmod(a, 360.0);
    if (a < 0) a += 360;
    return a;
}
} // namespace

void walk_forward(const WalkState& s, double out[3]) {
    const double yaw = s.yaw * kDeg, pitch = s.pitch * kDeg;
    out[0] = std::sin(yaw) * std::cos(pitch);
    out[1] = std::cos(yaw) * std::cos(pitch);
    out[2] = std::sin(pitch);
}

bool walk_look_at(WalkState& s, const double eye[3], const double target[3]) {
    const double d[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    const double flat = std::hypot(d[0], d[1]);
    if (!(flat > 0 || std::abs(d[2]) > 0)) return false;
    for (int a = 0; a < 3; ++a) s.eye[a] = eye[a];
    // Rakt upp eller ner har ingen kompassriktning; den gamla står kvar.
    if (flat > 0) s.yaw = wrap_degrees(std::atan2(d[0], d[1]) / kDeg);
    s.pitch = std::atan2(d[2], flat) / kDeg;
    return true;
}

double walk_speed_after(double speed, int steps, const WalkSettings& settings) {
    if (steps != 0) speed *= std::pow(settings.speed_step, steps);
    return std::clamp(speed, settings.min_speed, settings.max_speed);
}

bool walk_step(WalkState& s, const WalkInput& in, const WalkSettings& settings,
               const GroundProbe& probe) {
    bool changed = false;
    if (in.wheel != 0) s.speed = walk_speed_after(s.speed, in.wheel, settings);
    s.speed = std::clamp(s.speed, settings.min_speed, settings.max_speed);

    if (in.look_dx != 0 || in.look_dy != 0) {
        s.yaw = wrap_degrees(s.yaw + in.look_dx * settings.degrees_per_pixel);
        s.pitch = std::clamp(s.pitch - in.look_dy * settings.degrees_per_pixel,
                             -settings.max_pitch, settings.max_pitch);
        changed = true;
    }

    const double dt = std::max(0.0, in.seconds);
    // Svängen före steget, så att steget går i den nya riktningen.
    double turn = 0;
    if (in.keys.turn_right) turn += 1;
    if (in.keys.turn_left) turn -= 1;
    if (turn != 0 && dt > 0) {
        s.yaw = wrap_degrees(s.yaw + turn * settings.turn_rate * dt);
        changed = true;
    }
    const double speed = s.speed * (in.keys.run ? settings.run_factor : 1.0);
    // Framåt och höger i planet: blickens vågräta del.
    const double yaw = s.yaw * kDeg;
    const double fwd[2] = {std::sin(yaw), std::cos(yaw)};
    const double right[2] = {fwd[1], -fwd[0]};
    double ahead = 0, side = 0;
    if (in.keys.forward) ahead += 1;
    if (in.keys.back) ahead -= 1;
    if (in.keys.right) side += 1;
    if (in.keys.left) side -= 1;
    const double len = std::hypot(ahead, side);
    if (len > 0 && dt > 0) {
        const double step = speed * dt / len;   // diagonalt går lika fort
        s.eye[0] += (fwd[0] * ahead + right[0] * side) * step;
        s.eye[1] += (fwd[1] * ahead + right[1] * side) * step;
        changed = true;
    }
    double climb = 0;
    if (in.keys.up) climb += 1;
    if (in.keys.down) climb -= 1;
    const double climb_step = climb * speed * settings.climb_factor * dt;

    if (s.follow_ground && probe) {
        // Med markföljning ändrar höjdtangenterna ögats höjd över ytan.
        if (climb_step != 0)
            s.eye_height = std::max(settings.min_eye_height, s.eye_height + climb_step);
        s.ground_below = probe(s.eye, settings.probe_reach);
        if (s.ground_below) {
            const double target = s.eye[2] - *s.ground_below + s.eye_height;
            double dz = target - s.eye[2];
            dz = std::clamp(dz, -settings.max_fall * dt, settings.max_rise * dt);
            if (dz != 0) {
                s.eye[2] += dz;
                changed = true;
            }
        }
        if (climb_step != 0) changed = true;
    } else {
        s.ground_below.reset();
        if (climb_step != 0) {
            s.eye[2] += climb_step;
            changed = true;
        }
    }
    return changed;
}

} // namespace nwdview
