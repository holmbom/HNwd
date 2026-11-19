#include "governor.h"

#include <algorithm>
#include <cmath>

namespace nwdview {

namespace {

// Golvet för de gällande taken, som del av inställningarnas.
constexpr double kMinScale = 0.05;

// Hur mycket taken höjs vid varje anpassning med en bildtid under kRaiseBelow.
constexpr double kRaiseStep = 1.15;

// Det minsta taket sänks vid ett steg: en halvering.
constexpr double kMinCut = 0.5;

// Del av målet: under det här höjs taken.
constexpr double kRaiseBelow = 0.7;

// Del av målet: under det här faller reduce_quality() tillbaka.
constexpr double kReleaseBelow = 0.85;

// Det som ryms vid en sänkning av tröskeln, som del av taken.
constexpr double kLowerSlack = 0.9;

// Tröskeln sänks bara om den ryms minst så här mycket lägre.
constexpr double kLowerStep = 1.1;

// Sökningen slutar när högsta och lägsta gränsen ligger så här nära.
constexpr double kSearchRatio = 1.02;
constexpr int kMaxIterations = 12;

bool finite_positive(double v) { return v > 0 && std::isfinite(v); }

} // namespace

DrawGovernor::DrawGovernor(const GovernorSettings& s) : s_(s) {
    if (s_.max_triangles < 1) s_.max_triangles = 1;
    if (s_.max_draws < 1) s_.max_draws = 1;
    if (!(s_.target_ms >= 1e-3)) s_.target_ms = 1e-3;
    if (!(s_.min_pixels >= 1e-6)) s_.min_pixels = 1e-6;
    if (!(s_.max_pixels >= s_.min_pixels)) s_.max_pixels = s_.min_pixels;
    pixels_ = s_.min_pixels;
    // Ett tal som inte är ett tal (eller under golvet) börjar på golvet.
    if (!(s_.start_scale > kMinScale)) s_.start_scale = kMinScale;
    if (s_.start_scale > 1) s_.start_scale = 1;
    scale_ = s_.start_scale;
}

uint64_t DrawGovernor::triangle_cap() const {
    if (scale_ >= 1) return s_.max_triangles;
    return std::max<uint64_t>(1, static_cast<uint64_t>(static_cast<double>(s_.max_triangles) * scale_));
}

uint32_t DrawGovernor::draw_cap() const {
    if (scale_ >= 1) return s_.max_draws;
    return std::max<uint32_t>(1, static_cast<uint32_t>(static_cast<double>(s_.max_draws) * scale_));
}

bool DrawGovernor::fits(const DrawLoad& l, double factor) const {
    const double triangles = std::max(1.0, static_cast<double>(s_.max_triangles) * factor);
    const double draws = std::max(1.0, static_cast<double>(s_.max_draws) * factor);
    return static_cast<double>(l.triangles) <= triangles && static_cast<double>(l.draws) <= draws;
}

// Den lägsta tröskeln i [lo, hi] vars belastning ryms i `factor` gånger
// inställningarnas tak, eller hi när ingen gör det. `lo_unfit` och `hi_fit` är
// kända svar som sparar ett anrop.
double DrawGovernor::search(const std::function<DrawLoad(double)>& load_at, double factor,
                            double lo, double hi, bool lo_unfit, bool hi_fit) const {
    if (!lo_unfit && fits(load_at(lo), factor)) return lo;
    if (!(hi > lo)) return hi;
    if (!hi_fit && !fits(load_at(hi), factor)) return hi;
    // lo ryms inte och hi ryms: halvera geometriskt, eftersom tröskeln är en
    // storlek i bild.
    for (int i = 0; i < kMaxIterations && hi > lo * kSearchRatio; ++i) {
        const double mid = std::sqrt(lo * hi);
        if (fits(load_at(mid), factor)) hi = mid;
        else lo = mid;
    }
    return hi;
}

// Lägger till ett prov och räknar den utjämnade bildtiden. Svarar om det nu
// finns nog med prov för ett värde.
bool DrawGovernor::add_sample(double ms) {
    samples_[sample_next_] = ms;
    sample_next_ = (sample_next_ + 1) % kWindow;
    const size_t n = std::min(sample_count_ + 1, kWindow);
    sample_count_ = n;
    if (n < kMinSamples) return false;
    std::array<double, kWindow> sorted = samples_;
    // Insättningssortering av de n första: högst fem tal.
    for (size_t i = 1; i < n; ++i) {
        const double v = sorted[i];
        size_t j = i;
        for (; j > 0 && sorted[j - 1] > v; --j) sorted[j] = sorted[j - 1];
        sorted[j] = v;
    }
    smoothed_ms_ = sorted[n / 2];
    // Vid ett jämnt antal prov är medianen medelvärdet av de två i mitten.
    if (n % 2 == 0) smoothed_ms_ = (smoothed_ms_ + sorted[n / 2 - 1]) / 2;
    return true;
}

// Flyttar taken efter den utjämnade bildtiden, och sätter reduce_quality().
void DrawGovernor::adapt() {
    const double target = s_.target_ms;
    if (smoothed_ms_ > target) {
        const bool at_max = pixels_ >= s_.max_pixels * (1 - 1e-9);
        const bool at_floor = scale_ <= kMinScale * (1 + 1e-9);
        if (at_max || at_floor) {
            // Tröskeln eller taken kan inte göra mer.
            reduce_ = true;
            return;
        }
        scale_ = std::max(kMinScale, scale_ * std::max(kMinCut, target / smoothed_ms_));
        // De utjämnade proven mäter bilderna före ändringen.
        sample_count_ = 0;
        sample_next_ = 0;
        return;
    }
    if (smoothed_ms_ < kReleaseBelow * target) reduce_ = false;
    if (smoothed_ms_ < kRaiseBelow * target && scale_ < 1) {
        scale_ = std::min(1.0, scale_ * kRaiseStep);
        // De utjämnade proven mäter bilderna före höjningen.
        sample_count_ = 0;
        sample_next_ = 0;
    }
}

double DrawGovernor::next(const std::function<DrawLoad(double pixels)>& load_at, double frame_ms,
                          bool deterministic) {
    const double lo = s_.min_pixels, hi = s_.max_pixels;
    if (deterministic) {
        pixels_ = search(load_at, 1.0, lo, hi, false, false);
        return pixels_;
    }
    if (finite_positive(frame_ms) && add_sample(frame_ms)) adapt();

    // Hysteres: högre genast när belastningen vid den nuvarande tröskeln är
    // över taken; lägre först när den ryms i nio tiondelar vid en tröskel
    // minst en tiondel lägre.
    const double cur = pixels_;
    double result = cur;
    if (!fits(load_at(cur), scale_)) {
        result = search(load_at, scale_, cur, hi, true, false);
    } else if (cur > lo) {
        const double probe = std::max(lo, cur / kLowerStep);
        const double slack = kLowerSlack * scale_;
        if (fits(load_at(probe), slack))
            result = probe <= lo ? lo : search(load_at, slack, lo, probe, false, true);
    }
    pixels_ = std::clamp(result, lo, hi);
    return pixels_;
}

} // namespace nwdview
