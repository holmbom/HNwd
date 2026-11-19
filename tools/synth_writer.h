// Skriver en syntetisk fil med formatets belagda grammatiker, för prov med
// stora filer. Delas av verktyget nwdsynth och testerna.
//
// Allt följer ur fröet, och filen skrivs strömmande: det som hålls i minnet
// är en postlängd och ett id per geometripost och en storlek per chunk, inte
// fragmenten, trädet eller geometrin. Komprimeringen är zlib med lagrade
// block (RFC 1951, BTYPE 00), som varje avkodare läser.
//
// Filen har paketen
//
//   LcOpNwdGeometry            (4, b) prefix 65536: poster i chunkar som packas
//                              upp till exakt 65536 byte, utom den sista
//   LcOpNwdFragments           fragmenten i lövordning, med blandade poseklasser
//   LcOpNwdLogicalHierarchy    ett träd av grupper (53) och löv (22) med
//                              transformer (40, 41, 39) och en flik (86) per löv
//   LcOaPartitionProps         (4, b) prefix 0: en ström med rötterna (86 och 66)
//                              som lövens tomma attributplatser tar
//   LcOpGuidStore              ett GUID per löv
//   LcOpNwdPartition           filens partition med enhetskoden
//   LcOpNwdSpatialHierarchy    ett rumsligt träd över fragmenten (om det begärs)
//
// Posterna är trianglar (klass 94: en strip med normaler ur axelkoder) och
// linjer (klass 95: en polylinje). Fragmentens poser är sammansatta ur
// trädets transformer längs vägen till lövet och postens mittpunkt c, så att
// sambandet i pose_check.h håller. Trädet skriver klass 39 med de nio talen
// radvis och poseklass 17 kolumnvis; ingen av ordningarna är belagd mot en
// riktig fil, men de är desamma som nwdcheck prövar först.

#pragma once

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "inflate.h"
#include "partition.h"
#include "path_utf8.h"
#include "pose.h"

namespace synth {

namespace hd = HNwd::detail;

struct Options {
    uint64_t fragments = 0;
    uint64_t records = 0;
    uint64_t nodes = 0;      // lagrade noder i trädet: grupper och löv
    bool spatial = true;     // skriv LcOpNwdSpatialHierarchy
    int32_t unit = 2;        // enhetskoden i LcOpNwdPartition: 0, 1, 2 eller 3
    uint64_t seed = 1;
    // Postens storlek: diagonalen på postens låda i dess egna koordinater, i
    // meter. 0 och 0 ger en post med längden 0,5 till 5 m och bredden 0,1 till
    // 1 m, som postens frö bestämmer; annars ska 0 < min_size <= max_size, och
    // varje posts diagonal är då log-likformigt fördelad mellan dem.
    double min_size = 0, max_size = 0;
    uint32_t max_segments = 12;   // stripens längsta längd: 1 till max_segments segment per post
};

struct PackageInfo {
    std::string name;
    uint64_t offset = 0, length = 0;
};

struct Result {
    uint64_t file_bytes = 0;
    uint64_t leaves = 0, groups = 0, empty_groups = 0;
    uint64_t triangle_records = 0, line_records = 0;
    uint64_t chunks = 0;
    uint64_t geometry_bytes = 0;     // posterna uppackade
    uint64_t property_roots = 0;
    uint64_t poses[5] = {};          // null, 14, 15, 16, 17
    std::vector<PackageInfo> packages;
};

inline constexpr char kDataHeader[] = "#LcUStream-V1.1 binary double release C lichunk-008\n";
inline constexpr char kDirHeader[] = "#LcUStream-V1.1 binary double release utf8 navisworks:448 lizlib\n";
inline constexpr uint32_t kChunkBytes = 65536;
inline constexpr uint32_t kGroupFanout = 16;     // barn per grupp i trädet
inline constexpr uint32_t kSpatialLeaf = 32;     // fragment per löv i det rumsliga trädet
inline constexpr uint32_t kSpatialFanout = 7;
inline constexpr uint32_t kLooks = 32;
inline constexpr uint32_t kMaxSegments = 1'000'000;   // segment per post som högst går att begära

// ---------------------------------------------------------------------------
// Slumptal ur fröet.

inline uint64_t mix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

inline uint64_t hash(uint64_t seed, uint64_t kind, uint64_t index, uint64_t salt = 0) {
    return mix(mix(mix(seed ^ (kind << 56)) ^ index) ^ salt);
}

// Ett tal i [0, 1).
inline double unit_random(uint64_t h) { return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0); }

// Ett tal i [-1, 1).
inline double signed_random(uint64_t h) { return 2 * unit_random(h) - 1; }

// ---------------------------------------------------------------------------
// Utfilen.

// En utfil med en egen buffert. Ett skrivfel kommer ihåg sig själv.
class FileSink {
public:
    FileSink() = default;
    FileSink(const FileSink&) = delete;
    FileSink& operator=(const FileSink&) = delete;
    ~FileSink() {
        if (file_) std::fclose(file_);
    }

    bool open(const std::string& path) {
#ifdef _WIN32
        const std::wstring wide = hd::utf8_to_wide(path);
        file_ = wide.empty() ? nullptr : _wfopen(wide.c_str(), L"wb");
#else
        file_ = std::fopen(path.c_str(), "wb");
#endif
        if (!file_) {
            open_errno_ = errno;
            return false;
        }
        buf_.reserve(kBuffer);
        return true;
    }

