# HNwd — ändringslogg

Det som är kvar att göra ligger i [TODO.md](../TODO.md).

## 0.2.0

- Modellerna på alla nivåer: `Document::model()` ger varje nod av klass 32 i
  trädet, också dem som sitter i en annan modell, i preorder med
  `Model::parent` och `Model::depth`. `model_of_node()` och
  `model_of_fragment()` ger den innersta modellen. **För en fil med modeller i
  modeller ändras antalet modeller och deras index**; en modells fragment är
  hela delträdets. Filer utan sådana svarar som förut.
- `PropertyReader` i `TreeContent::Structure` håller en cache med tak
  (`open(doc, cache_bytes)`, förval 16 MiB) för flikar som flera noder delar
  och för noder som sitter på flera ställen i trädet, så att de inte läses om
  ur filen. Ett träd där nära hälften av förekomsterna är upprepningar läses
  nod för nod tiotals gånger fortare.
- Kontrollpunkterna i trädets strömmar står var 1 MiB i stället för var
  4 MiB.
- `nwdview` visar modellerna på översta nivån, som förut.
- `nwdview --pacing vsync|all|N`: bildtakten. `all` ritar varje bild hel
  (förvalet); med `vsync` eller ett antal bilder per sekund ritar ritytan det
  som ryms i tiden under rörelse och fyller på resten i stillhet.
- `nwdview` bygger på ramverkets version där ytor i samma plan har en fast
  ordning i ritytan och bildtakten går att välja.
- `nwdsynth --prop-streams`, `--shared-tabs`, `--common-tab` och
  `--instances`: flera egenskapsströmmar, delade flikar och delträd på flera
  ställen.

## 0.1.0

- Mappstruktur och bygge: CMake med Ninja, modulen `HNwd::core`, paraplyet
  `HNwd::HNwd`, testsviten `HNwd_tests`, installationsregler och CMake-paket.
- Sökvägar i UTF-8 på alla plattformar (`src/core/path_utf8.h`).
- Felkoder med text (`src/core/nwd_error.h`).
- Minnesmappad fil (`MappedFile`).
- Egen zlib-uppackning med Adler-32 och tak för uppackad storlek
  (`inflate_zlib()`).
- Filens yttre form: sektionsrubrikerna, katalogen över paketen och kontrollen
  att katalogen täcker datasektionen (`read_container()`). Katalogen hittas
  genom offseten i filens slut, annars bakifrån.
- Verktyget `nwddump`.
- [format.md](format.md): det som är prövat av filens yttre form.
- Objektströmmarna: pekarregeln, id och nästlingsdjup (`ObjectReader`).
- Strömmarna i de paket som har flera (`split_prefixed()`, `inflate_span()`).
- GUID-tabellen (`parse_guid_store()`) och källhuvudet i en `.nwc`
  (`parse_source_header()`).
- Geometriposterna: trianglar i strips, linjer, normaler ur palett, färger
  (`parse_record()`), och geometripaketet med chunkar som packas upp efter
  behov och en cache med tak (`GeometrySegment`). Normalernas bredd avgörs ur
  posterna.
- Placeringen: fragmenten med pose, utseende och geometrireferens
  (`parse_fragments()`).
- Objektträdet i en `.nwc`: noder, flikar med egenskaper, material och
  placering, med egenskaperna ur `LcOaPartitionProps` (`parse_hierarchy()`,
  `read_partition_props()`). En fil med flera modeller (klass 32) ger
  `Unsupported`.
- Flera modeller i en fil: klass 32 är avkodad, så trädet i en `.nwd` läses
  (`parse_partition_stream()`, `read_partition_tail()`). `nwddump --partition`.
- Det publika API:t `HNwd::Document` i modulen `HNwd::scene`: katalogen,
  geometrin per post och per fragment, fragmenten med pose och utseende,
  GUID-tabellen, källan, och trädet med noder, flikar och egenskaper. Paketen
  prövas mot varandra: fragmentets låda och antal mot geometriposten, och
  varje fragments `seq` mot ett löv i trädet.
- Verktyget `nwd2obj`.
- Tak mot filer som kostar mer än de väger: en geometripost längre än
  `Options::max_record_bytes` ger `LimitExceeded`, egenskapsströmmarna delar
  på ett tak, cachen räknar det minne chunkarna faktiskt håller och hittar
  den äldsta utan sökning, och en ström som är trasig i sitt första block
  kostar ingen stor buffert. Tar minnet slut ger anropet `LimitExceeded` i
  stället för ett undantag.
- Uppackningen avvisar ofullständiga Huffman-koder.
- En fil vars rubrik inte anger 64-bitars flyttal ger `Unsupported`.
- Ett paket med fel form ger `Corrupt`, inte `OutOfRange`.
- Ett program som länkar `HNwd::HNwd` får bara den publika headern i sin
  sökväg, inte de interna i `src/core/`.
- Sviten säger i slutraden hur många prov som hoppades över för att
  testdata saknas.
- Visaren `nwdview`: modellen i 3D, och ett klick på ett objekt visar dess
  namn, sökväg och egenskaper. Byggs med tester och verktyg och hämtar då
  GlazeUI; `HNWD_BUILD_VIEWER` stänger av den.
