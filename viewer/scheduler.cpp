#include "scheduler.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace nwdview {

namespace {

// Hörn per primitiv innan någon bit byggts; byten per primitiv följer då av
// samma tal som part_bytes räknar med (cells.h).
constexpr double kInitialVerticesPerPrimitive = 1.0;

// Närmaste avstånd som räknas, meter: ögat inuti en låda ger inte oändliga px.
constexpr double kMinDistance = 1e-3;

// Vikten för en låda som ligger helt bakom ögats plan men inom near_radius.
constexpr double kBehindWeight = 0.5;

// Hysteresen för minnets tröskel: den sänks först när det som behövs vid den
// lägre tröskeln ryms i den här delen av budgeten.
constexpr double kBudgetSlack = 0.9;

// En bit som visas parkeras först när den mäts under draw_pixels × så här.
constexpr double kParkFactor = 0.8;

// Så många bitar i rad som inte får plats avbryter en omgång.
constexpr int kMaxFailures = 32;

// Minnets tröskel söks med halvering tills den är högst så här mycket över den
// lägsta som ryms, i högst så många steg.
constexpr double kRefineRatio = 1.01;
constexpr int kRefineSteps = 16;

size_t excess(size_t used, size_t limit) { return used > limit ? used - limit : 0; }

PartBytes sum(PartBytes a, PartBytes b) { return {a.ram + b.ram, a.gpu + b.gpu}; }

// a − b, och aldrig under noll i någon av delarna.
PartBytes minus(PartBytes a, PartBytes b) {
    return {a.ram - std::min(a.ram, b.ram), a.gpu - std::min(a.gpu, b.gpu)};
}

bool nothing(PartBytes b) { return b.ram == 0 && b.gpu == 0; }

bool over(PartBytes used, PartBytes limit) { return used.ram > limit.ram || used.gpu > limit.gpu; }

void set_flag(uint8_t& flags, unsigned bit) { flags = static_cast<uint8_t>(flags | bit); }
void clear_flag(uint8_t& flags, unsigned bit) {
    flags = static_cast<uint8_t>(flags & ~bit);
}

// Största värdet av n · (p − öga) över lådan.
double max_along(const Box& b, const double eye[3], const double n[3]) {
    double s = 0;
    for (int a = 0; a < 3; ++a) {
        const double centre = (b.min[a] + b.max[a]) / 2, half = (b.max[a] - b.min[a]) / 2;
        s += n[a] * (centre - eye[a]) + std::fabs(n[a]) * half;
    }
    return s;
}

} // namespace

ViewFrustum ViewFrustum::orbit(const double eye[3], double azimuth_deg, double elevation_deg,
                               double fov_deg, double width, double height) {
    constexpr double kDeg = 3.14159265358979323846 / 180.0;
    const double az = azimuth_deg * kDeg, el = elevation_deg * kDeg;
    ViewFrustum v;
    for (int a = 0; a < 3; ++a) v.eye[a] = eye[a];
    v.forward[0] = std::sin(az) * std::cos(el);
    v.forward[1] = std::cos(az) * std::cos(el);
    v.forward[2] = -std::sin(el);
    v.right[0] = std::cos(az);
    v.right[1] = -std::sin(az);
    v.right[2] = 0;
    // upp = höger × framåt
    v.up[0] = v.right[1] * v.forward[2] - v.right[2] * v.forward[1];
    v.up[1] = v.right[2] * v.forward[0] - v.right[0] * v.forward[2];
    v.up[2] = v.right[0] * v.forward[1] - v.right[1] * v.forward[0];
    v.tan_half_fov = std::tan(fov_deg * kDeg / 2);
    v.width = width;
    v.height = height;
    return v;
}

double ViewFrustum::pixels_per_radian() const {
    if (!(tan_half_fov > 0) || !(height > 0)) return 0;
    return height / (2 * tan_half_fov);
}

double ViewFrustum::distance(const Box& box) const {
    if (!box.valid()) return std::numeric_limits<double>::infinity();
    double sq = 0;
    for (int a = 0; a < 3; ++a) {
        const double e = std::max({box.min[a] - eye[a], 0.0, eye[a] - box.max[a]});
        sq += e * e;
    }
    return std::sqrt(sq);
}

bool ViewFrustum::sees(const Box& box) const {
    if (!box.valid() || !(width > 0) || !(height > 0)) return false;
    // Bakom ögats plan.
    if (!(max_along(box, eye, forward) > 0)) return false;
    // De fyra sidoplanen: en punkt är innanför när tx·z − x ≥ 0, tx·z + x ≥ 0,
    // ty·z − y ≥ 0 och ty·z + y ≥ 0, med x, y, z i kamerans axlar. Lådan är
    // utanför när det största värdet över den är negativt.
    const double ty = tan_half_fov, tx = tan_half_fov * width / height;
    for (int k = 0; k < 4; ++k) {
        const double* axis = k < 2 ? right : up;
        const double scale = k < 2 ? tx : ty;
        const double sign = (k & 1) ? 1.0 : -1.0;
        double n[3];
        for (int a = 0; a < 3; ++a) n[a] = scale * forward[a] + sign * axis[a];
        if (max_along(box, eye, n) < 0) return false;
    }
    return true;
}

