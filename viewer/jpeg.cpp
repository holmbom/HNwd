#include "jpeg.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>

namespace nwdview {

namespace {

// Zickzackordningen: koefficient k i en ström står på plats kZigzag[k] i
// blocket (radvis, 8 × 8).
constexpr uint8_t kZigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// En Huffmantabell: koderna i kanonisk ordning, med en uppslagstabell för
// koder om högst kFastBits bitar och gränser per längd för de längre.
constexpr int kFastBits = 9;

struct Huffman {
    bool defined = false;
    uint8_t symbols[256] = {};
    uint16_t fast[1 << kFastBits] = {};   // (längd << 8) | symbol; 0 = ingen kort kod
    int32_t maxcode[18] = {};             // största kod av längden l, −1 utan koder
    int32_t valoffset[18] = {};           // symbolindex = kod + valoffset[l]
};

bool build_huffman(const uint8_t counts[16], const uint8_t* symbols, int total, Huffman& h,
                   std::string& error) {
    h = Huffman{};
    std::memcpy(h.symbols, symbols, static_cast<size_t>(total));
    int code = 0, k = 0;
    for (int l = 1; l <= 16; ++l) {
        const int n = counts[l - 1];
        if (n == 0) {
            h.maxcode[l] = -1;
        } else {
            h.valoffset[l] = k - code;
            for (int i = 0; i < n; ++i, ++k, ++code) {
                if (code >= (1 << l)) {
                    error = "en Huffmantabell har fler koder än längderna rymmer";
                    return false;
                }
                if (l <= kFastBits) {
                    const int shift = kFastBits - l;
                    for (int f = 0; f < (1 << shift); ++f)
                        h.fast[(code << shift) | f] =
                            static_cast<uint16_t>(l << 8 | h.symbols[k]);
                }
            }
            h.maxcode[l] = code - 1;
        }
        code <<= 1;
    }
    h.maxcode[17] = INT32_MAX;
    h.defined = true;
    return true;
}

// Bitarna i en entropikodad ström: utfyllnaden 0xFF 0x00 tas bort, och vid
// en markör tar strömmen slut. Bitar efter slutet räknas; den som läser
// dem har en trasig ström.
struct BitReader {
    const uint8_t* p = nullptr;
    const uint8_t* end = nullptr;
    uint64_t buf = 0;   // bitarna vänsterställda
    int count = 0;      // giltiga bitar i buf
    int padded = 0;     // nollbitar som lagts till efter strömmens slut
    bool at_marker = false;

    void reset(const uint8_t* from, const uint8_t* to) {
        p = from;
        end = to;
        buf = 0;
        count = 0;
        padded = 0;
        at_marker = false;
    }
    void fill() {
        while (count <= 56) {
            uint8_t byte = 0;
            if (!at_marker && p < end) {
                if (*p == 0xFF) {
                    if (p + 1 < end && p[1] == 0x00) {
                        byte = 0xFF;
                        p += 2;
                    } else {
                        at_marker = true;
                    }
                } else {
                    byte = *p++;
                }
            } else {
                at_marker = true;
            }
            if (at_marker) padded += 8;
            buf |= static_cast<uint64_t>(byte) << (56 - count);
            count += 8;
        }
    }
    // Läste avkodaren förbi strömmens slut?
    bool overrun() const { return padded > count; }
    int bit() {
        if (count < 1) fill();
        const int b = static_cast<int>(buf >> 63);
        buf <<= 1;
        --count;
        return b;
    }
    int bits(int n) {
        if (n == 0) return 0;
        if (count < n) fill();
        const int v = static_cast<int>(buf >> (64 - n));
        buf <<= n;
        count -= n;
        return v;
    }
    // Ett tal med `n` bitar enligt JPEG:s teckenregel.
    int extend(int n) {
        if (n == 0) return 0;
        const int v = bits(n);
        return v < (1 << (n - 1)) ? v - (1 << n) + 1 : v;
    }
    // Nästa symbol; −1 för en kod som inte finns.
    int decode(const Huffman& h) {
        if (count < 16) fill();
        const uint16_t f = h.fast[buf >> (64 - kFastBits)];
        if (f) {
            const int len = f >> 8;
            buf <<= len;
            count -= len;
            return f & 0xFF;
        }
        int32_t code = static_cast<int32_t>(buf >> (64 - (kFastBits + 1)));
        int l = kFastBits + 1;
        while (l <= 16 && code > h.maxcode[l]) {
            ++l;
            code = static_cast<int32_t>(buf >> (64 - l));
        }
        if (l > 16) return -1;
        buf <<= l;
        count -= l;
        const int idx = code + h.valoffset[l];
        if (idx < 0 || idx > 255) return -1;
        return h.symbols[idx];
    }
};

struct Component {
    int id = 0;
    int h = 1, v = 1;          // samplingsfaktorer
    int tq = 0;                // kvantiseringstabell
    int td = 0, ta = 0;        // Huffmantabeller i aktuell skanning
    int width = 0, height = 0; // komponentens mått i samplingar
    int bw = 0, bh = 0;        // block som täcker komponenten
    int pbw = 0, pbh = 0;      // block i lagringen (utfyllt till hela MCU)
    std::vector<int16_t> coef; // pbw × pbh × 64, i radordning per block
    std::array<uint16_t, 64> quant{};
    bool quant_latched = false;
    int dc_pred = 0;
};

struct Decoder {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t pos = 0;
    std::string error;

