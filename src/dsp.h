#pragma once
// Core 1 real-time path, run once per 1 ms block (48 frames):
//   I2S ring -> decode -> DC block -> [synthetic source override]
//   -> delay-and-sum beam (+ gate) -> USB output frames  (g_usb_ring)
//   -> 48k->16k decimation -> DOA frames               (g_doa_ring)

namespace dsp {

void init();
// Process every complete block waiting in the I2S ring. Never blocks.
void poll();

}  // namespace dsp
