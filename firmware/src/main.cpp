// Kommunikator firmware for M5StickC Plus2.
// Push-to-talk on button A, PDM mic uplink, onboard buzzer playback.
// Hat SPK2 is optional and off until button B selects it.
// The xAI API key never lives here. The device only knows a relay token.

#include <M5Unified.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <nvs.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>

#include <cstring>
#include <cstdlib>

#include "config.h"

enum class RunState : uint8_t {
  Boot,
  Provision,
  Wifi,
  Connecting,
  Idle,
  Listen,
  Think,
  Speak,
  Error
};

static Preferences prefs;
static WebServer portal(80);
static DNSServer dns;
static uint16_t lastHttp = 0;
static unsigned long lastProbeMs = 0;

class KommSocket : public WebSocketsClient {
 protected:
  void runCbEvent(WStype_t type, uint8_t* payload, size_t length) override {
    if (type == WStype_CONNECTED) {
      lastHttp = _client.cCode;
      Serial.printf("ws oppen http %u\n", lastHttp);
    } else if (type == WStype_DISCONNECTED) {
      unsigned kod = 0;
      if (payload && length >= 2) kod = (unsigned)((payload[0] << 8) | payload[1]);
      Serial.printf("ws stangd http %u kod %u\n", lastHttp, kod);
    }
    WebSocketsClient::runCbEvent(type, payload, length);
  }
};

static KommSocket webSocket;

static RunState state = RunState::Boot;
static bool useSpk2 = true;
static bool sessionReady = false;
static bool pttHeld = false;
static bool wsStarted = false;
static bool uiDirty = true;

static String wifiSsid;
static String wifiPass;
static String relayHost;
static String deviceToken;
static uint16_t relayPort = KOMM_RELAY_PORT;
static bool useTls = KOMM_USE_TLS != 0;
static char authHeader[220];

static String userLine;
static String assistantLine;
static String detailLine;
static int batteryMv = 0;
static unsigned long lastActivity = 0;
static unsigned long lastBatteryMs = 0;

static uint8_t audioRing[24 * 1024];
static uint16_t ringR = 0;
static uint16_t ringW = 0;
static portMUX_TYPE audioMux = portMUX_INITIALIZER_UNLOCKED;
static int16_t playBuf[3][256];
static uint8_t playIdx = 0;

static const char* stateName(RunState value) {
  switch (value) {
    case RunState::Boot: return "START";
    case RunState::Provision: return "PORTAL";
    case RunState::Wifi: return "WIFI";
    case RunState::Connecting: return "KOPPLAR";
    case RunState::Idle: return "REDO";
    case RunState::Listen: return "LYSSNAR";
    case RunState::Think: return "VANTAR";
    case RunState::Speak: return "SVARAR";
    case RunState::Error: return "FEL";
  }
  return "?";
}

static void setState(RunState next) {
  if (next == state) return;
  state = next;
  uiDirty = true;
  Serial.printf("state %s\n", stateName(state));
}

static void foldUtf8(String& out, const char* utf8) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(utf8);
  while (*p && out.length() < 180) {
    if (p[0] == 0xC3 && p[1]) {
      switch (p[1]) {
        case 0xA5:
        case 0xA4: out += 'a'; break;
        case 0xB6: out += 'o'; break;
        case 0x85:
        case 0x84: out += 'A'; break;
        case 0x96: out += 'O'; break;
        case 0xA9: out += 'e'; break;
        case 0x89: out += 'E'; break;
        default: out += '?'; break;
      }
      p += 2;
      continue;
    }
    if (*p < 0x80) {
      out += static_cast<char>(*p);
      p++;
      continue;
    }
    if ((*p & 0xE0) == 0xC0 && p[1]) p += 2;
    else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) p += 3;
    else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) p += 4;
    else p++;
    out += '?';
  }
}

static void markActivity() { lastActivity = millis(); }

