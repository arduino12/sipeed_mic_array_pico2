#include "doa.h"

#include <Arduino.h>
#include <arm_acle.h>
#include <math.h>
#include <string.h>

#include "config.h"
#include "fft.h"
#include "geometry.h"
#include "shared.h"

namespace doa {

static constexpr int N = fft::N;
static constexpr int HOP = 256;
static constexpr int K_LO = 10;   // 312.5 Hz
static constexpr int K_HI = 128;  // 4000 Hz
static constexpr int NB = K_HI - K_LO + 1;
static constexpr float LAG_MAX = 4.0f;  // samples @16k; max TDOA here is 3.73
static constexpr float LAG_STEP = 0.125f;
static constexpr int NH = (int)(LAG_MAX / LAG_STEP);  // 32 lag steps each side
static constexpr int NL = 2 * NH + 1;                  // 65 lags, index NH = zero lag
static constexpr int NPAIRS = NUM_MICS * (NUM_MICS - 1) / 2;  // 21

static float s_frame[NUM_MICS][N];
static float s_win[N];
static float s_re[N], s_im[N];
static float s_Xr[NUM_MICS][NB], s_Xi[NUM_MICS][NB];
// e^{j w_b tau} for tau = m * LAG_STEP >= 0, packed Q12 (lo = cos, hi = sin).
// Negative lags use the conjugate, so one dual-MAC pair gives both signs:
//   r(+tau) = sum Gr cos - Gi sin  (SMLSD),  r(-tau) = sum Gr cos + Gi sin  (SMLAD)
// Q12 x Q12 summed over NB bins stays below 2^31 (peak = NB * 2^24).
static constexpr float Q = 4096.0f;
static int32_t s_T[NH + 2][NB];
static uint8_t s_pi[NPAIRS], s_pj[NPAIRS];
static float s_r[NPAIRS][NL];
static float s_tau_idx[GRID][NPAIRS];
static float s_srp[GRID], s_map[GRID];
static float s_hop[HOP][NUM_MICS];

static Result s_res;
static Tuning s_tune;
static bool s_floor_init = false;
static float s_vx = 0, s_vy = 1;
static uint32_t s_busy_us, s_window_start_us;

void init() {
    fft::init();
    for (int n = 0; n < N; ++n) s_win[n] = 0.5f - 0.5f * cosf(2 * (float)M_PI * n / N);
    for (int m = 0; m < NH + 2; ++m) {  // row NH+1 is padding for the 2-wide loop
        float tau = m * LAG_STEP;
        for (int b = 0; b < NB; ++b) {
            float ph = 2 * (float)M_PI * (K_LO + b) * tau / N;
            int16_t c = (int16_t)lrintf(cosf(ph) * Q), s = (int16_t)lrintf(sinf(ph) * Q);
            s_T[m][b] = (int32_t)(((uint32_t)(uint16_t)s << 16) | (uint16_t)c);
        }
    }
    int p = 0;
    for (int i = 0; i < NUM_MICS; ++i)
        for (int j = i + 1; j < NUM_MICS; ++j) {
            s_pi[p] = i;
            s_pj[p] = j;
            ++p;
        }
    // TDOA of pair (i, j) for a source at az: t_i - t_j, where mic k hears the
    // wave proj(k)/c earlier than the centre.
    for (int g = 0; g < GRID; ++g) {
        float az = g * 360.0f / GRID;
        for (int q = 0; q < NPAIRS; ++q) {
            float d = (geom::proj(s_pj[q], az) - geom::proj(s_pi[q], az)) / SPEED_OF_SOUND * FS_DOA;
            s_tau_idx[g][q] = (d + LAG_MAX) / LAG_STEP;
        }
    }
    s_window_start_us = time_us_32();
}

static void __not_in_flash_func(spectra)() {
    // Two real channels per complex FFT: z = a + j b.
    for (int a = 0; a < NUM_MICS; a += 2) {
        const int b = a + 1;
        for (int n = 0; n < N; ++n) {
            s_re[n] = s_frame[a][n] * s_win[n];
            s_im[n] = b < NUM_MICS ? s_frame[b][n] * s_win[n] : 0.0f;
        }
        fft::forward(s_re, s_im);
        for (int i = 0; i < NB; ++i) {
            const int k = K_LO + i;
            const float zr = s_re[k], zi = s_im[k], cr = s_re[N - k], ci = s_im[N - k];
            s_Xr[a][i] = 0.5f * (zr + cr);
            s_Xi[a][i] = 0.5f * (zi - ci);
            if (b < NUM_MICS) {
                s_Xr[b][i] = 0.5f * (zi + ci);
                s_Xi[b][i] = 0.5f * (cr - zr);
            }
        }
    }
}

static inline uint32_t q12(float v) {  // round to int16, as the low half of a word
    return (uint16_t)(int16_t)(v + (v >= 0 ? 0.5f : -0.5f));
}

static void __not_in_flash_func(gcc)() {
    int32_t g[NB];
    const float scale = 1.0f / (Q * Q);
    for (int q = 0; q < NPAIRS; ++q) {
        const int i = s_pi[q], j = s_pj[q];
        for (int b = 0; b < NB; ++b) {
            // X_i conj(X_j), PHAT-normalised, packed Q12 (lo = re, hi = im)
            float re = s_Xr[i][b] * s_Xr[j][b] + s_Xi[i][b] * s_Xi[j][b];
            float im = s_Xi[i][b] * s_Xr[j][b] - s_Xr[i][b] * s_Xi[j][b];
            float k = Q / (sqrtf(re * re + im * im) + 1e-20f);
            g[b] = (int32_t)((q12(im * k) << 16) | q12(re * k));
        }
        float *r = s_r[q];
        for (int m = 0; m <= NH; m += 2) {
            const int32_t *t0 = s_T[m], *t1 = s_T[m + 1];
            int32_t p0 = 0, n0 = 0, p1 = 0, n1 = 0;
            for (int b = 0; b < NB; ++b) {
                const int32_t x = g[b];
                p0 = __smlsd(x, t0[b], p0);
                n0 = __smlad(x, t0[b], n0);
                p1 = __smlsd(x, t1[b], p1);
                n1 = __smlad(x, t1[b], n1);
            }
            r[NH + m] = p0 * scale;
            r[NH - m] = n0 * scale;
            if (m + 1 <= NH) {
                r[NH + m + 1] = p1 * scale;
                r[NH - m - 1] = n1 * scale;
            }
        }
    }
}

static void __not_in_flash_func(srp)() {
    for (int g = 0; g < GRID; ++g) {
        float acc = 0;
        for (int q = 0; q < NPAIRS; ++q) {
            float x = s_tau_idx[g][q];
            int l = (int)x;
            float f = x - l;
            acc += s_r[q][l] + f * (s_r[q][l + 1] - s_r[q][l]);
        }
        s_srp[g] = acc;
    }
}

bool poll() {
    if (g_doa_ring.used() < sizeof(s_hop)) return false;
    g_doa_ring.read(s_hop, sizeof(s_hop));
    const uint32_t t0 = time_us_32();

    for (int k = 0; k < NUM_MICS; ++k) {
        memmove(&s_frame[k][0], &s_frame[k][HOP], (N - HOP) * sizeof(float));
        for (int n = 0; n < HOP; ++n) s_frame[k][N - HOP + n] = s_hop[n][k];
    }

    spectra();
    // Activity is judged in the same band the direction comes from: a sound
    // outside 300-4000 Hz (e.g. a sibilant) would otherwise count as "active"
    // while its direction estimate is noise.
    float band = 0;
    for (int b = 0; b < NB; ++b) band += s_Xr[6][b] * s_Xr[6][b] + s_Xi[6][b] * s_Xi[6][b];
    // Parseval with a Hann window (sum w^2 = 3N/8), one-sided spectrum:
    // mean square of the band-limited centre-mic signal over the frame.
    const float band_ms = band * 16.0f / (3.0f * N * N);
    gcc();
    srp();

    int best = 0;
    for (int g = 1; g < GRID; ++g)
        if (s_srp[g] > s_srp[best]) best = g;
    const float ym = s_srp[(best + GRID - 1) % GRID], y0 = s_srp[best], yp = s_srp[(best + 1) % GRID];
    const float den = ym - 2 * y0 + yp;
    const float off = den < 0 ? 0.5f * (ym - yp) / den : 0.0f;
    const float peak = y0 > 0 ? y0 : 1e-9f;
    for (int g = 0; g < GRID; ++g) s_map[g] = s_srp[g] > 0 ? s_srp[g] / peak : 0.0f;

    Result &r = s_res;
    r.az = geom::wrap360((best + off) * 360.0f / GRID);
    r.conf = y0 / (float)(NPAIRS * NB);
    r.db = 10.0f * log10f(band_ms + 1e-20f);
    if (!s_floor_init) {
        r.floor_db = r.db;
        s_floor_init = true;
    } else if (r.db < r.floor_db) {
        r.floor_db += 0.3f * (r.db - r.floor_db);
    } else {
        r.floor_db += 0.03f;  // ~2 dB/s creep up
    }
    r.active = r.db > r.floor_db + s_tune.act_margin_db && r.conf >= s_tune.conf_min;
    if (r.active) {
        s_vx = 0.7f * s_vx + 0.3f * sinf(r.az * geom::DEG);
        s_vy = 0.7f * s_vy + 0.3f * cosf(r.az * geom::DEG);
        r.az_smooth = geom::wrap360(atan2f(s_vx, s_vy) / geom::DEG);
    }
    r.seq++;

    s_busy_us += time_us_32() - t0;
    g_stats.doa_hops++;
    const uint32_t now = time_us_32();
    if (now - s_window_start_us >= 1000000) {
        g_stats.doa_busy_us = s_busy_us;
        g_stats.doa_window_us = now - s_window_start_us;
        s_busy_us = 0;
        s_window_start_us = now;
    }
    return true;
}

void bench(char *out, int len) {
    uint32_t t0 = time_us_32();
    for (int i = 0; i < 10; ++i) fft::forward(s_re, s_im);
    uint32_t t_fft = time_us_32() - t0;
    t0 = time_us_32();
    spectra();
    uint32_t t_spec = time_us_32() - t0;
    t0 = time_us_32();
    gcc();
    uint32_t t_gcc = time_us_32() - t0;
    t0 = time_us_32();
    srp();
    uint32_t t_srp = time_us_32() - t0;
    // calibration: 100k independent FMAs in 4 chains
    volatile float seed = 1.0001f;
    float a = seed, b = seed, c = seed, d = seed, m = seed;
    t0 = time_us_32();
    for (int i = 0; i < 25000; ++i) {
        a = a * m + 0.5f; b = b * m + 0.5f; c = c * m + 0.5f; d = d * m + 0.5f;
    }
    uint32_t t_fma = time_us_32() - t0;
    seed = a + b + c + d;
    snprintf(out, len,
             "{\"ok\":\"bench\",\"fft512_us\":%.1f,\"spectra_us\":%lu,\"gcc_us\":%lu,\"srp_us\":%lu,"
             "\"fma_100k_us\":%lu}",
             t_fft / 10.0f, (unsigned long)t_spec, (unsigned long)t_gcc, (unsigned long)t_srp,
             (unsigned long)t_fma);
}

const Result &result() { return s_res; }
const float *srp_map() { return s_map; }
Tuning &tuning() { return s_tune; }

}  // namespace doa
