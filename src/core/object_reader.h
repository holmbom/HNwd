#pragma once
#include "byte_reader.h"
#include "nwd_error.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

namespace HNwd::detail {

// Djupast tillåtna nästling av objekt i objekt.
inline constexpr int kMaxNesting = 256;

// Läser en objektström: trädet, fragmenten, egenskaperna och partitionen är
// alla ett nät av objekt som pekar på varandra.
//
// En pekarplats är
//
//   [0]                    null
//   [1][id]                bakåtpekare till ett objekt vars kropp är FÄRDIGLÄST
//   [nästa id][klass]…     ett nytt objekt, följt av dess kropp
//
// Id räknas från 100 och gäller per ström. Ett objekt får sitt id innan
// kroppen läses, så objekt i kroppen får de följande numren, djupet först.
// Allt annat är fel: ett annat första tal, en klass som platsen inte tar emot,
// en bakåtpekare till ett okänt id eller till ett objekt som ännu läses.
//
// Felet fastnar, som i ByteReader: efter det första felet svarar varje anrop
// false, och code() och detail() beskriver det första.
//
// Läsaren går antingen över en buffert eller genom ett StreamWindow. Genom ett
// fönster är positionerna absoluta i strömmen, PAD8 räknas därifrån, och
// reglerna, felen och detail() är desamma. Två skillnader följer av att
// strömmens längd inte är känd förrän den är läst: count() prövar antalet mot
// strömmens längd om slutet är nått och annars mot strömmens tak, och ett fel
// i själva strömmen (trasig zlib, taket nått) ger strömmens felkod med dess
// text efter "<what> at byte <n>: ".
//
// Så läses en plats:
//
//   ObjectReader::Ptr p;
//   if (!r.ptr(p, {52}, false, true)) return false;
//   if (p.kind == ObjectReader::Ptr::New) {
//       … läs kroppen med r …
//       if (!r.finish(p.id, index_i_min_tabell)) return false;
//   }
//   // p.value är nu det värde objektet färdigställdes med, också för en
//   // bakåtpekare (fyll i det själv för New).
class ObjectReader {
public:
    // `what` är paketets namn och inleder detail(). Pekaren ska gälla så länge
    // läsaren gör det.
    ObjectReader(const uint8_t* data, size_t size, const char* what);
    // Genom ett fönster, från den position där fönstret började. Fönstret ska
    // gälla så länge läsaren gör det.
    ObjectReader(StreamWindow& window, const char* what);

    bool ok() const { return ok_; }
    size_t pos() const { return reader_.pos(); }
    size_t size() const { return reader_.size(); }
    int32_t next_id() const { return next_id_; }

    // CorruptObject, eller LimitExceeded för nästlingsdjupet; genom ett fönster
    // också strömmens fel (CorruptStream, StreamTooLarge, OutOfRange). Ok om
    // inget fel.
    ErrorCode code() const { return code_; }
    // "<what> at byte <n>: <regel>" för det första felet, annars tom.
    const std::string& detail() const { return detail_; }

    // Markerar strömmen som feltolkad vid den aktuella positionen. Regeln är
    // en kort engelsk text. Returnerar alltid false.
    bool fail(const char* rule);
    // Som fail(), men med LimitExceeded: datan går över ett tak.
    bool fail_limit(const char* rule);

    bool i32(int32_t& v);
    bool f32(float& v);
    // 64-bitarsvärden ligger på jämn 8 byte från strömmens början: står
    // markören på 4 mod 8 kommer först en int32 som ska vara 0.
    bool f64(double& v);
    bool i64(int64_t& v);

    // Sträng: [int32 längd][byte][nollor upp till 4]; -1 är null. Byten lämnas
    // ut som de ligger. En sträng som inte är giltig UTF-8 är inget fel, men
    // räknas.
    bool str(std::string& s, bool* is_null = nullptr);
    size_t non_utf8_strings() const { return non_utf8_; }

    // Hoppar över `len` byte och nollorna upp till 4.
    bool skip_padded(int64_t len);

