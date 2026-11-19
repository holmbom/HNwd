// nwdview — en visare för .nwd och .nwc.
//
//   nwdview [fil] [--png bild.png] [--view namn] [--budget MB] [--gpu-budget MB]
//           [--pick-cache MB] [--tile N] [--min-pixels N] [--max-draws N] [--max-triangles M]
//           [--size-margin F] [--no-lines] [--tour] [--spin varv] [--profile]
//           [--pacing vsync|all|N]
//           [--gpu integrated|discrete] [--uv raw|realworld]
//           [--eye x y z --look x y z] [--walk-mode] [--follow] [--walk meter]
//           [--speed m/s] [--clip on|off] [--clip-test "px py pz nx ny nz"]...
//   nwdview --decode-jpeg in.jpg ut.png [--max-side N]
//
// Modellen delas i bitar: varje fragment hör till en bit i ett detaljträd
// byggt ovanpå det rumsliga indexet, de stora fragmenten i stora noder och de
// små i små. En bit byggs, visas och släpps för sig, efter hur stort dess
// största fragment är i bild. Garantin är att varje fragment som är minst
// --min-pixels px stort (förval 3) syns; bitar vars största fragment är
// mindre än så byggs inte. Räcker inte minnet höjs tröskeln, och statusraden
// säger då till exempel "allt ≥ 7 px syns (minnet räcker inte för 3 px)". De
// största bitarna, upp till en femtedel av budgeten, är fasta: de byggs först,
// är det som syns innan resten kommer och släpps aldrig. Lådorna visas tills
// de är byggda. Varje mesh har detaljnivåer, så att ett fragment som blivit för
// litet i bild inte ritas; en bit under ritningens tröskel parkeras, och visas
// igen utan ny uppladdning. --tile är största antalet trianglar i en bit
// (förval 32768).
//
// En bit byggs bara till den storlek som behövs: de av dess fragment som är
// minst så stora som minnets tröskel på bitens avstånd, delad med två, ett
// prefix eftersom bitens fragment står störst först. När ögat kommer så nära
// att ett mindre fragment blir minst tröskeln i bild byggs biten om med en
// mindre storlek; den gamla versionen visas tills den nya är klar och den nya tar
// då dess platser i scenen. Fasta bitar byggs hela. Modellens linjer är en egen
// del i varje bit: kvadar (fyra hörn och två trianglar per segment) som ett eget
// program ritar med en fast bredd i px, med samma detaljnivåer, parkering,
// ordning och ritade andel som ytorna, i ett lager för linjer med en liten
// djupbias mot ögat så att en linje i samma plan som en yta syns ovanpå den.
// --no-lines ritar inga linjer. Snittplanen klipper linjerna som ytorna.
//
// Ritningens tröskel är den lägsta storlek i px vars ritade trianglar och
// ritanrop ryms under taken, uppskattat ur de byggda bitar som är i bild, och
// den flyttas av ritningens egen kostnad per bild (mål 22 ms): grafikkortets
// tid och ritfasens tid ur ramverkets profilerare, den större av dem. Är den över minnets
// tröskel står det i statusraden ("ritar ≥ 6 px för bildtakten"); det den
// stänger ute är byggt och visas igen när det går. --max-draws är taket för
// ritanrop per bild (förval 8000) och --max-triangles taket för ritade trianglar
// i miljoner.
// Medan kameran rör sig och bildtiden ändå är över målet sänks
// kantutjämningen till ett prov per pixel, och den höjs igen när kameran stått
// still en stund; uppladdningen till grafikkortet är mindre per bild medan
// kameran rör sig. De visade bitarnas former ligger i lagren med den närmaste
// först. Bygget tar också med kameran som den väntas stå om en sekund, ett
// påbörjat jobb som fallit långt under tröskeln avbryts, och en bit vars nivåer
// bakades för en mindre bild byggs om när bilden vuxit mer än marginalen.
// Med --png är ritningens tröskel bara uppskattningen (utom under --walk,
// --spin och --tour), kantutjämningen ändras aldrig och urvalet mäts med
// bildens storlek.
//
// --budget är RAM i MB och --gpu-budget grafikkortets minne i MB. Utan flaggor
// räknas förvalen ur maskinens fysiska minne, ur grafikkortets minne (ett diskret
// kort; ett integrerat kort, delat minne eller ett okänt delar budget med RAM)
// och ur dokumentets storlek; går maskinens minne inte att läsa står ett fel på
// stderr och båda budgeterna blir 1500 MB. Läsarnas cache, 64 MB per läsare
// (16 MB när RAM-budgeten är under 8 GB; en läsare per byggtråd och tre till för
// plockningen), plockningens geometricache (--pick-cache, förval 256 MB, högst en
// fjärdedel av RAM-budgeten) och detaljträdet räknas som fast RAM i budgeten.
//
// Fönstret hänger aldrig: UI-tråden tar bara emot färdiga svar, sätter former i
// lagren och uppdaterar texter, och allt som läser filen, räknar över bitar och
// fragment eller träffprovar går i arbetstrådar. Urvalet (vilka bitar som byggs,
// släpps, visas och parkeras, hur mycket av varje del som ritas, ordningen i
// lagren och ritningens tröskel) har en egen tråd; fönstret lämnar kameran och
// de byggda bitarnas svar och får färdiga listor tillbaka. Det som görs i
// UI-tråden i flera steg (visa och parkera bitar, sätta om hur mycket av en del
// som ritas, byta platser, släppa bitar, lägga om snittet) delar en tidsbudget om
// ett par ms per pumpvarv. En bit som parkeras står kvar i sina platser och ritas
// med noll element, och en plats som blir ledig får en liten platshållarmesh, så
// att ramverket inte bygger om lagren; meshar släpps i en arbetstråd, och texterna
// ändras högst två gånger per sekund. Scenen ritas och laddas upp av ritytans
// renderingstråd; uppladdningen
// är 32 MiB per bild medan kameran rör sig och 64 MiB när den står
// still. Med --profile skriver visaren hur många stopp över 4 ms som inträffat i
// UI-tråden efter startvyn och det längsta; ett stopp över 16 ms ger ett fel på
// stderr.
//
// Ett klick på ett objekt och koordinaten under pekaren kommer strax efter, inte
// genast: strålen genom pekaren tas i UI-tråden och ett jobb i en arbetstråd
// prövar bitarna längs den, närmast först, mot deras geometri (läst ur filen
// första gången, sedan ur geometricachen) och svarar med träffen. Ett nytt klick
// avbryter ett äldre; koordinaten har högst ett jobb åt gången och den senaste
// punkten gäller. Ramverkets egen plockning rör inte modellens meshar. I
// gångläget med markföljning söker ett jobb ytan under ögat ur geometricachen
// och ett annat håller geometrin för bitarna under ögat där; ögat står kvar på
// sin höjd tills ytan är funnen.
//
// Med --png väntar visaren tills grundbilden och detaljen för startvyn är
// klara, ritar den till en bild och avslutas. Bilden tas om tills två i följd
// är lika, så att bilderna ur materialen hunnit laddas upp; blir den inte
// stabil inom tidsgränsen står ett fel på stderr. --tour zoomar först in och
// ut igen, så att detalj byggs och släpps innan bilden ritas. --spin varv
// kretsar kameran runt målet, ett varv på 10 s, innan bilden tas; bildtakten
// (intervallens p50/p95/p99, bilder över 20 ms, ritanrop och trianglar per bild)
// skrivs som med --walk, och --profile skriver byggtiderna, hörn per triangel,
// hur många bitar som byggts om till en mindre storlek och hur stor del av de
// byggda bitarnas primitiver (trianglar och linjesegment) som är byggda mot
// hela bitarnas, postanvändningen och minnet.
//
// --eye och --look sätter startkameran: ögat och punkten det ser mot, i meter
// i modellens koordinater (som statusraden visar dem). --walk-mode startar i
// gångläget, --follow håller där ögat över ytan under, --speed är gångfarten
// (förval 1,5 m/s). --walk med --png går så många meter rakt fram i gångläget
// innan bilden tas, med verklig tid per bildruta, och skriver hur mycket
// detalj som saknades under gången och bildtakten; bilden när gången slutar skrivs som
// <bild>.gang.png.
//
// Visaren startar i filens aktuella vy när den ser på modellen, annars
// passas modellen in. --view startar i den sparade vyn med det namnet (en
// vy som saknas är ett fel), och listan Vyer flyttar kameran till en vy. --profile skriver
// tiderna och GlazeUI:s profilrapport på standard ut när fönstret stängs.
//
// En vy med påslagna snittplan klipper modellen när den väljs (också
// startvyn); Visa → Snitt slår av och på, och statusraden säger hur många
// plan som är på. --clip off startar med snittet avslaget. Vilken sida av ett
// plan filen behåller är inte belagt: visaren behåller det som ligger mellan
// ett par plan med motsatta normaler, och utan ett par den sida där vyns öga
// står (clip_from_camera() i model.h). --clip-test lägger ett plan genom
// punkten med normalen, i meter i modellens koordinater, i stället för
// startvyns, och behåller sidan normalen pekar mot; det kan anges upp till sex
// gånger. Det är till för att pröva snittet i en fil utan vyer med snitt.
//
// --gpu väljer grafikkort: discrete (förval) ber om det kraftfullaste,
// integrated om det strömsnålaste. --uv säger hur materialens bildkoordinater
// tolkas: realworld (förval) delar dem med bildens verkliga storlek när
// materialet anger den, raw tar dem som de står.
//
// --decode-jpeg avkodar en JPEG-fil med visarens egen avkodare och skriver
// den som PNG, utan fönster; --max-side skalar ner som för materialens
// bilder.
//
// Vänsterdrag vrider, mittendrag panorerar, hjulet zoomar. F2 (eller Visa →
// Gångläge) byter till gångläget: W A S D eller pilarna går, Shift springer,
// Q E svänger, R F eller PageUp PageDown ändrar höjden, ett drag med musen ser sig om,
// styrplattans tvåfingersdrag svänger i sidled och lutar blicken uppåt och nedåt, och plus,
// minus och hjulet ändrar gångfarten. Statusraden visar
// världskoordinaten i meter under pekaren; ett klick på ett objekt visar dess
// namn och egenskaper till höger, och listan till vänster visar och döljer
// modellerna.