static void ringClear() {
  portENTER_CRITICAL(&audioMux);
  ringR = 0;
  ringW = 0;
  portEXIT_CRITICAL(&audioMux);
}

static void ringPush(const uint8_t* data, size_t len) {
  portENTER_CRITICAL(&audioMux);
  for (size_t i = 0; i < len; i++) {
    uint16_t next = static_cast<uint16_t>((ringW + 1) % sizeof(audioRing));
    if (next == ringR) break;
    audioRing[ringW] = data[i];
    ringW = next;
  }
  portEXIT_CRITICAL(&audioMux);
}

static size_t ringPullSamples(int16_t* dest, size_t maxSamples) {
  size_t got = 0;
  portENTER_CRITICAL(&audioMux);
  while (got < maxSamples) {
    uint16_t available = static_cast<uint16_t>((ringW + sizeof(audioRing) - ringR) % sizeof(audioRing));
    if (available < 2) break;
    uint8_t lo = audioRing[ringR];
    ringR = static_cast<uint16_t>((ringR + 1) % sizeof(audioRing));
    uint8_t hi = audioRing[ringR];
    ringR = static_cast<uint16_t>((ringR + 1) % sizeof(audioRing));
    dest[got++] = static_cast<int16_t>(lo | (hi << 8));
  }
  portEXIT_CRITICAL(&audioMux);
  return got;
}

static bool startSpeaker() {
  M5.Mic.end();
  M5.Speaker.end();
  auto cfg = M5.Speaker.config();
  if (useSpk2) {
    cfg.pin_data_out = PIN_I2S_DOUT;
    cfg.pin_bck = PIN_I2S_BCLK;
    cfg.pin_ws = PIN_I2S_LRCLK;
    cfg.buzzer = false;
    cfg.use_dac = false;
    cfg.i2s_port = I2S_NUM_1;
    cfg.magnification = 16;
  } else {
    cfg.pin_data_out = PIN_BUZZER;
    cfg.pin_bck = I2S_PIN_NO_CHANGE;
    cfg.pin_ws = I2S_PIN_NO_CHANGE;
    cfg.buzzer = true;
    cfg.use_dac = false;
    cfg.i2s_port = I2S_NUM_1;
    cfg.magnification = 48;
  }
  cfg.stereo = false;
  cfg.sample_rate = PLAYBACK_HZ;
  M5.Speaker.config(cfg);
  M5.Speaker.setVolume(200);
  bool ok = M5.Speaker.begin();
  if (!ok && useSpk2) {
    useSpk2 = false;
    prefs.putBool("spk2", false);
    return startSpeaker();
  }
  return ok;
}

static bool startMic() {
  M5.Speaker.end();
  auto cfg = M5.Mic.config();
  cfg.pin_data_in = PIN_MIC_DATA;
  cfg.pin_ws = PIN_MIC_CLK;
  cfg.pin_bck = I2S_PIN_NO_CHANGE;
  cfg.sample_rate = CAPTURE_HZ;
  cfg.use_adc = false;
  cfg.stereo = false;
  cfg.i2s_port = I2S_NUM_0;
  cfg.over_sampling = 2;
  cfg.magnification = 16;
  M5.Mic.config(cfg);
  return M5.Mic.begin();
}

static void chirpLedc(bool releaseChirp) {
  ledcSetup(4, 1400, 10);
  ledcAttachPin(PIN_BUZZER, 4);
  if (!releaseChirp) {
    for (int freq = 900; freq <= 2100; freq += 300) {
      ledcWriteTone(4, freq);
      delay(14);
    }
  } else {
    for (int freq = 2100; freq >= 800; freq -= 320) {
      ledcWriteTone(4, freq);
      delay(16);
    }
  }
  ledcWrite(4, 0);
  ledcDetachPin(PIN_BUZZER);
}