    // Ett antal som inleder en lista. Varje post tar minst
    // `min_bytes_per_item` byte, så ett antal som inte ryms i resten av
    // strömmen är fel. Genom ett fönster är "resten" bara en övre gräns (se
    // klasskommentaren): ett prövat antal kan då ändå vara gigabyte större än
    // det strömmen har. Därför gäller för varje tal ur strömmen — ett antal,
    // en längd — att ingenting reserveras, ändras i storlek eller fylls efter
    // talet på en väg som kan gå genom ett fönster. Tabellen växer med det
    // som faktiskt läses (push_back, eller i steg med en fast övre gräns per
    // steg), så att minnet följer strömmens verkliga innehåll.
    bool count(uint32_t min_bytes_per_item, int32_t& n);

    // Den underförstådda roten: [nästa id] utan klass och kropp. Den räknas
    // som färdig direkt.
    bool implicit_root();

    // Nollor upp till 8 byte, och därefter ska strömmen vara slut.
    bool end();

    struct Ptr {
        enum Kind : uint8_t { Null, Back, New } kind = Null;
        int32_t id = 0;       // objektets id; 0 för null
        int32_t cls = 0;      // objektets klass; 0 för null
        uint32_t value = 0;   // det värde objektet färdigställdes med; 0 för New
    };

    // Läser en pekarplats som bara tar emot klasserna i `allowed`. För ett
    // nytt objekt ska anroparen läsa kroppen och sedan anropa finish().
    bool ptr(Ptr& out, std::initializer_list<int32_t> allowed, bool allow_null,
             bool allow_back);

    // Objektet `id` är färdigläst och går nu att peka bakåt på. `value` är
    // anroparens eget, till exempel ett index i en tabell.
    bool finish(int32_t id, uint32_t value);

    // Slår upp ett färdigt objekt före läsarens första id: klass och värde, eller
    // false om id:t är okänt.
    using Resolver = std::function<bool(int32_t id, int32_t& cls, uint32_t& value)>;

    // Läsaren börjar mitt i strömmen: nästa id är `next_id`, och en bakåtpekare
    // till ett id under det slås upp genom `resolve` i stället för i läsarens
    // egen tabell. Anropas innan något läses. Utan anropet börjar id på 100 och
    // ingenting slås upp utanför läsaren.
    void continue_ids(int32_t next_id, Resolver resolve);

    // Byte per id i läsarens tabell över objekten.
    static constexpr size_t kIdEntryBytes = 8;

private:
    // En post per id: 8 byte. Klassen ryms i 8 bitar, eftersom bara klasser
    // som en plats tar emot sparas, och de är alla under 256.
    struct Slot {
        uint32_t value = 0;
        uint8_t cls = 0;
        bool finished = false;
    };
    static_assert(sizeof(Slot) == kIdEntryBytes);

    // fail() med en egen position och felkod.
    bool fail_at(size_t pos, ErrorCode code, const std::string& rule);
    // För lite data: strömmens eget fel om fönstret har ett, annars `rule`.
    bool short_data(size_t pos, const char* rule);

    ByteReader reader_;
    const char* what_;
    std::vector<Slot> slots_;   // index = id - base_id_
    int32_t base_id_ = 100;     // första id i slots_
    int32_t next_id_ = 100;
    Resolver resolve_;          // id under base_id_
    int depth_ = 0;
    size_t non_utf8_ = 0;
    bool ok_ = true;
    ErrorCode code_ = ErrorCode::Ok;
    std::string detail_;
};

// Strikt UTF-8: inga överlånga former, inga surrogat, inget över U+10FFFF.
bool valid_utf8(const uint8_t* data, size_t size);

// Läser över kroppen i ett objekt av klass 185 (ett material som JSON med
// inbäddade filer), som både trädet och fragmenten kan peka på:
//
//   [str][str][i32 n] n × ( [str][i32 e][str]  och om e == 1: [str][i64 len][len byte, nollor upp till 4] )
//
// e ska vara 0 eller 1.
bool skip_class185(ObjectReader& r);

} // namespace HNwd::detail
