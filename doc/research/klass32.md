# Klass 32: partitionsnoden

Formatfakta om klass 32 och klass 27, så som exempelfilerna visar dem. Allt
nedan är prövat mot de tio exempelfilerna (nio `.nwc` och `Navisworks.nwd`) av
[src/core/partition.cpp](../../src/core/partition.cpp) och
[src/core/hierarchy.cpp](../../src/core/hierarchy.cpp), med testerna i
[tests/test_partition.cpp](../../tests/test_partition.cpp). Det som står under
"Okänt" är inte belagt.

## Var klass 32 finns

- **Paketet `LcOpNwdPartition`** (formen (0, 0)): en fristående objektström med
  ett enda objekt av klass 32. Den har ingen underförstådd rot, och id räknas från
  100. Alla tio filer har paketet.
- **Toppnoderna i `LcOpNwdLogicalHierarchy` i en `.nwd`**: en nod av klass 32 per
  inläst modell. En `.nwc` har en toppnod av klass 53.

Klass 32 står bara som toppnod. En klass 32 som barn eller som mål för en länk är
inte sedd.

## Grammatiken

Pekare, strängar och 64-bitarsvärden är som i [format.md](../format.md).
`PTR(c)` är en pekare till ett objekt av klass `c`. 64-bitarsvärden ligger på
jämn 8 byte från strömmens början.

```
PARTITION_STRÖM := [100][32] HUVUD SVANS
TOPPNOD_I_TRÄD  := [nästa id][32] HUVUD [i32 c] c × BARN SVANS

HUVUD  := [i32 0] [str namn] PTR(52) [i32 flags] [i32 X] [i32 Y] Y × ATTR
SVANS  := [i32 2] [i32 n] n × PTR(27)
          [3 × f64 färg] [i32 0] [str loader]
          [i32 format] [i32 läge] [i32 1]
          [3 × f64 a] [3 × f64 b]
          [i32 0 eller 1] [str text eller null] [i32 1] [str källsökväg]
          [3 × f64 c] [4 × f64 d]
          [i32 1] nollor upp till en multipel av 8

KLASS 27 := [i32 1] PTR(185) [i32 0] [3 × f64] [6 × f64] [3 × f64]
```

- `HUVUD` är en klass-53-kropp utan barnlistan: namnet, namnparet (klass 52),
  flaggor, elementindex X och Y attribut. `c` och `BARN` finns bara i trädet; de
  är som i en klass 53, och barnen är klass 22 eller 53.
- **`PARTITION_STRÖM`:** attributen är nya objekt av klass 31
  (`[str][15 × f64]`), som i exempelfilerna. Strömmen saknar egenskapsrötter, så
  en tom attributplats kan inte lösas upp där. Strömmen slutar där svansen slutar.
- **`TOPPNOD_I_TRÄD`:** attributen är som i en klass 53, och en tom attributplats
  tar nästa egenskapsrot ur `LcOaPartitionProps`. Nästa toppnod börjar direkt efter
  svansen.
- **Svansens slut:** `[i32 1]` följt av nollor upp till en multipel av 8. Det sista
  fältet före är en `f64`, så `[1]` ligger alltid på jämn 8 byte och följs av fyra
  nollbyte. Om de fyra byten är utfyllnad eller ett eget `[i32 0]` går inte att
  avgöra, eftersom byten är lika.
- **Klass 27 i svansen** skapar nya objekt i strömmen. Varje klass 27 tar ett id,
  och dess klass 185 (ett material som JSON, `skip_class185()`) tar nästa. Därför
  flyttas id-räkningen i trädet för varje partition som har ljus. En pekare i
  svansen får också vara en bakåtpekare till ett klass-27-objekt, och klass 185 i
  ett ljus får vara en bakåtpekare; det är inte sett i exempelfilerna och är bara
  prövat mot syntetiska strömmar.

## Fälten