double ViewFrustum::pixels_at(const double p[3], double size) const {
    const double z = (p[0] - eye[0]) * forward[0] + (p[1] - eye[1]) * forward[1] +
                     (p[2] - eye[2]) * forward[2];
    if (!(z > 1e-6) || height <= 0) return 0;
    return size * height / (2 * z * tan_half_fov);
}

namespace {

// En låda med fragment av storleken `size` mätt i px: s · K / avstånd. Noll
// när lådan varken når in i bilden eller ligger inom near_radius; inom
// near_radius, utanför bilden och helt bakom ögats plan räknas den med halva
// vikten. `potential` får värdet utan hänsyn till vad som syns.
double measure_box(const ViewFrustum& v, double pixels_per_radian, const Box& box, float size,
                   double& potential) {
    const double dist = v.distance(box);
    potential = static_cast<double>(size) * pixels_per_radian / std::max(dist, kMinDistance);
    if (v.sees(box)) return potential;
    if (!(v.near_radius > 0 && dist < v.near_radius)) return 0;
    return max_along(box, v.eye, v.forward) > 0 ? potential : potential * kBehindWeight;
}

// Vandrar trädet uppifrån med kameran `v` och anropar visit(tile, px, potential)
// för varje bit som mäts minst min_px: en nod vars största fragment mäts under
// det beskärs med hela sitt delträd, eftersom barnen aldrig mäts större.
template <class Visit>
void walk_tiles(const LodTree& tree, const ViewFrustum& v, double ppr, double min_px,
                std::vector<uint32_t>& stack, Visit&& visit) {
    stack.clear();
    stack.push_back(0);
    while (!stack.empty()) {
        const uint32_t n = stack.back();
        stack.pop_back();
        const LodNode& node = tree.nodes[n];
        if (!(node.max_size > 0)) continue;
        double potential = 0;
        const double node_px = measure_box(v, ppr, node.bounds, node.max_size, potential);
        if (!(node_px > 0) || node_px < min_px) continue;
        for (uint32_t t = node.first_tile; t < node.first_tile + node.tile_count; ++t) {
            const Tile& tile = tree.tiles[t];
            const double px = measure_box(v, ppr, tile.bounds, tile.max_size, potential);
            if (!(px > 0) || px < min_px) continue;
            visit(t, px, potential);
        }
        for (uint32_t k = node.child_count; k-- > 0;)
            stack.push_back(tree.children[node.first_child + k]);
    }
}

} // namespace

void TileScheduler::reset(const LodTree& tree, const Settings& s) {
    *this = TileScheduler{};
    settings_ = s;
    settings_.min_pixels = std::max(settings_.min_pixels, 1e-6);
    settings_.max_pixels = std::max(settings_.max_pixels, settings_.min_pixels);
    threshold_ = settings_.min_pixels;
    state_.assign(tree.tiles.size(), State{});
    bounds_.resize(tree.tiles.size());
    for (uint32_t t = 0; t < tree.tiles.size(); ++t) {
        state_[t].primitives = tree.tiles[t].primitives;
        state_[t].max_size = tree.tiles[t].max_size;
        bounds_[t] = tree.tiles[t].bounds;
        if (tree.tiles[t].pinned) {
            set_flag(state_[t].flags, kPinned);
            pinned_.push_back(t);
        }
    }
    std::sort(pinned_.begin(), pinned_.end(), [&tree](uint32_t a, uint32_t b) {
        if (tree.tiles[a].max_size != tree.tiles[b].max_size)
            return tree.tiles[a].max_size > tree.tiles[b].max_size;
        return a < b;
    });
}

bool TileScheduler::wants(const State& s) const {
    return (s.flags & (kBuilt | kPending | kFailed)) == 0 && s.refused_epoch != epoch_;
}

bool TileScheduler::rebuild_wanted(const State& s) const {
    return (s.flags & (kBuilt | kPending | kFailed | kPinned)) == kBuilt &&
           s.refused_epoch != epoch_ && s.plan_epoch == epoch_ && s.have.omitted > 0 &&
           static_cast<double>(s.have.omitted) >= s.need;
}

PartBytes TileScheduler::weigh(PartBytes b) const {
    if (settings_.shared) return {b.ram + b.gpu, 0};
    return b;
}

PartBytes TileScheduler::weighed_budget(double fraction) const {
    const auto scaled = [fraction](size_t v) {
        return static_cast<size_t>(static_cast<double>(v) * fraction);
    };
    if (settings_.shared) return {scaled(settings_.budget.ram), 0};
    return {scaled(settings_.budget.ram), scaled(settings_.budget.gpu)};
}

