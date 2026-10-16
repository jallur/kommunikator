# Kommunikator

Röst-AI på en M5StickC Plus2, med en relay framför xAI:s realtime-API (Grok Voice). Enheten har push-to-talk, transkript på skärmen och uppspelning i den inbyggda summern. Hat SPK2 är valfri och av från start. API-nyckeln ligger bara i relayn.

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

Användning: knapp A inne medan du pratar, släpp när du är klar. Pip vid tryck och släpp. Svaret spelas i summern och skrivs på skärmen. Knapp B växlar till Hat SPK2 när den finns. Efter tre minuter i vila släcker den skärmen och lättsover. Knapp A väcker.

AirPods-testet är en egen skiss. Den ersätter röstprogrammet tills `m5stick-c-plus2` laddas upp igen. Öppna fodralet och håll knappen på baksidan tills lampan blinkar vitt. Stickan kopplar till första enheten vars namn innehåller "AirPod" och spelar en ton, en paus och en mörkare ton.

```bash
cd firmware
python -m platformio run -e airpods-test -t upload
python -m platformio device monitor
python -m platformio run -e m5stick-c-plus2 -t upload
```

## Tester

```bash
cd src
python -m pip install -r requirements-dev.txt
python -m pytest -q
cd ../firmware
python -m platformio run
```

GitHub Actions kör samma två steg. Relay-testerna mockar xAI. Firmware-jobbet kompilerar skissen, det flashar inte.
