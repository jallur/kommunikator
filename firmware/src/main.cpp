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
#include <esp_coexist.h>
#include <mbedtls/platform.h>
#include <fcntl.h>
#include <sys/socket.h>

#include "config.h"

#if KOMM_A2DP
#include <esp_a2dp_api.h>
#include <BluetoothA2DPSource.h>
#endif

#include <cmath>
#include <cstring>
#include <cstdlib>

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
#if KOMM_A2DP
static bool useSpk2 = true;
#else
static bool useSpk2 = false;
#endif
static bool sessionReady = false;
static volatile bool pttHeld = false;
static uint32_t turnId = 0;
static bool hearTurn = false;
static bool wsStarted = false;
static bool speakerI2sLive = false;
static bool capQueued = false;
static bool capLive = false;
static bool micOpen = false;
static size_t capFill = 0;
static uint32_t micSent = 0;
static uint32_t micQuietMs = 0;
static uint32_t micChunkMs = 0;
static uint32_t micDrop = 0;
static int32_t micPeak = 0;
// One second of 16 kHz lives in PSRAM. A static buffer of that size sits in
// internal RAM and Bluetooth then fails esp_timer_create with ESP_ERR_NO_MEM.
static int16_t* micRing = nullptr;
static size_t micRingN = 0;
static volatile size_t micW = 0;
static volatile size_t micR = 0;
static portMUX_TYPE micMux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t micTaskHandle = nullptr;
static volatile bool micRun = false;
static volatile bool micInRead = false;
static bool micTaskOn = false;
static uint32_t listenSince = 0;
static uint32_t thinkSince = 0;
static int16_t capBuf[CAPTURE_SAMPLES];
// The listen path used to block inside a socket write or Mic.record(). Button
// edges are only sampled from loop(), so the stick then sat on LYSSNAR.
static volatile int wsFd = -1;
static volatile uint32_t loopMs = 0;
static volatile uint8_t wantAbort = 0;
static volatile bool listenLive = false;
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
static constexpr size_t kRingBytes = 256 * 1024;
static uint8_t* audioRing = nullptr;
// AirPods want 44.1 kHz stereo. This holds about 3 s of that, in PSRAM.
// The Bluetooth callback only copies from it.
static constexpr size_t kStereoBytes = 512 * 1024;
static uint8_t* stereoRing = nullptr;
static volatile size_t stR = 0;
static volatile size_t stW = 0;
static int16_t stPrev = 0;
static int32_t stFrac = 0;
static portMUX_TYPE stMux = portMUX_INITIALIZER_UNLOCKED;
static size_t ringR = 0;
static size_t ringW = 0;
static unsigned long lastAudioMs = 0;
static volatile uint32_t fillOutFrames = 0;
static volatile uint32_t fillEmptyCalls = 0;
static volatile uint32_t stereoDropFrames = 0;
static volatile uint32_t monoDropSamples = 0;
static bool captureReplay = true;
static uint8_t* replayBytes = nullptr;
static size_t replayCap = 0;
static size_t replayLen = 0;
static uint8_t playVolume = 200;
static uint8_t screenBright = 170;
static bool setupOpen = false;
static bool swallowPwrClick = false;
static int setupSel = 0;
// Plus2's buzzer is a 1-bit modulator and wants a fast I2S clock.
// S3 boards play the PCM rate the profile asked the relay for.
static uint32_t playClock = KOMM_A2DP ? 48000u : PLAYBACK_HZ;
static uint8_t buzzerMag = 48;
static uint8_t spk2Mag = 16;
static portMUX_TYPE audioMux = portMUX_INITIALIZER_UNLOCKED;
// Two of these sit in the speaker queue. About 32 ms, so one screen redraw
// cannot empty the queue.
static constexpr size_t kPlaySamples = 1408;
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
  if (next == RunState::Think) thinkSince = millis();
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
  stereoRing = static_cast<uint8_t*>(heap_caps_malloc(kStereoBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  Serial.printf("ljud ring %u %s\n", static_cast<unsigned>(kRingBytes),
                esp_ptr_external_ram(audioRing) ? "psram" : "dram");
  Serial.printf("ljud stereo %u %s\n", static_cast<unsigned>(kStereoBytes),
                stereoRing && esp_ptr_external_ram(stereoRing) ? "psram" : "saknas");
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
  portENTER_CRITICAL(&stMux);
  stR = 0;
  stW = 0;
  portEXIT_CRITICAL(&stMux);
  stPrev = 0;
  stFrac = 0;
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

static void stereoFromPcm(const uint8_t* data, size_t len);

static void ringPush(const uint8_t* data, size_t len) {
  if (!audioRing || len < 2) return;
  len &= ~size_t{1};
  if (captureReplay) replayAppend(data, len);
  portENTER_CRITICAL(&audioMux);
  for (size_t i = 0; i < len; i += 2) {
    while (ringCountHeld() >= kRingBytes - 2) {
      ringR = (ringR + 2) % kRingBytes;
      monoDropSamples++;
    }
    audioRing[ringW] = data[i];
    ringW = (ringW + 1) % kRingBytes;
    audioRing[ringW] = data[i + 1];
    ringW = (ringW + 1) % kRingBytes;
  }
  portEXIT_CRITICAL(&audioMux);
  lastAudioMs = millis();
  stereoFromPcm(data, len);
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

// AirPods Pro 1 speak classic A2DP (SBC). Only the Plus2 profile has that
// radio. StickS3 and CoreS3 Lite play on their own speaker.
#if KOMM_A2DP
// The library pages a saved address on a 10 s heartbeat. Bonded AirPods do
// not answer inquiry, so a search after the first miss just delays the
// next page. Keep paging, and search only after a long streak of misses.
static constexpr uint16_t kA2dpHeartBeat = 0xff00;

class KommA2dp : public BluetoothA2DPSource {
 public:
  int pageFails = 0;

  void noteConnected() { pageFails = 0; }

  // SUSPEND leaves this at STARTED, and a later START is then ignored.
  // The ack only restarts the stream when the state machine is waiting for it.
  esp_err_t kickMedia() {
    if (s_media_state == 0) return esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
    s_media_state = 1;
    return esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
  }

  // Already-paired AirPods answer a page and stay hidden from inquiry.
  // The library only pages on a 10 s heartbeat, so call this from the loop.
  void pageSaved() {
    if (s_a2d_state != APP_AV_STATE_UNCONNECTED || !has_last_connection()) return;
    memcpy(peer_bd_addr, last_connection, sizeof(esp_bd_addr_t));
    s_a2d_state = APP_AV_STATE_CONNECTING;
    esp_err_t err = esp_a2d_connect(peer_bd_addr);
    if (err != ESP_OK) {
      s_a2d_state = APP_AV_STATE_UNCONNECTED;
      Serial.printf("bt anrop fel %d\n", static_cast<int>(err));
      return;
    }
    Serial.println("bt anrop");
  }

 protected:
  void bt_app_av_state_unconnected_hdlr(uint16_t event, void* param) override {
    // One search after a long run of missed pages, not after the first.
    // A search cannot see buds that are only connectable.
    if (event == kA2dpHeartBeat && pageFails >= 6 && !pttHeld &&
        state != RunState::Listen && state != RunState::Speak) {
      pageFails = 0;
      s_a2d_state = APP_AV_STATE_DISCOVERING;
      Serial.println("bt sok");
      esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 4, 0);
      return;
    }
    if (event == kA2dpHeartBeat) pageFails++;
    BluetoothA2DPSource::bt_app_av_state_unconnected_hdlr(event, param);
  }

  void bt_app_gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) override {
    if (event == ESP_BT_GAP_DISC_STATE_CHANGED_EVT &&
        param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED &&
        s_a2d_state != APP_AV_STATE_DISCOVERED) {
      Serial.println("bt ingen ny");
      s_a2d_state = APP_AV_STATE_UNCONNECTED;
      pageFails = 0;
      return;
    }
    BluetoothA2DPSource::bt_app_gap_callback(event, param);
  }
};

static KommA2dp a2dp;
static volatile int a2dpState = ESP_A2D_CONNECTION_STATE_DISCONNECTED;
static int appliedA2dp = ESP_A2D_CONNECTION_STATE_DISCONNECTED;
static char a2dpName[48] = "";

static bool airpodsOn() {
  return a2dpState == ESP_A2D_CONNECTION_STATE_CONNECTED;
}

static bool a2dpMediaOn = false;
static bool a2dpMediaWant = false;
static unsigned long a2dpCmdMs = 0;
// Stays set for the whole reply. Clearing it whenever the buffer dips under
// the start watermark suspends A2DP and opens a gap of about a second.
static volatile bool airpodLatched = false;
// Silence ahead of the first real frame. AirPods drop the start of a new
// A2DP stream while SBC locks, which ate everything before "två".
static volatile size_t airpodPrerollLeft = 0;
#else
static int a2dpState = 0;
static int appliedA2dp = 0;
static char a2dpName[48] = "";
static bool airpodsOn() { return false; }
static bool a2dpMediaOn = false;
static bool a2dpMediaWant = false;
static unsigned long a2dpCmdMs = 0;
static volatile bool airpodLatched = false;
static volatile size_t airpodPrerollLeft = 0;
#endif
static IPAddress relayIp;
static bool relayIpOk = false;

#if KOMM_A2DP
static void onA2dpAudio(esp_a2d_audio_state_t audio, void*) {
  a2dpMediaOn = audio == ESP_A2D_AUDIO_STATE_STARTED;
  Serial.printf("bt ljud %d\n", static_cast<int>(audio));
}

static void setAirpodMedia(bool run) {
  if (!airpodsOn()) {
    a2dpMediaWant = false;
    return;
  }
  if (run) {
    if (a2dpMediaOn) {
      a2dpMediaWant = true;
      return;
    }
    if (a2dpMediaWant && millis() - a2dpCmdMs < 1000) return;
    a2dpMediaWant = true;
    a2dpCmdMs = millis();
    esp_err_t err = a2dp.kickMedia();
    Serial.printf("bt media redo %d on %d\n", static_cast<int>(err), a2dpMediaOn ? 1 : 0);
    return;
  }
  if (!a2dpMediaOn) {
    a2dpMediaWant = false;
    return;
  }
  if (!a2dpMediaWant && millis() - a2dpCmdMs < 500) return;
  a2dpMediaWant = false;
  a2dpCmdMs = millis();
  esp_err_t err = esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
  Serial.printf("bt media paus %d on %d\n", static_cast<int>(err), a2dpMediaOn ? 1 : 0);
}
#else
static void setAirpodMedia(bool) {}
#endif

static void cacheRelayIp(const char* host) {
  if (relayIpOk || host == nullptr || host[0] == '\0') return;
  IPAddress ip;
  if (!WiFi.hostByName(host, ip) || static_cast<uint32_t>(ip) == 0) {
    Serial.printf("dns miss %s\n", host);
    return;
  }
  relayIp = ip;
  relayIpOk = true;
  Serial.printf("dns %s %s\n", host, ip.toString().c_str());
}

static uint8_t avrcpVolume() {
  return static_cast<uint8_t>((static_cast<uint16_t>(playVolume) * 127) / 255);
}

#if KOMM_A2DP
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

static void rememberAirpodName(const char* name) {
  if (name == nullptr || name[0] == '\0') return;
  strncpy(a2dpName, name, sizeof(a2dpName) - 1);
  a2dpName[sizeof(a2dpName) - 1] = '\0';
  prefs.putString("airname", a2dpName);
}

static void logBtAddr(const char* why, const uint8_t* mac) {
  bool any = false;
  if (mac != nullptr) {
    for (int i = 0; i < 6; i++) {
      if (mac[i] != 0) any = true;
    }
  }
  if (!any) {
    Serial.printf("bt %s ingen\n", why);
    return;
  }
  Serial.printf("bt %s %02x:%02x:%02x:%02x:%02x:%02x\n", why, mac[0], mac[1], mac[2], mac[3],
                mac[4], mac[5]);
}

static bool pickAirpod(const char* name, esp_bd_addr_t address, int rssi) {
  if (name == nullptr || name[0] == '\0') return false;
  Serial.printf("bt namn %s rssi %d\n", name, rssi);
  if (!containsAirpod(name)) return false;
  rememberAirpodName(name);
  logBtAddr("val", address);
  Serial.printf("bt val %s\n", a2dpName);
  return true;
}

static void onA2dpConn(esp_a2d_connection_state_t link, void*) {
  a2dpState = static_cast<int>(link);
  if (link == ESP_A2D_CONNECTION_STATE_CONNECTED) a2dp.noteConnected();
  uiDirty = true;
  Serial.printf("bt lage %d\n", a2dpState);
}
#endif

static size_t stereoAvailFrames() {
  portENTER_CRITICAL(&stMux);
  size_t n = (stW + kStereoBytes - stR) % kStereoBytes;
  portEXIT_CRITICAL(&stMux);
  return n / 4;
}

static void stereoFromPcm(const uint8_t* data, size_t len) {
  // While the buds are down the buzzer owns the reply. Keeping a stereo
  // copy made that same ending play again, seconds after the screen and
  // the buzzer had already finished.
  if (!airpodsOn()) return;
  if (!stereoRing || len < 2) return;
  size_t w = stW;
  for (size_t i = 0; i + 1 < len; i += 2) {
    int16_t cur = static_cast<int16_t>(data[i] | (static_cast<uint16_t>(data[i + 1]) << 8));
    while (stFrac < 44100) {
      int32_t mixed = stPrev + static_cast<int32_t>(((int64_t)(cur - stPrev) * stFrac) / 44100);
      if (mixed > 32767) mixed = 32767;
      if (mixed < -32768) mixed = -32768;
      if ((w + 4) % kStereoBytes == stR) {
        portENTER_CRITICAL(&stMux);
        stR = (stR + 4) % kStereoBytes;
        portEXIT_CRITICAL(&stMux);
        stereoDropFrames++;
      }
      int16_t sample = static_cast<int16_t>(mixed);
      stereoRing[w] = static_cast<uint8_t>(sample);
      stereoRing[w + 1] = static_cast<uint8_t>(static_cast<uint16_t>(sample) >> 8);
      stereoRing[w + 2] = stereoRing[w];
      stereoRing[w + 3] = stereoRing[w + 1];
      w = (w + 4) % kStereoBytes;
      stFrac += static_cast<int32_t>(PLAYBACK_HZ);
    }
    stFrac -= 44100;
    stPrev = cur;
  }
  portENTER_CRITICAL(&stMux);
  stW = w;
  portEXIT_CRITICAL(&stMux);
}

#if KOMM_A2DP
static int32_t fillAirpods(Frame* frame, int32_t count) {
  if (!stereoRing || count <= 0) {
    fillEmptyCalls++;
    return 0;
  }
  // The wrapper already zeroed the buffer. Returning zero here makes the
  // stack drop the stream it just started, so an idle pull stays silent
  // instead of ending the session.
  if (pttHeld || !airpodLatched) return count;
  // The wrapper already zeroed the buffer. Spend the opening of a new stream
  // on that silence so the decoder locks before any word is sent.
  int32_t filled = 0;
  size_t silence = airpodPrerollLeft;
  if (silence > 0) {
    if (silence > static_cast<size_t>(count)) silence = static_cast<size_t>(count);
    airpodPrerollLeft -= silence;
    filled = static_cast<int32_t>(silence);
    fillOutFrames += static_cast<uint32_t>(silence);
    if (filled == count) return count;
  }
  size_t have = stereoAvailFrames();
  size_t room = static_cast<size_t>(count - filled);
  size_t n = have < room ? have : room;
  size_t r = stR;
  for (size_t i = 0; i < n; i++) {
    size_t o = (r + i * 4) % kStereoBytes;
    int16_t sample = static_cast<int16_t>(stereoRing[o] | (static_cast<uint16_t>(stereoRing[o + 1]) << 8));
    frame[filled + static_cast<int32_t>(i)].channel1 = sample;
    frame[filled + static_cast<int32_t>(i)].channel2 = sample;
  }
  if (n > 0) {
    size_t next = (r + n * 4) % kStereoBytes;
    portENTER_CRITICAL(&stMux);
    if (stR == r) stR = next;
    portEXIT_CRITICAL(&stMux);
    fillOutFrames += static_cast<uint32_t>(n);
  } else if (filled == 0) {
    fillEmptyCalls++;
  }
  // Returning the full request keeps the A2DP clock running through the
  // quiet between xAI's packets. The tail of the buffer is still zero.
  return count;
}
#endif

static void logAudio(const char* why) {
  unsigned age = lastAudioMs == 0 ? 0u : static_cast<unsigned>(millis() - lastAudioMs);
  Serial.printf(
      "ljud %s mono %u stereo %u ut %u tom %u tapS %u tapM %u media %d/%d ptt %d btn %d mic %u upp %u alder %u %s\n",
      why, static_cast<unsigned>(ringAvailSamples()), static_cast<unsigned>(stereoAvailFrames()),
      static_cast<unsigned>(fillOutFrames), static_cast<unsigned>(fillEmptyCalls),
      static_cast<unsigned>(stereoDropFrames), static_cast<unsigned>(monoDropSamples),
      a2dpMediaOn ? 1 : 0, a2dpMediaWant ? 1 : 0, pttHeld ? 1 : 0,
      gpio_get_level(static_cast<gpio_num_t>(PIN_BTN_A)) == 0 ? 1 : 0,
      static_cast<unsigned>(capFill), static_cast<unsigned>(micSent), age, stateName(state));
}

static void releaseSpeaker();
static bool startSpeaker();

static bool airpodsStarted = false;

#if KOMM_A2DP
static void startAirpods() {
  Serial.println("bt start");
  a2dp.set_local_name("Kommunikator");
  a2dp.set_ssid_callback(pickAirpod);
  a2dp.set_on_connection_state_changed(onA2dpConn, nullptr);
  a2dp.set_on_audio_state_changed(onA2dpAudio, nullptr);
  a2dp.set_auto_reconnect(true);
  a2dp.set_ssp_enabled(true);
  a2dp.set_reset_ble(true);
  a2dp.set_volume(avrcpVolume());
  a2dp.start(fillAirpods);
  // The library claims a display and a yes/no key. AirPods have neither, so
  // the pair stalls. Just Works lets the bond complete on its own.
  esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
  esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));
  logBtAddr("minne", *a2dp.get_last_peer_address());
}

