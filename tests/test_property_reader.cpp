// HNwd::PropertyReader: nodernas flikar och egenskaper per tråd. Med
// TreeContent::Structure lika med Full för varje nod i exempelfilerna, två
// läsare i var sin tråd, och ett dokument som stängs medan en läsare läser.
// Den syntetiska filen prövas i test_tree.cpp, där den byggs.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "HNwd.h"
#include "test_data.h"
#include "test_support.h"

using HNwd::Document;
using HNwd::NodeProperties;
using HNwd::PropertyReader;
using HNwd::Status;
using HNwd::TreeContent;

namespace {

void append_double(std::string& s, double d) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%a|", d);
    s += buf;
}

// Allt i flikarna och egenskaperna som text, så att två läsningar kan jämföras
// också efter att vyerna slutat gälla.
std::string signature(const NodeProperties& p) {
    std::string s;
    size_t k = 0;
    for (const HNwd::Tab& t : p.tabs) {
        s += "T:";
        s += t.name;
        s += '|';
        s += t.internal;
        s += '|' + std::to_string(t.property_count) + '\n';
        for (uint32_t j = 0; j < t.property_count && k < p.properties.size(); ++j, ++k) {
            const HNwd::Property& q = p.properties[k];
            s += " P:";
            s += q.name;
            s += '|';
            s += q.internal;
            s += '|' + std::to_string(q.raw_type) + '|' + std::to_string(static_cast<int>(q.type)) +
                 '|';
            for (const double d : q.d) append_double(s, d);
            s += std::to_string(q.i) + '|';
            s += q.text;
            s += '\n';
        }
    }
    s += "#" + std::to_string(p.properties.size()) + "/" + std::to_string(k);
    return s;
}

// Samma sak ur Document::tab() och property() (bara med Full).
std::string doc_signature(const Document& doc, uint32_t node) {
    NodeProperties p;
    const size_t tabs = doc.tab_count(node);
    for (size_t t = 0; t < tabs; ++t) {
        HNwd::Tab tab;
        if (!doc.tab(node, t, tab)) return "tab missing";
        p.tabs.push_back(tab);
        for (size_t j = 0; j < tab.property_count; ++j) {
            HNwd::Property q;
            if (!doc.property(node, t, j, q)) return "property missing";
            p.properties.push_back(q);
        }
    }
    return signature(p);
}

bool open_tree(Document& doc, const char* path, TreeContent content) {
    if (doc.open(path) != Status::Ok || doc.load_tree(content) != Status::Ok) {
        std::printf("  FAIL — %s: %s\n", path, doc.last_error());
        return false;
    }
    return true;
}

// Noderna 0, 1, … i ordning, eller baklänges med ett steg så att högst
// `max_reads` läses: baklänges packas trädets ström upp från en kontrollpunkt
// för varje nod, och det är dyrt.
std::vector<uint32_t> forward(size_t n) {
    std::vector<uint32_t> out(n);
    for (size_t i = 0; i < n; ++i) out[i] = static_cast<uint32_t>(i);
    return out;
}

std::vector<uint32_t> backward(size_t n, size_t max_reads) {
    const size_t step = n / max_reads + 1;
    std::vector<uint32_t> out;
    for (size_t i = n; i-- > 0;)
        if (i % step == 0 || i == n - 1) out.push_back(static_cast<uint32_t>(i));
    return out;
}

// Läser noderna i `order` och jämför med `expected`. Ger antalet noder som
// skiljer eller inte gick att läsa.
size_t read_all(PropertyReader& reader, const std::vector<std::string>& expected,
                const std::vector<uint32_t>& order, std::atomic<size_t>* progress = nullptr) {
    size_t bad = 0;
    NodeProperties p;
    for (size_t k = 0; k < order.size(); ++k) {
        const uint32_t i = order[k];
        if (reader.read(i, p) != Status::Ok || signature(p) != expected[i]) {
            if (bad++ < 3) std::printf("  FAIL — node %u: %s\n", i, reader.last_error());
        }
        if (progress) progress->store(k + 1);
    }
    return bad;
}

constexpr size_t kBackwardReads = 300;

int test_api() {
    int fails = 0;
    PropertyReader reader;
    NodeProperties p;
    p.tabs.resize(2);
    CHECK(!reader.is_open());
    CHECK(reader.read(0, p) == Status::NotLoaded && p.tabs.empty() && *reader.last_error() != 0);
    Document doc;
    CHECK(reader.open(doc) == Status::NotLoaded && !reader.is_open());

    // Flyttad ifrån: läsaren går att använda igen.
    PropertyReader moved = std::move(reader);
    CHECK(!moved.is_open());
    CHECK(reader.read(0, p) == Status::NotLoaded);   // NOLINT: flyttad ifrån med avsikt
    CHECK(reader.open(doc) == Status::NotLoaded);
    return fails;
}

