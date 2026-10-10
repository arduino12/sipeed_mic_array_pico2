// Sipeed R6+1 mic array on a Raspberry Pi Pico 2.
//
// core 0: USB (TinyUSB runs from IRQs), console, DOA, steering/gate, LEDs
// core 1: I2S capture -> beamformer -> USB audio + 16 kHz DOA feed
#include <Adafruit_TinyUSB.h>
#include <Arduino.h>

#include "config.h"
#include "console.h"
#include "doa.h"
#include "dsp.h"
#include "geometry.h"
#include "i2s_array.h"
#include "leds.h"
#include "settings.h"
#include "shared.h"
#include "usb_audio.h"

Control g_ctl;
Stats g_stats;
SpscRing<16384> g_usb_ring;
SpscRing<65536> g_doa_ring;

static volatile bool s_core0_ready = false;
static bool s_i2s_ok = false;

void setup() {
    settings::load();
    auto &st = settings::get();
    g_ctl.mode = st.mode;
    g_ctl.gain_db = st.gain_db;
    usb_audio::begin(st.mode);

    leds::begin();
    s_i2s_ok = i2s::begin();
    doa::init();
    s_core0_ready = true;
}

// Beam steering and gate follow the DOA estimate.
static void update_steering(uint32_t now_ms) {
    static uint32_t last_on_target_ms = 0;
    const auto &st = settings::get();
    const auto &r = doa::result();
    float steer = st.steer_auto ? r.az_smooth : geom::wrap360(st.steer_az - st.az_offset);
    g_ctl.steer_az = steer;

    bool on_target = r.active && (st.steer_auto || fabsf(geom::angdiff(r.az, steer)) <= st.gate_width);
    if (on_target) last_on_target_ms = now_ms;
    g_ctl.gate_open = now_ms - last_on_target_ms < 400;  // hangover across speech gaps
    g_ctl.gate_enabled = st.gate;
}

static void update_leds(uint32_t now_ms) {
    static uint32_t last = 0, last_active_ms = 0;
    static float shown_az = 0;
    if (now_ms - last < 33) return;
    last = now_ms;
    const auto &st = settings::get();
    switch (st.led_mode) {
        case leds::LED_DOA: {
            const auto &r = doa::result();
            if (r.active) {
                last_active_ms = now_ms;
                shown_az = r.az_smooth;
            }
            float age = (now_ms - last_active_ms) / 1000.0f;
            float strength = r.active ? fminf(1.0f, 0.4f + 2.0f * r.conf) : fmaxf(0.0f, 1.0f - age);
            leds::render_doa(shown_az, strength, g_ctl.steer_az, !st.steer_auto, now_ms);
            break;
        }
        case leds::LED_STATIC:
            leds::fill(st.led_rgb[0], st.led_rgb[1], st.led_rgb[2]);
            break;
        case leds::LED_OFF:
            leds::fill(0, 0, 0);
            break;
        default:  // LED_MANUAL: pixels set by `led set`
            break;
    }
    if (!s_i2s_ok) leds::fill((now_ms / 250) % 2 ? 255 : 0, 0, 0);  // blink red: capture failed
    leds::show(st.led_bright);
}

void loop() {
    const uint32_t now = millis();
    console::poll();
    if (doa::poll()) update_steering(now);
    update_leds(now);
    console::telemetry(now);
}

void setup1() {
    while (!s_core0_ready) tight_loop_contents();
    dsp::init();
}

void loop1() { dsp::poll(); }
