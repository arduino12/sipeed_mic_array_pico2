#pragma once
// Direction of arrival on core 0: SRP-PHAT built from GCC-PHAT of all 21 mic
// pairs, at 16 kHz, 512-sample frames, 256-sample hop (62.5 estimates/s).
//
// Per hop: 4 complex FFTs (two real channels each) -> PHAT cross-spectra in
// 300-4000 Hz -> each pair's GCC evaluated on a fractional-lag grid (+-4
// samples, 1/8 sample steps) via one shared phase table -> SRP over 72
// azimuths (5 deg) -> parabolic peak refinement.
#include <stdint.h>

namespace doa {

constexpr int GRID = 72;

struct Result {
    float az = 0;         // this hop's peak (deg, clockwise from mic 0)
    float az_smooth = 0;  // tracked direction, updated while active
    float conf = 0;       // SRP peak / max possible, 0..1
    float db = -120;      // centre-mic level this hop, dBFS
    float floor_db = -120;
    bool active = false;  // level above noise floor and confident peak
    uint32_t seq = 0;
};

struct Tuning {
    float act_margin_db = 8.0f;  // level above the noise floor to count as a source
    float conf_min = 0.12f;
};

void init();
// Consume one hop if available and update the estimate. Returns true when a
// new estimate was produced.
bool poll();
const Result &result();
const float *srp_map();  // GRID values, peak-normalised (last hop)
Tuning &tuning();
void bench(char *out, int len);  // time each stage, JSON into out

}  // namespace doa
