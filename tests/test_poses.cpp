// Sambandet mellan objektträdets transformer och fragmentens poser
// (tools/pose_check.h): mot exempelfilerna, och poseklass 17 kolumnvis och
// radvis mot ett syntetiskt träd med en klass 39.

#include <cstdio>
#include <string>
#include <vector>

#include "../tools/pose_check.h"
#include "mapped_file.h"
#include "test_data.h"
#include "test_support.h"

using testdata::Bytes;
using testdata::ByteWriter;

namespace {

// Poseklassernas plats i rapporten.
constexpr int kNull = 0, k14 = 1, k15 = 2, k16 = 3, k17 = 4;

// ---------------------------------------------------------------------------
// Exempelfilerna: varje fragment stämmer, och enheten står i trädet.

int check_sample(const testdata::SampleCounts& s) {
    int fails = 0;
    SKIP_IF_MISSING(s.path);
    HNwd::detail::MappedFile file;
    CHECK(file.open(s.path) == HNwd::detail::ErrorCode::Ok);
    if (!file.is_open()) return fails;
    posecheck::Input in;
    std::string why;
    const HNwd::detail::ErrorCode ec = posecheck::load(file.data(), file.size(), in, why);
    CHECK(ec == HNwd::detail::ErrorCode::Ok);
    if (ec != HNwd::detail::ErrorCode::Ok) {
        std::printf("  %s: %s\n", s.path, why.c_str());
        return fails;
    }
    posecheck::Report r;
    posecheck::check(in, r);

    CHECK(r.fragments == s.fragments);
    CHECK(r.unmatched == 0);
    // Ingen osymmetrisk klass 39 och ingen klass 17: varje fragment stämmer
    // med den första tolkningen.
    CHECK(!r.order39_matters);
    CHECK(r.stats[k17][0][0].count == 0);
    uint64_t total = 0;
    for (int slot = 0; slot < posecheck::kClassCount; ++slot) {
        const posecheck::ClassStats& st = r.stats[slot][0][0];
        CHECK(st.match == st.count);
        total += st.count;
    }
    CHECK(total == s.fragments);

    for (const posecheck::PartitionCheck& p : r.partitions) CHECK(p.consistent);
    if (s.top_class == 32) {
        // En partition per modell; skalan 1000 i posen är just enhetens kvot,
        // som står i trädet.
        CHECK(r.partitions.size() == s.top_nodes);
        CHECK(r.fragments_scaled == testdata::kSampleNwdScaledPoses);
        CHECK(r.stats[k16][0][0].count == testdata::kSampleNwdScaledPoses);
    } else {
        CHECK(r.partitions.empty());
        CHECK(r.fragments_scaled == 0);
        CHECK(r.stats[k16][0][0].count == 0);
    }
    return fails;
}

// ---------------------------------------------------------------------------
// Ett syntetiskt träd: toppnoden med klass 39 (matrisen M radvis och flytten
// t39), en grupp med klass 40 (t1) och ett löv. Fragmentet har mittpunkten c
// och poseklass 17 med M och flytten M · (t1 + c) + t39.

// M = R · diag(2, 3, 0,5) med R 90 grader kring Z; inte symmetrisk, och exakt i f32.
constexpr double kM[9] = {0, -3, 0, 2, 0, 0, 0, 0, 0.5};
constexpr double kT39[3] = {100, 200, 300};
constexpr double kT1[3] = {10, 20, 30};
constexpr double kC[3] = {1, 2, 3};

Bytes tree_stream() {
    ByteWriter w;
    w.i32(1);
    w.i32(0);
    w.implicit_root();
    w.i32(1);
    w.obj(53);
    w.str("Top");
    w.null_ptr();
    w.i32(0);
    w.i32(0);
    w.i32(1);
    w.obj(39);
    w.null_str();
    w.null_ptr();
    w.i32(0);
    for (const double v : kM) w.f64(v);
    for (const double v : kT39) w.f64(v);
    w.i32(0);
    w.i32(1);
    w.obj(53);
    w.str("Mid");
    w.null_ptr();
    w.i32(0);
    w.i32(0);
    w.i32(1);
    w.obj(40);
    w.null_str();
    w.null_ptr();
    w.i32(0);
    for (const double v : kT1) w.f64(v);
    w.i32(1);
    w.obj(22);
    w.str("Leaf");
    w.null_ptr();
    w.i32(0);
    w.i32(0);
    w.i32(0);
    w.i32(1);
    w.pad(8);
    return w.data;
}

// Ett fragment på lövet (seq 3) med poseklass 17. `rows`: de tolv talen är
// matrisens rader i stället för kolumner. `shift` läggs till flytten.
Bytes fragment_stream(bool rows, double shift) {
    double t[3];
    for (int r = 0; r < 3; ++r)
        t[r] = kM[3 * r] * (kT1[0] + kC[0]) + kM[3 * r + 1] * (kT1[1] + kC[1]) +
               kM[3 * r + 2] * (kT1[2] + kC[2]) + kT39[r] + shift;
    ByteWriter w;
    w.i32(1);
    w.obj(13);
    w.f64(1.5);
    w.i32(2);
    w.i32(4);
    w.i32(7);
    w.i32(3);
    w.obj(17);
    for (int a = 0; a < 3; ++a) {
        for (int b = 0; b < 3; ++b) w.f32(static_cast<float>(rows ? kM[3 * a + b] : kM[3 * b + a]));
        w.f32(0);
    }
    w.f32(1);
    for (const double v : t) w.f64(v);
    w.obj(55);
    w.i32(1);
    w.obj(54);
    for (int i = 0; i < 14; ++i) w.f32(0.5f);
    w.obj(93);
    for (const float v : {-1.f, -1.f, -1.f, 1.f, 1.f, 1.f}) w.f32(v);
    for (const double v : kC) w.f64(v);
    w.f32(2.5f);
    w.i32(7);
    w.i32(1);
    w.pad(8);
    return w.data;
}

Bytes synthetic_file(bool rows, double shift) {
    using testdata::Package;
    using testdata::zlib_store;
    std::vector<Package> pk;
    pk.push_back({"LcOpNwdLogicalHierarchy", 1, 0, 0, zlib_store(tree_stream())});
    pk.push_back({"LcOpNwdFragments", 1, 0, 0, zlib_store(fragment_stream(rows, shift))});
    return testdata::build_file(pk, true);
}

bool run(const Bytes& file, posecheck::Report& r) {
    posecheck::Input in;
    std::string why;
    if (posecheck::load(file.data(), file.size(), in, why) != HNwd::detail::ErrorCode::Ok) {
        std::printf("  synthetic: %s\n", why.c_str());
        return false;
    }
    posecheck::check(in, r);
    return true;
}

int test_class17() {
    int fails = 0;
    posecheck::Report r;

    // Kolumnvis: matrisen läst som FragmentStore läser den stämmer, med
    // klass 39 radvis. Klass 39 kolumnvis ger fel punkt.
    CHECK(run(synthetic_file(false, 0), r));
    CHECK(r.fragments == 1);
    CHECK(r.order39_matters);
    CHECK(r.asymmetric39 == 1);
    CHECK(r.stats[k17][0][0].match == 1);   // 39 rader, 17 kolumner
    CHECK(r.stats[k17][0][1].match == 0);   // 39 rader, 17 rader
    CHECK(r.stats[k17][1][0].match == 0);
    CHECK(r.stats[k17][1][1].match == 0);
    CHECK(r.stats[k17][1][1].linear_match == 1);   // båda transponerade: samma matris, fel punkt
    CHECK(r.stats[k17][1][1].point_match == 0);
    CHECK(posecheck::verdict39(r) == posecheck::Verdict::First);
    CHECK(posecheck::verdict17(r, posecheck::Order39::Rows) == posecheck::Verdict::First);
    CHECK(r.unmatched == 0);

    // Radvis: bara den transponerade läsningen stämmer.
    CHECK(run(synthetic_file(true, 0), r));
    CHECK(r.stats[k17][0][0].match == 0);
    CHECK(r.stats[k17][0][1].match == 1);
    CHECK(posecheck::verdict17(r, posecheck::Order39::Rows) == posecheck::Verdict::Second);
    CHECK(r.unmatched == 0);

    // En flytt som inte stämmer med trädet stämmer med ingen tolkning.
    CHECK(run(synthetic_file(false, 0.5), r));
    CHECK(r.unmatched == 1);
    CHECK(r.stats[k17][0][0].point_match == 0);
    CHECK(r.stats[k17][0][0].linear_match == 1);
    CHECK(posecheck::verdict17(r, posecheck::Order39::Rows) == posecheck::Verdict::Neither);
    CHECK(r.stats[kNull][0][0].count == 0);
    CHECK(r.stats[k14][0][0].count == 0);
    CHECK(r.stats[k15][0][0].count == 0);
    return fails;
}

} // namespace

int test_poses() {
    int fails = 0;
    for (const testdata::SampleCounts& s : testdata::kSamples) fails += check_sample(s);
    fails += test_class17();
    std::printf("test_poses: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