PartBytes TileScheduler::estimate(const State& s, uint64_t primitives) const {
    if (!nothing(s.known) && s.known_primitives > 0) {
        if (primitives == s.known_primitives) return s.known;
        const double scale = static_cast<double>(primitives) / static_cast<double>(s.known_primitives);
        return {static_cast<size_t>(static_cast<double>(s.known.ram) * scale),
                static_cast<size_t>(static_cast<double>(s.known.gpu) * scale)};
    }
    double ram = 0, gpu = 0;   // byte per primitiv
    if (built_primitives_ > 0) {
        ram = built_ram_ / built_primitives_;
        gpu = built_gpu_ / built_primitives_;
    } else {
        ram = static_cast<double>(kRamBytesPerVertex) * kInitialVerticesPerPrimitive +
              static_cast<double>(kIndexBytesPerTriangle);
        gpu = static_cast<double>(kGpuBytesPerVertex) * kInitialVerticesPerPrimitive +
              static_cast<double>(kIndexBytesPerTriangle);
    }
    const double count = static_cast<double>(primitives);
    return {static_cast<size_t>(ram * count) + 4096, static_cast<size_t>(gpu * count) + 4096};
}

TileScheduler::Plan TileScheduler::plan_for(const LodTree& tree, uint32_t tile, double need,
                                            bool pinned) const {
    const Tile& t = tree.tiles[tile];
    Plan whole;
    whole.primitives = t.primitives;
    if (pinned) return whole;
    const float size = static_cast<float>(need / std::max(1.0, settings_.size_margin));
    if (!(size > t.min_size)) return whole;
    const TilePrefix prefix = tree.prefix_at_least(tile, size);
    if (!(prefix.omitted > 0)) return whole;
    Plan plan;
    plan.min_size = size;
    plan.omitted = prefix.omitted;
    plan.primitives = prefix.primitives;
    return plan;
}

PartBytes TileScheduler::demand_at(const LodTree& tree, uint32_t tile, double threshold) const {
    const State& s = state_[tile];
    if (s.flags & (kBuilt | kPending)) {
        PartBytes d = sum(s.held, s.reserve);
        // Vid den här tröskeln skulle en byggd bit byggas om: den tar då det
        // större av vad den tar nu och vad ombyggnaden tar.
        if ((s.flags & (kBuilt | kPending | kFailed | kPinned)) == kBuilt && s.have.omitted > 0) {
            const double need = threshold * static_cast<double>(s.max_size) / s.pixels;
            if (static_cast<double>(s.have.omitted) >= need) {
                const PartBytes e = estimate(s, plan_for(tree, tile, need, false).primitives);
                d = {std::max(d.ram, e.ram), std::max(d.gpu, e.gpu)};
            }
        }
        return d;
    }
    const double need = threshold * static_cast<double>(s.max_size) / std::max(s.pixels, 1e-12);
    return estimate(s, plan_for(tree, tile, need, (s.flags & kPinned) != 0).primitives);
}

void TileScheduler::set_plan(const LodTree& tree, uint32_t tile, double pixels) {
    State& s = state_[tile];
    s.need = threshold_ * static_cast<double>(s.max_size) / pixels;
    s.plan = plan_for(tree, tile, s.need, (s.flags & kPinned) != 0);
    s.plan_epoch = epoch_;
}

void TileScheduler::sync_active(uint32_t tile) {
    State& s = state_[tile];
    const bool wanted = (s.flags & (kBuilt | kPending)) != 0;
    const bool has = (s.flags & kActive) != 0;
    if (wanted && !has) {
        set_flag(s.flags, kActive);
        s.active_pos = static_cast<uint32_t>(active_.size());
        active_.push_back(tile);
    } else if (!wanted && has) {
        const uint32_t pos = s.active_pos;
        const uint32_t moved = active_.back();
        active_[pos] = moved;
        state_[moved].active_pos = pos;
        active_.pop_back();
        clear_flag(s.flags, kActive);
    }
}

void TileScheduler::set_shown(uint32_t tile) {
    State& s = state_[tile];
    set_flag(s.flags, kShown);
    s.shown_pos = static_cast<uint32_t>(shown_.size());
    shown_.push_back(tile);
    ++shown_count_;
}

void TileScheduler::clear_shown(uint32_t tile) {
    State& s = state_[tile];
    const uint32_t pos = s.shown_pos;
    const uint32_t moved = shown_.back();
    shown_[pos] = moved;
    state_[moved].shown_pos = pos;
    shown_.pop_back();
    clear_flag(s.flags, kShown);
    --shown_count_;
}

void TileScheduler::drop_built(uint32_t tile) {
    State& s = state_[tile];
    resident_ = minus(resident_, s.held);
    s.held = PartBytes{};
    s.have = Plan{};
    if (s.flags & kShown) clear_shown(tile);
    clear_flag(s.flags, kBuilt);
    --built_count_;
    sync_active(tile);
}