static void chirpTone(bool releaseChirp) {
  if (!releaseChirp) {
    M5.Speaker.tone(1200, 40);
    delay(45);
    M5.Speaker.tone(1900, 50);
    delay(55);
  } else {
    M5.Speaker.tone(1900, 40);
    delay(45);
    M5.Speaker.tone(1000, 60);
    delay(65);
  }
  M5.Speaker.stop();
}

static void chirp(bool releaseChirp) {
  if (useSpk2) chirpLedc(releaseChirp);
  else chirpTone(releaseChirp);
}

static void sendJson(JsonDocument& doc) {
  char buf[384];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  if (n > 0 && n < sizeof(buf)) webSocket.sendTXT(buf, n);
}

static void sendHello() {
  JsonDocument doc;
  doc["type"] = "hello";
  doc["capture_rate"] = CAPTURE_HZ;
  doc["playback_rate"] = PLAYBACK_HZ;
  doc["format"] = "pcm16le";
  doc["device_id"] = "stickc-plus2";
  sendJson(doc);
}

static void sendPtt(const char* edge) {
  JsonDocument doc;
  doc["type"] = "ptt";
  doc["state"] = edge;
  sendJson(doc);
}

static void applyStatus(const char* name) {
  if (pttHeld) return;
  if (strcmp(name, "speaking") == 0) setState(RunState::Speak);
  else if (strcmp(name, "thinking") == 0) setState(RunState::Think);
  else if (strcmp(name, "idle") == 0) setState(RunState::Idle);
  else if (strcmp(name, "error") == 0) setState(RunState::Error);
  markActivity();
}

static void onTextMessage(const char* payload, size_t length) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, length)) return;
  const char* type = doc["type"] | "";
  if (strcmp(type, "hello.ok") == 0) {
    sessionReady = true;
    if (!pttHeld) setState(RunState::Idle);
    detailLine = "session";
    uiDirty = true;
    markActivity();
    return;
  }
  if (strcmp(type, "status") == 0) {
    applyStatus(doc["state"] | "");
    return;
  }
  if (strcmp(type, "transcript.user") == 0) {
    userLine = "";
    if (doc["cumulative"] | true) userLine = "";
    foldUtf8(userLine, doc["text"] | "");
    uiDirty = true;
    return;
  }
  if (strcmp(type, "transcript.assistant") == 0) {
    const char* text = doc["text"] | "";
    bool cumulative = doc["cumulative"] | false;
    if (cumulative) {
      if (text[0] == '\0') return;
      assistantLine = "";
    }
    foldUtf8(assistantLine, text);
    uiDirty = true;
    markActivity();
    return;
  }
  if (strcmp(type, "error") == 0) {
    setState(RunState::Error);
    detailLine = "";
    foldUtf8(detailLine, doc["message"] | "fel");
    const char* code = doc["code"] | "";
    if (code[0]) Serial.printf("ws fel %s\n", code);
    sessionReady = false;
    uiDirty = true;
    return;
  }
  if (strcmp(type, "pong") == 0) markActivity();
}

static void onWs(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      sessionReady = false;
      setState(RunState::Connecting);
      sendHello();
      break;
    case WStype_DISCONNECTED:
      sessionReady = false;
      if (state != RunState::Provision && state != RunState::Wifi) {
        detailLine = "bruten lank";
        setState(RunState::Connecting);
      }
      break;
    case WStype_TEXT:
      onTextMessage(reinterpret_cast<const char*>(payload), length);
      break;
    case WStype_BIN:
      if (!pttHeld) {
        ringPush(payload, length);
        setState(RunState::Speak);
        markActivity();
      }
      break;
    case WStype_ERROR:
      detailLine = "ws-fel";
      setState(RunState::Error);
      break;
    default:
      break;
  }
}

static bool nvsString(nvs_handle_t handle, const char* key, String& out) {
  size_t len = 0;
  esp_err_t err = nvs_get_str(handle, key, nullptr, &len);
  if (err == ESP_ERR_NVS_NOT_FOUND) return false;
  if (err != ESP_OK || len < 2 || len > 128) {
    if (err != ESP_OK) Serial.printf("nvs %s fel %d\n", key, (int)err);
    return false;
  }
  char buf[128];
  err = nvs_get_str(handle, key, buf, &len);
  if (err != ESP_OK) return false;
  out = buf;
  return true;
}

