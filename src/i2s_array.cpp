#include "i2s_array.h"

#include <hardware/clocks.h>
#include <hardware/dma.h>
#include <hardware/gpio.h>
#include <hardware/pio.h>
#include <hardware/pio_instructions.h>

#include "config.h"

namespace i2s {

static constexpr int LINES = 4;
static constexpr uint32_t RING_BYTES = RING_FRAMES * 2 * 4;  // 8 KB per line
static constexpr uint RING_BITS = 13;
static_assert(RING_BYTES == (1u << RING_BITS), "DMA ring must be a power of two");

static uint32_t s_ring[LINES][RING_BYTES / 4] __attribute__((aligned(RING_BYTES)));
static int s_dma[LINES] = {-1, -1, -1, -1};
static float s_div = 0;

// Clock master (side-set bit 0 = BCK, bit 1 = WS), two instructions per BCK.
//   0: set x, 30   side 0b00   ; left slot, BCK low (WS falls here)
//   1: nop         side 0b01
//   2: jmp x-- 1   side 0b00
//   3: nop         side 0b01
//   4: set x, 30   side 0b10   ; right slot
//   5: nop         side 0b11
//   6: jmp x-- 5   side 0b10
//   7: nop         side 0b11
static uint16_t s_clk_instr[8];
static pio_program_t s_clk_prog = {s_clk_instr, 8, -1, 0};

// Data follower, full system clock.
//   0: wait 1 gpio WS          ; one-time frame sync: WS high...
//   1: wait 0 gpio WS          ; ...then falling = start of a left slot
//   2: wait 0 gpio BCK         ; <- wrap target
//   3: wait 1 gpio BCK         ; rising edge
//   4: in pins, 1              ; -> wrap
static uint16_t s_rx_instr[5];
static pio_program_t s_rx_prog = {s_rx_instr, 5, -1, 0};

static void build_programs() {
    auto ss = [](uint v) { return pio_encode_sideset(2, v); };
    s_clk_instr[0] = pio_encode_set(pio_x, 30) | ss(0b00);
    s_clk_instr[1] = pio_encode_nop() | ss(0b01);
    s_clk_instr[2] = pio_encode_jmp_x_dec(1) | ss(0b00);
    s_clk_instr[3] = pio_encode_nop() | ss(0b01);
    s_clk_instr[4] = pio_encode_set(pio_x, 30) | ss(0b10);
    s_clk_instr[5] = pio_encode_nop() | ss(0b11);
    s_clk_instr[6] = pio_encode_jmp_x_dec(5) | ss(0b10);
    s_clk_instr[7] = pio_encode_nop() | ss(0b11);

    s_rx_instr[0] = pio_encode_wait_gpio(true, PIN_I2S_WS);
    s_rx_instr[1] = pio_encode_wait_gpio(false, PIN_I2S_WS);
    s_rx_instr[2] = pio_encode_wait_gpio(false, PIN_I2S_BCK);
    s_rx_instr[3] = pio_encode_wait_gpio(true, PIN_I2S_BCK);
    s_rx_instr[4] = pio_encode_in(pio_pins, 1);
}

bool begin() {
    build_programs();

    // --- data followers on PIO1, started first so they catch the first WS edge
    PIO rx = pio1;
    if (!pio_can_add_program(rx, &s_rx_prog)) return false;
    uint rx_off = pio_add_program(rx, &s_rx_prog);
    uint rx_mask = 0;
    for (int l = 0; l < LINES; ++l) {
        const uint pin = PIN_I2S_D0 + l;
        int sm = pio_claim_unused_sm(rx, false);
        if (sm < 0) return false;
        pio_gpio_init(rx, pin);
        gpio_disable_pulls(pin);  // RP2350-E9: no pull-downs on these inputs
        pio_sm_set_consecutive_pindirs(rx, sm, pin, 1, false);
        pio_sm_config c = pio_get_default_sm_config();
        sm_config_set_wrap(&c, rx_off + 2, rx_off + 4);
        sm_config_set_in_pins(&c, pin);
        sm_config_set_in_shift(&c, false, true, 32);  // MSB first, autopush 32
        sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
        pio_sm_init(rx, sm, rx_off, &c);

        s_dma[l] = dma_claim_unused_channel(false);
        if (s_dma[l] < 0) return false;
        dma_channel_config dc = dma_channel_get_default_config(s_dma[l]);
        channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
        channel_config_set_read_increment(&dc, false);
        channel_config_set_write_increment(&dc, true);
        channel_config_set_ring(&dc, true, RING_BITS);
        channel_config_set_dreq(&dc, pio_get_dreq(rx, sm, false));
        dma_channel_configure(s_dma[l], &dc, s_ring[l], &rx->rxf[sm], dma_encode_endless_transfer_count(), true);
        rx_mask |= 1u << sm;
    }
    pio_set_sm_mask_enabled(rx, rx_mask, true);

    // --- clock master on PIO0
    PIO tx = pio0;
    int sm = pio_claim_unused_sm(tx, false);
    if (sm < 0 || !pio_can_add_program(tx, &s_clk_prog)) return false;
    uint off = pio_add_program(tx, &s_clk_prog);
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, off, off + 7);
    sm_config_set_sideset(&c, 2, false, false);
    sm_config_set_sideset_pins(&c, PIN_I2S_BCK);
    s_div = (float)clock_get_hz(clk_sys) / (float)(FS * I2S_BCK_PER_FRAME * 2);
    sm_config_set_clkdiv(&c, s_div);
    pio_gpio_init(tx, PIN_I2S_BCK);
    pio_gpio_init(tx, PIN_I2S_WS);
    pio_sm_set_pins_with_mask(tx, sm, 0, (1u << PIN_I2S_BCK) | (1u << PIN_I2S_WS));
    pio_sm_set_consecutive_pindirs(tx, sm, PIN_I2S_BCK, 2, true);
    pio_sm_init(tx, sm, off, &c);
    pio_sm_set_enabled(tx, sm, true);
    return true;
}

uint32_t write_frame() {
    // Frames complete on all lines = the slowest line's progress. The lines
    // differ by at most a word, so this is never more than a frame behind.
    uint32_t f = RING_FRAMES;
    for (int l = 0; l < LINES; ++l) {
        uint32_t off = (dma_hw->ch[s_dma[l]].write_addr - (uint32_t)s_ring[l]) & (RING_BYTES - 1);
        uint32_t fl = off / 8;
        if (l == 0 || ((fl - f) & (RING_FRAMES - 1)) > RING_FRAMES / 2) f = fl;  // keep the oldest
    }
    return f;
}

void __not_in_flash_func(read_frame)(uint32_t idx, int32_t out[8], int bit_offset) {
    const uint32_t i = (idx & (RING_FRAMES - 1)) * 2;
    for (int l = 0; l < LINES; ++l) {
        out[2 * l] = (int32_t)((s_ring[l][i] << bit_offset) & 0xFFFFFF00u);
        out[2 * l + 1] = (int32_t)((s_ring[l][i + 1] << bit_offset) & 0xFFFFFF00u);
    }
}

uint32_t raw_word(uint32_t idx, int line, int right) {
    return s_ring[line & 3][(idx & (RING_FRAMES - 1)) * 2 + (right ? 1 : 0)];
}

float clkdiv() { return s_div; }

}  // namespace i2s
