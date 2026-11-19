# Att prova på macOS

Koden för macOS är skriven utan att ha körts där. Det här ska provas på en
maskin med det systemet.

## Bygge

- [ ] `cmake -B build -G Ninja && cmake --build build` med Apple Clang, utan
      varningar.
- [ ] `-DBUILD_SHARED_LIBS=ON` bygger och sviten startar.
- [ ] `cmake --install` och `find_package(HNwd)` från ett annat projekt.
- [ ] HNwd indraget med FetchContent i ett annat projekt bygger bara
      biblioteket och hämtar inte GlazeUI.
- [ ] Visaren bygger mot GlazeUI: dess trådpaket kräver en libc++ med
      `std::stop_token` (LLVM 20) eller sin reservflagga.

## Sviten

- [ ] `./build/tests/HNwd_tests` från repots rot, med exempelfilerna i `nwd/`
      (se `tests/test_data.h`). En av filerna har mellanslag i namnet.
- [ ] Samma svit byggd med `-fsanitize=address,undefined`. Särskilt
      uppackningen bit för bit (`InflateStream` i `inflate.cpp`, med
      kontrollpunkter och `resume()`) och fönstret över den (`StreamWindow` i
      `byte_reader.h`): `test_inflate`, `test_object_reader`,
      `test_fragments_stream`, `test_tab_reader` och `test_property_reader`.
      Dessutom bitläsningen och indexkoden i `geometry.cpp`, objektläsaren,
      och visarens JPEG-avkodare (`test_jpeg`).
- [ ] Samma svit byggd med `-fsanitize=thread`. Särskilt `test_readers`: fyra
      trådar med var sin `GeometryReader` över samma dokument, dokumentet
      stängt medan de läser, och `load_tree()` medan fyra trådar gör
      const-uppslag och läser geometri, med `Document::Impl::tree_view` och
      `guids_ready` som enda delade skrivningar. Likaså
      `test_property_reader` med en `PropertyReader` per tråd. Inga rapporter
      om kapplöpningar i `src/`. Kör sviten flera gånger i följd, till
      exempel 20.

## Det som är skrivet blint

- [ ] `MappedFile` i `src/core/mapped_file.cpp`: grenen med `open`, `fstat`
      och `mmap`. En katalog och en namngiven pipe ger `FileOpenFailed`
      utan att öppningen väntar. En fil som inte finns ger `FileNotFound`, en tom fil
      `FileOpenFailed`. En fil över 4 GB går att mappa. En mappad fil går att
      radera, och läsningen fortsätter. Samma mappade fil läst av flera
      trådar samtidigt (`test_readers`).
- [ ] Sökvägar med `åäö` och mellanslag, som UTF-8 rakt igenom:
      `tools/tool_main.h` (grenen med `main()`) ger verktygen argumenten;
      `Document::open()` öppnar filen; `nwd2obj` skriver OBJ, `.mtl` och
      bilderna och tar bort halvskrivna filer vid fel; `nwdsynth` skriver
      sin fil och tar bort den vid fel; `nwdview` tar filen ur `argv` och
      läser den, och `--png` och `--decode-jpeg` skriver till en sådan sökväg.
      Ett namn med `åäö` skrivet i NFC ska gå att öppna också om filsystemet
      lämnar det i NFD.
- [ ] `testdata::tmp_dir()` läser `TMPDIR` och faller tillbaka på `/tmp`;
      testerna lämnar inga filer kvar där.
- [ ] `nwdsynth` med miljontals fragment (till exempel `--fragments 5000000
      --records 100000 --nodes 1000000`), sedan `nwdcheck <fil> --poses`
      (avslutningskod 0), `nwddump <fil> --spatial --units` och
      `nwdview <fil> --png <bild>`; samma med `--no-spatial`.
- [ ] `nwdview` med GlazeUI:s Metal-backend: ritprogrammen i MSL
      (`msl_source()` i `viewer/textures.cpp`), med och utan bild och med
      snittplan, är skrivna utan att ha körts här. Rita en modell med
      materialbilder, sedd inifrån, och med `--clip-test`; prova också
      gångläget (F2) och `--png`. Visaren byggs som app-paket
      (`MACOSX_BUNDLE`); prova att starta den både som paket och med en fil
      som argument.
- [ ] `nwdview`: maskinens minne (`system_memory()` i `viewer/sysmem.cpp`,
      `sysctl hw.memsize`, `host_statistics64` och `task_info`) och förvalda
      budgetar. Grafikkortet delar minne med processorn, så statusraden ska
      visa en gemensam budget; `test_sysmem` i sviten ska gå igenom.
- [ ] `nwdview`: linjernas ritprogram i MSL (`line_msl()` i
      `viewer/textures.cpp`). Rita en modell med linjer: inget "kompilerar
      inte" på stderr, linjerna är en pixel breda och syns ovanpå ytor i
      samma plan, inga streck över bilden när en linjes ände ligger bakom
      ögat (gångläget), och `--clip-test` klipper dem.
- [ ] `nwdview` i en stor syntetisk fil (`nwdsynth <fil> --fragments 3000000
      --records 600000 --nodes 600000 --sizes 0.02:40 --detail 100`) med
      `--png <bild> --profile --spin 1` och med `--walk 150 --speed 3`:
      raderna `bildtakt` och `UI-tråden … efter startvyn` ska visa en bildtid
      under 22 ms i p95 och inga stopp över 4 ms. Urvalets tråd använder
      `std::condition_variable_any` med `std::stop_token`.