- Ett fragment utan pose står i geometrins mittpunkt, inte i origo.
- Punktposter (klass 96) avkodas: `MeshKind::Points`.
- Trianglar får ha färg per vertex.
- Placering med full matris (poseklass 17): `Pose::affine` och `Pose::m`;
  `world_normal()` räknar rätt också med olika skala per axel.
- Utseendets flaggor 16 och 64.
- Trädet: partitioner under partitioner, ljus utan beskrivning, id och text
  som egna klasser (66, 70, 76) också direkt på en nod, klass 92, och
  attributens gemensamma huvud med namnpar.
- En UV-bitbredd på 0 godtas.
- Biblioteket, testerna och verktygen bygger med MSVC på `/W4` utan
  varningar, och sviten går igenom där.
- Efter en andra granskning: sparade vyer tar minne i proportion till
  strömmen (en mapp bär ingen vy); strängpoolens dubblettsökning har ett tak
  för sonderingen, så tiden är linjär oavsett namn; småpaketens listor växer
  med det som läses; `Options::max_element_bytes` och
  `Options::max_material_json_bytes`; `load_spatial()` prövar sin källa;
  gränsen 4 GiB i trädet gäller bara skelettläget.
- Trädet tar en tredjedel av minnet: 53–66 byte per lagrad nod i skelettläge.
  Strängarnas dubbletter lagras en gång, transformerna i en pool, och
  skelettets bakåtpekare packade.
- Vyernas snittplan: `Camera::clip`, `clip_plane_count()`. `nwdview` klipper
  modellen mot dem; `--clip`, `--clip-test`.
- `nwdview`: gångläge (F2) i första person med fart i m/s, valfritt med
  ögat på fast höjd över ytan under; ytor sedda bakifrån belyses som
  framifrån; text ritas i sin verkliga storlek med ett tak i bild; `--png`
  väntar på materialens bilder. `--eye`, `--look`, `--walk-mode`, `--follow`,
  `--speed`, `--walk`.
- Kollisionstesterna (`LcOpClashElement`) och vyernas snittplan avkodas, och
  enhetskoden 1 är centimeter (`LengthUnit::Centimetre`).
- Efter en granskning: ingenting allokeras längre efter ett oprövat tal ur
  filen (materialens bilder och filtabell, partitionens ljus, det rumsliga
  trädets barn); ett trasigt rumsligt paket ger `Corrupt` i stället för ett
  tyst eget index; ett tidigare rumsligt index lever till `close()`;
  materialens JSON har tak och läses i linjär tid; en kvaternion som inte
  har längden 1 är ett fel; visarens JPEG-avkodare prövar skanningens
  komponenter och varje blockindex.
- Vyerna (aktuell, hem och sparade som träd), rutnäten, typsnitten, scenens
  material, bakgrunden, ljusen, filens identitet och inläsarens logg genom
  `Document`. `nwdview` startar i filens egen kamera, har en lista över
  sparade vyer, ritar rutnäten och listar nivåerna; `--view <namn>`.
- `nwdcheck --poses`: varje fragments pose är trädets transformer i följd
  gånger flytten till geometrins mitt. Därmed är det belagt att poseklass 17
  och trädets klass 39 står kolumnvis.
- `nwdsynth`: skriver en giltig syntetisk fil av valfri storlek.
- `load_tree()` får köras medan andra trådar läser samma dokument: trädet
  byggs vid sidan av och publiceras när det är färdigt och prövat. Visaren
  läser trädet i samma dokument som geometrin.
- Småpaketens former i formatversion 451: sparade vyer med mappar och
  markeringar, hemvyn, vyns nyare former, rutnät med linjer och nivåer,
  bakgrundens himmel, materiallistan (`LcReMaterialElement`), typsnitten
  (`LcOpNwdSharedNodes`), och de små inställningspaketen.
- `nwdview`: grovbilden byggs av modellens största delar i växande omgångar
  och syns inom någon sekund efter att modellen är läst; trädet läses som
  skelett och egenskaperna hämtas för det klickade objektet; materialens
  bilder ritas med upprepning (egen JPEG-avkodare); text ritas som
  etiketter; en lista väljer vilka modeller som syns; det diskreta
  grafikkortet är förval. `--gpu`, `--uv`, `--decode-jpeg`.
- De små paketen tolkas i `src/core/elements.*` och visas med
  `nwddump --elements`: inläsarens logg, filens identitet, XRef-tabellen,
  den aktuella vyn, ljusen, bakgrunden, strålkastaren, farten och gallringen.
  Utredningen står i `doc/research/element.md`.
- Det publika API:t är trådsäkert att läsa: `GeometryReader` och
  `PropertyReader` är läsare för var sin tråd, och delarna de delar lever
  tills den sista läsaren stängt. `Document::fragments()` och `looks()` är
  ersatta av `fragment(i, out)` och `look(i, out)`.
