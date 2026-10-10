#include "console.h"

#include <Adafruit_TinyUSB.h>
#include <Arduino.h>
#include <hardware/clocks.h>
#include <hardware/watchdog.h>
#include <pico/bootrom.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "doa.h"
#include "geometry.h"
#include "i2s_array.h"
#include "leds.h"
#include "settings.h"
#include "shared.h"
#include "usb_audio.h"

namespace console {

static char s_line[128];
static int s_len = 0;
static char s_out[768];

static const char *mode_name(uint8_t m) { return m == MODE_RAW8 ? "raw8" : "stereo"; }
static const char *src_name(uint8_t s) { return s == SRC_TONE ? "tone" : (s == SRC_SIM ? "sim" : "mics"); }
static const char *led_name(uint8_t m) {
    static const char *n[] = {"doa", "static", "manual", "off"};
    return m < 4 ? n[m] : "?";
}

static void reply(const char *s) {
    Serial.write(s, strlen(s));
    Serial.write('\n');
    Serial.flush();
}

static void err(const char *msg) {
    snprintf(s_out, sizeof(s_out), "{\"err\":\"%s\"}", msg);
    reply(s_out);
}

static void ok(const char *cmd) {
    snprintf(s_out, sizeof(s_out), "{\"ok\":\"%s\"}", cmd);
    reply(s_out);
}

static bool parse_f(const char *s, float &v) {
    if (!s) return false;
    char *end;
    v = strtof(s, &end);
    return end != s;
}

static void cmd_info() {
    const auto &st = settings::get();
    const bool sector = !st.steer_auto && st.gate;
    snprintf(s_out, sizeof(s_out),
             "{\"ok\":\"info\",\"fw\":\"%s\",\"mode\":\"%s\",\"src\":\"%s\",\"sys_hz\":%lu,\"pio_div\":%.4f,"
             "\"usb_mounted\":%d,\"streaming\":%d,\"steer\":\"%s\",\"steer_az\":%.1f,\"gate\":%d,"
             "\"gate_width\":%.1f,\"sector\":%d,\"sector_from\":%.1f,\"sector_to\":%.1f,\"mute_db\":%.0f,"
             "\"width\":%.0f,\"lookahead_ms\":%u,\"gain_db\":%.1f,\"led\":\"%s\",\"bright\":%u,\"offset\":%.1f,"
             "\"telem_hz\":%u,\"telem_fmt\":\"%s\",\"i2s_bit_offset\":%d}",
             FW_VERSION, mode_name(g_ctl.mode), src_name(g_ctl.source), (unsigned long)clock_get_hz(clk_sys),
             i2s::clkdiv(), usb_audio::mounted(), usb_audio::streaming(), st.steer_auto ? "auto" : "fixed",
             st.steer_az, st.gate, st.gate_width, sector, geom::wrap360(st.steer_az - st.gate_width),
             geom::wrap360(st.steer_az + st.gate_width), st.mute_db, st.width, st.lookahead_ms, st.gain_db,
             led_name(st.led_mode), st.led_bright, st.az_offset, st.telem_hz,
             st.telem_fmt == settings::TELEM_ODAS ? "odas" : "json", g_ctl.i2s_bit_offset);
    reply(s_out);
}

static void cmd_stats() {
    snprintf(s_out, sizeof(s_out),
             "{\"ok\":\"stats\",\"uptime_ms\":%lu,\"blocks\":%lu,\"core1_load\":%.3f,\"doa_load\":%.3f,"
             "\"dma_overruns\":%lu,\"usb_ring_full\":%lu,\"doa_ring_full\":%lu,\"usb_packets\":%lu,"
             "\"usb_underruns\":%lu,\"usb_restarts\":%lu,\"doa_hops\":%lu}",
             (unsigned long)millis(), (unsigned long)g_stats.blocks,
             (float)g_stats.core1_busy_us / g_stats.core1_window_us,
             (float)g_stats.doa_busy_us / g_stats.doa_window_us, (unsigned long)g_stats.dma_overruns,
             (unsigned long)g_stats.usb_ring_full, (unsigned long)g_stats.doa_ring_full,
             (unsigned long)g_stats.usb_packets, (unsigned long)g_stats.usb_underruns,
             (unsigned long)g_stats.usb_restarts, (unsigned long)g_stats.doa_hops);
    reply(s_out);
}

static void cmd_levels() {
    int n = snprintf(s_out, sizeof(s_out), "{\"ok\":\"levels\",\"rms_db\":[");
    for (int s = 0; s < NUM_SLOTS; ++s)
        n += snprintf(s_out + n, sizeof(s_out) - n, "%s%.1f", s ? "," : "", g_stats.level_db[s]);
    n += snprintf(s_out + n, sizeof(s_out) - n, "],\"peak_db\":[");
    for (int s = 0; s < NUM_SLOTS; ++s)
        n += snprintf(s_out + n, sizeof(s_out) - n, "%s%.1f", s ? "," : "", g_stats.peak_db[s]);
    n += snprintf(s_out + n, sizeof(s_out) - n, "],\"dc\":[");
    for (int s = 0; s < NUM_SLOTS; ++s)
        n += snprintf(s_out + n, sizeof(s_out) - n, "%s%.4f", s ? "," : "", g_stats.dc[s]);
    snprintf(s_out + n, sizeof(s_out) - n, "],\"slots\":\"D0L,D0R,D1L,D1R,D2L,D2R,D3L(empty),D3R(centre)\"}");
    reply(s_out);
}

static void cmd_map() {
    const float *m = doa::srp_map();
    int n = snprintf(s_out, sizeof(s_out), "{\"ok\":\"map\",\"step_deg\":%.1f,\"map\":[", 360.0f / doa::GRID);
    for (int g = 0; g < doa::GRID; ++g) n += snprintf(s_out + n, sizeof(s_out) - n, "%s%.2f", g ? "," : "", m[g]);
    snprintf(s_out + n, sizeof(s_out) - n, "]}");
    reply(s_out);
}

static void dispatch(char *line) {
    char *argv[6] = {};
    int argc = 0;
    for (char *tok = strtok(line, " \t"); tok && argc < 6; tok = strtok(nullptr, " \t")) argv[argc++] = tok;
    if (!argc) return;
    auto &st = settings::get();
    const char *c = argv[0];
    float v;

    if (!strcmp(c, "info")) {
        cmd_info();
    } else if (!strcmp(c, "stats")) {
        cmd_stats();
    } else if (!strcmp(c, "levels")) {
        cmd_levels();
    } else if (!strcmp(c, "map")) {
        cmd_map();
    } else if (!strcmp(c, "raw") && argc == 2) {
        // raw <slot>: 32 consecutive raw words of one I2S slot, hex
        int slot = atoi(argv[1]) & 7;
        uint32_t f = i2s::write_frame() - 40;
        int n = snprintf(s_out, sizeof(s_out), "{\"ok\":\"raw\",\"slot\":%d,\"words\":[", slot);
        for (int i = 0; i < 32; ++i)
            n += snprintf(s_out + n, sizeof(s_out) - n, "%s\"%08lX\"", i ? "," : "",
                          (unsigned long)i2s::raw_word(f + i, slot >> 1, slot & 1));
        snprintf(s_out + n, sizeof(s_out) - n, "]}");
        reply(s_out);
    } else if (!strcmp(c, "bench")) {
        doa::bench(s_out, sizeof(s_out));
        reply(s_out);
    } else if (!strcmp(c, "mode") && argc == 2) {
        uint8_t m;
        if (!strcmp(argv[1], "stereo")) m = MODE_STEREO;
        else if (!strcmp(argv[1], "raw8")) m = MODE_RAW8;
        else return err("mode stereo|raw8");
        if (m == st.mode) return ok("mode");
        st.mode = m;
        settings::save();
        ok("mode");
        delay(200);
        watchdog_reboot(0, 0, 10);
        while (true) tight_loop_contents();
    } else if (!strcmp(c, "steer") && argc == 2) {
        if (!strcmp(argv[1], "auto")) {
            st.steer_auto = 1;
        } else if (parse_f(argv[1], v)) {
            st.steer_auto = 0;
            st.steer_az = geom::wrap360(v);
        } else {
            return err("steer auto|<deg>");
        }
        ok("steer");
    } else if (!strcmp(c, "gate") && argc == 2) {
        if (!strcmp(argv[1], "off")) st.gate = 0;
        else if (!strcmp(argv[1], "on")) st.gate = 1;
        else if (parse_f(argv[1], v) && v > 0 && v <= 180) {
            st.gate = 1;
            st.gate_width = v;
        } else return err("gate off|on|<halfwidth_deg>");
        ok("gate");
    } else if (!strcmp(c, "sector") && argc >= 2) {
        // sector <from> <to>: pass only sound arriving between the two angles
        // (clockwise from <from>), silence otherwise. sector off: follow mode.
        float to;
        if (!strcmp(argv[1], "off")) {
            st.gate = 0;
            st.steer_auto = 1;
        } else if (argc == 3 && parse_f(argv[1], v) && parse_f(argv[2], to)) {
            float span = geom::wrap360(to - v);
            if (span < 5.0f) return err("sector must span at least 5 deg (clockwise from <from> to <to>)");
            st.steer_auto = 0;
            st.steer_az = geom::wrap360(v + 0.5f * span);
            st.gate = 1;
            st.gate_width = 0.5f * span;
        } else {
            return err("sector <from_deg> <to_deg> | sector off");
        }
        ok("sector");
    } else if (!strcmp(c, "mute") && argc == 2) {
        // level outside the sector: "full" = digital silence, or N dB below
        if (!strcmp(argv[1], "full")) st.mute_db = -200;
        else if (parse_f(argv[1], v) && fabsf(v) <= 80) st.mute_db = -fabsf(v);
        else return err("mute full|<dB 0..80>");
        ok("mute");
    } else if (!strcmp(c, "width") && argc == 2 && parse_f(argv[1], v) && v >= 0 && v <= 180) {
        st.width = v;
        ok("width");
    } else if (!strcmp(c, "lookahead") && argc == 2 && parse_f(argv[1], v) && v >= 0 && v <= 99) {
        st.lookahead_ms = (uint8_t)v;
        ok("lookahead");
    } else if (!strcmp(c, "gain") && argc == 2 && parse_f(argv[1], v) && v >= -20 && v <= 48) {
        st.gain_db = v;
        g_ctl.gain_db = v;
        ok("gain");
    } else if (!strcmp(c, "offset") && argc == 2 && parse_f(argv[1], v)) {
        st.az_offset = geom::wrap360(v);
        ok("offset");
    } else if (!strcmp(c, "led") && argc >= 2) {
        const char *a = argv[1];
        if (!strcmp(a, "doa")) st.led_mode = leds::LED_DOA;
        else if (!strcmp(a, "off")) st.led_mode = leds::LED_OFF;
        else if (!strcmp(a, "static") && argc == 5) {
            st.led_mode = leds::LED_STATIC;
            for (int i = 0; i < 3; ++i) st.led_rgb[i] = (uint8_t)atoi(argv[2 + i]);
        } else if (!strcmp(a, "set") && argc == 6) {
            st.led_mode = leds::LED_MANUAL;
            leds::set_pixel(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), atoi(argv[5]));
        } else if (!strcmp(a, "bright") && argc == 3) {
            int b = atoi(argv[2]);
            if (b < 1 || b > LED_BRIGHT_MAX) return err("bright 1..31");
            st.led_bright = (uint8_t)b;
        } else return err("led doa|off|static r g b|set i r g b|bright n");
        ok("led");
    } else if (!strcmp(c, "telem") && argc == 2) {
        if (!strcmp(argv[1], "off")) st.telem_hz = 0;
        else if (!strcmp(argv[1], "json")) st.telem_fmt = settings::TELEM_JSON;
        else if (!strcmp(argv[1], "odas")) st.telem_fmt = settings::TELEM_ODAS;
        else if (parse_f(argv[1], v) && v >= 1 && v <= 60) st.telem_hz = (uint8_t)v;
        else return err("telem off|json|odas|<hz 1..60>");
        ok("telem");
    } else if (!strcmp(c, "src") && argc >= 2) {
        if (!strcmp(argv[1], "mics")) g_ctl.source = SRC_MICS;
        else if (!strcmp(argv[1], "tone")) g_ctl.source = SRC_TONE;
        else if (!strcmp(argv[1], "sim") && argc == 3 && parse_f(argv[2], v)) {
            g_ctl.sim_az = geom::wrap360(v - st.az_offset);
            g_ctl.source = SRC_SIM;
        } else return err("src mics|tone|sim <deg>");
        ok("src");
    } else if (!strcmp(c, "tune") && argc == 3 && parse_f(argv[2], v)) {
        if (!strcmp(argv[1], "margin")) doa::tuning().act_margin_db = v;
        else if (!strcmp(argv[1], "conf")) doa::tuning().conf_min = v;
        else return err("tune margin|conf <v>");
        ok("tune");
    } else if (!strcmp(c, "i2soff") && argc == 2) {
        g_ctl.i2s_bit_offset = atoi(argv[1]) & 7;
        ok("i2soff");
    } else if (!strcmp(c, "save")) {
        if (!settings::save()) return err("flash write failed");
        ok("save");
    } else if (!strcmp(c, "defaults")) {
        uint8_t m = st.mode;
        settings::defaults();
        st.mode = m;
        g_ctl.gain_db = st.gain_db;
        ok("defaults");
    } else if (!strcmp(c, "reboot")) {
        ok("reboot");
        delay(100);
        watchdog_reboot(0, 0, 10);
        while (true) tight_loop_contents();
    } else if (!strcmp(c, "bootsel")) {
        ok("bootsel");
        delay(100);
        reset_usb_boot(0, 0);
    } else if (!strcmp(c, "help")) {
        reply("{\"ok\":\"help\",\"cmds\":\"info stats levels map | mode stereo|raw8 | sector <from> <to>|off | "
              "mute full|<dB> | width <deg> | lookahead <ms> | steer auto|<deg> | "
              "gate off|on|<deg> | gain <dB> | offset <deg> | led doa|off|static r g b|set i r g b|bright n | "
              "telem off|json|odas|<hz> | src mics|tone|sim <deg> | tune margin|conf <v> | save defaults reboot "
              "bootsel\"}");
    } else {
        err("unknown command, try help");
    }
}

