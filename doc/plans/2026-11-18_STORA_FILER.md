# nwdview för riktigt stora filer

Urval per fragment efter storlek i bild, billigare trianglar och minst 45
bilder per sekund.

Sökvägarna är relativa till repots rot. GlazeUI-hänvisningarna gäller
`build/_deps/glazeui-src/` på taggen v0.9.4. Radnumren gäller koden när planen
skrevs.

**(verifierat …)** betyder läst i koden på den angivna platsen.
**(antaget — mäts i …)** betyder att det inte går att avgöra ur koden; varje
antagande har en mätning i den etapp som först behöver det.

---

## 1. Bakgrund och mål

**Läget (verifierat).**
- **Cellerna.** `plan_cells` gör en cell av varje nod i det rumsliga indexet som
  har högst `cell_triangles` primitiver, förval 200 000 (`viewer/cells.cpp:10-50`,
  `viewer_window.h:43`).
- **Detaljen byggs per hel cell**, när cellens låda är minst `min_pixels` = 32 px
  (`scheduler.h`, `wants_full` i `scheduler.cpp`). Lådan mäts med djupet till
  mitten, som underskattar en låda nära ögat upp till ungefär en faktor 2.
- **Minnet per triangel.** `part_bytes` räknar 2·(28 byte per hörn + 4 byte per
  index) + 32 byte per triangel (`cells.cpp:125-133`). Varje mesh får
  `preparePicking()` i arbetstråden (`viewer_window.cpp:313`).
- **Vad budgeten rymmer.** Med 110–120 byte per triangel rymmer förvalet 1,5 GB
  omkring 13–14 miljoner trianglar. En modell med flera hundra miljoner trianglar
  ger tusentals celler om 200 000 trianglar, och en budget hundra gånger större
  fylls också.
- **Grovbilden** tar 20 % av budgeten (`viewer_window.cpp:23`) och är byggd ur
  samma fragment som cellerna, alltså dubbelt.
- **`detail_radius`/`detail_pixels`** (`scheduler.h`, `scheduler.cpp`,
  `viewer_window.cpp` `kDetailRadius`, `remeasure`, `note_walk_sample`) mäter
  varje cell inom 80 m som minst 32 px. Små celler nära ögat drar då in hela
  celler om 200 000 trianglar.
- **Ett frö till urval per fragment.** Cellens trianglar står med de största
  fragmenten först (`cells.cpp:205-222`). `big_triangles` (`cells.cpp:224,426-427`)
  blir en tvåstegs `MeshDesc::lods` (`viewer_window.cpp:307-311`).

**Mål.**
1. Det som är mindre än N px i bild varken byggs eller ritas. Allt som är minst
   N px syns, som en garanti. N är 3 px som förval och höjs bara när minnet eller
   bildtiden kräver det, och då syns den höjda tröskeln i statusraden.
2. Det som blir stort nog syns inom en bråkdel av en sekund.
3. Minst 45 bilder per sekund i gångläget och i orbit.
4. Minnesbudgeten håller, med ett förval som passar maskinen. När budgeten tar
   slut försvinner de minsta sakerna först, jämnt över bilden, aldrig hela
   områden.
5. `detail_radius`/`detail_pixels` tas bort. I gångläget följer "80 m" av
   garantin: på 80 m syns allt som är ungefär ≥ 0,2 m med N = 3 px och 1000 px
   bildhöjd.

---

## 2. Fakta som styr resten

### 2a. GlazeUI v0.9.4 (verifierat)

| Fakta | Var |
|---|---|
| `MeshDesc`: lägen som `Float3` relativt `origin`, `normals`, `colors`, `uvs`, uint32-`indices` och `lods` | `canvas/mesh.h:83-98` |
| `Lod{first, count, maxDistance}`. Nivån väljs efter avståndet från ögat till meshens placerade låda; den första vars `maxDistance` räcker ritas. Bortom den sista nivån ritas ingenting. | `mesh.h:71-81`; `src/canvas/mesh.cpp:313-321`; `scene3d.cpp:497-503` |
| `MeshStyle::range` snittas med nivån, och en ny stil med `setShape` laddar inte upp något igen | `scene3d.cpp:140-161`; `style3d.h:165-173` |
| CPU-kopian finns alltid kvar. `keepCpuCopy = false` gör meshen oplockbar och sparar inget minne. | `mesh.h:95-97`; `MANUAL.md` |
| Grafikkortets hörn är position `Float3` (12) + normal `SNorm8x4` (4) + färg `UNorm8x4` (4) + uv `Float2` (8). CPU-kopian har 12 + 12 + 4 (+ 8). | `src/canvas/programs3d.cpp:1256-1290`; `mesh_data.h` |
| Indexen är alltid 4 byte på grafikkortet | `residency.cpp:207` |
| Plockningens BVH byggs latent med `std::call_once`. En nod är 32 byte, ett löv har högst 8 trianglar. | `mesh.cpp:323-327`; `bvh.h:33-44` |
| En kopia på grafikkortet lever så länge meshens data lever. Att ta bort formen ur en plats och lägga in samma `Mesh` igen laddar alltså inte upp något. | `residency.cpp:173-190` |
| Uppladdningen strömmar inom `App::setCanvasUploadBudget` (förval 16 MiB per fönster och bild). Hörnen packas i UI-tråden. | `app.h`; `residency.cpp:194-232` |
| Varje bild: varje föremål i ett synligt lager köas och gallras, med ett ritanrop per segment | `scene3d.cpp:701-719`, `:480-524` |
| `Shape::instances`: celler om cirka 64, gallring och detaljnivå per cell, 52 byte per instans. Egna program ritar inte instanser. | `shape.h:526-527`; `src/canvas/instances.h:35`; `scene3d.cpp:572-587` |
| `HitResult::instance`; `castRay`; `HitOptions` saknar lagerfilter | `widgets/canvas.h` |
| Profileraren: `FrameProfile` med ritanrop och faser, `summary(fönster, sekunder)` med intervallens p50/p95/p99 och `missed50Hz` | `profiler.h:83-103`, `:131-147`, `:169` |
| Kamerans förvalda synfält är 45° på höjden | `canvas/camera.h:73` |
| `gfx::gpuAdapters()` med `memoryBytes` och `GpuKind` | `gfx/platform.h:179-198` |
| Visarens program faller tillbaka på den platta normalen ur derivatorna när normalen är noll, i alla tre språken | `viewer/textures.cpp:119-133, 201-215, 292-293` |

### 2b. HNwd — det som redan räcker (verifierat)

- `Fragment::record`, `primitives`, `bounds` och `pose` (`HNwd.h:172-179`).
  `fragment_world_box` (`HNwd.h:591`) ger varje fragments storlek utan att
  geometrin avkodas.
- Det rumsliga indexet har lådor och primitivsummor.
- Antalet fragment per geometripost räknas ur `Fragment::record` utan avkodning.
- Ingen biblioteksändring krävs. Två små tillägg är möjliga om mätningen pekar
  dit; de står i etapp 1.

### 2c. Det som inte går att avgöra ur koden (antaget — mäts i etapp 0)

- r = V/T: hörn per triangel.
- Kostnaden per ritanrop och per föremål i GlazeUI på maskinen.
- BVH:ns verkliga storlek (uppskattning ur `bvh.h`: cirka 20 byte per triangel).
- Var byggtiden går: avkodning, omräkning, `Mesh`-bygget eller `preparePicking`.
- Hur ofta geometriposter återanvänds.
- Hur stor andel av fragmenten som har platta normaler.

---

## 3. Idén: detaljträdet med additiv förfining

**Bitarna.** Varje fragment hör till exakt en **bit** (`Tile`) i ett
**detaljträd** (`LodTree`), byggt ovanpå det rumsliga indexet.
- **Placeringen.** Ett fragment med diagonalen s placeras i den *översta* nod n
  på vägen löv → rot där s ≥ α·D_n (D_n är nodens diagonal, α ≈ 1/32). Fragment
  som är för små för varje nod hamnar i lövet.
