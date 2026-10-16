// AirPods pairing test for M5StickC Plus2.
// Flash with: pio run -e airpods-test -t upload
// This replaces the voice firmware until env m5stick-c-plus2 is uploaded.
//
// Open the AirPods case, hold the back button until the light blinks white.
// The stick connects to the first device whose name contains "AirPod"
// and plays a tone, a pause, then a lower sound, over and over.

#include <M5Unified.h>
#include <WiFi.h>
#include <BluetoothA2DPSource.h>

#include <cmath>
#include <cstring>

static const int PIN_HOLD = 4;

static BluetoothA2DPSource a2dp;

static volatile int linkState = 0;
static char peerName[48] = "";
static bool uiDirty = true;

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

static bool pickDevice(const char* name, esp_bd_addr_t, int rssi) {
  if (name == nullptr || name[0] == '\0') return false;
  Serial.printf("bt namn %s rssi %d\n", name, rssi);
  if (!containsAirpod(name)) return false;
  strncpy(peerName, name, sizeof(peerName) - 1);
  peerName[sizeof(peerName) - 1] = '\0';
  uiDirty = true;
  Serial.printf("bt val %s\n", peerName);
  return true;
}

static void onConnection(esp_a2d_connection_state_t state, void*) {
  linkState = static_cast<int>(state);
  uiDirty = true;
  Serial.printf("bt lage %d\n", linkState);
}

static int32_t fillFrames(Frame* frame, int32_t count) {
  static float phase = 0.0f;
  static uint32_t tick = 0;
  const float twoPi = 6.2831853f;
  for (int32_t i = 0; i < count; i++) {
    uint32_t cycle = tick++ % (44100u * 4u);
    float freq = 440.0f;
    float amp = 0.0f;
    if (cycle < 44100u) {
      amp = 9000.0f;
    } else if (cycle >= 44100u + 6000u) {
      freq = 180.0f;
      amp = 9000.0f;
    }
    phase += twoPi * freq / 44100.0f;
    if (phase > twoPi) phase -= twoPi;
    float v = sinf(phase);
    if (freq < 200.0f) {
      v = sinf(phase) * 0.6f + sinf(phase * 2.0f) * 0.25f + sinf(phase * 3.0f) * 0.15f;
    }
    int sample = static_cast<int>(v * amp);
    if (sample > 32767) sample = 32767;
    if (sample < -32768) sample = -32768;
    frame[i].channel1 = static_cast<int16_t>(sample);
    frame[i].channel2 = frame[i].channel1;
  }
  return count;
}

static const char* stateText() {
  switch (linkState) {
    case ESP_A2D_CONNECTION_STATE_CONNECTING: return "KOPPLAR";
    case ESP_A2D_CONNECTION_STATE_CONNECTED: return "KOPPLAD";
    case ESP_A2D_CONNECTION_STATE_DISCONNECTING: return "SLAPPER";
    default: return "SOKER";
  }
}

static void draw() {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(0xFD20, TFT_BLACK);
  M5.Display.setCursor(4, 4);
  M5.Display.print("AIRPODS");
  M5.Display.setCursor(4, 28);
  M5.Display.print(stateText());
  M5.Display.setCursor(4, 52);
  if (peerName[0] != '\0') M5.Display.print(peerName);
  else M5.Display.print("Oppna fodral");
  M5.Display.setCursor(4, 84);
  if (linkState == ESP_A2D_CONNECTION_STATE_CONNECTED) M5.Display.print("Hor tonen");
  else M5.Display.print("Vitt blink");
  uiDirty = false;
}

void setup() {
  pinMode(PIN_HOLD, OUTPUT);
  digitalWrite(PIN_HOLD, HIGH);
  Serial.begin(115200);
  delay(50);

  auto cfg = M5.config();
  cfg.fallback_board = m5::board_t::board_M5StickCPlus2;
  cfg.internal_mic = false;
  cfg.internal_spk = false;
  M5.begin(cfg);
  M5.Display.setRotation(1);
  M5.Display.setFont(&fonts::Font2);
  M5.Display.setTextSize(1);
  M5.Display.setBrightness(170);
  draw();

  WiFi.mode(WIFI_OFF);
  Serial.println("bt start, satt AirPods i parning");
  a2dp.set_local_name("Kommunikator");
  a2dp.set_ssid_callback(pickDevice);
  a2dp.set_on_connection_state_changed(onConnection, nullptr);
  a2dp.set_auto_reconnect(true);
  a2dp.set_ssp_enabled(true);
  a2dp.set_reset_ble(true);
  a2dp.set_volume(100);
  a2dp.start(fillFrames);
}

void loop() {
  M5.update();
  if (uiDirty) draw();
  delay(50);
}
