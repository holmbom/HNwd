// Den ersatta operator new för testkörbaren: allokerar med malloc, som
// standardbibliotekets egen, och noterar storleken medan en mätning pågår.

#include "alloc_probe.h"

#include <atomic>
#include <cstdlib>
#include <new>

namespace {

std::atomic<bool> g_on{false};
std::atomic<size_t> g_largest{0};

void note(size_t n) {
    if (!g_on.load(std::memory_order_relaxed)) return;
    size_t cur = g_largest.load(std::memory_order_relaxed);
    while (n > cur && !g_largest.compare_exchange_weak(cur, n, std::memory_order_relaxed)) {
    }
}

void* allocate(size_t n) {
    note(n);
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}

} // namespace

namespace alloc_probe {

void start() {
    g_largest.store(0, std::memory_order_relaxed);
    g_on.store(true, std::memory_order_relaxed);
}

size_t stop() {
    g_on.store(false, std::memory_order_relaxed);
    return g_largest.load(std::memory_order_relaxed);
}

} // namespace alloc_probe

void* operator new(size_t n) { return allocate(n); }
void* operator new[](size_t n) { return allocate(n); }
void* operator new(size_t n, const std::nothrow_t&) noexcept {
    note(n);
    return std::malloc(n ? n : 1);
}
void* operator new[](size_t n, const std::nothrow_t&) noexcept {
    note(n);
    return std::malloc(n ? n : 1);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