    void write(const void* data, size_t n) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        pos_ += n;
        while (n > 0) {
            const size_t room = kBuffer - buf_.size();
            const size_t take = n < room ? n : room;
            buf_.insert(buf_.end(), p, p + take);
            p += take;
            n -= take;
            if (buf_.size() == kBuffer) flush();
        }
    }
    void u8(uint8_t v) { write(&v, 1); }
    void i32(int32_t v) {
        uint8_t b[4];
        for (int i = 0; i < 4; ++i) b[i] = static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * i));
        write(b, 4);
    }
    void i64(int64_t v) {
        uint8_t b[8];
        for (int i = 0; i < 8; ++i) b[i] = static_cast<uint8_t>(static_cast<uint64_t>(v) >> (8 * i));
        write(b, 8);
    }

    // Tömmer och stänger. Sant bara om varje skrivning och stängningen lyckades.
    bool finish() {
        flush();
        if (!file_) return false;
        if (std::ferror(file_)) failed_ = true;
        if (std::fclose(file_) != 0) failed_ = true;
        file_ = nullptr;
        return !failed_;
    }

    uint64_t pos() const { return pos_; }
    bool failed() const { return failed_; }
    int open_errno() const { return open_errno_; }

private:
    static constexpr size_t kBuffer = size_t(8) << 20;

    void flush() {
        if (file_ && !failed_ && !buf_.empty() &&
            std::fwrite(buf_.data(), 1, buf_.size(), file_) != buf_.size())
            failed_ = true;
        buf_.clear();
    }

    FILE* file_ = nullptr;
    std::vector<uint8_t> buf_;
    uint64_t pos_ = 0;
    bool failed_ = false;
    int open_errno_ = 0;
};

// En zlib-ström med lagrade block, skriven medan datan kommer. Ett block
// skrivs först när nästa byte finns, så att det sista blocket får sin
// slutflagga.
class StoredZlib {
public:
    explicit StoredZlib(FileSink& out) : out_(out), start_(out.pos()) {
        pending_.reserve(kBlock);
        const uint8_t head[2] = {0x78, 0x01};
        out_.write(head, 2);
    }

    void write(const void* data, size_t n) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        raw_ += n;
        while (n > 0) {
            if (pending_.size() == kBlock) emit(false);
            const size_t room = kBlock - pending_.size();
            const size_t take = n < room ? n : room;
            pending_.insert(pending_.end(), p, p + take);
            p += take;
            n -= take;
        }
    }

    // Det sista blocket och Adler-32. Ger strömmens längd i filen.
    uint64_t finish() {
        emit(true);
        const uint8_t sum[4] = {static_cast<uint8_t>(adler_ >> 24), static_cast<uint8_t>(adler_ >> 16),
                                static_cast<uint8_t>(adler_ >> 8), static_cast<uint8_t>(adler_)};
        out_.write(sum, 4);
        return out_.pos() - start_;
    }

    uint64_t raw() const { return raw_; }

private:
    static constexpr size_t kBlock = 65535;

    void emit(bool last) {
        const size_t n = pending_.size();
        const uint8_t head[5] = {static_cast<uint8_t>(last ? 1 : 0), static_cast<uint8_t>(n),
                                 static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(~n),
                                 static_cast<uint8_t>(~n >> 8)};
        out_.write(head, 5);
        if (n) {
            out_.write(pending_.data(), n);
            adler_ = hd::adler32(pending_.data(), n, adler_);
        }
        pending_.clear();
    }

    FileSink& out_;
    uint64_t start_;
    uint64_t raw_ = 0;
    uint32_t adler_ = 1;
    std::vector<uint8_t> pending_;
};

// Värden i en ström så som formatet lagrar dem, med positionen räknad från
// strömmens början (för PAD8) och id för objektströmmar.
template <class Out>
class Writer {
public:
    explicit Writer(Out& out) : out_(out) {}

    uint64_t pos() const { return pos_; }

    void raw(const void* p, size_t n) {
        out_.write(p, n);
        pos_ += n;
    }
    void i32(int32_t v) {
        uint8_t b[4];
        for (int i = 0; i < 4; ++i) b[i] = static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * i));
        raw(b, 4);
    }
    void f32(float v) {
        uint32_t u;
        std::memcpy(&u, &v, 4);
        i32(static_cast<int32_t>(u));
    }
    // 64-bitarsvärden på jämna 8 byte: en int32 0 först om positionen är 4 mod 8.
    void i64a(int64_t v) {
        if (pos_ % 8 == 4) i32(0);
        uint8_t b[8];
        for (int i = 0; i < 8; ++i) b[i] = static_cast<uint8_t>(static_cast<uint64_t>(v) >> (8 * i));
        raw(b, 8);
    }
    void f64(double v) {
        int64_t u;
        std::memcpy(&u, &v, 8);
        i64a(u);
    }
    void pad(size_t align) {
        static const uint8_t zeros[8] = {};
        const size_t n = (align - pos_ % align) % align;
        if (n) raw(zeros, n);
    }
    void str(const std::string& s) {
        i32(static_cast<int32_t>(s.size()));
        raw(s.data(), s.size());
        pad(4);
    }
    void null_str() { i32(-1); }

    int32_t obj(int32_t cls) {
        i32(next_id_);
        i32(cls);
        return next_id_++;
    }
    int32_t implicit_root() {
        i32(next_id_);
        return next_id_++;
    }
    void ref(int32_t id) {
        i32(1);
        i32(id);
    }
    void null_ptr() { i32(0); }

private:
    Out& out_;
    uint64_t pos_ = 0;
    int32_t next_id_ = 100;
};

// En buffert som Writer kan skriva i.
struct ByteBuffer {
    std::vector<uint8_t> data;
    void write(const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        data.insert(data.end(), b, b + n);
    }
};

// ---------------------------------------------------------------------------
// Geometriposterna.