- Fragmenten tolkas strömmande och lagras kompakt (omkring 100 byte per
  fragment), `load_…` tar en förloppsfunktion som kan avbryta, och
  `memory_bytes()` säger vad dokumentet håller.
- `load_spatial()`: det rumsliga trädet med världslåda och primitivsumma per
  nod, ur filen eller byggt av biblioteket. `fragment_world_box()`.
- `length_unit()`, och `model()` med enheten per inläst modell.
- `load_tree(TreeContent::Structure)`: trädet utan flikar, och
  `PropertyReader` som läser en nods egenskaper ur filen vid behov.
  `node_of_fragment()`, `parent()`. Trädet tolkas utan rekursion.
- Text (klass 103) avkodas: `MeshKind::Text` med sträng, låda, vridning och
  typsnittsindex.
- `nwdview` läser i arbetstrådar och ritar efter detaljnivå inom en
  minnesbudget: lådor, de största delarna, sedan detalj nära kameran.
  Statusraden visar världskoordinaten i meter. `--budget`, `--profile`;
  `--step` är borttagen.
- Material med inbäddade bilder (klass 185): `Document::material()` ger
  grundfärgen och bilden som den ligger i filen, `Look::material` pekar ut
  materialet, och `Mesh::uvs` bär UV-koordinaterna. `nwd2obj` skriver `vt`,
  bildfilerna och `map_Kd`.
- `InflateStream` och `StreamWindow`: uppackning och läsning bit för bit, med
  kontrollpunkter att återuppta från. Id-tabellen tar 8 byte per objekt.
- Det rumsliga trädet över fragmenten (`LcOpNwdSpatialHierarchy`) avkodas:
  `parse_spatial_hierarchy()`, `nwddump --spatial`.
- Längdenheten läses ur partitionen: `metres_per_unit()`, `nwddump --units`.
- En geometripost hämtas direkt ur sin chunk, utan att chunkarna före packas
  upp. Geometriläsaren är delad i en oföränderlig del och en läsare per tråd
  (`GeometryLayout`, `GeometryCursor`). `Options::measure_chunks`.
- `Status::Cancelled`.
- `HNWD_BUILD_VIEWER` är oberoende av `HNWD_BUILD_TESTS` och av som förval när
  HNwd dras in i ett annat projekt.
- Cirklar (klass 104) avkodas till 32 linjesegment och cylindermantlar
  (klass 105) till 64 trianglar med normaler. Deras låda räknas ut och får
  skilja från fragmentets på avrundningen.
- Textposter (klass 103) ger `Unsupported` i stället för `Corrupt`; `nwd2obj`
  och `nwdview` hoppar över dem och räknar dem.
- Förvalstaket för en uppackad ström är 2 GB.
- `nwdview --step N`, och vyn passas in på modellens huvuddel.
- `nwdview` för stora filer: fragmenten ligger i bitar i ett detaljträd, och
  en bit byggs när dess största fragment är minst tröskeln i bild (3 px som
  förval) och bara till den fragmentstorlek som behövs på dess avstånd.
  Grovbilden är ersatt av en grundbild: de största bitarna, som byggs först
  och alltid ritas.
- `nwdview`: två minnesbudgetar, `--budget` för RAM och `--gpu-budget` för
  grafikkortet, med förval efter maskinen. Räcker minnet inte höjs tröskeln,
  och statusraden säger vilken som gäller.
- `nwdview`: ritningens tröskel följer grafikkortets och ritningens tid och
  håller bildtiden; det närmaste ritas först, och kantutjämningen sänks under
  rörelse när det behövs.
- `nwdview`: urvalet räknas i en egen tråd, och klicket, koordinaten under
  markören och marken under ögat träffprovas i arbetstrådar. UI-tråden tar
  emot färdiga svar i små portioner; `--profile` räknar dess stopp.
- `nwdview`: linjerna ritas som en del av bitens mesh med ett eget ritprogram.
- `nwdview --spin`, `--tile`, `--min-pixels`, `--max-draws`, `--max-triangles`,
  `--pick-cache`, `--no-lines`; `--cells` är borta. `--profile` skriver
  byggtiderna per del, bildens faser, minnet mot det räknade och vad
  instanser skulle spara.
- `nwdsynth --sizes A:B` och `--detail N`: poster med spridda storlekar och
  fler segment, för prov i stor skala.
- Gångläget: Q och E svänger, R och F ändrar höjden, plus och minus ändrar
  farten (förval 1,5 m/s), ett drag med musen eller två fingrar på
  styrplattan ser sig om.
- `nwdview` bygger på ramverkets version med en renderingstråd för ritytan:
  scenen ritas och laddas upp utanför UI-tråden, och uppladdningen per bild
  och taket för ritanrop är höjda därefter.
- `nwdview`: klipplanen i orbit följer det närmaste i bilden och modellens
  huvuddel, så att det som står nära inte klipps och ytor nära varandra inte
  flimrar mer än djupet tillåter.
- `nwdview`: statusraden visar läget, vad som syns och varför, minnet, det
  som pågår och det som gått fel; räknarna står i dess tooltip.
- `nwdview --size-margin`: hur långt under den storlek som behövs en bit
  byggs.
