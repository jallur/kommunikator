# SECOND_OPINION.md — arkitektur- och flödesreview

Granskning av Kommunikator (relay i `src/app/`, firmware i `firmware/src/main.cpp`), med fokus på arkitektur, meddelandeflöde och eventhantering. Gjord 2026-10-17 på branch `cursor/kommunikator-m5-voice-be46`.

## Helhetsbild

Arkitekturen är sund och ovanligt väl dokumenterad. Tre nivåer med tydliga ansvar:

- **Stickan** äger knappar, ljud-in/ut och skärm. Den känner bara till relayn och en enhetstoken.
- **Relayn** äger API-nyckeln, översätter xAI:s händelser till ett litet enhetsprotokoll och gör turn-bokföringen.
- **xAI** nås bara från relayn, med hårdlåst URL (`settings.py` vägrar annan värd/path), vilket är ett bra skydd mot att nyckeln läcker till en gissad endpoint.

Styrkor värda att nämna:

- Protokollet mellan sticka och relay är litet och väldefinierat (JSON för händelser, binärt för PCM, `turn`-stämpling på allt nedåtgående). Det gör att stickan kan vara dum och relayn smart.
- Turn-id genereras av *enheten* och ekas tillbaka — rätt val, eftersom det är enheten som vet när knappen trycks. Relayn validerar (`_turn_id`) och stickan filtrerar status på sitt eget `turnId`.
- `xai_lock` i `bridge.py` serialiserar clear/cancel/commit/response.create mot ljudframes, med en bra kommentar om *varför* (en sen `response.create` får inte landa efter att nästa tur rensat bufferten). "Adoption"-logiken för spontant öppnade svar är genomtänkt.
- Dependency injection av `XaiFactory` gör relayn testbar utan nät, och testerna mockar xAI.
- Kända buggar (summern spelar om svansen när AirPods släpper) är dokumenterade som kända, inte gömda.

Den största strukturella svagheten är att **relayn återanvänder ett enda muterbart `_Turn`-objekt över alla turer** och att **xAI:s transkript-händelser inte korreleras mot någon tur**. Det är roten till de subtilaste felen nedan. Firmware-sidan är en 2500-raders fil med mycket global state — fungerar, men är på gränsen för vad som går att resonera om; en uppdelning (ljud, BT, WS, UI) vore nästa naturliga refactor.

## Meddelandeflödet, som det faktiskt ser ut i koden

Uppåt: knapp A → `ptt down` (JSON, med turn) → relayn skickar `response.cancel` (om svar pågår) + `input_audio_buffer.clear` → binära PCM-frames strömmas direkt → `ptt up` → `input_audio_buffer.commit` → relayn väntar ≤2,5 s på färdigt användartranskript → `response.create` med citatet, *eller* adopterar ett svar som xAI redan öppnat.

Nedåt: `response.created` → `status thinking` → första ljudramen föregås alltid av `status speaking` (stickan kräver den ordningen) → PCM i ≤3200-bytebitar (under WebSockets-bibliotekets 15 KiB-gräns) → `response.done` → `status idle`. Ljud utan aktiv `playing_id` släpps. Detta stämmer med DESIGN.md.

## Buggar och risker, viktigast först

### 1. Transkript-händelser korreleras inte med turen — fel citat kan skickas

`user_transcript()` i `protocol.py` och hanteringen i `_xai_to_device` (bridge.py:456–462) tar *varje* `input_audio_transcription.completed` och sätter `turn.user_transcript` + `transcript_ready`, oavsett vilken tur/item den hör till. Scenario: användaren trycker, släpper (commit A), trycker snabbt igen och släpper (commit B). Transkriptet för A kan vara i flykt och landa *efter* att ptt up för B rensat `user_transcript` (bridge.py:400–401). Då sätts `transcript_ready` med A:s text, och `_answer_after_transcript` för tur B citerar fel yttrande — exakt det problem hela citat-mekanismen finns för att undvika. xAI:s completed-event bör bära ett `item_id`; spara det id som hör till senaste commit och ignorera completed för andra. Samma gäller `transcript.user` till skärmen: den stämplas med *aktuell* turn-label, inte den tur ljudet hörde till.

### 2. `POST /v1/missions` returnerar 501 även vid framgång

`main.py:117` sätter `status_code=501` på routen. I dag räddas det av att `dispatch()` alltid kastar `MissionNotConfigured`, men den dag webhooken kopplas in kommer success-vägen (`return {"status": "accepted"}`) att svara **501 med kroppen "accepted"**. Sätt routen till 200/202 och låt undantaget ensamt ge 501 — då försvinner fällan innan någon kliver i den.

### 3. `response.done` nollar `response_active` villkorslöst

bridge.py:471–472. Efter `response.cancel` (ptt down) kan den gamla responsens `response.done` anlända *efter* att en ny respons skapats. Då sätts `response_active = False` fast ett svar faktiskt strömmar. Konsekvens: nästa ptt down skickar ingen `response.cancel`, så gammalt svar fortsätter generera på xAI-sidan (ljudet filtreras visserligen bort av `playing_id`, men xAI-tid och tokens bränns, och adoptions-grenen i `_answer_after_transcript` kan välja fel väg eftersom `turn.response_active` ljuger). Matcha `response.done` mot responsens id, eller ignorera done för en respons som redan avbrutits.

