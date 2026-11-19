# Egenskaperna i skelettläget och de nästlade modellerna

## 1. Bakgrund och mål

Två brister i läsaren.

**Egenskaperna i skelettläget.** Med `TreeContent::Structure` läser
`PropertyReader::read()` en nods flikar ur filen genom `detail::TabReader`. I ett
stort träd med upprepade delträd kostar det upp till en millisekund per nod.

**De nästlade modellerna.** `Document::model()` lämnar bara ut toppnoderna av
klass 32. En nod av klass 32 kan också sitta under en annan, i flera nivåer, med
egen enhetskod, källa och inläsare.

Mål:

- **M1.** I ett syntetiskt träd med delade flikar, upprepade delträd och flera
  egenskapsströmmar, läst nod för nod i expanderad preorder med förvald cache,
  är läsarens uppackade byte (`TabReaderStats::inflated`) högst 1,1 gånger det
  en läsning av varje lagrad nod en gång i stigande ordning kostar, plus 1 MiB.
- **M2.** Cachen håller högst `cache_bytes` (förval 16 MiB) efter varje
  `read()`. Därtill håller läsaren nio uppackningar med en buffert på 288 KiB
  var och det största enskilda objekt en nod läser.
- **M3.** Resultatet är exakt detsamma som med `TreeContent::Full`, också när
  cachen är 0 eller liten. Trådreglerna i `HNwd.h` håller: en läsare per tråd,
  inga lås, inga trådar.
- **M4.** `model()` ger alla förekomster av klass 32 i preorder, med `parent`
  och `depth`; `model_of_node()` och `model_of_fragment()` ger den innersta.
  Visaren beter sig som förut.

Enhetskoden lämnas ut som den står. Om posen för en nästlad modells fragment
redan är omräknad till den omslutande enheten är inte belagt, och löses inte här.

## 2. Varför läsningen är långsam

`TabReader::Source::open()` (`src/core/tab_reader.cpp`) väljer en uppackning som
står på eller före positionen och på eller efter närmaste kontrollpunkt. Finns
ingen sådan öppnas strömmen om från kontrollpunkten och hoppar fram till
positionen. Kontrollpunkterna står ungefär var `kCheckpointEvery` byte, så en
omstart kostar i snitt halva det avståndet, och uppackningen fyller dessutom sin
buffert förbi positionen.

Två mönster ger omstarter. Båda är mätta med en räknare i `Source::open()`:

- **Upprepade delträd.** Den publika läsaren går i expanderad preorder, och
  `Occurrences::stored()` (`src/scene/tree.cpp`) ger samma lagrade nod för varje
  förekomst. En upprepad förekomst hoppar bakåt i strömmen: en omstart för
  nodens attribut (markör 0) och en för dess egenskapsrötter (markör 1). I ett
  träd där nära hälften av förekomsterna är upprepningar är det den stora
  kostnaden. De upprepade noderna är små: några få egenskaper, och de flesta
  har inga flikar alls.
- **Delade flikar.** `import_shared_tabs()` (`src/core/hierarchy.cpp`) läser
  varje flik som nodens attribut pekar bakåt på från flikens egen position
  (markör 2). Uppackningen som läste fliken står sedan efter den, så nästa nod
  som pekar på samma flik kan behöva börja om. Hur ofta beror på filen: i
  exempelfilerna nästan aldrig, i andra träd för nästan varannan öppning.

I lagrad ordning utan delade flikar kostar en nod några mikrosekunder. Fönstret
på en byte, allokeringarna per anrop och uppslagen i skelettet är alltså inte
orsaken.

Ingenting hålls mellan anropen: `parse_node_tabs()` bygger en ny `Parser` per
anrop, och `TabReader` behåller bara var uppackningarna står.

## 3. Lösningen

### Uppgift 1

- **En cache per läsare** i `TabReader`, med ett tak i byte och äldsta ut
  först. Två slags poster: tolkade flikar ur trädets ström som flera noder
  delar (nyckel: flikens id), och flikarna för noder med mer än en förekomst
  (nyckel: den lagrade nodens index).
- **`TreeData::repeated`**: en bit per lagrad nod, sann om noden har mer än en
  förekomst. Räknas i `expand()`, bara i Structure.
- **Tätare kontrollpunkter**: `kCheckpointEvery` 1 MiB i stället för 4 MiB.
  Kontrollpunkterna tar då cirka 3 % av den uppackade storleken i stället för
  0,8 %.