    bool frame_seen = false;
    bool progressive = false;
    int width = 0, height = 0;
    int hmax = 1, vmax = 1;
    int mcux = 0, mcuy = 0;
    std::vector<Component> comps;
    uint16_t qt[4][64] = {};
    bool qt_defined[4] = {};
    Huffman dc[4], ac[4];
    int restart_interval = 0;
    int adobe_transform = -1;   // −1: ingen Adobe-markör
    bool jfif = false;
    int eobrun = 0;
    BitReader br;

    bool fail(const std::string& what) {
        error = what;
        return false;
    }
    bool need(size_t n) const { return pos + n <= size; }
    int u16(size_t at) const { return data[at] << 8 | data[at + 1]; }

    bool read_dqt(size_t at, size_t len) {
        size_t i = at;
        while (i < at + len) {
            const int pq = data[i] >> 4, tq = data[i] & 15;
            ++i;
            if (tq > 3) return fail("en kvantiseringstabell har ett nummer över 3");
            if (pq > 1) return fail("en kvantiseringstabell har en okänd precision");
            const size_t bytes = pq ? 128 : 64;
            if (i + bytes > at + len) return fail("en kvantiseringstabell är kortare än sin längd");
            for (int k = 0; k < 64; ++k) {
                const int q = pq ? u16(i + 2 * static_cast<size_t>(k)) : data[i + static_cast<size_t>(k)];
                qt[tq][kZigzag[k]] = static_cast<uint16_t>(q);
            }
            qt_defined[tq] = true;
            i += bytes;
        }
        return true;
    }

    bool read_dht(size_t at, size_t len) {
        size_t i = at;
        while (i < at + len) {
            if (i + 17 > at + len) return fail("en Huffmantabell är kortare än sin längd");
            const int tc = data[i] >> 4, th = data[i] & 15;
            if (tc > 1 || th > 3) return fail("en Huffmantabell har en okänd klass eller ett nummer över 3");
            uint8_t counts[16];
            int total = 0;
            for (int l = 0; l < 16; ++l) {
                counts[l] = data[i + 1 + static_cast<size_t>(l)];
                total += counts[l];
            }
            if (total > 256) return fail("en Huffmantabell har fler än 256 symboler");
            i += 17;
            if (i + static_cast<size_t>(total) > at + len)
                return fail("en Huffmantabell är kortare än sin längd");
            if (!build_huffman(counts, data + i, total, tc ? ac[th] : dc[th], error)) return false;
            i += static_cast<size_t>(total);
        }
        return true;
    }

