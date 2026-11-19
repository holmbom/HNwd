# Filformatet

Det HNwd vet om `.nwd` och `.nwc`. Varje avsnitt säger om uppgiften är **prövad**
av HNwd:s egen kod mot riktiga filer, eller bara **övertagen** från NwcIO:s
formatanteckningar (se [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)).

All data är little-endian.

## Grundtyper (prövat)

- **Sträng:** `[int32 längd][byte][nollor upp till 4 byte]`. Längden `-1` är en
  null-sträng utan byte och utan utfyllnad.
- **PAD8:** före ett 64-bitarsvärde står en `int32 0` om markören ligger på 4
  mod 8, räknat från den uppackade strömmens början.

## Filens yttre form (prövat)

En fil är tre sektioner. Varje sektion börjar med en rubrik: en rad skrivbar
ASCII som slutar på `\n`.

```
FIL       := DATA KATALOG SLUT
DATA      := RUBRIK PAKET{n}                       paketen ligger tätt efter varandra
KATALOG   := RUBRIK NOLLOR ZLIB [8 byte] [int64 offset till KATALOG]
SLUT      := RUBRIK                                filens sista byte är dess '\n'
NOLLOR    := nollor som fyller ut rubriken till en multipel av 8 byte
```

| Sektion | Rubrik |
|---|---|
| DATA, SLUT | `#LcUStream-V1.1 binary double release C lichunk-008` |
| KATALOG | `#LcUStream-V1.1 binary double release utf8 navisworks:448 lizlib` |

| Ord i rubriken | Betydelse |
|---|---|
| `double` | flyttal i poser och matriser är 64 bitar |
| `utf8`, `C` | teckenkodning |
| `navisworks:448` | formatversion |
| `lizlib`, `lichunk-008` | en zlib-ström, eller paket i följd |

- Katalogsektionens rubrik är 65 byte och följs av 7 nollor, så strömmen
  börjar 72 byte in i sektionen. Datasektionens rubrik är 52 byte och följs
  direkt av det första paketet, utan utfyllnad.
- De 8 byten närmast efter katalogströmmen är inte avkodade.
- `int64`-värdet före slutrubriken är katalogsektionens offset i filen. Det är
  vägen till katalogen utan att läsa resten av filen.

## Katalogen (prövat)

Katalogsektionens enda ström. Den listar varje paket i datasektionen.

```
DIRECTORY := [int32 -1][int32 n] ENTRY{n}          slutar exakt vid strömmens slut
ENTRY     := [str name][int32 flags] PAD8 [int64 offset][int64 length][int32 a][int32 b]
```

- `offset` är absolut i filen och `length` antalet byte i filen.
- **Paketen täcker datasektionen exakt:** det första börjar direkt efter
  datasektionens rubrik, varje paket börjar där det förra slutar, och det sista
  slutar där katalogsektionens rubrik börjar.
