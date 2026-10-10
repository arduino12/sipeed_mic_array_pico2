#pragma once
// I2S master capture of the 4 mic data lines.
//
// PIO0 SM: clock master, drives BCK/WS by side-set (64 BCK per frame).
// PIO1 SM0..3: one follower per data line. Each syncs once on a WS falling
// edge, then samples its line on every BCK rising edge and autopushes 32
// bits, so every FIFO word is one ready-made I2S slot (left, right, left...).
// Four endless DMA channels stream them into four 8 KB rings (1024 frames,
// ~21 ms) with no CPU involvement. Core 1 polls the DMA write pointers.
#include <stdint.h>

namespace i2s {

constexpr uint32_t RING_FRAMES = 1024;

bool begin();
// Index (mod RING_FRAMES) of the oldest frame still being written on any
// line; every frame before it is complete on all four lines.
uint32_t write_frame();
// The 8 slots of frame idx as left-justified 24-bit samples
// (slot = 2 * line + right). bit_offset = I2S data delay after the WS edge
// (1 for Philips I2S).
void read_frame(uint32_t idx, int32_t out[8], int bit_offset);
float clkdiv();

}  // namespace i2s
