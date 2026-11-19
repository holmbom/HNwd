# HNwd – plan och formatanteckningar

C++20-bibliotek som läser Navisworks-filer (`.nwd`, `.nwc`, `.nwf`) utan Navisworks.
Samma familj som HLas och HDxf: inga beroenden, MIT, ett publikt huvud.

Underlaget är en port av **NwcIO** (C#, MIT, <https://github.com/IOExtend/NwcIO>,
version 0.13.0, 2026-11-17). Formatbeskrivningen nedan är min sammanfattning av
deras `docs/format-notes.md` och källkod, plus egna slutsatser. Den är **inte
verifierad mot en riktig fil än**. Det är steg 1 i kväll.

---

## 1. Läget i korthet

- Det finns ingen officiell beskrivning av `.nwd`. Det enda kommersiella
  alternativet är ODA BimNv (betalt, kräver ODA-medlemskap).
- NwcIO har kartlagt containern, innehållsförteckningen, objektträdet, placeringen,
  geometrin och GUID:erna. Det mesta är testat på filer från Revit.
- **Det som saknas för `.nwd`:** toppnoderna i trädet (class 32,
  `LcOaPartition`). NwcIO kastar `NotSupportedException` där, så i en `.nwd` får man
  i dag geometri och placering men **inte trädet och egenskaperna**. Den luckan
  kan HNwd lösa.
- Andra luckor: måttenhet (lagras inte i något känt paket), färg- och UV-värden,
  punktposter (class 96) och kvantiserade positioner (negativ `p1`).

### Punktmoln
Navisworks bäddar normalt **inte** in skannade punktmoln i `.nwd`. Den pekar på
ReCap-filer (`.rcp`/`.rcs`, Autodesk-format) via XRef-tabellen (§3.8). HLas
hjälper alltså inte direkt. Class 96 ("punktposter") verkar vara små
punktmängder i själva modellen. ⚠️ Kontrollera med en riktig fil som har moln.

---

## 2. Licens och tillskrivning

HNwd blir MIT. NwcIO är MIT, så porten är tillåten om licenstexten följer med.

**`LICENSE`**: din vanliga MIT-text.

**`THIRD_PARTY_NOTICES.md`** (samma upplägg som i HLas):

```markdown
# Third-party notices

HNwd is licensed under the [MIT License](LICENSE). It contains the
third-party material below, under its own license.

HNwd has no other bundled dependencies.

---

## NwcIO

**Where:** the container, DbCache, hierarchy, fragment, geometry record and
GUID table readers in `src/core/` are a C++ port of the corresponding parsers
in NwcIO (<https://github.com/IOExtend/NwcIO>), and the format notes in
`doc/format.md` build on NwcIO's `docs/format-notes.md`.

The NwcIO files in question carry this notice:

    MIT License

    Copyright (c) mahach666

    Permission is hereby granted, free of charge, ...   ← klistra in hela texten ur deras LICENSE
```

Skriv också i README: *"Format knowledge builds on NwcIO by mahach666 (MIT)."*
Och samma rad som de har: *"Not affiliated with Autodesk. Navisworks is a trademark
of Autodesk, Inc."*

⛔ Ta inte med kod eller binärer från Autodesk, och plocka inte isär Navisworks.
Analys av filerna räcker.

---

## 3. Formatet

All data är little-endian. Strängar är `[int32 len][UTF-8][nollor upp till 4]`,
där `len == -1` betyder null (inga byte och ingen utfyllnad). `f64` och `i64`
ligger på jämn 8-byte räknat från början av **strömmen** (inte filen). Vid behov
ligger det en `int32 0` före som utfyllnad.

### 3.1 Container: `#LcUStream`

Filen består av sektioner. Varje sektion börjar med en ASCII-rad som slutar på `\n`:

```
#LcUStream-V1.1 binary double release utf8 navisworks:448 lizlib
#LcUStream-V1.1 binary double release C lichunk-008
```

| Token | Betydelse |
|---|---|
| `#LcUStream-V1.1` | formatidentitet |
| `double` | flyttal i matriser och poser är 64-bit |
| `utf8` / `C` | teckenkodning |
| `navisworks:448` | formatversion (448 ≈ Navisworks 2016–2025) |
| `lizlib` / `lichunk-008` | rå zlib-ström eller chunkad layout |

