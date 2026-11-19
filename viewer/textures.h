#pragma once

// Materialens bilder: avkodningen (PNG genom GlazeUI, JPEG med visarens
// egen avkodare) med nedskalning, och programmet som ritar en texturerad yta
// med bilden upprepad.
//
// GlazeUI:s inbyggda mesh-program samplar bilden med kanten utdragen
// (clamp), och UV-värdena i materialen ligger långt utanför 0..1. Därför
// ritas texturerade delar med ett eget program som tar bråkdelen av
// koordinaten (per axel, efter materialets upprepning) och samplar med
// koordinatens derivator, så att mip-nivån inte hoppar vid skarven.
//
// Alla visarens meshar ritas med visarens egna program, med och utan bild,
// som belyser en yta lika från båda sidor: normalen vänds mot betraktaren,
// och det som vänder sig bort från ljuset får ett motljus. Modeller har ofta
// enkelsidiga ytor och blandad vridningsriktning; sedda bakifrån blir de
// annars mörka. Programmen klipper också mot vyns snittplan.
//
// Modellens linjer ritas av ett tredje program. En linje är en rad segment, och
// varje segment är en kvad med fyra hörn: två vid vardera änden. Ett hörn bär
// sin egen ände (läget), segmentets andra ände och vilken sida av linjen det
// ligger på. Programmet projicerar båda ändarna, räknar linjens riktning i
// bildens px och flyttar hörnet vinkelrätt mot den, så att linjen får en fast
// bredd i px vid varje avstånd och vinkel. En ände bakom ögat klipps mot
// närplanet innan riktningen räknas; ett segment helt bakom ögat ritas inte.

#include "jpeg.h"
#include "model.h"

#include <glaze/glaze.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nwdview {

// Bildernas största sida efter nedskalningen.
inline constexpr int kMaxTextureSide = 1024;

// Avkodar en bildfil efter dess första byte (PNG eller JPEG) till RGBA8
// och skalar ner den med en heltalsfaktor (JPEG: 1/2, 1/4, 1/8 i
// DCT-domänen först) tills båda sidorna ryms i `max_side`. Trådsäker.
bool decode_texture(const uint8_t* data, size_t size, int max_side, DecodedImage& out,
                    std::string& error);

// Är byten en bild som decode_texture kan försöka avkoda?
bool texture_format_known(const uint8_t* data, size_t size);

// Programmens namn i ritytan och programmen: med bilden upprepad, och utan
// bild.
inline constexpr const char* kRepeatProgram = "nwdview_repeat";
inline constexpr const char* kLitProgram = "nwdview_lit";
glaze::Program repeat_program();
glaze::Program lit_program();

// Linjeprogrammet och linjernas bredd i logiska px (ritytans skala läggs på i
// programmet). Programmet läser läget (plats 0), färgen (plats 2) och user0
// (plats 5: xyz är segmentets andra ände relativt meshens origo, w är sidan,
// −1 eller +1).
inline constexpr const char* kLineProgram = "nwdview_line";
inline constexpr float kLineWidthPx = 1.0f;
glaze::Program line_program();

// GlazeUser-blocket för linjedelen: bredden i logiska px, och sedan snittets
// plan som tomma (set_line_clip_uniforms() fyller dem).
std::vector<float> line_uniforms(float width_px);

// Som set_clip_uniforms() men för linjeprogrammets block.
void set_line_clip_uniforms(std::vector<float>& uniforms, const ClipState& clip,
                            const double origin[3]);

// GlazeUser-blocket för en del: ljuset (som ritytans, med `rim` som
// motljusets styrka) och upprepningen per axel (bara med bild). Snittets
// plan står sist och är tomma här (inget klipps); set_clip_uniforms() fyller
// dem.
std::vector<float> lit_uniforms(const glaze::Lighting& light, bool repeat_u, bool repeat_v);

// Skriver snittets plan i en dels GlazeUser-block. Programmen får meshens
// lägen relativt dess origo (`origin`, meter), så planen flyttas dit: varje
// plan blir (n, d + n · origin). Ett fragment där n · läget + d < 0 för
// något plan ritas inte (discard). Planen utöver `clip.count` är tomma och
// klipper inget.
void set_clip_uniforms(std::vector<float>& uniforms, const ClipState& clip, const double origin[3]);

} // namespace nwdview