static void serviceAirpodPage() {
  if (!airpodsStarted || airpodsOn() || pttHeld || state == RunState::Listen) return;
  static unsigned long last = 0;
  if (millis() - last < 3000) return;
  last = millis();
  a2dp.pageSaved();
}

static void serviceAirpods() {
  serviceAirpodPage();
  int now = a2dpState;
  if (now == appliedA2dp) return;
  int prev = appliedA2dp;
  appliedA2dp = now;
  uiDirty = true;
  if (now != ESP_A2D_CONNECTION_STATE_CONNECTED) {
    a2dpMediaOn = false;
    a2dpMediaWant = false;
  }
  if (now == ESP_A2D_CONNECTION_STATE_CONNECTED) {
    Serial.printf("bt kopplad %s\n", a2dpName);
    if (state != RunState::Wifi) esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
    if (!pttHeld) releaseSpeaker();
  } else if (prev == ESP_A2D_CONNECTION_STATE_CONNECTED &&
             now == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
    Serial.println("bt slapp");
    portENTER_CRITICAL(&stMux);
    stR = 0;
    stW = 0;
    portEXIT_CRITICAL(&stMux);
    airpodLatched = false;
    airpodPrerollLeft = 0;
    if (state != RunState::Wifi) esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
    if (!pttHeld) startSpeaker();
  }
}
#else
static void startAirpods() {
  Serial.printf("kort %s utan a2dp\n", kBoard.deviceId);
}
static void serviceAirpods() {}
#endif