- **Följden.** Stora saker ligger i stora noder och små i små. En nods egna
  fragment ligger inom ungefär en oktav.
- **Uppdelningen.** En nods egna fragment delas i bitar om högst
  `max_tile_triangles` (förval 32 768) i rumslig ordning. En nod med färre än
  `min_tile_triangles` egna trianglar lämnar dem till föräldern.
- **Stora löv.** Löv med fler än 64 fragment delas vidare vid medianen längs den
  längsta axeln.

**Kriteriet och garantin.** Bit b behövs när

    P_b = s_max(b) · K / d(öga, b.låda) ≥ N,   K = bildhöjd / (2·tan(fov/2))

Ett fragment f i b har s_f ≤ s_max och d_f ≥ d(öga, b.låda), så dess
vinkelstorlek s_f·K/d_f ≤ P_b. Varje fragment som är ≥ N px ligger alltså i en
bit som behövs, hur fragmenten än grupperas. α avgör bara överbygget (hur mycket
som byggs under N px, cirka 1 + N/(α·K)) och antalet ritanrop.

**Vandringen.** Nod n kan bara ha en bit som behövs om
max_size(n)·K/d(öga, n.låda) ≥ N, och ett barn har aldrig ett större värde än sin
förälder. Mätningen vandrar trädet uppifrån och beskär. Kostnaden växer med
antalet bitar som behövs, inte med modellen.

**Detaljnivåerna i varje mesh.**
- Delarna (ogenomskinligt, genomskinligt och en per material med bild) står med
  de största fragmenten först. `lod_steps` bakar 2–4 `Lod`-nivåer så att GlazeUI
  på avståndet d ritar varje fragment med s·K_bak/d ≥ N_min. GlazeUI väljer nivån
  per bild, och bortom den sista nivån ritas biten inte alls.
- K_bak är K när jobbet startar, gånger marginalen 1,5, så att ett större fönster
  inte bryter garantin. Växer K mer än så byggs bitarna om med låg prioritet.
- `MeshStyle::range` sätter ett kortare prefix när ritningens tröskel är högre
  än N_min.

**Tre trösklar.**
- **N_min (förval 3 px):** garantins tröskel när allt räcker.
- **N_eff:** minnets tröskel. Den höjs när det som behövs vid N_min inte ryms i
  budgeten. Bitarna sorteras efter P och kapas där budgeten tar slut, med
  hysteres.
- **N_draw ≥ N_eff:** ritningens tröskel, styrd av antalet trianglar och
  ritanrop per bild och av uppmätt bildtid.

En bit som är byggd men under N_draw **parkeras**: formen tas ur lagret, och
`PartShape` med sin `glaze::Mesh` behålls. Den kostar ingenting per bild och syns
igen utan ny uppladdning.

**Grundbilden ersätter grovbilden.** Bitarna sorteras efter s_max. De största,
upp till 20 % av budgeten, märks som fasta (`pinned`): de byggs först och släpps
aldrig. `coarse.*`, grovlagret med djupbias och dubbletterna försvinner.

**Gångläget.** Bitar inom närradien (30 m, `kWalkNearRadius`) räknas oavsett
riktning, med halva vikten bakom ögat. En kamera om 1 s längs rörelsen (`ahead`)
ger förhandsbygget lägre vikt.

**När budgeten tar slut** släpps i den här ordningen:
1. Parkerade bitar utanför bild och utanför närradien, minst P först.
2. Bitar i bild under N_eff × 0,5.
3. Bitar i bild under N_eff.

Fasta bitar, och bitar med högre P än den som väntar, släpps aldrig. Räcker det
ändå inte höjs N_eff, och statusraden säger till exempel "allt ≥ 7 px syns
(minnet räcker inte för 3 px)".

---

## 4. Mätbara mål och hur de mäts

Mätfilerna är syntetiska och publika: de tio exempelfilerna i `nwd/`, och en stor
syntetisk fil skriven med `nwdsynth` (etapp 0), till exempel
`--fragments 2000000 --records 400000 --nodes 500000 --sizes 0.02:40 --detail 100`,
i en temporärkatalog.

| Mål | Värde | Mätning |
|---|---|---|
| **Bildtakt** | intervallets p95 ≤ 22,2 ms, p50 ≤ 16,7 ms, efter startvyn | `nwdview <fil> --png <bild> --profile --walk 150 --speed 3 [--follow]` och `--spin 1`. Rapporten skriver `profiler().summary(...)`: p50/p95/p99, `missed50Hz`, ritanrop och trianglar per bild. |
| **Garantin** | när urvalet står still är 0 synliga fragment ≥ N_eff utan sin bit | `test_tile_scheduler`: brute force över alla fragment och slumpade kameror. I visaren räknar `note_walk_sample` hål. |
| **Fort in** | från att en bit behövs tills den syns: p95 ≤ 300 ms i 1,5 m/s, ≤ 1 s i 5 m/s (sätts om efter etapp 0) | Schemaläggaren antecknar när en bit först behövs och när den installeras; `--walk` skriver p50/p95/max. |
| **Byte per triangel** | RAM och grafikkort var för sig, tabellen i avsnitt 6 | Summan av `part_bytes` delad med trianglarna, jämförd med processens minne. Skrivs i `--profile`. |
| **Det som garanterat syns** | allt ≥ N_eff px | Statusraden och `--profile` skriver N_eff och N_draw. |
| **UI-tråden** | mätningen p95 ≤ 2 ms; längsta postade funktion ≤ 8 ms | `stalls_` (`viewer_window.cpp` `stall`/`note_stalls`). |
| **Minnet** | `resident ≤ budget` alltid, för RAM och grafikkort | Statusraden och `--profile`; ett överskridande är ett fel på stderr. |

---

## 5. Steg i ordning

Varje etapp byggs i Release med 0 varningar och är färdig att provas och
committas för sig.

### Etapp 0 — Mätning och utgångsvärden

1. **`tools/synth_writer.h`, `tools/nwdsynth.cpp`.** Nya fält sist i
   `synth::Options`: `min_size`/`max_size` ger postens längd log-likformigt i
   meter, `max_segments` stripens längsta längd. Flaggorna `--sizes A:B` och
   `--detail N`. Förvalen ger exakt dagens fil (`record_shape`,
   `synth_writer.h:336-374`), så `test_synth` står oförändrat för dem.
2. **`viewer/cells.h/.cpp`.** `CellGeometry` får `read_seconds` (tiden i
   `fragment_mesh`), `convert_seconds`, `vertices` och `flat_fragments`.
3. **`viewer/model.h/.cpp`.** `record_use()`: ett histogram över hur många
   fragment som visar varje post, och trianglarna räknade en gång per post och
   en gång per fragment.
4. **`viewer/sysmem.h/.cpp` (ny, GlazeUI-fri).** `system_memory()`: fysiskt
   minne, tillgängligt minne och processens minne. Windows:
   `GlobalMemoryStatusEx` och `K32GetProcessMemoryInfo`, med `windows.h` genom
   `path_utf8.h`. Linux: `/proc/meminfo` och `/proc/self/statm`. macOS:
   `sysctl hw.memsize` och `task_info`.
5. **`viewer/viewer_window.h/.cpp`, `main.cpp`.** `build_cell_job` mäter
   `to_mesh` och `preparePicking` var för sig (`BuiltCell` får `mesh_seconds` och
   `pick_seconds`). `--profile` skriver summor och p95 per del, r = V/T, andelen
   platta fragment, postanvändningen och processens minne mot det räknade.
   `finish_walk` skriver `profiler().summary(...)`. Ny flagga `--spin <varv>` i
   `png_tick`: ett orbitvarv på 10 s per varv med `setTicking(true)` före bilden,
   och samma rapport.