void TileScheduler::update(const LodTree& tree, const ViewFrustum& view,
                           const ViewFrustum* ahead, double now) {
    if (tree.tiles.size() != state_.size()) return;
    ++epoch_;
    victims_.valid = false;
    cand_.clear();
    cand_pos_ = 0;
    needed_end_ = 0;
    ahead_.clear();
    ahead_pos_ = 0;
    cancel_.clear();
    ppr_ = view.pixels_per_radian();
    const double min_px = settings_.min_pixels;
    const bool measurable = ppr_ > 0 && view.width > 0 && view.height > 0;
    const double ahead_ppr = ahead ? ahead->pixels_per_radian() : 0;
    const bool ahead_measurable = ahead_ppr > 0 && ahead->width > 0 && ahead->height > 0;

    // Vandringen: en nod kan bara ha en bit som behövs när dess största
    // fragment mäts minst min_pixels, och barnen mäts aldrig större.
    if (measurable && !tree.nodes.empty()) {
        walk_tiles(tree, view, ppr_, min_px, walk_,
                   [this](uint32_t t, double px, double potential) {
                       State& s = state_[t];
                       s.pixels = px;
                       s.potential = static_cast<float>(potential);
                       s.epoch = epoch_;
                       cand_.push_back(t);
                   });
    }
    // Samma vandring med kameran om en stund: de bitar som den ser. De som
    // behövs nu sorteras bort när tröskeln är känd.
    if (ahead_measurable && !tree.nodes.empty()) {
        walk_tiles(tree, *ahead, ahead_ppr, min_px, walk_,
                   [this](uint32_t t, double px, double) {
                       State& s = state_[t];
                       s.ahead_pixels = px;
                       s.ahead_epoch = epoch_;
                       if (!(s.flags & kPinned)) ahead_.push_back(t);
                   });
    }

    // De byggda och påbörjade mäts om, också när vandringen inte nådde dem.
    for (const uint32_t t : active_) {
        State& s = state_[t];
        const Tile& tile = tree.tiles[t];
        if (s.epoch != epoch_) {
            double potential = 0;
            s.pixels =
                measurable ? measure_box(view, ppr_, tile.bounds, tile.max_size, potential) : 0;
            s.potential = static_cast<float>(potential);
            s.epoch = epoch_;
        }
        if (ahead_measurable && s.ahead_epoch != epoch_) {
            double potential = 0;
            s.ahead_pixels = measure_box(*ahead, ahead_ppr, tile.bounds, tile.max_size, potential);
            s.ahead_epoch = epoch_;
        }
    }

    // Kandidaterna, störst först.
    {
        std::vector<std::pair<double, uint32_t>> keyed;
        keyed.reserve(cand_.size());
        for (const uint32_t t : cand_) keyed.emplace_back(state_[t].pixels, t);
        std::sort(keyed.begin(), keyed.end(),
                  [](const std::pair<double, uint32_t>& a, const std::pair<double, uint32_t>& b) {
                      if (a.first != b.first) return a.first > b.first;
                      return a.second < b.second;
                  });
        for (size_t k = 0; k < keyed.size(); ++k) cand_[k] = keyed[k].second;
    }
    compute_threshold(tree);
    needed_end_ = static_cast<size_t>(
        std::partition_point(cand_.begin(), cand_.end(),
                             [this](uint32_t t) { return state_[t].pixels >= threshold_; }) -
        cand_.begin());

    // Förhandskandidaterna: det som `ahead` ser minst N_eff och som inte
    // behövs nu, störst först.
    {
        size_t kept = 0;
        for (const uint32_t t : ahead_) {
            const State& s = state_[t];
            if (current_ahead(s) < threshold_ || current_pixels(s) >= threshold_) continue;
            ahead_[kept++] = t;
        }
        ahead_.resize(kept);
        std::sort(ahead_.begin(), ahead_.end(), [this](uint32_t a, uint32_t b) {
            const double pa = state_[a].ahead_pixels, pb = state_[b].ahead_pixels;
            if (pa != pb) return pa > pb;
            return a < b;
        });
    }

    // Vad varje jobb som kan bli av bygger: för det som behövs nu mot kameran
    // och för förhandskandidaterna mot `ahead`.
    for (size_t k = 0; k < needed_end_; ++k) set_plan(tree, cand_[k], state_[cand_[k]].pixels);
    for (const uint32_t t : ahead_) set_plan(tree, t, state_[t].ahead_pixels);

    // Jobben som inte längre motiveras: påbörjade bitar under N_eff / 4 som
    // varken ligger nära eller väntas av `ahead`.
    if (measurable) {
        const double floor_px = threshold_ / 4;
        for (const uint32_t t : active_) {
            const State& s = state_[t];
            if ((s.flags & (kPending | kStale | kPinned)) != kPending) continue;
            if (current_pixels(s) >= floor_px || current_ahead(s) >= floor_px) continue;
            if (view.near_radius > 0 && view.distance(tree.tiles[t].bounds) < view.near_radius) continue;
            cancel_.push_back(t);
        }
    }

    // Latensen: när en bit först behövs. De som slutat behövas glöms.
    for (size_t k = 0; k < needed_end_; ++k) {
        State& s = state_[cand_[k]];
        if (s.flags & (kBuilt | kPinned)) continue;
        s.needed_epoch = epoch_;
        if (s.needed_since < 0) {
            s.needed_since = now;
            waiting_.push_back(cand_[k]);
        }
    }
    size_t kept = 0;
    for (const uint32_t t : waiting_) {
        State& s = state_[t];
        if (s.needed_since < 0) continue;
        if (s.needed_epoch != epoch_ || (s.flags & kBuilt)) {
            s.needed_since = -1;
            continue;
        }
        waiting_[kept++] = t;
    }
    waiting_.resize(kept);
}

