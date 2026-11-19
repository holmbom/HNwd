// Visarens JPEG-avkodare (viewer/jpeg.h): små strömmar byggda i koden, giltiga
// och trasiga. En trasig ström ger false med en text, aldrig en krasch eller
// en bild.

#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

#include "jpeg.h"
#include "test_support.h"

using testdata::Bytes;

namespace {

// En JPEG-ström som byggs segment för segment.
struct Jpeg {
    Bytes b;

    Jpeg() { b = {0xFF, 0xD8}; }

    void bytes(std::initializer_list<int> v) {
        for (int x : v) b.push_back(static_cast<uint8_t>(x));
    }
    void segment(int marker, const Bytes& body) {
        const size_t len = body.size() + 2;
        bytes({0xFF, marker, static_cast<int>(len >> 8), static_cast<int>(len & 0xFF)});
        b.insert(b.end(), body.begin(), body.end());
    }
    // Kvantiseringstabell 0, 8 bitar, alla 1.
    void dqt() {
        Bytes body(65, 1);
        body[0] = 0x00;
        segment(0xDB, body);
    }
    // En Huffmantabell med en enda kod av längden 1 för `symbol`.
    void dht(int cls, int id, int symbol) {
        Bytes body(18, 0);
        body[0] = static_cast<uint8_t>(cls << 4 | id);
        body[1] = 1;
        body[17] = static_cast<uint8_t>(symbol);
        segment(0xC4, body);
    }
    struct Comp {
        int id, hv;
    };
    void sof(int marker, int width, int height, std::initializer_list<Comp> comps) {
        Bytes body = {8, static_cast<uint8_t>(height >> 8), static_cast<uint8_t>(height),
                      static_cast<uint8_t>(width >> 8), static_cast<uint8_t>(width),
                      static_cast<uint8_t>(comps.size())};
        for (const Comp& c : comps) {
            body.push_back(static_cast<uint8_t>(c.id));
            body.push_back(static_cast<uint8_t>(c.hv));
            body.push_back(0);
        }
        segment(marker, body);
    }
    // En skanning med komponenterna `ids`, alla med tabellerna 0.
    void sos(std::initializer_list<int> ids, int ss = 0, int se = 63, int ahal = 0) {
        Bytes body = {static_cast<uint8_t>(ids.size())};
        for (int id : ids) {
            body.push_back(static_cast<uint8_t>(id));
            body.push_back(0x00);
        }
        body.push_back(static_cast<uint8_t>(ss));
        body.push_back(static_cast<uint8_t>(se));
        body.push_back(static_cast<uint8_t>(ahal));
        segment(0xDA, body);
    }
    void zeros(size_t n) { b.insert(b.end(), n, 0); }
    void eoi() { bytes({0xFF, 0xD9}); }
};

// Bitar med JPEG:s utfyllnad: 0xFF följs av 0x00, och sista byten fylls med ettor.
struct Bits {
    Bytes out;
    uint32_t acc = 0;
    int n = 0;
    void put(uint32_t v, int count) {
        for (int i = count - 1; i >= 0; --i) {
            acc = acc << 1 | ((v >> i) & 1);
            if (++n == 8) flush();
        }
    }
    void flush() {
        out.push_back(static_cast<uint8_t>(acc));
        if (acc == 0xFF) out.push_back(0x00);
        acc = 0;
        n = 0;
    }
    void finish() {
        while (n != 0) put(1, 1);
    }
};

// Tabellerna för de enklaste blocken: DC-skillnad 0 och direkt EOB, två
// nollbitar per block.
void plain_tables(Jpeg& j) {
    j.dht(0, 0, 0);
    j.dht(1, 0, 0);
}

bool decode(const Jpeg& j, nwdview::DecodedImage& img, std::string& error) {
    return nwdview::decode_jpeg(j.b.data(), j.b.size(), 0, img, error);
}

bool decodes_gray(const Jpeg& j, int w, int h) {
    nwdview::DecodedImage img;
    std::string error;
    if (!decode(j, img, error)) {
        std::printf("  (avkodningen gav: %s)\n", error.c_str());
        return false;
    }
    if (img.width != w || img.height != h || img.rgba.size() != size_t(w) * size_t(h) * 4) return false;
    for (size_t i = 0; i < img.rgba.size(); i += 4)
        if (img.rgba[i] != 128 || img.rgba[i + 1] != 128 || img.rgba[i + 2] != 128 || img.rgba[i + 3] != 255)
            return false;
    return true;
}

bool rejected(const Jpeg& j) {
    nwdview::DecodedImage img;
    std::string error;
    return !decode(j, img, error) && !error.empty() && img.rgba.empty() && img.width == 0;
}

} // namespace

