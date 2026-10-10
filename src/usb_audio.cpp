#include "usb_audio.h"

#include <Adafruit_TinyUSB.h>

#include "config.h"
#include "shared.h"

namespace usb_audio {

enum : uint8_t { ENT_INPUT_TERM = 0x01, ENT_OUTPUT_TERM = 0x02, ENT_CLOCK = 0x10 };

static uint8_t s_mode = MODE_STEREO;
static uint8_t s_itf_ac = 0, s_itf_as = 0;
static volatile bool s_restart = false;

static constexpr uint16_t AC_BODY_LEN =
    TUD_AUDIO20_DESC_CLK_SRC_LEN + TUD_AUDIO20_DESC_INPUT_TERM_LEN + TUD_AUDIO20_DESC_OUTPUT_TERM_LEN;
static constexpr uint16_t DESC_LEN = TUD_AUDIO20_DESC_IAD_LEN + TUD_AUDIO20_DESC_STD_AC_LEN +
                                     TUD_AUDIO20_DESC_CS_AC_LEN + AC_BODY_LEN +
                                     TUD_AUDIO20_DESC_STD_AS_LEN * 2 + TUD_AUDIO20_DESC_CS_AS_INT_LEN +
                                     TUD_AUDIO20_DESC_TYPE_I_FORMAT_LEN +
                                     TUD_AUDIO20_DESC_STD_AS_ISO_EP_LEN + TUD_AUDIO20_DESC_CS_AS_ISO_EP_LEN;

class MicArrayInterface : public Adafruit_USBD_Interface {
public:
    uint16_t getInterfaceDescriptor(uint8_t, uint8_t *buf, uint16_t bufsize) override {
        if (!buf) return DESC_LEN;
        if (bufsize < DESC_LEN) return 0;

        s_itf_ac = TinyUSBDevice.allocInterface(2);
        s_itf_as = s_itf_ac + 1;
        uint8_t ep_in = TinyUSBDevice.allocEndpoint(TUSB_DIR_IN);

        const uint8_t nch = usb_channels(s_mode);
        const uint8_t subslot = s_mode == MODE_RAW8 ? 2 : 4;
        const uint8_t bits = s_mode == MODE_RAW8 ? 16 : 24;
        // raw8 claims the 7.1 layout (FL FR FC LFE BL BR SL SR): Windows will
        // not build an 8-channel format for "non-predefined" channels, and the
        // spatial labels are meaningless here anyway (ch1..6 ring, 7 centre, 8 beam).
        const uint32_t chcfg = s_mode == MODE_RAW8 ? 0x0000063Fu
                                                    : (AUDIO20_CHANNEL_CONFIG_FRONT_LEFT |
                                                       AUDIO20_CHANNEL_CONFIG_FRONT_RIGHT);
        const uint16_t ep_size = (uint16_t)((FS / 1000 + 1) * nch * subslot);

        const uint8_t desc[] = {
            TUD_AUDIO20_DESC_IAD(s_itf_ac, 2, 0),
            TUD_AUDIO20_DESC_STD_AC(s_itf_ac, 0, _strid),
            TUD_AUDIO20_DESC_CS_AC(0x0200, AUDIO20_FUNC_MICROPHONE, AC_BODY_LEN, 0),
            TUD_AUDIO20_DESC_CLK_SRC(ENT_CLOCK, AUDIO20_CLOCK_SOURCE_ATT_INT_FIX_CLK,
                                     (AUDIO20_CTRL_R << AUDIO20_CLOCK_SOURCE_CTRL_CLK_FRQ_POS) |
                                         (AUDIO20_CTRL_R << AUDIO20_CLOCK_SOURCE_CTRL_CLK_VAL_POS),
                                     ENT_INPUT_TERM, 0),
            TUD_AUDIO20_DESC_INPUT_TERM(ENT_INPUT_TERM, AUDIO_TERM_TYPE_IN_GENERIC_MIC, 0, ENT_CLOCK, nch,
                                        chcfg, 0, 0, 0),
            TUD_AUDIO20_DESC_OUTPUT_TERM(ENT_OUTPUT_TERM, AUDIO_TERM_TYPE_USB_STREAMING, 0, ENT_INPUT_TERM,
                                         ENT_CLOCK, 0, 0),
            TUD_AUDIO20_DESC_STD_AS_INT(s_itf_as, 0, 0, 0),
            TUD_AUDIO20_DESC_STD_AS_INT(s_itf_as, 1, 1, 0),
            TUD_AUDIO20_DESC_CS_AS_INT(ENT_OUTPUT_TERM, 0, AUDIO20_FORMAT_TYPE_I,
                                       AUDIO20_DATA_FORMAT_TYPE_I_PCM, nch, chcfg, 0),
            TUD_AUDIO20_DESC_TYPE_I_FORMAT(subslot, bits),
            TUD_AUDIO20_DESC_STD_AS_ISO_EP(ep_in,
                                           (uint8_t)(TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ASYNCHRONOUS |
                                                     TUSB_ISO_EP_ATT_DATA),
                                           ep_size, 1),
            TUD_AUDIO20_DESC_CS_AS_ISO_EP(AUDIO20_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, 0,
                                          AUDIO20_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, 0),
        };
        static_assert(sizeof(desc) == DESC_LEN, "UAC2 descriptor length mismatch");
        memcpy(buf, desc, sizeof(desc));
        return sizeof(desc);
    }
};

static MicArrayInterface s_itf;
static uint8_t s_cfg_buf[512];

void begin(uint8_t mode) {
    s_mode = mode;
    // The core already started USB with only the CDC port; drop off the bus
    // before the host gets to enumerate that, add the audio function, and
    // come back with the final descriptor.
    TinyUSBDevice.detach();
    TinyUSBDevice.setID(USB_VID_MICARRAY, mode == MODE_RAW8 ? USB_PID_RAW8 : USB_PID_STEREO);
    TinyUSBDevice.setManufacturerDescriptor("DIY");
    TinyUSBDevice.setProductDescriptor(mode == MODE_RAW8 ? "Sipeed Mic Array 8ch" : "Sipeed Mic Array");
    TinyUSBDevice.setConfigurationBuffer(s_cfg_buf, sizeof(s_cfg_buf));
    s_itf.setStringDescriptor(mode == MODE_RAW8 ? "Sipeed Mic Array 8ch" : "Sipeed Mic Array");
    TinyUSBDevice.addInterface(s_itf);
    TinyUSBDevice.setConfigurationMaxPower(500);
    delay(20);
    TinyUSBDevice.attach();
}

bool mounted() { return TinyUSBDevice.mounted(); }
bool streaming() { return g_ctl.streaming; }

}  // namespace usb_audio