#include "jpeg.h"
#include "viewer_window.h"

#include "path_utf8.h"

#ifdef _WIN32
#include <shellapi.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

bool read_file(const std::string& path, std::vector<uint8_t>& out) {
#ifdef _WIN32
    FILE* f = _wfopen(HNwd::detail::utf8_to_wide(path).c_str(), L"rb");
#else
    FILE* f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n < 0) {
        std::fclose(f);
        return false;
    }
    out.resize(static_cast<size_t>(n));
    const bool ok = n == 0 || std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

int decode_jpeg_file(const std::string& in, const std::string& out, int max_side) {
    std::vector<uint8_t> bytes;
    if (!read_file(in, bytes)) {
        std::fprintf(stderr, "nwdview: %s kunde inte läsas\n", in.c_str());
        return 1;
    }
    nwdview::DecodedImage image;
    std::string error;
    if (!nwdview::decode_jpeg(bytes.data(), bytes.size(), max_side, image, error)) {
        std::fprintf(stderr, "nwdview: %s: %s\n", in.c_str(), error.c_str());
        return 1;
    }
    if (!gfx::encodePng(out, image.rgba, image.width, image.height)) {
        std::fprintf(stderr, "nwdview: %s kunde inte skrivas\n", out.c_str());
        return 1;
    }
    std::printf("%s: %d × %d, skrivet %d × %d\n", in.c_str(), image.source_width,
                image.source_height, image.width, image.height);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args;
#ifdef _WIN32
    // Argumenten som UTF-16, så att en sökväg med tecken utanför den aktiva
    // kodsidan kommer fram.
    (void)argc;
    (void)argv;
    int count = 0;
    if (wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count)) {
        for (int i = 1; i < count; ++i) args.push_back(HNwd::detail::wide_to_utf8(wide[i]));
        LocalFree(wide);
    }