6. **Körningen** mot de tio exempelfilerna och den syntetiska filen, med
   `--cells 200000` respektive `--cells 20000` för kostnaden per ritanrop.
   Talen antecknas i avsnitt 15. Därur sätts `kPickBytesPerTriangle`, förvalen
   för `max_draws` och `max_triangles`, och α.

### Etapp 1 — Snabba vinster utan ny struktur

1. **Två budgetar och rätt räkning.** `part_bytes` → `PartBytes{ram, gpu}`:
   RAM = 28 (+8) byte per hörn och 4 per index; grafikkortet = 20 (+8) byte per
   hörn och 4 per index. Sökträdet räknas för sig (`pick_bytes`). `LodScheduler`
   håller båda budgetarna. `kInitialBytesPerPrimitive` (`scheduler.cpp:11`) och
   `kBytesPerTriangle` (`viewer_window.cpp:24`) ersätts av formeln. `--budget`
   gäller RAM, nya `--gpu-budget` grafikkortet. Förvalen kommer ur
   `default_budgets()`: RAM blir hälften av det fysiska minnet minus dokumentets
   `memory_bytes`, minst 512 MB; grafikkortet 60 % av `memoryBytes` för ett
   diskret kort; för ett integrerat kort, delat minne eller okänt räknas de två i
   en gemensam budget.
2. **Plockningen förbereds efteråt.** `to_mesh` kör inte `preparePicking()`. Ett
   lågprioriterat jobb gör det när byggkön är tom, och sökträdets byte läggs till
   cellen (`add_bytes`).
3. **`detail_radius`/`detail_pixels` bort** ur `scheduler.h/.cpp` och
   `viewer_window.cpp` (`kDetailRadius`, `remeasure`, `note_walk_sample`).
4. **Trådarna.** `build_threads()` (`viewer_window.cpp:49-52`) blir
   clamp(hw − 2, 2, 16). `kReaderCache` (`:26`) blir 64 MB per läsare och räknas
   i RAM-budgeten.
5. **`build_fragments`** reserverar delarnas vektorer ur primitiverna och räknar
   posens matris och normalmatris en gång per fragment i stället för
   `world_point`/`world_normal` per hörn (`cells.cpp:368-376`).
6. **Möjliga tillägg i biblioteket**, bara om etapp 0 visar att läsningen är en
   betydande del: `read_fragment_mesh` bygger `prefix`
   (`src/scene/geometry_reader.cpp:35`) först vid ett fel, och eventuellt
   `GeometryReader::stats()`.
7. **Mätning efter etappen:** tiden till syns och processens minne mot det
   räknade.

### Etapp 2 — Detaljträdet ersätter cellerna och grovbilden

1. **`viewer/lod_tree.h/.cpp` (ny, GlazeUI-fri).** `plan_lod_tree()`:
   fragmentens diagonal räknas i trådar (mönster: `coarse.cpp:40-64`); stora löv
   delas vid medianen; varje fragment placeras uppåt efter α; en nod med för få
   trianglar lämnar dem till föräldern och en med för många delas i bitar;
   fragmenten sorteras störst först i varje bit; `max_size` räknas nerifrån och
   upp; de fasta bitarna märks; `tile_of` byggs per fragment. `tree_boxes()`
   ersätter `coarse_boxes`. Explicit stack (mönster: `cells.cpp:15-48`).
2. **`viewer/scheduler.h/.cpp`.** `TileScheduler` ersätter `LodScheduler`.
   `ViewFrustum` mäter med avståndet till lådans närmaste punkt (`distance`),
   får `pixels_per_radian()` och `sees()`, och behåller `pixels_at` för texterna.
   Mätningen vandrar trädet och mäter om de byggda. Kandidaterna sorteras en gång
   per mätning; `next_jobs` tar ur listan. Listan av offer byggs en gång per
   mätning. N_eff räknas ur prefixsummor. `shown_changes` ger bitarna som ska
   visas och parkeras. Latensen och hålen räknas.
3. **`viewer/cells.h/.cpp`.** `FragmentRange` får `size`. `build_tile` läser
   fragmenten i den ordning de redan står. Nya `lod_steps()` och
   `triangles_at_least()`. `Detail::Coarse`, `plan_cells`, `cell_of_item` och
   `coarse_boxes` tas bort när ingen använder dem.
4. **`viewer/coarse.h/.cpp` tas bort.**
5. **`viewer/viewer_window.h/.cpp`.** Bitarna ersätter cellerna: `Loaded` bär
   `LodTree`; `on_loaded` planerar trädet i `loader_`; `start_coarse`,
   `on_round`, `start_piece_job`, `on_piece`, `pieces_`, `coarse_layer_`,
   `covered_` och `coarse_*` tas bort; `build_cell_job` → `build_tile_job`;
   `to_mesh` tar `lod_steps`; `remeasure` ger `sched_.update(...)`; `on_built`
   och `install_full` ger `TileView`; `PartShape` får `drawn_triangles`, och
   `clipped_shape` lägger det i `range`; parkering och visning med
   `take_slot`/`free_slot`. Plockningen förbereds bara för fasta bitar och, i
   gångläget med markföljning, för bitar inom 10 m i xy under ögat; annars byggs
   sökträdet latent. `hit_fragment` utan grovgrenen. `toggle_model` räknar de
   berörda bitarna ur `tile_of`. `png_tick` och `profile_tick` utan grovbitarna;
   `note_walk_sample` räknar hålen genom schemaläggaren.
6. **`main.cpp`.** `--cells` byts mot `--tile N` och nya `--min-pixels N`.
7. **Mätning efter etappen:** byggda trianglar och överbygget, ritanrop per
   bild, tid till syns, hål och minne. α och bitens storlek sätts om.

### Etapp 3 — Ritbudgeten, förutsägelsen och avbrotten

1. **`viewer/governor.h/.cpp` (ny, GlazeUI-fri).** `DrawGovernor` sätter N_draw:
   den lägsta tröskeln där ritade trianglar och ritanrop ryms, uppskattat ur de
   visade bitarnas `triangles_at_least` vid deras avstånd. Taken flyttas av
   uppmätt bildtid (`onTick`-sekunderna i gångläget, tiden mellan
   kamerahändelser i orbit), mål 22 ms med hysteres. Med `--png` är N_draw
   deterministisk: bara uppskattningen.
2. **`scheduler.*`.** `ahead`: ögat om 1 s. `take_cancellations()` ger påbörjade
   bitar som fallit under N_eff/4 och inte är fasta eller nära.
3. **`viewer_window.*`.** Varje jobb får en egen `std::stop_source`, kopplad med
   `std::stop_callback` till workerns token. `range` per del och parkeringen
   följer N_draw; högst cirka 300 bitar visas eller parkeras per mätning.
   Mätningen sker var 50:e ms medan kameran rör sig och var 100:e ms annars
   (`request_update`). Uppladdningsbudgeten anpassas: 8 MiB per bild medan
   ritytan tickar, 32 MiB när den står still (mönster: `begin_capture`).
4. **Mätning:** `--walk` och `--spin` med p95, och hålen.

### Etapp 4 — Hörnen: platta normaler och delade hörn

Genomförs efter talen i etapp 0.

1. **`build_fragments`.** Ett triangelfragment vars normaler alla är ytans får
   nollnormaler, och dess hörn slås ihop på lika läge, färg och uv. Visarens
   program räknar då den platta normalen ur derivatorna.
2. **Valfritt:** en egen del för platta fragment utan normalström och en
   programvariant utan normalattribut i `textures.cpp`. GLSL och MSL skrivs
   blint.
3. **Mätning:** r före och efter, byte per triangel, bilderna mot de tio
   exempelfilerna.

### Etapp 5 — Instanser för upprepad geometri

Genomförs bara om etapp 0 visar vinst.

1. **`viewer/instancing.h/.cpp` (ny, GlazeUI-fri).** `plan_instancing()`
   grupperar fragment efter `Fragment::record`. En post blir en grupp när minst
   `min_count` fragment visar den, utseendet är ogenomskinligt, posten saknar
   bild och posen går att skriva som vridning och skala per axel.
   `instance_pose()` omvandlar posen till GlazeUI:s instans
   (R = Rz(−yaw)·Rx(pitch)·Ry(roll), skalan före vridningen, `style3d.h`).