- `name` matchas på texten efter sista `\`. En `.nwc` har prefixet `cache\`
  eller inget; en `.nwd` har modellens namn som prefix, eller inget.
- `flags`: `1` zlib-strömmar, `3` råa byte.
- `(a, b)`:
  - `(0, 0)`: paketet är en zlib-ström på hela längden.
  - `(4, b)`: 4 byte prefix, sedan zlib-strömmar tätt efter varandra. Den sista
    är `b` byte lång och är en innehållsförteckning över de övriga.

Paket som förekommer. De små paketen, från `LcOpNwdStats` och nedåt, står med
grammatik och belägg i [research/element.md](research/element.md); deras
tolkar i [src/core/elements.h](../src/core/elements.h), och
`nwddump <fil> --elements` skriver vad de innehåller.

| Paket | Form | Innehåll | Avkodas |
|---|---|---|---|
| `LcOpNwdGeometry` | (4, b), prefix `int32 65536` | geometriposter i chunkar | ja, utom kvantiserade positioner |
| `LcOaPartitionProps` | (4, b), prefix `int32 0` | egenskaper som en nod använder ensam | ja |
| `LcOpNwdLogicalHierarchy` | (0, 0) | objektträdet | ja |
| `LcOpNwdFragments` | (0, 0) | placering av varje geometribit | ja |
| `LcOpGuidStore` | (0, 0) | GUID per element | ja |
| `LcOpNwdPartition` | (0, 0) | filens egen partition, ett objekt av klass 32 | ja |
| `LcOpNwdSpatialHierarchy` | (0, 0) | ett rumsligt träd över fragmenten | ja |
| `LcOpDBCache` | (0, 0) | bara i `.nwc`: källhuvudet | fram till inläsarens namn; exportinställningarna efter det inte |
| `LcOpNwdStats` | (0, 0) | inläsarens logg | ja |
| `LcOpOdyScenePropsChunk`, `LcOpOdyFileInfoChunk` | (0, 0) | filens sökväg, prefix och GUID:er | grammatiken; flera GUID:ers roll är okänd |
| `LcOpXRefTable` | (0, 0) | externa referenser som JSON | ja, men en tabell med poster ger `Unsupported` |
| `LcOpCurrentViewElement`, `LcOpHomeViewElement` | (0, 0) | en kamera med snittplan | formerna 9, 843, 895 och 1919 |
| `LcOpSavedViewsElement` | (0, 0) | sparade vyer i mappar, med markeringar | ja |
| `LcOpGridElement` | (0, 0) | rutnät med linjer och nivåer | ja |
| `LcOpNwdSharedNodes` | (0, 0) | typsnitt | ja |
| `LcReMaterialElement` | (0, 0) | material som JSON | ja |
| `LcOpLightsElement`, `LcOpBackgroundElement` | (0, 0) | ljus; bakgrundens färger och himmel | ja |
| `LcOpClashElement` | (0, 0) | kollisionstester | grammatiken; bara i `nwddump` |
| `LcOpHeadlightElement`, `LcOpSpeedElement`, `LcOpCullingElement`, `LcOpToolElement`, `LcOpNextCommentIdElement`, `LcOpShadOverridesElement`, `LcOpTextureSpaceElement` | (0, 0) | visningsinställningar | grammatiken, betydelsen okänd; bara i `nwddump` |
| `LcOpCommonSchemas`, `LcOpZoneElement`, `LcOpSelectionSetsElement`, `LcOpPropertyOverrideElement`, `LcOdpDBDatabaseLinksElement` | (0, 0) | okänt | bara den tomma formen `[0][0]`; annat ger `Unsupported` |
| `LcOpNwdGeometryCompress`, `LcOpNwdSerial` | råa, 40 byte | okänt | nej |

## Zlib-strömmarna (prövat)

Varje ström är RFC 1950 runt RFC 1951, med Adler-32 sist. Strömmarnas
komprimerade längd stämmer exakt med katalogen: en ström i formen `(0, 0)` är
lika lång som paketet, och den sista i formen `(4, b)` är `b` byte.

## Paket med flera strömmar (prövat)

Formen `(4, b)`: `[prefix 4 byte][ström 0] … [ström k-1][innehållsförteckning, b byte]`.
Innehållsförteckningen ger de övriga strömmarnas komprimerade längder, och
`4 + summan + b` är paketets längd.

- `LcOpNwdGeometry`: `[int32 N][int32 ch][ch × int32 komprimerad chunkstorlek][N × int32 postlängd]`,
  nollor upp till 8; exakt `roundup8(8 + 4·ch + 4·N)` byte.
- `LcOaPartitionProps`: `[int32 k][k × int32 komprimerad längd]`, nollor upp
  till 8. I exempelfilerna är k = 1.

## Objektströmmar (prövat)

Trädet, fragmenten, egenskaperna och partitionen är nät av objekt. En
pekarplats är `[0]` (null), `[1][id]` (bakåtpekare till ett färdigläst objekt)
eller `[nästa id][klass]` följt av kroppen. Id räknas från 100 per ström, och
ett objekt får sitt id innan kroppen läses.

## GUID-tabellen (prövat)

`LcOpGuidStore`: `[int32 1][int32 n][n × 16 byte]`, exakt `8 + 16n` byte. Varje
GUID ligger i Windows-ordning: de tre första fälten little-endian.

## Källhuvudet i en `.nwc` (prövat)

`LcOpDBCache`: `[int64 7][str källsökväg][int32 -1] PAD8 [int64 unixtid][int64 källstorlek][str loader]`,
följt av inställningar för exporten som inte är avkodade. En `.nwd` saknar
paketet.

## Geometrin (prövat)

Chunkarna i `LcOpNwdGeometry`, uppackade och lagda efter varandra, är en följd
av poster med längderna ur innehållsförteckningen. Summan av postlängderna är
lika med den uppackade datan. En post kan gå över en chunkgräns. Varje chunk
utom den sista packas upp till exakt 65 536 byte, samma tal som paketets
prefix, så chunk i börjar på i · 65 536 och en post nås utan att chunkarna
före den packas upp. Det håller i alla exempelfilerna och i paket med
tiotusentals chunkar; HNwd prövar varje uppackning mot regeln, och
`Options::measure_chunks` mäter i stället varje chunk.

Postens grammatik står i [src/core/geometry.h](../src/core/geometry.h). Varje
post i exempelfilerna tolkas och slutar exakt på sin längd.

- **Klasser:** 94 trianglar, 95 linjer, 96 punkter. En triangelpost har alltid
  normaler, och färgen per vertex får finnas. Linjer och punkter har aldrig UV
  eller normaler, och **färgen får saknas**: ingen linje i exempelfilerna har
  den.
- **Punkter (klass 96)** har ingen längdlista: varje render-vertex är en punkt.
  Efter platserna står ett tal, 0 eller 1, vars betydelse inte är känd, och
  sedan är posten slut.
- **Cirkel (klass 104)** och **cylindermantel (klass 105)** är former, inte
  listor av positioner. Varje vektor är tre f32:

  ```
  [100][104][int 3][o][a][b][f32 r][int 0]            56 byte
  [100][105][int 0x30003][o][p][a][b][f32 r]          64 byte
  ```

  `o` är nollvektorn. `a` och `b` är vinkelräta enhetsvektorer som spänner
  cirkelns plan och `r` är radien: cirkeln är `r·(cos v · a + sin v · b)`
  kring origo. Cylinderns axel `p` går från bottencirkelns mitt till
  toppcirkelns och är parallell med `a × b`; manteln har inga ändytor.
  Fragmentets antal är 32 segment för en cirkel och 64 trianglar för en
  mantel, alltså en delning i 32 steg. Fragmentets låda är per axel
  `min(0, p) − e .. max(0, p) + e` med `e = r·(|a| + |b|)`: lådan kring den
  kvadrat som omsluter cirkeln, inte kring cirkeln själv. Den stämmer med den
  uträknade på en avrundningsenhet när, inte alltid på biten, så HNwd jämför
  de här lådorna med ett spelrum på fyra enheter i sista siffran.
- **Text (klass 103)** är `[100][103][sträng][f32 1][f32 1][f32 v][n][min]
  [max][int32 typsnitt]` och utfyllnad till 8. `v` är en vridning i radianer
  kring enhetsvektorn `n`, `min` och `max` är textens låda, lika med
  fragmentets, och typsnittet är ett index från 0. Glyferna står inte i
  posten: fragmentets antal primitiver går inte att räkna fram ur den.
- **Normalernas bredd** står inte i filen. Den är den bredd posterna går att
  tolka med: 16 bitar i varje `.nwc` och 8 i `.nwd` bland exempelfilerna.
  Posterna prövas framifrån, och den första som bara går att tolka med den
  ena bredden avgör; en post som inte går att tolka med någon av dem hoppas
  över i avgörandet.
- **Kvantiserade UV:** en bitbredd på 0 är en koordinat som är densamma i alla
  par och tar inga bitar.
- **Inte i exempelfilerna**, och därför bara prövat i sviten mot syntetiska
  poster: UV-platser, färgplatser, punkter, cirklar, cylindermantlar och text.
  Kvantiserade positioner avkodas inte.

## Placeringen (prövat)

`LcOpNwdFragments` är en objektström med ett fragment per geometribit;
klassernas kroppar står i [src/core/fragments.h](../src/core/fragments.h).

- Världskoordinat = `s · R(q) · v + t`, där `R(q)` är den vanliga
  rotationsmatrisen för kvaternionen `(x, y, z, w)`. Riktningen är belagd:
  en rätvinklig modell med roterade delar får sina jämna mått i världen bara
  så, inte med den omvända rotationen.
- Postens positioner är lagrade kring en mittpunkt, `c` i geometrireferensen
  (klass 93), så postens låda ligger runt origo. Posens flytt bär redan
  mittpunkten: för poser med bara flytt, under noder med bara flytt, är posens
  flytt lika med nodernas flytt plus `c` i vart och ett av tjugoåtta tusen
  prövade fragment.
- **Posen får vara null.** Det är fragmenten vars noder saknar flytt i trädet,
  och de står i mittpunkten: `t = c`, utan rotation och skala.
- Poseklass 16, med skala, förekommer bara i `.nwd` bland exempelfilerna, och
  skalan är där omkring 1000.
- **Poseklass 17** är en full matris, för en placering med olika skala per
  axel: `[3 × (3 × f32, f32 0)][f32 1] PAD8 [3 × f64 t]`, och världskoordinat
  = `M · v + t`. De tolv talen läses som matrisens tre kolumner, som en
  4 × 4-matris kolumnvis med flytten utbruten i dubbel precision. Att det är
  kolumner är belagt mot trädet: se sambandet nedan, som bara går ihop med
  den läsningen.
- **Posen är trädets transformer i följd.** För varje fragment är
  `pose = T(toppnod) · … · T(löv) · flytt(c)`, där `T` är nodens transform
  (klass 40, 41 eller 39) längs vägen från roten till fragmentets löv och `c`
  geometrins mitt ur klass 93. Det håller för varje fragment i exempelfilerna
  (`nwdcheck --poses`). Matrisen i klass 39 står kolumnvis, liksom
  poseklass 17; med rader går sambandet inte ihop i en fil som har
  osymmetriska matriser. Enheten ligger redan i trädet: en modell vars enhet
  skiljer sig från dokumentets har på sin partition en klass 39 med
  enhetskvoten gånger enhetsmatrisen, i exempelfilen 1000 för de två
  modellerna i meter. En partition kan också bära en egen placering.
- **Utseendets flaggor** (klass 55): 1, 16, 32 och 64 förekommer. Med 32
  följer en pekare till klass 185, som får vara null; med 64 följer ett tal,
  0 eller 1. 16 har inget eget fält. Vad flaggorna betyder är inte känt.
- Klass 185 är materialet: `[str json][str][int32 n]` och sedan n filer,
  `[str namn][int32 inbäddad][str]`, följda av `[str typ][int64 längd][byte,
  utfyllt till 4]` när `inbäddad` är 1. Byten är en bildfil som den är (JPEG
  eller PNG) och typen dess filändelse. `json` är ett träd av tillgångar,
  `{"version":2,"userassets":["0"],"materials":{"0":{…},"1":{…}}}`, där varje
  tillgång har `definition` och sina sektioner i `properties`. Tillgången med
  definitionen `Generic` har grundfärgen i `colors.generic_diffuse.values`,
  och dess `connections` pekar ut en tillgång med definitionen
  `UnifiedBitmap`, vars `uris.unifiedbitmap_Bitmap.values` är namnet på en av
  filerna. Samma tillgång har bildens skala, förskjutning, vridning och
  upprepning (`texture_UScale`, `texture_RealWorldScaleX` med `units`,
  `texture_URepeat` och deras syskon). Inte i exempelfilerna; prövat mot
  syntetiska strömmar.
- `seq` avtar aldrig längs strömmen, och `rec` ligger i `1 .. N`.
- Utseende, material och geometrireferens delas mellan fragment genom
  bakåtpekare.

## Objektträdet (prövat)

`LcOpNwdLogicalHierarchy`; klassernas kroppar och var de får stå står i
[src/core/hierarchy.h](../src/core/hierarchy.h).

- Trädet i en `.nwc` har en toppnod av klass 53.
- Den n-te tomma attributplatsen i trädet tar den n-te roten ur
  `LcOaPartitionProps`, och varje rot förbrukas. I exempelfilerna är rötterna
  flikar (klass 86) i en enda ström; ett paket kan ha hundratals strömmar.
- **En nods namnpar får vara null.** Flikar och nycklar har alltid ett.
- Trädet är en riktad acyklisk graf: delträd, flikar, material och namnpar
  delas genom bakåtpekare.
- **Attributen har ett gemensamt huvud:** `[str namn][ptr 52 eller null][int32
  flags]`, som en flik börjar. Materialet (klass 31) är huvudet och 14 f64. I
  exempelfilerna är materialets namnpar alltid null och flaggorna 0, och de
  åtta byten ser då ut som ett femtonde flyttal med värdet noll.
- **Id och text som egna klasser**, som rot i egenskapsströmmarna eller direkt
  som attribut på en nod:
  - 66: ett element-id, `[str null][ptr 52 nyckel][int32 flags] PAD8 [int64]`.
  - 70: ett id som text, `[str värde][ptr 52 nyckel][int32 flags]`.
  - 76: en text under en nyckel, `[str null][ptr 52 nyckel][int32 0][str värde]`.
  - Flaggorna i 66 och 70 varierar (0, 0x00020000, 0x01000000) utan att
    layouten ändras.
- **Klass 92** är huvudet, talet 96 och 48 byte. Innehållet är inte känt, och
  bara den storleken avkodas. I formatversion 451 har klass 92 också storleken
  96, och de 48 byten är nollor utom ett ord på byte 16 som har storleken av
  en unixtid i sekunder; att det är en tid är inte prövat.
- **Inte i exempelfilerna**, och därför bara prövat i sviten mot syntetiska
  strömmar: klasserna 79, 200, 185, 66, 70, 76 och 92, och ett material
  med namnpar. Klass 39 finns i `.nwd`, på de två partitionerna i meter.
  Klasserna 79 (i egenskapsströmmarna), 200, 66, 70, 76 och 92 förekommer i
  formatversion 451, och trädet med sina egenskapsströmmar tolkas där till
  exakt slut; flaggorna i 66 och 70 har där samma tre värden.
- **Typsnitten.** `LcOpNwdSharedNodes` har en lista av typsnitt (klass 102:
  namn och storlek). Att en texts typsnittsindex (geometrins klass 103) pekar
  in i den listan är inte belagt: ingen exempelfil har både text och
  typsnitt. I formatversion 451 ligger varje sådant index inom listan.
  Indexet prövas inte mot listan.
- Alla strängar i exempelfilerna är giltig UTF-8.

## Flera modeller i en fil: klass 32 (prövat)

I en `.nwd` är trädets toppnoder av klass 32, en per inläst modell, och under
var och en sitter modellens eget träd. Paketet `LcOpNwdPartition`, som finns i
varje fil, är ett fristående objekt av samma klass för filen själv.
`PTR(c)` är en pekare till ett objekt av klass c.

```
PARTITION_STRÖM := [100][32] HUVUD SVANS                         ingen underförstådd rot
TOPPNOD_I_TRÄD  := [nästa id][32] HUVUD [int32 c] c × BARN SVANS
HUVUD  := [int32 0][str namn] PTR(52) [int32 flags][int32 X][int32 Y] Y × ATTR
SVANS  := [int32 2][int32 n] n × PTR(27)
          [3 × f64][int32 0][str loader]
          [int32 format][int32 läge][int32 1]
          [3 × f64][3 × f64]
          [int32 0 eller 1][str text eller null][int32 1][str källsökväg]
          [3 × f64][4 × f64]
          [int32 1] nollor upp till 8
