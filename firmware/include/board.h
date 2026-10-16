#pragma once

// One session talks to the relay. The board only says which ears and mouth
// it has. Plus2 is the floor: classic A2DP and a shared GPIO for mic and
// speaker. StickS3 and CoreS3 Lite are ESP32-S3 with a real speaker and no
// classic Bluetooth, so AirPods are not part of their profile.

struct BoardProfile {
  const char* deviceId;
  bool a2dp;
  bool duplex;
  uint32_t captureHz;
  uint32_t playbackHz;
};

#if defined(KOMM_BOARD_STICKS3)
static constexpr BoardProfile kBoard{"sticks3", false, false, 16000, 24000};
#ifndef KOMM_A2DP
#define KOMM_A2DP 0
#endif
#elif defined(KOMM_BOARD_CORES3_LITE)
static constexpr BoardProfile kBoard{"cores3-lite", false, true, 16000, 24000};
#ifndef KOMM_A2DP
#define KOMM_A2DP 0
#endif
#else
static constexpr BoardProfile kBoard{"stickc-plus2", true, false, 16000, 44100};
#ifndef KOMM_A2DP
#define KOMM_A2DP 1
#endif
#endif