2. **`viewer_window.*`.** Ett lager för instanser med en `Shape::instances` per
   grupp; prototypen har en `Lod` med `maxDistance = s_max·K_bak/N_min`.
   Instansernas fragment tas ur bitarna. `hit_fragment` tolkar
   `HitResult::instance`. Modellfiltret bygger om gruppen i en arbetstråd.
   Snittet: instanser helt på den bortklippta sidan tas bort ur gruppen;
   instanser som skär ett plan ritas hela och räknas i statusraden.
3. **Mätning:** byte, ritanrop per bild och p95 med och utan instanser.

### Etapp 6 — Dokumenten, se avsnitt 13

---

## 6. Byte per triangel

r = V/T mäts i etapp 0; r = 1 är räkneexempel. Utan uv; uv lägger till 8 byte per
hörn i både RAM och på grafikkortet.

| Läge | RAM per triangel | Grafikkort per triangel | Summa vid r = 1 |
|---|---|---|---|
| Utgångsläget, som `part_bytes` räknar (56r + 56) | — | — | 112 |
| Utgångsläget, verkligt (antaget sökträd ≈ 20 B/T) | 28r + 12 + 20 | 20r + 12 | 92 |
| Etapp 1–2: sökträd bara nära ögat och för fasta bitar | 28r + 12 | 20r + 12 | 72 |
| Etapp 2: grovbildens dubbletter borta | samma per triangel; upp till 20 % av budgeten frigörs | | |
| Etapp 4: platta normaler och delade hörn (r → r′) | 28r′ + 12 | 20r′ + 12 | r′ = 0,7: 63 |
| Etapp 4, valfritt: platta delar utan normalström | 16r′ + 12 | 16r′ + 12 | r′ = 0,7: 46 |
| Etapp 5: instans | 56 byte per instans + prototypen en gång | 52 byte per instans | oberoende av T |

Den stora vinsten är ändå inte per triangel, utan att bara det som är ≥ N px
byggs.

---

## 7. Mönster att följa

- **GlazeUI-fri logik som prövas i sviten:** `viewer/jpeg.cpp` kompileras in i
  `HNwd_tests` (`tests/CMakeLists.txt`). Nya filer följer samma väg.
- **Trådar över fragmenten med avbrott:** `coarse.cpp:40-64`.
- **Explicit stack:** `cells.cpp:15-48`.
- **Prefixsummor för det som får släppas:** `scheduler.cpp` (`build_victims`,
  `make_room`).
- **Jobb, generation och svar till UI-tråden:** `start_cell_job`,
  `build_cell_job`, `on_built` i `viewer_window.cpp`.
- **Meshen med nivåer:** `viewer_window.cpp:285-315`.
- **Platser i lagren:** `take_slot`/`free_slot`.
- **Stil utan ny uppladdning:** `clipped_shape`.
- **Mätningens utskrift:** `note_stalls`/`stall`, `finish_walk`.
- **Testerna:** `int test_x()` i `tests/test_x.cpp`, deklarerad och anropad i
  `tests/main.cpp`; `CHECK` i `test_support.h`; `testdata::tmp_dir()`; den
  syntetiska filen skriven i testet (`test_synth.cpp`).
- **Kontraktet i headerns kommentar:** som `cells.h:33-38` och `coarse.h:3-14`.

---

## 8. Risker och kantfall

- **Jättefragment.** Ett ensamt fragment över `max_tile_triangles` blir en egen
  bit. En bit vars uppskattning är över en fjärdedel av budgeten byggs inte,
  räknas i statusraden och står på stderr.
- **Delade löv.** Testet prövar att varje bits låda rymmer dess fragment och att
  inget fragment står två gånger.
- **Fragment utan storlek.** Punkter och degenererade lådor får golvet 1e-6 m.
- **Genomskinligt.** Egen del per bit i det genomskinliga lagret. Sorteringen
  saknas fortfarande. De fasta bitarna tar med genomskinligt.
- **Texturerade delar.** En del per material och bit ger fler ritanrop; det
  räknas i ritbudgeten. Delarna instansieras inte.
- **Linjer och texter.** Varje polylinje är en form (`install_lines`) och kostar
  i UI-tråden. Taket för texter står kvar.
- **Modellfiltret.** De berörda bitarna hittas genom `tile_of`.
- **Snittplanen.** `range` och parkeringen går genom `clipped_shape` så att
  planen följer med. Instanser klipps inte (etapp 5).
- **Plockningen.** Ett sökträd som byggs latent i UI-tråden kostar millisekunder
  per bit, och dess minne räknas ungefärligt. Plockningen träffar bara det som
  ritas.
- **Ett nytt K.** Ett större fönster eller annat synfält gör de bakade nivåerna
  inaktuella bortom marginalen 1,5; bitarna byggs då om med låg prioritet.
- **`--png`.** N_draw är deterministisk med `--png`, och urvalet mäts med bildens
  storlek (`capture_tick`), så att garantin gäller i bilden.
- **Markföljningen.** Strålen nedåt behöver ett sökträd för bitarna under ögat;
  det förbereds inom 10 m. Under ett mycket högt N_eff kan ett tunt golv
  försvinna, och då står ögat kvar på samma höjd.
- **Uppladdningen packas i UI-tråden.** Därför anpassas budgeten i etapp 3.
- **Planens eget minne.** `frags`, `sizes` och `tile_of` tar cirka 12 byte per
  fragment. Det räknas och visas.
- **Visning i stora svep.** Därför taket per mätning.

---

## 9. Önskemål till ramverket

Inget av detta görs här. Huvuddelen fungerar med v0.9.4.

1. **Mesh utan CPU-kopia efter uppladdningen.** Sparar 28r + 12 byte per
   triangel i RAM. Kräver punkt 2.
2. **Plockning genom en id-buffert**, eller sökträd byggt ur grafikkortets
   kopia. Sparar cirka 20 byte per triangel och tar bort de latenta byggena i
   UI-tråden.
3. **16-bitars index per segment med högst 65 535 hörn.** 6 byte per triangel.
4. **Packade lägen**, till exempel int16 relativt segmentets låda.
5. **Egna program som ritar instanser**, så att instanserna får visarens ljus
   och snittplan.
6. **Godtyckliga snittplan i det inbyggda programmet.**
7. **Hörnpackningen i en arbetstråd.**
8. **Billigare ritanrop för många små meshar med samma program.**
9. **Detaljnivå i px i stället för avstånd.**
10. **Residensens verkliga byte per mesh**, läsbara för appen.

---

## 10. Plattformar

- **Windows** byggs och provas.
- **Linux och macOS** skrivs blint och antecknas i `doc/os/<os>/TESTS.md`:
  - `system_memory()`: talen rimliga mot systemets egna verktyg.
  - `memoryBytes` på macOS är delat minne, så förvalet där blir en gemensam
    budget.
  - Bildtakten och garantin med `--walk` och `--spin` mot den syntetiska filen.
  - Etapp 4, om programvarianten utan normal görs: GLSL och MSL.
  - `std::stop_source`/`std::stop_callback` per jobb.
- `windows.h` dras in bara genom `path_utf8.h`.

---

## 11. Uppdelning för implementationen

Högst två delar åt gången, med disjunkta filmängder. Samordnaren äger
`viewer/CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/main.cpp` och
`tests/test_support.h`/`test_data.h`, och låser gränssnitten nedan innan delarna
börjar. `viewer_window.h/.cpp` och `main.cpp` ägs alltid av del A. Del B äger de
GlazeUI-fria filerna och deras tester.