std::vector<uint32_t> TileScheduler::take_cancellations() {
    std::vector<uint32_t> out;
    out.reserve(cancel_.size());
    for (const uint32_t t : cancel_) {
        State& s = state_[t];
        // Kan ha tagits emot eller ogiltigförklarats sedan mätningen.
        if ((s.flags & (kPending | kStale | kPinned)) != kPending) continue;
        set_flag(s.flags, kStale);
        out.push_back(t);
    }
    cancel_.clear();
    cancelled_ += out.size();
    return out;
}

void TileScheduler::compute_threshold(const LodTree& tree) {
    const double lo = settings_.min_pixels, hi = settings_.max_pixels;
    PartBytes pinned_used;
    for (const uint32_t t : pinned_) pinned_used = sum(pinned_used, weigh(demand_at(tree, t, lo)));
    const PartBytes full = weighed_budget(1.0), slack = weighed_budget(kBudgetSlack);

    // Ryms de fasta bitarna och kandidaterna som mäts minst `threshold` i
    // budgeten? Varje kandidat räknas med det prefix som tröskeln ger, så
    // behovet avtar när tröskeln växer och sökningen nedan är monoton.
    const auto fits = [&](double threshold, PartBytes budget) {
        PartBytes used = pinned_used;
        if (over(used, budget)) return false;
        for (const uint32_t t : cand_) {
            const State& s = state_[t];
            if (s.pixels < threshold) break;
            if (s.flags & kPinned) continue;
            used = sum(used, weigh(demand_at(tree, t, threshold)));
            if (over(used, budget)) return false;
        }
        return true;
    };
    // Tröskeln: lägst min_pixels, annars precis över en bits mätvärde, så att
    // den biten och de som mäts mindre inte längre räknas. Den lägsta som ryms
    // i `budget`; det finns inget som ryms utan kandidater, och då gäller den
    // över den största.
    const size_t n = cand_.size();
    const auto point = [&](size_t i) {
        if (i == 0) return lo;
        return std::max(lo, std::nextafter(state_[cand_[n - i]].pixels,
                                           std::numeric_limits<double>::infinity()));
    };
    const auto least = [&](PartBytes budget) {
        if (n == 0 || fits(lo, budget)) return lo;
        size_t infeasible = 0, feasible = n;   // point(feasible) räknas alltid som möjlig
        while (feasible - infeasible > 1) {
            const size_t mid = infeasible + (feasible - infeasible) / 2;
            if (fits(point(mid), budget)) feasible = mid;
            else infeasible = mid;
        }
        // Mellan två punkter är samma bitar med, men prefixen krymper när
        // tröskeln växer, så den lägsta tröskeln som ryms kan ligga emellan.
        // Den söks med halvering på kvoten tills den är under kRefineRatio.
        double below = point(infeasible), above = point(feasible);
        for (int i = 0; i < kRefineSteps && above > below * kRefineRatio; ++i) {
            const double mid = std::sqrt(below * above);
            if (fits(mid, budget)) above = mid;
            else below = mid;
        }
        return above;
    };
    const double raised = least(full);
    double next = threshold_;
    if (next < raised) {
        next = raised;
    } else if (next > raised) {
        const double lowered = least(slack);
        if (next > lowered) next = lowered;
    }
    threshold_ = std::clamp(next, lo, hi);
}

void TileScheduler::build_victims() {
    victims_.list.clear();
    const double keep = threshold_ * settings_.keep_factor;
    for (const uint32_t t : active_) {
        const State& s = state_[t];
        // En bit som byggs om är byggd men släpps inte medan jobbet pågår.
        if (!(s.flags & kBuilt) || (s.flags & (kPinned | kPending))) continue;
        const double px = current_pixels(s);
        if (px > 0) {
            victims_.list.push_back({t, static_cast<uint8_t>(px < keep ? 2 : 3), px});
        } else {
            const double ahead_px = current_ahead(s);
            if (ahead_px >= threshold_) victims_.list.push_back({t, 1, ahead_px});
            else victims_.list.push_back({t, 0, static_cast<double>(s.potential)});
        }
    }
    std::sort(victims_.list.begin(), victims_.list.end(), [](const Victim& a, const Victim& b) {
        if (a.group != b.group) return a.group < b.group;
        if (a.key != b.key) return a.key < b.key;
        return a.tile < b.tile;
    });
    victims_.plain_end = static_cast<size_t>(
        std::partition_point(victims_.list.begin(), victims_.list.end(),
                             [](const Victim& v) { return v.group == 0; }) -
        victims_.list.begin());
    victims_.hidden_end = static_cast<size_t>(
        std::partition_point(victims_.list.begin(), victims_.list.end(),
                             [](const Victim& v) { return v.group <= 1; }) -
        victims_.list.begin());
    victims_.prefix.assign(victims_.list.size() + 1, PartBytes{});
    for (size_t k = 0; k < victims_.list.size(); ++k)
        victims_.prefix[k + 1] =
            sum(victims_.prefix[k], weigh(state_[victims_.list[k].tile].held));
    victims_.taken = 0;
    victims_.valid = true;
}

