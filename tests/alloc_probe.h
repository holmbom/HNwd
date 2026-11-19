// Mäter den största enskilda allokeringen under ett anrop. Testkörbaren
// ersätter operator new (alloc_probe.cpp); utanför en mätning räknas inget.
//
//   alloc_probe::Scope probe;
//   … anropet …
//   CHECK(probe.largest() < gräns);

#pragma once

#include <cstddef>

namespace alloc_probe {

// Startar en mätning och nollställer det största värdet.
void start();
// Slutar mäta. Ger den största allokeringen sedan start(), i byte.
size_t stop();

class Scope {
public:
    Scope() { start(); }
    ~Scope() {
        if (!stopped_) stop();
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    // Den största allokeringen sedan Scope skapades; mätningen slutar.
    size_t largest() {
        if (!stopped_) {
            largest_ = stop();
            stopped_ = true;
        }
        return largest_;
    }

private:
    bool stopped_ = false;
    size_t largest_ = 0;
};

} // namespace alloc_probe