### 4. `hearTurn` i firmware nollas aldrig vid tur-slut

`hearTurn` sätts på `status speaking` för rätt tur (main.cpp:1333) men rensas bara i `beginPtt`. Ljudgrinden är `!pttHeld && hearTurn` (main.cpp:1394) **utan turn-koll på de binära ramarna** — binära frames bär inget turn-id. Om relayn efter idle skickar ljud som hör till en annan/tidigare tur (t.ex. race kring `playing_id` vid snabb dubbeltryckning) spelas det upp som om det hörde till den synliga turen. I praktiken skyddar relayns `playing_id`-filtrering, men stickan litar här blint på att relayn aldrig gör fel. Billig härdning: nollställ `hearTurn` på `status idle`/`error` för aktuell tur.

### 5. Ptt up raderar redan mottagna live-partials

bridge.py:400: `turn.user_transcript = ""` vid ptt up slänger de kumulativa `updated`-partials som strömmat in under inspelningen. Om `completed` sedan uteblir (timeout 2,5 s) skickas `response.create` **utan citat** trots att relayn hade en nästan komplett text. Behåll senaste partial som fallback-citat vid timeout — det ligger närmare syftet ("svara på det yttrandet") än inget citat alls.

### 6. Odokumenterad skillnad: WS-handshaken accepteras före tokenkontroll

`main.py:90–101` kör `socket.accept()` först och verifierar token efteråt (svar + close 1008). Det är ett medvetet mönster (enheten får ett läsbart fel), men det betyder att vem som helst får en fullbordad 101-handshake och en accepterad socket innan avvisning. DESIGN.md säger "Ogiltig token stänger med 1008 innan någon xAI-socket öppnas" — det stämmer, men för en internetexponerad endpoint vore det värt att även nämna/överväga avslag på HTTP-nivå (403 före upgrade) som proxyn eller Starlette kan göra.

## Mindre observationer

- **`relay/.env` är föräldralös.** Compose läser `.env` i repo-roten; `relay/.env` används inte av något och innehåller riktiga värden. Den är inte git-trackad (bra), men bör tas bort eller flyttas så det inte finns två sanningar om var nyckeln bor.
- **Stale kommentar i `config.h`:** "Plus2 asks for 44.1 kHz because that is what its AirPods sink plays" — profilen i `board.h` säger 16000/16000, och DESIGN.md förklarar varför. Kommentaren beskriver ett tidigare läge och lurar nästa läsare.
- **Dött villkor i firmware:** `onTextMessage` för `transcript.user` (main.cpp:1338–1339) sätter `userLine = ""` och gör sedan `if (doc["cumulative"] | true) userLine = ""` — andra raden är verkningslös. Harmlöst men förvirrande.
- **`ConversationMemory` städar bara vid läsning.** Utgångna poster ligger kvar tills samma token frågar igen. Begränsat av antalet tokens, så ingen praktisk läcka, men en sweep (eller max-storlek) kostar tre rader.
- **`xai_lock` hålls under `link.send(pcm)`** (bridge.py:344–349). Stallar xAI-socketen blockeras även ptt-hanteringen (clear/cancel) tills sändningen släpper. Medveten backpressure kanske, men värt att veta: en seg xAI-socket gör knappen "död".
- **Ett `_Turn`-objekt för alla turer.** Fälten återställs manuellt på ptt down (13 rader tillståndsnollning i bridge.py:363–384) — varje nytt fält måste kommas ihåg där. Ett nytt `_Turn` per tryck, med `turn.id` som enda kontinuitet, skulle eliminera en hel klass av "glömd reset"-buggar (punkt 1 och 3 ovan är släktingar till detta).
- **Resamplern används i praktiken bara i kopia-läget** (16 k → 16 k i båda riktningar). Interpolationsvägen ser korrekt ut vid genomläsning och har tester, men den är ren Python per sample — om `XAI_INPUT_RATE` någon gång avviker från enhetens takt blir det en CPU-kostnad per 20 ms-block. Inget att åtgärda nu, bara en känd gräns.
- **Firmware-filen är 2549 rader** med ~80 globala variabler och tre samtidiga exekveringskontexter (loop-tasken, mic-tasken, BT-callbacken) som delar ringbuffertar. Låsdisciplinen (`stMux`/`audioMux`/`micMux` + `memw`-barriärer) ser korrekt ut där jag tittat, men varje ny funktion höjer risken. En uppdelning i moduler vore värd mer än någon enskild fix ovan.

## Sammanfattning

Designen är rätt: tunn enhet, smart relay, nyckeln stannar på servern, och turn-stämplingen löser rätt problem. De verkliga svagheterna är koncentrerade till en punkt — **korrelation av asynkrona xAI-händelser mot rätt tur/respons** (punkt 1, 3, 4). Jag skulle åtgärda punkt 2 (ren latent bugg, enradsfix) och punkt 1 (den enda som kan ge fel *svar* till användaren) först, och ta `_Turn`-per-tryck-refactorn i samma veva eftersom den gör 3 och 4 nästan gratis.