bool TileScheduler::make_room(PartBytes need, double waiting, bool ahead_job,
                              std::vector<uint32_t>& evict) {
    // Det som saknas i RAM och på grafikkortet. I en gemensam budget är det
    // summan som räknas, och den ligger i ram.
    const PartBytes held = weigh(sum(sum(resident_, reserved_), need));
    const PartBytes limit = weighed_budget(1.0);
    const PartBytes missing{excess(held.ram, limit.ram), excess(held.gpu, limit.gpu)};
    if (nothing(missing)) return true;
    if (!victims_.valid) build_victims();
    // De som får släppas är en början av listan: de utanför bilden, och de i
    // bild som mäts under `waiting` (grupp 2 och 3 har växande nycklar, och
    // grupp 2 ligger före grupp 3). Ett förhandsjobb släpper bara det som
    // ingen av kamerorna ser.
    const std::vector<Victim>& v = victims_.list;
    size_t eligible = victims_.plain_end;
    if (!ahead_job) {
        const size_t from = std::max(victims_.taken, victims_.hidden_end);
        eligible = static_cast<size_t>(
            std::partition_point(v.begin() + static_cast<std::ptrdiff_t>(from), v.end(),
                                 [waiting](const Victim& x) { return x.key < waiting; }) -
            v.begin());
    }
    const PartBytes base = victims_.prefix[victims_.taken];
    const PartBytes top = victims_.prefix[std::max(eligible, victims_.taken)];
    if (top.ram - base.ram < missing.ram || top.gpu - base.gpu < missing.gpu) return false;
    // Det minsta antalet som räcker i båda.
    const auto reaching = [&](size_t PartBytes::*field, size_t target) {
        return static_cast<size_t>(
            std::lower_bound(victims_.prefix.begin() + static_cast<std::ptrdiff_t>(victims_.taken),
                             victims_.prefix.begin() +
                                 static_cast<std::ptrdiff_t>(std::max(eligible, victims_.taken)) + 1,
                             target,
                             [field](const PartBytes& p, size_t t) { return p.*field < t; }) -
            victims_.prefix.begin());
    };
    const size_t take = std::max(reaching(&PartBytes::ram, base.ram + missing.ram),
                                 reaching(&PartBytes::gpu, base.gpu + missing.gpu));
    for (size_t k = victims_.taken; k < take; ++k) {
        const uint32_t tile = v[k].tile;
        if ((state_[tile].flags & (kBuilt | kPending)) != kBuilt) continue;
        drop_built(tile);
        ++evictions_;
        evict.push_back(tile);
    }
    victims_.taken = take;
    return true;
}