int test_jpeg() {
    int fails = 0;

    // Giltiga: gråskala 8 × 8, en komponent med samplingsfaktorn 2 × 2 (en
    // ensam komponent avkodas utan sammanflätning), YCbCr 4:2:0 sammanflätad
    // och tre skanningar med en komponent var i valfri ordning.
    {
        Jpeg j;
        j.dqt();
        j.sof(0xC0, 8, 8, {{1, 0x11}});
        plain_tables(j);
        j.sos({1});
        j.zeros(1);
        j.eoi();
        CHECK(decodes_gray(j, 8, 8));
    }
    {
        Jpeg j;
        j.dqt();
        j.sof(0xC0, 8, 8, {{1, 0x22}});
        plain_tables(j);
        j.sos({1});
        j.zeros(1);
        j.eoi();
        CHECK(decodes_gray(j, 8, 8));
    }
    {
        Jpeg j;
        j.dqt();
        j.sof(0xC0, 16, 16, {{1, 0x22}, {2, 0x11}, {3, 0x11}});
        plain_tables(j);
        j.sos({1, 2, 3});
        j.zeros(2);   // sex block
        j.eoi();
        CHECK(decodes_gray(j, 16, 16));
    }
    {
        Jpeg j;
        j.dqt();
        j.sof(0xC0, 16, 16, {{1, 0x22}, {2, 0x11}, {3, 0x11}});
        plain_tables(j);
        j.sos({3});
        j.zeros(1);
        j.sos({1});
        j.zeros(1);   // fyra block
        j.sos({2});
        j.zeros(1);
        j.eoi();
        CHECK(decodes_gray(j, 16, 16));
    }

    // Samma komponent två gånger i en skanning: den sammanflätade vägen skulle
    // räkna block som inte finns i komponentens lagring.
    {
        Jpeg j;
        j.dqt();
        j.sof(0xC0, 8, 8, {{1, 0x22}});
        plain_tables(j);
        j.sos({1, 1});
        j.zeros(4);
        j.eoi();
        CHECK(rejected(j));
    }
    {
        Jpeg j;
        j.dqt();
        j.sof(0xC0, 16, 16, {{1, 0x22}, {2, 0x11}, {3, 0x11}});
        plain_tables(j);
        j.sos({1, 2, 2});
        j.zeros(8);
        j.eoi();
        CHECK(rejected(j));
    }
    // Komponenterna i fel ordning, fler komponenter än bilden har, en som
    // inte finns.
    {
        Jpeg j;
        j.dqt();
        j.sof(0xC0, 16, 16, {{1, 0x22}, {2, 0x11}, {3, 0x11}});
        plain_tables(j);
        j.sos({2, 1, 3});
        j.zeros(8);
        j.eoi();
        CHECK(rejected(j));
    }
    {
        Jpeg j;
        j.dqt();
        j.sof(0xC0, 8, 8, {{1, 0x11}});
        plain_tables(j);
        j.sos({1, 2});
        j.zeros(4);
        j.eoi();
        CHECK(rejected(j));
    }
    {
        Jpeg j;
        j.dqt();
        j.sof(0xC0, 8, 8, {{1, 0x11}});
        plain_tables(j);
        j.sos({7});
        j.zeros(4);
        j.eoi();
        CHECK(rejected(j));
    }
    // Två komponenter med samma id i bildramen.
    {
        Jpeg j;
        j.dqt();
        j.sof(0xC0, 16, 16, {{1, 0x22}, {1, 0x11}, {3, 0x11}});
        plain_tables(j);
        j.sos({1});
        j.zeros(4);
        j.eoi();
        CHECK(rejected(j));
    }
    // Samplingsfaktorer som inte går jämnt upp.
    {
        Jpeg j;
        j.dqt();
        j.sof(0xC0, 24, 8, {{1, 0x31}, {2, 0x21}, {3, 0x11}});
        plain_tables(j);
        j.sos({1, 2, 3});
        j.zeros(8);
        j.eoi();
        CHECK(rejected(j));
    }
    // En skanning utan rubrik sist i filen.
    {
        Jpeg j;
        j.dqt();
        j.sof(0xC0, 8, 8, {{1, 0x11}});
        plain_tables(j);
        j.bytes({0xFF, 0xDA, 0x00, 0x02});
        CHECK(rejected(j));
    }
    // Progressiv förfining där Al inte är Ah − 1.
    {
        Jpeg j;
        j.dqt();
        j.sof(0xC2, 8, 8, {{1, 0x11}});
        plain_tables(j);
        j.sos({1}, 0, 0, 0x01);
        j.zeros(1);
        j.sos({1}, 0, 0, 0x20);
        j.zeros(1);
        j.eoi();
        CHECK(rejected(j));
    }
    // DC-prediktorn växer med 2047 per block: efter 17 block ryms den inte i
    // 16 bitar.
    {
        Jpeg j;
        j.dqt();
        j.sof(0xC0, 8 * 17, 8, {{1, 0x11}});
        j.dht(0, 0, 11);
        j.dht(1, 0, 0);
        j.sos({1});
        Bits bits;
        for (int k = 0; k < 17; ++k) {
            bits.put(0, 1);        // DC-koden: 11 bitar följer
            bits.put(0x7FF, 11);   // +2047
            bits.put(0, 1);        // EOB
        }
        bits.finish();
        j.b.insert(j.b.end(), bits.out.begin(), bits.out.end());
        j.eoi();
        CHECK(rejected(j));

        // 16 block ryms (16 · 2047 = 32752).
        Jpeg k;
        k.dqt();
        k.sof(0xC0, 8 * 16, 8, {{1, 0x11}});
        k.dht(0, 0, 11);
        k.dht(1, 0, 0);
        k.sos({1});
        Bits kb;
        for (int n = 0; n < 16; ++n) {
            kb.put(0, 1);
            kb.put(0x7FF, 11);
            kb.put(0, 1);
        }
        kb.finish();
        k.b.insert(k.b.end(), kb.out.begin(), kb.out.end());
        k.eoi();
        nwdview::DecodedImage img;
        std::string error;
        CHECK(decode(k, img, error) && img.width == 128 && img.height == 8);
    }

    std::printf("test_jpeg: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