    bool read_sof(size_t at, size_t len, uint8_t marker) {
        if (frame_seen) return fail("filen har mer än en bildram");
        if (marker == 0xC3 || marker == 0xC7 || marker == 0xCB || marker == 0xCF)
            return fail("förlustfri JPEG avkodas inte");
        if (marker == 0xC8) return fail("markören 0xC8 är reserverad och avkodas inte");
        if (marker >= 0xC9) return fail("aritmetiskt kodad JPEG avkodas inte");
        if (marker == 0xC5 || marker == 0xC6) return fail("hierarkisk JPEG avkodas inte");
        progressive = marker == 0xC2;
        if (len < 6) return fail("bildramen är för kort");
        if (data[at] != 8) return fail("bara 8 bitar per sampling avkodas (filen har " +
                                       std::to_string(data[at]) + ")");
        height = u16(at + 1);
        width = u16(at + 3);
        const int nc = data[at + 5];
        if (height == 0) return fail("bildens höjd står inte i bildramen (DNL avkodas inte)");
        if (width == 0) return fail("bildens bredd är 0");
        if (nc != 1 && nc != 3)
            return fail("bara 1 eller 3 komponenter avkodas (filen har " + std::to_string(nc) + ")");
        if (len < 6 + 3 * static_cast<size_t>(nc)) return fail("bildramen är för kort");
        comps.resize(static_cast<size_t>(nc));
        for (int c = 0; c < nc; ++c) {
            Component& k = comps[static_cast<size_t>(c)];
            const size_t b = at + 6 + 3 * static_cast<size_t>(c);
            k.id = data[b];
            k.h = data[b + 1] >> 4;
            k.v = data[b + 1] & 15;
            k.tq = data[b + 2];
            if (k.h < 1 || k.h > 4 || k.v < 1 || k.v > 4)
                return fail("en komponent har en samplingsfaktor utanför 1–4");
            if (k.tq > 3) return fail("en komponent pekar på en kvantiseringstabell över 3");
            for (int e = 0; e < c; ++e)
                if (comps[static_cast<size_t>(e)].id == k.id)
                    return fail("två komponenter i bildramen har samma id");
            hmax = std::max(hmax, k.h);
            vmax = std::max(vmax, k.v);
        }
        for (const Component& k : comps)
            if (hmax % k.h != 0 || vmax % k.v != 0)
                return fail("samplingsfaktorerna går inte jämnt upp (" + std::to_string(k.h) + "×" +
                            std::to_string(k.v) + " mot " + std::to_string(hmax) + "×" +
                            std::to_string(vmax) + ")");
        mcux = (width + 8 * hmax - 1) / (8 * hmax);
        mcuy = (height + 8 * vmax - 1) / (8 * vmax);
        // Lagringen täcker hela MCU:er också för en ensam komponent, så att
        // varje blockindex en skanning kan ge ligger i den.
        size_t total = 0;
        for (Component& k : comps) {
            k.width = (width * k.h + hmax - 1) / hmax;
            k.height = (height * k.v + vmax - 1) / vmax;
            k.bw = (k.width + 7) / 8;
            k.bh = (k.height + 7) / 8;
            k.pbw = mcux * k.h;
            k.pbh = mcuy * k.v;
            total += static_cast<size_t>(k.pbw) * static_cast<size_t>(k.pbh) * 64 * sizeof(int16_t);
        }
        if (total > kMaxJpegCoefficientBytes)
            return fail("bilden är för stor att avkoda (" + std::to_string(width) + " × " +
                        std::to_string(height) + ")");
        for (Component& k : comps)
            k.coef.assign(static_cast<size_t>(k.pbw) * static_cast<size_t>(k.pbh) * 64, 0);
        frame_seen = true;
        return true;
    }

    // Blocket (bx, by) i komponentens lagring, eller nullptr utanför den.
    int16_t* block(Component& c, int bx, int by) {
        if (bx < 0 || by < 0 || bx >= c.pbw || by >= c.pbh) return nullptr;
        return &c.coef[(static_cast<size_t>(by) * static_cast<size_t>(c.pbw) +
                        static_cast<size_t>(bx)) *
                       64];
    }

    // Lägger `diff` till komponentens DC-prediktor. Prediktorn och värdet
    // med approximationens skift `al` ska rymmas i en koefficient (int16);
    // annars är strömmen trasig.
    bool add_dc(Component& c, int diff, int al, int16_t& out) {
        const int64_t pred = static_cast<int64_t>(c.dc_pred) + diff;
        const int64_t value = pred * (int64_t(1) << al);
        if (value < INT16_MIN || value > INT16_MAX)
            return fail("en DC-koefficient ryms inte i 16 bitar");
        c.dc_pred = static_cast<int>(pred);
        out = static_cast<int16_t>(value);
        return true;
    }