| Etapp | Del A | Del B |
|---|---|---|
| 0 | `viewer_window.h/.cpp`, `main.cpp` | `tools/synth_writer.h`, `tools/nwdsynth.cpp`, `tests/test_synth.cpp`, `viewer/cells.h/.cpp`, `viewer/model.h/.cpp`, `viewer/sysmem.h/.cpp`, `tests/test_sysmem.cpp` |
| 1 | `viewer_window.h/.cpp`, `main.cpp` | `viewer/cells.h/.cpp`, `viewer/scheduler.h/.cpp`, `viewer/sysmem.h/.cpp`, `tests/test_viewer_cells.cpp`, `tests/test_sysmem.cpp` |
| 2 | `viewer_window.h/.cpp`, `main.cpp` | `viewer/lod_tree.h/.cpp`, `viewer/scheduler.h/.cpp`, `viewer/cells.h/.cpp`, `tests/test_lod_tree.cpp`, `tests/test_tile_scheduler.cpp`, `tests/test_viewer_cells.cpp` |
| 3 | `viewer_window.h/.cpp` | `viewer/governor.h/.cpp`, `viewer/scheduler.h/.cpp`, `tests/test_governor.cpp`, `tests/test_tile_scheduler.cpp` |
| 4 | (bara vid programvarianten: `viewer/textures.h/.cpp`) | `viewer/cells.h/.cpp`, `tests/test_viewer_cells.cpp` |
| 5 | `viewer_window.h/.cpp` | `viewer/instancing.h/.cpp`, `viewer/lod_tree.cpp`, `tests/test_instancing.cpp` |

### Gränssnitt som låses

Etapp 0:
```cpp
// viewer/cells.h — sist i CellGeometry
    double read_seconds = 0;      // i GeometryReader::fragment_mesh
    double convert_seconds = 0;   // världen, normalerna och delarna
    uint64_t vertices = 0;        // hörn i delarna
    uint64_t flat_fragments = 0;  // triangelfragment vars normaler alla är ytornas (inom 1e-3)

// viewer/model.h
// hist[k] = poster som visas av [2^k, 2^(k+1)) fragment.
struct RecordUse {
    std::vector<uint64_t> hist;
    uint64_t records = 0, triangles_once = 0, triangles_placed = 0;
};
RecordUse record_use(const Model& m, std::stop_token stop);

// viewer/sysmem.h (ny)
struct SystemMemory { uint64_t physical = 0, available = 0, process = 0; };
bool system_memory(SystemMemory& out, std::string& error);   // false och en text när systemet inte svarar

// tools/synth_writer.h — sist i synth::Options
    double min_size = 0, max_size = 0;   // meter; 0 = postens längd som förut, annars log-likformig
    uint32_t max_segments = 12;          // stripens längsta längd
```

Etapp 1:
```cpp
// viewer/cells.h
struct PartBytes { size_t ram = 0, gpu = 0; };
// CPU-kopian 12 + 12 + 4 (+ 8 med uv) byte per hörn och 4 per index;
// grafikkortet 12 + 4 + 4 (+ 8) per hörn och 4 per index.
PartBytes part_bytes(const MeshPart& p);
PartBytes pick_bytes(uint64_t triangles);        // plockningens sökträd
inline constexpr size_t kPickBytesPerTriangle = 20;

// viewer/scheduler.h — LodScheduler (ViewFrustum utan detail_radius/detail_pixels)
struct Settings { double min_pixels = 32; PartBytes budget; };
bool full_done(const std::vector<Cell>& cells, uint32_t cell, PartBytes bytes, bool ok,
               std::vector<uint32_t>& evict);
void add_bytes(uint32_t cell, PartBytes extra);
void add_fixed(PartBytes b); void remove_fixed(PartBytes b);
PartBytes resident() const; PartBytes fixed() const; PartBytes budget() const;

// viewer/sysmem.h
struct Budgets { PartBytes budget; bool shared = false; };   // shared: en budget för båda
Budgets default_budgets(const SystemMemory& sys, uint64_t gpu_memory, bool gpu_shared,
                        size_t document_bytes);
```

Etapp 2:
```cpp
// viewer/lod_tree.h (ny)
struct Tile {
    Box bounds;                         // fragmentens låda, meter
    float max_size = 0, min_size = 0;   // fragmentens diagonal, meter
    uint32_t node = 0;
    uint32_t frag_begin = 0, frag_count = 0;   // LodTree::frags, störst först
    uint64_t primitives = 0;
    bool pinned = false;                // grundbilden: byggs först, släpps aldrig
};
struct LodNode {
    Box bounds;
    float max_size = 0;                 // största fragmentet i delträdet
    uint32_t parent = HNwd::kNone;
    uint32_t first_child = 0, child_count = 0;   // LodTree::children
    uint32_t first_tile = 0, tile_count = 0;     // LodTree::tiles
};
struct LodTree {
    std::vector<LodNode> nodes;         // [0] roten, föräldern före barnen
    std::vector<uint32_t> children;
    std::vector<Tile> tiles;
    std::vector<uint32_t> frags;        // fragmentindex i dokumentet
    std::vector<float> sizes;           // diagonalen per post i frags, meter
    std::vector<uint32_t> tile_of;      // per fragment i dokumentet; HNwd::kNone utan låda
    size_t memory_bytes() const;
};
struct LodTreeSettings {
    double alpha = 1.0 / 32;
    uint32_t max_leaf_fragments = 64;
    uint64_t max_tile_triangles = 32'768;
    uint64_t min_tile_triangles = 2'048;
    size_t pinned_bytes = 0;            // grundbildens del av budgeten
    double bytes_per_primitive = 72;    // uppskattningen före bygget
    unsigned threads = 1;
};
struct LodTreeStats { double seconds = 0; uint32_t split_nodes = 0, pinned_tiles = 0;
                      uint64_t moved_up = 0, pinned_primitives = 0; };
bool plan_lod_tree(const Model& m, const LodTreeSettings& s, std::stop_token stop,
                   LodTree& out, LodTreeStats& stats);
struct NodeBox { Box bounds; uint32_t node = 0; };
std::vector<NodeBox> tree_boxes(const LodTree& t, size_t max_boxes);

// viewer/cells.h
struct FragmentRange { uint32_t first_triangle = 0; uint32_t fragment = 0; float size = 0; };
struct LodStep { uint32_t triangles = 0; double max_distance = 0; };   // 0 = obegränsat, bara sist
std::vector<LodStep> lod_steps(const std::vector<FragmentRange>& ranges, uint32_t triangles,
                               double min_pixels, double pixels_per_radian, int max_steps = 4);
uint32_t triangles_at_least(const std::vector<FragmentRange>& ranges, uint32_t triangles,
                            float min_size);
HNwd::Status build_tile(HNwd::GeometryReader& reader, const Model& m, const LodTree& tree,
                        uint32_t tile, const BuildLimits& limits, std::stop_token stop,
                        CellGeometry& out, std::string& error);

// viewer/scheduler.h — ViewFrustum får:
    double pixels_per_radian() const;   // height / (2 tan_half_fov)
    double distance(const Box& b) const;   // ögat till lådans närmaste punkt; 0 inuti
    bool sees(const Box& b) const;      // lådans omskrivna sfär når in i bilden

class TileScheduler {
public:
    struct Settings {
        double min_pixels = 3;          // garantins tröskel
        double max_pixels = 32;         // högst så här högt höjs minnets tröskel
        double keep_factor = 0.5;       // byggt behålls före annat ned till tröskeln × detta
        PartBytes budget;
    };
    void reset(const LodTree& tree, const Settings& s);
    void update(const LodTree& tree, const ViewFrustum& view, double now);
    std::vector<uint32_t> next_jobs(size_t max_jobs, std::vector<uint32_t>& evict);
    bool tile_done(uint32_t tile, PartBytes bytes, bool ok, double now,
                   std::vector<uint32_t>& evict);
    void add_bytes(uint32_t tile, PartBytes extra);
    bool invalidate(uint32_t tile);
    void shown_changes(double draw_pixels, size_t max_changes,
                       std::vector<uint32_t>& show, std::vector<uint32_t>& park);
    double threshold() const;           // minnets tröskel N_eff
    double pixels(uint32_t tile) const; // senaste mätningen: s_max·K/avstånd
    bool built(uint32_t tile) const;
    bool idle() const;
    size_t holes(double factor) const;  // i bild, P ≥ tröskeln × factor, inte byggda
    struct Latency { double p50 = 0, p95 = 0, max = 0; size_t count = 0; };
    Latency latency() const;            // från "behövs" till "byggd"
    PartBytes resident() const; size_t pending() const; size_t built_count() const;
    uint64_t evictions() const;
};
```

