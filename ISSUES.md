# Problem

Baslinjen är det som ligger på `main` efter merge av röstvägen för StickC Plus2. Angrip en punkt i taget. Ändra inte en löst punkt för att laga en öppen, om inte loggen visar att den lösta punkten är orsaken.

Relayn behöver inte byggas om för en ren firmwareändring. `platformio.ini` ligger i `firmware/`. Monitorn är 115200. 9600 ger `��`.

```
cd firmware
python -m platformio run -e m5stick-c-plus2 -t upload
python -m platformio device monitor -b 115200
```

## Öppna

### 1. Summern berättar historien en gång till

När AirPods spelar dräneras inte monoringen. Den är summerkopian, 256 KB. `tapM` står på 0 under svaret och `mono` växer. På `bt slapp` startar summern och spelar det som ligger kvar, och läget kan gå tillbaka till `SVARAR` långt efter att texten är klar.

Det här lämnades med flit. En tömning av båda ringarna på `bt slapp` sammanföll med att nästa svar bara hördes som slutet i lurarna. Tömningen hade inte kört i den loggen (`bt slapp` saknades), men den är tillbakatagen. Stereoringen töms fortfarande när länken släpper, så lurarna inte tar om slutet när de kommer tillbaka.

Förväntat tills detta är lagat: efter `bt slapp` är `mono` inte 0, och summern kan tala.

### 2. Bara slutet i lurarna, inte bekräftat på enheten

Senaste lyssningen, före `e97b2c3`: `bt ljud 2` kom vid `bt kopplad` och slogs genast av med `bt media paus` och `bt ljud 1`. Under hela `SVARAR` stod `ut` på 0. Stereoringen slog i taket 131071 och `tapS` räknade upp (249521 i den loggen), så början skrevs över. `bt ljud 2` kom först efter `status idle`. Det som spelades var för-rullen plus det som fick plats kvar i ringen.

`e97b2c3` lämnar A2DP-strömmen uppe så länge lurarna är kopplade. Tystnad håller den vid liv. Spärren släpper riktiga samples när en sekund finns buffrad. Det är inte kört på stickan än.

Kolla efter flash: efter `bt kopplad` ska `bt ljud 2` stanna, utan `bt media paus` före svaret. Under `SVARAR` ska `ut` räknas upp och `tapS` stanna på 0. Hela historien ska höras från början.

Risk med strömmen uppe under inspelning: upplänken eller socketen kan tappa fart. `mic tot` ska fortfarande täcka hela frågan, och socketen ska inte gå till `DISCONNECTED` mitt i svaret.

### 3. Åäö blir a, o eller punkt

Skärmens Font2 saknar åäö. `foldUtf8` viker å/ä till a och ö till o. Protokollet behåller UTF-8. Serieloggen går genom `copyPrintable`, som byter varje byte över 126 mot `.`. "Berätta" syns som `Ber..tta`. Det är loggen, inte att relayn tappade tecknen.

### 4. Mikrofonen ligger i taket

`mic tot`-raden visar `niva` 32751 eller 32752. Det är PDM-klippet. Talet har ändå transkriberats. Orörd, eftersom den inte stoppade turen. Om starkt tal låter sprucket i svaret är det här första stället att titta.

### 5. AirPods är långsamma när de är av vid start

Bondade lurar svarar på anrop, inte på inquiry. Firmware anropar den sparade adressen var tredje sekund (`bt anrop`) och söker först efter sex missade hjärtslag. Är lurarna av fylls loggen med `bt anrop`, `bt lage 1` och `bt lage 0`. Det är väntat. Ta inte bort bindningen för att det tar tid.

### 6. I2S port 1 has not installed

`E (502) I2S: i2s_driver_uninstall(2048): I2S port 1 has not installed` kommer vid start. Den har inte hängt ihop med tystnad eller avbrott. Jaga den inte som orsak till ett ljudfel.

## Lösta

De här har hörts på stickan, utom där det står annat. En ny ändring ska inte ta tillbaka beteendet i högerkolumnen.

| Vad som hände | Vad som gäller nu |
| --- | --- |
| Mikrofonen skickade en enda 128-byte-ram och xAI fick inget att transkribera. `M5.Mic.record` såg färdig ut för tidigt. | Plus2 läser SPM1423 med egen PDM-DMA. En hållen knapp ska ge `mic tot` för hela trycket. |
| AirPods behövde paras om efter varje start. | Senaste adressen återansluts (`34:31:8f:e9:fb:cd` på den här stickan). Just Works, utan ja/nej-knapp. |
| Svaret tystnade ungefär en sekund och fortsatte sedan. A2DP pausades varje gång bufferten dök under startstrecket. | Spärren ligger kvar för hela svaret. Den släpper när kön är tom och sista paketet är äldre än 600 ms, eller när PTT trycks. |
| Inget hördes före "två". SBC slängde starten av en ny ström. | 400 ms tystnad (17640 ramar vid 44,1 kHz) skickas när spärren slår till. |
| Skämtet klipptes och socketen dog med `DISCONNECTED kod 0` medan lurarna satt kvar. `kod 0` och tom orsak är ett lokalt tapp, inte en stängningsram från servern. | Uppspelning kör `ESP_COEX_PREFER_BALANCE`. Plus2 har `WEBSOCKETS_TCP_TIMEOUT` 5000. Wi-Fi-sömn ska vara på, annars avbryter IDF när BT och Wi-Fi är uppe samtidigt. `PREFER_BT` under svaret svalt socketen. |
| Lurarna föll när knappen hölls nere. `PREFER_WIFI` under PTT tog radion. | Så länge lurarna är kopplade, även under PTT och väntan, är samexistensen `BALANCE`. |
| Start efter paus gav `bt media start` med ok, men `ut` stod kvar på 0. Biblioteket lämnar medieläget på STARTED efter SUSPEND, och en ny START ignoreras. Historien gick till summern, och stereoslutet spelades när lurarna kom tillbaka. | `kickMedia` ställer läget på STARTING innan START. Stereoringen töms på `bt slapp`. Monokopian är punkt 1. |
| Svaret hackade efter första meningen. `tom` steg, stereo sjönk till ett paket, sedan `DISCONNECTED` med `alder` nära 5000. Wi-Fi bredvid A2DP gav ungefär 32 KB/s, alltså 16 kHz, inte 44,1 kHz. | Plus2 ber om `playback_rate` 16000 och samplar upp till 44100 lokalt. Prim buffert är 44100 stereoramar. En hel historia utan hack är hörd på den takten. Gå inte tillbaka till 44100 på tråden. |
| Sparade lurar svarade inte på inquiry, så första anropet följdes av ett långt `bt sok`. | Anrop var tredje sekund. Inquiry först efter sex missar, längd 4. |

## Så läses en tyst eller kapad tur

- `ut` står stilla och `tapS` växer: lurarna spelar inte, och ringen kastar början. Punkt 2.
- `bt ljud 1` efter `bt media paus`, och `bt ljud 2` kommer först när texten redan är `idle`: samma sak.
- `ws typ DISCONNECTED kod 0` och `alder` nära 5000: huvudloopen satt i läsningen medan radion var upptagen. Samexistens och takt, inte ett serverfel.
- `mono` stort och `tapM` 0 under svaret, sedan tal i summern efter `bt slapp`: punkt 1.
- `spel 16000` ska stå på hello-raden. `44100` där betyder att trådtakten är tillbaka och hacken kommer igen.