    // Ett block i en sekventiell skanning, eller en del av det i en
    // progressiv. Svarar false vid en ogiltig kod.
    bool decode_block(Component& c, int16_t* b, int ss, int se, int ah, int al) {
        if (!progressive) {
            const int t = br.decode(dc[c.td]);
            if (t < 0 || t > 11) return fail("en ogiltig Huffmankod för DC");
            if (!add_dc(c, br.extend(t), 0, b[0])) return false;
            for (int k = 1; k < 64;) {
                const int rs = br.decode(ac[c.ta]);
                if (rs < 0) return fail("en ogiltig Huffmankod för AC");
                const int r = rs >> 4, s = rs & 15;
                if (s == 0) {
                    if (r != 15) break;
                    k += 16;
                    continue;
                }
                k += r;
                if (k > 63) return fail("en koefficient hamnar utanför blocket");
                b[kZigzag[k]] = static_cast<int16_t>(br.extend(s));
                ++k;
            }
            return true;
        }
        if (ss == 0) {
            // DC: först värdet, sedan en bit per förfining.
            if (ah == 0) {
                const int t = br.decode(dc[c.td]);
                if (t < 0 || t > 11) return fail("en ogiltig Huffmankod för DC");
                if (!add_dc(c, br.extend(t), al, b[0])) return false;
            } else if (br.bit()) {
                b[0] = static_cast<int16_t>(b[0] | (1 << al));
            }
            return true;
        }
        if (ah == 0) {
            // AC, första passet för spannet.
            if (eobrun > 0) {
                --eobrun;
                return true;
            }
            for (int k = ss; k <= se;) {
                const int rs = br.decode(ac[c.ta]);
                if (rs < 0) return fail("en ogiltig Huffmankod för AC");
                const int r = rs >> 4, s = rs & 15;
                if (s == 0) {
                    if (r < 15) {
                        eobrun = (1 << r) - 1;
                        if (r) eobrun += br.bits(r);
                        break;
                    }
                    k += 16;
                    continue;
                }
                k += r;
                if (k > se) return fail("en koefficient hamnar utanför spannet");
                const int value = br.extend(s) * (1 << al);
                if (value < INT16_MIN || value > INT16_MAX)
                    return fail("en AC-koefficient ryms inte i 16 bitar");
                b[kZigzag[k]] = static_cast<int16_t>(value);
                ++k;
            }
            return true;
        }
        // AC, förfining: en bit för varje koefficient som redan är satt, och
        // nya koefficienter med värdet ±1 << al.
        const int p1 = 1 << al, m1 = -(1 << al);
        int k = ss;
        auto refine = [&](int16_t& v) {
            if (br.bit() && (v & p1) == 0) {
                const int nv = v >= 0 ? v + p1 : v + m1;
                if (nv < INT16_MIN || nv > INT16_MAX) return fail("en AC-koefficient ryms inte i 16 bitar");
                v = static_cast<int16_t>(nv);
            }
            return true;
        };
        if (eobrun == 0) {
            for (; k <= se; ++k) {
                const int rs = br.decode(ac[c.ta]);
                if (rs < 0) return fail("en ogiltig Huffmankod för AC");
                int r = rs >> 4, s = rs & 15;
                int value = 0;
                if (s) {
                    if (s != 1) return fail("en förfining har en koefficient större än 1");
                    value = br.bit() ? p1 : m1;
                } else if (r != 15) {
                    eobrun = 1 << r;
                    if (r) eobrun += br.bits(r);
                    break;
                }
                while (k <= se) {
                    int16_t& v = b[kZigzag[k]];
                    if (v != 0) {
                        if (!refine(v)) return false;
                    } else {
                        if (r == 0) break;
                        --r;
                    }
                    ++k;
                }
                if (value) {
                    if (k > se) return fail("en koefficient hamnar utanför spannet");
                    b[kZigzag[k]] = static_cast<int16_t>(value);
                }
            }
        }
        if (eobrun > 0) {
            for (; k <= se; ++k) {
                int16_t& v = b[kZigzag[k]];
                if (v != 0 && !refine(v)) return false;
            }
            --eobrun;
        }
        return true;
    }