Etapp 3:
```cpp
// viewer/scheduler.h — TileScheduler
    void update(const LodTree& tree, const ViewFrustum& view, const ViewFrustum* ahead, double now);
    std::vector<uint32_t> take_cancellations();

// viewer/governor.h (ny)
struct DrawLoad { uint64_t triangles = 0; uint32_t draws = 0; };
struct GovernorSettings {
    uint64_t max_triangles = 20'000'000;   // sätts ur etapp 0
    uint32_t max_draws = 3'000;            // sätts ur etapp 0
    double target_ms = 1000.0 / 45;
    double min_pixels = 3, max_pixels = 32;
};
class DrawGovernor {
public:
    explicit DrawGovernor(const GovernorSettings& s);
    // Ritningens tröskel: den lägsta vars belastning ryms. frame_ms 0 = okänd.
    double next(const std::function<DrawLoad(double pixels)>& load_at, double frame_ms,
                bool deterministic);
    double pixels() const;
};
```

Etapp 5:
```cpp
// viewer/instancing.h (ny)
struct InstancePose { double at[3] = {0, 0, 0}; float yaw = 0, pitch = 0, roll = 0;
                      float scale[3] = {1, 1, 1}; };
// Postens lägen relativt `center` (lådans mitt, postens enhet) skalade till meter;
// false när posen inte går att skriva som vridning och skala per axel.
bool instance_pose(const HNwd::Pose& pose, double metres_per_unit, const float center[3],
                   InstancePose& out);
struct InstanceGroup { uint32_t record = 0; uint32_t triangles = 0; float max_size = 0;
                       std::vector<uint32_t> fragments; };
struct InstancingSettings { uint32_t min_count = 16; uint32_t max_triangles = 65'536;
                            size_t max_groups = 4'096; };
struct InstancePlan { std::vector<InstanceGroup> groups; uint64_t fragments = 0, saved_bytes = 0; };
bool plan_instancing(const Model& m, const InstancingSettings& s, std::stop_token stop,
                     InstancePlan& out);
```

---

## 12. Tester

Alla nya tester är `int test_x()` i `tests/test_x.cpp`, deklarerade och anropade
i `tests/main.cpp` och listade i `tests/CMakeLists.txt`. Visarens GlazeUI-fria
`.cpp` kompileras in där. Varje nytt test bryts tillfälligt för att se att det
faller, och rapporten säger hur.

| Test | Etapp | Prövar |
|---|---|---|
| `test_synth` (utökat) | 0 | förvalen ger samma fil som förut; med `min_size`/`max_size` ligger varje fragments diagonal inom intervallet; `max_segments` ger det begärda antalet primitiver |
| `test_sysmem` | 0–1 | `system_memory()` ger fysiskt > 0, tillgängligt > 0 och process > 0; `default_budgets` klämmer, delar och subtraherar dokumentet enligt reglerna |
| `test_viewer_cells` | 1, 2, 4 | `part_bytes` mot formeln med och utan uv; `lod_steps`: för slumpade avstånd tar den nivå GlazeUI:s regel väljer med varje fragment med s·K/d ≥ N_min, nivåerna växer och är hela trianglar, och bortom den sista finns inget fragment ≥ N_min; `triangles_at_least` mot en linjär sökning; i etapp 4 att hopslagningen av hörn ger samma trianglar i världen och att bara platta fragment får nollnormaler |
| `test_lod_tree` | 2 | på en syntetisk fil med spridda storlekar och på exempelfilerna: varje fragment med låda står i exakt en bit; `tile_of` stämmer; bitarna står störst först; `max_size`/`min_size` stämmer; bitens låda rymmer fragmentens; trianglarna ≤ taket utom för ett ensamt fragment; nodens `max_size` ≥ delträdets; placeringsregeln; stora löv delade; samma resultat med 1 och 4 trådar; de fasta bitarnas uppskattning ≤ andelen |
| `test_tile_scheduler` | 2–3 | garantin: för slumpade kameror är varje fragment i bild med s·K/d ≥ N_eff (brute force ur fragmentens egna lådor) i en bit som är byggd eller väntar när urvalet står still; vandringen ger samma mängd som en mätning av varje bit; med en snål budget höjs N_eff tills det som behövs ryms, och osynligt släpps före synligt smått; fasta bitar släpps aldrig; hysteresen; `idle`; latensen med givna tider; `ahead` och `take_cancellations` |
| `test_governor` | 3 | belastning över taket höjer N_draw och under taket sänker den med hysteres; deterministiskt läge utan bildtid; gränserna |
| `test_instancing` | 5 | `instance_pose` mot `HNwd::world_point` inom 1e-6 relativt för spegling, enhetlig och axelvis skala; skjuvning ger false; grupperingen och trösklarna |

**Före varje commit**, på Windows:
1. Release-bygge med 0 varningar.
2. `./build/tests/HNwd_tests` en gång från repots rot.
3. `nwdview <fil> --png <bild> --profile` mot de tio exempelfilerna; bilderna
   granskas.
4. Den stora syntetiska filen med `--png --profile --walk 150 --speed 3`, med
   `--spin 1` och med `--tour`. Talen i avsnitt 4 skrivs i commit-meddelandet.

---

## 13. Dokumenten sist

- **Rot-`CLAUDE.md`, stycket om `viewer/`:** detaljträdet, bitarna och garantin;
  `--budget` (RAM), `--gpu-budget`, `--tile`, `--min-pixels` och `--spin`; de
  GlazeUI-fria filerna; att `coarse.*` är borta; att testerna bygger in visarens
  filer.
- **`TODO.md`:** önskemålen i avsnitt 9, att instanser inte klipps (om etapp 5
  görs), sorteringen av genomskinligt, och osäkerheterna i 2c som inte blivit
  belagda.
- **`doc/CHANGELOG.md`:** en post per etapp.
- **`README.md` och `README.sv.md`:** visarens flaggor och budgetens innebörd.
- **`doc/os/linux/TESTS.md` och `doc/os/macos/TESTS.md`:** raderna i avsnitt 10.
- **`viewer/main.cpp`:** huvudkommentaren med flaggorna.

---

## 14. Det som är osäkert

1. **r = V/T, andelen platta fragment och postanvändningen.** Etapp 0 mäter det,
   och etapp 4 och 5 genomförs bara om talen motiverar dem.
2. **Kostnaden per ritanrop och per föremål.** Den avgör α, bitens storlek,
   `max_draws` och om 45 bilder per sekund nås utan önskemål 8.
3. **Sökträdets storlek** är en uppskattning. Den mäts genom processens minne.
4. **Hur mycket hörnpackningen i UI-tråden kostar per MiB.** Mäts i etapp 3.
5. **Hur jämnt filers löv är fördelade.** Delningen vid medianen gör garantin
   oberoende av det; bara ritanropen påverkas.

---

## 15. Mätvärden

### Efter etapp 0 och 1

Mätt på Windows med ett diskret grafikkort med 8 GB minne och 64 GB RAM, mot två
syntetiska filer (`--sizes 0.02:40 --detail 100`): en med 300 000 fragment och
28 miljoner trianglar, en med 3 miljoner fragment och 280 miljoner trianglar.

