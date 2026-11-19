#pragma once
#include "nwd_error.h"
#include "object_reader.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace HNwd::detail {

// Klass 27 i svansen av en partition. Kroppen är
//
//   [i32 1] [ptr 185 eller null] [i32 0] [3 × f64] [6 × f64] [3 × f64]
//
// där klass 185 är ett material som JSON (skip_class185() läser över det).
// Vad flyttalen betyder är inte belagt, så de lämnas ut i filens ordning. Det
// filerna visar: i `.nwd` finns två objekt. Det första har tre nollor, sex
// ettor och sedan tre tal (0,371, 0,743, -0,557) med kvadratsumman 1; det
// andra har tre nollor, sex 0,7 och samma tre tal med omvänt tecken. Klass
// 185 är i båda ett ljus som JSON ("GenericPhotometricLight").
struct PartitionLight {
    int32_t version = 0;      // 1 i alla exempel
    double values[12] = {};
};

// Svansen i en partition (klass 32), efter attributen, och i trädet efter
// barnen:
//
//   [i32 2] [i32 n] n × PTR(27)
//   [3 × f64 färg] [i32 0] [str loader]
//   [i32 format] [i32 enhet] [i32 1]
//   [3 × f64 a] [3 × f64 b]
//   [i32 0 eller 1] [str text eller null] [i32 1] [str källsökväg]
//   [3 × f64 c] [4 × f64 d]
//   [i32 1] nollor upp till en multipel av 8
//
// PTR(27) är ett nytt objekt av klass 27 eller en bakåtpekare till ett.
// Alla flyttal och alla heltal som inte är markörer lämnas ut som de ligger;
// markörerna (2, 0, 1 och 1 ovan) ska ha det värdet.
struct PartitionTail {
    // Klass 27-objekten, som index i `lights` hos den som läser (en
    // bakåtpekare ger samma index som objektet fick).
    std::vector<uint32_t> light_refs;
    // Okänt. 0, 0, 0 i en `.nwc`. I en `.nwd` är det 0,3, 0,3, 0,3 i
    // dokumentets egen partition och i den första partitionen i trädet, och
    // 0, 0, 0 i de andra partitionerna i trädet.
    double colour[3] = {};
    // Namnet på inläsaren: "LcNwcLoaderPlugin:…" i en `.nwc`, ett annat
    // namn i dokumentets egen partition. Tom om strängen är null.
    std::string loader;
    // Formatversionen; 448 i alla exempel, samma tal som i sektionsrubriken.
    int32_t format_version = 0;
    // Längdenheten för modellens koordinater, en kod som metres_per_unit()
    // översätter. 2 eller 0 i `.nwc`-exemplen, 2 i `.nwd`.
    int32_t unit_code = 0;
    // Okänt. (0, 0, 1) i alla exempel.
    double a[3] = {};
    // Okänt. (0, 0, 0) i `.nwc`-exemplen, (0, 1, 0) i `.nwd`: dokumentets
    // egen partition och varje partition i trädet har samma värde.
    double b[3] = {};
    // En text före källsökvägen; tom om den saknas. Betydelsen är inte känd.
    std::string label;
    // Sökvägen till källfilen så som den står. Tom om strängen är null.
    std::string source_path;
    // Okänt. (0, 0, 0) i `.nwc`-exemplen, i `.nwd` (0, 1, 2,2e-16) i dokumentets
    // egen partition och i varje partition i trädet.
    double c[3] = {};
    // Okänt. (1, 1, 0, 0) i `.nwc`-exemplen. I `.nwd` är d[0] < d[2] och
    // d[1] < d[3], som hörnen i en rektangel (min x, min y, max x, max y): ett
    // eget värde för varje partition i trädet, och dokumentets egen partition
    // och den första partitionen i trädet har samma.
    double d[4] = {};
};

// Längdenheten i en partitionssvans (PartitionTail::unit_code) som meter per
// enhet. Belagt: 0 = meter, 1 = centimeter, 2 = millimeter, 3 = fot. Koden 1
// i formatversion 451: klass 39 på en partition med kod 1 under en med kod 2
// har skalan 10, och modellerna i LcOpClashElement har skalan 0,01 / 0,3048
// mot en fil med kod 3. Ger true och sätter `out` för en belagd kod; för en
// annan kod ger den false och lämnar `out` orörd.
bool metres_per_unit(int32_t unit_code, double& out);

// Läser svansen ur en pågående objektström, så att id-räkningen fortsätter
// där noden slutade. Klass 27-objekt som läses läggs sist i `lights`; en
// bakåtpekare ska peka på ett objekt som redan ligger där. Läsarens fel
// fastnar som vanligt.
bool read_partition_tail(ObjectReader& r, std::vector<PartitionLight>& lights,
                         PartitionTail& out);

// Det en partition bär när den ligger som egen ström.
struct Partition {
    // Nodens namn: modellens filnamn i en `.nwc`, dokumentets i en `.nwd`.
    // Tom om strängen är null.
    std::string name;
    bool has_user_name = false;      // nodens namnpar (klass 52) finns
    std::string user_display;        // "File" i alla exempel
    std::string user_internal;       // "LcOaPartition" i alla exempel
    int32_t flags = 0;               // 0 i alla exempel
    int32_t element = 0;             // X: index i GUID-tabellen + 1, eller 0
    uint32_t attribute_count = 0;    // Y: antalet attribut, alla klass 31 (material)
    std::vector<PartitionLight> lights;   // klass 27-objekten, i läsordning
    PartitionTail tail;
};

// Tolkar paketet LcOpNwdPartition, en fristående objektström:
//
//   [100][32] [i32 0] [str namn] PTR(52) [i32 flags] [i32 X] [i32 Y] Y × PTR(31) SVANS
//
// där svansen slutar på [i32 1] och nollor upp till 8, och strömmen slutar
// där. Id räknas från 100, utan underförstådd rot.
// Svansen står vid PartitionTail. Ett attribut är alltid ett nytt objekt av
// klass 31 ([str][15 × f64]).
//
// `out` ersätts. CorruptObject: brott mot grammatiken, med byten i `detail`
// ("LcOpNwdPartition at byte <n>: <regel>"). Unsupported: ett attribut som
// inte är klass 31, eller en tom attributplats (den tar en egenskapsrot, och
// strömmen har ingen).
ErrorCode parse_partition_stream(const uint8_t* data, size_t size, Partition& out,
                                 std::string* detail = nullptr);

} // namespace HNwd::detail
