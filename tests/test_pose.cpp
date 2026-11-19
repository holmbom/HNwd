// Posens matematik (pose.h) och fragmentlagrets pool av posetal.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "fragments.h"
#include "pose.h"
#include "test_support.h"

using namespace HNwd::detail;
using testdata::Bytes;
using testdata::ByteWriter;

namespace {

bool near(double a, double b, double eps = 1e-12) { return std::fabs(a - b) <= eps; }

// Ett fragment med en ny pose av klassen `cls` (0: null) och ett nytt
// utseende och en ny geometri, eller bakåtpekare till de första.
void fragment(ByteWriter& w, int cls, int32_t seq, double base, int32_t& app, int32_t& geom) {
    w.obj(13);
    w.f64(1.0);
    w.i32(2);
    w.i32(0);
    w.i32(0);
    w.i32(seq);
    if (cls == 0) {
        w.null_ptr();
    } else {
        w.obj(cls);
        if (cls == 17) {
            // Kolumnerna (2, 0, 0), (0, 3, 0), (0, 0, -4), var och en följd av 0.
            const float cols[3][3] = {{2, 0, 0}, {0, 3, 0}, {0, 0, -4}};
            for (const auto& c : cols) {
                for (const float v : c) w.f32(v);
                w.f32(0);
            }
            w.f32(1);
        }
        w.f64(base);
        w.f64(base + 1);
        w.f64(base + 2);
        if (cls == 15 || cls == 16) {
            w.f64(0);
            w.f64(0);
            w.f64(0.6);
            w.f64(0.8);
        }
        if (cls == 16) w.f64(-2);
    }
    if (app == 0) {
        app = w.obj(55);
        w.i32(1);
        w.obj(54);
        for (int i = 0; i < 14; ++i) w.f32(0.5f);
    } else {
        w.ref(app);
    }
    if (geom == 0) {
        geom = w.obj(93);
        const float box[6] = {-1, -2, -3, 1, 2, 3};
        for (const float v : box) w.f32(v);
        w.f64(7);
        w.f64(8);
        w.f64(9);
        w.f32(0);
        w.i32(0);
        w.i32(1);
    } else {
        w.ref(geom);
    }
}

// Lådan rymmer varje hörn som pose_point ger, och varje sida är ett hörns.
int check_box(const PoseD& p, const float box[6]) {
    int fails = 0;
    double lo[3], hi[3];
    pose_box(p, box, lo, hi);
    bool touches_lo[3] = {}, touches_hi[3] = {};
    for (int corner = 0; corner < 8; ++corner) {
        const float v[3] = {box[(corner & 1) ? 3 : 0], box[(corner & 2) ? 4 : 1],
                            box[(corner & 4) ? 5 : 2]};
        double w[3];
        pose_point(p, v, w);
        for (int k = 0; k < 3; ++k) {
            CHECK(w[k] >= lo[k] && w[k] <= hi[k]);
            touches_lo[k] = touches_lo[k] || w[k] == lo[k];
            touches_hi[k] = touches_hi[k] || w[k] == hi[k];
        }
    }
    for (int k = 0; k < 3; ++k) CHECK(touches_lo[k] && touches_hi[k]);
    return fails;
}

} // namespace

