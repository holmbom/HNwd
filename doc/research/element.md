# De små paketen

Formatfakta om paketen för visningen, filens identitet och inläsarens logg, så
som exempelfilerna visar dem. Tolkarna står i
[src/core/elements.h](../../src/core/elements.h), testerna i
[tests/test_elements.cpp](../../tests/test_elements.cpp), och
`nwddump <fil> --elements` skriver vad varje paket innehåller. Allt under
"Belagt" prövas av sviten mot de tio exempelfilerna (nio `.nwc` och
`Navisworks.nwd`); det som står "okänt" är inte belagt.

Det som står "i formatversion 451" gäller den versionen (talet i partitionens
svans), som ingen av exempelfilerna har. Sviten prövar de grammatikerna mot
syntetiska strömmar.

Varje paket är en enda zlib-ström (formen (0, 0)) utom `LcOpNwdSerial` och
`LcOpNwdGeometryCompress`, som är råa byte. Strängar, PAD8 och 64-bitarsvärden
är som i [format.md](../format.md). Varje ström slutar med nollor upp till en
multipel av 8 och sedan inget mer.

## Var paketen finns

Storleken är den uppackade strömmens, i byte.

| Paket | 01 | 02 | 03 | 04 | 05 | 06 | 07 | 08 | 09 | `.nwd` |
|---|---|---|---|---|---|---|---|---|---|---|
| `LcOpNwdGeometryCompress` (rå) | 40 | 40 | 40 | 40 | 40 | 40 | 40 | 40 | 40 | 40 |
| `LcOpNwdSerial` (rå) | 40 | 40 | 40 | 40 | 40 | 40 | 40 | 40 | 40 | 40 |
| `LcOpNwdSharedNodes` | 8 | 8 | 8 | 8 | 8 | 8 | 8 | 8 | 8 | 8 |
| `LcOpCommonSchemas` | 8 | 8 | 8 | 8 | 8 | 8 | 8 | 8 | 8 | 8 |
| `LcOpNwdStats` | 4952 | 2664 | 4480 | 2168 | 2136 | 2272 | 2208 | 1864 | 1904 | 24936 |
| `LcOpOdyScenePropsChunk` | 120 | 120 | 120 | 128 | 136 | 128 | 136 | 128 | 136 | 112 |
| `LcOpOdyFileInfoChunk` | 192 | 192 | 192 | 200 | 208 | 200 | 208 | 200 | 208 | 232 |
| `LcOpXRefTable` | 160 | 160 | 160 | 176 | 176 | 176 | 176 | 168 | 176 | 152 |
| `LcOpCurrentViewElement` | 808 | 808 | 808 | 808 | 808 | 808 | 808 | 808 | 808 | 952 |
| `LcOpHeadlightElement` | – | – | – | – | – | – | – | – | – | 16 |
| `LcOpSpeedElement` | – | – | – | – | – | – | – | – | – | 8 |
| `LcOpCullingElement` | – | – | – | – | – | – | – | – | – | 48 |
| `LcOpBackgroundElement` | – | – | – | – | – | – | – | – | – | 184 |
| `LcOpSavedViewsElement` | – | – | – | – | – | – | – | – | – | 8 |
| `LcOpLightsElement` | – | – | – | – | – | – | – | – | – | 7496 |
| `LcOpZoneElement` | – | – | – | – | – | – | – | – | – | 8 |

