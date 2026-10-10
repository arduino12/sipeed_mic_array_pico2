#pragma once
#include <stdint.h>

#define FW_VERSION "0.1.0"

// ---------------------------------------------------------------------------
// Pins (Raspberry Pi Pico 2)
// ---------------------------------------------------------------------------
constexpr unsigned PIN_I2S_BCK = 10;  // MIC_CK
constexpr unsigned PIN_I2S_WS = 11;   // MIC_WS -- must be BCK + 1 (PIO side-set pair)
constexpr unsigned PIN_I2S_D0 = 12;   // MIC_D0..D3 on GP12..GP15 -- one `in pins, 4`
constexpr unsigned PIN_LED_CK = 18;   // SPI0 SCK
constexpr unsigned PIN_LED_DA = 19;   // SPI0 TX

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------
constexpr uint32_t FS = 48000;
constexpr uint32_t I2S_BCK_PER_FRAME = 64;
constexpr int BLOCK = 48;      // frames per 1 ms processing block
constexpr int NUM_SLOTS = 8;   // 4 data lines x (left, right)
constexpr int NUM_MICS = 7;    // 0..5 = ring, 6 = centre

// I2S slot of each mic, slot = 2 * data_line + (right ? 1 : 0).
// From the schematic: D0 = U7(L, mic0) + U8(R, mic1), D1 = U13(L) + U14(R),
// D2 = U19(L) + U20(R), D3 = U1 centre mic (R). D3 left is not populated.
constexpr int MIC_SLOT[NUM_MICS] = {0, 1, 2, 3, 4, 5, 7};
constexpr int EMPTY_SLOT = 6;

// Geometry. Azimuth convention used everywhere (DOA, steering, LEDs):
// degrees clockwise from mic 0, looking at the LED side. Ring mic k sits at
// 60 * k degrees on a 40 mm radius; LED i sits at 30 * i degrees.
constexpr float MIC_RADIUS_M = 0.040f;
constexpr float SPEED_OF_SOUND = 343.0f;

// ---------------------------------------------------------------------------
// Direction of arrival (runs on core 0 at 16 kHz)
// ---------------------------------------------------------------------------
constexpr uint32_t FS_DOA = 16000;
constexpr int DECIM = FS / FS_DOA;  // 3

// ---------------------------------------------------------------------------
// LEDs
// ---------------------------------------------------------------------------
constexpr int NUM_LEDS = 12;
constexpr uint8_t LED_BRIGHT_MAX = 31;

// ---------------------------------------------------------------------------
// USB identity. pid.codes test VID; each audio mode gets its own PID so
// Windows never reuses a cached descriptor from the other mode.
// ---------------------------------------------------------------------------
constexpr uint16_t USB_VID_MICARRAY = 0x1209;
constexpr uint16_t USB_PID_STEREO = 0xA6E1;
constexpr uint16_t USB_PID_RAW8 = 0xA6E3;  // A6E2 was an early 8-ch layout Windows cached as 2-ch
