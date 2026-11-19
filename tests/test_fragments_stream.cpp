// Fragmenten tolkade direkt ur zlib-strömmen, genom ett fönster: samma lager
// som ur den uppackade bufferten, avbrott genom förloppet, och strömmens fel.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "container.h"
#include "fragments.h"
#include "inflate.h"
#include "mapped_file.h"
#include "test_data.h"
#include "test_support.h"

using namespace HNwd::detail;
using testdata::Bytes;
using testdata::ByteWriter;

namespace {

using Progress = std::function<bool(uint64_t, uint64_t)>;

// Ett material med en inbäddad fil på `bytes` byte.
void class185(ByteWriter& w, size_t bytes) {
    w.str("{\"version\":2}");
    w.str("");
    w.i32(1);
    w.str("bild.png");
    w.i32(1);
    w.str("navis");
    w.str("png");
    w.i64a(static_cast<int64_t>(bytes));
    for (size_t i = 0; i < bytes; ++i) w.u8(static_cast<uint8_t>(i * 13));
    w.pad(4);
}

// `count` fragment med alla poseklasser, delade och egna utseenden och
// geometrier, och ett material vars fil är `image` byte.
Bytes stream_bytes(int count, size_t image) {
    ByteWriter w;
    w.i32(count);
    int32_t app = 0, geom = 0;
    for (int i = 0; i < count; ++i) {
        w.obj(13);
        w.f64(0.25 * i);
        w.i32(i % 5 + 1);
        w.i32(i);
        w.i32(-i);
        w.i32(i / 3);
        const int cls = (i % 5 == 4) ? 0 : 14 + i % 5;   // 14, 15, 16, 17, null
        if (cls == 0) {
            w.null_ptr();
        } else {
            w.obj(cls);
            if (cls == 17) {
                for (int c = 0; c < 3; ++c) {
                    for (int r = 0; r < 3; ++r) w.f32(r == c ? 1.0f + c : 0.0f);
                    w.f32(0);
                }
                w.f32(1);
            }
            for (int k = 0; k < 3; ++k) w.f64(i * 10.0 + k);
            if (cls == 15 || cls == 16) {   // en kvaternion med längden 1
                const double x = 0.1 * (i % 7);
                w.f64(x);
                w.f64(0);
                w.f64(0);
                w.f64(std::sqrt(1 - x * x));
            }
            if (cls == 16) w.f64(1.0 + i % 3);
        }
        // Var sjunde fragment får ett eget utseende; det första har materialet.
        if (app == 0 || i % 7 == 0) {
            app = w.obj(55);
            w.i32(app == 102 ? 33 : 1);
            w.obj(54);
            for (int k = 0; k < 14; ++k) w.f32(static_cast<float>((i + k) % 4) * 0.25f);
            if (app == 102) {
                w.obj(185);
                class185(w, image);
            }
        } else {
            w.ref(app);
        }
        // Var elfte fragment får en egen geometri.
        if (geom == 0 || i % 11 == 0) {
            geom = w.obj(93);
            for (int k = 0; k < 6; ++k) w.f32(k < 3 ? -1.0f - i : 1.0f + i);
            for (int k = 0; k < 3; ++k) w.f64(i + 0.5 * k);
            w.f32(1.5f);
            w.i32(i);
            w.i32(1 + i % 9);
        } else {
            w.ref(geom);
        }
    }
    w.pad(8);
    return w.data;
}

bool same_store(const FragmentStore& a, const FragmentStore& b) {
    if (a.size() != b.size() || a.geomrefs.size() != b.geomrefs.size() ||
        a.looks.size() != b.looks.size() || a.materials.size() != b.materials.size() ||
        a.pose_blocks.size() != b.pose_blocks.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const FragmentStore::Item& x = a.items[i];
        const FragmentStore::Item& y = b.items[i];
        if (x.geomref != y.geomref || x.look != y.look || x.pose != y.pose || x.seq != y.seq ||
            x.primitives != y.primitives || x.pose_class != y.pose_class)
            return false;
        const double* p = a.pose_values(i);
        const double* q = b.pose_values(i);
        if (std::memcmp(p, q, FragmentStore::pose_size(x.pose_class) * sizeof(double)) != 0)
            return false;
    }
    for (size_t i = 0; i < a.geomrefs.size(); ++i)
        if (std::memcmp(&a.geomrefs[i], &b.geomrefs[i], sizeof(GeometryRefData)) != 0) return false;
    for (size_t i = 0; i < a.looks.size(); ++i)
        if (std::memcmp(a.looks[i].v, b.looks[i].v, sizeof a.looks[i].v) != 0 ||
            a.looks[i].material != b.looks[i].material)
            return false;
    for (size_t i = 0; i < a.materials.size(); ++i) {
        const MaterialData& x = a.materials[i];
        const MaterialData& y = b.materials[i];
        if (x.json != y.json || x.files.size() != y.files.size()) return false;
        for (size_t k = 0; k < x.files.size(); ++k)
            if (x.files[k].name != y.files[k].name || x.files[k].bytes != y.files[k].bytes)
                return false;
    }
    return true;
}

bool same_extra(const std::vector<FragmentExtra>& a, const std::vector<FragmentExtra>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].d != b[i].d || a[i].b != b[i].b || a[i].c != b[i].c || a[i].key != b[i].key ||
            a[i].appearance_flags != b[i].appearance_flags || a[i].f != b[i].f ||
            std::memcmp(a[i].center, b[i].center, sizeof a[i].center) != 0)
            return false;
    return true;
}