std::vector<TileScheduler::Job> TileScheduler::next_build_jobs(size_t max_jobs,
                                                               std::vector<uint32_t>& evict) {
    std::vector<Job> jobs;
    if (max_jobs == 0) return jobs;
    // De som inte fick plats. De som kommer efter mäts mindre och kan släppa
    // färre; en som är lika stor eller större i både RAM och på grafikkortet
    // får inte heller plats.
    std::vector<PartBytes> failed;
    const auto refused_before = [&](PartBytes need) {
        for (const PartBytes& f : failed)
            if (need.ram >= f.ram && need.gpu >= f.gpu) return true;
        return false;
    };
    int failures = 0;
    // En ombyggnad reserverar bara det som den nya formen tar mer än den
    // gamla, eftersom den gamla släpps när den nya tas emot.
    const auto take = [&](uint32_t tile, double waiting, bool ahead_job, bool rebuild) {
        State& s = state_[tile];
        Plan plan;
        plan.primitives = s.primitives;
        if (!(s.flags & kPinned) && s.plan_epoch == epoch_) plan = s.plan;
        PartBytes need = estimate(s, plan.primitives);
        if (rebuild) need = minus(need, s.held);
        if (refused_before(need) || !make_room(need, waiting, ahead_job, evict)) {
            failed.push_back(need);
            ++failures;
            s.refused_epoch = epoch_;
            return;
        }
        set_flag(s.flags, kPending);
        s.pend = plan;
        s.reserve = need;
        reserved_ = sum(reserved_, need);
        ++pending_;
        sync_active(tile);
        // Offren som räknats ut gäller inte längre: den som byggs om får inte släppas.
        if (rebuild) victims_.valid = false;
        jobs.push_back({tile, plan.min_size});
    };
    const auto room = [&] { return jobs.size() < max_jobs && failures < kMaxFailures; };

    // De fasta bitarna först: de byggs före allt annat och släpps aldrig.
    for (const uint32_t tile : pinned_) {
        if (!room()) break;
        if (wants(state_[tile]))
            take(tile, std::numeric_limits<double>::infinity(), false, false);
    }
    // Sedan kandidaterna som behövs. Allt före `cand_pos_` önskas inte.
    size_t k = cand_pos_;
    for (; k < needed_end_ && room(); ++k) {
        const State& s = state_[cand_[k]];
        if (s.flags & kPinned) continue;
        if (wants(s)) take(cand_[k], s.pixels, false, false);
        else if (rebuild_wanted(s)) take(cand_[k], s.pixels, false, true);
    }
    // Efter många försök utan plats prövas resten först vid nästa mätning.
    if (failures >= kMaxFailures) {
        for (; k < needed_end_; ++k) state_[cand_[k]].refused_epoch = epoch_;
        for (const uint32_t tile : pinned_)
            if (wants(state_[tile])) state_[tile].refused_epoch = epoch_;
    }
    cand_pos_ = k;
    // Förhandskandidaterna, när allt som behövs nu är taget.
    size_t a = ahead_pos_;
    if (k >= needed_end_) {
        for (; a < ahead_.size() && room(); ++a) {
            const State& s = state_[ahead_[a]];
            if (!wants(s)) continue;
            take(ahead_[a], current_ahead(s), true, false);
        }
        if (failures >= kMaxFailures)
            for (; a < ahead_.size(); ++a) state_[ahead_[a]].refused_epoch = epoch_;
    }
    ahead_pos_ = a;
    return jobs;
}

bool TileScheduler::tile_done(uint32_t tile, float min_size, PartBytes bytes, bool ok, double now,
                              std::vector<uint32_t>& evict) {
    if (tile >= state_.size()) return false;
    State& s = state_[tile];
    if (!(s.flags & kPending)) return false;
    // Ett jobb för en bit som redan är byggd är en ombyggnad.
    const bool rebuild = (s.flags & kBuilt) != 0;
    reserved_ = minus(reserved_, s.reserve);
    s.reserve = PartBytes{};
    const auto finish_job = [&] {
        clear_flag(s.flags, kPending);
        --pending_;
        sync_active(tile);
    };
    if (s.flags & kStale) {
        clear_flag(s.flags, kStale);
        finish_job();
        return false;
    }
    if (!ok) {
        set_flag(s.flags, kFailed);
        finish_job();
        return false;
    }
    s.known = bytes;
    s.known_primitives = s.pend.primitives;
    built_ram_ += static_cast<double>(bytes.ram);
    built_gpu_ += static_cast<double>(bytes.gpu);
    built_primitives_ += static_cast<double>(std::max<uint64_t>(1, s.pend.primitives));
    Plan built = s.pend;
    built.min_size = min_size;
    if (rebuild) {
        // Den nya formen ersätter den gamla: bara tillskottet ska få plats.
        // Biten är fortfarande påbörjad, så att den inte kan släppas medan
        // plats frigörs.
        if (!make_room(minus(bytes, s.held), current_pixels(s), false, evict)) {
            s.refused_epoch = epoch_;
            finish_job();
            return false;
        }
        resident_ = sum(minus(resident_, s.held), bytes);
        s.held = bytes;
        s.have = built;
        finish_job();
        return true;
    }
    finish_job();
    const double waiting = (s.flags & kPinned) ? std::numeric_limits<double>::infinity()
                                              : current_pixels(s);
    const bool ahead_job = !(s.flags & kPinned) && current_pixels(s) < threshold_ &&
                           current_ahead(s) >= threshold_;
    if (!make_room(bytes, waiting, ahead_job, evict)) {
        s.refused_epoch = epoch_;
        return false;
    }
    set_flag(s.flags, kBuilt);
    s.held = bytes;
    s.have = built;
    sync_active(tile);
    resident_ = sum(resident_, bytes);
    ++built_count_;
    if (s.needed_since >= 0) {
        latencies_.push_back(std::max(0.0, now - s.needed_since));
        s.needed_since = -1;
    }
    return true;
}

void TileScheduler::add_bytes(uint32_t tile, PartBytes extra) {
    if (tile >= state_.size()) return;
    State& s = state_[tile];
    if (!(s.flags & kBuilt)) return;
    s.held = sum(s.held, extra);
    resident_ = sum(resident_, extra);
}

bool TileScheduler::invalidate(uint32_t tile) {
    if (tile >= state_.size()) return false;
    State& s = state_[tile];
    s.known = PartBytes{};
    s.known_primitives = 0;
    clear_flag(s.flags, kFailed);
    s.refused_epoch = 0;
    cand_pos_ = 0;
    ahead_pos_ = 0;
    if (s.flags & kPending) set_flag(s.flags, kStale);
    if (!(s.flags & kBuilt)) return false;
    drop_built(tile);
    victims_.valid = false;
    return true;
}