- I en `.nwc` har paketen prefixet `cache\`. I `.nwd` har de den första
  modellens namn som prefix (`01_….ifc\`), utom `LcOpOdyFileInfoChunk`,
  `LcOpCommonSchemas` och `LcOpXRefTable`, som saknar prefix i alla tio.
- `.nwd` har en uppsättning av paketen, inte en per modell.
- Inte i någon exempelfil, men i formatversion 451: `LcOpHomeViewElement`,
  `LcOpGridElement`, `LcOpSelectionSetsElement`, `LcOpClashElement`,
  `LcOpToolElement`, `LcOpTextureSpaceElement`, `LcOpShadOverridesElement`,
  `LcOpPropertyOverrideElement`, `LcOpNextCommentIdElement`,
  `LcReMaterialElement`, `LcOdpDBDatabaseLinksElement`. Alla har en tolk.
- I formatversion 451 är `LcOpNwdSharedNodes`, `LcOpSavedViewsElement`,
  `LcOpBackgroundElement` och `LcOpCurrentViewElement` av andra former än i
  exemplen (se respektive avsnitt), och `LcOpNwdSerial` finns inte.

## De tomma paketen

`LcOpCommonSchemas`, `LcOpZoneElement`, `LcOpSelectionSetsElement`,
`LcOpPropertyOverrideElement` och `LcOdpDBDatabaseLinksElement`:

```
[i32 0] [i32 0]
```

Belagt: åtta nollbyte i varje fil som har paketet, också i formatversion 451.
Okänt: formen när paketet inte är tomt, och vilket av talen som är ett antal.
Tolken ger `Unsupported` om något av talen inte är 0.

`LcOpNwdSharedNodes` och `LcOpSavedViewsElement` har samma åtta nollbyte i
exemplen, men har innehåll i formatversion 451. Där är det första talet ett
antal och resten utfyllnad (se nedan), så de tomma formerna är `[i32 0]` och
nollor upp till 8.

## `LcOpToolElement` och `LcOpNextCommentIdElement`

Bara i formatversion 451.

```
Tool          := [i32 tool] nollor upp till 8
NextCommentId := [i32 a] [i32 0] [i32 b] [i32 0]
```

Belagt är bara att grammatiken tolkar paketet till exakt slut. I
NextCommentId går det inte att skilja två heltal med nollor efter från två
64-bitarsvärden; tolken läser heltal och ger `Unsupported` om en nolla inte är
0. Betydelserna är okända.

## `LcOpNwdStats`: inläsarens logg

```
[str text] nollor upp till 8
```

- Belagt: grammatiken tolkar alla tio till exakt slut. Texten är giltig UTF-8.
  I en `.nwc` är den inläsarens logg över källfilen.
- Belagt: i `.nwd` är texten de nio modellernas loggar efter varandra, i
  modellernas ordning, var och en inledd av `"\n" <modellens namn> "\n\n"`, där
  namnet är modellens partitionsnamn (`….ifc`). Texten är byte för byte
  sammanfogningen av de nio `.nwc`-filernas.
- Okänt: om texten har en form när inläsaren är en annan.

## `LcOpOdyScenePropsChunk`

```
[str sökväg] [16 byte GUID] [i32 0] [i32 0] nollor upp till 8
```

- Belagt: sökvägens sista led (efter sista `\`) är filens eget namn i alla
  tio, och sökvägen är samma sträng som `SavedFilename` i `LcOpXRefTable`.
- Belagt: GUID:en är en RFC 4122-GUID: version 5 (namnbaserad) i varje
  `.nwc`, version 4 (slump) i `.nwd`. I en `.nwc` är den samma som
  `scene_guid` i `LcOpOdyFileInfoChunk`; i `.nwd` är den inte det.
- Okänt: de två nollorna (tolken kräver 0, `Unsupported` annars); om de är
  två heltal eller ett 64-bitarsvärde efter PAD8 går inte att avgöra, eftersom
  byten är lika.

## `LcOpOdyFileInfoChunk`

```
[str null] [i32 0] [GUID file_guid] [i32 0] [str tag] [str name]
[i32 0] [GUID scene_guid] [i32 0] [str prefix]
[i32 0] [i32 0] [i32 -1] [i32 -1] [str prefix_third]
[GUID scene_guid igen] [str prefix igen]
[i32 0] [GUID guid_c] [GUID guid_d]
nollor upp till 8
```

GUID:erna ligger inte på jämn 8 byte: samma fält står på 0 mod 8 i en fil och
på 4 mod 8 i en annan, så nollorna före dem är egna fält, inte PAD8.

- Belagt: grammatiken tolkar alla tio till exakt slut.
- `prefix`: prefixet på paketnamnen i katalogen, utan bakstreck — `cache` i
  varje `.nwc`, den första modellens namn i `.nwd`. Den andra förekomsten är
  lika med den första i alla tio.
- `name`: i en `.nwc` filens eget namn; i `.nwd` samma som `prefix`.
- `scene_guid`: se ScenePropsChunk. Den andra förekomsten är lika med den
  första i alla tio.
- `file_guid`, `guid_c`: i en `.nwc` är de lika; i `.nwd` är de olika.
- `guid_d`: noll-GUID i varje `.nwc`, en GUID av version 4 i `.nwd`.
- Alla GUID:er som inte är noll är RFC 4122-GUID:er av version 4 eller 5.
- `tag`: strängen `G` i alla tio och i formatversion 451. Okänt.
- `prefix_third`: null (längden −1) i alla tio, så den lästes tidigare som en
  tredje markör −1. I formatversion 451 är den en sträng lika med `prefix`.
- Belagt i formatversion 451: grammatiken tolkar paketet till exakt slut;
  `name` är lika med `prefix`, som i `.nwd`.
- Okänt: vad GUID:erna identifierar (version 5 tyder på att de räknats ur ett
  namn, men vilket är inte känt), och markörerna. Tolken kräver markörerna och
  en null-sträng först, och ger `Unsupported` annars.

## `LcOpXRefTable`

```
[str json] nollor upp till 8
json := {"Type":"XRefTable","Version":1,"SavedFilename":"<sökväg>","Table":[]}
```

- Belagt: samma fyra nycklar i alla tio, `Table` alltid tom. `SavedFilename`
  är sökvägen till filen själv (se ScenePropsChunk).
- Okänt: postformen i `Table`. Tolken ger `Unsupported` för en tabell med
  poster, en okänd nyckel, en nyckel som saknas, en annan `Type` eller en
  annan `Version`.

## `LcOpCurrentViewElement` och `LcOpHomeViewElement`: kameran

```
[i32 form] [i32 projection]
[3 × f64 position] [4 × f64 rotation] [f64 u] [f64 angle]
[f64 near] [f64 far] [2 × f64 0]
form 843, 895, 1919:  [4 × f64] [str avatar] [4 × f64] [i32] PAD8 [4 × f64]
form 9, 843:          [f64 distance]
form 895, 1919:       [6 × f64 extra]
form 9:               [i32 0] [i32 0]
form 843:             [i32 0] [i32 2] [i32 2] [i32 0] [i32 0] [i32 0]
form 895:             [6 × i32]
form 1919:            [4 × i32] [f64] [2 × i32]
SNITT