// En post, så som den ritas: positionerna och lådan kring dem.
struct RecordShape {
    bool lines = false;
    uint32_t vertices = 0;
    uint32_t primitives = 0;
    std::vector<float> positions;   // xyz per vertex
    float bbox[6] = {};
};

// Post r (1-baserad) ur fröet, med längderna i filens enhet: `per_metre`
// enheter per meter. Med Options::max_size > 0 görs postens mått först med
// längden 1 och skalas sedan så att lådans diagonal blir den dragna storleken.
inline void record_shape(const Options& o, uint64_t r, double per_metre, RecordShape& out) {
    const uint64_t seed = o.seed;
    const uint64_t h = hash(seed, 1, r);
    out.lines = (h % 100) < 15;
    const uint32_t segments = 1 + static_cast<uint32_t>((h >> 8) % o.max_segments);
    const bool sized = o.max_size > 0;
    double length = (0.5 + 4.5 * unit_random(hash(seed, 2, r))) * per_metre;
    double width = (0.1 + 0.9 * unit_random(hash(seed, 3, r))) * per_metre;
    double bump = 0.05 * per_metre;   // trianglarnas höjdskillnad längs stripen
    if (sized) {
        length = 1;
        width = 0.05 + 0.45 * unit_random(hash(seed, 3, r));
        bump = 0.02;
    }
    out.positions.clear();
    if (out.lines) {
        out.vertices = segments + 1;
        out.primitives = segments;
        for (uint32_t i = 0; i <= segments; ++i) {
            out.positions.push_back(static_cast<float>(-length / 2 + length * i / segments));
            out.positions.push_back(static_cast<float>(width / 2 * std::sin(static_cast<double>(i))));
            out.positions.push_back(0.0f);
        }
    } else {
        out.vertices = 2 * (segments + 1);
        out.primitives = 2 * segments;
        for (uint32_t i = 0; i <= segments; ++i) {
            const float x = static_cast<float>(-length / 2 + length * i / segments);
            const float z = static_cast<float>(bump * std::sin(static_cast<double>(i)));
            for (const double y : {-width / 2, width / 2}) {
                out.positions.push_back(x);
                out.positions.push_back(static_cast<float>(y));
                out.positions.push_back(z);
            }
        }
    }
    if (sized) {
        // Storleken är log-likformig mellan min_size och max_size meter.
        const double lo = std::log(o.min_size), hi = std::log(o.max_size);
        const double size = std::exp(lo + (hi - lo) * unit_random(hash(seed, 4, r)));
        double lo_corner[3] = {out.positions[0], out.positions[1], out.positions[2]};
        double hi_corner[3] = {lo_corner[0], lo_corner[1], lo_corner[2]};
        for (size_t v = 0; v < out.positions.size(); v += 3)
            for (size_t k = 0; k < 3; ++k) {
                lo_corner[k] = std::min<double>(lo_corner[k], out.positions[v + k]);
                hi_corner[k] = std::max<double>(hi_corner[k], out.positions[v + k]);
            }
        double diagonal = 0;
        for (size_t k = 0; k < 3; ++k) diagonal += (hi_corner[k] - lo_corner[k]) * (hi_corner[k] - lo_corner[k]);
        const double scale = size * per_metre / std::sqrt(diagonal);
        for (float& p : out.positions) p = static_cast<float>(static_cast<double>(p) * scale);
    }
    for (int k = 0; k < 3; ++k) {
        out.bbox[k] = out.positions[static_cast<size_t>(k)];
        out.bbox[3 + k] = out.positions[static_cast<size_t>(k)];
    }
    for (size_t v = 0; v < out.positions.size(); v += 3) {
        for (size_t k = 0; k < 3; ++k) {
            out.bbox[k] = std::min(out.bbox[k], out.positions[v + k]);
            out.bbox[3 + k] = std::max(out.bbox[3 + k], out.positions[v + k]);
        }
    }
}

// Postens byte (se geometry.h). Trianglarna är en strip med normalen +Z ur
// axelkoden D4 för varje vertex; linjerna en polylinje utan färg.
inline void record_bytes(const RecordShape& s, std::vector<uint8_t>& out) {
    ByteBuffer b;
    Writer<ByteBuffer> w(b);
    const int32_t p1 = static_cast<int32_t>(s.vertices);
    w.i32(100);
    w.i32(s.lines ? 95 : 94);
    w.i32(101);
    w.i32(60);
    w.i32(p1);
    w.i32(3 * p1);
    for (const float f : s.positions) w.f32(f);
    w.i32(0);   // inga UV
    if (s.lines) {
        w.i32(0);   // inga normaler
    } else {
        w.i32(102);
        w.i32(100);
        w.i32(-p1);
        w.i32(0);   // ingen palett: varje normal är en axelkod
        const uint8_t axis = 0xD4;
        for (int32_t i = 0; i < p1; ++i) w.raw(&axis, 1);
        w.pad(4);
    }
    w.i32(0);   // ingen färg
    // Stripens längd p1 står med bredden bitar: minst fem, och fler när p1
    // inte ryms.
    uint32_t width = 5;
    while (width < 31 && (static_cast<uint64_t>(p1) >> width) != 0) ++width;
    w.i32(1);
    w.i32(static_cast<int32_t>(width));
    w.i32(1);
    w.i32(static_cast<int32_t>(static_cast<uint32_t>(p1) << (32 - width)));
    if (!s.lines) w.i32(0);   // S = 0: hörnen i ordning
    w.pad(8);
    out = std::move(b.data);
}

// ---------------------------------------------------------------------------
// Trädet.