- **Räknare** (`TabReaderStats`) som proven läser. Inga tidsgränser i sviten.

Inte valt: längre historik i `InflateStream` (rör den vältestade
uppackningen), en uppackning per ström (hundratals strömmar gånger 288 KiB),
fler uppackare per markör (hjälper inte: varje uppackare som läst en flik står
efter den), massläsning av ett intervall (nytt API, och samma minnesfråga).
Nästa åtgärd om omstarterna ändå dominerar: ett tak för hur långt `fill()`
läser förbi efter en omstart.

### Uppgift 2

- Klass 32 kan bara stå som toppnod eller som barn till klass 32
  (`node_children()` och `begin_slot()` i `hierarchy.cpp`), och partitionssvansen
  läses redan på alla nivåer. Det som saknas är att `find_models()` bara går
  genom toppnoderna.
- En lista i preorder, en modell före dem som sitter i den. `Model` får
  `parent` och `depth` sist. `first_fragment`/`fragment_count` gäller hela
  delträdet, de nästlade modellernas fragment inräknade: en modells egna
  fragment ligger i allmänhet inte i följd.
- För filer med nästlade modeller ändras antalet modeller och deras index.
  Filer utan nästling svarar som förut, och filen med en enda modell ur filens
  egen partition är oförändrad.
- Visaren hoppar över modeller med en förälder: listan, filtret och klicket
  räknar med modellerna på översta nivån (`ModelFilter` i `viewer/cells.cpp`
  antar disjunkta intervall i ordning).

## 4. Låsta gränssnitt

Samordnaren skriver dessa innan delarna startar. De ändras inte av delarna.

```cpp
// ===== include/HNwd.h =====
struct Model {
    std::string_view name;
    std::string_view source;
    std::string_view loader;
    Unit unit;
    uint32_t node = 0;             // en nod av klass 32, eller roten för en fil med en modell
    uint32_t first_fragment = 0;   // fragmenten i modellens delträd, de nästlade inräknade
    uint32_t fragment_count = 0;
    uint32_t parent = kNone;       // närmaste omslutande modell; kNone på översta nivån
    uint32_t depth = 0;            // antalet omslutande modeller
};

// class Document
    size_t model_count() const;
    bool model(size_t i, Model& out) const;
    uint32_t model_of_node(uint32_t node) const;      // den innersta; kNone utan modell och utanför
    uint32_t model_of_fragment(size_t fragment) const;

// class PropertyReader
    Status open(const Document& doc, size_t cache_bytes = size_t(16) << 20);

// ===== src/scene/document_impl.h =====
struct ModelEntry {
    uint32_t node = 0;
    std::string_view name, source, loader;
    int32_t unit_code = 0;
    uint32_t parent = kNone;      // index i TreeData::models
    uint32_t depth = 0;
};
// TreeData: bara i Structure, per lagrad nod, sann om noden har mer än en förekomst
    std::vector<bool> repeated;

// ===== src/core/hierarchy.h =====
inline constexpr uint64_t kCheckpointEvery = uint64_t(1) << 20;

struct NodeTabs {
    StringPool strings;
    std::vector<StoredName> names;
    std::vector<StoredProperty> properties;
    std::vector<StoredTab> tabs;
    size_t memory_bytes() const;   // byte som listorna håller, ur kapaciteten
};

class SharedTabCache {
public:
    virtual ~SharedTabCache() = default;
    // Fliken med id `id` i trädets ström, eller nullptr. Pekaren gäller till
    // nästa anrop på cachen.
    virtual const NodeTabs* find_tab(int32_t id) = 0;
    // Fliken med id `id`, som en NodeTabs med exakt en flik. Cachen får låta
    // bli att behålla den.
    virtual void keep_tab(int32_t id, NodeTabs&& tab) = 0;
};

ErrorCode parse_node_tabs(const Hierarchy& h, const HierarchySkeleton& skeleton, uint32_t node,
                          ObjectSource& source, NodeTabs& out, std::string* detail = nullptr,
                          SharedTabCache* cache = nullptr);

// ===== src/core/tab_reader.h =====
struct TabReaderStats {
    uint64_t reads = 0;        // read() som lyckades
    uint64_t node_hits = 0;    // av dem besvarade ur cachen
    uint64_t tab_hits = 0;     // delade flikar ur cachen
    uint64_t opens[3] = {};    // läsare öppnade per markör: 0 attribut, 1 rötter, 2 delade flikar
    uint64_t restarts = 0;     // öppningar som började om från en kontrollpunkt eller strömmens början
    uint64_t inflated = 0;     // byte som uppackningarna gått fram, överhoppade inräknade
};

class TabReader {
public:
    TabReader(const uint8_t* file, size_t size, const HierarchyStreams& streams, const Hierarchy& h,
              const HierarchySkeleton& skeleton, size_t cache_bytes = 0);
    ErrorCode read(uint32_t stored_node, NodeTabs& out, std::string* detail = nullptr,
                   bool keep = false);
    size_t cached_bytes() const;     // högst cache_bytes efter varje anrop
    TabReaderStats stats() const;
};
```