Efter raden följer zlib-strömmar (RFC 1950, första byte `0x78`, sedan `01`, `5E`,
`9C` eller `DA`) som ligger tätt efter varandra. Mellan vissa ligger 20 eller 40
byte hash/ramdata, och sist kan det finnas en svans.

⚠️ NwcIO letar sig fram genom att skanna efter zlib-signaturer byte för byte. Det
behövs inte när innehållsförteckningen finns (§3.2). Använd den, så blir det
slumpvis åtkomst.

### 3.2 Innehållsförteckningen (DbCache-katalogen)

**Sektion 1 har exakt en ström: katalogen.** Den listar alla paket i sektion 0
och täcker den helt.

```
DIRECTORY := [int32 -1][int32 n] ENTRY{n}           (slutar exakt vid strömmens slut)
ENTRY     := [str name][int32 flags] PAD8 [int64 offset][int64 length][int32 a][int32 b]
PAD8      := en int32 0 om markören står på 4 mod 8, annars inget
```

- `name`: matcha på texten efter sista `\`, till exempel `LcOpNwdGeometry`.
  `.nwd` har ett sökvägslikt prefix.
- `offset` är absolut i **filen**, `length` är antal byte i filen.
  Posterna ligger i filordning, tätt efter varandra.
- `flags`: `1` = zlib-strömmar, `3` = råa byte.
- `(a, b)`:
  - `(0, 0)`: en zlib-ström vid `offset`.
  - `(4, b)`: 4 byte prefix, sedan zlib-strömmar tätt efter varandra till
    `offset+length`. `b` är den komprimerade längden på den **sista** strömmen.

De paket vi behöver:

| Paket | Form | Innehåll |
|---|---|---|
| `LcOpNwdLogicalHierarchy` | (0,0) | objektträdet + delade egenskaper (§3.4) |
| `LcOaPartitionProps` | (4,b) | egenskaper som bara en nod använder (§3.5) |
| `LcOpNwdFragments` | (0,0) | placering av varje geometribit (§3.6) |
| `LcOpNwdGeometry` | (4,b) | trianglar och linjer (§3.7) |
| `LcOpGuidStore` | (0,0) | GUID per element (§3.3) |
| `LcOpNwdGeometryCompress` | flags 3 | 40 råa byte, okänt |

**Arbetsgång för HNwd:** minnesmappa filen → hitta rubriken för sektion 1 →
packa upp katalogen → packa sedan bara upp de paket som efterfrågas. Det är så
en fil på flera GB blir hanterbar.

### 3.3 GUID-tabellen (`LcOpGuidStore`)

```
[int32 1][int32 n][n × 16 byte GUID]     (Windows-ordning: Data1–3 little-endian)
```
Element med index `X` (från trädet) har GUID `X-1`. IFC GlobalId = de 128 bitarna i
big-endian, 22 tecken base64 (`0-9A-Za-z_$`).

### 3.4 Objektträdet (`LcOpNwdLogicalHierarchy`)

Det här är ett serialiserat objektnät med **bakåtpekare**.

```
STREAM  := [int32 k][k × int32 rootCounts]  [int32 100][int32 n] n × PTR  [int32 1]   pad8
PTR     := 0                         null
         | [1][id]                   pekare bakåt till ett FÄRDIGT objekt
         | [nextId][classId][body]   nytt objekt
