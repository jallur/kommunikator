# Kommunikator

Röst-AI på en M5StickC Plus2, med en relay framför xAI:s realtime-API (Grok Voice). Enheten har push-to-talk, transkript på skärmen och uppspelning via Hat SPK2. API-nyckeln ligger bara i relayn.

Arkitekturen, källorna och öppna frågor finns i [DESIGN.md](DESIGN.md).

Relayn ska köras som container på cloud01 eller en NAS. Den ska inte köras på Jonas Mac.

## Hårdvara

| Del | Kommentar |
| --- | --- |
| M5StickC Plus2 | ESP32-PICO-V3-02, 8 MB flash, 2 MB PSRAM |
| Skärm | 1.14" ST7789v2, 135×240 |
| Mikrofon | SPM1423, PDM |
| Summer | Passiv, ombord |
| Hat SPK2 | MAX98357 I2S, 1 W |
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

På den maskin som ska köra containern, inte på en arbetsdator som bara ska flasha:

```bash
cd relay
cp .env.example .env
# fyll i XAI_API_KEY och DEVICE_TOKENS (en token per enhet, kommaseparerat)
docker compose up -d --build
curl -fsS localhost:8080/healthz
```

Sätt en reverse proxy med TLS framför port 8080. Enheten ansluter med `wss` till den värden.

`POST /v1/missions` är ett senare gränssnitt mot Grok Bot. Den anropar inget och svarar 501. Se DESIGN.md.

## Snabbstart, firmware

```bash
cd firmware
cp include/secrets.h.example include/secrets.h
# sätt KOMM_RELAY_HOST och samma KOMM_DEVICE_TOKEN som i relayn
python -m platformio run -t upload
python -m platformio device monitor
```

`secrets.h` är gitignorerad.

Wi-Fi kan också sättas på enheten: håll knapp B inne vid start, eller starta utan sparat nät. Anslut till AP `Kommunikator` och öppna `http://192.168.4.1`. Formuläret sparar nät, relay och token i NVS.

Användning: knapp A inne medan du pratar, släpp när du är klar. Pip vid tryck och släpp. Knapp B växlar mellan SPK2 och summern. Efter tre minuter i vila släcker den skärmen och lättsover. Knapp A väcker.

## Tester

```bash
cd relay
python -m pip install -r requirements-dev.txt
python -m pytest -q
cd ../firmware
python -m platformio run
```

GitHub Actions kör samma två steg. Relay-testerna mockar xAI. Firmware-jobbet kompilerar skissen, det flashar inte.