using namespace usb_audio;

extern "C" {

// Clock source requests are the only class requests the host can make.
bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *req) {
    const uint8_t ctrl = TU_U16_HIGH(req->wValue);
    const uint8_t entity = TU_U16_HIGH(req->wIndex);
    if (entity != ENT_CLOCK) return false;

    if (ctrl == AUDIO20_CS_CTRL_SAM_FREQ) {
        if (req->bRequest == AUDIO20_CS_REQ_CUR) {
            // Must go through this helper: TinyUSB snoops the value to size
            // the IN packets for flow control.
            audio20_control_cur_4_t cur = {.bCur = (int32_t)FS};
            return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &cur, sizeof(cur));
        }
        if (req->bRequest == AUDIO20_CS_REQ_RANGE) {
            audio20_control_range_4_n_t(1) rng;
            rng.wNumSubRanges = 1;
            rng.subrange[0].bMin = (int32_t)FS;
            rng.subrange[0].bMax = (int32_t)FS;
            rng.subrange[0].bRes = 0;
            return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &rng, sizeof(rng));
        }
    } else if (ctrl == AUDIO20_CS_CTRL_CLK_VALID && req->bRequest == AUDIO20_CS_REQ_CUR) {
        uint8_t valid = 1;
        return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &valid, sizeof(valid));
    }
    return false;
}

bool tud_audio_set_req_entity_cb(uint8_t, tusb_control_request_t const *, uint8_t *) {
    return false;  // nothing is writable
}

bool tud_audio_set_itf_cb(uint8_t, tusb_control_request_t const *req) {
    const uint8_t itf = TU_U16_LOW(req->wIndex);
    const uint8_t alt = TU_U16_LOW(req->wValue);
    if (itf == s_itf_as) {
        if (alt != 0) s_restart = true;  // tx ISR re-primes the FIFO
        g_ctl.streaming = alt != 0;
    }
    return true;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t, tusb_control_request_t const *req) {
    if (TU_U16_LOW(req->wIndex) == s_itf_as) g_ctl.streaming = false;
    return true;
}

// Runs in the USB ISR after each packet has been queued. Moves whatever core 1
// has produced into the EP FIFO; never waits.
bool __not_in_flash_func(tud_audio_tx_done_isr)(uint8_t, uint16_t, uint8_t, uint8_t, uint8_t) {
    static uint8_t tmp[1024];
    const uint32_t fb = usb_frame_bytes(s_mode);
    tu_fifo_t *ff = tud_audio_get_ep_in_ff();
    if (!ff) return true;

    if (s_restart) {
        s_restart = false;
        g_usb_ring.drop_all();
        tud_audio_clear_ep_in_ff();
        // Start at the flow-control set point so packet sizes are steady from
        // the first frame.
        uint32_t prime = tud_audio_get_ep_in_fifo_threshold();
        prime -= prime % fb;
        memset(tmp, 0, sizeof(tmp));
        while (prime) {
            uint32_t n = prime < sizeof(tmp) ? prime : sizeof(tmp) - sizeof(tmp) % fb;
            tud_audio_write(tmp, (uint16_t)n);
            prime -= n;
        }
        g_stats.usb_restarts++;
    }

    uint32_t n = g_usb_ring.used();
    uint32_t room = tu_fifo_remaining(ff);
    if (n > room) n = room;
    n -= n % fb;
    while (n) {
        uint32_t chunk = n < sizeof(tmp) ? n : sizeof(tmp) - sizeof(tmp) % fb;
        g_usb_ring.read(tmp, chunk);
        tud_audio_write(tmp, (uint16_t)chunk);
        n -= chunk;
    }
    if (tu_fifo_count(ff) < (FS / 1000 - 1) * fb) g_stats.usb_underruns++;
    g_stats.usb_packets++;
    return true;
}

}  // extern "C"