27     := [int32 1] PTR(185 eller null) [int32 0][3 × f64][6 × f64][3 × f64]
```

- I trädet är attributen som i klass 53, och en tom plats tar nästa
  egenskapsrot. Nästa toppnod börjar direkt efter svansen, och objekten i
  svansen tar id som alla andra.
- Klass 27 är ett ljus, och dess klass 185 beskriver det som JSON. Bara filens
  egen partition och trädets första har ljus i exempelfilerna.
- **Partitioner får sitta under partitioner:** barnen till en nod av klass 32
  är av klass 22, 53 eller 32. En fil kan vara sammansatt av filer som själva
  är sammansatta.
- `format` är 448 i exempelfilerna; 451 förekommer också. `loader` och
  `källsökväg` säger vilken inläsare som gjorde modellen och ur vilken fil.
  Texten före källsökvägen får saknas; dess betydelse är inte känd.
- `läge` är längdenheten: 0 är meter, 1 centimeter, 2 millimeter och 3 fot.
  Modellerna i exempelfilerna har 2 eller 0, och i `.nwd`, vars egen partition
  har 2, bär just fragmenten ur modellerna med 0 skalan 1000 i sin pose. I en
  fil med 3 är posernas skalor omräkningar till fot: 0,00328084, 0,0328084,
  1/12, 1 och 3,28084. Koden 1 förekommer i formatversion 451, också på
  partitioner under partitioner: klass 39 på en sådan partition har skalan 10
  under en partition med 2, och under en med 3 kvoten 0,01 / 0,3048 gånger
  en matris som också andra partitioner där har (förenligt med en vridning).
  Samma kvot står i modellerna i `LcOpClashElement`
  ([research/element.md](research/element.md)). Andra koder är inte sedda och
  ger ingen enhet.
- Betydelsen av flyttalen och av de fasta talen är inte känd. De fasta talen
  ska ha just de värdena, annars är strömmen fel.
- Belägg: en och samma grammatik tolkar partitionsströmmen i alla tio
  exempelfilerna till exakt slut. I `.nwd`-trädet börjar varje toppnod direkt
  efter den föregåendes svans, strömmen slutar exakt, alla egenskapsrötter
  förbrukas, och varje delträd är nod för nod lika med modellens egen `.nwc`.

Mer i [research/klass32.md](research/klass32.md).

## Det rumsliga trädet (prövat)

`LcOpNwdSpatialHierarchy` är ett träd över fragmenten, utan lådor:

```
NOD := [int32 5][int32 k] k × NOD                 inre nod
     | [int32 2][int32 n] n × ([int32 1][int32 f])    löv; f är fragmentets index, från 0
     | [int32 4][int32 f]                         löv med ett enda fragment
```

Strömmen är en enda nod och slutar exakt efter den. Varje fragment står i
exakt ett löv. Löven är rumsligt samlade: fragmenten i ett löv ligger nära
varandra, tydligast i filer med många löv. En inre nod har 2–7 barn i
exempelfilerna, oftast 7. Formen med tagg 4 finns inte i exempelfilerna och
är prövad mot syntetiska strömmar.

## Det som inte är prövat (övertaget)

Paketen som är märkta "nej" i tabellen ovan, och de fält i de små paketen som
[research/element.md](research/element.md) kallar okända. Underlaget står i
[plans/HNwd-plan.md](plans/HNwd-plan.md) och
[plans/2026-11-17_LASAREN.md](plans/2026-11-17_LASAREN.md).
