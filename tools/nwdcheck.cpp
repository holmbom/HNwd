// nwdcheck — prövar samband i en fil som HNwd inte själv kräver.
//
//   nwdcheck <fil> --poses
//
// --poses sätter för varje fragment ihop transformerna längs vägen från
// trädets rot till fragmentets löv (klass 40, 41 och 39, nodens första) och
// geometrins mittpunkt c, och jämför med fragmentets egen pose: punkten i
// geometrins mitt och den linjära delen (se pose_check.h). Utskriften har en
// rad per poseklass med antalet, hur många som stämmer och de största
// avvikelserna. Klass 17 prövas med matrisen läst kolumnvis och radvis, och
// klass 39 i trädet med sina nio tal som rader och som kolumner när det spelar
// roll; verktyget säger vilken tolkning som stämmer. Varje partition (klass 32)
// i trädet skrivs med sin enhet, kvoten mot den omgivande enheten och om
// nodens transform är just den kvoten.
//
// Avslutningskoden är 0 när varje fragment stämmer med någon tolkning och
// varje partition bär sin enhet, 1 annars eller vid ett läsfel, 2 vid fel
// argument.

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <string>
#include <vector>

#include "mapped_file.h"
#include "nwd_error.h"
#include "pose_check.h"
#include "tool_main.h"