SNITT := [6 × i32 head] [3 × f64 head_values] [i32 6] 6 × PLAN
         [2 × i32 flags_a] [6 × f64 values_a] [2 × i32 flags_b] [6 × f64 values_b]
         [4 × f64 rotation]                                        680 byte
PLAN  := [i32 enabled] [i32 mode] [3 × f64 point] [3 × f64 normal] [3 × f64 tangent]
```

`form` är 9 i varje `.nwc`, 843 i `.nwd`, och 895 i `LcOpCurrentViewElement`
i formatversion 451. `LcOpHomeViewElement` (bara i formatversion 451) har
form 9 med läget i origo och identitetsrotationen, och samma 680 sista byte
som exemplen. Samma vy, från `form`, står i varje sparad vy (se
`LcOpSavedViewsElement`), där formerna 895 och 1919 förekommer.

Bitvis innehåller varje form den förra: 9 ⊂ 843 ⊂ 895 ⊂ 1919. Det tyder på
att formen är en mask över vilka fält som finns, men med fyra former går det
inte att säga vilken bit som lägger till vilket fält. 895 skiljer sig från 843
genom fem f64 till (sex f64 där 843 har `distance`) och 1919 från 895 genom en
f64 mellan det fjärde och femte heltalet. Tolken tar bara de fyra formerna
och ger `Unsupported` för en annan.

`projection` lästes tidigare som utfyllnad efter formen. I en sparad vy, där
formen kan stå på 4 mod 8, syns att det är ett eget fält: efter det kommer
PAD8 när det behövs. Det är 0 i form 9, 843 och 895 och 1 i form 1919, i
varje vy. I form 1919 är `angle` ett mått i filens längdenhet, många gånger
större än π, i stället för en vinkel under π/2 som i form 895, vilket
tyder på att 1 är en ortografisk vy. Det är inte prövat mot något annat. Ett
annat värde än 0 och 1 ger `Unsupported`.

Belagt:
- `rotation` är en kvaternion (x, y, z, w) med längden 1 (± 1e-9) i alla tio.
  I varje `.nwc` är den identiteten.
- Kameran tittar längs `R(rotation) · (0, 0, −1)`, med `R` som i fragmentens
  poser. I varje `.nwc` är `position + distance · blickriktningen` mitten av
  fragmentens världslåda (± 1e-9 av diagonalen); `position` ligger över lådan.
- I `.nwd` ligger varje vertex i modellen mellan `near` och `far` längs samma
  blickriktning: geometrins djup är 47290–93657 och `near`, `far` är 45987 och
  94512. I varje `.nwc` är de 1 och 10.
- `distance` i `.nwd` är samma tal som i modellen 01:s `.nwc`; blickpunkten
  ligger inte i lådans mitt där.
- SNITT är lika i alla tio.

Belagt i formatversion 451, i den aktuella vyn och i varje sparad vy:
- `rotation` har längden 1 (± 1e-9);
- `zero` är (0, 0);
- de fyra måtten först i avatarblocket är samma mått som i exemplet `.nwd`,
  omräknade till filens längdenhet (millimeter där). Blocket är alltså i
  längdenheten;
- i form 895 och 1919 är `extra[0..2]` (0, 0, 1) och `extra[5]` π/4 med tio
  decimaler; `extra[3]` och `extra[4]` varierar. Heltalen efter är desamma i
  varje vy;
- SNITT tolkas av grammatiken ovan i varje vy; se nedan.

Okänt:
- `u`: 1 i `.nwc`, 1,4496 i `.nwd`.
- `angle`: 0,785398 (π/4 avrundat till sex decimaler) i alla tio. Att det är
  synfältet är inte belagt.
- Blocket i form 843: talen 300, 1800, 1800, 150; strängen
  `construction_worker`; 0, 0, 3000, 1; heltalet 1; 0, 9800, 50000, 0. Bara
  ett exempel, så inget fält är belagt.
- Heltalen efter `distance`. Att de i form 843 skulle vara `[0][n]` och
  n × 8 byte passar båda formerna men är inte prövat.
- Heltalen efter `distance` eller `extra` och avatarblocket är desamma i varje
  vy i formatversion 451, också `u`-värdena upprepas mellan vyer; att `u` är
  bildens sidförhållande ligger nära till hands men är inte prövat.

Tolken ger `Unsupported` för en annan `form` och för mer efter SNITT än
utfyllnaden.

### SNITT: snittplanen

SNITT börjar alltid på jämn 8 byte, eftersom fälten före det slutar så. Det
första planet har därför PAD8 efter `mode` och tar 84 byte, de andra 80.

Belagt i formatversion 451, i den aktuella vyn och i varje sparad vy, där
SNITT förekommer i några olika former:
- grammatiken tolkar de 680 byten; talet 6 och `enabled` (0 eller 1) har
  inget annat värde;
- `normal` och `tangent` har längden 1 och är vinkelräta i varje plan;
- i två påslagna plan med motsatta normaler är skillnaden mellan punkterna
  parallell med normalen (sinus för vinkeln under 1e-10): `point` är en punkt
  i planet och `normal` dess normal;
- ett påslaget plan har `mode` 10; ett avslaget har sitt eget värde, 6, 7, 3,
  2, 4, 5 i planens ordning, samma som i exemplen;
- plan 0 och 1 slås på tillsammans, och plan 2 och 3 tillsammans med dem; i
  ett par är normalerna motsatta;
- `flags_a` är (1, 1) och `flags_b` (0, 1) precis i de vyer där något plan
  är påslaget, annars (0, 0) och (0, 0);
- `rotation` är en enhetskvaternion, och i varje påslaget plan är `normal`
  och `tangent` ±R(rotation)·x eller ±R(rotation)·y. Hemvyn har identiteten;
- `head` (2, 0, 0, 0, 2, 2), `head_values` (1, 0, 0) och `values_a`,
  `values_b` (1, 1, 1, 0, 0, 0) är lika i alla vyer, i exemplen och i
  formatversion 451.

I exemplen är inget plan påslaget, varje plan har punkten i origo, normalen
(0, 1, 0) och tangenten (0, −0, 1), och `rotation` är identiteten.

Okänt: `mode`, flaggorna, `head`, `values_a` och `values_b` (att de är en
skala och en flytt för en snittlåda ligger nära men är inte prövat), och vad
`tangent` styr.

## `LcOpHeadlightElement`, `LcOpSpeedElement`, `LcOpCullingElement`

Bara i `.nwd`, ett exempel var.

```
Headlight := [f64] [f64]                          0,3 och 1
Speed     := [f64]                                1/6
Culling   := [f64] [4 × 8 byte] [i32] nollor upp till 8     1; nollor; 1
```

Belagt är bara storleken och att grammatiken tolkar paketet till exakt slut.
I Culling är de fyra värdena nollor; om de är f64 eller heltal går inte att
avgöra. Betydelserna är okända.

## `LcOpBackgroundElement`

```
[i32 mode] PAD8 [7 × (3 × f64)] [i32 0] PTR(185 eller null) nollor upp till 8
```

Pekaren är en objektström för sig, med id från 100.

- Belagt: de sju trippeln är färger — varje komponent ligger i [0, 1] och är
  ett helt antal 255-delar, i exemplet `.nwd` och i formatversion 451. I
  exemplet: (0, 0, 0), (105, 140, 230), (210, 240, 255), (105, 140, 230),
  (210, 240, 255), (153, 153, 153), (0, 100, 0), i 255-delar.
- Det som i exemplet lästes som ett sista f64 0 är två heltal 0: en nolla och
  en null-pekare. I formatversion 451 är pekaren ett nytt objekt av klass 185
  (ett material som JSON, [format.md](../format.md)) vars grundtillgång har
  definitionen `SunAndSky`, alltså en himmel, och paketet är därför längre.
- Okänt: `mode`, nollan före pekaren (tolken kräver 0, `Unsupported` annars)
  och vilken färg som används till vad. Att trippel 2–3 och 4–5 är lika i
  exemplet tyder på två uppsättningar av samma två färger, men det är inte
  prövat.

## `LcOpLightsElement`

En objektström, id från 100:

```
LJUSEN := [i32 n] n × LJUS, nollor upp till 8
LJUS   := [i32 83] [str namn] [i32 0] [16 byte GUID] [i32 0] PTR(27)
          [3 × f64 origin] [3 × f64 direction]