```

- `nextId` börjar på 100 (id 100 är en underförstådd rot utan kropp). Ett objekt
  får sitt id **innan** kroppen läses, så nästlade objekt numreras djupet först.
- Id gäller per ström.
- ⛔ Allt annat (okänd klass, pekare till ett ofärdigt objekt) ska ge fel, inte
  gissningar.

**Klasser:**

| id | Namn | Kropp |
|---|---|---|
| 53 | grupp | `[str name][ptr 52][i32 flags][i32 X][i32 Y][Y × attr-ptr][i32 c][c × barn-ptr]` |
| 22 | löv | som 53 men utan `c` och barn |
| 32 | `LcOaPartition` (**bara .nwd**) | `[i32 0]` + en 53-kropp + **okänd svans** ← LUCKAN |
| 52 | namnpar | `[str visningsnamn][str internt namn]` |
| 86 | egenskapsflik | `[str name][ptr 52][i32 flags][i32 n] n × ([ptr 52 key][i32 type][value])` |
| 31 | material | samma fält som 54 men `f64`, plus namn |
| 40 | flytt | |
| 41 | flytt + kvaternion | `x → R(q)x + t` |
| 39 | 3×3 matris (kolumnvis) + flytt | |
| 79 | hyperlänkar | |
| 200, 185 | material som JSON | |
| 66 | Revit-id | `[str null][ptr 52 "LcRevitId"][i32 0x01000000] PAD8 [i64]` |

**Värdetyper i 86:** `0` inget · `1,5,6,7,10,11` f64 · `2,3` i32 · `4,9` sträng ·
`8` pekare till 52.

- `X` = elementindex: 0 om noden inte är ett element, annars är `X-1` index i
  GUID-tabellen. Revit skriver ibland samma element som två syskon med samma `X`.
- Samma `X` under olika föräldrar = länkad placering (länkad Revit-modell).
- **Trädet är egentligen en DAG**: ett barn kan vara en bakåtpekare, så samma delträd
  kan sitta på flera ställen. När man expanderar det blir det en kopia per väg.
- **`seq` (preorder-index):** gå från roten i lagrad ordning och räkna grupper.
  Ett delat delträd räknas igen varje gång det dyker upp. Fragmenten (§3.6) pekar på
  löv via detta index.

**Toppnoderna i en `.nwd`** är class 32. I en `.nwc` är de class 53.

### 3.5 Egenskaper per nod (`LcOaPartitionProps`)

```
SEGMENT := [int32 0] ZSTREAM{k} ZTOC
ZTOC    := [int32 k][k × int32 komprimerad längd] pad8
ZSTREAM i := n_i rötter av klass 86, 79 eller 66, pad8   (id från 100 per ström)
```
**Kopplingen:** den n-te **null-attributplatsen** i trädet (i byteordning) hör till
den n-te roten i ZSTREAM 0 ‖ 1 ‖ … Flikar som bara en nod använder ligger här, och
flikar som delas av flera noder ligger kvar i trädet.

### 3.6 Placering (`LcOpNwdFragments`)

```
[int32 n] n × PTR   pad8          (samma pekarregel som 3.4, varje rot är class 13)
```

| id | Kropp |
|---|---|
| 13 fragment | `[f64 D][i32 a][i32 b][i32 c][i32 seq][ptr pose][ptr appearance][ptr geometry]` |
| 14 pose | `[3 × f64 t]`, bara flytt |
| 15 pose | `[3 × f64 t][4 × f64 q (x,y,z,w)]` |
| 16 pose | 15 + `[f64 s]`, enhetlig skala, **kan vara negativ (spegling)** |
| 55 utseende | `[i32 flags][ptr 54]` + `[ptr 185]` om `flags & 32` |
| 54 material | 14 × f32: ambient 0–2, diffuse 3–5, specular 6–8, emissive 9–11, shininess 12, transparens 13 |
| 93 geometrireferens | `[6 × f32 bbox][3 × f64 c][f32][i32 key][i32 rec]`, `rec` = 1-baserat postnummer i §3.7 |

- Världskoordinat = `s · R(q) · v + t`. Ingen enhetsomräkning.
- `s < 0` → vänd triangelordningen.
- `a` = antal trianglar eller segment (bra kontrollsumma). `D`, `b` och `c` är okända.
- `seq` minskar aldrig längs strömmen, och ett löv äger en sammanhängande följd fragment.
- Flera fragment kan peka på samma geometripost (instansiering). Posen delas aldrig.

### 3.7 Geometri (`LcOpNwdGeometry`)

Paketet består av 4 byte prefix (`int32 65536`), sedan `ch` chunk-strömmar och sist
en TOC-ström.

```
TOC := [int32 N][int32 ch][ch × int32 komprimerad chunkstorlek][N × int32 postlängd]  pad8
```
Packa upp och lägg ihop chunkarna. Post `k` börjar vid summan av tidigare
längder, alltid jämnt på 8. ⚠️ **En post kan sträcka sig över en chunkgräns.** En
stor `.nwd` kan ha tusentals chunkar.

⚠️ För att slippa packa upp allt: bygg en tabell med kumulativ uppackad storlek
per chunk. Uppackad storlek står inte i TOC, så den kräver en första
genomgång eller att man packar upp efter behov och sparar i en cache. Mät.

**Post-grammatik** (`int` = int32 LE, all utfyllnad är 0):

```
RECORD := [100][cls] V UVSLOT NSLOT CSLOT P SFIELD? pad8
V      := [101][60][p1][p2][p2 × f32 xyz] VMAP?          npos = p2/3; VMAP finns om p1 != npos
UVSLOT := 0 | [id][61] UVBODY
NSLOT  := 0 | [id][100][-p1][pal] PACKED(3·pal, B) NTOK?   NTOK om pal != p1
CSLOT  := 0 | [id][58][5634][-p1][pal][pal × 4 byte] CTOK? CTOK om pal != p1
P      := [n][bits][n][ceil(n·bits/32) ord]               strip- eller polylinjelängder L
SFIELD := [S] TOKENS(S, p1)?                              bara class 94, tokens om S > 0
```

- `cls`: **94** trianglar (har alltid normaler, aldrig färg) · **95** linjer (bara
  färg) · **96** punkter (stöds inte än).
- `p1` = antal **render-vertexar**, `p2/3` = antal lagrade positioner, `VMAP` mappar
  mellan dem.
- **Trianglar:** hörnen är `0..p1-1` om `S = 0`, annars `TOKENS(S, p1)`. Dela upp
  efter `L` i strips. I varje strip: triangel `i` = `(c[i], c[i+1], c[i+2])` för
  jämnt `i` och `(c[i+1], c[i], c[i+2])` för udda `i`. Pariteten börjar om per strip.
  Framsidan är moturs.
- **Linjer:** dela `0..p1-1` efter `L` i öppna polylinjer.
- **Normaler:** varje palettpost är tre tvåkomplementsvärden på `B` bitar, delat
  med `2^(B-1)-1` och normaliserat. **`B` lagras inte**: 16 för `.nwc` och 8 för `.nwd`
  (den bredd där alla poster går att tolka).
- **Färgpalett:** `[A=FF][B][G][R]`.
- **PACKED:** bit `j` = bit `31 - j%32` i LE-ord `j/32`. Värde `k` ligger på bit
  `k·w … k·w+w-1`, mest signifikant först.
- **UVBODY:** rå (`[p1][fc][fc × f32]` + tokens om `fc/2 != p1`) eller kvantiserad
  (`[-p1][4 × f32][w][m][packade ord]` + tokens om `m != p1`, `bu = w & 0xFF`,
  `bv = w >> 8 & 0xFF`).

**TOKENS(count, fresh)**, en bytekod för indexmappar. Tillståndet är `out = []`, `next = 0`:

| Ledbyte | Byte | Betydelse |
|---|---|---|
| `00–3F` | 1 | lägg till `next .. next+b`, sedan `next += b+1` |
| `40–7F` | 1 | kopiera `out[n-d]`, `d = b-0x40` |
| `80–BF` | 1 | upprepa förra värdet `b-0x7F` gånger |
| `D0–D5` | 1 | bara normaler: axelkod `D0` +X, `D1` -X, `D2` +Y, `D3` -Y, `D4` +Z, `D5` -Z |
| `E0–EF xx` | 2 | kopiera `out[n-d]`, `d = ((b&15)<<8) \| xx` |
| `F0 hh ll` | 3 | literal `(hh<<8)\|ll` (< `next`) |

Vid slutet ska `next == fresh` gälla, och strömmen fylls ut till 4 byte. Det är en bra självkontroll.

⛔ Varje post ska sluta **exakt** på sin TOC-längd. Annars är tolkningen fel. Kontrollera det i varje post.

### 3.8 Övrigt

- **Partition-huvud (.nwc):** `[i64 7][str källsökväg][i32 -1] pad8 [i64 unixtid][i64 källstorlek][str loader-plugin]`
  + inställningssträngar. Revit-exporten skriver loader `nwexportrevit`.
- **XRef-tabell:** en ström med JSON `{"Type":"XRefTable","Version":1,"SavedFilename":…,"Table":[{"OriginalPath","RemappedPath"}]}`.
  Här syns texturer, länkade `.nwc`/`.nwd` och troligen ReCap-moln.
- **Vypunkter:** tagg `517` (`0x205`), följt av projektion, 3 × f64 position, 4 × f64
  kvaternion (vy längs −Z, upp +Y), aspekt, fokalavstånd, vyhöjd och near-plan.
- **Enhet:** finns inte i något avkodat paket. Revit = fot. Annars:
  kontrollera med kända koordinater (SWEREF 99 TM ger 6–7-siffriga meter).

---

## 4. Arkitektur (samma stil som HLas)

```
HNwd/
├── CMakeLists.txt            C++20, cmake 3.20, Ninja
├── CLAUDE.md
├── LICENSE                   MIT
├── THIRD_PARTY_NOTICES.md    NwcIO
├── README.md / README.sv.md
├── TODO.md
├── include/HNwd.h            enda publika huvudet
├── src/
│   ├── core/                 HNwd::core – inga beroenden
│   │   ├── mmap_file.*       minnesmappad fil, Win + POSIX
│   │   ├── inflate.*         egen zlib/DEFLATE-avkodare + Adler-32
│   │   ├── container.*       #LcUStream-rubriker, sektioner
│   │   ├── dbcache.*         katalogen (§3.2)
│   │   ├── objstream.*       pekarläsaren (PTR, id-tabell, alignment)
│   │   ├── hierarchy.*       klasserna i §3.4 + 32
│   │   ├── partition_props.* §3.5
│   │   ├── fragments.*       §3.6
│   │   ├── geometry.*        TOC + post-grammatik + TOKENS + PACKED (§3.7)
│   │   └── guids.*           §3.3
│   └── scene/                HNwd::scene – expanderat träd, world-transform, mesh-API
├── tools/
│   ├── nwddump.cpp           listar katalog, paket, klass-histogram, hexdump av okänt
│   └── nwd2obj.cpp           snabb visuell kontroll
├── tests/
│   ├── test_data.h           sökvägar + SKIP_IF_MISSING (som HLas)
│   └── …
└── doc/
    ├── format.md             §3 här, och den växer
    └── research/             loggar från hexgrävandet
