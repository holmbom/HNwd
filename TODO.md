# HNwd — TODO

Avklarat arbete ligger i [doc/CHANGELOG.md](doc/CHANGELOG.md).
Planer och utredningar ligger i [doc/plans/](doc/plans/).
Det som inte är avkodat i filformatet, och bibliotekets kända svagheter, står i
[LIMITATIONS.md](LIMITATIONS.md): det är inte planerat arbete.

## Nästa

- [ ] **Visaren med MSVC.** Bibliotek, tester och verktyg byggs med MSVC
      19.51 utan varningar på `/W4`, och sviten går igenom; visarens egna
      filer kompilerar rent. GlazeUI byggs inte med MSVC (makrona `min`/`max`
      ur `windows.h` och `file_clock::to_sys` i dess källor), så `nwdview`
      länkas inte där.
- [ ] **Prov på Linux och macOS.** Koden för dem är skriven utan att ha körts;
      [doc/os/linux/TESTS.md](doc/os/linux/TESTS.md) och
      [doc/os/macos/TESTS.md](doc/os/macos/TESTS.md) säger vad som ska provas.

## Minne och fart på stora filer

- [ ] **Baklängesläsning av egenskaper** i skelettläget kostar millisekunder
      per nod; i stigande ordning är den nära linjär.
- [ ] **Visaren:** hur bildens skala, vridning och förskjutning läggs på
      UV-värdena är antaget, liksom vilken sida av ett snittplan som
      behålls; genomskinliga ytor sorteras inte; textens vridning används
      inte; gångläget har ingen kollision. Ritprogrammen är bara provade med
      Direct3D.

## Senare

- [ ] **Visaren: tröskeln i mycket stora modeller.** När grafikminnet inte
      räcker för 3 px höjs tröskeln. Två vägar är inte gjorda: att inte bygga
      det som är skymt bakom stora ytor, och att rita upprepad geometri som
      instanser (`viewer/instancing.*` planerar grupperna men är inte
      inkopplad i fönstret; instanser får inte visarens ritprogram och
      klipps inte av snittplan).
- [ ] **Visaren: det ramverket sätter gränsen för.** En triangel kostar
      cirka 32 byte på grafikkortet (hörn om 20 byte och 32-bitars index) och
      mer i vanligt minne, där meshen behåller sin kopia efter uppladdningen;
      med 2–4 GB grafikminne höjs tröskeln i täta modeller. Ramverkets
      plockning bygger sökträd i tråden som frågar, så visaren träffprovar
      själv. Djupet har nära som 0 och långt bort som 1, så ytor nära
      varandra flimrar på avstånd när närplanet är litet.
- [ ] **Visaren: det som inte är provat i bild.** Linjernas utseende
      (bredden, och förskjutningen som låter dem synas ovanpå ytor i samma
      plan), och gången (`--walk`) i en stor fil efter att urvalet flyttats
      till en egen tråd.
- [ ] **Visaren: stopp i UI-tråden som är kvar.** Kön som lägger in och tar
      ut bitar har en tidsbudget per pumpvarv, men en enskild operation kan gå
      över den: enstaka stopp på 5–25 ms är mätta, och vilken operation det är
      är inte utrett. Ett byte av snitt sätts i portioner, men kön byggs och
      texterna uppdateras i ett svep över alla byggda bitar. En ny fil tömmer
      lagren i UI-tråden; mesharna frigörs i en arbetstråd.
- [ ] **Visaren `nwdview`** har inget träd över objekten, ingen sökning och
      kan inte dölja enskilda objekt, bara hela modeller.
- [ ] **IFC GlobalId ur en GUID.**
- [ ] **Färdigbyggda paket.** Repot har ingen automatik för utgåvor än:
      ett arbetsflöde som bygger paketen per verktygskedja vid varje tagg.
- [ ] **Användning i ett program som bygger på biblioteket.**
