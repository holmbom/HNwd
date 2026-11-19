#pragma once

// Systemets minne och minnesbudgetarna som följer av det. Ren std och HNwd —
// inga fönstertyper — så att samma kod går att använda utanför visaren.

#include "cells.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace nwdview {

// Minnet i byte: maskinens fysiska minne, det av det som går att ta i bruk
// utan att något annat skrivs ut till disk, och det som processen har i
// minnet just nu (arbetsmängden på Windows, resident storlek på Linux och
// macOS; minnesmappade filer räknas med).
struct SystemMemory {
    uint64_t physical = 0, available = 0, process = 0;
};

// Läser minnet av systemet. false och en text i `error` när systemet inte
// svarar, eller inte säger allt som behövs; `out` är då orörd.
bool system_memory(SystemMemory& out, std::string& error);

// Budgetarna för detaljerna, i RAM och på grafikkortet. Med `shared` är det en
// budget för båda: budget.ram gäller RAM och grafikkort tillsammans (och
// budget.gpu är lika stor, men används inte).
struct Budgets {
    PartBytes budget;
    bool shared = false;
};

// Förvalen:
//
// - RAM: hälften av det fysiska minnet minus dokumentets minne
//   (`document_bytes`, Document::memory_bytes()), dock minst 512 MiB.
// - Grafikkort: 60 % av `gpu_memory` för ett diskret kort.
// - För ett integrerat kort (`gpu_shared`), delat minne eller okänd
//   storlek (`gpu_memory` 0) räknas de två i en gemensam budget, lika stor
//   som RAM-budgeten.
Budgets default_budgets(const SystemMemory& sys, uint64_t gpu_memory, bool gpu_shared,
                        size_t document_bytes);

} // namespace nwdview
