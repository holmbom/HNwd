#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "byte_reader.h"
#include "container.h"
#include "inflate.h"
#include "mapped_file.h"
#include "object_reader.h"
#include "test_data.h"
#include "test_support.h"

using namespace HNwd::detail;
using testdata::Bytes;
using testdata::ByteWriter;

namespace {

using Ptr = ObjectReader::Ptr;

ObjectReader make(const ByteWriter& w, const char* what = "LcOpTest") {
    return ObjectReader(w.data.data(), w.data.size(), what);
}

// Läser strömmen från read_all_stream med alla anrop och avslutar med end().
// Varje avhuggen längd ska ge ett fel.
bool read_all(const Bytes& data) {
    ObjectReader r(data.data(), data.size(), "LcOpTest");
    Ptr p;
    if (!r.ptr(p, {13}, false, false)) return false;
    int32_t a, n;
    double d;
    std::string s;
    if (!r.i32(a) || !r.f64(d) || !r.str(s) || !r.count(4, n)) return false;
    for (int32_t i = 0; i < n; ++i)
        if (!r.i32(a)) return false;
    if (!r.skip_padded(5)) return false;
    if (!r.finish(p.id, 42)) return false;
    Ptr q;
    if (!r.ptr(q, {13}, false, true)) return false;
    if (q.kind != Ptr::Back || q.value != 42) return false;
    return r.end();
}

Bytes read_all_stream() {
    ByteWriter w;
    w.obj(13);
    w.i32(7);
    w.f64(2.5);
    w.str("abc");
    w.i32(2);
    w.i32(11);
    w.i32(12);
    w.hex("0102 0304 05");
    w.pad(4);
    w.ref(100);
    w.pad(8);
    return w.data;
}

// Ett class-185-objekt: två strängar och två poster, den andra med en inbäddad fil.
Bytes class185_stream(int32_t flag = 1, int64_t len = 5) {
    ByteWriter w;
    w.str("name");
    w.str("json");
    w.i32(2);
    w.str("a");
    w.i32(0);
    w.str("b");
    w.str("c");
    w.i32(flag);
    w.str("d");
    if (flag == 1) {
        w.str("file");
        w.i64a(len);
        w.hex("0102030405");
        w.pad(4);
    }
    w.pad(8);
    return w.data;
}

bool read_185(const Bytes& data) {
    ObjectReader r(data.data(), data.size(), "LcOpTest");
    return skip_class185(r) && r.end();
}

// ---------------------------------------------------------------------------
// Genom ett fönster.

// En läsning som skriver det den läser i `trace`.
using Script = std::function<void(ObjectReader&, std::string& trace)>;

// Allt som syns utifrån efter en läsning.
struct Outcome {
    std::string trace;
    bool ok;
    ErrorCode code;
    std::string detail;
    size_t pos;
    size_t non_utf8;
    int32_t next_id;