    // Läser en skanning från `pos` (efter SOS-segmentet) till nästa markör
    // som inte är en omstart.
    bool read_scan(size_t at, size_t len) {
        if (!frame_seen) return fail("en skanning kommer före bildramen");
        if (len < 1) return fail("skanningens rubrik är trasig");
        const int ns = data[at];
        if (ns < 1 || ns > static_cast<int>(comps.size()) || len < 4 + 2 * static_cast<size_t>(ns))
            return fail("skanningens rubrik är trasig");
        // Komponenterna står var och en en gång och i bildramens ordning; den
        // sammanflätade avkodningen bygger på det.
        std::vector<Component*> sc;
        size_t next_comp = 0;
        for (int i = 0; i < ns; ++i) {
            const int id = data[at + 1 + 2 * static_cast<size_t>(i)];
            const int t = data[at + 2 + 2 * static_cast<size_t>(i)];
            size_t found = next_comp;
            while (found < comps.size() && comps[found].id != id) ++found;
            if (found == comps.size()) {
                for (const Component& c : comps)
                    if (c.id == id)
                        return fail("skanningen nämner en komponent två gånger eller i fel ordning");
                return fail("skanningen nämner en komponent som inte finns");
            }
            next_comp = found + 1;
            Component& c = comps[found];
            if ((t >> 4) > 3 || (t & 15) > 3) return fail("skanningen pekar på en Huffmantabell över 3");
            c.td = t >> 4;
            c.ta = t & 15;
            sc.push_back(&c);
        }
        const size_t s = at + 1 + 2 * static_cast<size_t>(ns);
        const int ss = data[s], se = data[s + 1], ah = data[s + 2] >> 4, al = data[s + 2] & 15;
        if (progressive) {
            if (ss > se || se > 63 || (ss == 0 && se != 0) || (ss > 0 && ns != 1) || al > 13 ||
                ah > 13 || (ah != 0 && al != ah - 1))
                return fail("skanningens spektrala urval eller approximation är ogiltigt");
        } else if (ss != 0 || se != 63 || ah != 0 || al != 0) {
            return fail("en sekventiell skanning har ett spektralt urval");
        }
        int mcu_blocks = 0;
        for (Component* c : sc) {
            mcu_blocks += c->h * c->v;
            if (!qt_defined[c->tq]) return fail("en komponent saknar sin kvantiseringstabell");
            if (!c->quant_latched) {
                std::memcpy(c->quant.data(), qt[c->tq], sizeof c->quant);
                c->quant_latched = true;
            }
            const bool needs_dc = !progressive || (ss == 0 && ah == 0);
            const bool needs_ac = !progressive || ss > 0;
            if (needs_dc && !dc[c->td].defined) return fail("en DC-tabell saknas");
            if (needs_ac && !ac[c->ta].defined) return fail("en AC-tabell saknas");
            c->dc_pred = 0;
        }
        if (ns > 1 && mcu_blocks > 10) return fail("en MCU har fler än 10 block");
        eobrun = 0;

        pos = at + len;
        br.reset(data + pos, data + size);
        int restarts_left = restart_interval;
        int next_rst = 0;
        auto after_mcu = [&](bool last) -> bool {
            if (br.overrun()) return fail("den entropikodade strömmen tar slut för tidigt");
            if (restart_interval == 0 || last) return true;
            if (--restarts_left > 0) return true;
            // Omstart: nästa oinlästa byte ska vara RSTn. Bitarna som står
            // kvar i bufferten är utfyllnad.
            const uint8_t* m = br.p;
            if (m + 1 >= data + size || m[0] != 0xFF || m[1] != 0xD0 + next_rst)
                return fail("en omstartsmarkör saknas eller står i fel ordning");
            next_rst = (next_rst + 1) & 7;
            br.reset(m + 2, data + size);
            for (Component* c : sc) c->dc_pred = 0;
            eobrun = 0;
            restarts_left = restart_interval;
            return true;
        };

        if (ns == 1) {
            Component& c = *sc[0];
            const int total = c.bw * c.bh;
            int n = 0;
            for (int by = 0; by < c.bh; ++by)
                for (int bx = 0; bx < c.bw; ++bx) {
                    int16_t* b = block(c, bx, by);
                    if (!b) return fail("ett block ligger utanför komponenten");
                    if (!decode_block(c, b, ss, se, ah, al)) return false;
                    if (!after_mcu(++n == total)) return false;
                }
        } else {
            const int total = mcux * mcuy;
            int n = 0;
            for (int my = 0; my < mcuy; ++my)
                for (int mx = 0; mx < mcux; ++mx) {
                    for (Component* c : sc)
                        for (int v = 0; v < c->v; ++v)
                            for (int h = 0; h < c->h; ++h) {
                                int16_t* b = block(*c, mx * c->h + h, my * c->v + v);
                                if (!b) return fail("ett block ligger utanför komponenten");
                                if (!decode_block(*c, b, ss, se, ah, al)) return false;
                            }
                    if (!after_mcu(++n == total)) return false;
                }
        }
        // Till nästa markör som inte är utfyllnad.
        const uint8_t* q = br.p;
        while (q + 1 < data + size && !(q[0] == 0xFF && q[1] != 0x00 && q[1] != 0xFF &&
                                        !(q[1] >= 0xD0 && q[1] <= 0xD7)))
            ++q;
        pos = static_cast<size_t>(q - data);
        return true;
    }