27     := [i32 1] PTR(185 eller null) [i32 0] [12 × f64]
```

Klass 27 är samma som i en partitionssvans ([klass32.md](klass32.md)), och
klass 185 ett material som JSON ([format.md](../format.md)).

Belagt i `.nwd` (två ljus, "Distant Light" och "Distant Light (1)"):
- grammatiken tolkar paketet till exakt slut, med id 100–103;
- GUID:en är ljusets `VersionGUID` i klass 185:s JSON;
- de tolv talen i klass 27 är byte för byte samma som i de två ljusen i
  dokumentets egen partition (`LcOpNwdPartition`);
- talen 3–5 och 6–8 är lika, och lika med `specularColor` i ljusets JSON
  (1, 1, 1 i det ena; 0,69999998807907104 i det andra);
- de tre sista talen har längden 1, och `direction` är samma tre tal;
- `origin` är (0, 0, 0).

Okänt: talet 83 (tolken ger `Unsupported` för ett annat), de tre första talen
i klass 27 (nollor), och om `[i32 0]` efter namnet är ett fält eller PAD8 —
det står på 4 mod 8 i båda ljusen.

## `LcOpSavedViewsElement`: sparade vyer

En objektström, id från 100. Bara med innehåll i formatversion 451; tomt
(`[i32 0]` och nollor) i exemplet `.nwd`.

```
VYER      := [i32 n] n × POST, nollor upp till 8
POST      := [i32 slag] [str namn] [i32 0] [16 byte GUID] [i32 0]
             slag 0: VY [i32 r] r × MARKERING [i32 m] m × PTR(89)
             slag 4: [i32 c] c × POST