static void ensureAirpods() {
  if (airpodsStarted) return;
  airpodsStarted = true;
  startAirpods();
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
#if KOMM_A2DP
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
#endif
  cfg.stereo = false;
  // The buzzer is a 1-bit modulator. Its I2S clock has to stay fast or the
  // bitstream collapses into noise. playRaw still hands over 16 kHz PCM.
  cfg.sample_rate = playClock;
  cfg.task_priority = 3;
  // Default is 8 x 256 samples in internal RAM. With Bluetooth and TLS up,
  // that malloc fails and the driver can keep the buffers.
  cfg.dma_buf_count = 4;
  cfg.dma_buf_len = 64;
  M5.Speaker.config(cfg);
  M5.Speaker.setVolume(playVolume);
  bool ok = M5.Speaker.begin();
  speakerI2sLive = ok;
  if (!ok) i2s_driver_uninstall(cfg.i2s_port);
  if (!ok && useSpk2) {
    useSpk2 = false;
    prefs.putBool("spk2", false);
    return startSpeaker();
  }
  return ok;
}

static inline void micBarrier() { __asm__ __volatile__("memw" ::: "memory"); }

static bool allocMicRing() {
  if (micRing != nullptr) return true;
  constexpr size_t kSamples = 16000;
  micRing = static_cast<int16_t*>(
      heap_caps_malloc(kSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (micRing == nullptr) {
    Serial.println("mic ring saknas");
    return false;
  }
  micRingN = kSamples;
  Serial.printf("mic ring %u psram\n", static_cast<unsigned>(micRingN));
  return true;
}

static void micResetRing() {
  micW = 0;
  micR = 0;
  micBarrier();
}

static void micPush(const int16_t* src, size_t n) {
  if (micRing == nullptr || micRingN < 2) return;
  size_t w = micW;
  for (size_t i = 0; i < n; i++) {
    size_t next = (w + 1) % micRingN;
    if (next == micR) {
      micDrop += static_cast<uint32_t>(n - i);
      return;
    }
    micRing[w] = src[i];
    micBarrier();
    w = next;
    micW = w;
  }
}

static size_t micPop(int16_t* dst, size_t maxn) {
  if (micRing == nullptr || micRingN < 2) return 0;
  size_t n = 0;
  size_t r = micR;
  size_t w = micW;
  micBarrier();
  while (n < maxn && r != w) {
    dst[n++] = micRing[r];
    r = (r + 1) % micRingN;
  }
  micBarrier();
  micR = r;
  return n;
}

static void micWaitIdle() {
  for (int i = 0; i < 40 && micInRead; i++) delay(2);
}

#if KOMM_A2DP
// M5Unified 0.2.5's mic task does this division for the SPM1423. The
// legacy driver clock plus `sample <<= 4` was a square wave. Calling
// M5.Mic.record from our task only ever returned one 64-sample buffer:
// isRecording() is still false before their task wakes, and the next
// record() then waits forever on a buffer their task never finishes.
static void calcPdmDiv(uint32_t* div_a, uint32_t* div_b, uint32_t* div_n,
                       uint32_t baseClock, uint32_t targetFreq) {
  if (baseClock <= (targetFreq << 1)) {
    *div_n = 2;
    *div_a = 1;
    *div_b = 0;
    return;
  }
  uint32_t save_n = 255;
  uint32_t save_a = 63;
  uint32_t save_b = 62;
  if (targetFreq) {
    float fdiv = static_cast<float>(baseClock) / static_cast<float>(targetFreq);
    uint32_t n = static_cast<uint32_t>(fdiv);
    if (n < 256) {
      fdiv -= n;
      float check_base = static_cast<float>(baseClock);
      while (static_cast<int32_t>(targetFreq) >= 0) {
        targetFreq <<= 1;
        check_base *= 2;
      }
      float check_target = static_cast<float>(targetFreq);
      uint32_t save_diff = UINT32_MAX;
      if (n < 255) {
        save_a = 1;
        save_b = 0;
        save_n = n + 1;
        save_diff = static_cast<uint32_t>(fabsf(check_target - check_base / static_cast<float>(save_n)));
      }
      for (uint32_t a = 1; a < 64; ++a) {
        uint32_t b = static_cast<uint32_t>(roundf(static_cast<float>(a) * fdiv));
        if (a <= b) continue;
        float got = (check_base * static_cast<float>(a)) /
                    static_cast<float>(n * a + b);
        uint32_t diff = static_cast<uint32_t>(fabsf(check_target - got));
        if (save_diff <= diff) continue;
        save_diff = diff;
        save_a = a;
        save_b = b;
        save_n = n;
        if (!diff) break;
      }
    }
  }
  *div_n = save_n;
  *div_a = save_a;
  *div_b = save_b;
}

static void tunePdmClock(uint32_t sampleRate) {
  constexpr uint32_t kPll = 80000000;
  constexpr uint32_t kBits = 64;
  constexpr uint32_t kDivM = 2;
  constexpr uint32_t kOver = 2;
  uint32_t div_a = 1;
  uint32_t div_b = 0;
  uint32_t div_n = 2;
  calcPdmDiv(&div_a, &div_b, &div_n, kPll / (kBits * kDivM), sampleRate * kOver);
  I2S0.pdm_conf.rx_sinc_dsr_16_en = 1;
  I2S0.pdm_conf.pdm2pcm_conv_en = 1;
  I2S0.pdm_conf.rx_pdm_en = 1;
  I2S0.sample_rate_conf.rx_bck_div_num = kDivM;
  I2S0.clkm_conf.clkm_div_a = div_a;
  I2S0.clkm_conf.clkm_div_b = div_b;
  I2S0.clkm_conf.clkm_div_num = div_n;
  I2S0.clkm_conf.clka_en = 0;
  I2S0.conf.rx_reset = 1;
  I2S0.conf.rx_fifo_reset = 1;
  I2S0.conf.rx_reset = 0;
  I2S0.conf.rx_fifo_reset = 0;
  Serial.printf("mic pdm n=%u a=%u b=%u\n", div_n, div_a, div_b);
}

static int32_t pdmSum0 = 0;
static int32_t pdmSum1 = 0;
static int32_t pdmOffset = 0;
static int pdmOs = 2;
static int16_t pdmCarry = 0;
static bool pdmCarryOn = false;
static bool pdmDmaLogged = false;

static void pdmReset() {
  pdmSum0 = 0;
  pdmSum1 = 0;
  pdmOffset = 0;
  pdmOs = 2;
  pdmCarry = 0;
  pdmCarryOn = false;
  pdmDmaLogged = false;
}

static int16_t pdmClip(int32_t value) {
  if (value < INT16_MIN + 16) return static_cast<int16_t>(INT16_MIN + 16);
  if (value > INT16_MAX - 16) return static_cast<int16_t>(INT16_MAX - 16);
  return static_cast<int16_t>(value);
}

// Same oversampling, channel swap, DC tracker and gain as Mic_Class::mic_task
// on classic ESP32. Two raw pairs become two PCM samples at CAPTURE_HZ.
static size_t pdmFeed(const int16_t* src, size_t n, int16_t* dst, size_t cap, size_t* used) {
  size_t in = 0;
  size_t out = 0;
  while (out + 1 < cap) {
    while (pdmOs > 0) {
      int16_t s0;
      int16_t s1;
      if (pdmCarryOn) {
        if (in >= n) {
          *used = in;
          return out;
        }
        s0 = pdmCarry;
        s1 = src[in++];
        pdmCarryOn = false;
      } else if (in + 1 < n) {
        s0 = src[in++];
        s1 = src[in++];
      } else {
        if (in < n) {
          pdmCarry = src[in++];
          pdmCarryOn = true;
        }
        *used = in;
        return out;
      }
      pdmSum0 += s0;
      pdmSum1 += s1;
      pdmOs--;
    }
    pdmOs = 2;
    int32_t sv0 = pdmSum1;
    int32_t sv1 = pdmSum0;
    int32_t mixed = (sv0 + sv1) << 3;
    pdmOffset -= (mixed + pdmOffset + 16) >> 5;
    int32_t off = (pdmOffset + 8) >> 4;
    dst[out++] = pdmClip((sv0 + off) * 4);
    dst[out++] = pdmClip((sv1 + off) * 4);
    pdmSum0 = 0;
    pdmSum1 = 0;
  }
  *used = in;
  return out;
}

static bool installPdm(int dmaCount) {
  i2s_config_t i2sConfig;
  memset(&i2sConfig, 0, sizeof(i2sConfig));
  i2sConfig.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM);
  i2sConfig.sample_rate = 48000;
  i2sConfig.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  i2sConfig.channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT;
  i2sConfig.communication_format = I2S_COMM_FORMAT_STAND_PCM_SHORT;
  i2sConfig.dma_buf_count = dmaCount;
  i2sConfig.dma_buf_len = 64;

  i2s_pin_config_t pinConfig;
  memset(&pinConfig, 0xFF, sizeof(pinConfig));
  pinConfig.bck_io_num = I2S_PIN_NO_CHANGE;
  pinConfig.ws_io_num = PIN_MIC_CLK;
  pinConfig.data_out_num = I2S_PIN_NO_CHANGE;
  pinConfig.data_in_num = PIN_MIC_DATA;
  pinConfig.mck_io_num = I2S_PIN_NO_CHANGE;

  esp_err_t err = i2s_driver_install(I2S_NUM_0, &i2sConfig, 0, nullptr);
  if (err != ESP_OK) {
    i2s_driver_uninstall(I2S_NUM_0);
    err = i2s_driver_install(I2S_NUM_0, &i2sConfig, 0, nullptr);
  }
  if (err != ESP_OK) return false;
  if (i2s_set_pin(I2S_NUM_0, &pinConfig) != ESP_OK) {
    i2s_driver_uninstall(I2S_NUM_0);
    return false;
  }
  i2s_stop(I2S_NUM_0);
  tunePdmClock(CAPTURE_HZ);
  if (i2s_start(I2S_NUM_0) != ESP_OK) {
    i2s_driver_uninstall(I2S_NUM_0);
    return false;
  }
  int16_t junk[64];
  size_t dummy = 0;
  i2s_read(I2S_NUM_0, junk, sizeof(junk), &dummy, pdMS_TO_TICKS(10));
  i2s_read(I2S_NUM_0, junk, sizeof(junk), &dummy, pdMS_TO_TICKS(10));
  return true;
}

#endif

static void micReader(void*) {
  int16_t tmp[64];
  for (;;) {
    portENTER_CRITICAL(&micMux);
    bool run = micRun;
    if (run) micInRead = true;
    portEXIT_CRITICAL(&micMux);
    if (!run) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
      continue;
    }
#if KOMM_A2DP
    size_t bytes = 0;
    int16_t raw[128];
    esp_err_t err = i2s_read(I2S_NUM_0, raw, sizeof(raw), &bytes, pdMS_TO_TICKS(20));
    bool pushed = false;
    if (err == ESP_OK && bytes >= sizeof(int16_t)) {
      if (!pdmDmaLogged) {
        pdmDmaLogged = true;
        Serial.printf("mic dma %u\n", static_cast<unsigned>(bytes));
      }
      size_t n = bytes / sizeof(int16_t);
      size_t off = 0;
      while (off < n) {
        size_t used = 0;
        size_t produced = pdmFeed(raw + off, n - off, tmp, 64, &used);
        if (produced) {
          micPush(tmp, produced);
          pushed = true;
        }
        if (used == 0) break;
        off += used;
        if (!produced) break;
      }
    }
    micInRead = false;
    if (!pushed) vTaskDelay(pdMS_TO_TICKS(5));
#else
    size_t got = 0;
    // Wait until the background task has actually started, then until it
    // has filled tmp. isRecording() is also false before the task wakes,
    // and treating that as a finished buffer pushes stack noise.
    if (M5.Mic.record(tmp, 64, CAPTURE_HZ, false)) {
      for (int i = 0; i < 20 && !M5.Mic.isRecording(); i++) vTaskDelay(pdMS_TO_TICKS(2));
      for (int i = 0; i < 40 && M5.Mic.isRecording(); i++) vTaskDelay(pdMS_TO_TICKS(2));
      if (!M5.Mic.isRecording()) got = sizeof(tmp);
    }
    if (got >= sizeof(int16_t)) micPush(tmp, got / sizeof(int16_t));
    micInRead = false;
    if (got < sizeof(int16_t)) vTaskDelay(pdMS_TO_TICKS(5));
#endif
  }
}