| Tal | Värde | Följd |
|---|---|---|
| Bildtid med 23–29 miljoner ritade trianglar och cirka 950 ritanrop | p50 4,2 ms, p95 4,3 ms; grafikkortet 4,1 ms | Trianglarna är inte det som begränsar vid den mängden. `max_triangles` sätts till 60 miljoner. |
| UI-trådens del av en bild med cirka 950 ritanrop | innehållet p50 1,2 ms | Ungefär 1,2 µs per ritanrop. `max_draws` sätts till 4 000. |
| Plockningens sökträd | 19–23 s för 22–29 miljoner trianglar, cirka 0,8 µs per triangel | Den största delen av byggtiden i den mindre filen. Sökträdet byggs bara där det behövs. |
| Läsningen | 4,4 s för 22 miljoner trianglar i den mindre filen; 66 s för 29 miljoner i den större (p95 223 ms per bit om 50 000 trianglar) | Fragment utspridda i filen läses långsamt. En bit läser sina fragment i geometripostens ordning. |
| Omräkningen och meshbygget | 0,6 s respektive 0,4 s för 22 miljoner trianglar | Försumbart. |
| Hörn per triangel | 1,02 i de syntetiska filerna | Säger inget om verkliga filer; mäts där med `--profile`. |
| Platta fragment | 0 % i de syntetiska filerna | Som ovan. |
| Postanvändningen | 5 fragment per post i de syntetiska filerna (bestämd av flaggorna) | Som ovan. |
| Processens minne minus det räknade | 0,6 GB i den mindre filen, 1,8 GB i den större | Växer med filens storlek: den minnesmappade filens lästa sidor räknas i processen. |
| Förvalda budgetar på maskinen | RAM 32 GB, grafikkort 4,8 GB | Grafikkortets minne är den snävare gränsen: cirka 150 miljoner trianglar. |
| Läsarnas cache | 16 läsare × 64 MB = 1 GB fast | Stort på en mindre maskin; storleken följer RAM-budgeten i etapp 2. |

Gången genom den större filen byggde bara fem celler i detalj: en syntetisk
modell är gles, så den mäter bildtakt och läsning men inte urvalet i en tät
byggnad.

### Efter etapp 2

Samma maskin och den större syntetiska filen (280 miljoner trianglar), ett
orbitvarv.

| Tal | Värde | Följd |
|---|---|---|
| Bitar om högst 32 768 trianglar, golv 2 048 | 99 000 bitar, 8 300 fasta; 22 800 ritanrop per bild; bildtid p50 112 ms; grundbilden klar efter 105 s | Ritanropen är gränsen, cirka 3,5–5 µs styck i UI-tråden. Långa bilder stryper också byggtakten, eftersom svaren från byggjobben tas emot mellan bilderna. |
| Bitar om högst 131 072 trianglar, golv en fjärdedel av taket | 2 400 bitar, 268 fasta; 1 020 ritanrop per bild; bildtid p50 8,9 ms, p95 12,5 ms; grundbilden klar efter 16 s | Förvalet: `--tile` 131 072, golvet en fjärdedel av taket. `max_draws` sätts till 3 000. |
| Läsningen av grundbilden | 25 s trådtid för 29 miljoner trianglar, p95 251 ms per bit | Grundbildens fragment ligger utspridda i filen. |

En fast bit visas så snart den är byggd, parkeras aldrig, och dess sista
detaljnivå ritas på alla avstånd: annars är en modell sedd på långt håll tom.

I en tät modell kostar varje ritad triangel mångdubbelt mer på grafikkortet än i
den syntetiska, eftersom ytorna täcker varandra och varje pixel räknas många
gånger. Etapp 3 får därför två åtgärder till, utöver ritningens tröskel:

- **Närmast först.** De visade bitarna ritas i ordning efter avståndet till
  ögat, så att grafikkortets djuptest kan hoppa över täckta pixlar.
- **Kantutjämningen** sänks medan kameran rör sig och bildtiden är över målet,
  och höjs igen när den står still.

Etapp 4 görs inte: hörnen delas redan (hörn per triangel nära 1), så platta
normaler och delade hörn sparar lite. Etapp 5 (instanser) avgörs av
`--profile`-raden om vad instanser sparar vid olika gränser.

### Etapp 3b — UI-tråden hänger aldrig

Kravet: ingenting tungt går i UI-tråden, och fönstret svarar alltid, också när
ritningen inte hinner med. UI-tråden tar bara emot färdiga svar, sätter former i
lagren, läser kameran och uppdaterar texter. Allt som läser filen, räknar över
fragment eller bitar, sorterar, bygger geometri, träffprovar eller räknar
belastning görs i en arbetstråd. Ett enskilt arbete i UI-tråden tar högst 2 ms,
och det som kan bli många (visa, parkera, sätta om ritad andel, omordna, ta emot
byggda bitar) delar en tidsbudget om 2 ms per pumpvarv och fortsätter i nästa.
`--profile` räknar stopp över 4 ms efter startvyn och skriver det längsta med
namn; ett stopp över 16 ms är ett fel på stderr.

Det som går i UI-tråden och inte kan flyttas från visaren är ramverkets ritning:
ritanropen (några mikrosekunder styck — därför `max_draws` 800), uppladdningen
(därför 256–512 KiB per bild) och väntan på grafikkortet vid bildbytet (därför
ritningens tröskel). Att flytta dem kräver en ritningstråd i ramverket.

Det som i dag kan ta längre:

- **Träffprovet.** Klicket, koordinaten under pekaren och strålen nedåt i
  gångläget går genom ramverkets plockning, som bygger en meshs sökträd på plats
  första gången den nås — hundratals millisekunder per bit.
- **Uppladdningen.** Hörnen packas i UI-tråden, cirka 125 MB per sekund. En
  budget om 8–32 MiB per bild ger bilder på 60–250 ms.
- **Ritningens tak börjar högt** och sänks först när bilderna redan blivit
  långa.

Åtgärderna:

1. **Plockningen i arbetstrådar, utan ramverkets sökträd.** Meshen skapas med
   `keepCpuCopy = false`, så ramverkets plockning rör den inte och inget sökträd
   byggs. Nya `viewer/pick.h/.cpp` (ren std och HNwd): en stråle mot en bits
   trianglar (`CellGeometry`), och bitarna längs en stråle i den ordning strålen
   når deras lådor. Ett klick eller en koordinat blir ett jobb i en arbetstråd:
   bitarna längs strålen byggs som geometri (`build_tile`, utan mesh) och prövas i
   ordning tills en träff ligger närmare än nästa låda. Det senaste jobbet
   gäller; ett äldre avbryts. Bitarnas geometri för plockning hålls i en liten
   cache (senast använd släpps först, med ett tak i byte som räknas i
   RAM-budgeten).
2. **Marksökningen** (gångläget med höjden över ytan) läser bara ur cachen:
   bitarna under ögat hålls där av ett jobb som följer ögat. Finns ingen
   geometri än står ögat kvar på sin höjd, som när ingen yta finns.
3. **Uppladdningsbudgeten** är 256 KiB per bild medan kameran rör sig och
   512 KiB när den står still.
4. **Ritningens tak börjar lågt** (`GovernorSettings::start_scale`, förval 0,2)
   och höjs medan bildtiden håller.
5. **Jobben för plockningens sökträd** och deras minnesräkning (`pick_bytes`,
   `add_bytes` för sökträd) tas bort.

Tester: `test_pick` (strålen mot trianglar jämförd med en rak genomgång i
double, träffens fragment, punkt och avstånd; bitarna längs en stråle i rätt
ordning mot en mätning av varje bit; ritad andel, modellfilter och snittplan;
strålen nedåt), `test_governor` utökat med starten.