```

**Beroenden pekar åt ett håll:** `scene` → `core`. Upprätthålls via include-sökvägarna
precis som i HLas.

### Egen inflate
HLas och HDxf har inga beroenden, så HNwd behöver en egen DEFLATE-avkodare (cirka
300–400 rader: lagrade, fasta och dynamiska Huffman-block) + zlib-huvud + Adler-32.
Tips: skriv den, och testa den mot `zlib` **bara i testprojektet** (eller mot filer
som komprimerats med `python -c zlib.compress`). Sätt tak per ström och totalt, som
NwcIO gör (512 MiB / 2 GiB), men gör taken valbara.

### Minne för stora filer
NwcIO läser in hela filen och packar upp allt i RAM, så filer över 2 GB stoppas. HNwd ska:
1. minnesmappa filen,
2. läsa katalogen (liten),
3. packa upp träd, fragment och GUID (relativt små) direkt,
4. packa upp geometrichunkar **efter behov**, med en LRU-cache på chunknivå.

### Förslag på API (skiss)
```cpp
namespace HNwd {
enum class Status { Ok, NotLcUStream, NoDirectory, Corrupt, Unsupported, ... };

class Document {
public:
    Status open(const char* path);
    const FileInfo& info() const;            // version, format, loader, källa
    const DbCache& directory() const;        // för nwddump
    const Node* root() const;                // expanderat träd
    size_t fragment_count() const;
    bool   fragment(size_t i, Fragment& out) const;   // pose, material, rec, seq
    bool   mesh(uint32_t rec, MeshView& out);         // lazy, packar upp chunkar
    const char* last_error() const;
};
}
```

---

## 5. Portkarta (C# → C++)

Själva läsaren är cirka 3 000 rader av NwcIO:s 53 000. Resten (IFC, krockkontroll,
HTML, BCF, TimeLiner, skrivaren) hoppar vi över.

| Ordning | NwcIO (`src/NwcIO/Internal/…`) | Rader | HNwd |
|---|---|---|---|
| 1 | `Container/LcUStreamHeader.cs` | 69 | `container.*` |
| 1 | `Container/NwcContainerReader.cs` | 400 | `container.*` + `inflate.*` (skippa skanningen, använd katalogen) |
| 1 | `Container/LcRawStream.cs` | 69 | `container.*` |
| 2 | `Parsing/DbCacheParser.cs` | 143 | `dbcache.*` |
| 3 | `Parsing/HierarchyParser.cs` | 779 | `objstream.*` + `hierarchy.*` |
| 4 | `Parsing/FragmentParser.cs` | 176 | `fragments.*` |
| 5 | `Parsing/GeometrySegmentReader.cs` | 165 | `geometry.*` |
| 5 | `Parsing/GeometryRecordParser.cs` | 574 | `geometry.*` |
| 6 | `Parsing/GuidTableParser.cs` | 35 | `guids.*` |
| 7 | `Parsing/PartitionHeaderParser.cs` | 145 | `container.*` (bara .nwc) |
| – | `Model/Geometry3d.cs` | 260 | kvaternion → matris, bbox |
| – | `Export/IfcGuid.cs`, `NwcElementGuid.cs` | 154 | valfritt |

Repo att klona: `git clone https://github.com/IOExtend/NwcIO.git`. Läs
`docs/format-notes.md` parallellt.

