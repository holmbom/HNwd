// Den syntetiska filen (tools/synth_writer.h): skriven till en temporärfil
// genom samma kod som nwdsynth, öppnad och läst med hela det publika API:t.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "../tools/pose_check.h"
#include "../tools/synth_writer.h"
#include "HNwd.h"
#include "mapped_file.h"
#include "test_data.h"
#include "test_support.h"

using HNwd::Document;
using HNwd::Status;

namespace {

// Tar bort filen när den går ur räckvidd. Dokument som öppnat den ska
// deklareras efter den, så att de stängs först.
struct Removed {
    std::string path;
    ~Removed() { testdata::remove_utf8(path); }
};

// Läser allt i filen och prövar det mot det som skrevs.
int check_file(const std::string& path, const synth::Options& o, const synth::Result& r) {
    int fails = 0;
    Document doc;
    CHECK(doc.open(path.c_str()) == Status::Ok);
    if (!doc.is_open()) {
        std::printf("  open: %s\n", doc.last_error());
        return fails;
    }
    HNwd::Unit unit;
    CHECK(doc.length_unit(unit) == Status::Ok);
    CHECK(unit.code == o.unit);

    CHECK(doc.load_fragments() == Status::Ok);
    CHECK(doc.fragment_count() == o.fragments);
    CHECK(doc.record_count() == o.records);

    CHECK(doc.load_spatial() == Status::Ok);
    CHECK(doc.spatial_from_file() == o.spatial);
    CHECK(doc.spatial_node_count() > 0);
    if (const uint32_t* items = doc.spatial_items()) {
        std::vector<uint8_t> seen(doc.fragment_count(), 0);
        bool permutation = true;
        for (size_t i = 0; i < doc.fragment_count(); ++i) {
            if (items[i] >= seen.size() || seen[items[i]]) permutation = false;
            else seen[items[i]] = 1;
        }
        CHECK(permutation);
    } else {
        CHECK(false);
    }

    // Varje fragments geometri, med lådan och antalet prövade av läsaren.
    HNwd::Mesh mesh;
    size_t bad = 0;
    uint64_t primitives = 0;
    for (size_t i = 0; i < doc.fragment_count(); ++i) {
        if (doc.fragment_mesh(i, mesh) != Status::Ok) {
            if (bad++ == 0) std::printf("  fragment %zu: %s\n", i, doc.last_error());
            continue;
        }
        primitives += mesh.indices.size() / (mesh.kind == HNwd::MeshKind::Lines ? 2 : 3);
    }
    CHECK(bad == 0);
    CHECK(primitives > 0);

    // Trädet i båda lägena: Structure först, sedan Full med flikarna.
    CHECK(doc.load_tree(HNwd::TreeContent::Structure) == Status::Ok);
    CHECK(doc.node_count() == o.nodes + 1);
    CHECK(!doc.has_tabs());
    CHECK(doc.guid_count() == r.leaves);
    HNwd::NodeProperties props;
    {
        HNwd::PropertyReader reader;
        CHECK(reader.open(doc) == Status::Ok);
        HNwd::Fragment f;
        CHECK(doc.fragment(0, f));
        CHECK(reader.read(f.seq, props) == Status::Ok);
        // Lövet har fliken i trädet, och det första lövet också en rot.
        CHECK(props.tabs.size() == 2);
    }
    CHECK(doc.load_tree(HNwd::TreeContent::Full) == Status::Ok);
    CHECK(doc.has_tabs());
    HNwd::Fragment f;
    CHECK(doc.fragment(0, f));
    CHECK(doc.tab_count(f.seq) == 2);
    HNwd::Node node;
    CHECK(doc.node(f.seq, node));
    CHECK(node.kind == HNwd::NodeKind::Leaf);
    CHECK(doc.model_count() == 1);

    // Poserna följer trädets transformer.
    HNwd::detail::MappedFile file;
    CHECK(file.open(path.c_str()) == HNwd::detail::ErrorCode::Ok);
    posecheck::Input in;
    std::string why;
    CHECK(posecheck::load(file.data(), file.size(), in, why) == HNwd::detail::ErrorCode::Ok);
    posecheck::Report report;
    posecheck::check(in, report);
    CHECK(report.unmatched == 0);
    CHECK(report.fragments == o.fragments);
    for (int slot = 0; slot < posecheck::kClassCount; ++slot)
        CHECK(report.stats[slot][0][0].count == r.poses[slot]);
    return fails;
}

// Postens form som den skrevs innan storlekarna och segmentantalet fanns:
// facit för att förvalen ger samma poster.
void reference_shape(uint64_t seed, uint64_t r, double per_metre, bool& lines, uint32_t& primitives,
                     std::vector<float>& positions) {
    const uint64_t h = synth::hash(seed, 1, r);
    lines = (h % 100) < 15;
    const uint32_t segments = 1 + static_cast<uint32_t>((h >> 8) % 12);
    const double length = (0.5 + 4.5 * synth::unit_random(synth::hash(seed, 2, r))) * per_metre;
    const double width = (0.1 + 0.9 * synth::unit_random(synth::hash(seed, 3, r))) * per_metre;
    positions.clear();
    if (lines) {
        primitives = segments;
        for (uint32_t i = 0; i <= segments; ++i) {
            positions.push_back(static_cast<float>(-length / 2 + length * i / segments));
            positions.push_back(static_cast<float>(width / 2 * std::sin(static_cast<double>(i))));
            positions.push_back(0.0f);
        }
    } else {
        primitives = 2 * segments;
        for (uint32_t i = 0; i <= segments; ++i) {
            const float x = static_cast<float>(-length / 2 + length * i / segments);
            const float z = static_cast<float>(0.05 * per_metre * std::sin(static_cast<double>(i)));
            for (const double y : {-width / 2, width / 2}) {
                positions.push_back(x);
                positions.push_back(static_cast<float>(y));
                positions.push_back(z);
            }
        }
    }
}

// Diagonalen på en låda (min xyz, max xyz) i meter, när längderna är i enheter
// om `per_metre` per meter.
double box_diagonal(const float lo[3], const float hi[3], double per_metre) {
    double sum = 0;
    for (int k = 0; k < 3; ++k) {
        const double d = static_cast<double>(hi[k]) - static_cast<double>(lo[k]);
        sum += d * d;
    }
    return std::sqrt(sum) / per_metre;
}

// Antalet segment som post r ska ha med högst `max_segments` ur fröet.
uint32_t expected_segments(uint64_t seed, uint64_t r, uint32_t max_segments) {
    return 1 + static_cast<uint32_t>((synth::hash(seed, 1, r) >> 8) % max_segments);
}

// Förvalen: varje post är den som skrevs förut.
int check_default_shapes() {
    int fails = 0;
    synth::Options o;
    o.seed = 7;
    CHECK(o.min_size == 0 && o.max_size == 0 && o.max_segments == 12);
    synth::RecordShape shape;
    size_t differing = 0;
    for (const double per_metre : {1.0, 1000.0}) {
        for (uint64_t r = 1; r <= 3000; ++r) {
            bool lines = false;
            uint32_t primitives = 0;
            std::vector<float> positions;
            reference_shape(o.seed, r, per_metre, lines, primitives, positions);
            synth::record_shape(o, r, per_metre, shape);
            if (shape.lines != lines || shape.primitives != primitives || shape.positions != positions)
                ++differing;
        }
    }
    CHECK(differing == 0);
    return fails;
}

// Storlekarna och segmentantalet: varje posts diagonal ligger inom intervallet
// och fördelar sig över det, varje post har så många segment som fröet ger, och
// filen med dem går att läsa helt.
int check_sizes_and_detail() {
    int fails = 0;
    synth::Options o;
    o.fragments = 1500;
    o.records = 500;
    o.nodes = 1200;
    o.seed = 3;
    o.min_size = 0.02;
    o.max_size = 40;
    o.max_segments = 100;
    constexpr double kPerMetre = 1000;   // enhetskod 2: millimeter

    // Formerna direkt: diagonalen är exakt den dragna storleken, och segmenten
    // följer fröet.
    {
        synth::RecordShape shape;
        double smallest = 1e300, largest = 0;
        uint32_t most_segments = 0;
        size_t outside = 0, wrong_count = 0;
        for (uint64_t r = 1; r <= o.records; ++r) {
            synth::record_shape(o, r, kPerMetre, shape);
            const double d = box_diagonal(shape.bbox, shape.bbox + 3, kPerMetre);
            if (!(d >= o.min_size * (1 - 1e-5) && d <= o.max_size * (1 + 1e-5))) ++outside;
            smallest = std::min(smallest, d);
            largest = std::max(largest, d);
            const uint32_t segments = expected_segments(o.seed, r, o.max_segments);
            if (shape.primitives != (shape.lines ? segments : 2 * segments)) ++wrong_count;
            most_segments = std::max(most_segments, segments);
        }
        CHECK(outside == 0);
        CHECK(wrong_count == 0);
        // Log-likformigt: båda ändarna av intervallet används, knappt en
        // tredjedel av det i logaritmen vid var ände.
        CHECK(smallest < 0.2);
        CHECK(largest > 4);
        CHECK(most_segments > 12);
    }

    // Filen: varje fragments postlåda ligger inom intervallet, antalet
    // primitiver når över förvalets tak och allt går att läsa (check_file
    // avkodar varje post och prövar dess låda och antal mot fragmentet).
    {
        Removed file{testdata::tmp_path("hnwd_test_synth_sizes.nwd")};
        synth::Result r;
        std::string why;
        CHECK(synth::write(file.path, o, r, why));
        if (!why.empty()) std::printf("  write: %s\n", why.c_str());
        fails += check_file(file.path, o, r);

        Document doc;
        CHECK(doc.open(file.path.c_str()) == Status::Ok);
        CHECK(doc.load_fragments() == Status::Ok);
        size_t outside = 0;
        uint32_t most_primitives = 0;
        for (size_t i = 0; i < doc.fragment_count(); ++i) {
            HNwd::Fragment f;
            CHECK(doc.fragment(i, f));
            const double d = box_diagonal(f.bounds.min, f.bounds.max, kPerMetre);
            if (!(d >= o.min_size * (1 - 1e-5) && d <= o.max_size * (1 + 1e-5))) ++outside;
            most_primitives = std::max(most_primitives, f.primitives);
            CHECK(f.primitives <= 2 * o.max_segments);
        }
        CHECK(outside == 0);
        CHECK(most_primitives > 24);
    }

    // Ett segment per post: en linje eller två trianglar.
    {
        synth::Options one = o;
        one.max_segments = 1;
        synth::RecordShape shape;
        size_t wrong = 0;
        for (uint64_t r = 1; r <= one.records; ++r) {
            synth::record_shape(one, r, kPerMetre, shape);
            if (shape.primitives != (shape.lines ? 1u : 2u)) ++wrong;
        }
        CHECK(wrong == 0);
    }
    return fails;
}

} // namespace