    bool operator==(const Outcome& o) const {
        return trace == o.trace && ok == o.ok && code == o.code && detail == o.detail &&
               pos == o.pos && non_utf8 == o.non_utf8 && next_id == o.next_id;
    }
};

Outcome outcome(ObjectReader& r, std::string trace) {
    return {std::move(trace), r.ok(), r.code(), r.detail(), r.pos(), r.non_utf8_strings(),
            r.next_id()};
}

Outcome run_buffer(const Bytes& data, const Script& f) {
    ObjectReader r(data.data(), data.size(), "LcOpTest");
    std::string t;
    f(r, t);
    return outcome(r, t);
}

// `data` packad i lagrade block om `block` byte, läst genom ett fönster på
// minst `window` byte. Taket är strömmens längd, om inget annat ges.
Outcome run_window(const Bytes& data, size_t window, size_t block, const Script& f,
                   uint64_t cap = uint64_t(-1)) {
    const Bytes z = testdata::zlib_store(data, block);
    InflateStream s;
    s.open(z.data(), z.size(), cap == uint64_t(-1) ? data.size() : cap);
    StreamWindow w(s, window);
    ObjectReader r(w, "LcOpTest");
    std::string t;
    f(r, t);
    return outcome(r, t);
}

// Samma utfall genom varje fönsterstorlek och blockstorlek som med en buffert.
int same_as_buffer(const Bytes& data, const Script& f, const char* what) {
    int fails = 0;
    const Outcome ref = run_buffer(data, f);
    for (const size_t window : {size_t(1), size_t(3), size_t(5), size_t(8), size_t(13), size_t(4096)})
        for (const size_t block : {size_t(3), size_t(65535)}) {
            const Outcome o = run_window(data, window, block, f);
            if (!(o == ref)) {
                std::printf("  FAIL — %s, window %zu, block %zu: \"%s\" vs \"%s\"\n", what, window,
                            block, o.detail.c_str(), ref.detail.c_str());
                ++fails;
            }
        }
    return fails;
}

void add(std::string& t, double v) { t += std::to_string(v) + ";"; }
void add(std::string& t, int64_t v) { t += std::to_string(v) + ";"; }
void add(std::string& t, const std::string& s) { t += "[" + s + "];"; }

// Varje läsning i tur och ordning tills något fallerar.
void read_values(ObjectReader& r, std::string& t) {
    int32_t a;
    double d;
    int64_t i;
    float f;
    if (!r.i32(a)) return;
    add(t, int64_t(a));
    if (!r.f64(d)) return;
    add(t, d);
    if (!r.i32(a)) return;
    add(t, int64_t(a));
    if (!r.i64(i)) return;
    add(t, i);
    if (!r.i32(a) || !r.i32(a)) return;
    add(t, int64_t(a));
    if (!r.f64(d)) return;
    add(t, d);
    if (!r.f32(f)) return;
    add(t, double(f));
    if (!r.i32(a)) return;
    t += r.end() ? "end" : "no end";
}

void read_all_script(ObjectReader& r, std::string& t) {
    Ptr p;
    if (!r.ptr(p, {13}, false, false)) return;
    add(t, int64_t(p.id));
    int32_t a, n;
    double d;
    std::string s;
    if (!r.i32(a) || !r.f64(d) || !r.str(s) || !r.count(4, n)) return;
    add(t, d);
    add(t, s);
    for (int32_t i = 0; i < n; ++i) {
        if (!r.i32(a)) return;
        add(t, int64_t(a));
    }
    if (!r.skip_padded(5) || !r.finish(p.id, 42)) return;
    Ptr q;
    if (!r.ptr(q, {13}, false, true)) return;
    add(t, int64_t(q.value));
    t += r.end() ? "end" : "no end";
}

void strings_script(ObjectReader& r, std::string& t) {
    std::string s;
    bool is_null = false;
    while (r.str(s, &is_null)) add(t, is_null ? std::string("null") : s);
    t += "stop";
}

void ptrs_script(ObjectReader& r, std::string& t) {
    // Pekare tills något fallerar: nya objekt av klass 13–15 färdigställs
    // direkt, bakåtpekare tas emot.
    for (int k = 0; k < 2000; ++k) {
        Ptr p;
        if (!r.ptr(p, {13, 14, 15, 185, 200}, true, true)) return;
        add(t, int64_t(p.kind) * 1000 + p.cls);
        if (p.kind == Ptr::New && !r.finish(p.id, static_cast<uint32_t>(p.id) * 3)) return;
        if (p.kind == Ptr::Back) add(t, int64_t(p.value));
    }
}

void nest_script(ObjectReader& r, std::string& t) {
    Ptr p;
    while (r.ptr(p, {13}, false, false)) add(t, int64_t(p.id));
}

void class185_script(ObjectReader& r, std::string& t) {
    t += skip_class185(r) ? "skipped;" : "failed;";
    t += r.end() ? "end" : "no end";
}

void count_script(ObjectReader& r, std::string& t) {
    int32_t n = 0;
    if (!r.count(4, n)) return;
    add(t, int64_t(n));
    int32_t a;
    for (int32_t i = 0; i < n; ++i)
        if (!r.i32(a)) return;
    t += r.end() ? "end" : "no end";
}

void skip_script(ObjectReader& r, std::string& t) {
    int64_t len;
    if (!r.i64(len) || !r.skip_padded(len)) return;
    add(t, len);
    t += r.end() ? "end" : "no end";
}

// Alla avhuggna längder och en med byte efter slutet.
std::vector<Bytes> cuts(const Bytes& full) {
    std::vector<Bytes> out;
    for (size_t n = 0; n <= full.size(); ++n)
        out.emplace_back(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(n));
    Bytes extra = full;
    extra.insert(extra.end(), 8, 0);
    out.push_back(extra);
    return out;
}

int window_tests() {
    int fails = 0;

    // Samma värden, fel och detail som med en buffert, över varje gräns.
    {
        ByteWriter w;
        w.i32(5);
        w.f64(1.5);
        w.i32(-9);
        w.i64a(-123456789012345);
        w.i32(1);
        w.i32(2);
        w.f64(-0.25);
        w.f32(3.5f);
        w.i32(0);
        for (const Bytes& b : cuts(w.data)) fails += same_as_buffer(b, read_values, "values");
        Bytes bad = w.data;
        bad[4] = 1;   // PAD8 som inte är noll
        fails += same_as_buffer(bad, read_values, "pad8");
    }
    for (const Bytes& b : cuts(read_all_stream())) fails += same_as_buffer(b, read_all_script, "read_all");
    {
        ByteWriter w;
        for (const char* s : {"", "a", "abcd", "abcde", "\xC0\x80", "\xE2\x82\xAC", "ab\xFF"})
            w.str(s);
        w.null_str();
        w.str(std::string(37, 'x'));
        for (const Bytes& b : cuts(w.data)) fails += same_as_buffer(b, strings_script, "strings");
        ByteWriter bad;
        bad.i32(1);
        bad.hex("61000100");   // utfyllnad som inte är noll
        fails += same_as_buffer(bad.data, strings_script, "string pad");
        for (const int32_t len : {-2, 100, 0x7FFFFFFF}) {
            ByteWriter l;
            l.i32(len);
            l.zeros(50);
            fails += same_as_buffer(l.data, strings_script, "string length");
        }
    }
    {
        ByteWriter w;
        w.null_ptr();
        const int32_t a = w.obj(13);
        const int32_t b = w.obj(185);
        w.ref(b);
        w.ref(a);
        w.obj(200);
        w.ref(102);
        w.i32(1);
        w.i32(150);   // okänt id
        for (const Bytes& c : cuts(w.data)) fails += same_as_buffer(c, ptrs_script, "pointers");
        for (const int32_t lead : {2, 99, -5, 200}) {
            ByteWriter l;
            l.i32(lead);
            l.i32(13);
            fails += same_as_buffer(l.data, ptrs_script, "lead");
        }
        ByteWriter cls;
        cls.obj(16);
        fails += same_as_buffer(cls.data, ptrs_script, "class");
    }
    {
        ByteWriter w;
        for (int i = 0; i <= kMaxNesting; ++i) w.obj(13);
        const Outcome o = run_buffer(w.data, nest_script);
        CHECK(o.code == ErrorCode::LimitExceeded);
        fails += same_as_buffer(w.data, nest_script, "nesting");
    }
    for (const Bytes& b : cuts(class185_stream())) fails += same_as_buffer(b, class185_script, "185");
    for (const int64_t len : {int64_t(-1), int64_t(9), int64_t(0x7FFFFFFFFFFFFFFF)})
        fails += same_as_buffer(class185_stream(1, len), class185_script, "185 length");
    fails += same_as_buffer(class185_stream(2), class185_script, "185 flag");
    for (const int32_t n : {0, 3, 4, -1, 0x40000000}) {
        ByteWriter w;
        w.i32(n);
        w.zeros(12);
        fails += same_as_buffer(w.data, count_script, "count");
    }
    for (const int64_t len : {int64_t(0), int64_t(5), int64_t(13), int64_t(-1), int64_t(200000)}) {
        ByteWriter w;
        w.i64(len);
        w.zeros(13);
        w.pad(8);
        fails += same_as_buffer(w.data, skip_script, "skip_padded");
    }
    {
        ByteWriter w;   // utfyllnaden efter byten är inte noll
        w.i64(5);
        w.hex("0102030405 000100");
        fails += same_as_buffer(w.data, skip_script, "skip pad");
    }

    // Lång sträng och lång skip_padded genom ett litet fönster: fönstret
    // växer för strängen, och skip går fram i steg.
    {
        ByteWriter w;
        const std::string big(100003, 'q');
        w.str(big);
        w.i64a(300001);
        w.zeros(300001);
        w.pad(4);
        w.f64(7.25);
        w.pad(8);
        const Bytes z = testdata::zlib_store(w.data, 65535);
        InflateStream s;
        CHECK(s.open(z.data(), z.size(), w.data.size()) == ErrorCode::Ok);
        StreamWindow win(s, 16);
        ObjectReader r(win, "LcOpTest");
        std::string got;
        int64_t len = 0;
        double d = 0;
        CHECK(r.str(got) && got == big);
        CHECK(r.i64(len) && len == 300001 && r.skip_padded(len));
        CHECK(r.f64(d) && d == 7.25);
        CHECK(r.end() && r.pos() == w.data.size());
    }

    // Positionerna är absoluta i strömmen: efter resume vid byte 1004 (4 mod
    // 8) kommer PAD8 före ett f64.
    {
        ByteWriter w;
        w.zeros(1004);
        w.f64(2.5);       // PAD8 1004..1008, värdet 1008..1016
        w.str("xyz");     // 1016..1024
        w.pad(8);
        const Bytes z = testdata::zlib_store(w.data, 1004);
        InflateStream s;
        std::vector<InflateStream::Checkpoint> cps;
        CHECK(s.open(z.data(), z.size(), w.data.size()) == ErrorCode::Ok);
        s.record_checkpoints(0, &cps);
        CHECK(s.skip(w.data.size()) == ErrorCode::Ok);
        CHECK(cps.size() == 1 && cps[0].out_pos == 1004);
        if (cps.size() == 1) {
            CHECK(s.resume(cps[0]) == ErrorCode::Ok);
            StreamWindow win(s, 8);
            ObjectReader r(win, "LcOpTest");
            double d = 0;
            std::string str;
            CHECK(r.pos() == 1004);
            CHECK(r.f64(d) && d == 2.5 && r.pos() == 1016);
            CHECK(r.str(str) && str == "xyz");
            CHECK(r.end() && r.pos() == 1024);
        }
    }

    // Fel i strömmen kommer fram med strömmens kod efter "<what> at byte <n>: ".
    {
        Bytes data = read_all_stream();
        Bytes z = testdata::zlib_store(data, 7);
        z.back() ^= 1;   // Adler-32
        InflateStream s;
        CHECK(s.open(z.data(), z.size(), data.size()) == ErrorCode::Ok);
        StreamWindow win(s, 4);
        ObjectReader r(win, "LcOpTest");
        Ptr p;
        int32_t a, n;
        double d;
        std::string str;
        CHECK(r.ptr(p, {13}, false, false) && r.i32(a) && r.f64(d) && r.str(str) && r.count(4, n));
        CHECK(r.i32(a) && r.i32(a) && r.skip_padded(5) && r.finish(p.id, 1));
        CHECK(r.ptr(p, {13}, false, true));
        CHECK(!r.end());
        CHECK(r.code() == ErrorCode::CorruptStream);
        CHECK(r.detail().rfind("LcOpTest at byte ", 0) == 0);
        CHECK(r.detail().find("Adler-32") != std::string::npos);

        // Taket under strömmens längd.
        InflateStream t;
        const Bytes z2 = testdata::zlib_store(data, 7);
        CHECK(t.open(z2.data(), z2.size(), data.size() - 8) == ErrorCode::Ok);
        StreamWindow win2(t, 4);
        ObjectReader r2(win2, "LcOpTest");
        std::string trace;
        read_all_script(r2, trace);
        CHECK(!r2.ok());
        CHECK(r2.code() == ErrorCode::StreamTooLarge);
        CHECK(r2.detail().find("limit") != std::string::npos);

        // Trasig zlib mitt i: det före går att läsa.
        Bytes z3 = testdata::zlib_store(data, 7);
        // Block i om 7 byte börjar på 2 + 12·i; NLEN i block 4 (byte 28..35).
        z3[2 + 4 * 12 + 3] ^= 1;
        InflateStream u;
        CHECK(u.open(z3.data(), z3.size(), data.size()) == ErrorCode::Ok);
        StreamWindow win3(u, 4);
        ObjectReader r3(win3, "LcOpTest");
        CHECK(r3.ptr(p, {13}, false, false) && r3.i32(a) && r3.f64(d));
        CHECK(r3.pos() == 24);
        CHECK(!r3.str(str));   // längden på 24..28 finns, byten på 28.. gör det inte
        CHECK(r3.code() == ErrorCode::CorruptStream);
        CHECK(r3.detail().rfind("LcOpTest at byte 28: zlib stream at input byte ", 0) == 0);
    }

    // Antalet genom ett fönster prövas mot en övre gräns: taket tills slutet
    // är nått. Listan fallerar då på den post som saknas.
    {
        ByteWriter w;
        w.i32(4);
        w.zeros(12);
        const Outcome partial = run_window(w.data, 4, 65535, count_script, 1 << 20);
        CHECK(!partial.ok && partial.trace == "4;" && partial.code == ErrorCode::CorruptObject);
        CHECK(partial.detail == "LcOpTest at byte 16: i32 passes the end of the data");
        // Med hela strömmen i fönstret är slutet känt, som med en buffert.
        const Outcome whole = run_window(w.data, 4096, 65535, count_script, 1 << 20);
        CHECK(whole == run_buffer(w.data, count_script));
    }

    // StreamWindow: positioner, släppta byte, slutet och gränsen.
    {
        const Bytes data = testdata::pattern(10000);
        const Bytes z = testdata::zlib_store(data, 999);
        InflateStream s;
        CHECK(s.open(z.data(), z.size(), 1 << 20) == ErrorCode::Ok);
        StreamWindow win(s, 100);
        CHECK(win.start() == 0 && win.bound() == (1u << 20));
        CHECK(win.ensure(0, 10) && std::memcmp(win.at(0), data.data(), 10) == 0);
        CHECK(win.ensure(5000, 300) && std::memcmp(win.at(5000), data.data() + 5000, 300) == 0);
        // Inget är släppt: början finns kvar.
        CHECK(win.ensure(0, 10) && std::memcmp(win.at(0), data.data(), 10) == 0);
        win.release_before(6000);
        CHECK(win.ensure(6000, 4000) && std::memcmp(win.at(6000), data.data() + 6000, 4000) == 0);
        CHECK(!win.ensure(9999, 2));
        CHECK(win.error() == ErrorCode::Ok);
        CHECK(win.ends_at(10000) && !win.ends_at(9999));
        CHECK(win.bound() == 10000);
        CHECK(!win.ensure(10, 1));   // före det släppta
        CHECK(win.error() == ErrorCode::OutOfRange && !win.detail().empty());
    }

    // Id-tabellen: 8 byte per id, och klasser upp till 255 går fram och tillbaka.
    {
        CHECK(ObjectReader::kIdEntryBytes == 8);
        ByteWriter w;
        w.obj(255);
        w.obj(185);
        w.ref(100);
        w.ref(101);
        w.obj(256);
        ObjectReader r = make(w);
        Ptr p;
        CHECK(r.ptr(p, {255}, false, false) && r.finish(p.id, 0xFFFFFFFFu));
        CHECK(r.ptr(p, {185}, false, false) && r.finish(p.id, 7));
        CHECK(r.ptr(p, {255}, false, true) && p.cls == 255 && p.value == 0xFFFFFFFFu);
        CHECK(r.ptr(p, {185}, false, true) && p.cls == 185 && p.value == 7);
        CHECK(!r.ptr(p, {256}, false, false));
        CHECK(r.code() == ErrorCode::CorruptObject);
        CHECK(r.detail().find("8 bits") != std::string::npos);
    }
    return fails;
}

} // namespace

