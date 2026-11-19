#pragma once
#include "nwd_error.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace HNwd::detail {

// Förvalt tak för en uppackad ström. En trasig eller illasinnad ström ska ge
// ett fel, inte ta slut på minnet.
inline constexpr size_t kDefaultMaxStreamBytes = size_t(2) << 30;

// Adler-32 över `size` byte, fortsatt från `adler` (1 för en ny summa).
uint32_t adler32(const uint8_t* data, size_t size, uint32_t adler = 1);

// Packar upp EN zlib-ström (RFC 1950 runt RFC 1951) som börjar på `src`.
//
// `out` ersätts med den uppackade datan. `consumed` får strömmens exakta längd
// i byte, kontrollsumman inräknad — strömmar ligger tätt efter varandra i
// filen, och nästa börjar där den här slutar.
//
// CorruptStream: fel i huvudet, i blocken eller i kontrollsumman, eller en
//                ström som tar slut före sitt sista block.
// StreamTooLarge: den uppackade datan blir större än `max_output`.
//
// Vid fel är innehållet i `out` och `consumed` odefinierat.
ErrorCode inflate_zlib(const uint8_t* src, size_t src_size,
                       std::vector<uint8_t>& out,
                       size_t max_output = kDefaultMaxStreamBytes,
                       size_t* consumed = nullptr);

// Packar upp EN zlib-ström bit för bit, med samma regler och samma fel som
// inflate_zlib, utan att hela den uppackade datan finns i minnet på en gång.
// Klassen håller de senaste 32 KB utdata och en buffert på några hundra KB.
//
// Den komprimerade strömmen ska ligga i minnet, från `src` och högst `size`
// byte; den får sluta före `size` (consumed() säger var).
//
//   InflateStream z;
//   if (z.open(src, size, max) != ErrorCode::Ok) …
//   for (;;) {
//       size_t got;
//       if (z.read(buf, n, got, &detail) != ErrorCode::Ok) …   // fel
//       if (got == 0) break;                                   // slut, prövat
//       …
//   }
//
// Fel fastnar: efter det första felet ger read() och skip() samma fel igen.
// open() och resume() börjar om och släpper ett tidigare fel.
//
// Adler-32. Ett pass som går från open() till strömmens slut utan resume()
// prövar kontrollsumman, och slutet (read() med got 0) rapporteras först när
// den stämt. Efter resume() prövas den inte — summan beror på all data före
// kontrollpunkten — men kontrollsummans fyra byte ska ändå finnas. Det som
// läses om efter en resume() är därför prövat bara om samma ström en gång
// lästs igenom hela, i ett sådant pass.
//
// Kontrollpunkter sparas vid blockgränser, och bara under ett pass från
// open(): efter resume() sparas inga. En kontrollpunkt bär utdatans position,
// bitläget i indata och de senaste 32 KB utdata (färre i strömmens början),
// som är allt som behövs för att fortsätta därifrån.
class InflateStream {
public:
    // Läget vid en blockgräns.
    struct Checkpoint {
        uint64_t out_pos = 0;          // utdata före gränsen
        uint64_t in_bit = 0;           // bit i `src` där nästa blockhuvud börjar
        std::vector<uint8_t> window;   // de min(out_pos, 32768) sista byten utdata
    };

    InflateStream();
    ~InflateStream();
    InflateStream(const InflateStream&) = delete;
    InflateStream& operator=(const InflateStream&) = delete;

    // Börjar om på en ny ström: prövar zlib-huvudet. CorruptStream om `src`
    // är null, kortare än 6 byte eller huvudet är fel. Felet fastnar också för
    // read() och skip(). Inspelningen av kontrollpunkter stängs av.
    ErrorCode open(const uint8_t* src, size_t size, uint64_t max_output);

    // Upp till `n` byte (n > 0) till `dst`. `got` är `n` utom vid strömmens
    // slut, där det är det som återstod; ett anrop med got 0 och Ok betyder
    // att strömmen är slut (och, i ett pass från open(), att Adler-32 stämt).
    // Ett got under `n` med Ok kommer alltså bara när slutet redan är prövat.
    // CorruptStream eller StreamTooLarge som i inflate_zlib; `got` säger då
    // hur många byte som ändå skrevs.
    ErrorCode read(uint8_t* dst, size_t n, size_t& got, std::string* detail = nullptr);

    // Antalet byte utdata före nästa byte som read() lämnar.
    uint64_t position() const;

    // Sparar en kontrollpunkt i `sink` vid varje blockgräns som ligger minst
    // `every_bytes` utdata efter den förra (den första efter minst
    // `every_bytes`). nullptr stänger av. Gäller bara i ett pass från open().
    void record_checkpoints(uint64_t every_bytes, std::vector<Checkpoint>* sink);

    // Fortsätter från en kontrollpunkt i den ström som senast öppnades med
    // open(). position() blir cp.out_pos. OutOfRange om kontrollpunkten inte
    // kan höra till strömmen: bitläget utanför `src`, utdatan över taket
    // eller ett fönster av fel storlek. CorruptStream om ingen ström är öppen.
    ErrorCode resume(const Checkpoint& cp);

    // Hoppar över `n` byte utdata, som packas upp men inte lämnas ut.
    // OutOfRange om strömmen slutar först; positionen står då vid slutet.
    // Annars samma fel som read().
    ErrorCode skip(uint64_t n, std::string* detail = nullptr);

    // Strömmen är läst till sitt slut utan fel.
    bool finished() const;
    // Strömmens längd i `src`, kontrollsumman inräknad. Gäller när finished().
    size_t consumed() const;
    // Taket som gavs till open().
    uint64_t max_output() const;

private:
    struct State;
    std::unique_ptr<State> s_;
};

} // namespace HNwd::detail