// Grupper i ett delträd med `n` löv: gruppen själv, och om n > kGroupFanout
// grupper över delar av lika storlek.
class GroupCounter {
public:
    uint64_t groups(uint64_t n) {
        if (n <= kGroupFanout) return 1;
        const auto found = memo_.find(n);
        if (found != memo_.end()) return found->second;
        const uint64_t k = parts(n);
        const uint64_t small = n / k, large_count = n % k;
        const uint64_t g = 1 + large_count * groups(small + 1) + (k - large_count) * groups(small);
        memo_[n] = g;
        return g;
    }
    static uint64_t parts(uint64_t n) {
        const uint64_t k = (n + kGroupFanout - 1) / kGroupFanout;
        return k < kGroupFanout ? k : kGroupFanout;
    }

private:
    std::unordered_map<uint64_t, uint64_t> memo_;
};

// En nods transform, ur fröet och nodens plats.
struct NodeTransform {
    int32_t cls = 0;              // 0, 40, 41 eller 39
    bool uniform = true;          // klass 39: enhetlig skala
    double t[3] = {0, 0, 0};
    double q[4] = {0, 0, 0, 1};   // klass 41, och klass 39 med enhetlig skala
    double s = 1;                 // klass 39 med enhetlig skala
    double m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};   // klass 39, radvis
};

inline void random_quaternion(uint64_t h, double q[4]) {
    double n = 0;
    for (int k = 0; k < 4; ++k) {
        q[k] = signed_random(mix(h + static_cast<uint64_t>(k)));
        n += q[k] * q[k];
    }
    n = std::sqrt(n);
    if (n < 1e-3) {
        q[0] = q[1] = q[2] = 0;
        q[3] = 1;
        return;
    }
    for (int k = 0; k < 4; ++k) q[k] /= n;
}

// Planen: hur många löv och grupper trädet har.
struct Plan {
    Options o;
    double per_metre = 1000;   // enheter per meter
    uint64_t leaves = 0;
    uint64_t empty_groups = 0;  // tomma grupper sist under toppnoden, så att antalet noder blir det begärda
};

// Gruppens transform. Djup 1 är toppnoden. Under den flyttar hälften av
// grupperna till en plats i miljonklassen; djupare grupper flyttar ibland
// lite. Grupper roterar aldrig, så poseklassen följer av lövet.
inline NodeTransform group_transform(const Plan& p, uint64_t seq, uint32_t depth) {
    NodeTransform t;
    const uint64_t h = hash(p.o.seed, 10, seq);
    if (depth == 2 && h % 2 == 0) {
        t.cls = 40;
        t.t[0] = (150000.0 + 1000.0 * static_cast<double>((h >> 8) % 64)) * p.per_metre;
        t.t[1] = (6500000.0 + 1000.0 * static_cast<double>((h >> 16) % 64)) * p.per_metre;
        t.t[2] = 10.0 * p.per_metre;
    } else if (depth > 2 && h % 8 == 1) {
        t.cls = 40;
        for (int k = 0; k < 3; ++k) t.t[k] = 50.0 * signed_random(mix(h + 11 + static_cast<uint64_t>(k))) * p.per_metre;
    }
    return t;
}

// Lövets transform: ingen, flytt, flytt och rotation, eller en matris med
// enhetlig eller olika skala per axel. Flytten lägger löven i ett rutnät.
inline NodeTransform leaf_transform(const Plan& p, uint64_t seq, uint64_t ordinal) {
    NodeTransform t;
    const uint64_t h = hash(p.o.seed, 11, seq);
    const double u = unit_random(h);
    if (u < 0.15) return t;
    t.t[0] = 1.5 * static_cast<double>(ordinal % 2048) * p.per_metre;
    t.t[1] = 1.5 * static_cast<double>((ordinal / 2048) % 2048) * p.per_metre;
    t.t[2] = 3.0 * static_cast<double>(ordinal / (2048 * 2048)) * p.per_metre;
    if (u < 0.45) {
        t.cls = 40;
        return t;
    }
    random_quaternion(mix(h ^ 0x51), t.q);
    if (u < 0.75) {
        t.cls = 41;
        return t;
    }
    t.cls = 39;
    if (u < 0.90) {
        t.uniform = true;
        t.s = 0.5 + 1.5 * unit_random(mix(h ^ 0x52));
        hd::PoseD pose;
        for (int k = 0; k < 4; ++k) pose.q[k] = t.q[k];
        pose.s = t.s;
        hd::pose_linear(pose, t.m);
        return t;
    }
    // Olika skala per axel: R(q) · diag(sx, sy, sz), avrundat till f32 så att
    // poseklass 17, som lagrar f32, får exakt samma matris.
    t.uniform = false;
    hd::PoseD pose;
    for (int k = 0; k < 4; ++k) pose.q[k] = t.q[k];
    double r[9];
    hd::pose_linear(pose, r);
    double scale[3];
    for (int k = 0; k < 3; ++k) scale[k] = 0.5 + 1.5 * unit_random(mix(h ^ (0x60 + static_cast<uint64_t>(k))));
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            t.m[3 * row + col] = static_cast<double>(static_cast<float>(r[3 * row + col] * scale[col]));
    return t;
}

