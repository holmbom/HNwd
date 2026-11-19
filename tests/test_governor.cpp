// Ritbudgeten (viewer/governor.h): ritningens tröskel mot taken för trianglar
// och ritanrop, hysteresen, takens rörelse med bildtiden och utjämningen,
// det deterministiska läget och reduce_quality(). Ren std — ingen fönstertyp.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>

#include "governor.h"
#include "test_support.h"

using namespace nwdview;

namespace {

using LoadFn = std::function<DrawLoad(double)>;

// Belastningen vid tröskeln p: trianglarna avtar som 1/p² och ritanropen som
// 1/p, så som när större och större bitar bara ritas. `calls` räknar anropen.
LoadFn make_load(double triangles0, double draws0, int* calls = nullptr) {
    return [=](double p) {
        if (calls) ++*calls;
        DrawLoad l;
        l.triangles = static_cast<uint64_t>(triangles0 / (p * p));
        l.draws = static_cast<uint32_t>(draws0 / p);
        return l;
    };
}

GovernorSettings base() {
    GovernorSettings s;
    s.max_triangles = 1'000'000;
    s.max_draws = 1000;
    s.target_ms = 1000.0 / 45;
    s.min_pixels = 3;
    s.max_pixels = 32;
    s.start_scale = 1;   // de flesta prov utgår från de hela taken
    return s;
}

// ---------------------------------------------------------------------------
// Tröskeln mot taken.

int check_search() {
    int fails = 0;
    // Allt ryms vid min_pixels: svaret är exakt min_pixels.
    {
        DrawGovernor g(base());
        CHECK(g.pixels() == 3);
        CHECK(g.next(make_load(4e6, 1000), 0, false) == 3);
        CHECK(g.pixels() == 3);
        CHECK(!g.reduce_quality());
    }
    // Trianglarna begränsar: den lägsta tröskeln vars belastning ryms är
    // sqrt(1e8 / 1e6) = 10 px. Svaret ryms, och ligger högst ett par procent
    // över.
    {
        int calls = 0;
        DrawGovernor g(base());
        const double r = g.next(make_load(1e8, 1000, &calls), 0, false);
        CHECK(r > 9.99 && r < 10.25);
        CHECK(make_load(1e8, 1000)(r).triangles <= 1'000'000);
        CHECK(calls <= 16);
        // Belastningen står still: nästa svar är detsamma, med få anrop.
        calls = 0;
        CHECK(g.next(make_load(1e8, 1000, &calls), 0, false) == r);
        CHECK(calls <= 3);
    }
    // Ritanropen begränsar: 20 000 / p <= 1 000 ger 20 px.
    {
        DrawGovernor g(base());
        const double r = g.next(make_load(1e6, 20000), 0, false);
        CHECK(r > 19.99 && r < 20.45);
        CHECK(make_load(1e6, 20000)(r).draws <= 1000);
    }
    // Ingenting ryms: max_pixels.
    {
        DrawGovernor g(base());
        CHECK(g.next(make_load(1e12, 1e6), 0, false) == 32);
    }
    // Ett taket som just ryms: tröskeln står kvar på min_pixels.
    {
        DrawGovernor g(base());
        const LoadFn exact = [](double) {
            DrawLoad l;
            l.triangles = 1'000'000;
            l.draws = 1000;
            return l;
        };
        CHECK(g.next(exact, 0, false) == 3);
        const LoadFn over = [](double) {
            DrawLoad l;
            l.triangles = 1'000'001;
            l.draws = 1;
            return l;
        };
        CHECK(g.next(over, 0, false) == 32);
    }
    // Ogiltiga inställningar rättas: max_pixels under min_pixels blir min_pixels,
    // och noll som tak blir ett.
    {
        GovernorSettings s = base();
        s.min_pixels = 5;
        s.max_pixels = 2;
        DrawGovernor g(s);
        CHECK(g.next(make_load(1e12, 1e6), 0, false) == 5);
        CHECK(g.next(make_load(1, 1), 0, false) == 5);

        GovernorSettings z = base();
        z.max_triangles = 0;
        z.max_draws = 0;
        DrawGovernor gz(z);
        CHECK(gz.triangle_cap() == 1 && gz.draw_cap() == 1);
        const LoadFn nothing = [](double) { return DrawLoad{}; };
        CHECK(gz.next(nothing, 0, false) == 3);
    }
    return fails;
}

// ---------------------------------------------------------------------------
// Hysteresen.

int check_hysteresis() {
    int fails = 0;
    // Belastningen faller till 95 % av taket: tröskeln står kvar. Faller den
    // till hälften sänks den till där nio tiondelar av taket ryms (7,45 px), och
    // står sedan kvar. En tung belastning höjer den genast.
    {
        DrawGovernor g(base());
        const double r1 = g.next(make_load(1e8, 1000), 0, false);
        CHECK(r1 > 9.99 && r1 < 10.25);
        CHECK(g.next(make_load(0.95e8, 1000), 0, false) == r1);
        const double r2 = g.next(make_load(0.5e8, 1000), 0, false);
        CHECK(r2 > 7.4 && r2 < 7.65);
        CHECK(make_load(0.5e8, 1000)(r2).triangles <= 900'000);
        CHECK(g.next(make_load(0.5e8, 1000), 0, false) == r2);
        const double r3 = g.next(make_load(3e8, 1000), 0, false);
        CHECK(r3 > 17.2 && r3 < 17.8);
        CHECK(g.pixels() == r3);
    }
    // En belastning som svänger litet kring taket får inte fladdra: svaren
    // aldrig sjunker, och en andra omgång med samma värden ger samma svar hela
    // vägen.
    {
        DrawGovernor g(base());
        const double mult[] = {1.0, 1.04, 0.97, 1.03, 0.99, 1.02, 0.96, 1.01};
        double prev = 0, last = 0;
        for (const double m : mult) {
            const double r = g.next(make_load(1e8 * m, 1000), 0, false);
            CHECK(r >= prev);
            prev = r;
            last = r;
        }
        for (const double m : mult) CHECK(g.next(make_load(1e8 * m, 1000), 0, false) == last);
    }
    // Under min_pixels går den inte, och med ingen belastning står den på
    // min_pixels.
    {
        DrawGovernor g(base());
        g.next(make_load(1e8, 1000), 0, false);
        const LoadFn none = [](double) { return DrawLoad{}; };
        double r = 1e9;
        for (int i = 0; i < 20; ++i) r = g.next(none, 0, false);
        CHECK(r == 3);
    }
    return fails;
}

// ---------------------------------------------------------------------------
// Takens rörelse med bildtiden, och utjämningen.

int check_caps() {
    int fails = 0;
    // En bildtid under målet sänker inte taken, och en enstaka lång bild
    // slår inte igenom: medianen av de fem senaste proven är 10 ms.
    {
        DrawGovernor g(base());
        for (int i = 0; i < 4; ++i) g.next(make_load(1e8, 1000), 10, false);
        CHECK(g.triangle_cap() == 1'000'000 && g.draw_cap() == 1000);
        g.next(make_load(1e8, 1000), 300, false);
        CHECK(g.triangle_cap() == 1'000'000 && g.draw_cap() == 1000);
        CHECK(g.smoothed_ms() > 9.9 && g.smoothed_ms() < 20);
    }
    // En bildtid över målet sänker taken i proportion: 40 ms mot 22,2 ms ger
    // en faktor 0,556 efter tre prov, och tröskeln höjs.
    {
        DrawGovernor g(base());
        const double before = g.next(make_load(1e8, 1000), 40, false);
        g.next(make_load(1e8, 1000), 40, false);
        CHECK(g.triangle_cap() == 1'000'000);   // två prov räcker inte
        g.next(make_load(1e8, 1000), 40, false);
        CHECK(g.triangle_cap() > 540'000 && g.triangle_cap() < 570'000);
        CHECK(g.draw_cap() > 540 && g.draw_cap() < 570);
        CHECK(g.smoothed_ms() == 40);
        CHECK(g.pixels() > 13 && g.pixels() > before);
        CHECK(make_load(1e8, 1000)(g.pixels()).triangles <= g.triangle_cap());
        // De utjämnade proven glöms vid ändringen: två nya prov ändrar inget.
        g.next(make_load(1e8, 1000), 40, false);
        g.next(make_load(1e8, 1000), 40, false);
        CHECK(g.triangle_cap() > 540'000 && g.triangle_cap() < 570'000);

        // Taken sänks tills tröskeln står på sitt tak; sedan rörs de inte, och
        // de går aldrig under golvet.
        for (int i = 0; i < 100; ++i) g.next(make_load(1e8, 1000), 40, false);
        CHECK(g.triangle_cap() >= 49'999 && g.triangle_cap() < 570'000);
        CHECK(g.draw_cap() >= 50 && g.draw_cap() < 570);
        CHECK(g.pixels() == 32);
        CHECK(g.reduce_quality());
    }
    // Under 70 % av målet höjs de tillbaka, 15 % per tre bilder och aldrig över
    // inställningarnas tak, och reduce_quality() faller tillbaka.
    {
        DrawGovernor g(base());
        for (int i = 0; i < 100; ++i) g.next(make_load(1e8, 1000), 40, false);
        CHECK(g.reduce_quality());
        const uint64_t floor_cap = g.triangle_cap();
        uint64_t prev = floor_cap;
        int steps_to_double = 0;
        for (int i = 0; i < 600; ++i) {
            g.next(make_load(1e8, 1000), 5, false);
            CHECK(g.triangle_cap() <= 1'000'000);
            CHECK(g.triangle_cap() >= prev);
            if (!steps_to_double && g.triangle_cap() >= 2 * floor_cap) steps_to_double = i;
            prev = g.triangle_cap();
        }
        CHECK(g.triangle_cap() == 1'000'000 && g.draw_cap() == 1000);
        CHECK(!g.reduce_quality());
        // Fem höjningar med 15 % fördubblar, en var tredje bild: det tar mer
        // än tio anrop och mindre än fyrtio.
        CHECK(steps_to_double >= 10 && steps_to_double <= 40);
        // Tröskeln är tillbaka nära det som ryms i nio tiondelar av taken
        // (10,5 px); den sänks i steg om minst en tiondel.
        CHECK(g.pixels() > 10.5 && g.pixels() < 12.0);
    }
    // Mellan 70 % och 100 % av målet står taken kvar.
    {
        DrawGovernor g(base());
        for (int i = 0; i < 3; ++i) g.next(make_load(1e8, 1000), 40, false);
        const uint64_t cap = g.triangle_cap();
        CHECK(cap < 1'000'000);
        for (int i = 0; i < 50; ++i) g.next(make_load(1e8, 1000), 20, false);
        CHECK(g.triangle_cap() == cap);
    }
    // En okänd bildtid (0) lämnar taken och proven orörda.
    {
        DrawGovernor g(base());
        for (int i = 0; i < 3; ++i) g.next(make_load(1e8, 1000), 40, false);
        const uint64_t cap = g.triangle_cap();
        const double smoothed = g.smoothed_ms();
        for (int i = 0; i < 50; ++i) g.next(make_load(1e8, 1000), 0, false);
        CHECK(g.triangle_cap() == cap && g.smoothed_ms() == smoothed);
        // Och negativa eller ogiltiga värden räknas som okända.
        g.next(make_load(1e8, 1000), -5, false);
        g.next(make_load(1e8, 1000), std::nan(""), false);
        CHECK(g.triangle_cap() == cap && g.smoothed_ms() == smoothed);
    }
    return fails;
}

// ---------------------------------------------------------------------------
// Starten och höjningen.

int check_start() {
    int fails = 0;
    // Förvalet är en femtedel, och taken börjar där.
    {
        const GovernorSettings d;
        CHECK(d.start_scale == 0.2);
        CHECK(d.max_draws == 800);
        GovernorSettings s = base();
        s.start_scale = d.start_scale;
        DrawGovernor g(s);
        CHECK(g.triangle_cap() >= 199'999 && g.triangle_cap() <= 200'001);
        CHECK(g.draw_cap() >= 199 && g.draw_cap() <= 201);
    }
    // Starten rättas till [golvet, 1]; ett tal som inte är ett tal blir golvet.
    {
        GovernorSettings s = base();
        s.start_scale = 0;
        CHECK(DrawGovernor(s).triangle_cap() >= 49'999 && DrawGovernor(s).triangle_cap() <= 50'001);
        s.start_scale = -3;
        CHECK(DrawGovernor(s).draw_cap() == 50);
        s.start_scale = std::nan("");
        CHECK(DrawGovernor(s).draw_cap() == 50);
        s.start_scale = 7;
        CHECK(DrawGovernor(s).triangle_cap() == 1'000'000 && DrawGovernor(s).draw_cap() == 1000);
        s.start_scale = 1;
        CHECK(DrawGovernor(s).triangle_cap() == 1'000'000);
    }
    // Det första svaret räknas mot de låga taken: en belastning som ryms i de
    // hela taken vid 10 px ryms i en femtedel först vid sqrt(1e8 / 2e5) = 22,4
    // px (trianglarna) — och ritanropen vid 5 px ger inget mer.
    {
        GovernorSettings s = base();
        s.start_scale = 0.2;
        DrawGovernor low(s);
        const double r = low.next(make_load(1e8, 1000), 0, false);
        CHECK(r > 22.3 && r < 22.9);
        DrawGovernor full(base());
        const double f = full.next(make_load(1e8, 1000), 0, false);
        CHECK(f > 9.99 && f < 10.25);
        CHECK(r > f);
    }
    // Det deterministiska läget använder hela taken också med en låg start,
    // och rör inte regleringens tillstånd.
    {
        GovernorSettings s = base();
        s.start_scale = 0.2;
        DrawGovernor g(s);
        const double r = g.next(make_load(1e8, 1000), 1000, true);
        CHECK(r > 9.99 && r < 10.25);
        CHECK(g.triangle_cap() >= 199'999 && g.triangle_cap() <= 200'001);
    }
    // Höjningen: bildtiden håller (5 ms mot 22 ms). Första höjningen kommer
    // med det tredje provet och är 15 %; sedan glöms proven, så nästa kommer
    // tre prov senare. Aldrig över inställningarnas tak.
    {
        GovernorSettings s = base();
        s.start_scale = 0.2;
        DrawGovernor g(s);
        const LoadFn load = make_load(1e8, 1000);
        g.next(load, 5, false);
        g.next(load, 5, false);
        CHECK(g.triangle_cap() >= 199'999 && g.triangle_cap() <= 200'001);
        g.next(load, 5, false);
        CHECK(g.triangle_cap() >= 229'000 && g.triangle_cap() <= 231'000);
        CHECK(g.draw_cap() >= 229 && g.draw_cap() <= 231);
        g.next(load, 5, false);
        g.next(load, 5, false);
        CHECK(g.triangle_cap() >= 229'000 && g.triangle_cap() <= 231'000);
        g.next(load, 5, false);
        CHECK(g.triangle_cap() >= 264'000 && g.triangle_cap() <= 265'000);

        // Taken når inställningarnas tak på ungefär en sekund (60 bilder) och
        // går aldrig över det.
        uint64_t prev = g.triangle_cap();
        int frames_to_full = 0;
        for (int i = 0; i < 500; ++i) {
            g.next(load, 5, false);
            CHECK(g.triangle_cap() <= 1'000'000 && g.draw_cap() <= 1000);
            CHECK(g.triangle_cap() >= prev);
            prev = g.triangle_cap();
            if (!frames_to_full && g.triangle_cap() == 1'000'000) frames_to_full = i + 1;
        }
        CHECK(frames_to_full > 0 && frames_to_full <= 60);
        CHECK(g.triangle_cap() == 1'000'000 && g.draw_cap() == 1000);
        CHECK(g.pixels() >= 3 && g.pixels() <= 32);
    }
    // Mellan 70 % och 100 % av målet höjs de inte, och över målet sänks de från
    // starten som vanligt.
    {
        GovernorSettings s = base();
        s.start_scale = 0.2;
        DrawGovernor g(s);
        for (int i = 0; i < 60; ++i) g.next(make_load(1e8, 1000), 20, false);
        CHECK(g.triangle_cap() >= 199'999 && g.triangle_cap() <= 200'001);
        for (int i = 0; i < 3; ++i) g.next(make_load(1e8, 1000), 44, false);
        CHECK(g.triangle_cap() < 120'000 && g.triangle_cap() >= 99'000);
    }
    // En okänd bildtid höjer ingenting: starten står kvar.
    {
        GovernorSettings s = base();
        s.start_scale = 0.2;
        DrawGovernor g(s);
        for (int i = 0; i < 50; ++i) g.next(make_load(1e8, 1000), 0, false);
        CHECK(g.triangle_cap() >= 199'999 && g.triangle_cap() <= 200'001);
    }
    return fails;
}

// ---------------------------------------------------------------------------
// Det deterministiska läget.

int check_deterministic() {
    int fails = 0;
    DrawGovernor slow(base());
    for (int i = 0; i < 30; ++i) slow.next(make_load(1e8, 1000), 40, false);
    const uint64_t cap = slow.triangle_cap();
    const uint32_t draws = slow.draw_cap();
    const double smoothed = slow.smoothed_ms();
    const bool reduced = slow.reduce_quality();
    CHECK(cap < 1'000'000);

    DrawGovernor fresh(base());
    const double a = slow.next(make_load(1e8, 1000), 1000, true);
    const double b = fresh.next(make_load(1e8, 1000), 0, true);
    const double c = fresh.next(make_load(1e8, 1000), 5, true);
    CHECK(a == b && b == c);
    CHECK(a > 9.99 && a < 10.25);   // bara inställningarnas tak
    // Inget som styr svaren ändras.
    CHECK(slow.triangle_cap() == cap && slow.draw_cap() == draws);
    CHECK(slow.smoothed_ms() == smoothed);
    CHECK(slow.reduce_quality() == reduced);
    CHECK(fresh.triangle_cap() == 1'000'000 && !fresh.reduce_quality() && fresh.smoothed_ms() == 0);
    CHECK(slow.pixels() == a);

    // Ingen hysteres: en lättare belastning ger genast det lägre svaret.
    const double lighter = slow.next(make_load(0.5e8, 1000), 1000, true);
    CHECK(lighter > 7.06 && lighter < 7.25);
    CHECK(slow.next(make_load(1e8, 1000), 0, true) == a);
    // Svaret hör bara till belastningen, hur många gånger det än frågas.
    for (int i = 0; i < 5; ++i) CHECK(slow.next(make_load(0.5e8, 1000), 0, true) == lighter);
    return fails;
}

// ---------------------------------------------------------------------------
// reduce_quality().

int check_reduce_quality() {
    int fails = 0;
    // Tröskeln står på max_pixels och bildtiden är över målet: sant efter tre
    // prov, och det står kvar mellan 85 % och 100 % av målet. Faller först när
    // den utjämnade bildtiden är under 85 % av målet.
    {
        DrawGovernor g(base());
        const LoadFn never = make_load(1e12, 1e6);
        for (int i = 0; i < 2; ++i) g.next(never, 40, false);
        CHECK(!g.reduce_quality());
        g.next(never, 40, false);
        CHECK(g.pixels() == 32);
        CHECK(g.reduce_quality());
        for (int i = 0; i < 10; ++i) g.next(never, 20, false);
        CHECK(g.reduce_quality());
        for (int i = 0; i < 6; ++i) g.next(never, 10, false);
        CHECK(!g.reduce_quality());
        // Utan bildtid står den kvar som den är.
        for (int i = 0; i < 3; ++i) g.next(never, 40, false);
        CHECK(g.reduce_quality());
        for (int i = 0; i < 10; ++i) g.next(never, 0, false);
        CHECK(g.reduce_quality());
    }
    // Belastningen ryms alltid men bildtiden är över målet ändå: taken sänks
    // till golvet och reduce_quality() blir sant där.
    {
        DrawGovernor g(base());
        const LoadFn tiny = make_load(1e4, 30);
        int i = 0;
        for (; i < 300 && !g.reduce_quality(); ++i) {
            CHECK(g.next(tiny, 40, false) == 3);
            CHECK(g.triangle_cap() >= 49'999);
        }
        CHECK(i < 300);
        CHECK(g.triangle_cap() <= 50'001);
    }
    // En bildtid under målet gör det aldrig sant.
    {
        DrawGovernor g(base());
        const LoadFn never = make_load(1e12, 1e6);
        for (int i = 0; i < 50; ++i) {
            g.next(never, 20, false);
            CHECK(!g.reduce_quality());
        }
    }
    // En enstaka lång bild gör det inte sant (medianen av fem prov).
    {
        DrawGovernor g(base());
        const LoadFn never = make_load(1e12, 1e6);
        for (int i = 0; i < 6; ++i) g.next(never, 10, false);
        g.next(never, 500, false);
        CHECK(!g.reduce_quality());
    }
    return fails;
}

} // namespace

int test_governor() {
    int fails = 0;
    fails += check_search();
    fails += check_hysteresis();
    fails += check_caps();
    fails += check_start();
    fails += check_deterministic();
    fails += check_reduce_quality();
    std::printf("test_governor: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