int test_samples() {
    int fails = 0;
    for (const testdata::SampleCounts& sc : testdata::kSamples) {
        if (!testdata::exists(sc.path)) {
            std::printf("  SKIP — %s saknas\n", sc.path);
            ++g_skipped;
            continue;
        }
        Document full, st;
        if (!open_tree(full, sc.path, TreeContent::Full) ||
            !open_tree(st, sc.path, TreeContent::Structure)) {
            ++fails;
            continue;
        }
        const size_t n = full.node_count();
        CHECK(st.node_count() == n);

        // Ett dokument utan träd går inte att öppna en läsare mot.
        {
            Document bare;
            PropertyReader r;
            CHECK(bare.open(sc.path) == Status::Ok);
            CHECK(r.open(bare) == Status::NotLoaded && !r.is_open());
        }

        PropertyReader rf, rs;
        CHECK(rf.open(full) == Status::Ok && rs.open(st) == Status::Ok);
        CHECK(rf.is_open() && rs.is_open() && *rs.last_error() == 0);

        // Varje nod: Structure lika med Full, och Full lika med Document.
        std::vector<std::string> expected(n);
        size_t bad = 0, tabs = 0, props = 0;
        NodeProperties a, b;
        const auto t0 = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < n; ++i) {
            const Status sa = rf.read(i, a);
            const Status sb = rs.read(i, b);
            expected[i] = signature(a);
            if (sa != Status::Ok || sb != Status::Ok || expected[i] != signature(b) ||
                expected[i] != doc_signature(full, i)) {
                if (bad++ < 3)
                    std::printf("  FAIL — %s: node %u: %s / %s\n", sc.path, i, rf.last_error(),
                                rs.last_error());
            }
            tabs += b.tabs.size();
            props += b.properties.size();
        }
        const double secs =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(bad == 0);
        CHECK(tabs > 0 && props > 0);
        std::printf("  %s: %zu nodes, %zu tabs, %zu properties equal in Full and Structure "
                    "(%.3f s for both)\n",
                    sc.path, n, tabs, props, secs);

        // Roten har inga flikar; ett index utanför ger OutOfRange och tomt.
        CHECK(rs.read(0, b) == Status::Ok && b.tabs.empty() && b.properties.empty());
        CHECK(rs.read(static_cast<uint32_t>(n), b) == Status::OutOfRange && b.tabs.empty());
        CHECK(rf.read(HNwd::kNone, a) == Status::OutOfRange && a.tabs.empty());
        CHECK(*rs.last_error() != 0);

        // Baklänges med samma läsare.
        const std::vector<uint32_t> all = forward(n);
        const std::vector<uint32_t> back = backward(n, kBackwardReads);
        CHECK(read_all(rs, expected, back) == 0);

        // Läsarens tak: ingen cache och en liten ger samma flikar som Full,
        // framlänges och baklänges med samma läsare.
        for (const size_t cache : {size_t(0), size_t(4096)}) {
            PropertyReader rc;
            CHECK(rc.open(st, cache) == Status::Ok);
            CHECK(read_all(rc, expected, all) == 0);
            CHECK(read_all(rc, expected, back) == 0);
            CHECK(read_all(rc, expected, all) == 0);
        }

        // Två läsare i var sin tråd, i var sin riktning.
        {
            PropertyReader r1, r2;
            CHECK(r1.open(st) == Status::Ok && r2.open(st) == Status::Ok);
            size_t bad1 = 0, bad2 = 0;
            std::thread t1([&] { bad1 = read_all(r1, expected, all); });
            std::thread t2([&] { bad2 = read_all(r2, expected, back); });
            t1.join();
            t2.join();
            CHECK(bad1 == 0 && bad2 == 0);
        }

        // Dokumentet stängs medan en läsare läser i en annan tråd, i båda lägena.
        for (const TreeContent c : {TreeContent::Structure, TreeContent::Full}) {
            Document doc;
            if (!open_tree(doc, sc.path, c)) {
                ++fails;
                continue;
            }
            PropertyReader r;
            CHECK(r.open(doc) == Status::Ok);
            std::atomic<size_t> done{0};
            size_t bad_close = 0;
            std::thread t([&] { bad_close = read_all(r, expected, all, &done); });
            while (done.load() < n / 4) std::this_thread::yield();
            doc.close();
            t.join();
            CHECK(!doc.is_open() && bad_close == 0);
            // Läsaren läser vidare också när dokumentet är förstört.
            {
                Document gone;
                CHECK(open_tree(gone, sc.path, c));
                CHECK(r.open(gone) == Status::Ok);
            }
            CHECK(r.read(static_cast<uint32_t>(n - 1), b) == Status::Ok &&
                  signature(b) == expected[n - 1]);
            r.close();
            CHECK(!r.is_open() && r.read(1, b) == Status::NotLoaded);
        }
    }
    return fails;
}

} // namespace

int test_property_reader() {
    int fails = 0;
    fails += test_api();
    fails += test_samples();
    std::printf("test_property_reader: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
