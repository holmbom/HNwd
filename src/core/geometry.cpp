#include "geometry.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace HNwd::detail {

namespace {

// Det första brottet mot grammatiken: var, vilken regel och vilken felkod.
struct Fault {
    ErrorCode code = ErrorCode::CorruptGeometry;
    size_t at = 0;
    std::string rule;
};

ErrorCode report(const Fault& f, const char* what, std::string* detail) {
    if (detail)
        *detail = std::string(what) + " at byte " + std::to_string(f.at) + ": " + f.rule;
    return f.code;
}

uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// En läsmarkör med gränskontroll. Varje läsning som misslyckas fyller i `fault`
// och svarar false; `pos` räknas från buffertens början.
struct Cursor {
    Cursor(const uint8_t* data, size_t size, size_t start, Fault& fault)
        : d(data), n(size), pos(start), f(fault) {}

    const uint8_t* d;
    size_t n;
    size_t pos;
    Fault& f;

    size_t left() const { return n - pos; }

    bool bad(size_t at, std::string rule, ErrorCode code = ErrorCode::CorruptGeometry) {
        f.code = code;
        f.at = at;
        f.rule = std::move(rule);
        return false;
    }

    bool i32(int32_t& v) {
        if (left() < 4) return bad(pos, "unexpected end");
        v = static_cast<int32_t>(le32(d + pos));
        pos += 4;
        return true;
    }

    // Läser ett heltal som ska vara exakt `want`.
    bool expect(int32_t want, const char* rule) {
        const size_t at = pos;
        int32_t v;
        if (!i32(v)) return false;
        if (v != want) return bad(at, rule);
        return true;
    }

    bool skip(uint64_t bytes, const char* rule) {
        if (bytes > left()) return bad(pos, rule);
        pos += static_cast<size_t>(bytes);
        return true;
    }

