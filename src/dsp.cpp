#include "dsp.h"

#include <Arduino.h>
#include <math.h>

#include "geometry.h"
#include "i2s_array.h"
#include "shared.h"

namespace dsp {

static constexpr int HIST = 32;               // history kept in front of each block
static constexpr int DEC_TAPS = 24;           // 48k -> 16k anti-alias FIR
static constexpr float INV_2_31 = 1.0f / 2147483648.0f;
static constexpr float DC_R = 0.995f;         // DC blocker pole (~38 Hz)

static uint32_t s_rd;                          // next I2S frame to read
static float s_x[NUM_MICS][HIST + BLOCK];      // per-mic signal, history first
static float s_noise[HIST + BLOCK];            // simulated-source noise
static float s_dc_x1[NUM_MICS], s_dc_y1[NUM_MICS];
static float s_dec_h[DEC_TAPS];
static uint32_t s_dec_phase;

// Fractional delay: integer part + 4-tap Lagrange weights (taps at -1..+2).
struct FracDelay {
    int i[NUM_MICS];
    float c[NUM_MICS][4];
};
static FracDelay s_beam, s_sim;
static float s_beam_az = NAN, s_sim_az = NAN;
static float s_gate_g = 1.0f;

// tone source: one rotating phasor per mic (no sinf per sample)
static float s_tone_c[NUM_MICS], s_tone_s[NUM_MICS], s_tone_rc[NUM_MICS], s_tone_rs[NUM_MICS];
static uint32_t s_rng = 0x12345678u;

// level metering (100 ms windows): AC RMS, DC and peak of the raw slots
static float s_sum[NUM_SLOTS], s_sumsq[NUM_SLOTS], s_peak[NUM_SLOTS];
static int s_level_blocks;

static uint32_t s_busy_us, s_window_start_us;

static void lagrange4(float f, float c[4]) {
    c[0] = -f * (f - 1.0f) * (f - 2.0f) / 6.0f;
    c[1] = (f + 1.0f) * (f - 1.0f) * (f - 2.0f) / 2.0f;
    c[2] = -(f + 1.0f) * f * (f - 2.0f) / 2.0f;
    c[3] = (f + 1.0f) * f * (f - 1.0f) / 6.0f;
}

// sign = +1: steering delays for the beam (aligns a wave from az).
// sign = -1: propagation delays of a plane wave arriving from az (simulation).
static void make_delays(FracDelay &d, float az_deg, float sign) {
    float base = MIC_RADIUS_M / SPEED_OF_SOUND * FS + 1.5f;
    for (int k = 0; k < NUM_MICS; ++k) {
        float dk = base + sign * geom::proj(k, az_deg) / SPEED_OF_SOUND * FS;
        int ik = (int)floorf(dk);
        d.i[k] = ik;
        lagrange4(dk - ik, d.c[k]);
    }
}

static inline float delayed(const float *buf, int n, int i, const float c[4]) {
    const float *p = buf + HIST + n - i;
    return c[0] * p[1] + c[1] * p[0] + c[2] * p[-1] + c[3] * p[-2];
}

static inline uint32_t xorshift() {
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

void init() {
    // Blackman-windowed sinc, cutoff 6.5 kHz, unity DC gain.
    float fc = 6500.0f / FS, sum = 0;
    for (int t = 0; t < DEC_TAPS; ++t) {
        float m = t - (DEC_TAPS - 1) / 2.0f;
        float sinc = (m == 0) ? 2 * fc : sinf(2 * (float)M_PI * fc * m) / ((float)M_PI * m);
        float w = 0.42f - 0.5f * cosf(2 * (float)M_PI * t / (DEC_TAPS - 1)) +
                  0.08f * cosf(4 * (float)M_PI * t / (DEC_TAPS - 1));
        s_dec_h[t] = sinc * w;
        sum += s_dec_h[t];
    }
    for (int t = 0; t < DEC_TAPS; ++t) s_dec_h[t] /= sum;

    for (int k = 0; k < NUM_MICS; ++k) {
        float w = 2 * (float)M_PI * 500.0f * (k + 1) / FS;
        s_tone_rc[k] = cosf(w);
        s_tone_rs[k] = sinf(w);
        s_tone_c[k] = 1.0f;
        s_tone_s[k] = 0.0f;
    }
    s_rd = i2s::write_frame();
    s_window_start_us = time_us_32();
}

static void __not_in_flash_func(process_block)() {
    const uint8_t mode = g_ctl.mode;
    const uint8_t source = g_ctl.source;
    const int bit_offset = g_ctl.i2s_bit_offset;

    // --- capture --------------------------------------------------------
    for (int n = 0; n < BLOCK; ++n) {
        int32_t slot[NUM_SLOTS];
        i2s::read_frame(s_rd + n, slot, bit_offset);
        for (int s = 0; s < NUM_SLOTS; ++s) {
            float v = slot[s] * INV_2_31;
            s_sum[s] += v;
            s_sumsq[s] += v * v;
            float a = fabsf(v);
            if (a > s_peak[s]) s_peak[s] = a;
        }
        for (int k = 0; k < NUM_MICS; ++k) {
            float v = slot[MIC_SLOT[k]] * INV_2_31;
            float y = v - s_dc_x1[k] + DC_R * s_dc_y1[k];
            s_dc_x1[k] = v;
            s_dc_y1[k] = y;
            s_x[k][HIST + n] = y;
        }
    }
    s_rd += BLOCK;

    // --- synthetic sources (self-test) ----------------------------------
    if (source == SRC_TONE) {
        for (int k = 0; k < NUM_MICS; ++k) {
            float c = s_tone_c[k], s = s_tone_s[k];
            const float rc = s_tone_rc[k], rs = s_tone_rs[k];
            for (int n = 0; n < BLOCK; ++n) {
                s_x[k][HIST + n] = 0.1f * s;
                float c2 = c * rc - s * rs;
                s = s * rc + c * rs;
                c = c2;
            }
            float norm = 1.5f - 0.5f * (c * c + s * s);  // keep |phasor| = 1
            s_tone_c[k] = c * norm;
            s_tone_s[k] = s * norm;
        }
    } else if (source == SRC_SIM) {
        if (g_ctl.sim_az != s_sim_az) {
            s_sim_az = g_ctl.sim_az;
            make_delays(s_sim, s_sim_az, -1.0f);
        }
        for (int n = 0; n < BLOCK; ++n)
            s_noise[HIST + n] = ((int32_t)xorshift() >> 8) * (0.1f / 8388608.0f);
        for (int k = 0; k < NUM_MICS; ++k)
            for (int n = 0; n < BLOCK; ++n)
                s_x[k][HIST + n] = delayed(s_noise, n, s_sim.i[k], s_sim.c[k]) +
                                   ((int32_t)xorshift() >> 8) * (0.002f / 8388608.0f);
    }

    // --- beamformer -------------------------------------------------------
    if (g_ctl.steer_az != s_beam_az) {
        s_beam_az = g_ctl.steer_az;
        make_delays(s_beam, s_beam_az, +1.0f);
    }
    float beam[BLOCK];
    const float gate_target = (g_ctl.gate_enabled && !g_ctl.gate_open) ? 0.03f : 1.0f;
    for (int n = 0; n < BLOCK; ++n) {
        float acc = 0;
        for (int k = 0; k < NUM_MICS; ++k) acc += delayed(s_x[k], n, s_beam.i[k], s_beam.c[k]);
        s_gate_g += (gate_target - s_gate_g) * 0.002f;
        beam[n] = acc * (1.0f / NUM_MICS) * s_gate_g;
    }

    // --- USB output -------------------------------------------------------
    if (g_ctl.streaming) {
        const float g = powf(10.0f, g_ctl.gain_db / 20.0f);
        if (mode == MODE_RAW8) {
            int16_t out[BLOCK * 8];
            for (int n = 0; n < BLOCK; ++n) {
                for (int k = 0; k < NUM_MICS; ++k) {
                    float v = s_x[k][HIST + n] * g * 32768.0f;
                    out[n * 8 + k] = (int16_t)(v > 32767.f ? 32767.f : (v < -32768.f ? -32768.f : v));
                }
                float v = beam[n] * g * 32768.0f;
                out[n * 8 + 7] = (int16_t)(v > 32767.f ? 32767.f : (v < -32768.f ? -32768.f : v));
            }
            if (!g_usb_ring.write(out, sizeof(out))) g_stats.usb_ring_full++;
        } else {
            int32_t out[BLOCK * 2];
            for (int n = 0; n < BLOCK; ++n) {
                float a = beam[n] * g, b = s_x[6][HIST + n] * g;
                a = a > 0.99999f ? 0.99999f : (a < -1.0f ? -1.0f : a);
                b = b > 0.99999f ? 0.99999f : (b < -1.0f ? -1.0f : b);
                out[n * 2] = (int32_t)(a * 2147483648.0f) & (int32_t)0xFFFFFF00;
                out[n * 2 + 1] = (int32_t)(b * 2147483648.0f) & (int32_t)0xFFFFFF00;
            }
            if (!g_usb_ring.write(out, sizeof(out))) g_stats.usb_ring_full++;
        }
    }

    // --- 48k -> 16k for DOA -----------------------------------------------
    float dec[BLOCK / DECIM + 1][NUM_MICS];
    int nd = 0;
    for (int n = 0; n < BLOCK; ++n) {
        if (++s_dec_phase < DECIM) continue;
        s_dec_phase = 0;
        for (int k = 0; k < NUM_MICS; ++k) {
            const float *p = &s_x[k][HIST + n];
            float acc = 0;
            for (int t = 0; t < DEC_TAPS; ++t) acc += s_dec_h[t] * p[-t];
            dec[nd][k] = acc;
        }
        ++nd;
    }
    if (!g_doa_ring.write(dec, nd * sizeof(dec[0]))) g_stats.doa_ring_full++;

    // --- keep history for the next block ---------------------------------
    for (int k = 0; k < NUM_MICS; ++k) memcpy(&s_x[k][0], &s_x[k][BLOCK], HIST * sizeof(float));
    memcpy(&s_noise[0], &s_noise[BLOCK], HIST * sizeof(float));

    // --- metering ---------------------------------------------------------
    if (++s_level_blocks >= 100) {
        for (int s = 0; s < NUM_SLOTS; ++s) {
            float mean = s_sum[s] / (100 * BLOCK);
            float var = s_sumsq[s] / (100 * BLOCK) - mean * mean;
            g_stats.level_db[s] = 10.0f * log10f((var > 0 ? var : 0) + 1e-20f);
            g_stats.dc[s] = mean;
            g_stats.peak_db[s] = 20.0f * log10f(s_peak[s] + 1e-10f);
            s_sum[s] = 0;
            s_sumsq[s] = 0;
            s_peak[s] = 0;
        }
        s_level_blocks = 0;
    }
    g_stats.blocks++;
}

void poll() {
    uint32_t avail = (i2s::write_frame() - s_rd) & (i2s::RING_FRAMES - 1);
    if (avail > i2s::RING_FRAMES - 2 * BLOCK) {
        // Fell a whole ring behind: resynchronise to the newest data.
        g_stats.dma_overruns++;
        s_rd = i2s::write_frame() - BLOCK;
        return;
    }
    while (avail >= (uint32_t)BLOCK) {
        uint32_t t0 = time_us_32();
        process_block();
        s_busy_us += time_us_32() - t0;
        avail -= BLOCK;
    }
    uint32_t now = time_us_32();
    if (now - s_window_start_us >= 1000000) {
        g_stats.core1_busy_us = s_busy_us;
        g_stats.core1_window_us = now - s_window_start_us;
        s_busy_us = 0;
        s_window_start_us = now;
    }
}

}  // namespace dsp
