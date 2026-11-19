[English](README.md) · **Svenska**

# HNwd

Ett C++20-bibliotek som läser Navisworks-filer (`.nwd`, `.nwc`) utan
Navisworks och utan beroenden. En publik header, `include/HNwd.h`.

**Läge: tidig utveckling, version 0.1.0.** [TODO.md](TODO.md)
säger vad som återstår.

## Vad det läser

- **Geometrin:** trianglar, linjer, punkter, cirklar, cylindermantlar och text
  (sträng, låda och vridning; inte glyfer), med normaler, färger och UV.
- **Placeringen i världen:** varje fragments pose, med `world_point()` och
  `world_normal()`, och dess världslåda.
- **Materialen:** färgerna per utseende, och materialbeskrivningen med
  grundfärg, inbäddad bild (JPEG, PNG) och UV-transform.
- **Objektträdet** med namn, flikar och egenskaper: helt i minnet, eller som
  ett skelett där en nods egenskaper läses först när de behövs
  (`TreeContent::Structure` med `PropertyReader`).
- **Modellerna** i en fil med flera, med källa, inläsare och längdenhet.
- **Ett rumsligt träd** över fragmenten, med världslåda och antal primitiver
  per nod, för att välja vad som ska läsas efter detaljnivå: filens eget,
  eller ett som HNwd bygger.
- **Vyerna:** den aktuella vyn, hemvyn och de sparade vyerna i mappar, med
  snittplan och markeringar. **Rutnät** med linjer och nivåer, typsnitt,
  scenens material, bakgrund, ljus, filens identitet och inläsarens logg.
- **Trådar:** const-anropen får göras från flera trådar, varje tråd som läser
  geometri eller egenskaper har en egen `GeometryReader` eller
  `PropertyReader`, och `load_tree()` får köras medan andra läser.
- **Förlopp och avbrott:** de långa anropen (`load_geometry()`,
  `load_fragments()`, `load_spatial()`, `load_tree()`) tar en funktion som
  får veta hur långt de har kommit och kan stoppa dem (`Status::Cancelled`).
- **Minnet:** `memory_bytes()` säger vad dokumentet håller. Filen minnesmappas
  och läses genom sin katalog; geometrin packas upp bit för bit genom en cache
  med tak, så en fil på flera GB kostar bara det som används.

## Läsa en fil

```cpp
#include "HNwd.h"

HNwd::Document doc;
if (doc.open("model.nwd") != HNwd::Status::Ok)
    return 1;                                   // doc.last_error() säger varför
if (doc.load_fragments() != HNwd::Status::Ok)   // placering, utseenden, material
    return 1;

HNwd::Fragment f;
HNwd::Mesh mesh;
for (size_t i = 0; i < doc.fragment_count(); ++i) {
    doc.fragment(i, f);
    if (doc.fragment_mesh(i, mesh) != HNwd::Status::Ok)
        continue;                               // t.ex. kvantiserade positioner: Unsupported
    if (mesh.kind == HNwd::MeshKind::Text || mesh.positions.empty())
        continue;
    double world[3];
    HNwd::world_point(f.pose, &mesh.positions[0], world);   // första vertexen
}

HNwd::Unit unit;
if (doc.length_unit(unit) == HNwd::Status::Ok) {
    // unit.metres_per_unit räknar om filens koordinater till meter
}

// Trädet utan egenskaper; en nods egenskaper läses när de behövs.
if (doc.fragment_count() > 0 &&
    doc.load_tree(HNwd::TreeContent::Structure) == HNwd::Status::Ok) {
    const uint32_t n = doc.node_of_fragment(0);   // objektet fragment 0 hör till
    HNwd::Node node;
    doc.node(n, node);                            // node.name
    HNwd::PropertyReader reader;
    HNwd::NodeProperties props;
    if (reader.open(doc) == HNwd::Status::Ok && reader.read(n, props) == HNwd::Status::Ok) {
        // props.tabs och props.properties
    }
}
```

## Verktyg och visare

Byggs när HNwd byggs för sig självt; inte del av biblioteket.

- `nwddump <fil>` visar sektionerna, katalogen och varje paket;
  `--spatial`, `--units`, `--elements` och `--partition` tolkar delar av det.
- `nwd2obj <in> <ut.obj> [--local] [--offset x y z] [--max-fragments N] [--no-mtl]`
  skriver geometrin som OBJ i världskoordinater, med material och bilder.