VY        := som LcOpCurrentViewElement, från form till de 680 byten
MARKERING := [i32 typ]
             typ 1: [4 × f64]            två punkter (x0, y0, x1, y1)
             typ 4: [str text] [2 × f64]  en punkt (x, y)
             [i32 width] PAD8 [3 × f64 colour] [i32 pattern]
89        := [str element] KROPP
             "LcOpShadOverridesElement":    ([i32 1] [i32 index])* [i32 0]
             "LcOpShadFragMaterialElement": [i32 0] [i32 0]
```

- Slag 0 är en vy, slag 4 en mapp med `c` poster; mappar ligger i mappar.
  Huvudet `[i32 slag] [str namn] [i32 0] [GUID] [i32 0]` är samma som i
  `LcOpLightsElement` (slag 83) och `LcReMaterialElement` (slag 80).
- Ett 89-objekt delas mellan vyer genom bakåtpekare.
- Belagt i formatversion 451: grammatiken tolkar paketet till exakt slut; varje
  vy har form 895 eller 1919, och varje vy följer det som står om kameran
  ovan; markeringarna är av typ 1 och 4, deras punkter ligger i [−1, 1] och
  färgen är tre tal i [0, 1]; `width` och `pattern` är desamma i alla.
  Indexen i ShadOverrides är samma mängd som i paketet
  `LcOpShadOverridesElement`, men stigande.
- Att punkterna ligger i [−1, 1] tyder på normerade skärmkoordinater; det är
  inte prövat. Okänt: `width`, `pattern`, vad indexen pekar ut, och vad ett
  FragMaterial som inte är tomt innehåller.
- Tolken ger `Unsupported` för ett annat slag, en annan markeringstyp, ett
  annat element och ett FragMaterial som inte är `[0][0]`, och
  `LimitExceeded` för mappar djupare än 256.

## `LcOpShadOverridesElement` och `LcOpTextureSpaceElement`

Bara i formatversion 451.

```
ShadOverrides := [i32 n] n × ( [i32 index] [i32 flags] ) nollor upp till 8
TextureSpace  := [i32 0] [i32 n] n × ( [i32 index] [i32 kind] [16 × f64] ) nollor upp till 8
```

- Belagt: grammatikerna tolkar paketen till exakt slut. I ShadOverrides är
  `flags` samma värde i varje post, och indexen är samma mängd som i varje
  ShadOverrides-objekt i de sparade vyerna. I TextureSpace är `kind` och de
  sexton talen lika i varje post.
- Indexen i de två paketen är tal av samma storlek, och några förekommer i
  båda. Vad de pekar ut (fragment, noder) är inte prövat.
- Okänt: `flags`, `kind` och talen. Nollan först i TextureSpace ger
  `Unsupported` om den inte är 0.

## `LcOpNwdSharedNodes`

En objektström, id från 100. Tomt i exemplen; med innehåll i formatversion
451.

```
[i32 n] n × PTR(102), nollor upp till 8
102 := [str name] PAD8 [f64 size] [4 × i32]
```

- Belagt: grammatiken tolkar paketet till exakt slut, med ett nytt objekt för
  varje post. `name` är namnet på ett typsnitt; de fyra heltalen är desamma i
  varje post.
- Okänt: vad `size` mäter och i vilken enhet, heltalen, och vad som använder
  typsnitten. En post får vara en bakåtpekare.

## `LcOpGridElement`: rutnät

Bara i formatversion 451.

```
NÄT    := [i32 n] n × SYSTEM [i32 tail_a] [i32 tail_b] nollor upp till 8
SYSTEM := [str name] PAD8 [3 × f64 origin] [3 × (3 × f64) axes]
          [i32 nl] nl × LINJE [i32 nv] nv × NIVÅ [i32 level]
