#include "settings.h"

#include <EEPROM.h>

#include "shared.h"

namespace settings {

static constexpr uint32_t MAGIC = 0x4D494341;  // "MICA"
static constexpr uint16_t VERSION = 2;
static Settings s;

void defaults() {
    s = {};
    s.magic = MAGIC;
    s.version = VERSION;
    s.mode = MODE_STEREO;
    s.steer_auto = 1;
    s.steer_az = 0;
    s.gate = 0;
    s.gate_width = 30;
    s.led_mode = 0;
    s.led_bright = 4;
    s.led_rgb[0] = 0;
    s.led_rgb[1] = 40;
    s.led_rgb[2] = 80;
    s.az_offset = 0;
    s.gain_db = 18;  // speech at 1 m lands near -40 dBFS; clips above ~108 dB SPL
    s.telem_hz = 10;
    s.telem_fmt = TELEM_JSON;
}

void load() {
    EEPROM.begin(256);
    EEPROM.get(0, s);
    if (s.magic != MAGIC || s.version != VERSION) defaults();
}

bool save() {
    EEPROM.put(0, s);
    return EEPROM.commit();
}

Settings &get() { return s; }

}  // namespace settings
