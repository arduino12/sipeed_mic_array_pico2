#pragma once
// 12x SK9822 (APA102-compatible) on SPI0. LED i sits at 30 * i degrees,
// clockwise from mic 0 (the chain runs U4..U11 then U12..U3).
#include <stdint.h>

namespace leds {

enum Mode : uint8_t { LED_DOA = 0, LED_STATIC = 1, LED_MANUAL = 2, LED_OFF = 3 };

void begin();
void set_pixel(int i, uint8_t r, uint8_t g, uint8_t b);
void fill(uint8_t r, uint8_t g, uint8_t b);
void show(uint8_t bright5);

struct Sector {
    bool on;          // pass-only-this-sector mode
    float center;     // deg, board frame
    float halfwidth;  // deg
    bool open;        // gate currently passing audio
};

// Direction display: a soft blob at az (strength 0..1), plus a dim marker at
// the beam steering direction when show_steer is set. With a sector, its LEDs
// glow blue while muted and green while passing audio.
void render_doa(float az, float strength, float steer_az, bool show_steer, const Sector &sec, uint32_t now_ms);

}  // namespace leds