`read_node_tabs()` står kvar utan cache.

## 5. Delarna

Tre delar med disjunkta filer. Varje del bygger mot de låsta huvudena utan de
andras kod. Under arbetet byggs det bara, med 0 varningar; proven skrivs men
körs inte.

### Del A — core: cacherna och räknarna

Filer: `src/core/hierarchy.cpp`, `src/core/tab_reader.cpp`,
`tests/test_tab_reader.cpp`.

**`hierarchy.cpp`, `Parser`:**

- Ny medlem `SharedTabCache* tab_cache_ = nullptr`. `run_region()` tar den som
  parameter och `parse_node_tabs()` skickar den vidare.
- `bool shared_tab(int32_t id) const`: sann om skelettets referenslista för
  ström 0 har id:t och klassen är 86, 66, 70 eller 76.
- `void copy_out(uint32_t local, NodeTabs& out)`: fliken `h_.tabs[local]` med
  sina egenskaper till en `NodeTabs` med exakt en flik. Strängarna går genom
  poolen, namnparen numreras om lokalt, `prop_begin` blir 0, och listorna
  krymps. `kNone` bevaras exakt: flikens namn, namnparens `display` och
  `internal`, och `text` för de typer som inte bär text. `to_tab()` i
  `tree.cpp` och `same_tabs()` i `test_tab_reader.cpp` skiljer null från tom
  sträng.
- `bool copy_in(const NodeTabs& from, uint32_t& local)`: lägger fliken sist i
  `h_` och ger dess index. Poolen kan ge `LimitExceeded`.
- Fångsten i `begin_slot()`, efter att ett objekt är färdigtolkat: i läget
  Region, med en cache, när läsaren är trädets (inte egenskapsströmmens) och
  objektet är en flik som `shared_tab()` känner: `copy_out` och `keep_tab`.
  Rötterna i egenskapsströmmarna har id ur en annan ström och ska inte med.
- `import_shared_tabs()`: före öppningen av markör 2 frågas cachen; en träff
  ger `copy_in` och ingen öppning. Efter en lyckad tolkning genom markör 2
  läggs fliken i cachen. Pekaren ur `find_tab` används innan cachen anropas
  igen.

**`tab_reader.cpp`:**

- `class TabReader::Cache : public SharedTabCache`: platser i en vektor med en
  LRU-lista genom dem, som `GeometryCursor` i `geometry_segment.cpp`. Nyckeln
  är slaget (0 flik, 1 nod) i de höga 32 bitarna och värdet i de låga. En post
  räknas som `memory_bytes()` plus 64 byte. En post större än taket sparas
  inte; en nyckel som redan finns flyttas först; efter varje inläggning kastas
  de äldsta tills summan ryms.
- `Source` räknar `opens[markör]`, `restarts` (grenen som öppnar strömmen om)
  och `inflated`: positionen när läsaren lämnas tillbaka minus positionen före
  `skip()`.
- `TabReader`: `Cache` byggs bara när taket är över 0. `read()` svarar ur
  nodcachen vid träff; annars `parse_node_tabs()` med cachen; ett fel tömmer
  `out` och lägger ingenting i cachen; med `keep` läggs nodens flikar in.
- `kWindowBytes`, `kCursors` och `kInflaters` ändras inte.

### Del B — scene: modellerna, `repeated`, läsarens tak, visarens rad

Filer: `src/scene/tree.cpp`, `src/scene/property_reader.cpp`,
`viewer/model.cpp`, `tests/test_tree.cpp`, `tests/test_property_reader.cpp`.

- `expand()`: två nya utvärden. `repeated` (bara när den efterfrågas): en bit
  per lagrad nod, satt när en förekomsts lagrade nod redan är sedd; hjälpbiten
  släpps efteråt. Och antalet förekomster med klass 32. `load_tree()` sparar
  `repeated` i trädet när innehållet är Structure.