    // Nollor fram till nästa multipel av `align`, räknat från buffertens början.
    bool pad(size_t align) {
        while (pos % align != 0) {
            if (pos >= n) return bad(pos, "unexpected end");
            if (d[pos] != 0) return bad(pos, "pad byte is not zero");
            ++pos;
        }
        return true;
    }
};

// Axelvektorerna för axelkod -1 .. -6: +X, -X, +Y, -Y, +Z, -Z.
constexpr float kAxes[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};

// Ett index ur `out` räknat bakifrån: avståndet ska vara 1 .. antalet värden.
bool copy_back(Cursor& c, size_t at, uint32_t d, std::vector<int32_t>& out) {
    if (d < 1 || d > out.size()) return c.bad(at, "back-reference distance is out of range");
    out.push_back(out[out.size() - d]);
    return true;
}

bool tokens(Cursor& c, uint32_t count, uint32_t fresh, bool axis, std::vector<int32_t>& out) {
    out.clear();
    if (count > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
        return c.bad(c.pos, "token count is too large");
    // En ledbyte ger högst 64 värden; ett tal som inte ryms i resten av datan
    // får inte leda till någon allokering.
    if (count > 64ull * c.left())
        return c.bad(c.pos, "token count exceeds what the remaining bytes can encode");
    out.reserve(std::min<size_t>(count, c.left()));

    uint32_t next = 0;
    while (out.size() < count) {
        const size_t at = c.pos;
        if (c.left() < 1) return c.bad(at, "unexpected end");
        const uint8_t b = c.d[c.pos++];
        const uint32_t remain = count - static_cast<uint32_t>(out.size());

        uint32_t add = 1;
        if (b <= 0x3F)
            add = static_cast<uint32_t>(b) + 1;
        else if (b >= 0x80 && b <= 0xBF)
            add = static_cast<uint32_t>(b) - 0x7F;
        if (add > remain) return c.bad(at, "token runs past the count");

        if (b <= 0x3F) {
            for (uint32_t i = 0; i < add; ++i) out.push_back(static_cast<int32_t>(next++));
        } else if (b <= 0x7F) {
            if (!copy_back(c, at, static_cast<uint32_t>(b) - 0x40, out)) return false;
        } else if (b <= 0xBF) {
            if (out.empty()) return c.bad(at, "repeat has no previous value");
            const int32_t last = out.back();
            out.insert(out.end(), add, last);
        } else if (axis && b >= 0xD0 && b <= 0xD5) {
            out.push_back(0xCF - static_cast<int32_t>(b));
        } else if (b >= 0xE0 && b <= 0xEF) {
            if (c.left() < 1) return c.bad(c.pos, "unexpected end");
            const uint32_t d = (static_cast<uint32_t>(b & 0x0F) << 8) | c.d[c.pos++];
            if (!copy_back(c, at, d, out)) return false;
        } else if (b == 0xF0) {
            if (c.left() < 2) return c.bad(c.pos, "unexpected end");
            const uint32_t literal = (static_cast<uint32_t>(c.d[c.pos]) << 8) | c.d[c.pos + 1];
            c.pos += 2;
            if (literal >= next) return c.bad(at, "literal is not below the next fresh value");
            out.push_back(static_cast<int32_t>(literal));
        } else {
            return c.bad(at, "lead byte is undefined");
        }
    }

    if (next != fresh)
        return c.bad(c.pos, "next " + std::to_string(next) + " != fresh " + std::to_string(fresh));
    return true;
}

// Hoppar förbi `count` värden om `width` bitar (valfri bredd) och prövar att
// de oanvända bitarna i sista ordet är 0. Ger antalet ord.
bool skip_packed(Cursor& c, uint64_t count, uint64_t width, uint64_t& words) {
    const uint64_t bits = count * width;
    words = (bits + 31) / 32;
    if (words > c.left() / 4) return c.bad(c.pos, "packed words pass the end");
    const uint64_t used = bits % 32;
    if (words > 0 && used != 0) {
        const size_t last = c.pos + 4 * static_cast<size_t>(words - 1);
        if ((le32(c.d + last) & (0xFFFFFFFFu >> used)) != 0)
            return c.bad(last, "unused packed bit is set");
    }
    c.pos += 4 * static_cast<size_t>(words);
    return true;
}

bool packed(Cursor& c, uint64_t count, uint32_t width, std::vector<uint32_t>& out) {
    out.clear();
    if (width < 1 || width > 32) return c.bad(c.pos, "packed width is not in 1..32");
    const size_t start = c.pos;
    uint64_t words = 0;
    if (!skip_packed(c, count, width, words)) return false;

    // Orden är kontrollerade mot datan, så antalet värden ryms i den.
    out.resize(static_cast<size_t>(count));
    const uint64_t mask = (uint64_t(1) << width) - 1;
    for (uint64_t k = 0; k < count; ++k) {
        const uint64_t bit = k * width;
        const uint64_t word = bit >> 5;
        const unsigned off = static_cast<unsigned>(bit & 31);
        // Ett värde om högst 32 bitar ligger i högst två ord: läs dem som ett
        // 64-bitarsfönster med mest signifikanta biten först.
        uint64_t window = static_cast<uint64_t>(le32(c.d + start + 4 * word)) << 32;
        if (word + 1 < words) window |= le32(c.d + start + 4 * (word + 1));
        out[static_cast<size_t>(k)] = static_cast<uint32_t>((window >> (64 - off - width)) & mask);
    }
    return true;
}

// Ett f32 ur datan, utan prov.
float f32_at(const uint8_t* p) {
    const uint32_t bits = le32(p);
    float f;
    std::memcpy(&f, &bits, sizeof bits);
    return f;
}

// `width` bitar (0–32) från bit `bit` i en följd av little-endian-ord, mest
// signifikanta biten först som i PACKED. Orden ska vara kontrollerade mot datan.
uint32_t bits_at(const uint8_t* words, uint64_t word_count, uint64_t bit, uint32_t width) {
    if (width == 0) return 0;
    const uint64_t word = bit >> 5;
    const unsigned off = static_cast<unsigned>(bit & 31);
    uint64_t window = static_cast<uint64_t>(le32(words + 4 * word)) << 32;
    if (word + 1 < word_count) window |= le32(words + 4 * (word + 1));
    const uint64_t mask = (uint64_t(1) << width) - 1;
    return static_cast<uint32_t>((window >> (64 - off - width)) & mask);
}

// Paren (u, v) per render-vertex ur `pairs` och mappen; utan mapp är paret
// render-vertexens eget.
void spread_uvs(const std::vector<float>& pairs, const std::vector<int32_t>* map, uint32_t p1,
                std::vector<float>& out) {
    if (!map) {
        out = pairs;
        return;
    }
    out.resize(2 * static_cast<size_t>(p1));
    for (uint32_t r = 0; r < p1; ++r) {
        const size_t k = static_cast<size_t>((*map)[r]);
        out[2 * static_cast<size_t>(r)] = pairs[2 * k];
        out[2 * static_cast<size_t>(r) + 1] = pairs[2 * k + 1];
    }
}

// UV-kroppen: paren lämnas ut per render-vertex i `out`.
bool uv_body(Cursor& c, int32_t p1, std::vector<float>& out) {
    std::vector<int32_t> map;
    size_t at = c.pos;
    int32_t n;
    if (!c.i32(n)) return false;
    if (n == p1) {
        at = c.pos;
        int32_t fc;
        if (!c.i32(fc)) return false;
        if (fc <= 0 || fc % 2 != 0) return c.bad(at, "UV float count is not positive and even");
        const size_t floats_at = c.pos;
        if (!c.skip(4ull * static_cast<uint32_t>(fc), "UV floats pass the end")) return false;
        std::vector<float> pairs(static_cast<size_t>(fc));
        for (size_t k = 0; k < pairs.size(); ++k) {
            pairs[k] = f32_at(c.d + floats_at + 4 * k);
            if (!std::isfinite(pairs[k])) return c.bad(floats_at + 4 * k, "UV value is not finite");
        }
        if (fc / 2 != p1) {
            if (!tokens(c, static_cast<uint32_t>(p1), static_cast<uint32_t>(fc / 2), false, map) ||
                !c.pad(4))
                return false;
            spread_uvs(pairs, &map, static_cast<uint32_t>(p1), out);
        } else {
            spread_uvs(pairs, nullptr, static_cast<uint32_t>(p1), out);
        }
        return true;
    }
    if (n == -p1) {
        const size_t range_at = c.pos;
        if (!c.skip(16, "UV range passes the end")) return false;
        float range[4];   // umin, vmin, umax, vmax
        for (size_t k = 0; k < 4; ++k) {
            range[k] = f32_at(c.d + range_at + 4 * k);
            if (!std::isfinite(range[k])) return c.bad(range_at + 4 * k, "UV range is not finite");
        }
        at = c.pos;
        int32_t w;
        if (!c.i32(w)) return false;
        const uint32_t uw = static_cast<uint32_t>(w);
        const uint32_t bu = uw & 0xFF, bv = (uw >> 8) & 0xFF;
        // En bredd på 0 är en koordinat som är densamma i alla par.
        if ((uw >> 16) != 0) return c.bad(at, "UV bit widths are not two bytes");
        const size_t m_at = c.pos;
        int32_t m;
        if (!c.i32(m)) return false;
        if (m <= 0 || m > p1) return c.bad(m_at, "UV value count is not in 1..p1");
        const size_t words_at = c.pos;
        uint64_t words = 0;
        if (!skip_packed(c, static_cast<uint32_t>(m), bu + bv, words)) return false;
        if (bu > 32 || bv > 32)
            return c.bad(at, "a quantised UV width above 32 bits is not decoded",
                         ErrorCode::Unsupported);

        // Orden är kontrollerade mot datan, så antalet par ryms i den.
        std::vector<float> pairs(2 * static_cast<size_t>(m));
        const double du = bu ? (range[2] - static_cast<double>(range[0])) /
                                   static_cast<double>((uint64_t(1) << bu) - 1)
                             : 0.0;
        const double dv = bv ? (range[3] - static_cast<double>(range[1])) /
                                   static_cast<double>((uint64_t(1) << bv) - 1)
                             : 0.0;
        for (uint64_t k = 0; k < static_cast<uint64_t>(m); ++k) {
            const uint64_t bit = k * (bu + bv);
            const uint32_t qu = bits_at(c.d + words_at, words, bit, bu);
            const uint32_t qv = bits_at(c.d + words_at, words, bit + bu, bv);
            pairs[2 * k] = static_cast<float>(range[0] + qu * du);
            pairs[2 * k + 1] = static_cast<float>(range[1] + qv * dv);
        }
        if (m != p1) {
            if (!tokens(c, static_cast<uint32_t>(p1), static_cast<uint32_t>(m), false, map) ||
                !c.pad(4))
                return false;
            spread_uvs(pairs, &map, static_cast<uint32_t>(p1), out);
        } else {
            spread_uvs(pairs, nullptr, static_cast<uint32_t>(p1), out);
        }
        return true;
    }
    return c.bad(at, "UV count is neither p1 nor -p1");
}

bool slot_id(Cursor& c, int32_t& next_id, int32_t id, size_t at) {
    if (id != next_id) return c.bad(at, "slot id is out of order");
    ++next_id;
    return true;
}

// Ett flyttal i en form; det får inte vara oändligt eller NaN.
bool real(Cursor& c, double& v) {
    if (c.left() < 4) return c.bad(c.pos, "unexpected end");
    const uint32_t bits = le32(c.d + c.pos);
    float f;
    std::memcpy(&f, &bits, sizeof bits);
    if (!std::isfinite(f)) return c.bad(c.pos, "shape value is not finite");
    v = f;
    c.pos += 4;
    return true;
}

bool vec3(Cursor& c, double v[3]) { return real(c, v[0]) && real(c, v[1]) && real(c, v[2]); }

double dot(const double u[3], const double v[3]) {
    return u[0] * v[0] + u[1] * v[1] + u[2] * v[2];
}

// En cirkel (klass 104) eller en cylindermantel (klass 105), efter [100][cls].
bool shape(Cursor& c, bool cylinder, RecordData& out) {
    constexpr double kTol = 1e-5;
    constexpr double kPi = 3.14159265358979323846;
    constexpr uint32_t n = kShapeSides;

    if (!c.expect(cylinder ? 0x30003 : 3, "shape header is not the expected value")) return false;

    const size_t origin_at = c.pos;
    double origin[3], p[3] = {0, 0, 0}, a[3], b[3], r;
    if (!vec3(c, origin)) return false;
    if (origin[0] != 0 || origin[1] != 0 || origin[2] != 0)
        return c.bad(origin_at, "a shape with a non-zero origin is not decoded",
                     ErrorCode::Unsupported);
    const size_t p_at = c.pos;
    if (cylinder && !vec3(c, p)) return false;
    const size_t a_at = c.pos;
    if (!vec3(c, a) || !vec3(c, b)) return false;
    const size_t r_at = c.pos;
    if (!real(c, r)) return false;
    if (!cylinder && !c.expect(0, "circle does not end with zero")) return false;
    if (!c.pad(8)) return false;
    if (c.pos != c.n) return c.bad(c.pos, "record does not end at its length");

    if (std::fabs(dot(a, a) - 1) > kTol || std::fabs(dot(b, b) - 1) > kTol ||
        std::fabs(dot(a, b)) > kTol)
        return c.bad(a_at, "shape directions are not perpendicular unit vectors");
    if (!(r > 0)) return c.bad(r_at, "shape radius is not positive");

    // Cylinderns axel ska stå vinkelrätt mot cirkeln; `along` är dess längd
    // med tecken längs a × b.
    double along = 0;
    if (cylinder) {
        const double normal[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
                                  a[0] * b[1] - a[1] * b[0]};
        along = dot(p, normal);
        const double length = std::sqrt(dot(p, p));
        double off = 0;
        for (int k = 0; k < 3; ++k) off = std::max(off, std::fabs(p[k] - along * normal[k]));
        if (length == 0 || off > kTol * length)
            return c.bad(p_at, "cylinder axis is not perpendicular to its circle");
    }

    // Lådan är den kring kvadraten som omsluter cirkeln, svept längs axeln.
    for (int k = 0; k < 3; ++k) {
        const double e = r * (std::fabs(a[k]) + std::fabs(b[k]));
        out.bbox[k] = static_cast<float>(std::min(0.0, p[k]) - e);
        out.bbox[3 + k] = static_cast<float>(std::max(0.0, p[k]) + e);
    }
    out.box_exact = false;
    out.lines = !cylinder;

    const uint32_t rings = cylinder ? 2 : 1;
    out.positions.resize(3 * static_cast<size_t>(n) * rings);
    if (cylinder) out.normals.resize(out.positions.size());
    for (uint32_t i = 0; i < n; ++i) {
        const double angle = 2 * kPi * i / n;
        const double co = std::cos(angle), si = std::sin(angle);
        for (uint32_t k = 0; k < 3; ++k) {
            const double radial = co * a[k] + si * b[k];
            out.positions[3 * i + k] = static_cast<float>(r * radial);
            if (cylinder) {
                out.positions[3 * (n + i) + k] = static_cast<float>(r * radial + p[k]);
                out.normals[3 * i + k] = out.normals[3 * (n + i) + k] =
                    static_cast<float>(radial);
            }
        }
    }

    if (!cylinder) {
        out.primitives = n;
        out.indices.reserve(2 * static_cast<size_t>(n));
        for (uint32_t i = 0; i < n; ++i) {
            out.indices.push_back(i);
            out.indices.push_back((i + 1) % n);
        }
        return true;
    }
    out.primitives = 2 * n;
    out.indices.reserve(6 * static_cast<size_t>(n));
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t j = (i + 1) % n;
        // Moturs sedd utifrån, åt vilket håll axeln än pekar.
        const uint32_t lo_i = along > 0 ? i : j, lo_j = along > 0 ? j : i;
        out.indices.insert(out.indices.end(),
                           {lo_i, lo_j, n + lo_j, lo_i, n + lo_j, n + lo_i});
    }
    return true;
}

// En text (klass 103), efter [100][cls].
bool text(Cursor& c, RecordData& out) {
    const size_t len_at = c.pos;
    int32_t len;
    if (!c.i32(len)) return false;
    if (len < 0 || static_cast<size_t>(len) > c.left())
        return c.bad(len_at, "text length is negative or passes the end");
    out.text_utf8.assign(reinterpret_cast<const char*>(c.d + c.pos), static_cast<size_t>(len));
    c.pos += static_cast<size_t>(len);
    if (!c.pad(4)) return false;

    const size_t scale_at = c.pos;
    double sx, sy, angle, axis[3], lo[3], hi[3];
    if (!real(c, sx) || !real(c, sy)) return false;
    if (sx != 1 || sy != 1)
        return c.bad(scale_at, "a text whose two leading values are not 1 is not decoded",
                     ErrorCode::Unsupported);
    if (!real(c, angle)) return false;
    const size_t axis_at = c.pos;
    if (!vec3(c, axis)) return false;
    if (std::fabs(dot(axis, axis) - 1) > 1e-4)
        return c.bad(axis_at, "text axis is not a unit vector");
    const size_t box_at = c.pos;
    if (!vec3(c, lo) || !vec3(c, hi)) return false;
    for (int k = 0; k < 3; ++k) {
        if (lo[k] > hi[k]) return c.bad(box_at, "text box is inverted");
        out.bbox[k] = static_cast<float>(lo[k]);
        out.bbox[3 + k] = static_cast<float>(hi[k]);
    }
    const size_t font_at = c.pos;
    int32_t font;
    if (!c.i32(font)) return false;
    if (font < 0) return c.bad(font_at, "text font index is negative");
    if (!c.pad(8)) return false;
    if (c.pos != c.n) return c.bad(c.pos, "record does not end at its length");

    out.text = true;
    out.text_angle = static_cast<float>(angle);
    for (int k = 0; k < 3; ++k) out.text_axis[k] = static_cast<float>(axis[k]);
    out.text_font = font;
    return true;
}

bool body(Cursor& c, int normal_bits, RecordData& out) {
    if (!c.expect(100, "record id is not 100")) return false;
    size_t at = c.pos;
    int32_t cls;
    if (!c.i32(cls)) return false;
    if (cls == 103) return text(c, out);
    if (cls == 104 || cls == 105) return shape(c, cls == 105, out);
    if (cls != 94 && cls != 95 && cls != 96)
        return c.bad(at, "class is not 94, 95, 96, 103, 104 or 105");
    const bool lines = cls == 95;
    const bool points = cls == 96;
    out.lines = lines;
    out.points = points;

    if (!c.expect(101, "position object id is not 101") ||
        !c.expect(60, "position class is not 60"))
        return false;

    const size_t p1_at = c.pos;
    int32_t p1_raw;
    if (!c.i32(p1_raw)) return false;
    if (p1_raw < 0)
        return c.bad(p1_at, "quantised positions (negative render vertex count) are not decoded",
                     ErrorCode::Unsupported);
    if (p1_raw == 0) return c.bad(p1_at, "render vertex count is zero");
    const uint32_t p1 = static_cast<uint32_t>(p1_raw);
    // Ingen post har fler render-vertexar än byte.
    if (p1 > c.n) return c.bad(p1_at, "render vertex count is above the record length");

    at = c.pos;
    int32_t p2;
    if (!c.i32(p2)) return false;
    if (p2 <= 0 || p2 % 3 != 0)
        return c.bad(at, "position float count is not a positive multiple of 3");
    if (4ull * static_cast<uint32_t>(p2) > c.left()) return c.bad(c.pos, "positions pass the end");
    const uint32_t npos = static_cast<uint32_t>(p2) / 3;
    if (p1 < npos) return c.bad(p1_at, "render vertex count is below the position count");

    // De lagrade positionerna, och deras ram.
    std::vector<float> stored(static_cast<size_t>(p2));
    for (uint32_t i = 0; i < npos; ++i) {
        for (uint32_t j = 0; j < 3; ++j) {
            const size_t k = static_cast<size_t>(i) * 3 + j;
            const uint32_t bits = le32(c.d + c.pos + 4 * k);
            std::memcpy(&stored[k], &bits, sizeof bits);
            if (!std::isfinite(stored[k]))
                return c.bad(c.pos + 4 * k, "position is not finite");
            if (i == 0) {
                out.bbox[j] = out.bbox[3 + j] = stored[k];
            } else {
                out.bbox[j] = std::min(out.bbox[j], stored[k]);
                out.bbox[3 + j] = std::max(out.bbox[3 + j], stored[k]);
            }
        }
    }
    c.pos += 4 * static_cast<size_t>(p2);

    std::vector<int32_t> map;
    if (p1 != npos) {
        if (!tokens(c, p1, npos, false, map) || !c.pad(4)) return false;
        out.positions.resize(3 * static_cast<size_t>(p1));
        for (uint32_t r = 0; r < p1; ++r)
            for (uint32_t j = 0; j < 3; ++j)
                out.positions[3 * static_cast<size_t>(r) + j] =
                    stored[3 * static_cast<size_t>(map[r]) + j];
    } else {
        out.positions = std::move(stored);
    }

    // Platserna: UV, normaler, färg. Id räknas från 102 i den ordning platserna finns.
    int32_t next_id = 102;
    int32_t id;

    at = c.pos;
    if (!c.i32(id)) return false;
    const bool has_uv = id != 0;
    if (has_uv) {
        if (!slot_id(c, next_id, id, at) || !c.expect(61, "UV class is not 61") ||
            !uv_body(c, p1_raw, out.uvs))
            return false;
    }

    at = c.pos;
    if (!c.i32(id)) return false;
    const bool has_normals = id != 0;
    if (has_normals) {
        if (!slot_id(c, next_id, id, at) || !c.expect(100, "normal class is not 100") ||
            !c.expect(-p1_raw, "normal count is not -p1"))
            return false;
        at = c.pos;
        int32_t pal;
        if (!c.i32(pal)) return false;
        if (pal < 0 || pal > std::numeric_limits<int32_t>::max() / 3)
            return c.bad(at, "normal palette size is out of range");
        std::vector<uint32_t> raw;
        if (!packed(c, 3ull * static_cast<uint32_t>(pal), static_cast<uint32_t>(normal_bits), raw))
            return false;
        std::vector<float> palette(raw.size());
        for (size_t k = 0; 3 * k < raw.size(); ++k)
            palette_normal(raw[3 * k], raw[3 * k + 1], raw[3 * k + 2], normal_bits, &palette[3 * k]);
        out.normal_palette = static_cast<uint32_t>(pal);

        if (static_cast<uint32_t>(pal) == p1) {
            out.normals = std::move(palette);
        } else {
            if (!tokens(c, p1, static_cast<uint32_t>(pal), true, map) || !c.pad(4)) return false;
            out.normals.resize(3 * static_cast<size_t>(p1));
            for (uint32_t r = 0; r < p1; ++r) {
                const int32_t k = map[r];
                const float* src = k >= 0 ? &palette[3 * static_cast<size_t>(k)] : kAxes[-k - 1];
                std::copy(src, src + 3, &out.normals[3 * static_cast<size_t>(r)]);
            }
        }
    }

    at = c.pos;
    if (!c.i32(id)) return false;
    const bool has_colors = id != 0;
    if (has_colors) {
        if (!slot_id(c, next_id, id, at) || !c.expect(58, "colour class is not 58") ||
            !c.expect(5634, "colour flag is not 5634") ||
            !c.expect(-p1_raw, "colour count is not -p1"))
            return false;
        at = c.pos;
        int32_t cpal;
        if (!c.i32(cpal)) return false;
        if (cpal <= 0) return c.bad(at, "colour palette size is not positive");
        if (4ull * static_cast<uint32_t>(cpal) > c.left())
            return c.bad(c.pos, "colour entries pass the end");
        // En post är [A][B][G][R]: som little-endian-uint32 är det 0xRRGGBBAA.
        std::vector<uint32_t> palette(static_cast<size_t>(cpal));
        for (size_t k = 0; k < palette.size(); ++k) palette[k] = le32(c.d + c.pos + 4 * k);
        c.pos += 4 * palette.size();

        if (static_cast<uint32_t>(cpal) == p1) {
            out.colors = std::move(palette);
        } else {
            if (!tokens(c, p1, static_cast<uint32_t>(cpal), false, map) || !c.pad(4)) return false;
            out.colors.resize(p1);
            for (uint32_t r = 0; r < p1; ++r) out.colors[r] = palette[static_cast<size_t>(map[r])];
        }
    }

    // Trianglar har alltid normaler och får ha färg; linjer och punkter får
    // bara ha färg, och den får saknas.
    if ((lines || points) ? (has_uv || has_normals) : !has_normals)
        return c.bad(c.pos, "slot set does not match the class");

    // Punkter: efter platserna ett tal, 0 eller 1, och sedan är posten slut.
    // Varje render-vertex är en punkt.
    if (points) {
        at = c.pos;
        int32_t flag;
        if (!c.i32(flag)) return false;
        if (flag != 0 && flag != 1) return c.bad(at, "point record does not end with 0 or 1");
        if (!c.pad(8)) return false;
        if (c.pos != c.n) return c.bad(c.pos, "record does not end at its length");
        out.primitives = p1;
        return true;
    }

    // P: antal, bredd, antal igen, och längderna.
    at = c.pos;
    int32_t count;
    if (!c.i32(count)) return false;
    if (count <= 0) return c.bad(at, "primitive count is not positive");
    int32_t width;
    if (!c.i32(width)) return false;
    if (width < 1 || width > 31) return c.bad(at + 4, "primitive length width is not in 1..31");
    if (!c.expect(count, "second primitive count differs")) return false;
    const size_t lengths_at = c.pos;
    std::vector<uint32_t> lengths;
    if (!packed(c, static_cast<uint32_t>(count), static_cast<uint32_t>(width), lengths))
        return false;
    uint64_t sum = 0;
    for (const uint32_t len : lengths) {
        if (len < (lines ? 2u : 3u)) return c.bad(lengths_at, "primitive length is too short");
        sum += len;
    }

    // Hörnen: linjer går över 0 .. p1-1, trianglar över 0 .. p1-1 eller TOKENS(S, p1).
    std::vector<int32_t> corners;
    bool identity = true;
    if (lines) {
        if (sum != p1) return c.bad(lengths_at, "primitive lengths do not sum to p1");
    } else {
        at = c.pos;
        int32_t s;
        if (!c.i32(s)) return false;
        if (s < 0) return c.bad(at, "corner count is negative");
        if (s > 0 && static_cast<uint32_t>(s) <= p1)
            return c.bad(at, "corner count is not above p1");
        const uint32_t corner_count = s == 0 ? p1 : static_cast<uint32_t>(s);
        if (corner_count > c.n) return c.bad(at, "corner count is above the record length");
        if (s > 0) {
            identity = false;
            if (!tokens(c, corner_count, p1, false, corners) || !c.pad(4)) return false;
        }
        if (sum != corner_count)
            return c.bad(lengths_at, "primitive lengths do not sum to the corner count");
    }

    if (!c.pad(8)) return false;
    if (c.pos != c.n) return c.bad(c.pos, "record does not end at its length");

    // Primitiverna. Hörnen är redan prövade mot p1.
    size_t start = 0;
    if (lines) {
        out.primitives = static_cast<uint32_t>(sum - static_cast<uint64_t>(count));
        out.indices.reserve(2 * static_cast<size_t>(out.primitives));
        for (const uint32_t len : lengths) {
            for (uint32_t i = 0; i + 1 < len; ++i) {
                out.indices.push_back(static_cast<uint32_t>(start + i));
                out.indices.push_back(static_cast<uint32_t>(start + i + 1));
            }
            start += len;
        }
    } else {
        out.primitives = static_cast<uint32_t>(sum - 2 * static_cast<uint64_t>(count));
        out.indices.reserve(3 * static_cast<size_t>(out.primitives));
        auto corner = [&](size_t k) {
            return identity ? static_cast<uint32_t>(k) : static_cast<uint32_t>(corners[k]);
        };
        for (const uint32_t len : lengths) {
            for (uint32_t i = 0; i + 2 < len; ++i) {
                const uint32_t a = corner(start + i), b = corner(start + i + 1);
                // Udda trianglar i en strip byter de två första hörnen, så att
                // varje triangel förblir moturs.
                out.indices.push_back(i % 2 == 0 ? a : b);
                out.indices.push_back(i % 2 == 0 ? b : a);
                out.indices.push_back(corner(start + i + 2));
            }
            start += len;
        }
    }
    return true;
}

} // namespace

