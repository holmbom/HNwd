// Central plats för testdata. Byt filer här — inte i varje testfil.
//
// Filerna ligger inte i repot (nwd/ är gitignorerad). Exempelprojektet nedan
// är publikt; hämta och packa upp det med:
//
//   mkdir -p nwd/bimcollab
//   curl -L -o nwd/bimcollab.zip https://download.bimcollab.com/support/ZOOM/BIMcollab_Navisworks-example-file.zip
//   unzip nwd/bimcollab.zip -d nwd/bimcollab
//
// Sviten körs från repots rot. Saknas en fil hoppas testet över i stället för
// att krascha (SKIP_IF_MISSING).

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

// För utf8_to_wide/wide_to_utf8 nedan. Headern definierar NOMINMAX innan den
// drar in windows.h, så std::min och std::max överlever den här includen.
#include "path_utf8.h"

namespace testdata {

// Katalogen för testernas EGNA temporärfiler — inte för testdatan.
//
// På POSIX läses bara TMPDIR. Windows sätter inte den utan TEMP och TMP, så där
// provas TMPDIR först och sedan de två, och en körning som sätter TMPDIR beter
// sig lika på båda plattformarna.
inline std::string tmp_dir() {
    std::string s;
#ifdef _WIN32
    // _wgetenv, inte getenv: getenv ger byten i den aktiva ANSI-kodsidan, och
    // ett TEMP under `C:\Users\Åsa\` är då inte giltig UTF-8.
    for (const wchar_t* var : {L"TMPDIR", L"TEMP", L"TMP"}) {
        const wchar_t* d = _wgetenv(var);
        if (!d || !*d) continue;
        s = HNwd::detail::wide_to_utf8(d);
        if (!s.empty()) break;
    }
    if (s.empty()) s = ".";
#else
    if (const char* d = std::getenv("TMPDIR")) { if (*d) s = d; }
    if (s.empty()) s = "/tmp";
#endif
    // Bort med avslutande separator: anroparen lägger på sin egen.
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\'))
        s.pop_back();
    return s;
}

// Snedstrecket framåt fungerar även på Windows.
inline std::string tmp_path(const std::string& name) {
    return tmp_dir() + "/" + name;
}

// ifstream med en wchar_t*-sökväg finns i både MSVC och libstdc++ på MinGW.
// const wchar_t*, inte std::wstring: den överlagringen finns bara i MSVC.
inline std::ifstream ifstream_utf8(const std::string& path,
                                   std::ios::openmode mode = std::ios::binary) {
#ifdef _WIN32
    const std::wstring w = HNwd::detail::utf8_to_wide(path);
    if (w.empty()) return std::ifstream();
    return std::ifstream(w.c_str(), mode);
#else
    return std::ifstream(path, mode);
#endif
}

inline bool remove_utf8(const std::string& path) {
    return HNwd::detail::remove_utf8(path);
}

// Exempelprojektet: nio modeller som var sin .nwc, och en .nwd där de är
// sammanslagna.
inline constexpr const char* kSampleNwd = "nwd/bimcollab/Navisworks.nwd";
inline constexpr const char* kSampleNwc[] = {
    "nwd/bimcollab/01_BIMcollab_Example_ARC.nwc",
    "nwd/bimcollab/02_BIMcollab_Example_STR.nwc",
    "nwd/bimcollab/03_BIMcollab_Example_MEP.nwc",
    "nwd/bimcollab/04_BIMcollab_Example_Elevatorshaft.nwc",
    "nwd/bimcollab/05_BIMcollab_Example_Hollow_core_slabs.nwc",
    "nwd/bimcollab/06_BIMcollab_Example_Limestone walls.nwc",
    "nwd/bimcollab/07_BIMcollab_Example_Structural Steel.nwc",
    "nwd/bimcollab/08_BIMcollab_Example_Ventilation.nwc",
    "nwd/bimcollab/09_BIMcollab_Example_Heating_Plumbing.nwc",
};

// Formatversionen i katalogsektionens rubrik, och antalet paket i katalogen.
inline constexpr int kSampleVersion = 448;
inline constexpr size_t kSampleNwdPackages = 23;
inline constexpr size_t kSampleNwcPackages = 17;

// Vad varje exempelfil innehåller. Talen är paketens egna räknare:
// geometriposter och chunkar ur geometrins innehållsförteckning, fragment,
// GUID, egenskapsrötter ur trädets huvud, och trädets toppnoder med sin klass.
struct SampleCounts {
    const char* path;
    uint32_t records, chunks, fragments, guids, property_roots, top_nodes;
    int top_class;
};
inline constexpr SampleCounts kSamples[] = {
    {kSampleNwc[0], 1449, 24, 3759, 1723, 287, 1, 53},
    {kSampleNwc[1], 245, 1, 355, 250, 137, 1, 53},
    {kSampleNwc[2], 1795, 50, 4914, 1979, 13, 1, 53},
    {kSampleNwc[3], 44, 1, 534, 784, 254, 1, 53},
    {kSampleNwc[4], 308, 24, 537, 414, 19, 1, 53},
    {kSampleNwc[5], 78, 1, 82, 48, 55, 1, 53},
    {kSampleNwc[6], 25, 1, 161, 106, 32, 1, 53},
    {kSampleNwc[7], 1587, 23, 6521, 425, 13, 1, 53},
    {kSampleNwc[8], 9545, 53, 39384, 1522, 14, 1, 53},
    {kSampleNwd, 14999, 163, 56247, 6075, 824, 9, 32},
};

// Det som är belagt om varje exempelfil utöver räknarna: den rumsliga
// hierarkins inre noder, löv, djup (roten är 0) och största löv, enhetskoden i
// filens egen partition, och den sista geometrichunkens uppackade storlek.
struct SampleFacts {
    const char* path;
    uint32_t spatial_inner, spatial_leaves, spatial_depth, spatial_max_leaf;
    int32_t unit_code;
    uint32_t last_chunk_bytes;
};
inline constexpr SampleFacts kFacts[] = {
    {kSampleNwc[0], 123, 723, 4, 75, 2, 42528},
    {kSampleNwc[1], 3, 10, 2, 81, 2, 63112},
    {kSampleNwc[2], 267, 1594, 4, 57, 2, 39896},
    {kSampleNwc[3], 11, 54, 3, 17, 2, 34744},
    {kSampleNwc[4], 40, 232, 3, 13, 2, 34848},
    {kSampleNwc[5], 1, 3, 1, 52, 2, 24168},
    {kSampleNwc[6], 5, 27, 2, 15, 2, 22016},
    {kSampleNwc[7], 64, 368, 4, 188, 0, 50696},
    {kSampleNwc[8], 90, 529, 4, 686, 0, 17400},
    {kSampleNwd, 633, 3777, 5, 536, 2, 33352},
};

// I exempelfilen med flera modeller: antalet poser med skalan 1000, som är
// fragmenten i de två modellerna med enhetskod 0.
inline constexpr uint32_t kSampleNwdScaledPoses = 45905;

inline bool exists(const char* path) {
    std::ifstream f = ifstream_utf8(path);
    return f.good();
}

} // namespace testdata

// Antalet prov som hoppats över för att testdata saknas. Slutraden i main()
// säger det, så att en körning utan testdata inte ser ut som en fullständig.
inline int g_skipped = 0;

// Hoppar över resten av testfunktionen när testdata saknas — i stället för att
// köra vidare mot en misslyckad open.
#define SKIP_IF_MISSING(path)                                                  \
    do {                                                                       \
        if (!testdata::exists(path)) {                                         \
            std::printf("  SKIP — %s saknas\n", (path));                       \
            ++g_skipped;                                                       \
            return 0;                                                          \
        }                                                                      \
    } while (0)