// Tolkar den komprimerade strömmen `z` genom ett fönster om `window` byte.
ErrorCode parse_z(const Bytes& z, FragmentStore& out, const Progress& progress,
                  std::vector<FragmentExtra>* extra, std::string* detail, size_t window,
                  InflateStream* used = nullptr) {
    InflateStream local;
    InflateStream& s = used ? *used : local;
    const ErrorCode e = s.open(z.data(), z.size(), kDefaultMaxStreamBytes);
    if (e != ErrorCode::Ok) return e;
    return parse_fragments(s, out, progress, extra, detail, window);
}

int test_synthetic() {
    int fails = 0;
    constexpr size_t kWindow = 64 * 1024;
    // Fler fragment än fönstret rymmer, och en bild som är större än det.
    const Bytes raw = stream_bytes(3000, 150000);
    CHECK(raw.size() > 4 * kWindow);
    const Bytes z = testdata::zlib_store(raw, 4093);

    FragmentStore a, b;
    std::vector<FragmentExtra> ea, eb;
    std::string da, db;
    CHECK(parse_fragments(raw.data(), raw.size(), a, &ea, &da) == ErrorCode::Ok);
    if (!da.empty()) std::printf("  %s\n", da.c_str());
    InflateStream s;
    CHECK(parse_z(z, b, {}, &eb, &db, kWindow, &s) == ErrorCode::Ok);
    if (!db.empty()) std::printf("  %s\n", db.c_str());
    CHECK(s.finished() && s.consumed() == z.size());
    CHECK(a.size() == 3000 && same_store(a, b) && same_extra(ea, eb));
    CHECK(b.materials.size() == 1 && b.materials[0].files.size() == 1 &&
          b.materials[0].files[0].bytes.size() == 150000);

    // Förloppet: först 0, sist alla, aldrig bakåt, och totalen är antalet.
    {
        std::vector<uint64_t> seen;
        bool total_ok = true;
        const Progress record = [&](uint64_t done, uint64_t total) {
            seen.push_back(done);
            total_ok = total_ok && total == 3000;
            return true;
        };
        FragmentStore c;
        CHECK(parse_z(z, c, record, nullptr, nullptr, kWindow) == ErrorCode::Ok);
        CHECK(total_ok && !seen.empty() && seen.front() == 0 && seen.back() == 3000);
        bool rising = true;
        for (size_t i = 1; i < seen.size(); ++i) rising = rising && seen[i] > seen[i - 1];
        CHECK(rising);
    }

    // Avbrott: före det första, och mitt i.
    for (const int stop_at : {1, 2}) {
        int calls = 0;
        const Progress stop = [&](uint64_t, uint64_t) { return ++calls < stop_at; };
        FragmentStore c;
        std::vector<FragmentExtra> ec;
        std::string d;
        const Bytes many = testdata::zlib_store(stream_bytes(40000, 10));
        CHECK(parse_z(many, c, stop, &ec, &d, kWindow) == ErrorCode::Cancelled);
        CHECK(calls == stop_at);
        CHECK(c.size() == 0 && c.pose_blocks.empty() && c.looks.empty() && ec.empty());
        CHECK(d.find("LcOpNwdFragments: cancelled after ") == 0);
        if (stop_at == 2) CHECK(d.find("after 16384 of 40000") != std::string::npos);
    }

    // Samma fel och samma text som ur bufferten för en trasig objektström.
    {
        Bytes bad = raw;
        bad[4 + 4] = 99;   // det första fragmentets klass
        FragmentStore c, d;
        std::string ta, tb;
        const ErrorCode e1 = parse_fragments(bad.data(), bad.size(), c, nullptr, &ta);
        const ErrorCode e2 = parse_z(testdata::zlib_store(bad), d, {}, nullptr, &tb, kWindow);
        CHECK(e1 == ErrorCode::CorruptObject && e2 == e1 && ta == tb && !ta.empty());
        CHECK(d.size() == 0);
    }

    // Ett antal som inte ryms: strömmen tar slut, och inget allokeras efter talet.
    {
        Bytes big = stream_bytes(2, 10);
        big[0] = 0xFF;
        big[1] = 0xFF;
        big[2] = 0xFF;
        big[3] = 0x7F;
        FragmentStore c;
        std::string d;
        CHECK(parse_z(testdata::zlib_store(big), c, {}, nullptr, &d, kWindow) ==
              ErrorCode::CorruptObject);
        CHECK(c.size() == 0 && c.items.capacity() == 0);
    }

    // En trasig zlib-ström: kontrollsumman fel, och strömmen avhuggen.
    {
        Bytes wrong = z;
        wrong.back() ^= 1;
        FragmentStore c;
        std::string d;
        CHECK(parse_z(wrong, c, {}, nullptr, &d, kWindow) == ErrorCode::CorruptStream);
        CHECK(d.find("LcOpNwdFragments at byte ") == 0 && c.size() == 0);
        const Bytes cut(z.begin(), z.begin() + static_cast<std::ptrdiff_t>(z.size() / 2));
        CHECK(parse_z(cut, c, {}, nullptr, &d, kWindow) == ErrorCode::CorruptStream);
        CHECK(c.size() == 0);
    }
    return fails;
}