    bool parse() {
        if (size < 4 || data[0] != 0xFF || data[1] != 0xD8) return fail("filen börjar inte som en JPEG");
        pos = 2;
        bool scanned = false;
        for (;;) {
            // Markören, efter eventuell utfyllnad med 0xFF.
            while (pos < size && data[pos] != 0xFF) ++pos;
            while (pos < size && data[pos] == 0xFF) ++pos;
            if (pos >= size) {
                if (scanned) return true;   // EOI saknas, men allt är läst
                return fail("filen tar slut före bilddatan");
            }
            const uint8_t marker = data[pos++];
            if (marker == 0xD9) break;
            if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;
            if (!need(2)) return fail("filen tar slut i ett segment");
            const size_t len = static_cast<size_t>(u16(pos));
            if (len < 2 || pos + len > size) return fail("ett segment är längre än filen");
            const size_t body = pos + 2, blen = len - 2;
            pos += len;
            switch (marker) {
            case 0xDB:
                if (!read_dqt(body, blen)) return false;
                break;
            case 0xC4:
                if (!read_dht(body, blen)) return false;
                break;
            case 0xCC: return fail("aritmetiskt kodad JPEG avkodas inte");
            case 0xDD:
                if (blen < 2) return fail("omstartsintervallet är för kort");
                restart_interval = u16(body);
                break;
            case 0xDA:
                if (!read_scan(body, blen)) return false;
                scanned = true;
                break;
            case 0xDC: return fail("DNL-markören avkodas inte");
            case 0xEE:
                if (blen >= 12 && std::memcmp(data + body, "Adobe", 5) == 0)
                    adobe_transform = data[body + 11];
                break;
            case 0xE0:
                if (blen >= 5 && std::memcmp(data + body, "JFIF", 5) == 0) jfif = true;
                break;
            default:
                if (marker >= 0xC0 && marker <= 0xCF) {
                    if (!read_sof(body, blen, marker)) return false;
                }
                // APPn, COM och okända segment hoppas över.
                break;
            }
        }
        if (!frame_seen) return fail("filen saknar bildram");
        if (!scanned) return fail("filen saknar bilddata");
        return true;
    }
};

// En invers DCT som ger n × n samplingar (n = 8, 4, 2 eller 1) ur blockets
// n × n lägsta frekvenser. Med n < 8 blir varje sampling medelvärdet av det
// område den täcker, i frekvensernas mening.
struct Idct {
    int n = 8;
    float table[8][8] = {};   // table[x][u] = c(u) · cos((2x + 1)uπ / 2n) / 2

    explicit Idct(int size) : n(size) {
        const double pi = 3.14159265358979323846;
        for (int x = 0; x < n; ++x)
            for (int u = 0; u < n; ++u) {
                const double cu = u == 0 ? std::sqrt(0.5) : 1.0;
                table[x][u] = static_cast<float>(cu * std::cos((2 * x + 1) * u * pi / (2.0 * n)) / 2);
            }
    }