static void logLink() {
  lastProbeMs = millis();
  if (!useTls) {
    Serial.printf("tls av %s:%u\n", relayHost.c_str(), (unsigned)relayPort);
    return;
  }
  WiFiClientSecure probe;
  probe.setCACert(KOMM_CA_CERT);
  probe.setTimeout(5);
  char errText[48];
  errText[0] = 0;
  if (!probe.connect(relayHost.c_str(), relayPort, 8000)) {
    int err = probe.lastError(errText, sizeof(errText));
    Serial.printf("tls fel %d %s:%u\n", err, relayHost.c_str(), (unsigned)relayPort);
    probe.stop();
    return;
  }
  Serial.printf("tls ok %s:%u\n", relayHost.c_str(), (unsigned)relayPort);
  probe.print(F("GET /ws HTTP/1.1\r\nHost: "));
  probe.print(relayHost);
  probe.printf(":%u\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n", (unsigned)relayPort);
  probe.print(F("Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nAuthorization: Bearer "));
  probe.print(deviceToken);
  probe.print(F("\r\n\r\n"));
  char line[32];
  size_t n = 0;
  while (n + 1 < sizeof(line)) {
    int c = probe.read();
    if (c < 0 || c == '\n' || c == '\r') break;
    line[n++] = (char)c;
  }
  line[n] = 0;
  int http = 0;
  const char* space = strchr(line, ' ');
  if (space) http = atoi(space + 1);
  lastHttp = (uint16_t)http;
  Serial.printf("ws http %d\n", http);
  probe.stop();
}

