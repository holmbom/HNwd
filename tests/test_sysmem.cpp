// Systemets minne och budgetförvalen (viewer/sysmem.h). Ren std och HNwd —
// ingen fönstertyp — så det prövas utan visaren.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "sysmem.h"
#include "test_support.h"

using namespace nwdview;

namespace {

constexpr uint64_t kMiB = uint64_t(1) << 20;
constexpr uint64_t kGiB = uint64_t(1) << 30;

// Det riktiga systemet: alla tre talen finns, och de hänger ihop.
int check_system_memory() {
    int fails = 0;
    SystemMemory before;
    std::string error;
    CHECK(system_memory(before, error));
    if (!error.empty()) std::printf("  system_memory: %s\n", error.c_str());
    CHECK(before.physical > 0);
    CHECK(before.available > 0);
    CHECK(before.process > 0);
    CHECK(before.available <= before.physical);
    CHECK(before.process <= before.physical);

    // Processens minne växer med minne som tas och skrivs: hälften av 64 MiB
    // märks i mätningen.
    {
        std::vector<unsigned char> block(64 * kMiB, 1);
        uint64_t sum = 0;
        for (size_t i = 0; i < block.size(); i += 4096) sum += block[i];
        CHECK(sum == block.size() / 4096);
        SystemMemory during;
        CHECK(system_memory(during, error));
        CHECK(during.process >= before.process + 32 * kMiB);
    }

    // Ett lyckat svar skriver över allt som stod i `out`.
    SystemMemory stale;
    stale.physical = 1;
    stale.available = 2;
    stale.process = 3;
    CHECK(system_memory(stale, error));
    CHECK(stale.physical == before.physical);
    CHECK(stale.available > 0 && stale.process > 0);
    return fails;
}

SystemMemory machine(uint64_t physical) {
    SystemMemory sys;
    sys.physical = physical;
    sys.available = physical / 2;
    sys.process = 100 * kMiB;
    return sys;
}

// Förvalen: RAM är hälften av det fysiska minnet minus dokumentet, minst
// 512 MiB; ett diskret kort ger 60 % av sitt minne som egen budget, och annars
// delar de två en budget.
int check_default_budgets() {
    int fails = 0;

    // Hälften minus dokumentet; diskret kort med egen budget.
    {
        const Budgets b = default_budgets(machine(16 * kGiB), 8 * kGiB, false, size_t(1 * kGiB));
        CHECK(b.budget.ram == 7 * kGiB);
        CHECK(b.budget.gpu == 8 * kGiB * 6 / 10);
        CHECK(!b.shared);
    }
    // Utan dokument är RAM exakt hälften.
    {
        const Budgets b = default_budgets(machine(32 * kGiB), 4 * kGiB, false, 0);
        CHECK(b.budget.ram == 16 * kGiB);
        CHECK(b.budget.gpu == 4 * kGiB * 6 / 10);
    }
    // Dokumentet som tar mer än hälften, och en liten maskin: golvet.
    {
        const Budgets b = default_budgets(machine(4 * kGiB), 2 * kGiB, false, size_t(3 * kGiB));
        CHECK(b.budget.ram == 512 * kMiB);
        const Budgets small = default_budgets(machine(512 * kMiB), 2 * kGiB, false, 0);
        CHECK(small.budget.ram == 512 * kMiB);
        // Dokumentet precis lika stort som hälften ger också golvet.
        const Budgets equal = default_budgets(machine(4 * kGiB), 2 * kGiB, false, size_t(2 * kGiB));
        CHECK(equal.budget.ram == 512 * kMiB);
    }
    // Strax över golvet följer talet hälften minus dokumentet.
    {
        const Budgets b = default_budgets(machine(4 * kGiB), 2 * kGiB, false, size_t(1536 * kMiB));
        CHECK(b.budget.ram == 512 * kMiB);
        const Budgets above = default_budgets(machine(4 * kGiB), 2 * kGiB, false, size_t(1535 * kMiB));
        CHECK(above.budget.ram == 513 * kMiB);
    }
    // Ett integrerat kort delar minne med processorn: en budget, lika stor som
    // RAM-budgeten.
    {
        const Budgets b = default_budgets(machine(16 * kGiB), 2 * kGiB, true, size_t(1 * kGiB));
        CHECK(b.shared);
        CHECK(b.budget.ram == 7 * kGiB);
        CHECK(b.budget.gpu == b.budget.ram);
    }
    // Ett kort utan känd storlek räknas som delat, också utan att det sagts.
    {
        const Budgets b = default_budgets(machine(16 * kGiB), 0, false, 0);
        CHECK(b.shared);
        CHECK(b.budget.ram == 8 * kGiB);
        CHECK(b.budget.gpu == b.budget.ram);
        const Budgets both = default_budgets(machine(16 * kGiB), 0, true, 0);
        CHECK(both.shared && both.budget.ram == 8 * kGiB);
    }
    // Ett diskret kort ger sin budget oavsett RAM, och 60 % räknas heltalsriktigt
    // också för tal som inte går jämnt upp.
    {
        const Budgets b = default_budgets(machine(8 * kGiB), 1001, false, 0);
        CHECK(!b.shared);
        CHECK(b.budget.gpu == 600);
        const Budgets c = default_budgets(machine(8 * kGiB), 1009, false, 0);
        CHECK(c.budget.gpu == 605);
    }
    // Ett okänt fysiskt minne ger golvet.
    {
        const Budgets b = default_budgets(SystemMemory{}, 8 * kGiB, false, 0);
        CHECK(b.budget.ram == 512 * kMiB);
    }
    return fails;
}

} // namespace

int test_sysmem() {
    int fails = 0;
    fails += check_system_memory();
    fails += check_default_budgets();
    std::printf("test_sysmem: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