void poll() {
    while (Serial.available()) {
        int ch = Serial.read();
        if (ch < 0) break;
        if (ch == '\r' || ch == '\n') {
            if (s_len) {
                s_line[s_len] = 0;
                dispatch(s_line);
                s_len = 0;
            }
        } else if (s_len < (int)sizeof(s_line) - 1) {
            s_line[s_len++] = (char)ch;
        }
    }
}

void telemetry(uint32_t now_ms) {
    static uint32_t last = 0;
    const auto &st = settings::get();
    if (!st.telem_hz || now_ms - last < 1000u / st.telem_hz) return;
    last = now_ms;
    if (!Serial.dtr()) return;
    const auto &r = doa::result();
    char buf[256];
    int n;
    float az = geom::wrap360(r.az + st.az_offset);
    float az_s = geom::wrap360(r.az_smooth + st.az_offset);
    if (st.telem_fmt == settings::TELEM_ODAS) {
        // ODAS SSL-style potential source, unit vector in the array frame.
        float e = r.active ? r.conf : 0.0f;
        n = snprintf(buf, sizeof(buf),
                     "{\"timeStamp\":%lu,\"src\":[{\"x\":%.3f,\"y\":%.3f,\"z\":0.000,\"E\":%.3f}]}",
                     (unsigned long)r.seq, sinf(az * geom::DEG), cosf(az * geom::DEG), e);
    } else {
        n = snprintf(buf, sizeof(buf),
                     "{\"t\":%lu,\"az\":%.1f,\"az_s\":%.1f,\"conf\":%.3f,\"db\":%.1f,\"floor\":%.1f,\"act\":%d,"
                     "\"steer\":%.1f,\"gate\":%d}",
                     (unsigned long)now_ms, az, az_s, r.conf, r.db, r.floor_db, r.active,
                     geom::wrap360(g_ctl.steer_az + st.az_offset), g_ctl.gate_open);
    }
    // Never block on telemetry: drop the line if the host is not draining.
    if (Serial.availableForWrite() >= n + 1) {
        Serial.write(buf, n);
        Serial.write('\n');
    }
}

}  // namespace console