#else
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
#endif
    nwdview::ViewerOptions options;
    std::string jpeg_in, jpeg_out;
    int max_side = 0;
    std::string gpu = "discrete";
    bool has_eye = false, has_look = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        const bool has_value = i + 1 < args.size();
        if (a == "--png" && has_value) {
            options.png = args[++i];
        } else if (a == "--view" && has_value) {
            options.view = args[++i];
        } else if (a == "--budget" && has_value) {
            options.budget_mb = std::max<size_t>(
                1, static_cast<size_t>(std::strtoull(args[++i].c_str(), nullptr, 10)));
        } else if (a == "--gpu-budget" && has_value) {
            options.gpu_budget_mb = std::max<size_t>(
                1, static_cast<size_t>(std::strtoull(args[++i].c_str(), nullptr, 10)));
        } else if (a == "--size-margin" && has_value) {
            char* end = nullptr;
            const double v = std::strtod(args[++i].c_str(), &end);
            if (!end || *end != '\0' || !(v >= 1) || !std::isfinite(v)) {
                std::fprintf(stderr, "nwdview: --size-margin tar ett tal minst 1: %s\n",
                             args[i].c_str());
                return 2;
            }
            options.size_margin = v;
        } else if (a == "--no-lines") {
            options.lines = false;
        } else if (a == "--pick-cache" && has_value) {
            options.pick_cache_mb = std::max<size_t>(
                1, static_cast<size_t>(std::strtoull(args[++i].c_str(), nullptr, 10)));
        } else if (a == "--tile" && has_value) {
            options.tile_triangles = std::max<uint64_t>(
                1, static_cast<uint64_t>(std::strtoull(args[++i].c_str(), nullptr, 10)));
        } else if (a == "--min-pixels" && has_value) {
            const std::string& v = args[++i];
            char* end = nullptr;
            const double d = std::strtod(v.c_str(), &end);
            if (end == v.c_str() || *end != '\0' || !(d > 0)) {
                std::fprintf(stderr, "nwdview: --min-pixels tar ett tal större än noll: %s\n",
                             v.c_str());
                return 2;
            }
            options.min_pixels = d;
        } else if (a == "--max-draws" && has_value) {
            const std::string& v = args[++i];
            char* end = nullptr;
            const unsigned long long n = std::strtoull(v.c_str(), &end, 10);
            if (end == v.c_str() || *end != '\0' || n == 0 || n > 0xFFFFFFFFull) {
                std::fprintf(stderr, "nwdview: --max-draws tar ett heltal större än noll: %s\n",
                             v.c_str());
                return 2;
            }
            options.governor.max_draws = static_cast<uint32_t>(n);
        } else if (a == "--max-triangles" && has_value) {
            const std::string& v = args[++i];
            char* end = nullptr;
            const double m = std::strtod(v.c_str(), &end);
            if (end == v.c_str() || *end != '\0' || !(m > 0)) {
                std::fprintf(stderr,
                             "nwdview: --max-triangles tar ett tal större än noll (miljoner): %s\n",
                             v.c_str());
                return 2;
            }
            options.governor.max_triangles = static_cast<uint64_t>(m * 1e6);
        } else if (a == "--pacing" && has_value) {
            // all (varje bild hel, förvalet), vsync eller bilder per sekund.
            const std::string v = args[++i];
            if (v == "vsync") {
                options.pacing_fps = 0;
            } else if (v == "all") {
                options.pacing_fps = -1;
            } else {
                char* end = nullptr;
                const double fps = std::strtod(v.c_str(), &end);
                if (end == v.c_str() || *end != '\0' || !(fps > 0)) {
                    std::fprintf(stderr, "nwdview: --pacing tar vsync, all eller bilder per sekund\n");
                    return 2;
                }
                options.pacing_fps = fps;
            }
        } else if (a == "--profile") {
            options.profile = true;
        } else if (a == "--tour") {
            options.tour = true;
        } else if (a == "--gpu" && has_value) {
            gpu = args[++i];
            if (gpu != "integrated" && gpu != "discrete") {
                std::fprintf(stderr, "nwdview: --gpu tar integrated eller discrete\n");
                return 2;
            }
        } else if (a == "--uv" && has_value) {
            const std::string& v = args[++i];
            if (v == "raw") {
                options.uv = nwdview::UvMode::Raw;
            } else if (v == "realworld") {
                options.uv = nwdview::UvMode::RealWorld;
            } else {
                std::fprintf(stderr, "nwdview: --uv tar raw eller realworld\n");
                return 2;
            }
        } else if ((a == "--eye" || a == "--look") && i + 3 < args.size()) {
            double* p = a == "--eye" ? options.eye : options.look;
            for (int k = 0; k < 3; ++k) {
                const std::string& v = args[++i];
                char* end = nullptr;
                p[k] = std::strtod(v.c_str(), &end);
                if (end == v.c_str() || *end != '\0') {
                    std::fprintf(stderr, "nwdview: %s tar tre tal i meter: %s\n", a.c_str(),
                                 v.c_str());
                    return 2;
                }
            }
            if (a == "--eye") has_eye = true;
            else has_look = true;
        } else if (a == "--walk-mode") {
            options.walk = true;
        } else if (a == "--follow") {
            options.follow_ground = true;
        } else if ((a == "--walk" || a == "--speed" || a == "--spin") && has_value) {
            const std::string& v = args[++i];
            char* end = nullptr;
            const double d = std::strtod(v.c_str(), &end);
            if (end == v.c_str() || *end != '\0' || !(d > 0)) {
                std::fprintf(stderr, "nwdview: %s tar ett tal större än noll: %s\n", a.c_str(),
                             v.c_str());
                return 2;
            }
            if (a == "--walk") options.walk_distance = d;
            else if (a == "--spin") options.spin = d;
            else options.walk_speed = d;
        } else if (a == "--clip" && has_value) {
            const std::string& v = args[++i];
            if (v == "on") {
                options.clip = true;
            } else if (v == "off") {
                options.clip = false;
            } else {
                std::fprintf(stderr, "nwdview: --clip tar on eller off\n");
                return 2;
            }
        } else if (a == "--clip-test" && has_value) {
            const std::string& v = args[++i];
            double t[6];
            const char* s = v.c_str();
            bool ok = true;
            for (double& x : t) {
                char* end = nullptr;
                x = std::strtod(s, &end);
                if (end == s) {
                    ok = false;
                    break;
                }
                s = end;
            }
            while (ok && *s == ' ') ++s;
            if (!ok || *s != '\0' || !nwdview::add_clip_plane(options.clip_test, t, t + 3)) {
                std::fprintf(stderr,
                             "nwdview: --clip-test tar \"px py pz nx ny nz\": en punkt och en "
                             "normal som inte är noll, i meter, högst sex plan: %s\n",
                             v.c_str());
                return 2;
            }
        } else if (a == "--decode-jpeg" && i + 2 < args.size()) {
            jpeg_in = args[++i];
            jpeg_out = args[++i];
        } else if (a == "--max-side" && has_value) {
            max_side = std::max(0, std::atoi(args[++i].c_str()));
        } else if (options.path.empty()) {
            options.path = a;
        } else {
            std::fprintf(stderr, "nwdview: okänt argument: %s\n", a.c_str());
            return 2;
        }
    }
    if (!jpeg_in.empty()) return decode_jpeg_file(jpeg_in, jpeg_out, max_side);
    if (options.spin > 0 && options.png.empty()) {
        std::fprintf(stderr, "nwdview: --spin kräver --png\n");
        return 2;
    }
    if (has_eye != has_look) {
        std::fprintf(stderr, "nwdview: --eye och --look anges tillsammans\n");
        return 2;
    }
    options.has_eye = has_eye;
    if (options.walk_distance > 0) options.walk = true;

    // Före första fönstret: kortet väljs när fönstret skapar sin renderare.
    gfx::setGpuPreference(gpu == "integrated" ? gfx::GpuPreference::LowPower
                                              : gfx::GpuPreference::HighPerformance);

    const bool profile = options.profile;
    // Med --png avgör profileraren när uppladdningen till grafikkortet är klar.
    if (profile || !options.png.empty()) glaze::profiler().setEnabled(true);

    int code = 0;
    {
        nwdview::ViewerWindow window(std::move(options));
        code = window.run();
        if (code == 0) code = window.result();
        if (profile) {
            const std::string text =
                "nwdview: tider\n" + window.timings() + glaze::profiler().report();
            std::fwrite(text.data(), 1, text.size(), stdout);
            std::fflush(stdout);
        }
    }
    return code;
}