void TileScheduler::shown_changes(double draw_pixels, size_t max_changes,
                                  std::vector<uint32_t>& show, std::vector<uint32_t>& park) {
    show.clear();
    park.clear();
    if (max_changes == 0) return;
    std::vector<std::pair<double, uint32_t>> to_show, to_park;
    for (const uint32_t t : active_) {
        const State& s = state_[t];
        if (!(s.flags & kBuilt)) continue;
        const double px = current_pixels(s);
        if (s.flags & kPinned) {
            // En fast bit visas så snart den är byggd och parkeras aldrig.
            if (!(s.flags & kShown))
                to_show.emplace_back(std::numeric_limits<double>::infinity(), t);
        } else if (s.flags & kShown) {
            if (px < draw_pixels * kParkFactor) to_park.emplace_back(px, t);
        } else if (px >= draw_pixels) {
            to_show.emplace_back(px, t);
        }
    }
    std::sort(to_show.begin(), to_show.end(),
              [](const std::pair<double, uint32_t>& a, const std::pair<double, uint32_t>& b) {
                  if (a.first != b.first) return a.first > b.first;
                  return a.second < b.second;
              });
    std::sort(to_park.begin(), to_park.end(),
              [](const std::pair<double, uint32_t>& a, const std::pair<double, uint32_t>& b) {
                  if (a.first != b.first) return a.first < b.first;
                  return a.second < b.second;
              });
    size_t i = 0, j = 0;
    while (show.size() + park.size() < max_changes && (i < to_show.size() || j < to_park.size())) {
        if (i < to_show.size()) show.push_back(to_show[i++].second);
        if (show.size() + park.size() < max_changes && j < to_park.size())
            park.push_back(to_park[j++].second);
    }
    for (const uint32_t t : show) set_shown(t);
    for (const uint32_t t : park) clear_shown(t);
}

void TileScheduler::shown_order(const ViewFrustum& view, std::vector<uint32_t>& out) const {
    out.clear();
    std::vector<std::pair<double, uint32_t>> keyed;
    keyed.reserve(shown_.size());
    for (const uint32_t t : shown_) keyed.emplace_back(view.distance(bounds_[t]), t);
    std::sort(keyed.begin(), keyed.end(),
              [](const std::pair<double, uint32_t>& a, const std::pair<double, uint32_t>& b) {
                  if (a.first != b.first) return a.first < b.first;
                  return a.second < b.second;
              });
    out.reserve(keyed.size());
    for (const auto& k : keyed) out.push_back(k.second);
}

double TileScheduler::pixels(uint32_t tile) const {
    if (tile >= state_.size()) return 0;
    return current_pixels(state_[tile]);
}

bool TileScheduler::built(uint32_t tile) const {
    return tile < state_.size() && (state_[tile].flags & kBuilt) != 0;
}

float TileScheduler::built_min_size(uint32_t tile) const {
    return tile < state_.size() ? state_[tile].have.min_size : 0.0f;
}

bool TileScheduler::shown(uint32_t tile) const {
    return tile < state_.size() && (state_[tile].flags & kShown) != 0;
}

bool TileScheduler::idle() const {
    if (pending_ > 0) return false;
    for (const uint32_t t : pinned_)
        if (wants(state_[t])) return false;
    for (size_t k = cand_pos_; k < needed_end_; ++k) {
        const State& s = state_[cand_[k]];
        if (wants(s) || rebuild_wanted(s)) return false;
    }
    for (size_t k = ahead_pos_; k < ahead_.size(); ++k)
        if (wants(state_[ahead_[k]])) return false;
    return true;
}

size_t TileScheduler::holes(double factor) const {
    const double limit = threshold_ * factor;
    size_t count = 0;
    for (const uint32_t t : cand_) {
        const State& s = state_[t];
        if (s.pixels < limit) break;
        if (!(s.flags & kBuilt)) {
            ++count;
        } else if (!(s.flags & kPinned) && s.have.omitted > 0 &&
                   static_cast<double>(s.have.omitted) >=
                       limit * static_cast<double>(s.max_size) / s.pixels) {
            // Något som är minst så stort i bild saknas i den byggda formen.
            ++count;
        }
    }
    return count;
}

TileScheduler::Latency TileScheduler::latency() const {
    Latency l;
    l.count = latencies_.size();
    if (latencies_.empty()) return l;
    std::vector<double> sorted = latencies_;
    std::sort(sorted.begin(), sorted.end());
    const auto rank = [&](double fraction) {
        const size_t k = static_cast<size_t>(std::ceil(fraction * static_cast<double>(sorted.size())));
        return sorted[std::min(sorted.size(), std::max<size_t>(1, k)) - 1];
    };
    l.p50 = rank(0.5);
    l.p95 = rank(0.95);
    l.max = sorted.back();
    return l;
}

} // namespace nwdview