LINJE  := [str label] PAD8 [2 × f64] [3 × i32] PAD8 [4 × f64 ends]
NIVÅ   := [str name] PAD8 [f64 elevation]
```

- Belagt: grammatiken tolkar paketet till exakt slut.
- `origin` är en punkt i filens koordinater (samma storlek som modellens).
  `axes[0]` och `axes[1]` har längden 1 och är vinkelräta; `axes[2]` är lika
  med `axes[1]` i varje system.
- `ends` är två punkter (x0, y0, x1, y1): i varje linje är x0 = x1 eller
  y0 = y1, alltså en linje parallell med en av systemets axlar.
- `elevation` är stigande inom varje system, och i filens längdenhet: där en
  nivås namn är en höjd i meter är talet samma höjd i längdenheten.
- `level` ligger i [−1, nv) och är −1 precis när systemet saknar nivåer; det
  ser ut som den valda nivån. Tolken ger `Unsupported` för ett värde utanför.
- `tail_a` är mindre än `n`.
- Okänt: de två f64 och tre heltal i en linje (heltalen är desamma i varje
  linje), `tail_a` och `tail_b`.

## `LcReMaterialElement`: material

En objektström, id från 100. Bara i formatversion 451.

```
[i32 n] n × ( [i32 80] [str name] [i32 0] [16 byte GUID] [i32 0] PTR(185 eller null) )
nollor upp till 8
```

- Belagt: grammatiken tolkar paketet till exakt slut. Varje post har ett nytt
  objekt av klass 185 (material som JSON, med inbäddade bilder som i trädet).
  `name` förekommer i materialets JSON, och GUID:en är en RFC 4122-GUID av
  version 4.
- GUID:en är grundtillgångens `VersionGUID` i en del av materialen men inte i
  alla, så sambandet är inte belagt.
- Okänt: vad som använder materialen. Ett annat slag än 80 ger `Unsupported`.

## `LcOpClashElement`: kollisionstester

Bara i formatversion 451. Inga pekare; strängar, PAD8 och 64-bitarsvärden som
i objektströmmarna.

```
KOLL   := [i32 n] n × ROT [i32 nr] nr × REGEL [i32 nm] nm × MODELL
          nollor upp till 8