namespace {

constexpr const char* kUsage = "usage: nwdcheck <file> --poses\n";

const char* class_name(uint8_t pose_class) {
    switch (pose_class) {
    case 0: return "null";
    case 14: return "14";
    case 15: return "15";
    case 16: return "16";
    default: return "17";
    }
}

void print_row(const char* label, const posecheck::ClassStats& s) {
    std::printf("  %-28s %10" PRIu64 " %10" PRIu64 " %10" PRIu64 " %10" PRIu64 "   %-12.4g %-12.4g\n",
                label, s.count, s.match, s.point_match, s.linear_match, s.max_point, s.max_linear);
}

const char* verdict_text(posecheck::Verdict v, const char* first, const char* second) {
    switch (v) {
    case posecheck::Verdict::NoData: return "not decided";
    case posecheck::Verdict::Both: return "not decided: both readings agree";
    case posecheck::Verdict::First: return first;
    case posecheck::Verdict::Second: return second;
    default: return "NEITHER reading agrees";
    }
}

int run_poses(const std::string& path) {
    const auto started = std::chrono::steady_clock::now();
    HNwd::detail::MappedFile file;
    if (const HNwd::detail::ErrorCode e = file.open(path.c_str()); e != HNwd::detail::ErrorCode::Ok) {
        std::fprintf(stderr, "error: %s: %s\n", path.c_str(), HNwd::detail::error_message(e));
        return 1;
    }
    posecheck::Input in;
    std::string why;
    if (const HNwd::detail::ErrorCode e = posecheck::load(file.data(), file.size(), in, why);
        e != HNwd::detail::ErrorCode::Ok) {
        std::fprintf(stderr, "error: %s: %s\n", HNwd::detail::error_message(e), why.c_str());
        return 1;
    }
    posecheck::Report r;
    posecheck::check(in, r);

    double doc_metres = 0;
    std::printf("file:        %s\n", path.c_str());
    if (!in.has_doc_unit)
        std::printf("unit:        the file has no LcOpNwdPartition\n");
    else if (HNwd::detail::metres_per_unit(in.doc_unit, doc_metres))
        std::printf("unit:        code %d, %.6g metres per unit\n", in.doc_unit, doc_metres);
    else
        std::printf("unit:        code %d, not known\n", in.doc_unit);
    std::printf("tree:        %" PRIu64 " nodes expanded; transforms: class 40 %" PRIu64
                ", class 41 %" PRIu64 ", class 39 %" PRIu64 " (mirroring flag set %" PRIu64
                ", asymmetric matrix %" PRIu64 ")\n",
                r.nodes, r.transforms[0], r.transforms[1], r.transforms[2], r.mirroring39,
                r.asymmetric39);
    std::printf("fragments:   %" PRIu64 "\n", r.fragments);
    std::printf("relation:    pose = T(top) * ... * T(leaf) * translate(c); tolerance %.0e relative\n",
                posecheck::kTolerance);

    // Partitionerna.
    bool units_ok = true;
    if (r.partitions.empty()) {
        std::printf("\npartitions:  none in the tree; the coordinates are in the file's own unit\n");
    } else {
        std::printf("\npartitions (class 32) in the tree:\n");
        std::printf("  %8s %6s %6s %12s %10s %12s %10s  %s\n", "node", "unit", "outer", "ratio",
                    "transform", "deviation", "fragments", "unit in tree");
        for (const posecheck::PartitionCheck& p : r.partitions) {
            if (!p.consistent) units_ok = false;
            char ratio[32] = "unknown", deviation[32] = "-", transform[16] = "none";
            if (p.known) {
                std::snprintf(ratio, sizeof ratio, "%.6g", p.ratio);
                std::snprintf(deviation, sizeof deviation, "%.3g", p.deviation);
            }
            if (p.transform_class) std::snprintf(transform, sizeof transform, "%d", p.transform_class);
            std::printf("  %8u %6d %6d %12s %10s %12s %10" PRIu64 "  %s\n", p.node, p.unit_code,
                        p.outer_code, ratio, transform, deviation, p.fragments,
                        p.consistent ? "yes" : (p.known ? "NO" : "unit not known"));
        }
        std::printf("  fragments under a partition with a ratio other than 1: %" PRIu64 "\n",
                    r.fragments_scaled);
    }

    // Klasserna.
    std::printf("\nposes:\n");
    std::printf("  %-28s %10s %10s %10s %10s   %-12s %-12s\n", "class", "count", "match", "point",
                "linear", "max point", "max linear");
    const int orders39 = r.order39_matters ? 2 : 1;
    for (int slot = 0; slot < posecheck::kClassCount; ++slot) {
        const uint8_t cls = posecheck::kClasses[slot];
        for (int v = 0; v < orders39; ++v) {
            const char* tree = !r.order39_matters ? "" : (v == 0 ? ", tree 39 rows" : ", tree 39 columns");
            if (cls == 17) {
                for (int o = 0; o < 2; ++o) {
                    const std::string label = std::string("17 ") + (o == 0 ? "columns" : "rows") + tree;
                    print_row(label.c_str(), r.stats[slot][v][o]);
                }
            } else {
                const std::string label = std::string(class_name(cls)) + tree;
                print_row(label.c_str(), r.stats[slot][v][0]);
            }
        }
    }

    std::printf("\nclass 39 in the tree: %s\n",
                r.transforms[2] == 0 ? "not in the file"
                : !r.order39_matters
                    ? "not decided: no asymmetric class-39 matrix lies on a fragment's path"
                    : verdict_text(posecheck::verdict39(r), "rows (the nine numbers are the matrix row by row)",
                                   "columns (the nine numbers are the matrix column by column)"));
    const posecheck::Order39 best39 =
        posecheck::verdict39(r) == posecheck::Verdict::Second ? posecheck::Order39::Columns : posecheck::Order39::Rows;
    std::printf("pose class 17:        %s\n",
                r.stats[4][0][0].count == 0
                    ? "not in the file"
                    : verdict_text(posecheck::verdict17(r, best39), "columns (as FragmentStore reads it)",
                                   "rows (FragmentStore reads it transposed)"));
    if (r.unmatched)
        std::printf("NOT MATCHED:          %" PRIu64 " fragments agree with no reading; the first is fragment %" PRIu64 "\n",
                    r.unmatched, r.first_unmatched);
    else
        std::printf("all fragments:        match\n");
    if (!units_ok) std::printf("UNITS:                a partition's transform is not its unit ratio\n");

    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::printf("time:                 %.2f s\n", seconds);
    return r.unmatched == 0 && units_ok ? 0 : 1;
}

} // namespace

int tool_run(const std::vector<std::string>& args) {
    std::string path;
    bool poses = false;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--poses") {
            poses = true;
        } else if (args[i].size() > 1 && args[i][0] == '-' && args[i][1] == '-') {
            std::fputs(kUsage, stderr);
            return 2;
        } else if (path.empty()) {
            path = args[i];
        } else {
            std::fputs(kUsage, stderr);
            return 2;
        }
    }
    if (path.empty() || !poses) {
        std::fputs(kUsage, stderr);
        return 2;
    }
    return run_poses(path);
}