---

## 6. Luckan: class 32 i .nwd

NwcIO-koden säger rakt ut: *"LcOpNwdLogicalHierarchy holds .nwd partition nodes
(class 32), which are not decoded"*. Det som är känt:

```
32 := [i32 0] <class-53-kropp> <svans som inte är avkodad>
```

**Metod:**
1. Gör `nwddump` som skriver ut var varje class-32-objekt börjar, var 53-kroppen
   slutar och 256 byte efter i hex.
2. Gör testfiler i Navisworks (eller be någon som har det):
   - en `.nwd` med **en** inläst modell, sedan samma med **två** och **tre**,
   - samma modell sparad två gånger med och utan vypunkt eller utan markering.
   Jämför svansarna. Den del som växer med antalet modeller är det vi letar efter.
3. Gissningar att pröva först: svansen innehåller troligen sådant som hör till
   den inlästa modellen: källsökväg (sträng), tidsstämpel (i64), filstorlek (i64),
   loader-plugin (sträng), alltså samma fält som partition-huvudet i en `.nwc`, och
   kanske en transform för modellen (class 39/40/41-pekare). Sök efter `[int32 len]`
   följt av läsbar text, och efter värden som `-1`.
4. Svansen kan innehålla nya PTR, och då kommer id-räkningen ur fas. Kontrollen är
   att resten av strömmen går att tolka utan fel och slutar exakt på `[int32 1]` + pad8.