ROT    := [i32 slag] [str namn] [i32 0] [GUID] [i32 0]
          slag 53: [i32 c] c × TEST
          slag 54, 55: [i32 0]
TEST   := [i32 50] [str namn] [i32 0] [GUID] [i32 0]
          [i32 0] [i32 -1] [i32 0] [i32 0] [i32 -1] [i32 0] [i32 0] [i32 1] [i32 1]
          [2 × f64] [3 × f64 0] [i32 1] [i32 0]
REGEL  := [str namn] [i32 1]
MODELL := [str fil] [str sökväg] [str prefix] [GUID] [str namn] [str loader]
          [i32 0] [i32 mode] [9 × f64 matris] [3 × f64 flytt]
          [i32 enhet] [i32 1] [f64 0] [3 × f64 up] [3 × f64 north]
          [i32 0] [i32 -1] [i32 -1] [i32 -1] [i32 0] [i32 0] [i32 1]
          [str fil] [str sökväg] [i64 tid] [i64 storlek] [f64 0] [3 × f64 c] [GUID]
```

- Belagt: grammatiken tolkar paketet till exakt slut. Rötterna är av slag 53
  (testerna, med poster av slag 50), 54 och 55, med samma huvud som de
  sparade vyerna; 54 och 55 har inga poster.
- Reglerna är namn på insticksmoduler. Talet efter varje regel är 1.
- En modell är en annan fil, utpekad med namn och sökväg: `fil` är sista
  ledet i `sökväg`, och båda står två gånger, lika. `prefix`, GUID:en och
  `namn` är som prefixet, scen-GUID:en och namnet i `LcOpOdyFileInfoChunk`:
  `prefix` är `cache` när modellen är en `.nwc`, och modeller med samma
  `namn` har samma GUID.
- `matris` står radvis och är en vridning kring z gånger skalan
  `metres_per_unit(enhet) / metres_per_unit(filens egen kod)`, med alla fyra
  koderna 0–3; därav koden 1 som centimeter ([klass32.md](klass32.md)).
  `north` är matrisens andra rad, normerad, och `up` (0, 0, 1), på 1e-15
  när. `c` är `north[0]`, `north[1]` och ett tal nära 2^−52. De tre
  vektorerna har samma form som `a`, `b` och `c` i en partitionssvans.
- `tid` och `storlek` står på samma sätt som källfilens tid och storlek i
  `LcOpDBCache`; att de är det är inte prövat.
- Om reglerna och modellerna hör till roten av slag 55, som står sist, eller
  till paketet går inte att avgöra.
- Okänt: testernas två f64 och tre nollor, `mode`, de fasta talen, och vad
  den sista GUID:en i en modell identifierar. Testerna har inga urval av
  objekt i formatversion 451; hur ett urval står är inte känt.
- Tolken ger `Unsupported` för ett annat slag, ett annat fast tal och en
  nolla som inte är 0.

## `LcOpNwdSerial` och `LcOpNwdGeometryCompress`

40 råa byte var; ingen tolk.

- Belagt: de 16 första byten är lika i de två paketen och i alla tio filer.
  `LcOpNwdSerial` är lika i alla tio. `LcOpNwdGeometryCompress` är lika i de
  nio `.nwc` och en annan i `.nwd`, vilket samvarierar med normalernas bredd
  (16 bitar i `.nwc`, 8 i `.nwd`).
- De 16 första byten är ingen RFC 4122-GUID (variantbitarna stämmer inte).
  De 24 sista ser ut som slumpmässiga byte.
- I formatversion 451 finns bara `LcOpNwdGeometryCompress`, och det är byte
  för byte samma som i exemplet `.nwd`.
- Okänt: allt annat. Med bara två värden går ingen grammatik att belägga.