static void startMicTask() {
  if (micTaskHandle != nullptr) return;
  if (!allocMicRing()) return;
  if (xTaskCreatePinnedToCore(micReader, "mic", 3072, nullptr, 4, &micTaskHandle, 1) != pdPASS) {
    micTaskHandle = nullptr;
    Serial.println("mic task nej");
    return;
  }
  micTaskOn = true;
  Serial.printf("mic task intern %u\n",
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
}

static void stopMic() {
  portENTER_CRITICAL(&micMux);
  micRun = false;
  portEXIT_CRITICAL(&micMux);
  capFill = 0;
  capQueued = false;
  capLive = false;
  if (!micOpen) return;
  micWaitIdle();
#if KOMM_A2DP
  i2s_stop(I2S_NUM_0);
  i2s_driver_uninstall(I2S_NUM_0);
#else
  M5.Mic.end();
#endif
  micOpen = false;
}

static void armMic(const char* how) {
  micResetRing();
#if KOMM_A2DP
  pdmReset();
#endif
  micOpen = true;
  micSent = 0;
  micDrop = 0;
  micPeak = 0;
  micQuietMs = millis();
  micChunkMs = millis();
  portENTER_CRITICAL(&micMux);
  micRun = true;
  portEXIT_CRITICAL(&micMux);
  if (micTaskHandle != nullptr) xTaskNotifyGive(micTaskHandle);
  Serial.printf("mic %s intern %u\n", how,
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
}

#if KOMM_A2DP
static bool startPdmMic() {
  const int counts[] = {8, 4, 2};
  for (int count : counts) {
    if (!installPdm(count)) continue;
    Serial.printf("mic dma-buf %d\n", count);
    armMic("pdm");
    return true;
  }
  Serial.println("mic begin nej");
  return false;
}
#endif

static bool startMic() {
  stopMic();
  releaseSpeaker();
  M5.Mic.end();
#if KOMM_A2DP
  // GPIO0 is the SPM1423 clock and the Hat SPK2 word select. The speaker
  // has to be gone before the PDM driver claims it.
  return startPdmMic();
#else
  auto mic = M5.Mic.config();
  mic.dma_buf_count = 4;
  mic.dma_buf_len = 64;
  M5.Mic.config(mic);
  if (!M5.Mic.begin()) {
    Serial.println("mic begin nej");
    return false;
  }
  armMic("m5");
  return true;
#endif
}

static bool wsCanSend();

static bool sendCapture(size_t samples) {
  if (samples < 2 || !sessionReady) return false;
  if (!wsCanSend()) return false;
  size_t bytes = samples * sizeof(int16_t);
  if (!webSocket.sendBIN(reinterpret_cast<uint8_t*>(capBuf), bytes)) return false;
  micSent += bytes;
  if (micSent == bytes) Serial.printf("mic skick %u\n", static_cast<unsigned>(bytes));
  return true;
}

static void flushCapture() {
  if (capFill >= 2) {
    uint32_t start = millis();
    while (!wsCanSend() && millis() - start < 150) delay(5);
    size_t samples = capFill;
    if (!sendCapture(samples)) {
      Serial.printf("mic ej skick %u\n", static_cast<unsigned>(samples));
    } else {
      capFill = 0;
    }
  }
  Serial.printf("mic tot %u tap %u niva %ld\n", static_cast<unsigned>(micSent),
                static_cast<unsigned>(micDrop), static_cast<long>(micPeak));
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
  doc["device_id"] = kBoard.deviceId;
  sendJson(doc);
  Serial.printf("hello %s spel %u\n", kBoard.deviceId, static_cast<unsigned>(PLAYBACK_HZ));
}

static void sendPtt(const char* edge) {
  JsonDocument doc;
  doc["type"] = "ptt";
  doc["state"] = edge;
  doc["turn"] = turnId;
  sendJson(doc);
  Serial.printf("ptt %s turn %u\n", edge, static_cast<unsigned>(turnId));
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
    uint32_t turn = doc["turn"] | 0;
    if (turn != turnId) return;
    const char* name = doc["state"] | "";
    if (strcmp(name, "speaking") == 0) hearTurn = true;
    applyStatus(name);
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
#if KOMM_A2DP
      esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
#endif
      setState(RunState::Connecting);
      sendHello();
      break;
    case WStype_DISCONNECTED:
      wsFd = -1;
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
      if (!pttHeld && hearTurn) {
        ringPush(payload, length);
        setState(RunState::Speak);
        markActivity();
        Serial.printf("ljud in %u ", static_cast<unsigned>(length));
        logAudio("paket");
      } else {
        Serial.printf("ljud slapp %u B turn %u\n", static_cast<unsigned>(length),
                      static_cast<unsigned>(turnId));
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
    int rssi = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
    Serial.printf("ws typ DISCONNECTED kod %u orsak %s rssi %d heap %u storst %u\n", closeCode,
                  closeReason, rssi, static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(ESP.getMaxAllocHeap()));
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

static void clampSocket(int fd) {
  if (fd < 0) return;
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 250000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

extern "C" void kommClampWs(void* client) {
  if (client == nullptr) return;
  int fd = useTls ? static_cast<WiFiClientSecure*>(client)->fd() : static_cast<WiFiClient*>(client)->fd();
  wsFd = fd;
  clampSocket(fd);
}

static bool wsCanSend() {
  int fd = wsFd;
  if (fd < 0) return false;
  fd_set set;
  FD_ZERO(&set);
  FD_SET(fd, &set);
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 0;
  return select(fd + 1, nullptr, &set, nullptr, &tv) > 0;
}

extern "C" int kommWsConnect(void* client, const char* host, uint16_t port, int32_t timeout) {
  if (client == nullptr || host == nullptr) return 0;
  if (!relayIpOk) cacheRelayIp(host);
  int ok = 0;
  int fd = -1;
  if (relayIpOk && useTls) {
    auto* secure = static_cast<WiFiClientSecure*>(client);
    // Seconds, and only for the TCP+TLS connect. The library later calls
    // setTimeout(WEBSOCKETS_TCP_TIMEOUT) and would treat that as seconds.
    secure->setTimeout(8);
    Serial.printf("ws ip %s\n", relayIp.toString().c_str());
    ok = secure->connect(relayIp, port, host, KOMM_CA_CERT, nullptr, nullptr);
    if (ok) fd = secure->fd();
  } else if (relayIpOk) {
    Serial.printf("ws ip %s\n", relayIp.toString().c_str());
    auto* plain = static_cast<WiFiClient*>(client);
    ok = plain->connect(relayIp, port, timeout);
    if (ok) fd = plain->fd();
  } else {
    auto* plain = static_cast<WiFiClient*>(client);
    ok = plain->connect(host, port, timeout);
    if (ok) fd = plain->fd();
  }
  if (ok) {
    wsFd = fd;
    clampSocket(fd);
  } else {
    wsFd = -1;
  }
  return ok;
}

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
  // Association is done. Bluetooth may start now, but the relay handshake
  // still needs the radio until the socket is up.
  ensureAirpods();
#if KOMM_A2DP
  esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
#endif
  cacheRelayIp(relayHost.c_str());
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
    if (nvsString(handle, "airname", value)) {
      strncpy(a2dpName, value.c_str(), sizeof(a2dpName) - 1);
      a2dpName[sizeof(a2dpName) - 1] = '\0';
    }
    nvs_close(handle);
  }
  playVolume = prefs.getUChar("vol", playVolume);
  screenBright = prefs.getUChar("bright", screenBright);
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

static uint32_t wifiArmedMs = 0;
static uint32_t wifiBeganMs = 0;
static uint32_t wifiLogMs = 0;

static const char* wifiStatusName(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS: return "vantar";
    case WL_NO_SSID_AVAIL: return "inget nat";
    case WL_SCAN_COMPLETED: return "skannad";
    case WL_CONNECTED: return "kopplad";
    case WL_CONNECT_FAILED: return "nekad";
    case WL_CONNECTION_LOST: return "tappad";
    case WL_DISCONNECTED: return "fran";
    default: return "okand";
  }
}

static void beginWifi() {
  setState(RunState::Wifi);
  // AirPods paging and Wi-Fi association share one radio. Prefer Wi-Fi
  // until the relay socket exists, or the stick stays in WIFI forever.
#if KOMM_A2DP
  esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
#endif
  WiFi.mode(WIFI_STA);
  // Classic Bluetooth and Wi-Fi share the radio. The IDF aborts if modem
  // sleep is off while both are enabled.
  WiFi.setSleep(true);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  wifiArmedMs = millis();
  wifiBeganMs = wifiArmedMs;
  wifiLogMs = wifiArmedMs;
  Serial.printf("wifi start %s\n", wifiSsid.c_str());
  uiDirty = true;
}

static void serviceWifi() {
  if (state != RunState::Wifi) return;
  if (WiFi.status() == WL_CONNECTED) {
    startWebSocket();
    return;
  }
  uint32_t now = millis();
  if (now - wifiLogMs >= 2000) {
    wifiLogMs = now;
    wl_status_t status = WiFi.status();
    Serial.printf("wifi status %d %s\n", static_cast<int>(status), wifiStatusName(status));
  }
  if (!airpodsStarted && wifiArmedMs != 0 && now - wifiArmedMs >= 12000) {
    ensureAirpods();
#if KOMM_A2DP
    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
#endif
  }
  if (wifiBeganMs != 0 && now - wifiBeganMs >= 8000) {
    wifiBeganMs = now;
    Serial.println("wifi omstart");
    WiFi.disconnect(false);
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  }
}

static void beginPtt() {
  if (state == RunState::Provision || state == RunState::Wifi) return;
  pttHeld = true;
  listenLive = true;
  listenSince = millis();
  if (turnId == 0xffffffffu) turnId = 0;
  turnId++;
  hearTurn = false;
  capQueued = false;
  capLive = false;
  ringClear();
  setAirpodMedia(false);
  // Wi-Fi priority drops the AirPods link while the microphone is open.
  // The socket still has to move, so share the radio when the buds are up.
  esp_coex_preference_set(airpodsOn() ? ESP_COEX_PREFER_BALANCE : ESP_COEX_PREFER_WIFI);
  logAudio("rensar");
  M5.Speaker.stop();
  // AirPods already own the radio. Starting the buzzer I2S here asks for
  // another DMA buffer in internal RAM and fails once Bluetooth is up.
  if (airpodsOn() || useSpk2) {
    releaseSpeaker();
    chirpLedc(false);
  } else {
    chirp(false);
    releaseSpeaker();
  }
  if (!startMic()) {
    detailLine = "mic";
    pttHeld = false;
    listenLive = false;
    if (!airpodsOn()) startSpeaker();
    setState(RunState::Error);
    return;
  }
  if (sessionReady) sendPtt("down");
  listenSince = millis();
  setState(RunState::Listen);
  userLine = "";
  uiDirty = true;
  markActivity();
}

static bool activityBusy() {
  return pttHeld || state == RunState::Listen || state == RunState::Think || state == RunState::Speak;
}

static void abortActivity() {
  bool wasPtt = pttHeld;
  pttHeld = false;
  capQueued = false;
  capLive = false;
  stopMic();
  M5.Speaker.stop();
  ringClear();
  setAirpodMedia(false);
  esp_coex_preference_set(airpodsOn() ? ESP_COEX_PREFER_BALANCE : ESP_COEX_PREFER_WIFI);
  if (wasPtt && sessionReady) sendPtt("up");
  if (!airpodsOn() && !M5.Speaker.isRunning()) startSpeaker();
  setupOpen = false;
  swallowPwrClick = true;
  setState(sessionReady ? RunState::Idle : RunState::Connecting);
  uiDirty = true;
  markActivity();
  listenLive = false;
  Serial.println("avbryt");
  logAudio("avbryt");
}

static bool drainMic();

static void endPtt() {
  if (!pttHeld) return;
  pttHeld = false;
  listenLive = false;
  // Stop the reader and let it push the last DMA block before the relay
  // sees ptt up. stopMic() drops capFill, so the send has to happen first.
  if (micTaskOn) {
    portENTER_CRITICAL(&micMux);
    micRun = false;
    portEXIT_CRITICAL(&micMux);
#if KOMM_A2DP
    if (micOpen) i2s_stop(I2S_NUM_0);
#endif
    micWaitIdle();
  }
  drainMic();
  flushCapture();
  capQueued = false;
  capLive = false;
  stopMic();
  if (sessionReady) sendPtt("up");
  if (airpodsOn() || useSpk2) chirpLedc(true);
  else {
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

static void printFitted(const char* text, int maxW);

static const int kSetupRows = 7;

static void bumpVolume() {
  int next = (static_cast<int>(playVolume) / 25 + 1) * 25;
  if (next > 250) next = 0;
  playVolume = static_cast<uint8_t>(next);
  prefs.putUChar("vol", playVolume);
  if (M5.Speaker.isRunning()) M5.Speaker.setVolume(playVolume);
#if KOMM_A2DP
  a2dp.set_volume(avrcpVolume());
#endif
}

static void bumpBright() {
  if (screenBright < 80) screenBright = 100;
  else if (screenBright < 140) screenBright = 170;
  else screenBright = 40;
  prefs.putUChar("bright", screenBright);
  M5.Display.setBrightness(screenBright);
}

static void adjustSetup() {
  if (setupSel == 0) bumpVolume();
  else if (setupSel == 1) toggleOutput();
  else if (setupSel == 2) bumpBright();
  uiDirty = true;
  markActivity();
}

static void setupValue(int row, char* buf, size_t n) {
  buf[0] = 0;
  if (row == 0) snprintf(buf, n, "%u", playVolume);
  else if (row == 1) snprintf(buf, n, "%s", useSpk2 ? "SPK2" : "SUM");
  else if (row == 2) snprintf(buf, n, "%u", screenBright);
  else if (row == 3) {
    if (WiFi.status() == WL_CONNECTED) {
      snprintf(buf, n, "%ddB %s", WiFi.RSSI(), wifiSsid.c_str());
    } else {
      snprintf(buf, n, "ingen");
    }
  } else if (row == 4) {
    snprintf(buf, n, "%s", sessionReady ? relayHost.c_str() : "ej kopplad");
  } else if (row == 5) {
    if (batteryMv > 0) snprintf(buf, n, "%d mV", batteryMv);
    else snprintf(buf, n, "okand");
  } else if (airpodsOn() && a2dpName[0]) {
    snprintf(buf, n, "%s", a2dpName);
  } else if (airpodsOn()) {
    snprintf(buf, n, "kopplad");
  } else if (a2dpName[0]) {
    snprintf(buf, n, "%s", a2dpName);
  } else {
    snprintf(buf, n, "inga");
  }
}

static const char* setupLabel(int row) {
  switch (row) {
    case 0: return "Volym";
    case 1: return "Utgang";
    case 2: return "Ljus";
    case 3: return "WiFi";
    case 4: return "Relay";
    case 5: return "Batteri";
    default: return "Lurar";
  }
}

static void drawSetup() {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(0xFD20, TFT_BLACK);
  M5.Display.setTextWrap(false);
  int lh = lineHeightPx();
  M5.Display.setCursor(4, 2);
  M5.Display.print("INSTALLNING");
  const char* hint = "PWR";
  M5.Display.setCursor(M5.Display.width() - M5.Display.textWidth(hint) - 4, 2);
  M5.Display.print(hint);
  for (int row = 0; row < kSetupRows; row++) {
    int y = 2 + lh + row * lh;
    M5.Display.setCursor(2, y);
    M5.Display.print(row == setupSel ? ">" : " ");
    const char* label = setupLabel(row);
    M5.Display.print(label);
    char value[48];
    setupValue(row, value, sizeof(value));
    int x = 8 + M5.Display.textWidth(label) + 8;
    int room = M5.Display.width() - x - 4;
    if (room < 24) room = 24;
    M5.Display.setCursor(x, y);
    printFitted(value, room);
  }
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
  if (setupOpen) {
    drawSetup();
    uiDirty = false;
    return;
  }
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

static size_t takeCaptured(int16_t* dst, size_t maxn) {
  if (micTaskOn) return micPop(dst, maxn);
  return 0;
}

static bool drainMic() {
  if (!micOpen && !micTaskOn) return false;
  int16_t tmp[64];
  bool any = false;
  for (int spins = 0; spins < 48; spins++) {
    if (capFill >= CAPTURE_SAMPLES) {
      if (!sendCapture(capFill)) return any;
      capFill = 0;
      capLive = true;
    }
    size_t samples = takeCaptured(tmp, 64);
    if (samples == 0) break;
    any = true;
    micQuietMs = millis();
    for (size_t i = 0; i < samples; i++) {
      if (capFill >= CAPTURE_SAMPLES) {
        if (!sendCapture(capFill)) return true;
        capFill = 0;
        capLive = true;
      }
      int32_t sample = tmp[i];
      if (sample > 32767) sample = 32767;
      if (sample < -32768) sample = -32768;
      int32_t mag = sample < 0 ? -sample : sample;
      if (mag > micPeak) micPeak = mag;
      capBuf[capFill++] = static_cast<int16_t>(sample);
    }
  }
  if (capFill >= CAPTURE_SAMPLES && sendCapture(capFill)) {
    capFill = 0;
    capLive = true;
  }
  return any;
}

static void serviceCapture() {
  if (!pttHeld || !micOpen) return;
#if KOMM_A2DP
  if ((GPIO.in1.data & (1UL << (GPIO_NUM_35 - 32))) == 0) {
    abortActivity();
    return;
  }
#endif
  if (!sessionReady) {
    int16_t trash[64];
    for (int i = 0; i < 8; i++) {
      if (takeCaptured(trash, 64) == 0) break;
    }
    return;
  }
  drainMic();
  // A full 20 ms frame is best, but a short press or a stalled DMA queue
  // used to sit under that and never leave the stick. The relay only asks
  // Grok to answer after at least one binary frame.
  if (capFill >= 2 && millis() - micChunkMs >= 100) {
    if (sendCapture(capFill)) {
      capFill = 0;
      capLive = true;
    }
    micChunkMs = millis();
  }
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
  const int total = static_cast<int>(PLAYBACK_HZ);
  for (int i = 0; i < total;) {
    int n = 128;
    if (i + n > total) n = total - i;
    for (int s = 0; s < n; s++) {
      int t = i + s;
      float v;
      if (t < static_cast<int>(PLAYBACK_HZ / 5)) {
        v = sinf(t * 6.2831853f * 440.0f / static_cast<float>(PLAYBACK_HZ));
      } else {
        float phase = t * 6.2831853f * 180.0f / static_cast<float>(PLAYBACK_HZ);
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
#if KOMM_A2DP
    a2dp.set_volume(avrcpVolume());
#endif
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

static void serviceAudioLog() {
  static unsigned long last = 0;
  static uint32_t prevOut = 0;
  static uint32_t prevEmpty = 0;
  size_t queued = airpodsOn() ? stereoAvailFrames() : ringAvailSamples();
  bool hot = queued > 0 || pttHeld || state == RunState::Speak || state == RunState::Listen;
  if (!hot || millis() - last < 500) return;
  last = millis();
  uint32_t outNow = fillOutFrames;
  uint32_t emptyNow = fillEmptyCalls;
  Serial.printf("ljud puls +ut %u +tom %u\n", static_cast<unsigned>(outNow - prevOut),
                static_cast<unsigned>(emptyNow - prevEmpty));
  prevOut = outNow;
  prevEmpty = emptyNow;
  logAudio("puls");
}

static void servicePlayback() {
  if (airpodsOn()) {
    if (!pttHeld && M5.Speaker.isRunning()) releaseSpeaker();
    size_t queued = stereoAvailFrames();
    unsigned long age = lastAudioMs == 0 ? 100000ul : millis() - lastAudioMs;
    // 600 ms at 44.1 kHz. Used only to open the stream, not to keep it open.
    constexpr size_t kPrimeFrames = 26460;
    constexpr unsigned long kTailMs = 600;
    if (pttHeld) {
      airpodLatched = false;
      airpodPrerollLeft = 0;
    } else if (!airpodLatched) {
      airpodLatched = queued >= kPrimeFrames || (queued > 0 && age > kTailMs);
      // 400 ms at 44.1 kHz. Only on the edge that opens the stream.
      if (airpodLatched) airpodPrerollLeft = 17640;
    } else if (queued == 0 && age > kTailMs) {
      airpodLatched = false;
      airpodPrerollLeft = 0;
    }
    bool feed = airpodLatched;
    static bool fed = false;
    if (feed != fed) {
      fed = feed;
      logAudio(feed ? "spela" : "vantar");
    }
    setAirpodMedia(feed);
    // Stay in balance for the whole time the buds are connected, including
    // the wait after the button. Wi-Fi priority during that wait is what
    // drops the link before the answer arrives.
    static int coexMode = -1;
    if (coexMode != 2) {
      coexMode = 2;
      esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
      Serial.println("ljud radio balans");
    }
    if (!feed && state == RunState::Speak && queued == 0 && age > kTailMs) {
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
  if (setupOpen) {
    M5.Display.setBrightness(screenBright);
    return;
  }
  if (state != RunState::Idle) {
    if (state != RunState::Provision) M5.Display.setBrightness(screenBright);
    return;
  }
  unsigned long quiet = millis() - lastActivity;
  if (quiet > DIM_AFTER_MS) M5.Display.setBrightness(24);
  if (quiet < SLEEP_AFTER_MS) return;
#if KOMM_A2DP
  if (a2dpState != ESP_A2D_CONNECTION_STATE_DISCONNECTED) return;
#else
  // Wake pin and power hold are Plus2 pins. S3 boards stay awake.
  return;
#endif
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
  M5.Display.setBrightness(screenBright);
  markActivity();
  wsStarted = false;
  beginWifi();
}

void setup() {
#if KOMM_A2DP
  pinMode(PIN_HOLD, OUTPUT);
  digitalWrite(PIN_HOLD, HIGH);
#endif
  Serial.begin(115200);
  delay(20);
  mbedtls_platform_set_calloc_free(kommTlsCalloc, kommTlsFree);

  auto cfg = M5.config();
#if defined(KOMM_BOARD_STICKS3)
  cfg.fallback_board = m5::board_t::board_M5StickS3;
#elif defined(KOMM_BOARD_CORES3_LITE)
  cfg.fallback_board = m5::board_t::board_M5StackCoreS3;
#else
  cfg.fallback_board = m5::board_t::board_M5StickCPlus2;
#endif
  Serial.printf("kort %s duplex %d a2dp %d\n", kBoard.deviceId, kBoard.duplex ? 1 : 0,
                kBoard.a2dp ? 1 : 0);
  prefs.begin("komm", false);
  loadConfig();
  cfg.internal_mic = true;
#if KOMM_A2DP
  cfg.internal_spk = !useSpk2;
  cfg.external_speaker.hat_spk2 = useSpk2;
#else
  cfg.internal_spk = true;
  cfg.external_speaker.hat_spk2 = false;
#endif
  M5.begin(cfg);
  M5.Display.setRotation(1);
  M5.Display.setFont(&fonts::Font2);
  M5.Display.setTextSize(1);
  M5.Display.setBrightness(screenBright);
  M5.Display.setTextWrap(false);
  M5.BtnB.setHoldThresh(700);
  speakerI2sLive = cfg.internal_spk || useSpk2;
  if (!allocRing()) Serial.println("ljud ring saknas");
  startSpeaker();
  M5.Mic.end();
  replayCap = 256 * 1024;
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
  startMicTask();

  M5.update();
  xTaskCreatePinnedToCore(
      [](void*) {
        uint8_t high = 0;
        uint32_t cutAt = 0;
        for (;;) {
          bool live = listenLive;
#if KOMM_A2DP
          int released = gpio_get_level(static_cast<gpio_num_t>(PIN_BTN_A)) != 0;
          int power = gpio_get_level(GPIO_NUM_35) == 0;
#else
          int released = 0;
          int power = 0;
#endif
          if (!live || !released) high = 0;
          else if (++high >= 2) wantAbort = 1;
          if (live && power) wantAbort = 2;
          uint32_t beat = loopMs;
          bool stalled = beat != 0 && millis() - beat > 700;
          if (live && stalled && (released || power) && cutAt != beat) {
            cutAt = beat;
            int fd = wsFd;
            if (fd >= 0) shutdown(fd, SHUT_RDWR);
            Serial.println("vakt lossar");
          }
          vTaskDelay(pdMS_TO_TICKS(20));
        }
      },
      "vakt", 3072, nullptr, 5, nullptr, 0);
  bool forcePortal = M5.BtnB.isPressed() || wifiSsid.length() == 0;
  if (forcePortal) enterProvision();
  else beginWifi();
  draw();
}

void loop() {
  loopMs = millis();
  M5.update();
  serviceSerial();
  if (wantAbort && listenLive && millis() - listenSince > 250) {
    uint8_t why = wantAbort;
    bool up = gpio_get_level(static_cast<gpio_num_t>(PIN_BTN_A)) != 0;
    if (why == 2 || up) {
      wantAbort = 0;
      if (why == 2) abortActivity();
      else if (pttHeld) {
        Serial.println("ptt upp");
        endPtt();
      }
    }
  }
  if (state == RunState::Provision) {
    dns.processNextRequest();
    portal.handleClient();
    if (uiDirty) draw();
    return;
  }

  if (state != RunState::Provision && millis() > 1500 &&
      (M5.BtnPWR.wasClicked() || (activityBusy() && M5.BtnPWR.isPressed()))) {
    if (activityBusy()) {
      abortActivity();
    } else if (swallowPwrClick) {
      swallowPwrClick = false;
    } else {
      setupOpen = !setupOpen;
      uiDirty = true;
      markActivity();
      Serial.println(setupOpen ? "installning oppnas" : "installning stangs");
    }
  }

  if (state == RunState::Wifi) {
    serviceWifi();
  } else if (wsStarted) {
    if (pttHeld) serviceCapture();
    webSocket.loop();
  }
  if (setupOpen) {
    if (M5.BtnB.wasClicked()) {
      setupSel = (setupSel + 1) % kSetupRows;
      uiDirty = true;
      markActivity();
    }
    if (M5.BtnA.wasClicked()) adjustSetup();
  } else {
    if (M5.BtnA.wasPressed()) {
      if (state == RunState::Wifi || state == RunState::Provision) {
        Serial.println("ptt vantar nat");
      } else {
        Serial.println("ptt ned");
        beginPtt();
      }
    }
    if (M5.BtnA.wasReleased()) {
      Serial.println("ptt upp");
      endPtt();
    }
    if (!pttHeld && state != RunState::Listen) {
      if (M5.BtnB.wasReleaseFor(700)) toggleOutput();
      else if (M5.BtnB.wasClicked()) scrollAnswer();
    }
  }

  serviceAirpods();
  if (pttHeld) serviceCapture();
  servicePlayback();
  if (state == RunState::Think && !pttHeld && millis() - thinkSince > 20000 &&
      stereoAvailFrames() == 0 && ringAvailSamples() == 0) {
    Serial.println("vantan slut");
    setState(RunState::Idle);
  }
  serviceAudioLog();

  if (millis() - lastBatteryMs > 10000) {
    lastBatteryMs = millis();
    int next = M5.Power.getBatteryVoltage();
    if (next != batteryMv) {
      batteryMv = next;
      uiDirty = true;
    }
  }
  if (setupOpen) {
    static unsigned long lastSetupDraw = 0;
    if (millis() - lastSetupDraw > 1000) {
      lastSetupDraw = millis();
      uiDirty = true;
    }
  }
  maybeSleep();
  if (uiDirty) {
    servicePlayback();
    draw();
  }
}
