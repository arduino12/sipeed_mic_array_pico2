#pragma once
// State shared between core 0 (USB, console, DOA, LEDs) and core 1 (I2S
// capture, beamformer, USB audio producer). Every field has a single writer;
// the comment says which.
#include <stdint.h>
#include "config.h"
#include "ring.h"

enum AudioMode : uint8_t { MODE_STEREO = 0, MODE_RAW8 = 1 };
enum SourceSel : uint8_t { SRC_MICS = 0, SRC_TONE = 1, SRC_SIM = 2 };

struct Control {
    uint8_t mode = MODE_STEREO;           // core0 at boot, fixed afterwards
    volatile uint8_t source = SRC_MICS;   // core0
    volatile float sim_az = 0.0f;         // core0: direction of the simulated source
    volatile float steer_az = 0.0f;       // core0: beam steering direction (deg)
    volatile bool gate_enabled = false;   // core0
    volatile bool gate_open = true;       // core0: DOA says the steered direction is talking
    volatile float gain_db = 18.0f;       // core0: digital gain applied to USB audio
    volatile bool streaming = false;      // USB ISR: host has the audio stream open
    // core0 (debug): BCK position of the MSB within a captured slot word. The
    // datasheet says Philips I2S (MSB one BCK after WS), but raw captures show
    // the sign bit at position 0 -- either the mic starts early or the
    // followers' first sample lands one edge late; either way the 24-bit word
    // (18 significant bits + zero padding) occupies positions 0..23.
    volatile int i2s_bit_offset = 0;
};

struct Stats {
    // core 1
    volatile uint32_t blocks = 0;
    volatile uint32_t dma_overruns = 0;
    volatile uint32_t usb_ring_full = 0;
    volatile uint32_t doa_ring_full = 0;
    volatile uint32_t core1_busy_us = 0;    // busy time over the last window
    volatile uint32_t core1_window_us = 1;
    volatile float level_db[NUM_SLOTS] = {};  // per I2S slot, 100 ms AC RMS, dBFS
    volatile float dc[NUM_SLOTS] = {};        // per I2S slot, mean (full scale = 1)
    volatile float peak_db[NUM_SLOTS] = {};
    // USB ISR
    volatile uint32_t usb_packets = 0;
    volatile uint32_t usb_underruns = 0;
    volatile uint32_t usb_restarts = 0;
    // core 0
    volatile uint32_t doa_hops = 0;
    volatile uint32_t doa_busy_us = 0;
    volatile uint32_t doa_window_us = 1;
};

extern Control g_ctl;
extern Stats g_stats;

// core1 -> USB ISR: interleaved output frames in the current mode's format
extern SpscRing<16384> g_usb_ring;
// core1 -> core0 DOA: 16 kHz frames of NUM_MICS floats
extern SpscRing<65536> g_doa_ring;

inline uint32_t usb_frame_bytes(uint8_t mode) { return mode == MODE_RAW8 ? 8 * 2 : 2 * 4; }
inline uint32_t usb_channels(uint8_t mode) { return mode == MODE_RAW8 ? 8 : 2; }
