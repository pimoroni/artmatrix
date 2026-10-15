#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/structs/bus_ctrl.h"

#include "artmatrix.hpp"

namespace pimoroni {

static uint32_t __attribute__ ((aligned (4))) front_buffer[width * height];
static Pixel __attribute__ ((aligned (4))) back_buffer[width * height];

// A second drawing surface, so a transition can hand the outgoing and incoming
// plugins a surface each and blend them here instead of freezing both into
// snapshots on core0. See update().
static uint32_t __attribute__ ((aligned (4))) second_buffer[width * height];

// Kept out of the class so ArtMatrix stays small enough for the placement-new
// buffer the bindings reserve for it.
static uint16_t __attribute__ ((aligned (4))) adc_buffer[ADC_SAMPLES];

/*
What calibration measured, kept out of the class so it survives a MicroPython
soft reset. That reconstructs ArtMatrix, which would otherwise walk the panel
through white again every time someone runs a script.

A hard reset zeroes these, and power_tick() clears them where it concludes the
supply is gone, so the panel is only measured again when there is a reason to.
Note power_down() deliberately does not: it also runs on the orderly core1
teardown that a soft reset performs, with the supply still connected.
*/
static bool calibration_valid = false;
static float calibration_gain_i = 0.0f;
static float calibration_gain_v = 0.0f;
static float calibration_derate = 1.0f;

// GAMMA_14BIT scaled by set_brightness(). Save ~0.12ms per flip by keeping it
// in SRAM.
static uint16_t __not_in_flash("gamma_table") gamma_lut[256];

// 4x4 ordered dither, 0..15, spending the four bits of gamma_lut that the
// panel's 10 bitplanes can't show.
static const uint8_t __not_in_flash("bayer") BAYER_4X4[16] = {
     0,  8,  2, 10,
    12,  4, 14,  6,
     3, 11,  1,  9,
    15,  7, 13,  5
};

#define GPIO_INIT_BOTH(pin, initial) gpio_init(pin); gpio_set_function(pin, GPIO_FUNC_SIO); gpio_set_dir(pin, true); gpio_put(pin, initial); \
                                     gpio_init(pin + 30); gpio_set_function(pin + 30, GPIO_FUNC_SIO); gpio_set_dir(pin + 30, true); gpio_put(pin + 30, initial)

#define GPIO_HIZ_BOTH(pin) gpio_init(pin); gpio_set_function(pin, GPIO_FUNC_SIO); gpio_set_dir(pin, false); gpio_disable_pulls(pin); \
                           gpio_init(pin + 30); gpio_set_function(pin + 30, GPIO_FUNC_SIO); gpio_set_dir(pin + 30, false); gpio_disable_pulls(pin + 30)

#define FOR_EACH_MATRIX_PIN(op, ...) op(pin_r0, ##__VA_ARGS__); op(pin_g0, ##__VA_ARGS__); op(pin_b0, ##__VA_ARGS__); \
                                     op(pin_r1, ##__VA_ARGS__); op(pin_g1, ##__VA_ARGS__); op(pin_b1, ##__VA_ARGS__); \
                                     op(pin_row_a, ##__VA_ARGS__); op(pin_row_b, ##__VA_ARGS__); op(pin_row_c, ##__VA_ARGS__); \
                                     op(pin_row_d, ##__VA_ARGS__); op(pin_row_e, ##__VA_ARGS__)

// Every HUB75 pin an input, no pulls. An unpowered panel leaks whatever we
// drive into it back up its VLED rail and onto VBUS, and some USB supplies
// then refuse to start into their own back-fed output. Nothing may be driven
// until VLED_SENSE says the panel has its own supply.
void ArtMatrix::gpio_hiz(void) {
    FOR_EACH_MATRIX_PIN(GPIO_HIZ_BOTH);
    GPIO_HIZ_BOTH(pin_clk);
    GPIO_HIZ_BOTH(pin_stb);

    // OEn is active low, so it must never be left floating: the pads come out
    // of reset pulled down, and a panel with VLED but no scan would sit with
    // its rows enabled at DC. Hold it deasserted with the internal pull-up -
    // tens of microamps, against the milliamps a driven pin would back-feed.
    GPIO_HIZ_BOTH(pin_oe);
    gpio_pull_up(pin_oe);
    gpio_pull_up(pin_oe + 30);
}

void ArtMatrix::gpio_drive(void) {
    FOR_EACH_MATRIX_PIN(GPIO_INIT_BOTH, 0);
    GPIO_INIT_BOTH(pin_clk, !clk_polarity);
    GPIO_INIT_BOTH(pin_stb, !stb_polarity);
    GPIO_INIT_BOTH(pin_oe, !oe_polarity);
}

ArtMatrix::ArtMatrix()
 {
    apply_gamma_scale(0);
    gpio_hiz();

    adc_init();
    adc_gpio_init(ADC_BASE_PIN + ADC_CUR_SENSE);
    adc_gpio_init(ADC_BASE_PIN + ADC_SENSOR_IN);
    adc_gpio_init(ADC_BASE_PIN + ADC_VLED_SENSE);
    adc_set_round_robin((1u << ADC_CUR_SENSE) | (1u << ADC_SENSOR_IN) | (1u << ADC_VLED_SENSE));
    adc_fifo_setup(true, true, 1, false, false);
    adc_set_clkdiv(ADC_CLKDIV);

    dma_channel_adc = dma_claim_unused_channel(true);
    dma_channel_config config_adc = dma_channel_get_default_config(dma_channel_adc);
    channel_config_set_transfer_data_size(&config_adc, DMA_SIZE_16);
    channel_config_set_read_increment(&config_adc, false);
    channel_config_set_write_increment(&config_adc, true);
    channel_config_set_dreq(&config_adc, DREQ_ADC);
    dma_channel_configure(dma_channel_adc, &config_adc, adc_buffer, &adc_hw->fifo, ADC_SAMPLES, false);

    adc_capture_start();
}

void gpio_put_both(uint32_t pin, bool value) {
    gpio_put(pin, value);
    gpio_put(pin + 30, value);
}

void ArtMatrix::start(irq_handler_t handler) {
    if(handler) {
        // Prevent ghosting. In PIO cycles, so derived from the nominal clock
        // to stay put when the user overclocks.
        uint32_t latch_cycles = SYS_CLK_HZ / 4000000;

        // State machines, programs and DMA channels were claimed by begin().
        artmatrix_data_rgb888_program_init(pio_a, sm_data_a1, data_prog_offs_a, DATA_BASE_PIN, pin_clk);
        artmatrix_data_rgb888_program_init(pio_a, sm_data_a2, data_prog_offs_a, DATA_BASE_PIN + 3, pin_clk);

        artmatrix_data_rgb888_program_init(pio_b, sm_data_b1, data_prog_offs_b, DATA_BASE_PIN + 30, pin_clk + 30);
        artmatrix_data_rgb888_program_init(pio_b, sm_data_b2, data_prog_offs_b, DATA_BASE_PIN + 30 + 3, pin_clk + 30);

        artmatrix_row_program_init(pio_a, sm_row_a, row_prog_offs_a, ROWSEL_BASE_PIN, ROWSEL_N_PINS, pin_stb, latch_cycles);
        artmatrix_row_program_init(pio_b, sm_row_b, row_prog_offs_b, ROWSEL_BASE_PIN + 30, ROWSEL_N_PINS, pin_stb + 30, latch_cycles);

        // Keep PIO constant when overclocked, so refresh rate and OEn pulse
        // widths (and therefore brightness) don't move with clk_sys.
        const float clkdiv = 1.5f * clock_get_hz(clk_sys) / (float)SYS_CLK_HZ;
        pio_sm_set_clkdiv(pio_a, sm_data_a1, clkdiv);
        pio_sm_set_clkdiv(pio_a, sm_data_a2, clkdiv);
        pio_sm_set_clkdiv(pio_b, sm_data_b1, clkdiv);
        pio_sm_set_clkdiv(pio_b, sm_data_b2, clkdiv);
        pio_sm_set_clkdiv(pio_a, sm_row_a, clkdiv);
        pio_sm_set_clkdiv(pio_b, sm_row_b, clkdiv);

        dma_channel_config config_a1 = dma_channel_get_default_config(dma_channel_a1);
        channel_config_set_transfer_data_size(&config_a1, DMA_SIZE_32);
        channel_config_set_bswap(&config_a1, false);
        channel_config_set_dreq(&config_a1, pio_get_dreq(pio_a, sm_data_a1, true));
        dma_channel_configure(dma_channel_a1, &config_a1, &pio_a->txf[sm_data_a1], NULL, 0, false);

        dma_channel_config config_a2 = dma_channel_get_default_config(dma_channel_a2);
        channel_config_set_transfer_data_size(&config_a2, DMA_SIZE_32);
        channel_config_set_bswap(&config_a2, false);
        channel_config_set_dreq(&config_a2, pio_get_dreq(pio_a, sm_data_a2, true));
        dma_channel_configure(dma_channel_a2, &config_a2, &pio_a->txf[sm_data_a2], NULL, 0, false);

        dma_channel_config config_b1 = dma_channel_get_default_config(dma_channel_b1);
        channel_config_set_transfer_data_size(&config_b1, DMA_SIZE_32);
        channel_config_set_bswap(&config_b1, false);
        channel_config_set_dreq(&config_b1, pio_get_dreq(pio_b, sm_data_b1, true));
        dma_channel_configure(dma_channel_b1, &config_b1, &pio_b->txf[sm_data_b1], NULL, 0, false);

        dma_channel_config config_b2 = dma_channel_get_default_config(dma_channel_b2);
        channel_config_set_transfer_data_size(&config_b2, DMA_SIZE_32);
        channel_config_set_bswap(&config_b2, false);
        channel_config_set_dreq(&config_b2, pio_get_dreq(pio_b, sm_data_b2, true));
        dma_channel_configure(dma_channel_b2, &config_b2, &pio_b->txf[sm_data_b2], NULL, 0, false);

        // begin() installed the handler, shared by all four DMA channels.
        dma_channel_set_irq1_enabled(dma_channel_a1, true);

        irq_set_enabled(DMA_IRQ_1, true);

        row_a = 0;
        bit_a = 0;

        artmatrix_data_rgb888_set_shift(pio_a, 0, data_prog_offs_a, bit_a);
        artmatrix_data_rgb888_set_shift(pio_b, 0, data_prog_offs_b, bit_a);

        dma_channel_set_trans_count(dma_channel_a1, width, false);
        dma_channel_set_trans_count(dma_channel_a2, width, false);

        dma_channel_set_trans_count(dma_channel_b1, width, false);
        dma_channel_set_trans_count(dma_channel_b2, width, false);

        dma_channel_set_read_addr(dma_channel_a1, &back_buffer, false);
        dma_channel_set_read_addr(dma_channel_a2, &back_buffer[width * height / 4], false);

        dma_channel_set_read_addr(dma_channel_b1, &back_buffer[panel_b_offset], false);
        dma_channel_set_read_addr(dma_channel_b2, &back_buffer[panel_b_offset + (width * height / 4)], false);

        dma_start_channel_mask((0b1 << dma_channel_a1) | (0b1 << dma_channel_a2) | (0b1 << dma_channel_b1) | (0b1 << dma_channel_b2));

        hw_set_bits(&bus_ctrl_hw->priority, (BUSCTRL_BUS_PRIORITY_PROC1_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS | BUSCTRL_BUS_PRIORITY_DMA_W_BITS));

        //pio_enable_sm_mask_in_sync(pio_a, (0b1 << sm_data_a1) | (0b1 << sm_data_a2));
        //pio_enable_sm_mask_in_sync(pio_b, (0b1 << sm_data_b1) | (0b1 << sm_data_b2));
        pio_enable_sm_multi_mask_in_sync(pio_a, 0, (0b1 << sm_data_a1) | (0b1 << sm_data_a2), (0b1 << sm_data_b1) | (0b1 << sm_data_b2));
    }
}

void ArtMatrix::stop(irq_handler_t handler) {
    (void)handler;
    irq_set_enabled(DMA_IRQ_1, false);

    if(dma_channel_a1 != -1) {
        dma_channel_set_irq1_enabled(dma_channel_a1, false);
        dma_channel_cleanup(dma_channel_a1);
        dma_channel_cleanup(dma_channel_a2);
        dma_channel_cleanup(dma_channel_b1);
        dma_channel_cleanup(dma_channel_b2);
    }

    // Quiesce, but leave the state machines and programs claimed. A brownout
    // cycles start()/stop(), and releasing here would hand PIO2 to whoever
    // asks next - we would not get it back.
    pio_sm_set_enabled(pio_a, sm_data_a1, false);
    pio_sm_set_enabled(pio_a, sm_data_a2, false);
    pio_sm_set_enabled(pio_a, sm_row_a, false);
    pio_sm_set_enabled(pio_b, sm_data_b1, false);
    pio_sm_set_enabled(pio_b, sm_data_b2, false);
    pio_sm_set_enabled(pio_b, sm_row_b, false);

    pio_sm_clear_fifos(pio_a, sm_data_a1);
    pio_sm_clear_fifos(pio_a, sm_data_a2);
    pio_sm_clear_fifos(pio_a, sm_row_a);
    pio_sm_clear_fifos(pio_b, sm_data_b1);
    pio_sm_clear_fifos(pio_b, sm_data_b2);
    pio_sm_clear_fifos(pio_b, sm_row_b);

    // Make sure the GPIO is in a known good state
    // since we don't know what the PIO might have done with it
    gpio_put_masked(0b111111 << pin_r0, 0);
    gpio_put_masked(0b11111 << pin_row_a, 0);

    gpio_put_masked64((uint64_t)0b111111 << (pin_r0 + 30), 0);
    gpio_put_masked64((uint64_t)0b11111 << (pin_row_a + 30), 0);

    gpio_put_both(pin_clk, !clk_polarity);
    gpio_put_both(pin_oe, !oe_polarity);
}

ArtMatrix::~ArtMatrix() {
    release();

    adc_run(false);

    if(dma_channel_adc != -1 && dma_channel_is_claimed(dma_channel_adc)) {
        dma_channel_cleanup(dma_channel_adc);
        dma_channel_unclaim(dma_channel_adc);
        dma_channel_adc = -1;
    }
}

// The panel isn't started here: power_tick() brings it up once VLED_SENSE
// reports a supply, and takes it back down to hi-z if that supply goes.
//
// The hardware is claimed here rather than in start(), though. start() may not
// run until a supply appears, by which time Python has imported plasma and
// picovector and they have taken the state machines we wanted - claiming late
// panics with "PIO 2 SM already claimed" on the first hot-plug.
void ArtMatrix::begin(irq_handler_t handler) {
    irq_handler = handler;

    pio_sm_claim(pio_a, sm_data_a1);
    pio_sm_claim(pio_a, sm_data_a2);
    pio_sm_claim(pio_a, sm_row_a);
    pio_sm_claim(pio_b, sm_data_b1);
    pio_sm_claim(pio_b, sm_data_b2);
    pio_sm_claim(pio_b, sm_row_b);

    pio_set_gpio_base(pio_b, 16);

    data_prog_offs_a = pio_add_program(pio_a, &artmatrix_data_rgb888_program);
    data_prog_offs_b = pio_add_program(pio_b, &artmatrix_data_rgb888_program);
    row_prog_offs_a = pio_add_program(pio_a, &artmatrix_row_program);
    row_prog_offs_b = pio_add_program(pio_b, &artmatrix_row_program);

    dma_channel_a1 = dma_claim_unused_channel(true);
    dma_channel_a2 = dma_claim_unused_channel(true);
    dma_channel_b1 = dma_claim_unused_channel(true);
    dma_channel_b2 = dma_claim_unused_channel(true);

    irq_set_exclusive_handler(DMA_IRQ_1, handler);
    irq_set_priority(DMA_IRQ_1, PICO_HIGHEST_IRQ_PRIORITY);
}

void ArtMatrix::release(void) {
    if(irq_handler == nullptr) return;

    irq_remove_handler(DMA_IRQ_1, irq_handler);
    irq_handler = nullptr;

    const int channels[4] = {dma_channel_a1, dma_channel_a2, dma_channel_b1, dma_channel_b2};
    for(uint i = 0u; i < 4u; i++) {
        if(channels[i] != -1 && dma_channel_is_claimed(channels[i])) dma_channel_unclaim(channels[i]);
    }
    dma_channel_a1 = dma_channel_a2 = dma_channel_b1 = dma_channel_b2 = -1;

    pio_remove_program(pio_a, &artmatrix_data_rgb888_program, data_prog_offs_a);
    pio_remove_program(pio_b, &artmatrix_data_rgb888_program, data_prog_offs_b);
    pio_remove_program(pio_a, &artmatrix_row_program, row_prog_offs_a);
    pio_remove_program(pio_b, &artmatrix_row_program, row_prog_offs_b);

    pio_sm_unclaim(pio_a, sm_data_a1);
    pio_sm_unclaim(pio_a, sm_data_a2);
    pio_sm_unclaim(pio_a, sm_row_a);
    pio_sm_unclaim(pio_b, sm_data_b1);
    pio_sm_unclaim(pio_b, sm_data_b2);
    pio_sm_unclaim(pio_b, sm_row_b);
}

void ArtMatrix::get_power_stats(uint32_t counts[7], float headroom[7]) {
    counts[0] = vled_open_counts;
    counts[1] = last_vled_mean;   // what the budgets are measured against
    counts[2] = last_vled_min;    // what the trip uses
    counts[3] = last_cur;         // what the budget is measured against
    counts[4] = last_cur_peak;    // what the load switch limits on
    counts[5] = cur_idle_counts;
    counts[6] = is_calibrating() ? 1u : 0u;

    headroom[0] = last_headroom_v;
    headroom[1] = last_headroom_i;
    headroom[2] = brightness_ceiling;
    headroom[3] = brightness_allowed;
    headroom[4] = brightness_applied;

    // The white measurement itself, so a ceiling can be worked out by hand
    // against a different budget without reflashing to find out.
    headroom[5] = gain_i;
    headroom[6] = gain_v;
}

uint16_t ArtMatrix::get_sensor(void) {
    return sensor_counts;
}

bool ArtMatrix::is_powered(void) {
    return panel_powered;
}

// While set, back_buffer holds the calibration grid and update() must leave it
// alone.
bool ArtMatrix::is_calibrating(void) {
    return calibrating || presenting;
}

void __no_inline_not_in_flash_func(ArtMatrix::dma_complete()) {

    if(dma_channel_get_irq1_status(dma_channel_a1)) {
        dma_channel_acknowledge_irq1(dma_channel_a1);
        dma_channel_wait_for_finish_blocking(dma_channel_a2);
        dma_channel_wait_for_finish_blocking(dma_channel_b1);
        dma_channel_wait_for_finish_blocking(dma_channel_b2);

        // SM is finished when it stalls on empty TX FIFO
        artmatrix_wait_tx_stall(pio_a, sm_data_a1);
        artmatrix_wait_tx_stall(pio_a, sm_data_a2);
        artmatrix_wait_tx_stall(pio_b, sm_data_b1);
        artmatrix_wait_tx_stall(pio_b, sm_data_b2);

        // Check that previous OEn pulse is finished, else things WILL get out of sequence
        artmatrix_wait_tx_stall(pio_a, sm_row_a);
        artmatrix_wait_tx_stall(pio_b, sm_row_b);

        // Latch row data, pulse output enable for new row. Blanking is a zero
        // width pulse: pulse_loop still runs once, so OEn asserts for a single
        // 6ns cycle, 0.06% of a frame. Unlike dimming the gamma table it lands
        // on the next row instead of waiting for the next update().
        const uint32_t pulse = blank ? 0 : ((brightness << bit_a) - 4);

        // Both row SMs just stalled, so their TX FIFOs are known empty.
        pio_sm_put(pio_a, sm_row_a, row_a | (pulse << 5));
        pio_sm_put(pio_b, sm_row_b, row_a | (pulse << 5));

        row_a++;

        if(row_a == height / 4) {
            row_a = 0;
            bit_a++;
            if (bit_a == BIT_DEPTH) {
                bit_a = 0;
                // Frame boundary. Picking up a new multiplier part-way through
                // would give this frame a mismatched set of bitplane weights.
                brightness = brightness_next;
            }
            artmatrix_data_rgb888_set_shift(pio_a, 0, data_prog_offs_a, bit_a);
            artmatrix_data_rgb888_set_shift(pio_b, 0, data_prog_offs_b, bit_a);
        }

        uint32_t row_offset = row_a * width;
    
        dma_channel_set_read_addr(dma_channel_a1, &back_buffer[row_offset], false);
        dma_channel_set_read_addr(dma_channel_a2, &back_buffer[row_offset + (width * height / 4)], false);
    
        dma_channel_set_read_addr(dma_channel_b1, &back_buffer[panel_b_offset + row_offset], false);
        dma_channel_set_read_addr(dma_channel_b2, &back_buffer[panel_b_offset + row_offset + (width * height / 4)], false);

        //pio_clkdiv_restart_sm_multi_mask(pio_a, 0, (0b1 << sm_data_a1) | (0b1 << sm_data_a2), (0b1 << sm_data_b1) | (0b1 << sm_data_b2));
        dma_start_channel_mask((0b1 << dma_channel_a1) | (0b1 << dma_channel_a2) | (0b1 << dma_channel_b1) | (0b1 << dma_channel_b2));
    }
}

uint32_t* ArtMatrix::get_framebuffer(void) {
    return front_buffer;
}

uint32_t* ArtMatrix::get_surface(uint32_t index) {
    return index ? second_buffer : front_buffer;
}

// gamma_lut holds 14 bits; the panel shows the top 10. `d` is 0..15, so the
// discarded bits round up in one pixel of the 4x4 cell per unit. Peak entry is
// 1023 << 4, so (16368 + 15) >> 4 == 1023 and this can't overflow its field.
static inline uint32_t gamma_pack(uint32_t rgb, uint32_t d) {
    const uint32_t r = gamma_lut[rgb & 0xff];
    const uint32_t g = gamma_lut[(rgb >> 8) & 0xff];
    const uint32_t b = gamma_lut[(rgb >> 16) & 0xff];

    return ((r + d) >> 4) | (((g + d) >> 4) << 10) | (((b + d) >> 4) << 20);
}

// Mix two packed pixels, t = 0 giving all `a` and 255 all `b`. Both 8-bit lanes
// of each half-word hold at most 255 * 255, so the channels can't bleed into
// each other and the pair can be done in one multiply. The alpha byte rides
// along in the odd lane; gamma_pack ignores it.
static inline uint32_t lerp8888(uint32_t a, uint32_t b, uint32_t t) {
    const uint32_t it = 255u - t;

    uint32_t rb = ((a & 0x00ff00ffu) * it) + ((b & 0x00ff00ffu) * t);
    uint32_t ga = (((a >> 8) & 0x00ff00ffu) * it) + (((b >> 8) & 0x00ff00ffu) * t);

    // x / 255, as (x + 128 + (x >> 8)) >> 8 per lane
    rb = ((rb + 0x00800080u + ((rb >> 8) & 0x00ff00ffu)) >> 8) & 0x00ff00ffu;
    ga = ((ga + 0x00800080u + ((ga >> 8) & 0x00ff00ffu))) & 0xff00ff00u;

    return rb | ga;
}

// Walk a frame through the gamma table into back_buffer. Blend is a template
// parameter so the common case carries no per-pixel test, and always_inline
// puts both instantiations in update()'s SRAM section.
template<bool Blend>
static inline __attribute__((always_inline))
void render(const uint32_t *a, const uint32_t *b, uint32_t t, uint32_t b_offset) {
    for(uint32_t y = 0u; y < height; y++) {
        // We are swapping X and Y to achieve a 90 degree rotation
        // Mirror along the Y axis (top to bottom)
        const uint32_t sx = height - 1 - y;
        const uint8_t *dither = &BAYER_4X4[(y & 3) << 2];
        const uint32_t row = y * width;

        // Source x picks the panel, so walk each half separately. Within a
        // half the destination just advances one row (`width`) per pixel.
        // width / 2 is a multiple of 4, so the dither phase carries over.
        Pixel *pb = &back_buffer[b_offset + sx];
        for(uint32_t x = 0u; x < width / 2; x++, pb += width) {
            const uint32_t i = row + x;
            *pb = gamma_pack(Blend ? lerp8888(a[i], b[i], t) : a[i], dither[x & 3]);
        }

        Pixel *pa = &back_buffer[sx];
        for(uint32_t x = 0u; x < width / 2; x++, pa += width) {
            const uint32_t i = row + width / 2 + x;
            *pa = gamma_pack(Blend ? lerp8888(a[i], b[i], t) : a[i], dither[x & 3]);
        }
    }
}

/*
Render the frame Python has finished drawing. `blend` mixes the two drawing
surfaces, so a transition is one pass here rather than a pair of snapshots and a
composite on core0.

This reads the drawing surfaces directly, which is only safe because core0 is
not drawing while it waits on the flip. The exception is the one repaint core1
makes when a recalibration hands back, which reads them without a flip, so a
frame that was drawn once comes back after power_up() clears back_buffer. Core0
may be mid-draw then, which costs at most one torn frame.

Must not be called while is_calibrating(): back_buffer holds the calibration
grid. The caller holds the flip until calibration ends rather than
dropping it, so the first thing drawn at boot still reaches the panel.
*/
// Section named by hand: __no_inline_not_in_flash_func stringifies its argument
// into the section name, and a parameter list with a space in it emits a
// .section directive the assembler can't parse.
void __noinline __not_in_flash("ArtMatrix_update") ArtMatrix::update(uint8_t blend) {
    if(blend == 0u) {
        render<false>(front_buffer, nullptr, 0u, panel_b_offset);
    } else if(blend == 255u) {
        render<false>(second_buffer, nullptr, 0u, panel_b_offset);
    } else {
        render<true>(front_buffer, second_buffer, blend, panel_b_offset);
    }
}

// GAMMA_14BIT is linear light, so scaling it is just a multiply. `scale` is
// 1.10 fixed point. Nothing about the scan changes, so this can't flicker.
void ArtMatrix::apply_gamma_scale(uint32_t scale) {
    gamma_lut[0] = 0;
    for(uint32_t i = 1u; i < 256u; i++) {
        const uint32_t v = (GAMMA_14BIT[i] * scale + 512) >> 10;
        // Hold non-zero inputs above black; the dither can still show a 1.
        gamma_lut[i] = (v == 0 && scale != 0) ? 1 : v;
    }
}

// Uniform fill of the scan buffer, in the layout gamma_pack produces.
void ArtMatrix::fill_back_buffer(uint32_t packed) {
    for(uint32_t i = 0u; i < width * height; i++) back_buffer[i] = packed;
}

static uint32_t hsv_to_rgb(uint32_t h, uint32_t s, uint32_t v) {
    const int hs = h * 6;
    const int region = hs >> 8;
    const int remainder = hs & 0xff;

    const uint32_t p = (v * (255 - s)) / 255;
    const uint32_t q = (v * (255 - (s * remainder) / 255)) / 255;
    const uint32_t t = (v * (255 - (s * (255 - remainder)) / 255)) / 255;

    uint32_t r, g, b;
    switch(region) {
        case 0:  r = v; g = t; b = p; break;
        case 1:  r = q; g = v; b = p; break;
        case 2:  r = p; g = v; b = t; break;
        case 3:  r = p; g = q; b = v; break;
        case 4:  r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }

    return r | (g << 8) | (b << 16);
}

static uint32_t grid_color(uint column, uint tint) {
    const uint32_t hue = (column * GRID_PITCH * GRID_HUE_PER_PIXEL) % 255;
    const uint32_t target = hsv_to_rgb(hue, GRID_SATURATION, GRID_VALUE);

    const float t = (float)tint / (float)CALIBRATE_TINT_CAPTURES;
    const float eased = t * t * (3.0f - 2.0f * t);

    uint32_t rgb = 0;
    for(uint shift = 0u; shift < 24u; shift += 8u) {
        const float channel = (float)((target >> shift) & 0xff);
        rgb |= (uint32_t)(255.0f + (channel - 255.0f) * eased + 0.5f) << shift;
    }
    return rgb;
}

void ArtMatrix::grid_draw_square(uint index, uint32_t rgb) {
    const uint32_t x0 = (index / GRID_SIZE) * GRID_PITCH + GRID_INSET;
    const uint32_t y0 = (index % GRID_SIZE) * GRID_PITCH + GRID_INSET;

    for(uint32_t y = y0; y < y0 + GRID_SQUARE; y++) {
        const uint32_t sx = height - 1 - y;
        const uint8_t *dither = &BAYER_4X4[(y & 3) << 2];
        for(uint32_t x = x0; x < x0 + GRID_SQUARE; x++) {
            const uint32_t column = x < width / 2 ? panel_b_offset + x * width : (x - width / 2) * width;
            back_buffer[column + sx] = gamma_pack(rgb, dither[x & 3]);
        }
    }
}

void ArtMatrix::grid_light(uint count) {
    while(count-- && grid_lit < GRID_SQUARES) {
        grid_draw_square(grid_lit, grid_color(grid_lit / GRID_SIZE, grid_tint));
        grid_lit++;
    }
}

void ArtMatrix::grid_repaint(void) {
    for(uint i = 0u; i < grid_lit; i++) {
        grid_draw_square(i, grid_color(i / GRID_SIZE, grid_tint));
    }
}

float ArtMatrix::grid_coverage(void) {
    return (float)(grid_lit * GRID_SQUARE * GRID_SQUARE) / (float)(width * height);
}

// Perceptual 0.0 - 1.0 to a fraction of full light. Equal steps of the input
// should look equally spaced, which CIE L* gives us.
static float perceptual_to_light(float value) {
    const float l = value * 100.0f;
    const float t = (l + 16.0f) / 116.0f;
    return l > 8.0f ? t * t * t : l / 903.3f;
}

// Takes a fraction of full light, NOT a perceptual value. Current and droop are
// both linear in light, so the power loop must control light. Driving
// it in perceptual units puts a cubic in the loop: down at 0.4 perceptual, a
// step of 0.05 is a two-fold change in light, the gain runs away, and the loop
// saws back and forth across its own deadband.
//
// Core1 only: it writes the table update() reads.
void ArtMatrix::apply_brightness(float light) {
    // Take the lowest OEn multiplier whose duty covers the request, since a
    // higher one only costs refresh rate, then let the gamma scale make up the
    // shortfall exactly. The coarse steps are 3-14% of light on their own, so
    // interpolating between them is what keeps a ramp from visibly stepping.
    //
    // Hysteresis, because OE_DUTY is calculated rather than measured: the
    // interpolation only cancels the step if the table matches the panel
    // exactly, and it does not. Sitting on a boundary the multiplier would
    // otherwise chatter, and each toggle shows as a jump.
    uint i = oe_index;
    while(i < BRIGHTNESS_STEPS - 1 && OE_DUTY[i] < light) i++;
    while(i > 0 && light <= OE_DUTY[i - 1] * (1.0f - OE_STEP_HYSTERESIS)) i--;

    oe_index = i;
    brightness_next = BRIGHTNESS_MIN + i;

    float scale = light / OE_DUTY[i];
    if(scale > 1.0f) scale = 1.0f;

    apply_gamma_scale((uint32_t)(scale * 1024.0f + 0.5f));
}

/*
Solve each measured white gain for the light it permits and take the lower. The
gains are per unit of all-white light, so this is what a fully lit panel may
have, and every picture is below it by construction. A gain of zero means
calibration never reached that limit, so it permits everything.

Cheap enough to recompute each tick, which is what lets set_current_limit() take
effect without measuring the panel again.
*/
void ArtMatrix::update_ceiling(float led_budget, float sag_budget) {
    const float light_i = gain_i > 0.0f ? led_budget / gain_i : 1.0f;
    const float light_v = gain_v > 0.0f ? sag_budget / gain_v : 1.0f;

    last_headroom_i = light_i;
    last_headroom_v = light_v;

    float light = (light_i < light_v ? light_i : light_v) * CALIBRATE_MARGIN * ceiling_derate;
    if(light > 1.0f) light = 1.0f;
    if(light < 0.0f) light = 0.0f;

    brightness_ceiling = light;
}

// Hold on to a measurement so a soft reset can reuse it. The derate rides along:
// a supply that has already tripped should come back derated, not optimistic.
void ArtMatrix::store_calibration(void) {
    calibration_gain_i = gain_i;
    calibration_gain_v = gain_v;
    calibration_derate = ceiling_derate;
    calibration_valid = true;
}

void ArtMatrix::start_calibration(void) {
    fill_back_buffer(0);
    grid_lit = 0;
    grid_tint = 0;

    presenting = false;
    calibrating = true;
    calibrate_phase = CALIBRATE_OPEN;
    calibrate_dwell = CALIBRATE_OPEN_DWELL;
    calibrate_led_sum = 0.0f;
    calibrate_sag_sum = 0.0f;
    calibrate_light_sum = 0.0f;
    calibrate_measured = 0;
}

void ArtMatrix::finish_calibration(float led_budget, float sag_budget) {
    calibrating = false;

    update_ceiling(led_budget, sag_budget);

    if(grid_lit) {
        presenting = true;
    } else {
        brightness_allowed = brightness_ceiling;
    }
}

void ArtMatrix::present_step(void) {
    grid_light(CALIBRATE_SQUARES_PER_CAPTURE);

    const bool settled = grid_lit >= GRID_SQUARES && brightness_allowed == brightness_ceiling;

    if(settled && grid_tint < CALIBRATE_TINT_CAPTURES) {
        grid_tint++;
        grid_repaint();
    } else if(settled) {
        presenting = false;
    } else if(brightness_allowed < brightness_ceiling) {
        brightness_allowed += CALIBRATE_PRESENT_STEP;
        if(brightness_allowed > brightness_ceiling) brightness_allowed = brightness_ceiling;
    } else if(brightness_allowed > brightness_ceiling) {
        brightness_allowed -= CALIBRATE_PRESENT_STEP;
        if(brightness_allowed < brightness_ceiling) brightness_allowed = brightness_ceiling;
    }
}

/*
Any core. Ignore the measured ceiling and drive at whatever set_brightness()
asks for, so a panel that has mis-calibrated can still be seen and debugged.

Bypasses the ceiling and nothing else: the trip still blanks on a rail that
collapses, and the panel still refuses to drive until VLED_SENSE says a supply
is there. A trip also clears this, so a panel driven past what its supply can
hold comes back by itself at the measured ceiling.
*/
void ArtMatrix::set_force_on(bool force) {
    force_light = force;
}

/*
Any core. Throw away the stored calibration and measure the panel again, which
means lighting the grid again. For a supply or panel swapped while powered,
where nothing has told us the world changed.

The flag is picked up by core1 rather than acted on here: power_tick() owns
every piece of state this touches.
*/
void ArtMatrix::recalibrate(void) {
    recalibrate_request = true;
}

// Any core. How much the panel may draw in total, LEDs and idle together.
// Defaults to the ceiling, so this only ever lowers it - for a supply that is
// weaker than the one we ship with. The load switch limits at 3.3A whatever is
// plugged in, and 3.0A is all a sink may take without negotiating USB-C PD.
void ArtMatrix::set_current_limit(float amps) {
    uint32_t counts = (uint32_t)(amps * CUR_COUNTS_PER_AMP + 0.5f);

    if(counts > CUR_LIMIT_COUNTS) counts = CUR_LIMIT_COUNTS;
    if(counts <= (uint32_t)cur_idle_avg) counts = (uint32_t)cur_idle_avg + 1u;

    cur_budget_counts = counts;
}

// Any core, perceptual. Core1's power loop picks this up and ramps toward it.
void ArtMatrix::set_brightness(float value) {
    if(value < 0.0f) value = 0.0f;
    if(value > 1.0f) value = 1.0f;
    brightness_target = value;
}

void ArtMatrix::adc_capture_start(void) {
    adc_run(false);
    adc_fifo_drain();

    // Round robin resumes from the selected input, so re-selecting here pins
    // sample i to input (i % 3) for the whole capture. A free-running ring
    // can't do this: no power of two divides by three, so the mapping would
    // rotate on every wrap.
    adc_select_input(ADC_CUR_SENSE);

    dma_channel_set_write_addr(dma_channel_adc, adc_buffer, false);
    dma_channel_set_trans_count(dma_channel_adc, ADC_SAMPLES, true);

    adc_run(true);
}

void ArtMatrix::power_up(void) {
    // back_buffer still holds the last frame at its old scale, and only update()
    // and calibration write it, so the panel stays dark until we light it.
    // Nothing here keeps a copy of the frame to put back.
    fill_back_buffer(0);
    apply_gamma_scale(0);

    gpio_drive();
    start(irq_handler);

    brightness_allowed = 0.0f;
    brightness_applied = -1.0f;
    brightness_ceiling = 0.0f;
    ceiling_derate = 1.0f;
    vled_absent_count = 0;
    vled_open_counts = 0;
    vled_open_avg = 0.0f;
    vled_open_min_counts = 0;
    vled_open_min_avg = 0.0f;
    cur_idle_counts = 0;
    cur_idle_avg = 0.0f;
    gain_i = 0.0f;
    gain_v = 0.0f;
    oe_index = 0;

    // Hold at black for the first stretch either way: the open-circuit rail and
    // the panel's idle draw belong to this power-up, and everything below is
    // measured against them. Only the grid is skipped when a previous
    // calibration on this supply is still good.
    start_calibration();
    over_budget_count = 0;

    accum_cur = 0.0f;
    accum_vled = 0.0f;
    accum_light = 0.0f;
    accum_count = 0;
    accum_vled_min = 4095;
    accum_cur_peak = 0;
    last_led = 0.0f;
    last_sag = 0.0f;
    last_light = 0.0f;
    last_led_budget = 0.0f;
    last_sag_budget = 0.0f;

    // Let the 15ms soft start finish before believing anything we measure.
    settle_count = POWER_SETTLE_CAPTURES;

    // Stay dark until power_tick() has seen the rail and a frame has shifted.
    blank = true;
    panel_powered = true;
}

void ArtMatrix::power_down(void) {
    if(!panel_powered) return;

    stop(irq_handler);
    gpio_hiz();

    apply_gamma_scale(0);
    brightness_allowed = 0.0f;
    brightness_applied = -1.0f;
    vled_good_count = 0;
    vled_open_counts = 0;
    vled_open_min_counts = 0;
    calibrating = false;
    presenting = false;
    blank = true;
    panel_powered = false;
}

// Light the panel at whatever the current state permits. Split out because
// power_tick() returns from several places and every one of them must leave the
// panel driven, not just the path that reaches the bottom.
void ArtMatrix::apply_target(void) {
    // Calibration drives the panel itself: the ramp is the measurement, and
    // both the target and the override would corrupt it.
    float value = brightness_allowed;

    if(!calibrating) {
        // brightness_allowed is light; the target the user set is perceptual.
        value = perceptual_to_light(brightness_target);
        if(!force_light && brightness_allowed < value) value = brightness_allowed;
    }

    if(value != brightness_applied) {
        apply_brightness(value);
        brightness_applied = value;

        // The OEn multiplier lands on the next row, but the gamma scale reaches
        // the panel through back_buffer, so it only shows once something writes
        // that again. Calibration owns the buffer and must repack its grid at
        // the new scale; otherwise it is the application's next update().
        if(calibrating || presenting) grid_repaint();
    }
}

// Core1. Cheap to call often; the work only happens once a capture lands.
void ArtMatrix::power_tick(void) {
    if(dma_channel_is_busy(dma_channel_adc)) return;
    adc_run(false);

    uint32_t sum[3] = {0, 0, 0};
    uint32_t vled_sag = 4095;
    uint32_t cur_peak = 0;

    for(uint32_t i = 0u; i < ADC_SAMPLES; i++) {
        const uint32_t sample = adc_buffer[i];
        sum[i % 3] += sample;

        // Worst instantaneous sag across the window, not the mean. Panel
        // current swings hard within a frame - a bright generation of Life
        // drags the rail under the trip point and back before a mean ever
        // moves. Averaging hides precisely the transient we must react to.
        if((i % 3) == 2u && sample < vled_sag) vled_sag = sample;

        // The budget is enforced on the mean, but the load switch limits on the
        // instantaneous current, and BCM puts most of the frame in the MSB
        // plane. Reported only, as the peak-to-mean ratio.
        if((i % 3) == 0u && sample > cur_peak) cur_peak = sample;
    }

    const uint32_t vled_counts = sum[2] / ADC_PER_INPUT;

    // Keep the sub-count resolution the averaging bought us. Rounding 128
    // summed samples down to an integer throws away seven bits, and the LED
    // current is a small difference against a much larger idle draw.
    const float cur_capture = (float)sum[0] / (float)ADC_PER_INPUT;
    const float vled_capture = (float)sum[2] / (float)ADC_PER_INPUT;

    sensor_counts = (sum[1] / ADC_PER_INPUT) << 4; // 12-bit -> u16, as machine.ADC

    if(!panel_powered) {
        // No averaged tick will run to fill these, and power_stats() is how you
        // watch a supply that is not coming up.
        last_cur = sum[0] / ADC_PER_INPUT;
        last_vled_mean = vled_counts;
        last_vled_min = vled_sag;
        last_cur_peak = cur_peak;

        // Debounce a supply that's still coming up.
        if(vled_counts >= VLED_GOOD_COUNTS) {
            if(++vled_good_count >= VLED_DEBOUNCE) power_up();
        } else {
            vled_good_count = 0;
        }
        adc_capture_start();
        return;
    }

    if(settle_count) {
        // VLED is still ramping and CUR_SENSE is reading 1.65A of inrush into
        // the panel's bulk capacitance, not its idle draw.
        settle_count--;
        adc_capture_start();
        return;
    }

    if(recalibrate_request) {
        recalibrate_request = false;
        calibration_valid = false;

        if(!calibrating) {
            gain_i = 0.0f;
            gain_v = 0.0f;
            ceiling_derate = 1.0f;
            brightness_ceiling = 0.0f;
            brightness_allowed = 0.0f;
            over_budget_count = 0;
            start_calibration();

            accum_cur = 0.0f;
            accum_vled = 0.0f;
            accum_light = 0.0f;
            accum_count = 0;
            accum_vled_min = 4095;
            accum_cur_peak = 0;
        }
    }

    if(vled_counts >= VLED_ABSENT_COUNTS) vled_absent_count = 0;

    /*
    The trip runs on every capture, on the worst sample in it. A rail folding
    cannot wait for an average, and this is the only thing here fast enough to
    catch it.

    It acts on budgets from the last averaged tick, up to POWER_AVERAGE_CAPTURES
    old. That is the right trade: they describe what the panel was already
    doing, and nothing about them changes the decision to blank.
    */
    if(vled_sag < VLED_TRIP_COUNTS) {
        if(!blank) {
            // Blank before judging the supply. With the LEDs off an overload
            // recovers and a removal doesn't, and we can't tell them apart
            // while we're the thing pulling the rail down. Tearing the PIO
            // down here instead would unload the rail, read 5V, restart, and
            // collapse again - a flashing panel.
            blank = true;

            // The override is the likeliest way to reach a collapse, so drop
            // it: the panel then comes back at its measured ceiling by itself.
            const bool was_forced = force_light;
            force_light = false;

            // Calibration is lighting the grid to find this point, so a trip is
            // its answer, taken at the light that caused it. Otherwise
            // the ceiling only moves if the ceiling is what collapsed the rail;
            // an override reaching it says nothing about white.
            if(calibrating) {
                gain_i = last_light > 0.0f ? last_led / last_light : 0.0f;
                gain_v = last_light > 0.0f ? last_sag / last_light : 0.0f;
                ceiling_derate = TRIP_DERATE;
                store_calibration();
                finish_calibration(last_led_budget, last_sag_budget);
            } else {
                if(!was_forced) ceiling_derate *= TRIP_DERATE;
                store_calibration();
                update_ceiling(last_led_budget, last_sag_budget);
                brightness_allowed = brightness_ceiling;
            }
        } else if(vled_counts < VLED_ABSENT_COUNTS && ++vled_absent_count >= VLED_DEBOUNCE) {
            // Still dead with the LEDs off, so the supply is gone rather than
            // overloaded. Stop driving before we leak into the panel again.
            //
            // Discard the calibration here and not in power_down(), which also
            // runs on an orderly core1 teardown - which a MicroPython soft reset
            // does every time, and which leaves the supply exactly where it was.
            calibration_valid = false;
            power_down();
        }

        apply_target();
        adc_capture_start();
        return;
    }

    if(blank) {
        // Only resume once the rail is comfortably back, not the instant it
        // crosses the trip point. Blanking also covers the first tick after
        // power_up(), so nothing lights until a real frame has been clocked
        // in behind a deasserted OEn.
        // On the mean: the minimum runs tens of counts below it on ripple
        // alone, so a minimum-vs-GOOD test can never be satisfied and the
        // panel stays dark on a rail that has already recovered.
        if(vled_counts >= VLED_GOOD_COUNTS) blank = false;
    }

    /*
    Everything past here regulates, and regulates on an average of whole
    captures.

    One capture is a 4ms window against a 3.15ms frame at the shortest OEn
    multiplier, and dma_complete() scans every row of a bitplane before moving
    to the next - so bitplane 9, half the frame's lit time, is one contiguous
    block that lands inside the window either once or twice depending on phase.
    Measured, that is 6% of scatter on the mean current, and the LED current is
    a small difference against a much larger idle draw, so at low light it comes
    out as tens of percent. Averaging whole captures walks the window across the
    frame and takes it back out.
    */
    accum_cur += cur_capture;
    accum_vled += vled_capture;
    if(vled_sag < accum_vled_min) accum_vled_min = vled_sag;
    if(cur_peak > accum_cur_peak) accum_cur_peak = cur_peak;
    if(calibrating && !blank && brightness_applied > 0.0f) accum_light += brightness_applied * grid_coverage();

    if(!blank) {
        if(calibrating && calibrate_phase == CALIBRATE_FILL) grid_light(CALIBRATE_SQUARES_PER_CAPTURE);
        if(presenting) present_step();
    }

    if(++accum_count < POWER_AVERAGE_CAPTURES) {
        apply_target();
        adc_capture_start();
        return;
    }

    const float cur_avg = accum_cur / (float)accum_count;
    const float vled_avg = accum_vled / (float)accum_count;
    last_light = accum_light / (float)accum_count;

    // Report what the loop acted on, not one arbitrary window of it.
    last_cur = (uint32_t)cur_avg;
    last_vled_mean = (uint32_t)vled_avg;
    last_cur_peak = accum_cur_peak;
    last_vled_min = accum_vled_min;

    accum_cur = 0.0f;
    accum_vled = 0.0f;
    accum_light = 0.0f;
    accum_count = 0;
    accum_vled_min = 4095;
    accum_cur_peak = 0;

    // LEDs only ever add to the panel's draw, so the smallest total we have
    // ever seen is the idle draw. Sampling it once at power_up() catches the
    // inrush instead, latching it above the real current - the LED current then
    // clamps to zero, the budget reads headroom that isn't there, and nothing
    // stops the ramp until the rail folds.
    if(cur_idle_avg <= 0.0f || cur_avg < cur_idle_avg) cur_idle_avg = cur_avg;
    cur_idle_counts = (uint32_t)cur_idle_avg;

    // Learn what this supply gives us with the LEDs off. Everything below is
    // measured against that, not against a nominal 5V. Track it both ways: a
    // running maximum would never notice the supply getting worse, and would
    // leave the panel chasing a target it can no longer reach.
    if(blank || brightness_applied <= 0.0f) {
        // Learn the open rail from the same statistic the budgets compare
        // against: the mean. The minimum of a window wanders with wherever
        // bitplane 9's OEn pulse lands in it - tens of counts of ripple on a
        // panel drawing no LED current at all - which would read as a droop
        // that was never there.
        vled_open_avg = vled_open_avg > 0.0f
            ? vled_open_avg + (vled_avg - vled_open_avg) * 0.125f
            : vled_avg;
        vled_open_counts = (uint32_t)vled_open_avg;

        // And the same window minimum droop is measured with below, so the
        // panel's own ripple appears at both ends and cancels.
        const float open_min = (float)last_vled_min;
        vled_open_min_avg = vled_open_min_avg > 0.0f
            ? vled_open_min_avg + (open_min - vled_open_min_avg) * 0.125f
            : open_min;
        vled_open_min_counts = (uint32_t)vled_open_min_avg;
    }

    // What the panel is drawing over its own idle load, and how far it is
    // pulling the rail down from open circuit. Kept, because the trip runs
    // between averaged ticks and has nothing fresher to work from.
    last_led = cur_avg > cur_idle_avg ? cur_avg - cur_idle_avg : 0.0f;

    /*
    Droop, minimum to minimum. The load pulls the rail down inside a frame and
    lets it back up, so the mean barely registers: measured, 10mV of mean-to-mean
    against a real dip of 280mV. Regulating on that reads as no droop at all, and
    the limit never engages however hard the panel is driven.
    */
    const float dip = vled_open_min_avg > (float)last_vled_min
                    ? vled_open_min_avg - (float)last_vled_min : 0.0f;
    last_sag = dip > (float)VLED_SAG_NOISE_COUNTS ? dip - (float)VLED_SAG_NOISE_COUNTS : 0.0f;

    // Never target a rail below the trip point plus its guard.
    uint32_t vled_target = VLED_TRIP_COUNTS + VLED_TRIP_MARGIN_COUNTS;
    if(vled_open_min_counts > vled_target + VLED_DROOP_COUNTS) {
        vled_target = vled_open_min_counts - VLED_DROOP_COUNTS;
    }

    last_sag_budget = vled_open_min_avg > (float)vled_target
                    ? vled_open_min_avg - (float)vled_target : 0.0f;
    const float budget = (float)cur_budget_counts;
    last_led_budget = budget > cur_idle_avg ? budget - cur_idle_avg : 0.0f;

    const float led = last_led;
    const float led_budget = last_led_budget;
    const float sag_budget = last_sag_budget;
    // Droop only counts once there is an open-circuit reading to measure it
    // against. Until then both it and its budget are zero, which would read as
    // over budget and stop the calibration walk on its first step.
    const bool sag_measured = vled_open_min_avg > 0.0f;
    const bool over_budget = led >= led_budget
                          || (sag_measured && last_sag >= sag_budget);

    if(blank) {
        // Nothing to regulate with the output off: the budgets describe LED
        // current that is not being drawn.
    } else if(calibrating) {
        if(calibrate_dwell) {
            // One averaged tick discarded after the fill stops, so that what is
            // measured is entirely the panel at its final drive.
            calibrate_dwell--;
        } else if(calibration_valid) {
            // Already measured on this supply, and the dwell above has just
            // refreshed the rail and idle draw the budgets are taken against.
            // Nothing to gain from lighting the panel again.
            gain_i = calibration_gain_i;
            gain_v = calibration_gain_v;
            ceiling_derate = calibration_derate;
            finish_calibration(led_budget, sag_budget);
        } else if(calibrate_phase == CALIBRATE_OPEN) {
            calibrate_phase = CALIBRATE_FILL;
            brightness_allowed = CALIBRATE_LIGHT;
        } else if(calibrate_phase == CALIBRATE_FILL) {
            if(over_budget || grid_lit >= GRID_SQUARES) {
                calibrate_phase = CALIBRATE_MEASURE;
                calibrate_dwell = CALIBRATE_DWELL;
            }
        } else {
            // Hold here and average, rather than taking the gain from whichever
            // single tick happened to cross the budget.
            calibrate_led_sum += led;
            calibrate_sag_sum += last_sag;
            calibrate_light_sum += last_light;

            if(++calibrate_measured >= CALIBRATE_MEASURE_TICKS) {
                gain_i = calibrate_light_sum > 0.0f ? calibrate_led_sum / calibrate_light_sum : 0.0f;
                gain_v = calibrate_light_sum > 0.0f ? calibrate_sag_sum / calibrate_light_sum : 0.0f;
                store_calibration();
                finish_calibration(led_budget, sag_budget);
            }
        }
    } else {
        // Nothing here reads the frame. The ceiling came off a real white
        // screen, so it already assumes every pixel could be lit, and only the
        // budgets or a backstop bite can move it.
        if(over_budget && ++over_budget_count >= BACKSTOP_DEBOUNCE) {
            over_budget_count = 0;
            if(ceiling_derate > BACKSTOP_DERATE_MIN) ceiling_derate -= BACKSTOP_DERATE_STEP;
        } else if(!over_budget) {
            over_budget_count = 0;
        }

        update_ceiling(led_budget, sag_budget);

        // Catch up with the ceiling rather than jumping to it, so a bite reads
        // as a dip. Retreat much faster than we advance.
        if(brightness_allowed > brightness_ceiling) {
            brightness_allowed -= BACKSTOP_STEP_DOWN;
            if(brightness_allowed < brightness_ceiling) brightness_allowed = brightness_ceiling;
        } else if(brightness_allowed < brightness_ceiling) {
            brightness_allowed += BACKSTOP_STEP_UP;
            if(brightness_allowed > brightness_ceiling) brightness_allowed = brightness_ceiling;
        }

        if(brightness_allowed < 0.0f) brightness_allowed = 0.0f;
    }

    apply_target();
    adc_capture_start();
}

}