5. När det fungerar: skriv in det i `doc/format.md` och skicka gärna en PR till NwcIO.

---

## 7. Testning

- **Oracle:** kör NwcIO på samma fil (`dotnet tool install --global NwcIO.Cli`,
  `nwcio fil.nwd fil.json --no-tree`) och jämför antal meshar, trianglar, bbox och
  transforms med HNwd. Använd `--no-tree` för `.nwd`.
- **Självkontroller som redan finns i formatet:**
  - varje geometripost slutar exakt på TOC-längden,
  - TOKENS: `next == fresh` vid slut,
  - fragment `a` == faktiskt antal trianglar eller segment,
  - fragmentets bbox (class 93) == bbox för postens positioner,
  - `seq` minskar aldrig.
- `tests/test_data.h` med `SKIP_IF_MISSING`, som i HLas. Testfilerna checkas
  inte in. Kör UBSan/ASan på bitläsningen och TOKENS.
- Testfiler att ha:
  1. en liten `.nwc` från Revit (fungerar säkert med NwcIO),
  2. en liten `.nwd` med en modell,
  3. en `.nwd` med flera modeller (luckan),
  4. en `.nwd` på flera GB (minne och fart),
  5. en `.nwd` med punktmoln (för att se hur moln lagras).

---

## 8. Kvällens ordning

1. Skapa repo `HNwd` med skelett, CMake, LICENSE och THIRD_PARTY_NOTICES.
2. `mmap_file` + `inflate` med tester.
3. Rubrikerna i `container` + `dbcache`, och `tools/nwddump` som listar paketen.
   **Kör på en riktig `.nwd`.** Stämmer namnen i §3.2?
4. `objstream` + `hierarchy` utan class 32. Kör mot en `.nwc` först.
5. `fragments` + `geometry` → `tools/nwd2obj` och titta på resultatet.
6. Börja gräva i class 32 (§6).

---

## 9. Risker

- NwcIO är nytt (preview, en enda squashad commit, inga publicerade testfiler).
  Allt här är **påståenden** tills vi har kört det mot riktiga filer.
- Det mesta är testat på Revit-filer. Filer från Civil 3D, MicroStation och
  PLS-CADD kan ha andra klasser.
- Autodesk kan ändra formatet (`navisworks:448` är en version, så kolla vad din fil
  har).
- Kvantiserade positioner och class 96 är okända. Om dina filer använder dem
  behövs mer grävande.