- `find_models()`: en explicit stack i preorder med ramen
  `{next, end, parent_model, depth}`, som börjar över toppnoderna. `next` går
  till nästa syskon med delträdets storlek. En nod av klass 32 ger en
  `ModelEntry` med svansen läst som förut, `parent` och `depth`, och en ram
  över nodens barn. I andra klasser går den inte ner. Efteråt prövas att
  antalet modeller ur trädet är antalet förekomster av klass 32; annars
  `Corrupt` med en text som börjar "internal error". Filens egen partition blir
  modell som förut.
- `model()` fyller i `parent` och `depth`.
- `model_of_node(n)`: samma krav som `model_count()`. Ur ett och samma träd:
  den sista modellen med `node <= n` (binärsökning; `node` växer med indexet),
  sedan uppåt genom `parent` tills noden ligger i modellens delträd; annars
  `kNone`. `model_of_fragment(f)`: fragmentets seq som i `node_of_fragment()`,
  sedan samma uppslag ur samma träd.
- `TreeData::memory_bytes()` räknar också `repeated`.
- `property_reader.cpp`: `open()` ger `cache_bytes` till `TabReader`; `read()`
  ger `keep` = noden är upprepad.
- `viewer/model.cpp`: hoppa över modeller med en förälder, och räkna
  reservnamnet efter listans längd.

### Del C — den syntetiska skrivaren

Filer: `tools/synth_writer.h`, `tools/nwdsynth.cpp`, `tests/test_synth.cpp`.

Förvalen ger byte för byte samma fil som förut.

- `Options` får `prop_streams` (1), `shared_tabs` (0: varje m:te löv äger en
  ny flik med en egenskap, de andra pekar bakåt på den senaste ägarens),
  `common_tab` (löv 0 äger en flik som varje annat löv pekar bakåt på) och
  `instances` (0). Attributens ordning: transform, gemensam flik, gruppflik,
  egen flik, rot.
- `Result` får `expanded_nodes` och `shared_tab_refs`.
- Trädets huvud får `prop_streams` strömmar med rotantal per ström, rötterna
  fördelade i följd, och paketet `LcOaPartitionProps` får lika många strömmar.
- `instances > 0`: efter huvudträdet skrivs en andra toppnod med så många
  barn, alla bakåtpekare till grupper vars barn är löv, valda med fröet.
  Fragmentens seq ändras inte, eftersom de nya förekomsterna kommer sist.
- `prop_streams` utanför 1..rötterna och `shared_tabs` = 1 ger `false`, en
  text och ingen fil.
- `nwdsynth`: `--prop-streams K`, `--shared-tabs M`, `--common-tab`,
  `--instances N`, och raderna i utskriften.

## 6. Mönster att följa

| Vad | Mall |
|---|---|
| LRU med platser och tak | `GeometryCursor` i `src/core/geometry_segment.cpp` (`load`, `unlink`, `touch`, `evict`) |
| Räknare som proven läser | `GeometryCursor::inflated_total()`, prövad i `tests/test_geometry_segment.cpp` |
| Ett tak som parameter med förval | `GeometryReader::open(doc, cache_bytes)` |
| Preorder med explicit stack | `expand()` i `src/scene/tree.cpp` |
| Närmaste partition ovanför | `tools/pose_check.h` |
| Partitionssvans och nästlade partitioner i prov | `tests/test_document.cpp`, `tests/test_variants.cpp` |
| Syntetiska filer genom `Document` | `Opened`, `Spec` och `build` i `tests/test_tree.cpp`; delade delträd i `test_shared_deep` |
| Graf med delade flikar och rötter | `graph()`, `tree_file()` och `check_every_node()` i `tests/test_tab_reader.cpp` |
| Structure mot Full | `same_tabs` i `test_tab_reader.cpp`; `signature` och `read_all` i `test_property_reader.cpp` |

## 7. Risker och kantfall

- **Null och tom sträng** ska bevaras när en flik kopieras.
- **Fel id-rymd**: bara flikar ur trädets ström läggs i cachen.
- **Pekare in i cachen**: en inläggning kan flytta platserna.
- **Inga fel i cachen**: en nod läggs in först när `read()` lyckats, en flik
  först när dess objekt är färdigtolkat.
- **En post större än taket** sparas inte; med taket 0 finns ingen cache.
- **`repeated`** läses av flera läsare men skrivs aldrig efter att trädet är
  publicerat.
