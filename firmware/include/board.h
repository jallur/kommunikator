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
// 16 kHz on the wire. A2DP still plays 44.1 kHz after a local upsample.
// 44.1 kHz PCM does not fit beside the A2DP stream on this radio, so the
// story plays smoothly for the first buffered second and then stutters.
static constexpr BoardProfile kBoard{"stickc-plus2", true, false, 16000, 16000};
#ifndef KOMM_A2DP
#define KOMM_A2DP 1
#endif
#endif