int test_synth() {
    int fails = 0;
    synth::Options o;
    o.fragments = 2000;
    o.records = 700;
    o.nodes = 1500;
    o.seed = 7;

    // Med det rumsliga trädet, i millimeter.
    {
        Removed file{testdata::tmp_path("hnwd_test_synth.nwd")};
        synth::Result r;
        std::string why;
        CHECK(synth::write(file.path, o, r, why));
        if (!why.empty()) std::printf("  write: %s\n", why.c_str());
        // Varje poseklass förekommer, och alla fragment hamnade i ett löv.
        for (const uint64_t n : r.poses) CHECK(n > 0);
        CHECK(r.poses[0] + r.poses[1] + r.poses[2] + r.poses[3] + r.poses[4] == o.fragments);
        CHECK(r.chunks >= 2);
        CHECK(r.leaves + r.groups == o.nodes);
        fails += check_file(file.path, o, r);
    }
    // Utan det rumsliga trädet, i meter: läsaren bygger ett eget index.
    {
        o.spatial = false;
        o.unit = 0;
        Removed file{testdata::tmp_path("hnwd_test_synth_own.nwd")};
        synth::Result r;
        std::string why;
        CHECK(synth::write(file.path, o, r, why));
        fails += check_file(file.path, o, r);
    }
    // Samma frö ger samma fil.
    {
        o.spatial = true;
        o.unit = 2;
        Removed a{testdata::tmp_path("hnwd_test_synth_a.nwd")}, b{testdata::tmp_path("hnwd_test_synth_b.nwd")};
        synth::Result ra, rb;
        std::string why;
        CHECK(synth::write(a.path, o, ra, why));
        CHECK(synth::write(b.path, o, rb, why));
        HNwd::detail::MappedFile fa, fb;
        CHECK(fa.open(a.path.c_str()) == HNwd::detail::ErrorCode::Ok);
        CHECK(fb.open(b.path.c_str()) == HNwd::detail::ErrorCode::Ok);
        CHECK(fa.size() == fb.size() && fa.size() == ra.file_bytes);
        CHECK(fa.is_open() && fb.is_open() && fa.size() == fb.size() && std::memcmp(fa.data(), fb.data(), fa.size()) == 0);
    }
    // Fel val ger ett fel och ingen fil.
    {
        synth::Options bad = o;
        bad.unit = 4;
        synth::Result r;
        std::string why;
        CHECK(!synth::write(testdata::tmp_path("hnwd_test_synth_bad.nwd"), bad, r, why));
        CHECK(!why.empty());
        CHECK(!testdata::exists(testdata::tmp_path("hnwd_test_synth_bad.nwd").c_str()));
    }
    // Förvalens poster, och storlekarna och segmentantalet.
    fails += check_default_shapes();
    fails += check_sizes_and_detail();
    // Ogiltiga storlekar och segmentantal ger ett fel och ingen fil.
    {
        struct Bad {
            double min_size, max_size;
            uint32_t max_segments;
        };
        const Bad cases[] = {{5, 1, 12},   // omvänt intervall
                             {0, 5, 12},   // bara den ena änden
                             {1, 0, 12},
                             {-1, 5, 12},
                             {0.5, std::numeric_limits<double>::infinity(), 12},   // inte ändligt
                             {0, 0, 0},    // inget segment
                             {0, 0, synth::kMaxSegments + 1}};
        for (const Bad& c : cases) {
            synth::Options bad = o;
            bad.min_size = c.min_size;
            bad.max_size = c.max_size;
            bad.max_segments = c.max_segments;
            synth::Result r;
            std::string why;
            const std::string path = testdata::tmp_path("hnwd_test_synth_bad.nwd");
            CHECK(!synth::write(path, bad, r, why));
            CHECK(!why.empty());
            CHECK(!testdata::exists(path.c_str()));
        }
    }
    std::printf("test_synth: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