- **En lagrad partition på två ställen** ger två modeller med samma svans.
- **Fragment utanför alla modeller** finns i filer med toppnoder av en annan
  klass än 32.
- **MSVC `/W4`**: explicita omvandlingar till `uint32_t`, inga skuggade namn.

## 8. Plattformar

Ingen ny plattformskod. Linux och macOS skrivs utan att ha körts.

## 9. Tester

Varje nytt prov har ett sabotage: en ändring i koden som ska få det att falla.

**`tests/test_tab_reader.cpp`** — en ny graf med en toppnod och 61 löv, där löv
0 äger fliken S och löv 1–60 pekar bakåt på S och har en egen flik:

| Prov | Prövar | Sabotage |
|---|---|---|
| T1 | Stigande med cache 1 MiB: `opens[2] == 0`, `tab_hits == 60`, varje nod lika med Full | Ta bort fångsten i `begin_slot` |
| T2 | Fallande med cache: `opens[2] == 1`, `tab_hits == 59` | Hoppa över `find_tab` |
| T3 | Cache 0: `opens[2] == 60`, `tab_hits == 0`, `cached_bytes() == 0`, lika med Full | Bygg cachen också med taket 0 |
| T4 | `read(…, keep=true)` två gånger: `node_hits == 1`, oförändrat `opens[0]`; med `keep=false` 0 | Strunta i `keep` |
| T5 | Taket 1: inget sparas. Ett tak för tre poster: `cached_bytes()` högst taket efter varje läsning | Ta bort utkastningen |
| T6 | Ett fel i S: löv 0 och sedan löv 1 ger båda fel med samma läsare | Lägg in fliken före den är färdigtolkad |
| T7 | Den befintliga grafen i alla ordningar med cache 1 MiB och 64 byte, lika med Full | Fel nyckel i cachen |
| T8 | Exempelfilerna med cache, lika med Full | — |
| T9 | Stigande: `0 < inflated` högst summan av strömmarnas uppackade längd | Räkna från strömmens början |

**`tests/test_tree.cpp`:**

- `test_nested_models()`: tre nivåer av klass 32 med enhetskoderna 3, 2, 1, en
  grupp på toppnivå och en fjärde modell med koden 0. Prövar `model()` med
  `parent`, `depth`, enhet, källa, inläsare och intervall, `model_of_node` för
  varje nod och `model_of_fragment` för varje fragment, i Full och Structure,
  och att allt är tomt före `load_fragments()`. Sabotage: bara toppnoderna;
  den yttersta modellen i stället för den innersta.
- `check_tree` och `check_models` släpper kravet att en modells förälder är
  roten och att alla intervall ligger i följd: det gäller översta nivån. En
  nästlad modells intervall ligger i förälderns. I exempelfilerna har varje
  modell `parent == kNone` och `depth == 0`.
- `test_shared_properties()`: ett träd med en delad flik, ett upprepat delträd
  och två rötter. Structure med cache 0, 512 och förval, stigande och
  fallande, lika med Full.

**`tests/test_property_reader.cpp`:** exempelfilerna med `open(doc, 0)` och
`open(doc, 4096)`, lika med Full.

**`tests/test_synth.cpp`:**

- Förvalen ger samma fil som förut.
- En fil med fem egenskapsströmmar, delade flikar, en gemensam flik och 200
  upprepade delträd: `PropertyReader` i Structure lika med Full för varje nod.
- Samma fil i core: `inflated` i expanderad ordning högst 1,1 gånger den i
  lagrad ordning plus 1 MiB, `opens[2] == 0`, och `node_hits` lika med
  förekomsterna minus de lagrade noderna.
- Val som inte går ger `false`, en text och ingen fil.

Före commit körs hela sviten från repots rot: `./build/tests/HNwd_tests`.

## 10. Dokumenten

| Fil | Ändring |
|---|---|
| `CLAUDE.md` | raderna för `tab_reader`, `tree.cpp`, `property_reader.cpp` och `nwdsynth` |
| `README.md`, `README.sv.md` | modellerna i flera nivåer; läsarens tak |
| `doc/format.md`, `doc/research/klass32.md` | klass 32 står också under klass 32, med egen svans och enhetskod |
| `doc/CHANGELOG.md` | cachen, kontrollpunkterna, modellerna, de nya flaggorna |
| `TODO.md` | punkten om baklängesläsning |
| `LIMITATIONS.md` | kostnaden för en nod utanför cachen; den nästlade enhetens omräkning är inte belagd |
