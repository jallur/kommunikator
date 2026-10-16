// Kommunikator firmware for M5StickC Plus2.
// Push-to-talk on button A, PDM mic uplink, onboard buzzer playback.
// Button B pages the answer. A long press still selects Hat SPK2.
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
#include <driver/i2s.h>
#include <driver/rtc_io.h>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>

#include <BluetoothA2DPSource.h>

#include <cmath>
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
static uint16_t closeCode = 0;
static char closeReason[80];

static void logWsEvent(WStype_t type, uint8_t* payload, size_t length);

class KommSocket : public WebSocketsClient {
 protected:
  void runCbEvent(WStype_t type, uint8_t* payload, size_t length) override {
    if (type == WStype_CONNECTED) lastHttp = _client.cCode;
    logWsEvent(type, payload, length);
    WebSocketsClient::runCbEvent(type, payload, length);
  }
};

static KommSocket webSocket;

static RunState state = RunState::Boot;
static bool useSpk2 = true;
static bool sessionReady = false;
static volatile bool pttHeld = false;
static bool wsStarted = false;
static bool speakerI2sLive = false;
static bool capQueued = false;
static int16_t capBuf[CAPTURE_SAMPLES];
static volatile bool uiDirty = true;

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

// 24 KB held under a second, and a full ring used to drop a single byte, so
// every later sample was swapped and the buzzer turned into noise. 56 KB is
// what still fits in DRAM: about 1.7 s. Past that, whole samples are dropped.
static constexpr size_t kRingBytes = 56 * 1024;
static uint8_t* audioRing = nullptr;
static size_t ringR = 0;
static size_t ringW = 0;
static unsigned long lastAudioMs = 0;
static bool captureReplay = true;
static uint8_t* replayBytes = nullptr;
static size_t replayCap = 0;
static size_t replayLen = 0;
static uint8_t playVolume = 200;
static uint32_t playClock = 48000;
static uint8_t buzzerMag = 48;
static uint8_t spk2Mag = 16;
static portMUX_TYPE audioMux = portMUX_INITIALIZER_UNLOCKED;
// Two of these sit in the speaker queue. 512 samples is 32 ms at 16 kHz,
// so one screen redraw cannot empty the queue.
static constexpr size_t kPlaySamples = 512;
static int16_t playBuf[3][kPlaySamples];
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