| Fält | Vad exempelfilerna visar |
|---|---|
| namn | Modellens filnamn (`….ifc`) i en `.nwc`; `Navisworks.nwd` i dokumentets egen partition. Samma namn som toppnoden i `.nwd`-trädet. |
| namnpar | Ett nytt klass-52-objekt: `"File"` och `"LcOaPartition"` i alla tio. |
| flags | 0 i alla tio. |
| X | Elementindex som i en klass 53: ett index i filens egen GUID-tabell, så värdet är inte detsamma i `.nwc` och i `.nwd` (modell 01: 406 mot 1155, modell 02: 139 mot 2660). Är det 0 i den ena är det 0 i den andra. |
| Y | 1 i `.nwc` (ett klass-31-material), 0 i dokumentets egen partition. |
| n | 0 i varje `.nwc`. I `.nwd` är det 2 i dokumentets egen partition och i den första partitionen i trädet, och 0 i de åtta andra. |
| färg | `(0, 0, 0)` i `.nwc`. `(0,3, 0,3, 0,3)` i dokumentets egen partition och i den första partitionen i trädet, `(0, 0, 0)` i de andra. Betydelsen är okänd. |
| loader | Inläsarens namn: `LcNwcLoaderPlugin:` och ett namn i alla `.nwc`, ett annat namn i dokumentets egen partition. Samma värde i trädets partition för en modell som i modellens `.nwc`. |
| format | 448 i alla tio, samma tal som `navisworks:448` i sektionsrubriken. |
| läge | 2 i `.nwc` 01–07, 0 i 08 och 09. 2 i dokumentets egen partition. I trädet har varje modell samma värde som i sin `.nwc`. Det är längdenheten, `metres_per_unit()`: 0 meter, 1 centimeter, 2 millimeter, 3 fot (se nedan och [format.md](../format.md)). |
| a | `(0, 0, 1)` i alla tio. |
| b | `(0, 0, 0)` i `.nwc`. `(0, 1, 0)` i dokumentets egen partition och i varje partition i trädet. |
| källsökväg | Sökvägen till källfilen så som den stod när filen skrevs: en `.ifc` i `.nwc` och i trädet, en `.nwd` i dokumentets egen partition. Samma sträng i trädets partition för en modell som i modellens `.nwc`. |
| c | `(0, 0, 0)` i `.nwc`. `(0, 1, 2,2e-16)` i dokumentets egen partition och i varje partition i trädet. |
| d | `(1, 1, 0, 0)` i `.nwc`. I `.nwd` är `d[0] < d[2]` och `d[1] < d[3]`: fyra tal som hörn i en rektangel. Varje partition i trädet har sitt eget värde, och den första partitionen har samma värde som dokumentets egen. |

Tid och storlek för källfilen finns inte i partitionen. De står i `LcOpDBCache`
([source_header.h](../../src/core/source_header.h)), som bara finns i en `.nwc`.

Klass 27, i de två objekten i `.nwd`: `[i32 1]`, en klass 185 med ett ljus som JSON
(`"tag":"GenericPhotometricLight"`), `[i32 0]` och tolv `f64`. Det första objektet
har tre nollor, sex ettor och sedan `(0,371, 0,743, -0,557)`. Det andra har tre
nollor, sex 0,7 och samma tre tal med omvänt tecken. De tre sista i varje objekt
har kvadratsumman 1. Betydelsen är okänd.

Ljusen i trädets första partition är de två i dokumentets egen partition, med
samma värden.

## Hur det är belagt

- **Alla tio `LcOpNwdPartition`-strömmar** tolkas av EN grammatik till exakt slut.
  I `.nwc` är svansen 288–304 byte, i `.nwd` 7560 byte.