    // `coef` är blocket i radordning (8 × 8) med kvantiseringen; ut n × n
    // värden med nivåförskjutningen 128, klampade.
    void run(const int16_t* coef, const uint16_t* quant, uint8_t* out, size_t stride) const {
        float in[8][8];
        for (int v = 0; v < n; ++v)
            for (int u = 0; u < n; ++u)
                in[v][u] = static_cast<float>(coef[v * 8 + u]) * static_cast<float>(quant[v * 8 + u]);
        // Raderna först (u → x), sedan kolumnerna (v → y).
        float rows[8][8];
        for (int v = 0; v < n; ++v)
            for (int x = 0; x < n; ++x) {
                float s = 0;
                for (int u = 0; u < n; ++u) s += table[x][u] * in[v][u];
                rows[v][x] = s;
            }
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                float s = 0;
                for (int v = 0; v < n; ++v) s += table[y][v] * rows[v][x];
                const float val = std::round(s + 128.0f);
                out[static_cast<size_t>(y) * stride + static_cast<size_t>(x)] =
                    static_cast<uint8_t>(std::clamp(val, 0.0f, 255.0f));
            }
    }
};

uint8_t clamp8(float v) {
    return static_cast<uint8_t>(std::clamp(std::round(v), 0.0f, 255.0f));
}

} // namespace

bool looks_like_jpeg(const uint8_t* data, size_t size) {
    return size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
}