// Går igenom trädet i preorder och anropar besökaren:
//   group(seq, depth, child_count, transform), sedan barnen, sedan group_end()
//   leaf(seq, ordinal, transform)
// Djupet är högst log16 av antalet löv plus två, så rekursionen är grund.
template <class Visitor>
void walk_range(const Plan& p, GroupCounter& gc, uint64_t first_leaf, uint64_t n, uint32_t depth,
                uint64_t extra, uint64_t& seq, Visitor& v) {
    const uint64_t group_seq = seq++;
    const bool leaves_below = n <= kGroupFanout;
    const uint64_t k = leaves_below ? n : GroupCounter::parts(n);
    v.group(group_seq, depth, k + extra, group_transform(p, group_seq, depth));
    if (leaves_below) {
        for (uint64_t i = 0; i < n; ++i) {
            const uint64_t leaf_seq = seq++;
            v.leaf(leaf_seq, first_leaf + i, leaf_transform(p, leaf_seq, first_leaf + i));
        }
    } else {
        const uint64_t small = n / k, large_count = n % k;
        uint64_t at = first_leaf;
        for (uint64_t i = 0; i < k; ++i) {
            const uint64_t part = small + (i < large_count ? 1 : 0);
            walk_range(p, gc, at, part, depth + 1, 0, seq, v);
            at += part;
        }
    }
    for (uint64_t i = 0; i < extra; ++i) {
        const uint64_t empty_seq = seq++;
        v.group(empty_seq, depth + 1, 0, NodeTransform{});
        v.group_end();
    }
    v.group_end();
}

template <class Visitor>
void walk(const Plan& p, Visitor& v) {
    GroupCounter gc;
    uint64_t seq = 1;
    walk_range(p, gc, 0, p.leaves, 1, p.empty_groups, seq, v);
}

// Antalet fragment i löv nummer `ordinal`: fragmenten fördelade jämnt.
inline uint64_t leaf_fragment_count(const Plan& p, uint64_t ordinal) {
    const uint64_t f = p.o.fragments, l = p.leaves;
    return (ordinal + 1) * f / l - ordinal * f / l;
}

// ---------------------------------------------------------------------------
// Paketen.

// Ett transformattribut: HUVUD (null-namn, inget namnpar, flaggor 0) och
// kroppen.
template <class W>
void write_transform(W& w, const NodeTransform& t) {
    w.obj(t.cls);
    w.null_str();
    w.null_ptr();
    w.i32(0);
    if (t.cls == 39)
        for (const double v : t.m) w.f64(v);
    for (const double v : t.t) w.f64(v);
    if (t.cls == 41)
        for (const double v : t.q) w.f64(v);
    if (t.cls == 39) w.i32(0);
}

// Ett namnpar: nytt första gången, sedan en bakåtpekare.
template <class W>
void name_pair(W& w, int32_t& id, const char* display, const char* internal) {
    if (id) {
        w.ref(id);
        return;
    }
    id = w.obj(52);
    w.str(display);
    w.str(internal);
}

// Löv med en tom attributplats, som tar en rot ur LcOaPartitionProps.
inline bool leaf_has_root(uint64_t ordinal) { return ordinal % 4 == 0; }

// Trädets ström.
template <class W>
struct TreeVisitor {
    TreeVisitor(W& writer, const Plan& plan) : w(writer), p(plan) {}

    W& w;
    const Plan& p;
    int32_t group_pair = 0, leaf_pair = 0, tab_pair = 0, key_name = 0, key_number = 0, key_length = 0;
    uint64_t groups = 0;

    void group(uint64_t seq, uint32_t depth, uint64_t children, const NodeTransform& t) {
        (void)depth;
        ++groups;
        w.obj(53);
        w.str("Group " + std::to_string(seq));
        name_pair(w, group_pair, "Group", "LcOaNode");
        w.i32(0);
        w.i32(0);
        w.i32(t.cls ? 1 : 0);
        if (t.cls) write_transform(w, t);
        w.i32(static_cast<int32_t>(children));
    }
    void group_end() {}
    void leaf(uint64_t seq, uint64_t ordinal, const NodeTransform& t) {
        (void)seq;
        w.obj(22);
        w.str("Part " + std::to_string(ordinal));
        name_pair(w, leaf_pair, "Geometry", "LcOaGeometry");
        w.i32(0);
        w.i32(static_cast<int32_t>(ordinal + 1));   // elementet: GUID nummer ordinal
        const bool root = leaf_has_root(ordinal);
        w.i32((t.cls ? 1 : 0) + 1 + (root ? 1 : 0));
        if (t.cls) write_transform(w, t);
        // En flik med tre egenskaper: text, heltal och flyttal.
        w.obj(86);
        w.str("Item");
        name_pair(w, tab_pair, "Item", "LcOaItem");
        w.i32(0);
        w.i32(3);
        name_pair(w, key_name, "Name", "LcOaName");
        w.i32(4);
        w.str("Part " + std::to_string(ordinal));
        name_pair(w, key_number, "Number", "LcOaNumber");
        w.i32(2);
        w.i32(static_cast<int32_t>(ordinal % 1000000));
        name_pair(w, key_length, "Length", "LcOaLength");
        w.i32(1);
        w.f64(0.25 * static_cast<double>(ordinal % 1000));
        if (root) w.null_ptr();
    }
};

// Fragmentens ström.
template <class W>
struct FragmentVisitor {
    FragmentVisitor(W& writer, const Plan& plan, std::vector<int32_t>& ids, Result& r)
        : w(writer), p(plan), geom_ids(ids), result(r) {}

    W& w;
    const Plan& p;
    std::vector<int32_t>& geom_ids;   // per post: klass-93-objektets id, 0 före första
    Result& result;
    int32_t look_ids[kLooks] = {};
    uint64_t next_fragment = 0;
    RecordShape shape;
    // Gruppernas flyttar längs vägen; grupper roterar inte.
    std::vector<double> stack_t;
    std::vector<int> stack_any;

    void group(uint64_t, uint32_t, uint64_t, const NodeTransform& t) {
        double base[3] = {0, 0, 0};
        int any = 0;
        if (!stack_any.empty()) {
            for (int k = 0; k < 3; ++k) base[k] = stack_t[stack_t.size() - 3 + static_cast<size_t>(k)];
            any = stack_any.back();
        }
        for (int k = 0; k < 3; ++k) stack_t.push_back(base[k] + (t.cls ? t.t[k] : 0.0));
        stack_any.push_back(any || t.cls != 0);
    }
    void group_end() {
        stack_t.resize(stack_t.size() - 3);
        stack_any.pop_back();
    }

