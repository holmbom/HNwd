#pragma once

// En JPEG-avkodare utan beroenden: JPEG-filens byte till RGBA8.
//
// Det som läses: baseline och utökad sekventiell Huffman (SOF0, SOF1) och
// progressiv Huffman (SOF2), 8 bitar, en komponent (gråskala) eller tre
// (YCbCr, eller RGB när filen säger det), samplingsfaktorer 1–4 per
// komponent, omstartsmarkörer. Allt annat — aritmetisk kodning, förlustfri,
// hierarkisk, 12 bitar, fyra komponenter (CMYK) — ger false med en text,
// liksom en fil som bryter formatet. En gissad bild lämnas aldrig ut.
//
// Stora bilder skalas ner när de avkodas: först med 1/2, 1/4 eller 1/8 i
// DCT-domänen (bara de lägre frekvenserna räknas), sedan vid behov med en
// heltalsfaktor genom medelvärde, tills båda sidorna ryms i `max_side`.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nwdview {

struct DecodedImage {
    int width = 0;                 // efter nedskalningen
    int height = 0;
    int source_width = 0;          // som filen anger den
    int source_height = 0;
    std::vector<uint8_t> rgba;     // width × height × 4, rad 0 överst, alfa 255
};

// Största bild som avkodas, räknat i koefficienternas minne.
inline constexpr size_t kMaxJpegCoefficientBytes = size_t(512) << 20;

// Avkodar `data`. `max_side` ≤ 0 skalar inte ner. false och `error` satt
// när filen inte går att avkoda; `out` är då tom.
bool decode_jpeg(const uint8_t* data, size_t size, int max_side, DecodedImage& out,
                 std::string& error);

// Börjar byten som en JPEG-fil (SOI-markören)?
bool looks_like_jpeg(const uint8_t* data, size_t size);

} // namespace nwdview