bool decode_jpeg(const uint8_t* data, size_t size, int max_side, DecodedImage& out,
                 std::string& error) {
    out = DecodedImage{};
    try {
        auto d = std::make_unique<Decoder>();
        d->data = data;
        d->size = size;
        if (!d->parse()) {
            error = d->error;
            return false;
        }
        const int W = d->width, H = d->height;

        // DCT-skalan: den minsta av 1, 2, 4, 8 som får bilden att rymmas.
        int scale = 1;
        if (max_side > 0)
            while (scale < 8 && std::max((W + scale - 1) / scale, (H + scale - 1) / scale) > max_side)
                scale *= 2;
        const int n = 8 / scale;
        const int sw = (W + scale - 1) / scale, sh = (H + scale - 1) / scale;
        const Idct idct(n);

        // Varje komponent till ett plan i sin egen upplösning.
        struct Plane {
            int w = 0, h = 0;          // giltiga samplingar
            int stride = 0;
            int rx = 1, ry = 1;        // uppsamplingen i x och y
            std::vector<uint8_t> px;
        };
        std::vector<Plane> planes(d->comps.size());
        for (size_t ci = 0; ci < d->comps.size(); ++ci) {
            Component& c = d->comps[ci];
            Plane& p = planes[ci];
            p.stride = c.bw * n;
            p.px.assign(static_cast<size_t>(p.stride) * static_cast<size_t>(c.bh * n), 0);
            p.w = (c.width + scale - 1) / scale;
            p.h = (c.height + scale - 1) / scale;
            // Att faktorerna går jämnt upp är prövat i bildramen.
            p.rx = d->hmax / c.h;
            p.ry = d->vmax / c.v;
            for (int by = 0; by < c.bh; ++by)
                for (int bx = 0; bx < c.bw; ++bx)
                    idct.run(d->block(c, bx, by), c.quant.data(),
                             &p.px[static_cast<size_t>(by * n) * static_cast<size_t>(p.stride) +
                                   static_cast<size_t>(bx * n)],
                             static_cast<size_t>(p.stride));
            std::vector<int16_t>().swap(c.coef);
        }

        // En komponents värde vid utbildens pixel: dubbel upplösning med
        // triangelfilter (3/4 närmast, 1/4 grannen), annars upprepning.
        auto sample_row = [](const Plane& p, int y, int sw_out, std::vector<float>& row) {
            row.resize(static_cast<size_t>(sw_out));
            auto at = [&](int xx, int yy) {
                xx = std::clamp(xx, 0, p.w - 1);
                yy = std::clamp(yy, 0, p.h - 1);
                return static_cast<float>(p.px[static_cast<size_t>(yy) * static_cast<size_t>(p.stride) +
                                                static_cast<size_t>(xx)]);
            };
            auto column = [&](int xx) {
                if (p.ry == 2) {
                    const int cy = y / 2, ny = (y & 1) ? cy + 1 : cy - 1;
                    return 0.75f * at(xx, cy) + 0.25f * at(xx, ny);
                }
                return at(xx, y / p.ry);
            };
            for (int x = 0; x < sw_out; ++x) {
                if (p.rx == 2) {
                    const int cx = x / 2, nx = (x & 1) ? cx + 1 : cx - 1;
                    row[static_cast<size_t>(x)] = 0.75f * column(cx) + 0.25f * column(nx);
                } else {
                    row[static_cast<size_t>(x)] = column(x / p.rx);
                }
            }
        };

        const bool rgb_source =
            d->comps.size() == 3 &&
            (d->adobe_transform == 0 ||
             (d->adobe_transform < 0 && !d->jfif && d->comps[0].id == 'R' && d->comps[1].id == 'G' &&
              d->comps[2].id == 'B'));
        std::vector<uint8_t> img(static_cast<size_t>(sw) * static_cast<size_t>(sh) * 4);
        std::vector<float> r0, r1, r2;
        for (int y = 0; y < sh; ++y) {
            uint8_t* row = &img[static_cast<size_t>(y) * static_cast<size_t>(sw) * 4];
            sample_row(planes[0], y, sw, r0);
            if (planes.size() == 1) {
                for (int x = 0; x < sw; ++x) {
                    const uint8_t g = clamp8(r0[static_cast<size_t>(x)]);
                    row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = g;
                    row[x * 4 + 3] = 255;
                }
                continue;
            }
            sample_row(planes[1], y, sw, r1);
            sample_row(planes[2], y, sw, r2);
            for (int x = 0; x < sw; ++x) {
                const size_t i = static_cast<size_t>(x);
                if (rgb_source) {
                    row[x * 4] = clamp8(r0[i]);
                    row[x * 4 + 1] = clamp8(r1[i]);
                    row[x * 4 + 2] = clamp8(r2[i]);
                } else {
                    const float Y = r0[i], cb = r1[i] - 128.0f, cr = r2[i] - 128.0f;
                    row[x * 4] = clamp8(Y + 1.402f * cr);
                    row[x * 4 + 1] = clamp8(Y - 0.344136f * cb - 0.714136f * cr);
                    row[x * 4 + 2] = clamp8(Y + 1.772f * cb);
                }
                row[x * 4 + 3] = 255;
            }
        }

        // Ryms den ändå inte: medelvärde över f × f pixlar.
        int f = 1;
        if (max_side > 0)
            while (std::max((sw + f - 1) / f, (sh + f - 1) / f) > max_side) ++f;
        out.source_width = W;
        out.source_height = H;
        if (f == 1) {
            out.width = sw;
            out.height = sh;
            out.rgba = std::move(img);
            return true;
        }
        out.width = (sw + f - 1) / f;
        out.height = (sh + f - 1) / f;
        out.rgba.assign(static_cast<size_t>(out.width) * static_cast<size_t>(out.height) * 4, 0);
        for (int y = 0; y < out.height; ++y)
            for (int x = 0; x < out.width; ++x) {
                unsigned sum[4] = {0, 0, 0, 0};
                unsigned count = 0;
                for (int yy = y * f; yy < std::min(sh, y * f + f); ++yy)
                    for (int xx = x * f; xx < std::min(sw, x * f + f); ++xx) {
                        const uint8_t* s =
                            &img[(static_cast<size_t>(yy) * static_cast<size_t>(sw) + static_cast<size_t>(xx)) * 4];
                        for (int k = 0; k < 4; ++k) sum[k] += s[k];
                        ++count;
                    }
                uint8_t* t = &out.rgba[(static_cast<size_t>(y) * static_cast<size_t>(out.width) +
                                        static_cast<size_t>(x)) *
                                       4];
                for (int k = 0; k < 4; ++k) t[k] = static_cast<uint8_t>((sum[k] + count / 2) / count);
            }
        return true;
    } catch (const std::bad_alloc&) {
        out = DecodedImage{};
        error = "minnet räckte inte för bilden";
        return false;
    }
}

} // namespace nwdview
