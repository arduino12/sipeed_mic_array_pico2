#pragma once
// Persistent settings (flash, via the core's EEPROM emulation). `save` writes
// them; a mode change saves and reboots so USB re-enumerates.
#include <stdint.h>

namespace settings {

enum TelemFmt : uint8_t { TELEM_JSON = 0, TELEM_ODAS = 1 };

struct Settings {
    uint32_t magic;
    uint16_t version;
    uint8_t mode;          // AudioMode
    uint8_t steer_auto;    // 1 = beam follows the DOA
    float steer_az;        // fixed steering direction (deg)
    uint8_t gate;          // 1 = mute the beam when the steered direction is quiet
    float gate_width;      // +- degrees around steer_az that count as "on target"
    uint8_t led_mode;      // leds::Mode
    uint8_t led_bright;    // 1..31 (SK9822 global current)
    uint8_t led_rgb[3];    // LED_STATIC colour
    float az_offset;       // added to reported/displayed azimuth
    float gain_db;         // USB audio gain
    uint8_t telem_hz;      // 0 = off
    uint8_t telem_fmt;     // TelemFmt
    float mute_db;         // output level while the gate is closed; <= -90 means digital silence
    float width;           // stereo width (deg): L/R beams at steer -+ width/2, 0 = identical channels
    uint8_t lookahead_ms;  // output delay so the gate decision precedes the audio
};

void load();
bool save();
void defaults();
Settings &get();

}  // namespace settings