int test_object_reader() {
    int fails = 0;

    // Heltal och flyttal, med PAD8 på 4 mod 8 och utan på 0 mod 8.
    {
        ByteWriter w;
        w.i32(5);       // 0..4
        w.f64(1.5);     // pad 4..8, värde 8..16
        w.i32(-9);      // 16..20
        w.i64a(-123456789012345);   // pad 20..24, värde 24..32
        w.i32(1);       // 32..36
        w.i32(2);       // 36..40
        w.f64(-0.25);   // ingen pad, 40..48
        w.f32(3.5f);    // 48..52
        w.i32(0);       // 52..56
        ObjectReader r = make(w);
        int32_t a = 0;
        double d = 0;
        int64_t i = 0;
        float f = 0;
        CHECK(r.i32(a) && a == 5 && r.pos() == 4);
        CHECK(r.f64(d) && d == 1.5 && r.pos() == 16);
        CHECK(r.i32(a) && a == -9);
        CHECK(r.i64(i) && i == -123456789012345 && r.pos() == 32);
        CHECK(r.i32(a) && a == 1 && r.i32(a) && a == 2);
        CHECK(r.f64(d) && d == -0.25 && r.pos() == 48);
        CHECK(r.f32(f) && f == 3.5f && r.pos() == 52);
        CHECK(r.i32(a) && a == 0);
        CHECK(r.end());
        CHECK(r.ok() && r.code() == ErrorCode::Ok && r.detail().empty());
    }

    // PAD8 som inte är noll.
    {
        ByteWriter w;
        w.i32(5);
        w.i32(1);       // paddingen
        w.i64(9);
        ObjectReader r = make(w);
        int32_t a;
        int64_t v;
        CHECK(r.i32(a));
        CHECK(!r.i64(v));
        CHECK(!r.ok());
        CHECK(r.code() == ErrorCode::CorruptObject);
        CHECK(r.detail().rfind("LcOpTest at byte ", 0) == 0);
        CHECK(r.detail().find("alignment pad") != std::string::npos);
    }

    // Feltillståndet fastnar och behåller det första felet.
    {
        ByteWriter w;
        w.i32(1);
        ObjectReader r = make(w);
        int32_t a;
        CHECK(r.i32(a));
        CHECK(!r.i32(a));
        const std::string first = r.detail();
        CHECK(!first.empty());
        CHECK(!r.i32(a));
        CHECK(!r.fail("other"));
        CHECK(r.detail() == first);
        double d;
        std::string s;
        int32_t n;
        CHECK(!r.f64(d) && !r.str(s) && !r.count(1, n) && !r.end() && !r.implicit_root());
        Ptr p;
        CHECK(!r.ptr(p, {1}, true, true) && !r.finish(100, 0));
    }

    // Strängar: tom, null, med utfyllnad, och felen.
    {
        ByteWriter w;
        w.str("");
        w.null_str();
        w.str("a");
        w.str("abcd");
        w.str("abcde");
        w.pad(8);
        ObjectReader r = make(w);
        std::string s;
        bool is_null = true;
        CHECK(r.str(s, &is_null) && s.empty() && !is_null);
        CHECK(r.str(s, &is_null) && s.empty() && is_null);
        CHECK(r.str(s) && s == "a");
        CHECK(r.str(s) && s == "abcd");
        CHECK(r.str(s, &is_null) && s == "abcde" && !is_null);
        CHECK(r.end() && r.non_utf8_strings() == 0);
    }
    {
        ByteWriter w;
        w.i32(1);
        w.u8('a');
        w.u8(0);
        w.u8(1);        // utfyllnad som inte är noll
        w.u8(0);
        ObjectReader r = make(w);
        std::string s;
        CHECK(!r.str(s));
        CHECK(r.detail().find("pad") != std::string::npos);
    }
    {
        ByteWriter w;
        w.i32(-2);      // bara -1 är null
        ObjectReader r = make(w);
        std::string s;
        CHECK(!r.str(s));
        CHECK(r.code() == ErrorCode::CorruptObject);
    }
    {
        ByteWriter w;
        w.i32(100);     // längden ryms inte
        w.zeros(50);
        ObjectReader r = make(w);
        std::string s;
        CHECK(!r.str(s));
    }
    {
        ByteWriter w;
        w.i32(5);       // längden ryms, men inte utfyllnaden
        w.text("abcde");
        ObjectReader r = make(w);
        std::string s;
        CHECK(!r.str(s));
    }
    {
        ByteWriter w;
        w.i32(0x7FFFFFFF);
        w.zeros(16);
        ObjectReader r = make(w);
        std::string s;
        CHECK(!r.str(s));
    }

    // Ogiltig UTF-8 är inget fel men räknas; giltig flerbytes-UTF-8 räknas inte.
    {
        const std::vector<std::string> bad = {
            "\xC0\x80",             // överlång form
            "\xC1\xBF",             // överlång form
            "\xE0\x80\x80",         // överlång form
            "\xED\xA0\x80",         // surrogat
            "\xF4\x90\x80\x80",     // över U+10FFFF
            "\xF5\x80\x80\x80",     // ledare som inte finns
            "\xE2\x82",             // avhuggen
            "\x80",                 // ensam fortsättningsbyte
            "ab\xFF",
        };
        const std::vector<std::string> good = {
            "", "plain", "\xC3\xA5\xC3\xA4\xC3\xB6", "\xE2\x82\xAC", "\xF0\x90\x8D\x88",
            "\xF4\x8F\xBF\xBF", "\xED\x9F\xBF", std::string("a\0b", 3), "\xC2\x80",
        };
        ByteWriter w;
        for (const std::string& s : bad) w.str(s);
        for (const std::string& s : good) w.str(s);
        w.null_str();
        ObjectReader r = make(w);
        std::string s;
        for (const std::string& expect : bad) {
            CHECK(r.str(s) && s == expect);
        }
        CHECK(r.non_utf8_strings() == bad.size());
        for (const std::string& expect : good) {
            CHECK(r.str(s) && s == expect);
        }
        CHECK(r.str(s));
        CHECK(r.non_utf8_strings() == bad.size());
        CHECK(r.end());

        for (const std::string& b : bad)
            CHECK(!valid_utf8(reinterpret_cast<const uint8_t*>(b.data()), b.size()));
        for (const std::string& g : good)
            CHECK(valid_utf8(reinterpret_cast<const uint8_t*>(g.data()), g.size()));
        CHECK(valid_utf8(nullptr, 0));
    }

    // Antal: ska rymmas i resten av strömmen.
    {
        ByteWriter w;
        w.i32(3);
        w.zeros(12);
        ObjectReader r = make(w);
        int32_t n = 0;
        CHECK(r.count(4, n) && n == 3);
    }
    {
        ByteWriter w;
        w.i32(4);
        w.zeros(12);    // fyra poster à 4 byte ryms inte
        ObjectReader r = make(w);
        int32_t n;
        CHECK(!r.count(4, n));
        CHECK(r.detail().find("count") != std::string::npos);
    }
    {
        ByteWriter w;
        w.i32(-1);
        w.zeros(12);
        ObjectReader r = make(w);
        int32_t n;
        CHECK(!r.count(1, n));
    }
    {
        ByteWriter w;
        w.i32(0x7FFFFFFF);
        w.zeros(64);
        ObjectReader r = make(w);
        int32_t n;
        CHECK(!r.count(0xFFFFFFFFu, n));   // produkten får inte slå runt
    }
    {
        ByteWriter w;
        w.i32(0x40000000);
        w.zeros(64);
        ObjectReader r = make(w);
        int32_t n;
        CHECK(!r.count(4, n));             // 2^32 slår runt i 32 bitar
    }
    {
        ByteWriter w;
        w.i32(0);
        ObjectReader r = make(w);
        int32_t n = 1;
        CHECK(r.count(1000, n) && n == 0);
    }

    // skip_padded.
    {
        ByteWriter w;
        w.hex("0102030405");
        w.pad(4);
        w.i32(9);
        ObjectReader r = make(w);
        int32_t a;
        CHECK(r.skip_padded(5) && r.pos() == 8);
        CHECK(r.i32(a) && a == 9);
    }
    {
        ByteWriter w;
        w.hex("01020304 0500 0001");   // padding med en etta
        ObjectReader r = make(w);
        CHECK(!r.skip_padded(5));
    }
    for (const int64_t len : {int64_t(-1), int64_t(9), int64_t(0x7FFFFFFFFFFFFFFF),
                              int64_t(0x7FFFFFFFFFFFFFFD), -0x7FFFFFFFFFFFFFFF - 1}) {
        ByteWriter w;
        w.zeros(8);
        ObjectReader r = make(w);
        CHECK(!r.skip_padded(len));
    }
    {
        ByteWriter w;
        w.hex("01020304 05");           // längden ryms, utfyllnaden gör det inte
        ObjectReader r = make(w);
        CHECK(!r.skip_padded(5));
    }
    {
        ByteWriter w;
        ObjectReader r = make(w);
        CHECK(r.skip_padded(0) && r.end());
    }

    // implicit_root: id 100, och nästa objekt får 101.
    {
        ByteWriter w;
        w.implicit_root();
        w.obj(13);
        ObjectReader r = make(w);
        Ptr p;
        CHECK(r.next_id() == 100);
        CHECK(r.implicit_root() && r.next_id() == 101);
        CHECK(r.ptr(p, {13}, false, false) && p.kind == Ptr::New && p.id == 101 && p.cls == 13);
        CHECK(r.next_id() == 102);
    }
    {
        ByteWriter w;
        w.i32(101);
        ObjectReader r = make(w);
        CHECK(!r.implicit_root());
        CHECK(r.detail().find("implicit root") != std::string::npos);
    }
    {
        // Roten räknas som färdig och går att peka på om platsen tar klass 0.
        ByteWriter w;
        w.implicit_root();
        w.ref(100);
        w.ref(100);
        ObjectReader r = make(w);
        Ptr p;
        CHECK(r.implicit_root());
        CHECK(r.ptr(p, {0}, false, true) && p.kind == Ptr::Back && p.cls == 0 && p.id == 100);
        CHECK(!r.ptr(p, {13}, false, true));   // klassen tas inte emot
    }

    // Pekare: null, nya objekt, bakåtpekare.
    {
        ByteWriter w;
        w.null_ptr();
        w.null_ptr();
        const int32_t a = w.obj(13);      // 100
        w.i32(77);
        const int32_t b = w.obj(14);      // 101, i kroppen av 100
        w.ref(b);                         // bakåtpekare i samma kropp, 101 är färdigt
        const int32_t c = w.obj(15);      // 102
        w.ref(a);
        w.ref(a);
        CHECK(a == 100 && b == 101 && c == 102);

        ObjectReader r = make(w);
        Ptr p;
        int32_t x;
        CHECK(r.ptr(p, {13}, true, false) && p.kind == Ptr::Null && p.id == 0 && p.cls == 0);
        CHECK(r.ptr(p, {13}, true, true) && p.kind == Ptr::Null);
        CHECK(r.ptr(p, {13}, false, false) && p.kind == Ptr::New && p.id == 100 && p.cls == 13);
        CHECK(r.i32(x) && x == 77);
        Ptr inner;
        CHECK(r.ptr(inner, {14}, false, false) && inner.id == 101 && inner.cls == 14);
        CHECK(r.finish(inner.id, 5));
        Ptr back;
        CHECK(r.ptr(back, {14}, false, true));
        CHECK(back.kind == Ptr::Back && back.id == 101 && back.cls == 14 && back.value == 5);
        Ptr third;
        CHECK(r.ptr(third, {15}, false, false) && third.id == 102);
        CHECK(r.finish(third.id, 6));
        // 100 är fortfarande ofärdigt.
        CHECK(!r.ptr(back, {13}, false, true));
        CHECK(r.detail().find("unfinished") != std::string::npos);
    }

    // Alla pekarfel, ett i taget.
    {
        struct Case {
            const char* name;
            Bytes bytes;
            int32_t allowed;
            bool allow_null, allow_back;
        };
        auto make_bytes = [](auto fill) {
            ByteWriter w;
            fill(w);
            return w.data;
        };
        const std::vector<Case> cases = {
            {"null where an object is needed", make_bytes([](ByteWriter& w) { w.null_ptr(); }), 13, false, true},
            {"back-reference where a new object is needed", make_bytes([](ByteWriter& w) { w.i32(1); w.i32(100); }), 13, true, false},
            {"back-reference to an unknown id (below 100)", make_bytes([](ByteWriter& w) { w.i32(1); w.i32(99); }), 13, true, true},
            {"back-reference to an unknown id (not yet used)", make_bytes([](ByteWriter& w) { w.i32(1); w.i32(100); }), 13, true, true},
            {"back-reference to a negative id", make_bytes([](ByteWriter& w) { w.i32(1); w.i32(-1); }), 13, true, true},
            {"lead that is neither 0, 1 nor the next id (2)", make_bytes([](ByteWriter& w) { w.i32(2); w.i32(13); }), 13, true, true},
            {"lead that is the next id plus one", make_bytes([](ByteWriter& w) { w.i32(101); w.i32(13); }), 13, true, true},
            {"lead that is the previous id", make_bytes([](ByteWriter& w) { w.i32(99); w.i32(13); }), 13, true, true},
            {"negative lead", make_bytes([](ByteWriter& w) { w.i32(-5); w.i32(13); }), 13, true, true},
            {"class that the slot does not allow", make_bytes([](ByteWriter& w) { w.obj(14); }), 13, true, true},
            {"lead without a class", make_bytes([](ByteWriter& w) { w.i32(100); }), 13, true, true},
            {"back-reference without an id", make_bytes([](ByteWriter& w) { w.i32(1); }), 13, true, true},
            {"empty stream", Bytes(), 13, true, true},
        };
        for (const Case& c : cases) {
            ObjectReader r(c.bytes.data(), c.bytes.size(), "LcOpTest");
            Ptr p;
            CHECK(!r.ptr(p, {c.allowed}, c.allow_null, c.allow_back));
            if (r.ok()) std::printf("  not rejected: %s\n", c.name);
            CHECK(!r.ok());
            CHECK(r.code() == ErrorCode::CorruptObject);
            CHECK(r.detail().rfind("LcOpTest at byte ", 0) == 0);
            CHECK(p.kind == Ptr::Null);
        }
        // En tom lista över tillåtna klasser tar inte emot något nytt objekt.
        {
            ByteWriter e;
            e.obj(13);
            ObjectReader er = make(e);
            Ptr ep;
            CHECK(!er.ptr(ep, {}, true, true));
        }
        // En bakåtpekare till ett färdigt objekt av en klass platsen inte tar emot.
        ByteWriter w;
        w.obj(13);
        w.ref(100);
        ObjectReader r = make(w);
        Ptr p;
        CHECK(r.ptr(p, {13}, false, false));
        CHECK(r.finish(p.id, 1));
        CHECK(!r.ptr(p, {14, 15}, false, true));
        CHECK(r.detail().find("class") != std::string::npos);
    }

    // finish: okänt id, redan färdigt, och utan öppet objekt.
    {
        ByteWriter w;
        w.obj(13);
        w.obj(13);
        {
            ObjectReader r = make(w);
            Ptr p;
            CHECK(r.ptr(p, {13}, false, false));
            CHECK(!r.finish(101, 0));       // ännu inte läst
        }
        {
            ObjectReader r = make(w);
            Ptr p;
            CHECK(r.ptr(p, {13}, false, false) && r.finish(p.id, 0));
            CHECK(!r.finish(p.id, 0));      // redan färdigt
        }
        {
            ObjectReader r = make(w);
            CHECK(!r.finish(100, 0));
            CHECK(!r.finish(99, 0));
        }
    }

    // Djup: 256 objekt i varandra går, det 257:e ger LimitExceeded. finish minskar djupet.
    {
        ByteWriter w;
        for (int i = 0; i < kMaxNesting; ++i) w.obj(13);
        {
            ObjectReader r = make(w);
            Ptr p;
            std::vector<int32_t> ids;
            for (int i = 0; i < kMaxNesting; ++i) {
                CHECK(r.ptr(p, {13}, false, false));
                ids.push_back(p.id);
            }
            CHECK(r.ok());
            for (size_t i = ids.size(); i-- > 0;) CHECK(r.finish(ids[i], static_cast<uint32_t>(i)));
            CHECK(r.ok());
        }
        w.obj(13);   // nummer 257
        ObjectReader r = make(w);
        Ptr p;
        for (int i = 0; i < kMaxNesting; ++i) CHECK(r.ptr(p, {13}, false, false));
        CHECK(r.ok());
        CHECK(!r.ptr(p, {13}, false, false));
        CHECK(r.code() == ErrorCode::LimitExceeded);
        CHECK(r.detail().rfind("LcOpTest at byte ", 0) == 0);
        CHECK(!r.ok());
    }
    {
        // Många objekt efter varandra ger inget djup så länge varje är färdigt.
        ByteWriter w;
        for (int i = 0; i < 1000; ++i) w.obj(13);
        ObjectReader r = make(w);
        Ptr p;
        for (int i = 0; i < 1000; ++i) {
            CHECK(r.ptr(p, {13}, false, false) && p.id == 100 + i);
            CHECK(r.finish(p.id, 0));
        }
        CHECK(r.ok() && r.end());
    }

    // end(): nollor till 8 och därefter slut.
    {
        ByteWriter w;
        w.i32(1);
        w.i32(0);
        ObjectReader r = make(w);
        int32_t a;
        CHECK(r.i32(a) && r.end() && r.pos() == 8);
    }
    {
        ByteWriter w;       // redan på 8 och slut
        w.i64(0);
        ObjectReader r = make(w);
        int64_t v;
        CHECK(r.i64(v) && r.end());
    }
    {
        ByteWriter w;       // tom ström
        ObjectReader r = make(w);
        CHECK(r.end());
    }
    {
        ByteWriter w;       // byte kvar efter paddingen
        w.i32(1);
        w.i64(0);
        w.i32(0);
        ObjectReader r = make(w);
        int32_t a;
        CHECK(r.i32(a) && !r.end());
    }
    {
        ByteWriter w;       // paddingen är inte noll
        w.i32(1);
        w.i32(1);
        ObjectReader r = make(w);
        int32_t a;
        CHECK(r.i32(a) && !r.end());
        CHECK(r.detail().find("pad") != std::string::npos);
    }
    {
        ByteWriter w;       // strömmen slutar inte på en multipel av 8
        w.i32(1);
        ObjectReader r = make(w);
        int32_t a;
        CHECK(r.i32(a) && !r.end());
    }
    {
        ByteWriter w;       // markören mitt i strömmen: paddingen är mer än 8 byte bort
        w.zeros(16);
        ObjectReader r = make(w);
        int32_t a;
        CHECK(r.i32(a) && !r.end());
    }

    // En hel ström läses, och varje avhuggen längd ger ett fel.
    {
        const Bytes full = read_all_stream();
        CHECK(read_all(full));
        for (size_t n = 0; n < full.size(); ++n) {
            const Bytes cut(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(n));
            CHECK(!read_all(cut));
        }
        Bytes extra = full;
        extra.insert(extra.end(), 8, 0);   // byte efter slutet
        CHECK(!read_all(extra));
    }

    // Class 185.
    {
        CHECK(read_185(class185_stream()));
        for (size_t n = 0; n < class185_stream().size(); ++n) {
            const Bytes full = class185_stream();
            const Bytes cut(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(n));
            CHECK(!read_185(cut));
        }
        CHECK(!read_185(class185_stream(2)));        // flaggan är varken 0 eller 1
        CHECK(!read_185(class185_stream(-1)));
        CHECK(!read_185(class185_stream(1, -1)));    // negativ längd
        CHECK(!read_185(class185_stream(1, 9)));     // längden ryms inte
        CHECK(!read_185(class185_stream(1, 0x7FFFFFFFFFFFFFFF)));
        ByteWriter w;
        w.str("a");
        w.str("b");
        w.i32(1000);                                  // fler poster än byten räcker till
        w.zeros(64);
        CHECK(!read_185(w.data));

        Bytes bad = class185_stream();
        ObjectReader r(bad.data(), bad.size(), "LcOpTest");
        CHECK(skip_class185(r) && r.end());
        bad[0] = 0xFF;                                // längden på första strängen
        CHECK(!read_185(bad));
    }

    // Prov mot exempelfilerna: id-regeln i LcOpNwdFragments i den minsta .nwc.
    {
        const testdata::SampleCounts* smallest = nullptr;
        for (const testdata::SampleCounts& s : testdata::kSamples)
            if (s.top_class == 53 && (!smallest || s.fragments < smallest->fragments)) smallest = &s;
        if (!smallest || !testdata::exists(smallest->path)) {
            std::printf("  SKIP — exempelfilen saknas\n");
            ++g_skipped;
        } else {
            MappedFile file;
            Container c;
            CHECK(file.open(smallest->path) == ErrorCode::Ok);
            CHECK(file.is_open() &&
                  read_container(file.data(), file.size(), c) == ErrorCode::Ok);
            const DbCacheEntry* e = file.is_open() ? find_entry(c.entries, "LcOpNwdFragments") : nullptr;
            CHECK(e != nullptr);
            StreamSpan span;
            Bytes data;
            if (e) {
                CHECK(last_stream(*e, file.size(), span) == ErrorCode::Ok);
                CHECK(inflate_span(file.data(), file.size(), span, kDefaultMaxStreamBytes, data) ==
                      ErrorCode::Ok);
            }
            if (!data.empty()) {
                ObjectReader r(data.data(), data.size(), "LcOpNwdFragments");
                int32_t n = 0;
                Ptr p;
                CHECK(r.i32(n));
                CHECK(static_cast<uint32_t>(n) == smallest->fragments);
                CHECK(r.ptr(p, {13}, false, false));
                CHECK(p.kind == Ptr::New && p.id == 100 && p.cls == 13);
                CHECK(r.ok());
                CHECK(r.next_id() == 101);
            }
        }
    }

    fails += window_tests();

    std::printf("test_object_reader: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
