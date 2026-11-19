# HNwd — utvecklarguide

C++20-bibliotek som läser Navisworks-filer (`.nwd`, `.nwc`) utan Navisworks.
Inga beroenden, MIT, en publik header.

## Bygga

Verktygskedjan är MSYS2 UCRT64: GCC, CMake och Ninja ur `C:\msys64\ucrt64\bin`.

```bash
cmake -B build -G Ninja -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Utan byggtyp blir bygget Release. Varningarna prövas i Release: optimeringen
hittar sådant ett Debug-bygge inte ser. Bygget ska ge **0 varningar** på
`-Wall -Wextra -Wpedantic`; varje nytt mål får nivån genom `hnwd_warnings()` i
[CMakeLists.txt](CMakeLists.txt). Med MSVC (`vcvars64.bat`, sedan samma
kommandon med `-DCMAKE_CXX_COMPILER=cl -DHNWD_BUILD_VIEWER=OFF` i en egen
byggkatalog) gäller samma krav på `/W4`; visaren byggs inte där.

`HNWD_BUILD_TESTS` styr tester och `tools/`, `HNWD_BUILD_VIEWER` visaren och
`HNWD_INSTALL` installationsreglerna. Alla tre är på när HNwd är
toppnivåprojektet och av när det dras in med FetchContent eller
`add_subdirectory`.

Visaren `nwdview` är det enda med ett beroende: den hämtar GlazeUI (en fast
tagg i [viewer/CMakeLists.txt](viewer/CMakeLists.txt)) vid konfigureringen, och
kräver CMake 3.24. Ett projekt som använder HNwd som bibliotek hämtar aldrig
GlazeUI. `-DHNWD_BUILD_VIEWER=OFF` bygger tester och verktyg utan den, och
utan nät. `cmake --install` ger biblioteken, `HNwd.h` och
CMake-paketet, så att `find_package(HNwd)` ger `HNwd::HNwd`, `HNwd::core` och `HNwd::scene`.

## Tester

Den kompletta sviten är en körbar, och den körs från repots rot så att `nwd/`
hittas:

```bash
./build/tests/HNwd_tests
```

Körd från en annan katalog hittas inte testfilerna, och slutraden säger då hur
många prov som hoppades över.

Testfilerna ligger i `nwd/`, som inte är i git. De är ett publikt
exempelprojekt som hämtas från
<https://download.bimcollab.com/support/ZOOM/BIMcollab_Navisworks-example-file.zip>
och packas upp i `nwd/bimcollab/`; kommandona står i `test_data.h`. Ett test
vars fil saknas
skriver `SKIP` (`SKIP_IF_MISSING` i [tests/test_data.h](tests/test_data.h)).
Sökvägar till testdata och förväntade värden står i `test_data.h`, inte i
varje testfil.

Ett nytt test är en funktion `int test_x()` i `tests/test_x.cpp` som returnerar
antalet fel, deklareras och anropas i [tests/main.cpp](tests/main.cpp) och läggs
till i [tests/CMakeLists.txt](tests/CMakeLists.txt).

## Struktur

- `include/HNwd.h` — den enda publika headern. Versionstalen där är samma som
  `project(VERSION)` i `CMakeLists.txt` och ändras tillsammans med den.
- `src/` — moduler, ett CMake-mål per katalog. **Beroenden pekar åt ett håll**,
  och gränsen upprätthålls av include-stigarna i modulens `CMakeLists.txt`, så
  ett felaktigt beroende blir ett kompileringsfel.

| Modul | Mål | Innehåll | Beror på |
|---|---|---|---|
| `src/core/` | `HNwd::core` | filformatet, ett paket per fil | inget |
| `src/scene/` | `HNwd::scene` | det publika API:t `Document`: sätter ihop paketen, prövar att de stämmer med varandra, expanderar trädet | core |

Filerna i `src/core/`, i den ordning de bygger på varandra:

| Fil | Innehåll |
|---|---|
| `path_utf8.h` | UTF-8-sökvägar, och enda stället som drar in `windows.h` |
| `nwd_error.h/.cpp` | `ErrorCode` och `error_message()` |
| `mapped_file.h/.cpp` | `MappedFile`: filen minnesmappad för läsning |
| `inflate.h/.cpp` | `inflate_zlib()` och `adler32()`: egen zlib-uppackning med tak; `InflateStream`: samma uppackning bit för bit, med kontrollpunkter att återuppta från |
| `byte_reader.h` | `ByteReader`: little-endian-läsning med gränskontroll, strängar och utfyllnad; `StreamWindow`: ett glidande fönster på en `InflateStream` |
| `dbcache.h/.cpp` | `parse_dbcache()`: katalogens poster |
| `container.h/.cpp` | `read_container()`: sektionsrubrikerna och katalogen; `last_stream()`, `split_prefixed()` och `inflate_span()`: strömmarna i ett paket |
| `object_reader.h/.cpp` | `ObjectReader`: objektströmmarnas pekarregel, id, nästlingsdjup, över en buffert eller ett `StreamWindow`; `valid_utf8()`, `skip_class185()` |
| `guid_store.h/.cpp` | `parse_guid_store()`: GUID-tabellen; `guid_text()` |
| `source_header.h/.cpp` | `parse_source_header()`: varifrån en `.nwc` kommer |
| `material.h/.cpp` | `read_class185()`: ett material som JSON med inbäddade bildfiler; `parse_material_json()`: grundfärgen, den inkopplade bilden och dess UV-transform |
| `pose.h/.cpp` | posens matematik: punkt, normal, linjär del, spegling och världslåda |
| `fragments.h/.cpp` | `parse_fragments()`: placeringen och utseendet för varje geometribit, ur en buffert eller strömmande ur en `InflateStream`, lagrade kompakt i `FragmentStore` |
| `partition_props.h/.cpp` | `locate_partition_props()`: var strömmarna med de egenskaper en nod använder ensam ligger; `read_partition_props()`: dem uppackade |
| `partition.h/.cpp` | `parse_partition_stream()`, `read_partition_tail()`: klass 32, noden för en inläst modell; `metres_per_unit()`: enhetskoden i meter |
| `hierarchy.h/.cpp` | `parse_hierarchy()`: objektträdet som lagrad graf, med flikar, egenskaper, material och placering, helt eller som skelett (`HierarchyContent`, `HierarchySkeleton`); `locate_hierarchy()`, `parse_node_tabs()` |
| `tab_reader.h/.cpp` | `TabReader`, `read_node_tabs()`: flikarna och egenskaperna för en nod, lästa ur filen från närmaste kontrollpunkt |
| `geometry.h/.cpp` | `parse_record()`: en geometripost till trianglar, linjer eller punkter, också cirklar, cylindermantlar och text; `decode_tokens()`, `decode_packed()`, `parse_geometry_toc()` |
| `geometry_segment.h/.cpp` | geometripaketet: `GeometryLayout` (oföränderlig när den är läst: chunkarnas och posternas lägen, normalernas bredd) och `GeometryCursor` (en läsares cache, en per tråd); `GeometrySegment` är de två tillsammans |
| `spatial.h/.cpp` | `parse_spatial_hierarchy()`: filens rumsliga träd över fragmenten |
| `elements.h/.cpp` | de små paketen: inläsarens logg, filens identitet, XRef-tabellen, vyerna (aktuell, hem och sparade, med snittplan och markeringar), rutnäten, typsnitten, materiallistan, ljusen, bakgrunden, kollisionstesterna och visningsinställningarna |

Filerna i `src/scene/`:

| Fil | Innehåll |
|---|---|
| `document_impl.h` | `Document::Impl`: det ett öppet dokument håller, och delarna läsarna delar; delas av filerna nedan |
| `document.cpp` | `Document` utom trädet: `open`, geometrin, fragmenten, materialen, enheten, GUID, källan, XRef-tabellen, `memory_bytes()`; `world_point()`, `world_normal()`, `mirrors()` |
| `geometry_reader.cpp` | `GeometryReader`: geometrin för en tråd, med länkkontrollerna mot fragmentet |
| `spatial_index.cpp` | `load_spatial()`: det rumsliga trädet med världslådor och primitivsummor, ur filen eller byggt här |
| `tree.cpp` | `load_tree()` och noderna: trädet expanderat i preorder, helt eller som skelett; modellerna |
| `elements_api.cpp` | vyerna, rutnäten, typsnitten, scenens material, bakgrunden, ljusen, filens identitet och loggen genom `Document`; `camera_forward()`, `clip_plane_count()` |
| `property_reader.cpp` | `PropertyReader`: en nods flikar och egenskaper, ur minnet eller ur filen |

Biblioteket startar inga trådar och har inga lås. Vad som får göras samtidigt
står vid `Document` i `HNwd.h`: const-anropen från flera trådar, en
`GeometryReader` och en `PropertyReader` per tråd, och `load_tree()` medan
andra trådar läser, eftersom trädet byggs vid sidan av och publiceras färdigt
(`tree_view` i `document_impl.h`).

`core` ser inte `HNwd.h`; `scene` är den enda modulen som gör det.
`HNwd::kNone` och `HNwd::detail::kNone` finns båda, så skriv inte
`using namespace detail` inne i `namespace HNwd`.

- `tests/` — sviten `HNwd_tests`. `test_support.h` bygger syntetiska strömmar
  och hela filer (`ByteWriter`, `zlib_store()`, `build_file()`, `prefixed()`),
  `inflate_vectors.h` är facit för uppackningen, och `test_data.h` har
  exempelfilerna med vad var och en innehåller (`kSamples`, `kFacts`) och
  `tmp_dir()` för temporärfiler. `alloc_probe.h` mäter den största
  allokeringen under ett anrop (testkörbaren ersätter `operator new`), och
  `fragment_view.h` lägger ut ett fragments fält för jämförelse. `test_poses`
  och `test_synth` använder `tools/pose_check.h` och `tools/synth_writer.h`.
  Sviten bygger in visarens delar utan fönster (`viewer/jpeg.cpp`,
  `model.cpp`, `cells.cpp`, `lod_tree.cpp`, `scheduler.cpp`, `governor.cpp`,
  `pick.cpp`, `instancing.cpp`, `sysmem.cpp`), så de prövas också utan
  visaren: `test_jpeg`, `test_viewer_cells`, `test_lod_tree`,
  `test_tile_scheduler`, `test_governor`, `test_pick`, `test_instancing` och
  `test_sysmem`.
- `tools/` — hjälpverktyg, inte del av biblioteket. `nwddump <fil>` visar
  sektionerna, katalogen och varje pakets sista ström uppackad (`--hex N`
  byte av varje), och med
  `--partition` klass 32 i paketet `LcOpNwdPartition`, med `--spatial` det
  rumsliga trädet, med `--units` filens längdenhet och med `--elements`
  de små paketen tolkade.
  `nwd2obj <in> <ut.obj> [--local] [--offset x y z] [--max-fragments N] [--no-mtl]`
  skriver geometrin som OBJ med material och bilder, i världskoordinater, en
  grupp per objekt. `nwdcheck <fil> --poses` prövar varje fragments pose mot
  trädets transformer (`pose_check.h`).
  `nwdsynth <ut.nwd> --fragments N --records M --nodes K [--no-spatial] [--unit k] [--seed S] [--sizes A:B] [--detail N]`
  skriver en giltig syntetisk fil av valfri storlek, för prov i stor skala
  (`synth_writer.h`); `--sizes` sprider posternas storlek mellan A och B
  meter och `--detail` är flest segment per post. Ett nytt
  verktyg läggs till med `hnwd_tool()` i `tools/CMakeLists.txt` och skriver
  `tool_run()` i stället för `main()` ([tools/tool_main.h](tools/tool_main.h)).
- `viewer/` — visaren `nwdview`, ett fönster byggt på GlazeUI med bara det
  publika API:t i `HNwd.h` (och `path_utf8.h` för argumenten). `nwdview [fil]` visar modellen; ett klick på ett
  objekt visar dess namn och egenskaper, och statusraden visar
  världskoordinaten i meter under markören. Modellen läses i arbetstrådar.
  Fragmenten ligger i bitar i ett detaljträd ovanpå det rumsliga indexet
  (`lod_tree.*`), stora fragment högt upp och små längst ner. En bit byggs
  när dess största fragment är minst tröskeln i bild (förval 3 px,
  `--min-pixels`), och bara till den fragmentstorlek som behövs på dess
  avstånd; de största bitarna är grundbilden, som byggs först och alltid
  ritas. Urvalet (`scheduler.*`) och ritningens tröskel (`governor.*`, som
  håller bildtiden) räknas i en egen tråd; UI-tråden tar bara emot färdiga
  svar och sätter former, i små portioner. Minnet har två budgetar,
  `--budget MB` för RAM och `--gpu-budget MB` för grafikkortet, med förval
  efter maskinen (`sysmem.*`); räcker det inte höjs tröskeln, och
  statusraden säger vilken som gäller. Klicket, koordinaten under markören
  och marken under ögat i gångläget träffprovas i arbetstrådar mot bitens
  geometri (`pick.*`). Materialens bilder ritas (egen JPEG-avkodare i
  `jpeg.*`), text ritas som etiketter, linjerna ritas som en del av bitens
  mesh, och en lista väljer vilka modeller som syns. `model.*`, `cells.*`,
  `lod_tree.*`, `scheduler.*`, `governor.*`, `pick.*`, `instancing.*`,
  `sysmem.*`, `walk.*` och `jpeg.*` är fria från GlazeUI och prövas i
  sviten. `instancing.*` planerar vilka geometriposter som kan ritas som
  instanser; den är inte inkopplad i fönstret. `textures.*` har bilderna
  och visarens ritprogram, i HLSL, GLSL och MSL, för ytor och för linjer,
  som klipper mot vyns snittplan.
  `--gpu integrated|discrete` väljer
  grafikkort och `--uv raw|realworld` hur UV-värdena läggs på bilden.
  Visaren startar i filens aktuella vy; `--view <namn>` startar i en sparad
  vy, listan Vyer byter vy, rutnäten ritas, och `--clip on|off` och
  `--clip-test "px py pz nx ny nz"` styr snittet. `--eye x y z --look x y z`
  sätter startkameran.
  F2 byter till ett gångläge i första person (`--walk-mode`, `--follow`,
  `--speed`); rörelsen räknas i `walk.*`,
  som är fri från GlazeUI. `nwdview <fil> --png <bild.png>` ritar modellen till
  en bild och avslutar, och `--profile` skriver tiderna — så går en ändring
  i läsaren att SE utan att någon sitter vid skärmen. `--tour` zoomar in och
  ut före bilden, `--walk <meter>` går och `--spin <varv>` snurrar före
  bilden; med `--profile` skriver de bildtiden, var i bilden tiden går och
  UI-trådens stopp. `--tile N` är största bit i trianglar, `--max-draws` och
  `--max-triangles` ritningens tak, och `--no-lines` stänger av linjerna.
  `nwdview --decode-jpeg <in.jpg> <ut.png>` prövar JPEG-avkodaren utan
  fönster. Regeln om inga beroenden gäller
  biblioteket, inte visaren.
- `doc/format.md` — filformatet, med vad som är prövat och vad som är övertaget.
- `doc/plans/` — planer. `doc/research/` — anteckningar från filanalys
  (`klass32.md`, och `element.md` om de små paketen).

`HNwd` är ett INTERFACE-paraply som länkar alla moduler. En ny modul läggs till
i `src/CMakeLists.txt` och i listan `hnwd_modules` i rot-`CMakeLists.txt`.

## Ursprung och hårda regler

HNwd bygger på NwcIO (<https://github.com/IOExtend/NwcIO>, .NET, MIT): hur
filerna läses kommer därifrån. Tillskrivningen står i
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) och i README.

- ⛔ **C++ rakt igenom, skrivet från grunden.** Inga filer och ingen struktur
  kopieras ur NwcIO. Det som hämtas därifrån är layouterna i filformatet.
- ⛔ **Ingen kod och inga binärer från Autodesk**, och Navisworks plockas inte
  isär. Formatet lärs genom analys av filer.
- ⛔ **Inga beroenden i biblioteket.** Bara C++-standardbiblioteket; också
  uppackningen är egen. Visaren i `viewer/` är undantaget, och den följer
  aldrig med när HNwd används som bibliotek.
- ⛔ **Fel är aldrig tysta.** En tolkning som inte går ihop ger ett fel, inte en
  gissning.
- ⛔ **Privata filer nämns aldrig i projektet.** En fil någon provar med får
  köras genom verktygen, med sökvägen som argument, men ingenting om den skrivs
  in: inte att den finns, inte namn, sökväg, storlek, ursprung eller innehåll —
  varken i kod, tester, dokument, planer eller commit-meddelanden. Det som lärs
  ur en fil antecknas som fakta om formatet.
- Testdata är bara publika exempelfiler med en nedladdningslänk i
  [tests/test_data.h](tests/test_data.h). De checkas aldrig in.

## Sökvägar

Sökvägar är **UTF-8 på alla plattformar**. På Windows går varje filöppning via
W-varianterna med konvertering i [src/core/path_utf8.h](src/core/path_utf8.h);
A-varianterna, `fopen`, `std::remove` och `std::ifstream` tolkar byten i den
aktiva ANSI-kodsidan och ger `FileNotFound` för en sökväg med `åäö`.

## Plattformar

Windows, Linux och macOS. Utvecklingen och testerna sker på Windows med MSYS2
UCRT64; Linux och macOS skrivs blint. Det som rör ett annat OS och inte kunnat
provas antecknas i `doc/os/<os>/TESTS.md`.

- ⛔ **`windows.h` dras in på exakt ett ställe**,
  [src/core/path_utf8.h](src/core/path_utf8.h), som definierar `NOMINMAX` och
  `WIN32_LEAN_AND_MEAN` först. Utan det förstör makrona `min` och `max` varje
  `std::min` i filer som når headern.
- ⛔ **Stacken är 1 MB på Windows.** Stora buffertar hör på heapen.
- ⛔ **En minnesmappad fil öppnas med `FILE_SHARE_DELETE`**, annars går den inte
  att radera eller byta namn på medan den är mappad, vilket POSIX tillåter.

## Planering

Öppna punkter ligger i [TODO.md](TODO.md), avklarat arbete i
[doc/CHANGELOG.md](doc/CHANGELOG.md). Planer ligger i `doc/plans/` med namnet
`ÅÅÅÅ-MM-DD_NAMN.md`. Det som inte är avkodat i filformatet och bibliotekets
kända svagheter står i [LIMITATIONS.md](LIMITATIONS.md), på engelska: det är
inte planerat arbete, och hör inte hemma i TODO.md.
