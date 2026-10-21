#pragma once

// One session talks to the relay. The board only says which ears, mouth
// and fingers it has. Plus2 is the floor: classic A2DP and a shared GPIO
// for mic and speaker. StickS3 and the CoreS3 family are ESP32-S3 with a
// real speaker and no classic Bluetooth, so AirPods are not part of their
// profile. CoreS3, CoreS3-SE and CoreS3 Lite share the touch panel; only
// SE is missing the GC0308, and that is probed at runtime.

struct BoardProfile {
  const char* deviceId;
  bool a2dp;
  bool duplex;
  bool touchUi;
  uint32_t captureHz;
  uint32_t playbackHz;
};

#if defined(KOMM_BOARD_CORES3_LITE) && !defined(KOMM_BOARD_CORES3)
#define KOMM_BOARD_CORES3
#endif

#if defined(KOMM_BOARD_STICKS3)
static constexpr BoardProfile kBoard{"sticks3", false, false, false, 16000, 24000};
#ifndef KOMM_A2DP
#define KOMM_A2DP 0
#endif
#elif defined(KOMM_BOARD_CORES3)
// Same image for CoreS3, CoreS3-SE and CoreS3 Lite. The camera button
// checks the GC0308 and says so when the sensor is not fitted.
static constexpr BoardProfile kBoard{"cores3", false, true, true, 16000, 24000};
#ifndef KOMM_A2DP
#define KOMM_A2DP 0
#endif
#else
// 16 kHz on the wire. A2DP still plays 44.1 kHz after a local upsample.
// 44.1 kHz PCM does not fit beside the A2DP stream on this radio, so the
// story plays smoothly for the first buffered second and then stutters.
static constexpr BoardProfile kBoard{"stickc-plus2", true, false, false, 16000, 16000};
#ifndef KOMM_A2DP
#define KOMM_A2DP 1
#endif
#endif