static void foldUtf8(String& out, const char* utf8, size_t limit) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(utf8);
  while (*p && out.length() < limit) {
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

static bool allocRing() {
  audioRing = static_cast<uint8_t*>(heap_caps_malloc(kRingBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!audioRing) audioRing = static_cast<uint8_t*>(malloc(kRingBytes));
  if (!audioRing) return false;
  Serial.printf("ljud ring %u %s\n", static_cast<unsigned>(kRingBytes),
                esp_ptr_external_ram(audioRing) ? "psram" : "dram");
  return true;
}

static size_t ringCountHeld() {
  return (ringW + kRingBytes - ringR) % kRingBytes;
}

static void ringClear() {
  portENTER_CRITICAL(&audioMux);
  ringR = 0;
  ringW = 0;
  portEXIT_CRITICAL(&audioMux);
}

static void replayAppend(const uint8_t* data, size_t len) {
  if (!replayBytes || replayCap < 2) return;
  if (lastAudioMs != 0 && millis() - lastAudioMs > 200) replayLen = 0;
  if (replayLen >= replayCap) return;
  size_t room = replayCap - replayLen;
  if (len > room) len = room;
  len &= ~size_t{1};
  if (!len) return;
  memcpy(replayBytes + replayLen, data, len);
  replayLen += len;
}

static void ringPush(const uint8_t* data, size_t len) {
  if (!audioRing || len < 2) return;
  len &= ~size_t{1};
  if (captureReplay) replayAppend(data, len);
  portENTER_CRITICAL(&audioMux);
  for (size_t i = 0; i < len; i += 2) {
    while (ringCountHeld() >= kRingBytes - 2) ringR = (ringR + 2) % kRingBytes;
    audioRing[ringW] = data[i];
    ringW = (ringW + 1) % kRingBytes;
    audioRing[ringW] = data[i + 1];
    ringW = (ringW + 1) % kRingBytes;
  }
  portEXIT_CRITICAL(&audioMux);
  lastAudioMs = millis();
}

static size_t ringAvailSamples() {
  portENTER_CRITICAL(&audioMux);
  size_t n = ringCountHeld() / 2;
  portEXIT_CRITICAL(&audioMux);
  return n;
}

static size_t ringPullSamples(int16_t* dest, size_t maxSamples) {
  if (!audioRing) return 0;
  size_t got = 0;
  portENTER_CRITICAL(&audioMux);
  while (got < maxSamples && ringCountHeld() >= 2) {
    uint8_t lo = audioRing[ringR];
    ringR = (ringR + 1) % kRingBytes;
    uint8_t hi = audioRing[ringR];
    ringR = (ringR + 1) % kRingBytes;
    dest[got++] = static_cast<int16_t>(lo | (static_cast<uint16_t>(hi) << 8));
  }
  portEXIT_CRITICAL(&audioMux);
  return got;
}

// AirPods Pro 1 speak classic A2DP (SBC). The stick is the source. The mic
// stays on the stick. While the buds are connected the buzzer is quiet and
// this callback turns the 16 kHz reply into 44.1 kHz stereo.
static BluetoothA2DPSource a2dp;
static volatile int a2dpState = ESP_A2D_CONNECTION_STATE_DISCONNECTED;
static int appliedA2dp = ESP_A2D_CONNECTION_STATE_DISCONNECTED;
static char a2dpName[48] = "";

static bool airpodsOn() {
  return a2dpState == ESP_A2D_CONNECTION_STATE_CONNECTED;
}

static uint8_t avrcpVolume() {
  return static_cast<uint8_t>((static_cast<uint16_t>(playVolume) * 127) / 255);
}

static bool containsAirpod(const char* name) {
  if (name == nullptr) return false;
  char buf[48];
  size_t n = strlen(name);
  if (n >= sizeof(buf)) n = sizeof(buf) - 1;
  for (size_t i = 0; i < n; i++) {
    char c = name[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    buf[i] = c;
  }
  buf[n] = '\0';
  return strstr(buf, "airpod") != nullptr;
}

static bool pickAirpod(const char* name, esp_bd_addr_t, int rssi) {
  if (name == nullptr || name[0] == '\0') return false;
  Serial.printf("bt namn %s rssi %d\n", name, rssi);
  if (!containsAirpod(name)) return false;
  strncpy(a2dpName, name, sizeof(a2dpName) - 1);
  a2dpName[sizeof(a2dpName) - 1] = '\0';
  Serial.printf("bt val %s\n", a2dpName);
  return true;
}

static void onA2dpConn(esp_a2d_connection_state_t state, void*) {
  a2dpState = static_cast<int>(state);
  uiDirty = true;
  Serial.printf("bt lage %d\n", a2dpState);
}

static int32_t fillAirpods(Frame* frame, int32_t count) {
  static int32_t phase = 0;
  static int16_t s0 = 0;
  static int16_t s1 = 0;
  static bool primed = false;
  static int hold = 0;
  // Relay chunks are about 100 ms. 80 ms of silence used to look like the
  // end of the sentence, so playback started thin and then ran dry. 600 ms
  // covers a late chunk. A hole after that resumes as soon as audio returns,
  // instead of waiting out another full buffer.
  constexpr size_t kPrimeSamples = 9600;
  constexpr unsigned long kGapMs = 250;
  constexpr int kHoldFrames = 44100 / 40;
  if (pttHeld || !airpodsOn()) {
    primed = false;
    phase = 0;
    hold = 0;
    s0 = 0;
    s1 = 0;
    for (int32_t i = 0; i < count; i++) frame[i].channel1 = frame[i].channel2 = 0;
    return count;
  }
  if (!primed) {
    size_t avail = ringAvailSamples();
    bool ended = lastAudioMs != 0 && millis() - lastAudioMs > kGapMs;
    if (avail == 0 || (avail < kPrimeSamples && !ended)) {
      for (int32_t i = 0; i < count; i++) frame[i].channel1 = frame[i].channel2 = 0;
      return count;
    }
    primed = true;
    hold = 0;
    s0 = 0;
    if (ringPullSamples(&s1, 1) != 1) s1 = 0;
  }
  for (int32_t i = 0; i < count; i++) {
    if (!primed) {
      frame[i].channel1 = frame[i].channel2 = 0;
      continue;
    }
    int32_t mixed = s0 + static_cast<int32_t>(((int64_t)(s1 - s0) * phase) / 44100);
    if (mixed > 32767) mixed = 32767;
    if (mixed < -32768) mixed = -32768;
    frame[i].channel1 = frame[i].channel2 = static_cast<int16_t>(mixed);
    phase += 16000;
    while (phase >= 44100) {
      phase -= 44100;
      s0 = s1;
      if (ringPullSamples(&s1, 1) == 1) {
        hold = 0;
        continue;
      }
      if (hold < kHoldFrames) {
        s1 = s0;
        hold++;
        continue;
      }
      s1 = 0;
      if (s0 == 0 && (lastAudioMs == 0 || millis() - lastAudioMs > kGapMs)) {
        primed = false;
        phase = 0;
        hold = 0;
      }
    }
  }
  return count;
}

static void releaseSpeaker();
static bool startSpeaker();

static void startAirpods() {
  Serial.println("bt start, satt AirPods i parning");
  a2dp.set_local_name("Kommunikator");
  a2dp.set_ssid_callback(pickAirpod);
  a2dp.set_on_connection_state_changed(onA2dpConn, nullptr);
  a2dp.set_auto_reconnect(true);
  a2dp.set_ssp_enabled(true);
  a2dp.set_reset_ble(true);
  a2dp.set_volume(avrcpVolume());
  a2dp.start(fillAirpods);
}

static void serviceAirpods() {
  int now = a2dpState;
  if (now == appliedA2dp) return;
  int prev = appliedA2dp;
  appliedA2dp = now;
  uiDirty = true;
  if (now == ESP_A2D_CONNECTION_STATE_CONNECTED) {
    Serial.printf("bt kopplad %s\n", a2dpName);
    if (!pttHeld) releaseSpeaker();
  } else if (prev == ESP_A2D_CONNECTION_STATE_CONNECTED &&
             now == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
    Serial.println("bt slapp");
    if (!pttHeld) startSpeaker();
  }
}

static void releaseSpeaker() {
  i2s_port_t port = M5.Speaker.config().i2s_port;
  M5.Speaker.end();
  if (!speakerI2sLive) return;
  i2s_driver_uninstall(port);
  speakerI2sLive = false;
}

static bool startSpeaker() {
  capQueued = false;
  M5.Mic.end();
  releaseSpeaker();
  auto cfg = M5.Speaker.config();
  if (useSpk2) {
    cfg.pin_data_out = PIN_I2S_DOUT;
    cfg.pin_bck = PIN_I2S_BCLK;
    cfg.pin_ws = PIN_I2S_LRCLK;
    cfg.buzzer = false;
    cfg.use_dac = false;
    cfg.i2s_port = I2S_NUM_1;
    cfg.magnification = spk2Mag;
  } else {
    cfg.pin_data_out = PIN_BUZZER;
    cfg.pin_bck = I2S_PIN_NO_CHANGE;
    cfg.pin_ws = I2S_PIN_NO_CHANGE;
    cfg.buzzer = true;
    cfg.use_dac = false;
    cfg.i2s_port = I2S_NUM_1;
    cfg.magnification = buzzerMag;
  }
  cfg.stereo = false;
  // The buzzer is a 1-bit modulator. Its I2S clock has to stay fast or the
  // bitstream collapses into noise. playRaw still hands over 16 kHz PCM.
  cfg.sample_rate = playClock;
  cfg.task_priority = 3;
  M5.Speaker.config(cfg);
  M5.Speaker.setVolume(playVolume);
  bool ok = M5.Speaker.begin();
  speakerI2sLive = ok;
  if (!ok && useSpk2) {
    useSpk2 = false;
    prefs.putBool("spk2", false);
    return startSpeaker();
  }
  return ok;
}

static bool startMic() {
  capQueued = false;
  releaseSpeaker();
  M5.Mic.end();
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
  cfg.dma_buf_len = 128;
  cfg.task_priority = 2;
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

static const int kAnswerRows = 48;
static const int kAnswerCols = 42;
static char answerRows[kAnswerRows][kAnswerCols];
static int answerRowsN = 0;
static int answerScroll = 0;

static int lineHeightPx() {
  int h = M5.Display.fontHeight();
  return h > 0 ? h : 16;
}

static int answerOriginY() { return 2 + lineHeightPx() * 2; }

static int answerVisible() {
  int n = (M5.Display.height() - answerOriginY() - 2) / lineHeightPx();
  return n > 0 ? n : 1;
}

static void clearAnswerRows() {
  answerRowsN = 0;
  answerScroll = 0;
}

static void rewrapAnswer() {
  answerRowsN = 0;
  const char* s = assistantLine.c_str();
  const int limit = M5.Display.width() - 12;
  while (*s && answerRowsN < kAnswerRows) {
    while (*s == ' ') s++;
    if (*s == '\n') {
      s++;
      continue;
    }
    if (!*s) break;
    int take = 0;
    int breakAt = -1;
    for (int i = 1; s[i - 1] && s[i - 1] != '\n' && i < kAnswerCols; i++) {
      if (s[i - 1] == ' ') breakAt = i - 1;
      char probe[kAnswerCols];
      memcpy(probe, s, static_cast<size_t>(i));
      probe[i] = 0;
      if (M5.Display.textWidth(probe) > limit) break;
      take = i;
    }
    if (take <= 0) break;
    bool more = s[take] && s[take] != '\n';
    int use = (more && breakAt > 0) ? breakAt : take;
    memcpy(answerRows[answerRowsN], s, static_cast<size_t>(use));
    answerRows[answerRowsN][use] = 0;
    int end = use;
    while (end > 0 && answerRows[answerRowsN][end - 1] == ' ') answerRows[answerRowsN][--end] = 0;
    answerRowsN++;
    s += use;
    if (*s == '\n') s++;
  }
  int maxScroll = answerRowsN - answerVisible();
  if (maxScroll < 0) maxScroll = 0;
  if (answerScroll > maxScroll) answerScroll = maxScroll;
}

static void scrollAnswer() {
  int visible = answerVisible();
  int maxScroll = answerRowsN - visible;
  if (maxScroll < 0) maxScroll = 0;
  if (maxScroll == 0) {
    markActivity();
    return;
  }
  if (answerScroll >= maxScroll) answerScroll = 0;
  else {
    answerScroll += visible;
    if (answerScroll > maxScroll) answerScroll = maxScroll;
  }
  uiDirty = true;
  markActivity();
}

static void applyStatus(const char* name) {
  if (pttHeld) return;
  if (strcmp(name, "speaking") == 0) setState(RunState::Speak);
  else if (strcmp(name, "thinking") == 0) {
    assistantLine = "";
    clearAnswerRows();
    uiDirty = true;
    setState(RunState::Think);
  }
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
    foldUtf8(userLine, doc["text"] | "", 160);
    uiDirty = true;
    return;
  }
  if (strcmp(type, "transcript.assistant") == 0) {
    const char* text = doc["text"] | "";
    bool cumulative = doc["cumulative"] | false;
    bool fresh = cumulative || assistantLine.length() == 0;
    if (cumulative) {
      if (text[0] == '\0') return;
      assistantLine = "";
    }
    foldUtf8(assistantLine, text, 1600);
    if (fresh) answerScroll = 0;
    rewrapAnswer();
    uiDirty = true;
    markActivity();
    return;
  }
  if (strcmp(type, "error") == 0) {
    setState(RunState::Error);
    detailLine = "";
    foldUtf8(detailLine, doc["message"] | "fel", 80);
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

static const char* wsTypeName(WStype_t type) {
  switch (type) {
    case WStype_ERROR: return "ERROR";
    case WStype_DISCONNECTED: return "DISCONNECTED";
    case WStype_CONNECTED: return "CONNECTED";
    case WStype_TEXT: return "TEXT";
    case WStype_BIN: return "BIN";
    case WStype_FRAGMENT_TEXT_START: return "FRAG_TEXT";
    case WStype_FRAGMENT_BIN_START: return "FRAG_BIN";
    case WStype_FRAGMENT: return "FRAG";
    case WStype_FRAGMENT_FIN: return "FRAG_FIN";
    case WStype_PING: return "PING";
    case WStype_PONG: return "PONG";
  }
  return "?";
}

static void copyPrintable(char* out, size_t outLen, const char* in, size_t inLen) {
  size_t n = inLen;
  if (n > outLen - 1) n = outLen - 1;
  for (size_t i = 0; i < n; i++) {
    char c = in ? in[i] : '.';
    if (c == '\r' || c == '\n') c = ' ';
    out[i] = (c >= 32 && c < 127) ? c : '.';
  }
  out[n] = 0;
}

static void logWsEvent(WStype_t type, uint8_t* payload, size_t length) {
  if (type == WStype_CONNECTED) {
    Serial.printf("ws typ CONNECTED http %u\n", lastHttp);
    return;
  }
  if (type == WStype_DISCONNECTED) {
    Serial.printf("ws typ DISCONNECTED kod %u orsak %s\n", closeCode, closeReason);
    return;
  }
  if (type == WStype_BIN || type == WStype_FRAGMENT_BIN_START || type == WStype_PING || type == WStype_PONG) {
    return;
  }
  if (type == WStype_TEXT || type == WStype_ERROR) {
    char buf[96];
    copyPrintable(buf, sizeof(buf), reinterpret_cast<const char*>(payload), length);
    if (deviceToken.length() > 0 && strstr(buf, deviceToken.c_str())) {
      Serial.printf("ws typ %s <hemlig>\n", wsTypeName(type));
      return;
    }
    Serial.printf("ws typ %s %s\n", wsTypeName(type), buf);
    return;
  }
  Serial.printf("ws typ %s\n", wsTypeName(type));
}

extern "C" void kommOnStatus(const char* line) {
  closeCode = 0;
  closeReason[0] = 0;
  char buf[96];
  size_t n = 0;
  if (line) {
    while (line[n] && line[n] != '\r' && line[n] != '\n' && n + 1 < sizeof(buf)) n++;
  }
  copyPrintable(buf, sizeof(buf), line, n);
  const char* space = strchr(buf, ' ');
  if (space) lastHttp = (uint16_t)atoi(space + 1);
  Serial.printf("ws rad %s\n", buf);
}

extern "C" void kommOnClose(unsigned code, const char* reason, size_t len) {
  closeCode = (uint16_t)code;
  copyPrintable(closeReason, sizeof(closeReason), reason, len);
}

extern "C" void kommOnArm() {
  closeCode = 0;
  closeReason[0] = 0;
}

// mbedTLS is built to allocate only from internal RAM. With classic
// Bluetooth up, the two 16 KB SSL buffers do not fit. Large blocks go to
// PSRAM; small ones stay internal for the crypto hardware.
extern "C" void* kommTlsCalloc(size_t n, size_t size) {
  size_t bytes = n * size;
  void* p = nullptr;
  if (bytes >= 2048) p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_calloc(n, size, MALLOC_CAP_8BIT);
  return p;
}

extern "C" void kommTlsFree(void* p) { heap_caps_free(p); }

static void logTls() {
  Serial.printf("heap fri %u storst %u\n", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  if (!useTls) {
    Serial.printf("tls av %s:%u\n", relayHost.c_str(), (unsigned)relayPort);
    return;
  }
  WiFiClientSecure probe;
  probe.setCACert(KOMM_CA_CERT);
  char errText[48];
  errText[0] = 0;
  if (!probe.connect(relayHost.c_str(), relayPort, 8000)) {
    int err = probe.lastError(errText, sizeof(errText));
    Serial.printf("tls fel %d %s:%u\n", err, relayHost.c_str(), (unsigned)relayPort);
    probe.stop();
    return;
  }
  Serial.printf("tls ok %s:%u\n", relayHost.c_str(), (unsigned)relayPort);
  probe.stop();
}

static void startWebSocket() {
  if (deviceToken.length() == 0 || relayHost.length() == 0) {
    detailLine = "saknar token/vard";
    setState(RunState::Error);
    return;
  }
  Serial.printf("wifi %s rssi %d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  logTls();
  // The library adds the trailing CRLF. One here would end the request early.
  snprintf(authHeader, sizeof(authHeader), "Authorization: Bearer %s", deviceToken.c_str());
  webSocket.setExtraHeaders(authHeader);
  webSocket.onEvent(onWs);
  webSocket.setReconnectInterval(3000);
  if (useTls) {
    webSocket.beginSslWithCA(relayHost.c_str(), relayPort, "/ws", KOMM_CA_CERT, "");
  } else {
    webSocket.begin(relayHost.c_str(), relayPort, "/ws", "");
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
  // Classic Bluetooth and Wi-Fi share the radio. The IDF aborts if modem
  // sleep is off while both are enabled.
  WiFi.setSleep(true);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  uiDirty = true;
}

static void beginPtt() {
  if (state == RunState::Provision || state == RunState::Wifi) return;
  pttHeld = true;
  capQueued = false;
  ringClear();
  M5.Speaker.stop();
  if (airpodsOn() && !useSpk2 && !M5.Speaker.isRunning()) startSpeaker();
  if (useSpk2) {
    releaseSpeaker();
    chirp(false);
  } else {
    chirp(false);
    releaseSpeaker();
  }
  if (!startMic()) {
    detailLine = "mic";
    pttHeld = false;
    if (!airpodsOn()) startSpeaker();
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
  capQueued = false;
  M5.Mic.end();
  if (sessionReady) sendPtt("up");
  if (airpodsOn()) {
    if (!useSpk2) {
      if (!M5.Speaker.isRunning()) startSpeaker();
      chirp(true);
      releaseSpeaker();
    } else {
      chirp(true);
    }
  } else if (useSpk2) {
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
  if (!airpodsOn()) startSpeaker();
  detailLine = airpodsOn() ? "AIR" : (useSpk2 ? "SPK2" : "summer");
  uiDirty = true;
  markActivity();
}

static void printFitted(const char* text, int maxW) {
  if (text[0] == 0) return;
  if (M5.Display.textWidth(text) <= maxW) {
    M5.Display.print(text);
    return;
  }
  char buf[48];
  size_t keep = strlen(text);
  if (keep > sizeof(buf) - 1) keep = sizeof(buf) - 1;
  while (keep > 1) {
    memcpy(buf, text, keep);
    buf[keep] = 0;
    if (M5.Display.textWidth(buf) <= maxW) break;
    keep--;
  }
  buf[keep] = 0;
  M5.Display.print(buf);
}

static void draw() {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(0xFD20, TFT_BLACK);
  M5.Display.setTextWrap(false);
  int lh = lineHeightPx();
  M5.Display.setCursor(4, 2);
  M5.Display.printf("KOMM %s", stateName(state));
  char meta[24];
  const char* outName = airpodsOn() ? "AIR" : (useSpk2 ? "SPK2" : "SUM");
  if (batteryMv > 0) snprintf(meta, sizeof(meta), "%dmV %s", batteryMv, outName);
  else snprintf(meta, sizeof(meta), "%s", outName);
  int metaW = M5.Display.textWidth(meta);
  M5.Display.setCursor(M5.Display.width() - metaW - 4, 2);
  M5.Display.print(meta);

  M5.Display.setCursor(4, 2 + lh);
  if (state == RunState::Error && detailLine.length()) {
    printFitted(detailLine.c_str(), M5.Display.width() - 8);
  } else if (userLine.length()) {
    M5.Display.print(">");
    printFitted(userLine.c_str(), M5.Display.width() - 16);
  }

  int y0 = answerOriginY();
  int visible = answerVisible();
  int maxW = M5.Display.width() - 12;
  for (int i = 0; i < visible; i++) {
    int row = answerScroll + i;
    if (row >= answerRowsN) break;
    M5.Display.setCursor(4, y0 + i * lh);
    printFitted(answerRows[row], maxW);
  }
  if (answerRowsN > visible && answerScroll + visible < answerRowsN) {
    int y = M5.Display.height() - 6;
    int x = M5.Display.width() - 8;
    M5.Display.fillTriangle(x, y, x + 6, y, x + 3, y + 5, 0xFD20);
  }
  uiDirty = false;
}

static void serviceCapture() {
  if (!pttHeld || !sessionReady) return;
  if (!capQueued) {
    capQueued = M5.Mic.record(capBuf, CAPTURE_SAMPLES, CAPTURE_HZ);
    return;
  }
  // record() returns as soon as the buffer is queued. Sending it then lets
  // the mic task write into a dead stack frame and panics core 1.
  if (M5.Mic.isRecording()) return;
  webSocket.sendBIN(reinterpret_cast<uint8_t*>(capBuf), sizeof(capBuf));
  capQueued = M5.Mic.record(capBuf, CAPTURE_SAMPLES, CAPTURE_HZ);
}

static void printAudio() {
  uint8_t mag = useSpk2 ? spk2Mag : buzzerMag;
  Serial.printf("ljud v=%u klocka=%u mag=%u sparat=%u\n", playVolume, playClock, mag,
                (unsigned)(replayLen / 2));
}

static bool audioBusy() { return pttHeld; }

static void playSaved() {
  if (audioBusy()) {
    Serial.println("ljud upptagen");
    return;
  }
  if (replayLen < 2) {
    Serial.println("ljud inget sparat");
    return;
  }
  if (!airpodsOn() && !M5.Speaker.isRunning()) startSpeaker();
  captureReplay = false;
  ringClear();
  ringPush(replayBytes, replayLen);
  captureReplay = true;
  Serial.println("ljud spelar senaste");
}

static void playTone() {
  if (audioBusy()) {
    Serial.println("ljud upptagen");
    return;
  }
  if (!airpodsOn() && !M5.Speaker.isRunning()) startSpeaker();
  captureReplay = false;
  ringClear();
  int16_t chunk[128];
  const int total = 16000;
  for (int i = 0; i < total;) {
    int n = 128;
    if (i + n > total) n = total - i;
    for (int s = 0; s < n; s++) {
      int t = i + s;
      float v;
      if (t < 3200) {
        v = sinf(t * 6.2831853f * 440.0f / 16000.0f);
      } else {
        float phase = t * 6.2831853f * 180.0f / 16000.0f;
        v = sinf(phase) * 0.55f + sinf(phase * 2.0f) * 0.3f + sinf(phase * 3.0f) * 0.15f;
      }
      int sample = static_cast<int>(v * 10000.0f);
      if (sample > 32767) sample = 32767;
      if (sample < -32768) sample = -32768;
      chunk[s] = static_cast<int16_t>(sample);
    }
    ringPush(reinterpret_cast<uint8_t*>(chunk), static_cast<size_t>(n) * 2);
    i += n;
  }
  captureReplay = true;
  Serial.println("ljud ton");
}

static int cmdInt(const char* text, int fallback) {
  while (*text == ' ') text++;
  if (*text == '\0') return fallback;
  return atoi(text);
}

static void handleAudioCmd(char* line) {
  while (*line == ' ') line++;
  if (*line == '\0') return;
  char op = line[0];
  const char* arg = line + 1;
  if (op == 't') {
    playTone();
    printAudio();
    return;
  }
  if (op == 'p') {
    playSaved();
    printAudio();
    return;
  }
  if (op == 'v') {
    int volume = cmdInt(arg, -1);
    if (volume < 0 || volume > 255) {
      Serial.println("ljud v 0-255");
      return;
    }
    playVolume = static_cast<uint8_t>(volume);
    if (M5.Speaker.isRunning()) M5.Speaker.setVolume(playVolume);
    a2dp.set_volume(avrcpVolume());
    printAudio();
    return;
  }
  if (op == 'r') {
    int rate = cmdInt(arg, -1);
    if (rate != 16000 && rate != 22050 && rate != 24000 && rate != 32000 && rate != 44100 &&
        rate != 48000) {
      Serial.println("ljud r 16000|22050|24000|32000|44100|48000");
      return;
    }
    playClock = static_cast<uint32_t>(rate);
    if (!audioBusy() && !airpodsOn()) startSpeaker();
    printAudio();
    return;
  }
  if (op == 'm') {
    int mag = cmdInt(arg, -1);
    if (mag < 1 || mag > 64) {
      Serial.println("ljud m 1-64");
      return;
    }
    if (useSpk2) spk2Mag = static_cast<uint8_t>(mag);
    else buzzerMag = static_cast<uint8_t>(mag);
    if (!audioBusy() && !airpodsOn()) startSpeaker();
    printAudio();
    return;
  }
  Serial.println("ljud: t ton, p senaste, v 0-255, r takt, m 1-64, ?");
  printAudio();
}

static char cmdLine[32];
static uint8_t cmdLen = 0;

static void serviceSerial() {
  while (Serial.available() > 0) {
    int raw = Serial.read();
    if (raw < 0) return;
    char c = static_cast<char>(raw);
    if (c == '\r' || c == '\n') {
      if (cmdLen == 0) continue;
      cmdLine[cmdLen] = '\0';
      cmdLen = 0;
      handleAudioCmd(cmdLine);
      continue;
    }
    if (cmdLen < sizeof(cmdLine) - 1) cmdLine[cmdLen++] = c;
  }
}

static void servicePlayback() {
  if (airpodsOn()) {
    if (!pttHeld && M5.Speaker.isRunning()) releaseSpeaker();
    if (state == RunState::Speak && ringAvailSamples() == 0 && millis() - lastAudioMs > 200) {
      setState(RunState::Idle);
    }
    return;
  }
  if (pttHeld || !M5.Speaker.isRunning()) return;
  // Keep both speaker slots full. A half-empty queue runs dry while the
  // screen redraws, which is the short silence in the middle of a reply.
  while (M5.Speaker.isPlaying(0) < 2) {
    size_t avail = ringAvailSamples();
    bool stale = millis() - lastAudioMs > 60;
    if (avail < kPlaySamples && !(stale && avail > 0)) break;
    size_t want = avail < kPlaySamples ? avail : kPlaySamples;
    size_t n = ringPullSamples(playBuf[playIdx], want);
    if (n == 0) break;
    M5.Speaker.playRaw(playBuf[playIdx], n, PLAYBACK_HZ, false, 1, 0, false);
    playIdx = static_cast<uint8_t>((playIdx + 1) % 3);
    setState(RunState::Speak);
  }
  if (state == RunState::Speak && ringAvailSamples() == 0 && M5.Speaker.isPlaying(0) == 0 &&
      millis() - lastAudioMs > 120) {
    setState(RunState::Idle);
  }
}

static void maybeSleep() {
  if (state != RunState::Idle) {
    if (state != RunState::Provision) M5.Display.setBrightness(170);
    return;
  }
  unsigned long quiet = millis() - lastActivity;
  if (quiet > DIM_AFTER_MS) M5.Display.setBrightness(24);
  if (quiet < SLEEP_AFTER_MS) return;
  if (a2dpState != ESP_A2D_CONNECTION_STATE_DISCONNECTED) return;
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
  mbedtls_platform_set_calloc_free(kommTlsCalloc, kommTlsFree);

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
  M5.Display.setTextWrap(false);
  M5.BtnB.setHoldThresh(700);
  speakerI2sLive = cfg.internal_spk || useSpk2;
  if (!allocRing()) Serial.println("ljud ring saknas");
  startSpeaker();
  M5.Mic.end();
  replayCap = 32 * 1024;
  replayBytes = static_cast<uint8_t*>(heap_caps_malloc(replayCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!replayBytes) replayBytes = static_cast<uint8_t*>(malloc(replayCap));
  if (!replayBytes) {
    replayCap = 12 * 1024;
    replayBytes = static_cast<uint8_t*>(malloc(replayCap));
  }
  if (!replayBytes) replayCap = 0;
  Serial.printf("ljud replay %u byte\n", static_cast<unsigned>(replayCap));
  Serial.println("ljud: t ton, p senaste, v 0-255, r takt, m 1-64, ?");
  printAudio();
  markActivity();
  startAirpods();

  M5.update();
  bool forcePortal = M5.BtnB.isPressed() || wifiSsid.length() == 0;
  if (forcePortal) enterProvision();
  else beginWifi();
  draw();
}

void loop() {
  M5.update();
  serviceSerial();
  if (state == RunState::Provision) {
    dns.processNextRequest();
    portal.handleClient();
    if (uiDirty) draw();
    return;
  }

  if (state == RunState::Wifi) {
    if (WiFi.status() == WL_CONNECTED) startWebSocket();
  } else if (wsStarted) {
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
  if (!pttHeld && state != RunState::Listen) {
    if (M5.BtnB.wasReleaseFor(700)) toggleOutput();
    else if (M5.BtnB.wasClicked()) scrollAnswer();
  }

  serviceAirpods();
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
  if (uiDirty) {
    servicePlayback();
    draw();
  }
}
