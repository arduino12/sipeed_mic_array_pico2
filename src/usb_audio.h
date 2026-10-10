#pragma once
// UAC2 microphone function added next to the core's CDC serial port.
//
// Topology: Clock Source (fixed 48 kHz, read-only) -> Input Terminal (mic)
// -> Output Terminal (USB streaming). No Feature Unit: nothing for the host
// to set, so no control request can stall or reconfigure anything at run
// time. One async isochronous IN endpoint; TinyUSB's IN flow control sends
// 47/48/49 frames per packet based on FIFO fill, which absorbs the drift
// between our crystal and the host's SOF clock.
//
// Data path: core 1 -> g_usb_ring -> (USB ISR, tud_audio_tx_done_isr) ->
// TinyUSB EP FIFO. No callback ever waits for data.
#include <stdint.h>

namespace usb_audio {

// Call first thing in setup(): re-enumerates with the audio interface.
void begin(uint8_t mode);
bool mounted();
bool streaming();

}  // namespace usb_audio