// Ur exempelfilen: strömmens lager är buffertens.
int test_sample(const char* path) {
    SKIP_IF_MISSING(path);
    int fails = 0;
    MappedFile file;
    CHECK(file.open(path) == ErrorCode::Ok);
    Container c;
    CHECK(read_container(file.data(), file.size(), c) == ErrorCode::Ok);
    const DbCacheEntry* e = find_entry(c.entries, "LcOpNwdFragments");
    CHECK(e != nullptr);
    if (!e) return fails;
    StreamSpan span;
    CHECK(last_stream(*e, file.size(), span) == ErrorCode::Ok);
    std::vector<uint8_t> bytes;
    CHECK(inflate_span(file.data(), file.size(), span, kDefaultMaxStreamBytes, bytes) ==
          ErrorCode::Ok);

    FragmentStore a, b;
    std::vector<FragmentExtra> ea, eb;
    CHECK(parse_fragments(bytes.data(), bytes.size(), a, &ea) == ErrorCode::Ok);
    InflateStream s;
    CHECK(s.open(file.data() + span.offset, static_cast<size_t>(span.length),
                 kDefaultMaxStreamBytes) == ErrorCode::Ok);
    std::string detail;
    CHECK(parse_fragments(s, b, {}, &eb, &detail, 64 * 1024) == ErrorCode::Ok);
    if (!detail.empty()) std::printf("  %s: %s\n", path, detail.c_str());
    CHECK(s.finished() && s.consumed() == span.length);
    const bool same = same_store(a, b) && same_extra(ea, eb);
    CHECK(same);
    if (!same) std::printf("  %s: the streamed store differs from the buffer's\n", path);
    return fails;
}

} // namespace

int test_fragments_stream() {
    int fails = test_synthetic();
    for (const testdata::SampleCounts& s : testdata::kSamples) fails += test_sample(s.path);
    std::printf("test_fragments_stream: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
