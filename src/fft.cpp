#include "fft.h"

#include <math.h>
#include <pico.h>
#include <stdint.h>

namespace fft {

static float s_cos[N / 2], s_sin[N / 2];
static uint16_t s_rev[N];

void init() {
    for (int i = 0; i < N / 2; ++i) {
        s_cos[i] = cosf(2.0f * (float)M_PI * i / N);
        s_sin[i] = sinf(2.0f * (float)M_PI * i / N);
    }
    for (int i = 0; i < N; ++i) {
        int r = 0;
        for (int b = 0; b < LOG2N; ++b)
            if (i & (1 << b)) r |= 1 << (LOG2N - 1 - b);
        s_rev[i] = (uint16_t)r;
    }
}

void __not_in_flash_func(forward)(float *re, float *im) {
    for (int i = 0; i < N; ++i) {
        int r = s_rev[i];
        if (i < r) {
            float t = re[i]; re[i] = re[r]; re[r] = t;
            t = im[i]; im[i] = im[r]; im[r] = t;
        }
    }
    for (int len = 2; len <= N; len <<= 1) {
        const int half = len >> 1, step = N / len;
        for (int i = 0; i < N; i += len) {
            for (int j = 0; j < half; ++j) {
                const float wr = s_cos[j * step], wi = -s_sin[j * step];
                const int a = i + j, b = a + half;
                const float tr = re[b] * wr - im[b] * wi;
                const float ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
            }
        }
    }
}

}  // namespace fft