ErrorCode decode_tokens(const uint8_t* data, size_t size, size_t& pos, uint32_t count,
                        uint32_t fresh, bool axis, std::vector<int32_t>& out,
                        std::string* detail) {
    out.clear();
    Fault f;
    Cursor c(data, size, pos, f);
    if (pos > size) {
        c.bad(pos, "position is past the end");
        return report(f, "record", detail);
    }
    if (!tokens(c, count, fresh, axis, out)) {
        out.clear();
        return report(f, "record", detail);
    }
    pos = c.pos;
    return ErrorCode::Ok;
}

ErrorCode decode_packed(const uint8_t* data, size_t size, size_t& pos, uint32_t count,
                        uint32_t width, std::vector<uint32_t>& out, std::string* detail) {
    out.clear();
    Fault f;
    Cursor c(data, size, pos, f);
    if (pos > size) {
        c.bad(pos, "position is past the end");
        return report(f, "record", detail);
    }
    if (!packed(c, count, width, out)) {
        out.clear();
        return report(f, "record", detail);
    }
    pos = c.pos;
    return ErrorCode::Ok;
}

void palette_normal(uint32_t x, uint32_t y, uint32_t z, int bits, float out[3]) {
    out[0] = out[1] = out[2] = 0.0f;
    if (bits < 2 || bits > 32) return;
    const int64_t half = int64_t(1) << (bits - 1);
    const double scale = static_cast<double>(half - 1);
    // Tvåkomplement i `bits` bitar.
    auto value = [&](uint32_t v) {
        const int64_t u = static_cast<int64_t>(v) & ((half << 1) - 1);
        return static_cast<double>(u >= half ? u - 2 * half : u) / scale;
    };
    const double vx = value(x), vy = value(y), vz = value(z);
    const double len = std::sqrt(vx * vx + vy * vy + vz * vz);
    if (len == 0.0) return;
    out[0] = static_cast<float>(vx / len);
    out[1] = static_cast<float>(vy / len);
    out[2] = static_cast<float>(vz / len);
}

