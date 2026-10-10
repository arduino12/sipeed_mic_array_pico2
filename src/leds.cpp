#include "leds.h"

#include <hardware/gpio.h>
#include <hardware/spi.h>
#include <math.h>

#include "config.h"
#include "geometry.h"

namespace leds {

static uint8_t s_rgb[NUM_LEDS][3];

void begin() {
    spi_init(spi0, 4000000);
    spi_set_format(spi0, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(PIN_LED_CK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_LED_DA, GPIO_FUNC_SPI);
    fill(0, 0, 0);
    show(1);
}

void set_pixel(int i, uint8_t r, uint8_t g, uint8_t b) {
    if (i < 0 || i >= NUM_LEDS) return;
    s_rgb[i][0] = r;
    s_rgb[i][1] = g;
    s_rgb[i][2] = b;
}

void fill(uint8_t r, uint8_t g, uint8_t b) {
    for (int i = 0; i < NUM_LEDS; ++i) set_pixel(i, r, g, b);
}

void show(uint8_t bright5) {
    uint8_t buf[4 + NUM_LEDS * 4 + 8] = {0};
    if (bright5 > LED_BRIGHT_MAX) bright5 = LED_BRIGHT_MAX;
    for (int i = 0; i < NUM_LEDS; ++i) {
        uint8_t *p = &buf[4 + 4 * i];
        p[0] = 0xE0 | bright5;
        p[1] = s_rgb[i][2];  // SK9822 order: B, G, R
        p[2] = s_rgb[i][1];
        p[3] = s_rgb[i][0];
    }
    spi_write_blocking(spi0, buf, sizeof(buf));
}

void render_doa(float az, float strength, float steer_az, bool show_steer, const Sector &sec, uint32_t now_ms) {
    if (strength > 1) strength = 1;
    // idle: faint slow breathing so it is obvious the board is alive
    float idle = 6.0f + 4.0f * sinf(now_ms * 0.002f);
    for (int i = 0; i < NUM_LEDS; ++i) {
        float d = fabsf(geom::angdiff(i * 30.0f, az));
        float blob = strength * expf(-(d * d) / (2.0f * 22.0f * 22.0f));
        float r = 255.0f * blob, g = 120.0f * blob * blob, b = idle * (1.0f - blob);
        // an LED counts as in the sector if any part of its 30 deg slice is
        const float ds = fabsf(geom::angdiff(i * 30.0f, sec.center));
        if (sec.on && ds <= sec.halfwidth + 15.0f) {
            if (sec.open) g += 90.0f;
            else b += 60.0f;
        } else if (!sec.on && show_steer && fabsf(geom::angdiff(i * 30.0f, steer_az)) < 15.0f) {
            b += 70.0f;
            g += 20.0f;
        }
        set_pixel(i, (uint8_t)fminf(r, 255.f), (uint8_t)fminf(g, 255.f), (uint8_t)fminf(b, 255.f));
    }
}

}  // namespace leds