static void startWebSocket() {
  if (deviceToken.length() == 0 || relayHost.length() == 0) {
    detailLine = "saknar token/vard";
    setState(RunState::Error);
    return;
  }
  Serial.printf("wifi %s rssi %d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  logLink();
  snprintf(authHeader, sizeof(authHeader), "Authorization: Bearer %s\r\n", deviceToken.c_str());
  webSocket.setExtraHeaders(authHeader);
  webSocket.onEvent(onWs);
  webSocket.setReconnectInterval(3000);
  if (useTls) {
    webSocket.beginSslWithCA(relayHost.c_str(), relayPort, "/ws", KOMM_CA_CERT);
  } else {
    webSocket.begin(relayHost.c_str(), relayPort, "/ws");
  }
  wsStarted = true;
  setState(RunState::Connecting);
}

static void loadConfig() {
  relayHost = KOMM_RELAY_HOST;
  deviceToken = KOMM_DEVICE_TOKEN;
  useSpk2 = false;
  nvs_handle_t handle = 0;
  if (nvs_open("komm", NVS_READONLY, &handle) == ESP_OK) {
    String value;
    if (nvsString(handle, "ssid", value)) wifiSsid = value;
    if (nvsString(handle, "pass", value)) wifiPass = value;
    if (nvsString(handle, "host", value)) relayHost = value;
    if (nvsString(handle, "token", value)) deviceToken = value;
    uint16_t port = 0;
    if (nvs_get_u16(handle, "port", &port) == ESP_OK && port) relayPort = port;
    uint8_t flag = 0;
    if (nvs_get_u8(handle, "tls", &flag) == ESP_OK) useTls = flag != 0;
    if (nvs_get_u8(handle, "spk2", &flag) == ESP_OK) useSpk2 = flag != 0;
    nvs_close(handle);
  }
#ifdef KOMM_WIFI_SSID
  if (wifiSsid.length() == 0) wifiSsid = KOMM_WIFI_SSID;
#ifdef KOMM_WIFI_PASS
  if (wifiPass.length() == 0) wifiPass = KOMM_WIFI_PASS;
#endif
#endif
}

static void enterProvision() {
  setState(RunState::Provision);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  WiFi.softAP("Kommunikator");
  dns.start(53, "*", WiFi.softAPIP());
  portal.on("/", HTTP_GET, []() {
    String page =
        "<!doctype html><meta name=viewport content='width=device-width'>"
        "<h2>Kommunikator</h2><form method=POST action=/save>"
        "SSID<br><input name=ssid maxlength=32><br>"
        "Losen<br><input name=pass type=password maxlength=64><br>"
        "Relay-vard<br><input name=host value='" + relayHost + "'><br>"
        "Port<br><input name=port value='" + String(relayPort) + "'><br>"
        "Enhetstoken<br><input name=token><br>"
        "<label><input type=checkbox name=tls " + String(useTls ? "checked" : "") + "> TLS</label><br>"
        "<button type=submit>Spara</button></form>";
    portal.send(200, "text/html", page);
  });
  portal.on("/save", HTTP_POST, []() {
    prefs.putString("ssid", portal.arg("ssid"));
    prefs.putString("pass", portal.arg("pass"));
    if (portal.arg("host").length()) prefs.putString("host", portal.arg("host"));
    if (portal.arg("token").length()) prefs.putString("token", portal.arg("token"));
    if (portal.arg("port").length()) prefs.putUShort("port", portal.arg("port").toInt());
    prefs.putBool("tls", portal.hasArg("tls"));
    portal.send(200, "text/plain", "Sparat. Startar om.");
    delay(400);
    ESP.restart();
  });
  portal.onNotFound([]() {
    portal.sendHeader("Location", "/");
    portal.send(302, "text/plain", "");
  });
  portal.begin();
  uiDirty = true;
}

static void beginWifi() {
  setState(RunState::Wifi);
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  uiDirty = true;
}

static void beginPtt() {
  if (state == RunState::Provision || state == RunState::Wifi) return;
  pttHeld = true;
  ringClear();
  M5.Speaker.stop();
  if (useSpk2) {
    M5.Speaker.end();
    chirp(false);
  } else {
    chirp(false);
    M5.Speaker.end();
  }
  if (!startMic()) {
    detailLine = "mic";
    pttHeld = false;
    startSpeaker();
    setState(RunState::Error);
    return;
  }
  if (sessionReady) sendPtt("down");
  setState(RunState::Listen);
  userLine = "";
  uiDirty = true;
  markActivity();
}

static void endPtt() {
  if (!pttHeld) return;
  pttHeld = false;
  M5.Mic.end();
  if (sessionReady) sendPtt("up");
  if (useSpk2) {
    chirp(true);
    startSpeaker();
  } else {
    startSpeaker();
    chirp(true);
  }
  setState(sessionReady ? RunState::Think : RunState::Idle);
  markActivity();
}

static void toggleOutput() {
  useSpk2 = !useSpk2;
  prefs.putBool("spk2", useSpk2);
  startSpeaker();
  detailLine = useSpk2 ? "SPK2" : "summer";
  uiDirty = true;
  markActivity();
}

static void draw() {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(0xFD20, TFT_BLACK);
  M5.Display.setCursor(4, 2);
  M5.Display.printf("KOMM  %s", stateName(state));
  M5.Display.setCursor(4, 20);
  if (batteryMv > 0) M5.Display.printf("%dmV %s", batteryMv, useSpk2 ? "SPK2" : "SUM");
  else M5.Display.print(useSpk2 ? "SPK2" : "SUM");
  M5.Display.setCursor(4, 42);
  M5.Display.print(">");
  M5.Display.print(userLine.substring(0, 72));
  M5.Display.setCursor(4, 78);
  M5.Display.print(assistantLine.substring(0, 96));
  M5.Display.setCursor(4, 118);
  M5.Display.print(detailLine.substring(0, 26));
  uiDirty = false;
}

static void serviceCapture() {
  if (!pttHeld || !sessionReady) return;
  int16_t samples[CAPTURE_SAMPLES];
  if (!M5.Mic.record(samples, CAPTURE_SAMPLES, CAPTURE_HZ)) {
    delay(5);
    return;
  }
  webSocket.sendBIN(reinterpret_cast<uint8_t*>(samples), sizeof(samples));
}

static void servicePlayback() {
  if (pttHeld || !M5.Speaker.isRunning()) return;
  if (M5.Speaker.isPlaying(0) >= 2) return;
  size_t n = ringPullSamples(playBuf[playIdx], 256);
  if (n == 0) return;
  M5.Speaker.playRaw(playBuf[playIdx], n, PLAYBACK_HZ, false, 1, 0, false);
  playIdx = static_cast<uint8_t>((playIdx + 1) % 3);
  setState(RunState::Speak);
}

static void maybeSleep() {
  if (state != RunState::Idle) {
    if (state != RunState::Provision) M5.Display.setBrightness(170);
    return;
  }
  unsigned long quiet = millis() - lastActivity;
  if (quiet > DIM_AFTER_MS) M5.Display.setBrightness(24);
  if (quiet < SLEEP_AFTER_MS) return;
  setState(RunState::Boot);
  M5.Display.setBrightness(0);
  webSocket.disconnect();
  WiFi.disconnect(true);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  if (esp_sleep_enable_ext0_wakeup(GPIO_NUM_37, 0) != ESP_OK) {
    M5.Display.setBrightness(24);
    setState(RunState::Idle);
    return;
  }
  gpio_hold_en(GPIO_NUM_4);
  esp_light_sleep_start();
  gpio_hold_dis(GPIO_NUM_4);
  digitalWrite(PIN_HOLD, HIGH);
  M5.Display.setBrightness(170);
  markActivity();
  wsStarted = false;
  beginWifi();
}

void setup() {
  pinMode(PIN_HOLD, OUTPUT);
  digitalWrite(PIN_HOLD, HIGH);
  Serial.begin(115200);
  delay(20);

  auto cfg = M5.config();
  cfg.fallback_board = m5::board_t::board_M5StickCPlus2;
  prefs.begin("komm", false);
  loadConfig();
  cfg.internal_mic = true;
  cfg.internal_spk = !useSpk2;
  cfg.external_speaker.hat_spk2 = useSpk2;
  M5.begin(cfg);
  M5.Display.setRotation(1);
  M5.Display.setFont(&fonts::Font2);
  M5.Display.setTextSize(1);
  M5.Display.setBrightness(170);
  startSpeaker();
  M5.Mic.end();
  markActivity();

  M5.update();
  bool forcePortal = M5.BtnB.isPressed() || wifiSsid.length() == 0;
  if (forcePortal) enterProvision();
  else beginWifi();
  draw();
}

void loop() {
  M5.update();
  if (state == RunState::Provision) {
    dns.processNextRequest();
    portal.handleClient();
    if (uiDirty) draw();
    return;
  }

  if (state == RunState::Wifi) {
    if (WiFi.status() == WL_CONNECTED) startWebSocket();
  } else if (wsStarted) {
    if (!webSocket.isConnected() && millis() - lastProbeMs > 3000) logLink();
    webSocket.loop();
  }

  if (M5.BtnA.wasPressed()) {
    Serial.println("ptt ned");
    beginPtt();
  }
  if (M5.BtnA.wasReleased()) {
    Serial.println("ptt upp");
    endPtt();
  }
  if (M5.BtnB.wasPressed() && !pttHeld && state != RunState::Listen) toggleOutput();

  if (pttHeld) serviceCapture();
  servicePlayback();

  if (millis() - lastBatteryMs > 10000) {
    lastBatteryMs = millis();
    int next = M5.Power.getBatteryVoltage();
    if (next != batteryMv) {
      batteryMv = next;
      uiDirty = true;
    }
  }
  maybeSleep();
  if (uiDirty) draw();
}
