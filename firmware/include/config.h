#pragma once

#include <Arduino.h>

#if __has_include("secrets.h")
#include "secrets.h"
#endif
#include "isrg_root_x1.h"
#include "board.h"

#ifndef KOMM_RELAY_HOST
#define KOMM_RELAY_HOST "trek.aldenback.com"
#endif
#ifndef KOMM_RELAY_PORT
#define KOMM_RELAY_PORT 443
#endif
#ifndef KOMM_USE_TLS
#define KOMM_USE_TLS 1
#endif
#ifndef KOMM_DEVICE_TOKEN
#define KOMM_DEVICE_TOKEN ""
#endif
#ifndef KOMM_CA_CERT
#define KOMM_CA_CERT ISRG_ROOT_X1
#endif

// Rates come from the board profile. Plus2 asks for 16 kHz on the wire
// and upsamples to 44.1 kHz for AirPods. S3 boards ask for 24 kHz and play
// it on their own speaker.
static constexpr uint32_t CAPTURE_HZ = kBoard.captureHz;
static constexpr uint32_t PLAYBACK_HZ = kBoard.playbackHz;
static const size_t CAPTURE_SAMPLES = 320;  // 20 ms

// StickC-Plus2 pin map:
// https://docs.m5stack.com/en/core/StickC-Plus2
static const int PIN_HOLD = 4;       // keep power on after button C wake
static const int PIN_BUZZER = 2;     // passive buzzer
static const int PIN_MIC_CLK = 0;    // SPM1423 CLK, shared with SPK2 LRCLK
static const int PIN_MIC_DATA = 34;  // SPM1423 DATA
static const int PIN_I2S_BCLK = 26;  // Hat SPK2 BCLK
static const int PIN_I2S_LRCLK = 0;  // Hat SPK2 LRCLK
static const int PIN_I2S_DOUT = 25;  // Hat SPK2 DIN
static const int PIN_BTN_A = 37;
static const int PIN_BATTERY = 38;

static const uint32_t DIM_AFTER_MS = 20000;
static const uint32_t SLEEP_AFTER_MS = 180000;