- **`.nwd`-trädet** tolkas till exakt slut med samma svans:
  - nio toppnoder av klass 32, den första på byte 16, och var och en börjar med
    pekaren `[nästa id][32]` direkt efter föregående svans. I strömmen finns
    exakt åtta sådana pekare utöver den första, med växande id;
  - strömmen slutar på `[1][0]` (svansens slut) och `[1][0]` (trädets slut);
  - 824 egenskapsrötter förbrukas (`null_slots == 824`), och inga blir över;
  - varje partitions barn är strukturellt lika med motsvarande `.nwc`-träd:
    samma antal expanderade noder, samma antal lagrade noder, och samma klass,
    namn, flaggor, antal flikar, antal barn och elementstatus i varje nod i
    preorder. Partitionens `expanded` är 1 + toppnodens i `.nwc`. Trädet har 21106
    lagrade noder: summan av de nio `.nwc`-filernas plus de nio partitionerna.
    Modellerna matchas på partitionens namn;
  - summan av de nio `.nwc`-filernas fragment är 56247 och av deras rötter 824,
    samma som `.nwd`;
  - partitionens källsökväg, loader, format, läge och `a` är lika med
    `LcOpNwdPartition` i modellens `.nwc`.
- **Fragmentens `seq`:** i `.nwd` pekar varje fragments `seq` på ett löv (klass 22)
  i det expanderade trädet, med den underförstådda roten som index 0 och en
  klass-32-nod räknad som en nod. Samma sak gäller i varje `.nwc`.

## Okänt

- Vad `färg`, `a`, `b`, `c` och `d` betyder. Inget koordinatsystem är belagt.
- De tre första av de tolv talen i klass 27. Talen 3–5 och 6–8 är lika med
  `specularColor` i ljusets JSON, och de tre sista har längden 1 och är samma
  tal som `direction` där; se [element.md](element.md).
- Markörerna i svansen: `2` först, `0` före loadern, `1` efter läget, och
  `[0][-1][1]` före källsökvägen. Alla har samma värde i alla tio, och en
  annan fil kan ha andra. HNwd ger då fel i stället för att gissa.
- Om `[i32 0]` i klass 27 och i svansen är heltal eller utfyllnad, och om talen
  före första `f64` är heltal. Byten är de samma.
- Attribut av annan klass än 31, och en tom attributplats, i den fristående
  strömmen.
- Om en klass 32 kan ha andra barn än ett, eller stå någon annanstans än som
  toppnod.
- Om svansen har en annan form i filer från andra inläsare än de två i
  exempelfilerna.

## Formatversion 451

- Partitioner sitter under partitioner, och läget 1 förekommer, också under
  en partition med läget 2 eller 3. En partition vars läge skiljer sig från
  den omgivandes (närmaste partition ovanför, annars filens egen) har en
  klass 39 vars linjära del är kvoten mellan lägenas meter per enhet gånger
  enhetsmatrisen: 10 för läget 1 under 2. Där den inte är det avviker den från
  kvoten gånger enhetsmatrisen med kvoten gånger samma tal i varje sådan
  partition, vilket passar en och samma vridning; så för läget 1 under 3 med
  kvoten 0,01 / 0,3048. Det belägger läget 1 som centimeter.
- `a`, `b` och `c` har samma form som `up`, `north` och `c` i modellerna i
  `LcOpClashElement` ([element.md](element.md)), där `north` är den normerade
  andra raden i modellens rotationsmatris och `c` dess två första tal följda
  av ett tal nära 2^−52. Det tyder på att `b` är modellens nordriktning och
  `a` uppåt; i en partition är det inte prövat, eftersom `b` där bara har
  setts som `(0, 1, 0)`.

## Former utöver exempelfilerna

- Barnen till en nod av klass 32 kan själva vara av klass 32.
- Ett ljus (klass 27) kan sakna sin beskrivning: pekaren till klass 185 är då
  null.
- Före källsökvägen står `[i32 0 eller 1][str text eller null]`. I
  exempelfilerna är talet 0 och texten null.
- Formatversionen i svansen kan vara 451.

Sviten prövar de här formerna mot syntetiska strömmar (`tests/test_variants.cpp`).