- `nwdcheck <fil> --poses` prövar varje fragments pose mot trädets
  transformer.
- `nwdsynth <ut.nwd> --fragments N --records M --nodes K` skriver en giltig
  syntetisk fil av valfri storlek, för prov i stor skala.
- `nwdview [fil]` visar en modell i 3D, läst i arbetstrådar: det som är minst
  några pixlar i bild byggs, inom en minnesbudget för RAM (`--budget MB`) och
  en för grafikkortet (`--gpu-budget MB`), båda satta efter maskinen som
  förval. Ett klick på ett objekt visar dess
  namn och egenskaper; materialens bilder, text, rutnät och sparade vyer
  ritas; F2 går i första person. `--view <namn>` startar i en sparad vy, och
  `nwdview <fil> --png <bild.png>` ritar modellen till en bild och avslutar.

## Bygga

Krav: CMake 3.20+, Ninja och en C++20-kompilator. Utvecklas på Windows med
MSYS2 UCRT64 (GCC); koden skrivs för Windows, Linux och macOS.

```sh
cmake -B build -G Ninja
cmake --build build
./build/tests/HNwd_tests      # från repots rot
```

- Utan byggtyp blir bygget Release. `-DBUILD_SHARED_LIBS=ON` bygger delade
  bibliotek.
- `HNWD_BUILD_TESTS` (tester och verktyg), `HNWD_BUILD_VIEWER` (visaren) och
  `HNWD_INSTALL` (installationsregler och CMake-paket) är på när HNwd byggs
  för sig självt och av när ett annat projekt drar in det med FetchContent
  eller `add_subdirectory`. Ett sådant projekt får bara biblioteket: länka
  `HNwd::HNwd`.
- Visaren bygger på [GlazeUI](https://github.com/holmbom/GlazeUI), som CMake
  hämtar vid konfigureringen; den kräver CMake 3.24.
  `-DHNWD_BUILD_VIEWER=OFF` bygger tester och verktyg utan den, och utan nät.
- `cmake --install` ger biblioteken, `HNwd.h` och ett CMake-paket:
  `find_package(HNwd)` ger `HNwd::HNwd`.

Testfilerna ligger inte i repot. De är ett publikt exempelprojekt som hämtas
från
<https://download.bimcollab.com/support/ZOOM/BIMcollab_Navisworks-example-file.zip>
och packas upp i `nwd/bimcollab/`. Ett test vars fil saknas skriver `SKIP`;
[tests/test_data.h](tests/test_data.h) har kommandona och sökvägarna.

## Begränsningar

- Kvantiserade positioner avkodas inte; sådana poster ger `Unsupported`.
- Text ger sin sträng, låda, vridning och sitt typsnittsindex, inte sina
  glyfer.
- Andra enhetskoder än 0–3 (meter, centimeter, millimeter, fot) är inte kända.
- Fält vars betydelse inte är känd lämnas inte ut: se
  [LIMITATIONS.md](LIMITATIONS.md),
  [doc/format.md](doc/format.md) och
  [doc/research/element.md](doc/research/element.md).
- Linux och macOS är skrivna för men ännu inte provade.

## Dokumentation

- [README.md](README.md): beskrivningen på engelska.
- [CLAUDE.md](CLAUDE.md): utvecklarguiden: moduler, regler och bygget.
  Skriven för både människor och AI-verktyg.
- [doc/format.md](doc/format.md): filformatet, med vad som är prövat.
- [doc/CHANGELOG.md](doc/CHANGELOG.md) och [TODO.md](TODO.md): vad som är
  gjort och vad som återstår.
- [LIMITATIONS.md](LIMITATIONS.md): det som inte är avkodat, och kända
  svagheter (på engelska).

## Ursprung

HNwd började som en kopia av [NwcIO](https://github.com/IOExtend/NwcIO) av
mahach666 (MIT), ett .NET-bibliotek som läser samma filer. HNwd är skrivet från
grunden i C++ och delar inga källfiler med det; kunskapen om filformatet bygger
på NwcIO:s läsare och formatanteckningar. Se
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Licens

[MIT](LICENSE) © 2026 Erik Holmbom

Inte knutet till Autodesk. Navisworks är ett varumärke som tillhör Autodesk, Inc.