    void leaf(uint64_t seq, uint64_t ordinal, const NodeTransform& t) {
        const uint64_t count = leaf_fragment_count(p, ordinal);
        const double* g = stack_t.data() + stack_t.size() - 3;
        const bool any = stack_any.back() || t.cls != 0;
        // Lövets linjära del radvis.
        double l[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        if (t.cls == 41) {
            hd::PoseD pose;
            for (int k = 0; k < 4; ++k) pose.q[k] = t.q[k];
            hd::pose_linear(pose, l);
        } else if (t.cls == 39) {
            for (int k = 0; k < 9; ++k) l[k] = t.m[k];
        }
        for (uint64_t i = 0; i < count; ++i) fragment(seq, t, l, g, any);
    }

    void fragment(uint64_t seq, const NodeTransform& t, const double l[9], const double g[3], bool any) {
        const uint64_t index = next_fragment++;
        const uint64_t seed = p.o.seed;
        const uint64_t record = index < p.o.records ? index + 1 : 1 + hash(seed, 20, index) % p.o.records;
        record_shape(p.o, record, p.per_metre, shape);
        double c[3];
        for (int k = 0; k < 3; ++k)
            c[k] = 2.0 * signed_random(hash(seed, 21, record, static_cast<uint64_t>(k))) * p.per_metre;

        w.obj(13);
        w.f64(1.5);
        w.i32(static_cast<int32_t>(shape.primitives));
        w.i32(4);
        w.i32(7);
        w.i32(static_cast<int32_t>(seq));

        // Posen: trädets transformer tillämpade på c.
        double pt[3];
        for (int k = 0; k < 3; ++k)
            pt[k] = l[3 * k] * c[0] + l[3 * k + 1] * c[1] + l[3 * k + 2] * c[2] + (t.cls ? t.t[k] : 0.0) + g[k];
        if (!any) {
            w.null_ptr();
            ++result.poses[0];
        } else if (t.cls == 0 || t.cls == 40) {
            w.obj(14);
            for (const double v : pt) w.f64(v);
            ++result.poses[1];
        } else if (t.cls == 41) {
            w.obj(15);
            for (const double v : pt) w.f64(v);
            for (const double v : t.q) w.f64(v);
            ++result.poses[2];
        } else if (t.uniform) {
            w.obj(16);
            for (const double v : pt) w.f64(v);
            for (const double v : t.q) w.f64(v);
            w.f64(t.s);
            ++result.poses[3];
        } else {
            w.obj(17);
            for (int col = 0; col < 3; ++col) {
                for (int row = 0; row < 3; ++row) w.f32(static_cast<float>(t.m[3 * row + col]));
                w.f32(0.0f);
            }
            w.f32(1.0f);
            for (const double v : pt) w.f64(v);
            ++result.poses[4];
        }

        // Utseendet: ett av kLooks, nytt första gången.
        const uint32_t look = static_cast<uint32_t>(hash(seed, 22, index) % kLooks);
        if (look_ids[look]) {
            w.ref(look_ids[look]);
        } else {
            look_ids[look] = w.obj(55);
            const int32_t flags[4] = {1, 17, 65, 33};
            const int32_t f = flags[look % 4];
            w.i32(f);
            w.obj(54);
            const float d = 0.2f + 0.6f * static_cast<float>(look) / kLooks;
            const float values[14] = {0.1f, 0.1f, 0.1f, d, 1 - d, 0.5f, 0.2f, 0.2f, 0.2f, 0, 0, 0, 0.25f,
                                      look % 8 == 7 ? 0.5f : 0.0f};
            for (const float v : values) w.f32(v);
            if (f & 32) w.null_ptr();
            if (f & 64) w.i32(static_cast<int32_t>((look / 4) % 2));
        }

        // Geometrireferensen: en per post, delad genom bakåtpekare.
        int32_t& id = geom_ids[static_cast<size_t>(record)];
        if (id) {
            w.ref(id);
        } else {
            id = w.obj(93);
            for (const float v : shape.bbox) w.f32(v);
            for (const double v : c) w.f64(v);
            w.f32(2.5f);
            w.i32(7);
            w.i32(static_cast<int32_t>(record));
        }
    }
};

// Det rumsliga trädet över fragmenten i ordning: löv om högst kSpatialLeaf,
// inre noder med upp till kSpatialFanout barn. Ett löv med ett fragment får
// taggen 4.
template <class W>
void write_spatial(W& w, uint64_t first, uint64_t n) {
    if (n <= kSpatialLeaf) {
        if (n == 1) {
            w.i32(4);
            w.i32(static_cast<int32_t>(first));
            return;
        }
        w.i32(2);
        w.i32(static_cast<int32_t>(n));
        for (uint64_t i = 0; i < n; ++i) {
            w.i32(1);
            w.i32(static_cast<int32_t>(first + i));
        }
        return;
    }
    uint64_t k = (n + kSpatialLeaf - 1) / kSpatialLeaf;
    if (k > kSpatialFanout) k = kSpatialFanout;
    w.i32(5);
    w.i32(static_cast<int32_t>(k));
    const uint64_t small = n / k, large_count = n % k;
    uint64_t at = first;
    for (uint64_t i = 0; i < k; ++i) {
        const uint64_t part = small + (i < large_count ? 1 : 0);
        write_spatial(w, at, part);
        at += part;
    }
}

// Skriver filen. false och `why` vid fel; en halvskriven fil tas bort.
inline bool write(const std::string& path, const Options& o, Result& result, std::string& why) {
    result = Result{};
    Plan p;
    p.o = o;
    double metres = 0;
    if (!hd::metres_per_unit(o.unit, metres)) {
        why = "the unit code " + std::to_string(o.unit) + " is not one of 0, 1, 2 and 3";
        return false;
    }
    p.per_metre = 1.0 / metres;
    if (o.nodes < 2) {
        why = "the tree needs at least 2 nodes: a group and a leaf";
        return false;
    }
    if (o.fragments == 0 || o.records == 0) {
        why = "the file needs at least one fragment and one record";
        return false;
    }
    if (o.fragments > 0x7FFFFFFF || o.records > 0x7FFFFFFF || o.nodes > 0x7FFFFFF0) {
        why = "a count is larger than a 32-bit index in the file allows";
        return false;
    }
    if (o.max_segments < 1 || o.max_segments > kMaxSegments) {
        why = "max_segments is not in 1.." + std::to_string(kMaxSegments);
        return false;
    }
    if ((o.min_size != 0 || o.max_size != 0) &&
        !(std::isfinite(o.min_size) && std::isfinite(o.max_size) && o.min_size > 0 &&
          o.min_size <= o.max_size)) {
        why = "the sizes need 0 < min_size <= max_size, or both 0";
        return false;
    }

    // Så många löv som ryms med sina grupper; resten blir tomma grupper.
    {
        GroupCounter gc;
        uint64_t lo = 1, hi = o.nodes;
        while (lo < hi) {
            const uint64_t mid = lo + (hi - lo + 1) / 2;
            if (mid + gc.groups(mid) <= o.nodes)
                lo = mid;
            else
                hi = mid - 1;
        }
        if (lo + gc.groups(lo) > o.nodes) {
            why = "the tree cannot be laid out with " + std::to_string(o.nodes) + " nodes";
            return false;
        }
        p.leaves = lo;
        p.empty_groups = o.nodes - lo - gc.groups(lo);
        result.leaves = p.leaves;
        result.groups = gc.groups(lo) + p.empty_groups;
        result.empty_groups = p.empty_groups;
    }

    FileSink out;
    if (!out.open(path)) {
        why = "cannot open " + path + " for writing: " + std::strerror(out.open_errno());
        return false;
    }
    const auto fail = [&](const std::string& text) {
        out.finish();
        hd::remove_utf8(path);
        why = text;
        return false;
    };

    struct Entry {
        std::string name;
        uint64_t offset, length;
        int32_t a, b;
    };
    std::vector<Entry> entries;
    out.write(kDataHeader, sizeof kDataHeader - 1);

    // Geometrin: posterna i chunkar om exakt kChunkBytes, utom den sista.
    {
        const uint64_t offset = out.pos();
        out.i32(static_cast<int32_t>(kChunkBytes));
        std::vector<uint32_t> chunk_sizes, record_lengths;
        record_lengths.reserve(static_cast<size_t>(o.records));
        std::vector<uint8_t> chunk, bytes;
        chunk.reserve(kChunkBytes);
        RecordShape shape;
        const auto flush_chunk = [&]() {
            StoredZlib z(out);
            z.write(chunk.data(), chunk.size());
            chunk_sizes.push_back(static_cast<uint32_t>(z.finish()));
            chunk.clear();
        };
        for (uint64_t r = 1; r <= o.records; ++r) {
            record_shape(o, r, p.per_metre, shape);
            record_bytes(shape, bytes);
            if (shape.lines) ++result.line_records; else ++result.triangle_records;
            record_lengths.push_back(static_cast<uint32_t>(bytes.size()));
            result.geometry_bytes += bytes.size();
            size_t at = 0;
            while (at < bytes.size()) {
                const size_t take = std::min(bytes.size() - at, size_t(kChunkBytes) - chunk.size());
                chunk.insert(chunk.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at),
                             bytes.begin() + static_cast<std::ptrdiff_t>(at + take));
                at += take;
                if (chunk.size() == kChunkBytes) flush_chunk();
            }
        }
        if (!chunk.empty()) flush_chunk();
        if (chunk_sizes.size() > 0x7FFFFFFF) return fail("too many chunks");
        result.chunks = chunk_sizes.size();
        StoredZlib z(out);
        Writer<StoredZlib> toc(z);
        toc.i32(static_cast<int32_t>(o.records));
        toc.i32(static_cast<int32_t>(chunk_sizes.size()));
        for (const uint32_t s : chunk_sizes) toc.i32(static_cast<int32_t>(s));
        for (const uint32_t s : record_lengths) toc.i32(static_cast<int32_t>(s));
        toc.pad(8);
        const uint64_t b = z.finish();
        entries.push_back({"LcOpNwdGeometry", offset, out.pos() - offset, 4, static_cast<int32_t>(b)});
    }