```cpp
// viewer/pick.h (ny)
struct PickRay { double origin[3] = {0, 0, 0}; double dir[3] = {0, 0, 1}; };   // dir behöver inte vara normerad
struct PickHit {
    bool hit = false;
    double distance = 0;          // längs strålen, i strålens enhet (meter när dir är normerad)
    double point[3] = {0, 0, 0};  // världen, meter
    uint32_t fragment = HNwd::kNone;
};
// Hur stor del av varje del av en bit som ritas: de första `drawn[i]`
// trianglarna i del i. En tom lista = allt.
struct PickLimits { std::vector<uint32_t> drawn; const ClipState* clip = nullptr; };
// Närmaste träff mot bitens trianglar, framsida som baksida; en träff på den
// bortklippta sidan av ett snittplan räknas inte.
PickHit pick_geometry(const CellGeometry& g, const PickRay& ray, const PickLimits& limits,
                      double max_distance);
struct TileAlongRay { uint32_t tile = 0; double enter = 0; };   // avståndet där strålen når lådan
// De givna bitarna vars låda strålen når inom `max_distance`, närmast först.
void tiles_along_ray(const LodTree& tree, const std::vector<uint32_t>& tiles, const PickRay& ray,
                     double max_distance, std::vector<TileAlongRay>& out);

// viewer/governor.h — sist i GovernorSettings
    double start_scale = 0.2;   // takens andel när regleringen börjar; höjs medan bildtiden håller
```

### Efter etapp 3b

Mätt på den mindre syntetiska filen med en snål grafikbudget (`--gpu-budget 250`),
så att bitar byts ut hela tiden under två orbitvarv:

| Tal | Värde | Följd |
|---|---|---|
| Linjerna som en form per polylinje i ett eget lager | ramverket bygger om och laddar upp hela lagret när en fjärdedel av det bytts ut: 13 MB uppladdat per bild i snitt, bildtid p99 364 ms; utan linjer 0,14 MB och p99 4,9 ms | Linjerna ritas som en del av bitens mesh (etapp 3c). |
| Minnets tröskel | en bit byggs hel så snart dess största fragment behövs, så budgeten fylls av fragment som är långt under tröskeln i bild | Bitar byggs till den storlek som behövs (etapp 3c). |
| Ritanropen | ritningens fas tar cirka 1–2 µs per anrop; ett tak på 800 höjer ritningens tröskel i onödan när många bitar syns | Visarens förval för `max_draws` är 3 000. |
| Urvalets mätning i UI-tråden | växer med antalet bitar som behövs, och går över 4 ms i en modell med tiotusentals bitar | Urvalet flyttas till en egen tråd (etapp 3d). |
| Träffprovet i arbetstråden | bitarna längs strålen läses ur filen, upp till sekunder för ett klick i en stor fil | Godtagbart utanför UI-tråden, men långsamt. |

### Etapp 3c — Linjerna i meshen, och bitar byggda till den storlek som behövs

**Linjerna.** En bits linjer blir en egen del av dess geometri och ritas som en
mesh med ett eget program, i samma lager och med samma uppladdning, detaljnivåer
och parkering som resten av biten. Linjelagret med en form per polylinje
försvinner. Ett segment är fyra hörn och två trianglar; varje hörn bär
segmentets andra ände och vilken sida och ände det är, och programmet flyttar
hörnet i bilden så att linjen blir en fast bredd i px. Segmenten står med de
största fragmenten först, som trianglarna, så att detaljnivåerna och den ritade
andelen gäller dem också. Träffprovet prövar inte linjedelen.

**Bitar byggda till en storlek.** Ett byggjobb får en minsta fragmentstorlek, och
bara bitens fragment som är minst så stora byggs — ett prefix, eftersom bitens
fragment står störst först. Storleken är den som blir minnets tröskel på bitens
avstånd, delad med två som marginal. När ögat kommit så nära att ett fragment
under den byggda storleken blivit minst tröskeln i bild byggs biten om med en
mindre storlek; den gamla visas tills den nya är klar. Fasta bitar byggs hela.
Garantin står kvar: varje fragment som är minst tröskeln i bild ligger i det
byggda prefixet. Minnet som en bit behöver uppskattas ur prefixets primitiver.

```cpp
// viewer/cells.h — MeshPart
    bool lines = false;           // en linjedel: fyra hörn och två trianglar per segment
    std::vector<float> other;     // linjedel: xyz per hörn, segmentets andra ände relativt origin
    std::vector<float> corner;    // linjedel: två tal per hörn — sidan (−1 eller +1) och änden (0 eller 1)
// CellGeometry::parts: [0] ogenomskinligt, [1] genomskinligt, [2] linjer, sedan
// en del per material med bild. `polylines` utgår.
// viewer/cells.h — BuildLimits
    float min_size = 0;           // fragment med mindre diagonal byggs inte; 0 = alla

// viewer/scheduler.h — TileScheduler
    struct Job { uint32_t tile = 0; float min_size = 0; };   // 0 = hela biten
    // Ersätter next_jobs: också ombyggnader av bitar som är byggda till en för
    // stor storlek.
    std::vector<Job> next_build_jobs(size_t max_jobs, std::vector<uint32_t>& evict);
    // Ersätter tile_done: `min_size` är storleken jobbet byggdes till.
    bool tile_done(uint32_t tile, float min_size, PartBytes bytes, bool ok, double now,
                   std::vector<uint32_t>& evict);
    float built_min_size(uint32_t tile) const;   // 0 = hela biten; meningslös för en obyggd
```

Tester: `test_viewer_cells` (linjedelen: fyra hörn per segment med rätt ändar,
sidor och index; störst först; `min_size` ger exakt prefixet), `test_pick`
(linjedelen träffas inte), `test_tile_scheduler` (storleken följer avståndet och
tröskeln; ombyggnaden när ögat kommer närmare och inte annars; garantin brute
force mot det byggda prefixet; minnet räknas för prefixet; fasta bitar hela).

### Etapp 3d — Urvalet i en egen tråd

Schemaläggaren och regleringens underlag körs i en egen tråd i visaren. UI-tråden
lämnar kamerans läge och de byggda bitarnas svar och får tillbaka färdiga listor:
vad som ska byggas, släppas, visas, parkeras och ordnas, och tröskeln. Ingen
mätning av urvalet sker i UI-tråden.

Det som etappen åtgärdar:

| Vad | Åtgärd |
|---|---|
| Urvalets mätning, kandidaterna, ordningen och regleringens sökning går i UI-tråden och växer med antalet bitar | De räknas i urvalets tråd. |
| Meshar frigörs i UI-tråden när bitar släpps eller ersätts | De lämnas till en arbetstråd. |
| En tom form i en plats i ett meshlager räknas av ramverket som död, och lagret byggs om när en fjärdedel är död | En plats töms aldrig: en parkerad del står kvar med ritad andel noll, och en ledig plats får en platshållare eller en ny mesh på plats. |
| Varje ändring i textlagret kostar ett textbygge | Texterna ändras i klump, högst två gånger per sekund. |
| Regleringen följer tiden mellan bilder, som också innehåller annat arbete | Den följer ritningens egen kostnad: grafikkortets tid och ritfasens tid. |

Mätt på den mindre syntetiska filen med en snål grafikbudget, två orbitvarv:
bildtid p99 5,2 ms (364 ms före etapp 3c), inga ombyggda lager, ett stopp över
4 ms i UI-tråden.

### Ändringar i de låsta gränssnitten

- `GovernorSettings::max_draws = 3'000`.
- `DrawGovernor` får `bool reduce_quality() const`: sant medan bildtiden är över
  målet trots att ritningens tröskel står på sitt tak eller belastningen redan
  ryms — anroparen sänker då kantutjämningen.
- `TileScheduler::shown_order(const ViewFrustum& view, std::vector<uint32_t>& out)`:
  de visade bitarna, närmast ögat först (avståndet till bitens låda).

- `LodScheduler::Settings` och `TileScheduler::Settings` har `bool shared = false`
  sist: `budget.ram` gäller då RAM och grafikkort tillsammans.
- `build_tile` läser fragmenten i geometripostens ordning och lägger trianglarna
  i bitens ordning, störst först.
- `GovernorSettings`: `max_triangles = 60'000'000`, `max_draws = 4'000`.