int test_pose() {
    int fails = 0;

    // Rotation 90 grader kring Z med skalan 2: x → 2y.
    {
        PoseD p;
        p.q[2] = 1;   // inte normerad: (0, 0, 1, 1) normeras
        p.q[3] = 1;
        p.s = 2;
        p.t[0] = 10;
        const float v[3] = {1, 0, 0};
        double w[3];
        pose_point(p, v, w);
        CHECK(near(w[0], 10) && near(w[1], 2) && near(w[2], 0));
        double m[9];
        pose_linear(p, m);
        CHECK(near(m[1], -2) && near(m[3], 2) && near(m[8], 2));
        CHECK(!pose_mirrors(p));
        p.s = -1;
        CHECK(pose_mirrors(p));

        // Normalen följer rotationen och har längden 1 också med skalan.
        p.s = 3;
        const float n[3] = {1, 0, 0};
        float out[3];
        pose_normal(p, n, out);
        CHECK(near(out[0], 0, 1e-6) && near(out[1], 1, 1e-6) && near(out[2], 0, 1e-6));
    }

    // En matris: olika skala per axel, speglad i z, och normalen vinkelrät mot ytan.
    {
        PoseD p;
        p.affine = true;
        const double m[9] = {2, 0, 0, 0, 3, 0, 0, 0, -4};
        for (int i = 0; i < 9; ++i) p.m[i] = m[i];
        p.t[2] = 1;
        const float v[3] = {1, 1, 1};
        double w[3];
        pose_point(p, v, w);
        CHECK(w[0] == 2 && w[1] == 3 && w[2] == -3);
        CHECK(pose_mirrors(p));
        const float n[3] = {0, 0, 1};
        float out[3];
        pose_normal(p, n, out);
        // Speglad i z: ytan z = 0 vänds, och normalen pekar åt samma håll från
        // ytan som förut, alltså mot −z.
        CHECK(near(out[2], -1, 1e-6) && near(out[0], 0, 1e-6));
    }

    // En kvaternion med längden 0 ger ingen rotation.
    {
        PoseD p;
        p.q[3] = 0;
        double m[9];
        pose_linear(p, m);
        CHECK(m[0] == 1 && m[4] == 1 && m[8] == 1 && m[1] == 0);
    }

    // Världslådan mot de åtta hörnen, för en rad poser.
    {
        const float box[6] = {-1.5f, 0.25f, -3, 2, 4.75f, 0.5f};
        PoseD p;
        fails += check_box(p, box);
        p.t[0] = 1e6;
        p.t[1] = -2.5e6;
        p.q[0] = 0.1;
        p.q[1] = 0.7;
        p.q[2] = -0.3;
        p.q[3] = 0.6;
        fails += check_box(p, box);
        p.s = -1000;
        fails += check_box(p, box);
        PoseD a;
        a.affine = true;
        const double m[9] = {1, 1, 0, 0, 2, -1, 0.5, 0, 3};
        for (int i = 0; i < 9; ++i) a.m[i] = m[i];
        a.t[2] = 12;
        fails += check_box(a, box);

        // Utan rotation är lådan postens låda flyttad.
        PoseD plain;
        plain.t[0] = 5;
        double lo[3], hi[3];
        pose_box(plain, box, lo, hi);
        CHECK(lo[0] == 3.5 && hi[0] == 7 && lo[1] == 0.25 && hi[2] == 0.5);
    }

    // Poolen: varje klass tar sina tal, i ordningen i fragments.h.
    {
        CHECK(FragmentStore::pose_size(0) == 3 && FragmentStore::pose_size(14) == 3 &&
              FragmentStore::pose_size(15) == 7 && FragmentStore::pose_size(16) == 8 &&
              FragmentStore::pose_size(17) == 12);
        CHECK(sizeof(FragmentStore::Item) == 24);

        const int classes[] = {14, 15, 16, 17, 0, 16};
        ByteWriter w;
        w.i32(6);
        int32_t app = 0, geom = 0;
        for (int i = 0; i < 6; ++i) fragment(w, classes[i], i, 100.0 * (i + 1), app, geom);
        w.pad(8);

        FragmentStore st;
        std::string detail;
        CHECK(parse_fragments(w.data.data(), w.data.size(), st, nullptr, &detail) == ErrorCode::Ok);
        if (!detail.empty()) std::printf("  %s\n", detail.c_str());
        CHECK(st.size() == 6 && st.geomrefs.size() == 1 && st.looks.size() == 1);
        CHECK(st.pose_blocks.size() == 1);
        if (st.size() == 6 && st.pose_blocks.size() == 1) {
            // Talen ligger efter varandra: 3 + 7 + 8 + 12 + 3 + 8.
            const uint32_t starts[] = {0, 3, 10, 18, 30, 33};
            for (int i = 0; i < 6; ++i) CHECK(st.items[i].pose == starts[i]);
            CHECK(st.pose_blocks[0].size() == 41);

            PoseD p;
            st.pose(0, p);   // 14: bara flytten
            CHECK(p.t[0] == 100 && p.t[2] == 102 && p.q[3] == 1 && p.s == 1 && !p.affine);
            st.pose(1, p);   // 15: flytten och q
            CHECK(p.t[0] == 200 && p.q[2] == 0.6 && p.q[3] == 0.8 && p.s == 1);
            CHECK(st.pose_values(1)[5] == 0.6);
            st.pose(2, p);   // 16: också s
            CHECK(p.t[1] == 301 && p.q[3] == 0.8 && p.s == -2);
            CHECK(st.pose_values(2)[7] == -2);
            st.pose(3, p);   // 17: matrisen radvis, sedan flytten
            CHECK(p.affine && p.m[0] == 2 && p.m[4] == 3 && p.m[8] == -4 && p.m[1] == 0);
            CHECK(p.t[0] == 400 && p.t[2] == 402);
            CHECK(st.pose_values(3)[9] == 400);
            st.pose(4, p);   // null: geometrins mittpunkt
            CHECK(st.items[4].pose_class == 0);
            CHECK(p.t[0] == 7 && p.t[1] == 8 && p.t[2] == 9 && p.q[3] == 1 && p.s == 1);
            st.pose(5, p);
            CHECK(p.t[0] == 600 && p.s == -2);

            for (int i = 0; i < 6; ++i) {
                CHECK(st.items[i].geomref == 0 && st.items[i].look == 0);
                CHECK(st.items[i].seq == i && st.items[i].primitives == 2);
            }
            CHECK(st.geomrefs[0].record == 1 && st.geomrefs[0].bbox[5] == 3);
        }
        // Minnet: minst lagrets delar, och inte mer än ett block i poolen.
        const size_t floor = st.items.capacity() * sizeof(FragmentStore::Item) +
                             st.pose_blocks[0].capacity() * sizeof(double);
        CHECK(st.memory_bytes() >= floor && st.memory_bytes() < floor + 4096);
    }

    // Poolen över flera block: ingen pose delas av två block.
    {
        const int count = 20000;   // 20000 × 8 tal är mer än två block
        ByteWriter w;
        w.i32(count);
        int32_t app = 0, geom = 0;
        for (int i = 0; i < count; ++i) fragment(w, 16, i, i, app, geom);
        w.pad(8);
        FragmentStore st;
        CHECK(parse_fragments(w.data.data(), w.data.size(), st) == ErrorCode::Ok);
        CHECK(st.size() == static_cast<size_t>(count));
        CHECK(st.pose_blocks.size() == 3);
        bool all = st.size() == static_cast<size_t>(count);
        for (size_t i = 0; all && i < st.size(); ++i) {
            const uint32_t at = st.items[i].pose;
            all = at % FragmentStore::kPoseBlock + 8 <= FragmentStore::kPoseBlock;
            PoseD p;
            st.pose(i, p);
            all = all && p.t[0] == static_cast<double>(i) && p.s == -2;
        }
        CHECK(all);
        // Det sista blocket är krympt till det som används.
        CHECK(st.pose_blocks.back().capacity() == st.pose_blocks.back().size());
    }

    std::printf("test_pose: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