    // Fragmenten, i lövordning.
    {
        const uint64_t offset = out.pos();
        StoredZlib z(out);
        Writer<StoredZlib> w(z);
        w.i32(static_cast<int32_t>(o.fragments));
        std::vector<int32_t> geom_ids(static_cast<size_t>(o.records) + 1, 0);
        FragmentVisitor<Writer<StoredZlib>> v(w, p, geom_ids, result);
        walk(p, v);
        if (v.next_fragment != o.fragments) return fail("internal error: the leaves did not take every fragment");
        w.pad(8);
        z.finish();
        entries.push_back({"LcOpNwdFragments", offset, out.pos() - offset, 0, 0});
    }

    // Trädet.
    const uint64_t roots = (p.leaves + 3) / 4;
    result.property_roots = roots;
    {
        const uint64_t offset = out.pos();
        StoredZlib z(out);
        Writer<StoredZlib> w(z);
        w.i32(1);
        w.i32(static_cast<int32_t>(roots));
        w.implicit_root();
        w.i32(1);   // en toppnod
        TreeVisitor<Writer<StoredZlib>> v(w, p);
        walk(p, v);
        w.i32(1);
        w.pad(8);
        z.finish();
        entries.push_back({"LcOpNwdLogicalHierarchy", offset, out.pos() - offset, 0, 0});
    }

