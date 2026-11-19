// nwdsynth — skriver en syntetisk fil med formatets belagda grammatiker, för
// prov med stora filer.
//
//   nwdsynth <ut.nwd> --fragments N --records M --nodes K [--no-spatial]
//            [--unit 0|1|2|3] [--seed S] [--sizes A:B] [--detail N]
//
// Filen följer helt ur fröet (förval 1) och skrivs strömmande, så att också
// miljontals fragment, poster och noder går att skriva med lite minne. Vad
// filen innehåller står i synth_writer.h. --unit är enhetskoden i filens
// partition (förval 2, millimeter). --no-spatial utelämnar det rumsliga
// trädet, så att läsaren får bygga ett eget. --sizes A:B ger varje posts
// diagonal log-likformigt mellan A och B meter (utan flaggan är posterna
// 0,5 till 5 m långa), och --detail N upp till N segment i en posts strip
// (förval 12).

#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "synth_writer.h"
#include "tool_main.h"

namespace {

constexpr const char* kUsage =
    "usage: nwdsynth <out.nwd> --fragments N --records M --nodes K [--no-spatial] "
    "[--unit 0|1|2|3] [--seed S] [--sizes A:B] [--detail N]\n";

bool parse_count(const std::string& text, uint64_t& out) {
    if (text.empty() || text[0] < '0' || text[0] > '9') return false;
    char* end = nullptr;
    errno = 0;
    out = std::strtoull(text.c_str(), &end, 10);
    return end && *end == '\0' && errno != ERANGE;
}

// Ett tal som börjar med en siffra.
bool parse_metres(const std::string& text, double& out) {
    if (text.empty() || text[0] < '0' || text[0] > '9') return false;
    char* end = nullptr;
    errno = 0;
    out = std::strtod(text.c_str(), &end);
    return end && *end == '\0' && errno != ERANGE;
}

// "A:B" som två mått i meter.
bool parse_sizes(const std::string& text, double& lo, double& hi) {
    const size_t colon = text.find(':');
    if (colon == std::string::npos) return false;
    return parse_metres(text.substr(0, colon), lo) && parse_metres(text.substr(colon + 1), hi);
}

} // namespace

int tool_run(const std::vector<std::string>& args) {
    synth::Options o;
    std::string path;
    bool has_f = false, has_r = false, has_n = false;
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        const bool value = i + 1 < args.size();
        uint64_t v = 0;
        if (a == "--fragments" && value && parse_count(args[++i], o.fragments)) {
            has_f = true;
        } else if (a == "--records" && value && parse_count(args[++i], o.records)) {
            has_r = true;
        } else if (a == "--nodes" && value && parse_count(args[++i], o.nodes)) {
            has_n = true;
        } else if (a == "--seed" && value && parse_count(args[++i], o.seed)) {
        } else if (a == "--unit" && value && parse_count(args[++i], v) && v <= 3) {
            o.unit = static_cast<int32_t>(v);
        } else if (a == "--sizes" && value && parse_sizes(args[++i], o.min_size, o.max_size)) {
        } else if (a == "--detail" && value && parse_count(args[++i], v) && v >= 1 &&
                   v <= synth::kMaxSegments) {
            o.max_segments = static_cast<uint32_t>(v);
        } else if (a == "--no-spatial") {
            o.spatial = false;
        } else if (!a.empty() && a[0] != '-' && path.empty()) {
            path = a;
        } else {
            std::fputs(kUsage, stderr);
            return 2;
        }
    }
    if (path.empty() || !has_f || !has_r || !has_n) {
        std::fputs(kUsage, stderr);
        return 2;
    }

    const auto started = std::chrono::steady_clock::now();
    synth::Result r;
    std::string why;
    if (!synth::write(path, o, r, why)) {
        std::fprintf(stderr, "error: %s\n", why.c_str());
        return 1;
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    std::printf("file:        %s\n", path.c_str());
    std::printf("size:        %" PRIu64 " byte (%.2f GB), zlib with stored blocks\n", r.file_bytes,
                static_cast<double>(r.file_bytes) / 1e9);
    std::printf("unit code:   %d\n", o.unit);
    std::printf("seed:        %" PRIu64 "\n", o.seed);
    if (o.max_size > 0)
        std::printf("sizes:       %g to %g m (record diagonal)\n", o.min_size, o.max_size);
    std::printf("detail:      up to %u segments per record\n", o.max_segments);
    std::printf("records:     %" PRIu64 " (triangles %" PRIu64 ", lines %" PRIu64 "), %" PRIu64
                " byte in %" PRIu64 " chunks\n",
                o.records, r.triangle_records, r.line_records, r.geometry_bytes, r.chunks);
    std::printf("fragments:   %" PRIu64 "; poses null %" PRIu64 ", 14 %" PRIu64 ", 15 %" PRIu64
                ", 16 %" PRIu64 ", 17 %" PRIu64 "\n",
                o.fragments, r.poses[0], r.poses[1], r.poses[2], r.poses[3], r.poses[4]);
    std::printf("tree:        %" PRIu64 " nodes: %" PRIu64 " leaves, %" PRIu64 " groups (%" PRIu64
                " empty); %" PRIu64 " property roots\n",
                o.nodes, r.leaves, r.groups, r.empty_groups, r.property_roots);
    std::printf("spatial:     %s\n", o.spatial ? "written" : "left out");
    std::printf("packages:\n");
    for (const synth::PackageInfo& p : r.packages)
        std::printf("  %-26s %14" PRIu64 " byte at %" PRIu64 "\n", p.name.c_str(), p.length, p.offset);
    std::printf("time:        %.2f s\n", seconds);
    return 0;
}