ErrorCode parse_record(const uint8_t* record, size_t size, int normal_bits, RecordData& out,
                       std::string* detail) {
    out = RecordData{};
    Fault f;
    Cursor c(record, size, 0, f);
    if (normal_bits != 8 && normal_bits != 16) {
        c.bad(0, "normal width is not 8 or 16");
        return report(f, "record", detail);
    }
    if (!body(c, normal_bits, out)) {
        out = RecordData{};
        return report(f, "record", detail);
    }
    return ErrorCode::Ok;
}

ErrorCode parse_geometry_toc(const uint8_t* data, size_t size, GeometryToc& out,
                             std::string* detail) {
    out = GeometryToc{};
    Fault f;
    Cursor c(data, size, 0, f);
    auto fail = [&](size_t at, const char* rule) {
        c.bad(at, rule);
        out = GeometryToc{};
        return report(f, "geometry TOC", detail);
    };

    int32_t records, chunks;
    if (size < 8) return fail(0, "header passes the end");
    c.i32(records);
    c.i32(chunks);
    if (records < 0) return fail(0, "record count is negative");
    if (chunks < 0) return fail(4, "chunk count is negative");

    const uint64_t body_size =
        8 + 4 * static_cast<uint64_t>(chunks) + 4 * static_cast<uint64_t>(records);
    if (size != (body_size + 7) / 8 * 8)
        return fail(0, "length is not 8 + 4ch + 4N rounded up to 8");
    for (size_t i = static_cast<size_t>(body_size); i < size; ++i)
        if (data[i] != 0) return fail(i, "pad byte is not zero");

    // Längden är kontrollerad mot talen, så allokeringarna ryms i datan.
    out.chunk_sizes.reserve(static_cast<size_t>(chunks));
    for (int32_t i = 0; i < chunks; ++i) {
        const size_t at = c.pos;
        int32_t v = 0;
        if (!c.i32(v) || v <= 0) return fail(at, "chunk size is not positive");
        out.chunk_sizes.push_back(static_cast<uint32_t>(v));
    }
    out.record_lengths.reserve(static_cast<size_t>(records));
    for (int32_t k = 0; k < records; ++k) {
        const size_t at = c.pos;
        int32_t v = 0;
        if (!c.i32(v) || v <= 0 || v % 8 != 0)
            return fail(at, "record length is not a positive multiple of 8");
        out.record_lengths.push_back(static_cast<uint32_t>(v));
    }
    return ErrorCode::Ok;
}

} // namespace HNwd::detail