    // Egenskapsrötterna: varannan en flik med ett heltal, varannan ett element-id.
    {
        const uint64_t offset = out.pos();
        out.i32(0);
        uint64_t stream_length = 0;
        {
            StoredZlib z(out);
            Writer<StoredZlib> w(z);
            int32_t tab_pair = 0, key_pair = 0, id_pair = 0;
            for (uint64_t j = 0; j < roots; ++j) {
                if (j % 2 == 0) {
                    w.obj(86);
                    w.null_str();
                    name_pair(w, tab_pair, "Element", "LcOaElement");
                    w.i32(0);
                    w.i32(1);
                    name_pair(w, key_pair, "Id", "LcOaId");
                    w.i32(2);
                    w.i32(static_cast<int32_t>(j % 0x7FFFFFFF));
                } else {
                    w.obj(66);
                    w.null_str();
                    name_pair(w, id_pair, "Element Id", "LcRevitId");
                    w.i32(0x01000000);
                    w.i64a(static_cast<int64_t>(1 + j % 0x7FFFFFFE));
                }
            }
            w.pad(8);
            stream_length = z.finish();
        }
        if (stream_length > 0x7FFFFFFF) return fail("the property stream is larger than its 32-bit length allows");
        StoredZlib z(out);
        Writer<StoredZlib> toc(z);
        toc.i32(1);
        toc.i32(static_cast<int32_t>(stream_length));
        toc.pad(8);
        const uint64_t b = z.finish();
        entries.push_back({"LcOaPartitionProps", offset, out.pos() - offset, 4, static_cast<int32_t>(b)});
    }

    // GUID-tabellen: ett per löv.
    {
        const uint64_t offset = out.pos();
        StoredZlib z(out);
        Writer<StoredZlib> w(z);
        w.i32(1);
        w.i32(static_cast<int32_t>(p.leaves));
        for (uint64_t j = 0; j < p.leaves; ++j) {
            const uint64_t a = hash(o.seed, 30, j), b = hash(o.seed, 31, j);
            uint8_t g[16];
            for (int k = 0; k < 8; ++k) {
                g[k] = static_cast<uint8_t>(a >> (8 * k));
                g[8 + k] = static_cast<uint8_t>(b >> (8 * k));
            }
            w.raw(g, 16);
        }
        z.finish();
        entries.push_back({"LcOpGuidStore", offset, out.pos() - offset, 0, 0});
    }

    // Filens partition med enhetskoden.
    {
        const uint64_t offset = out.pos();
        StoredZlib z(out);
        Writer<StoredZlib> w(z);
        w.obj(32);
        w.i32(0);
        w.str("synthetic.nwd");
        w.obj(52);
        w.str("File");
        w.str("LcOaPartition");
        w.i32(0);
        w.i32(0);
        w.i32(0);
        w.i32(2);
        w.i32(0);
        for (int i = 0; i < 3; ++i) w.f64(0);
        w.i32(0);
        w.str("nwdsynth");
        w.i32(448);
        w.i32(o.unit);
        w.i32(1);
        for (const double v : {0.0, 0.0, 1.0, 0.0, 0.0, 0.0}) w.f64(v);
        w.i32(0);
        w.null_str();
        w.i32(1);
        w.str("synthetic");
        for (const double v : {0.0, 0.0, 0.0, 1.0, 1.0, 0.0, 0.0}) w.f64(v);
        w.i32(1);
        w.pad(8);
        z.finish();
        entries.push_back({"LcOpNwdPartition", offset, out.pos() - offset, 0, 0});
    }

    // Det rumsliga trädet.
    if (o.spatial) {
        const uint64_t offset = out.pos();
        StoredZlib z(out);
        Writer<StoredZlib> w(z);
        write_spatial(w, 0, o.fragments);
        z.finish();
        entries.push_back({"LcOpNwdSpatialHierarchy", offset, out.pos() - offset, 0, 0});
    }

    // Katalogen och slutet.
    const uint64_t dir_section = out.pos();
    {
        out.write(kDirHeader, sizeof kDirHeader - 1);
        static const uint8_t zeros[7] = {};
        out.write(zeros, 7);
        ByteBuffer buf;
        Writer<ByteBuffer> d(buf);
        d.i32(-1);
        d.i32(static_cast<int32_t>(entries.size()));
        for (const Entry& e : entries) {
            d.str(e.name);
            d.i32(1);
            d.pad(8);
            d.i64a(static_cast<int64_t>(e.offset));
            d.i64a(static_cast<int64_t>(e.length));
            d.i32(e.a);
            d.i32(e.b);
        }
        StoredZlib z(out);
        z.write(buf.data.data(), buf.data.size());
        z.finish();
        out.i64(0);
        out.i64(static_cast<int64_t>(dir_section));
        out.write(kDataHeader, sizeof kDataHeader - 1);
    }
    for (const Entry& e : entries) result.packages.push_back({e.name, e.offset, e.length});
    result.file_bytes = out.pos();
    if (out.failed() || !out.finish()) {
        hd::remove_utf8(path);
        why = "writing " + path + " failed";
        return false;
    }
    return true;
}

} // namespace synth
