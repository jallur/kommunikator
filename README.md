# Kommunikator

Röst-AI på en M5StickC Plus2 eller en M5Stack CoreS3 (SE eller Lite), med en relay framför xAI:s realtime-API (Grok Voice). Plus2 har push-to-talk, transkript på skärmen och uppspelning i den inbyggda summern. Hat SPK2 är valfri och av från start. CoreS3 har samma samtal på pekskärmen, med PTT, inställningar och kamera som knappar. API-nyckeln ligger bara i relayn.

Arkitekturen, källorna och öppna frågor finns i [DESIGN.md](DESIGN.md).

Relayn körs med Docker på cloud01, i `/home/grok/kommunikator`, byggd från `./src`. Jonas arbetsdator är inte värd.

## Hårdvara

| Del | Kommentar |
| --- | --- |
| M5StickC Plus2 | ESP32-PICO-V3-02, 8 MB flash, 2 MB PSRAM |
| Skärm | 1.14" ST7789v2, 135×240 |
| Mikrofon | SPM1423, PDM |
| Summer | Passiv, ombord. Standardutgång tills en Hat SPK2 sätts på |
| Hat SPK2 | Valfri senare. MAX98357 I2S, 1 W. Knapp B slår på den |
| Knappar | A PTT, B utgång/portal, C ström |
| M5Stack CoreS3-SE | ESP32-S3, 16 MB flash, 8 MB PSRAM. Samma firmware som Lite |
| CoreS3-skärm | 2.0" 320×240, pekskärm. Texten tar de övre 3×2 rutorna |
| Knappar på glaset | PTT, INST, KAM. SE har ingen kamera, knappen säger det |
| Mikrofon och högtalare | ES7210, två mickar, AW88298 1 W. Ingen AirPods, ingen Hat SPK2 |

Pinnar från [StickC-Plus2](https://docs.m5stack.com/en/core/StickC-Plus2) och M5:s SPK2-exempel:

| Funktion | GPIO |
| --- | --- |
| HOLD (måste vara hög efter väckning) | 4 |
| Summer | 2 |
| SPM1423 CLK och SPK2 LRCLK | 0 |
| SPM1423 DATA | 34 |
| SPK2 BCLK | 26 |
| SPK2 DIN | 25 |
| Knapp A | 37 |
| Knapp B | 39 |
| Knapp C / väckning | 35 |
| Batteri-ADC | 38 |
| TFT MOSI, CLK, DC, RST, CS, BL | 15, 13, 14, 12, 5, 27 |

GPIO0 delas. Tal och lyssning turas om. Det är avsiktligt och följer PTT.

## Snabbstart, relay

På cloud01, i `/home/grok/kommunikator`. Inte på Jonas arbetsdator. Proxyn och nätet `proxy` finns redan.

```bash
cd /home/grok/kommunikator
cp .env.example .env
# fyll i XAI_API_KEY och DEVICE_TOKENS (en token per enhet, kommaseparerat)
# PORT kan utelämnas. Standard är 8090, lyssning på 0.0.0.0.
docker compose up -d --build
curl -fsS https://trek.aldenback.com/health
```

`docker compose` bygger imagen från `./src`. Tjänsten heter `kommunikator-relay`, läser `.env` och har en healthcheck mot `/health`. Enheten ansluter med `wss://trek.aldenback.com/ws`. Certet ska vara Let's Encrypt. Firmware litar på ISRG Root X1.

`POST /v1/missions` är ett senare gränssnitt mot Grok Bot. Den anropar inget och svarar 501. Se DESIGN.md.

## Snabbstart, firmware

```bash
cd firmware
cp include/secrets.h.example include/secrets.h
# KOMM_RELAY_HOST är redan trek.aldenback.com. Sätt samma KOMM_DEVICE_TOKEN som DEVICE_TOKENS.
python -m platformio run -t upload
python -m platformio device monitor
```

`secrets.h` är gitignorerad.

Wi-Fi kan också sättas på enheten: håll knapp B inne vid start, eller starta utan sparat nät. Anslut till AP `Kommunikator` och öppna `http://192.168.4.1`. Formuläret sparar nät, relay och token i NVS.

Användning: knapp A inne medan du pratar, släpp när du är klar. Pip vid tryck och släpp. Svaret spelas i summern och skrivs på skärmen. Kopplas AirPods går svaret dit i stället, och skärmen visar `AIR`. Knapp B växlar till Hat SPK2 när den finns. Efter tre minuter i vila släcker den skärmen. Lättsömn väntar tills lurarna är frånkopplade. Knapp A väcker.

Öppna fodralet och håll knappen på baksidan tills lampan blinkar vitt, första gången. Sedan räcker det att lurarna är ute ur fodralet. Stickan tar första enheten vars namn innehåller "AirPod".

`airpods-test` är bara tonen, utan röst. Den vanliga uppladdningen är `m5stick-c-plus2`.

CoreS3-SE, CoreS3 och CoreS3 Lite byggs med `cores3`. Samma bild körs på alla tre. PSRAM är quad (`qio_qspi`), som Arduino 2.0.16 sätter för CoreS3.

Håll RST på undersidan inne i tre sekunder tills den gröna lampan tänds, och släpp. Då är enheten i nedladdningsläge. På macOS heter porten `/dev/cu.usbmodem…`. `/dev/cu.Bluetooth-Incoming-Port` är inte CoreS3. Syns ingen `usbmodem` är kabeln ofta bara laddning. Uppladdningen går på 115200. USB-porten på macOS tappar kontakten om farten höjs till 1 500 000 efter att stubben startat, och esptool skriver då `No serial data received`. En bild på 1,2 MB tar omkring två minuter.

```bash
ls /dev/cu.usb*
# byt usbmodem1101 mot namnet som ls skrev ut
python -m platformio run -e cores3 -t upload --upload-port /dev/cu.usbmodem1101
python -m platformio device monitor -b 115200 --port /dev/cu.usbmodem1101
```

Skärmen är liggande, 320×240. De övre två tredjedelarna är texten. Under den ligger tre knappar: PTT hålls inne medan du pratar, INST öppnar volym och ljus, KAM tar en bild. Saknas sensorn, som på SE, står det `INGEN KAMERA`. Ett tryck i bilden tar en ny. Strömknappen vid start, eller inget sparat nät, öppnar portalen.

## Tester

```bash
cd src
python -m pip install -r requirements-dev.txt
python -m pytest -q
cd ../firmware
python -m platformio run
```

GitHub Actions kör samma två steg. Relay-testerna mockar xAI. Firmware-jobbet kompilerar skissen, det flashar inte.
