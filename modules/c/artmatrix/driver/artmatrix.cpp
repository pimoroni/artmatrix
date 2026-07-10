#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/structs/bus_ctrl.h"

#include "artmatrix.hpp"

namespace pimoroni {

static uint32_t __attribute__ ((aligned (4))) front_buffer[width * height];
static Pixel __attribute__ ((aligned (4))) back_buffer[width * height];

// front_buffer is what Python draws into, through memoryview(display), so it is
// only a whole frame at the instant update() is called. The servo repaints
// whenever it changes the gamma scale, which is any 4ms - reading front_buffer
// then would push half-drawn frames to the panel. Commit a copy instead.
static uint32_t __attribute__ ((aligned (4))) committed_buffer[width * height];

// Kept out of the class so ArtMatrix stays small enough for the placement-new
// buffer the bindings reserve for it.
static uint16_t __attribute__ ((aligned (4))) adc_buffer[ADC_SAMPLES];

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

void ArtMatrix::get_power_stats(uint32_t counts[7], float headroom[4]) {
    counts[0] = vled_open_counts;
    counts[1] = last_vled_mean;   // what the servo uses
    counts[2] = last_vled_min;    // what the trip uses
    counts[3] = last_cur;
    counts[4] = cur_idle_counts;
    counts[5] = frame_demand;
    counts[6] = last_demand_white;

    headroom[0] = last_headroom_v;
    headroom[1] = last_headroom_i;
    headroom[2] = brightness_allowed;
    headroom[3] = brightness_applied;
}

uint16_t ArtMatrix::get_sensor(void) {
    return sensor_counts;
}

bool ArtMatrix::is_powered(void) {
    return panel_powered;
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
        // on the next row instead of waiting for a repaint.
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

// gamma_lut holds 14 bits; the panel shows the top 10. `d` is 0..15, so the
// discarded bits round up in one pixel of the 4x4 cell per unit. Peak entry is
// 1023 << 4, so (16368 + 15) >> 4 == 1023 and this can't overflow its field.
//
// Accumulates the channel values as it goes. Panel current is proportional to
// duty(brightness) times this sum, which is what lets power_tick() ask "what
// would all-white cost at this brightness" instead of only ever measuring the
// frame in front of it.
static inline uint32_t gamma_pack(uint32_t rgb, uint32_t d, uint32_t *demand) {
    const uint32_t r = gamma_lut[rgb & 0xff];
    const uint32_t g = gamma_lut[(rgb >> 8) & 0xff];
    const uint32_t b = gamma_lut[(rgb >> 16) & 0xff];

    // Accumulate before the shift. The panel's current follows the shifted
    // values, but at low brightness most of those quantise to zero while
    // demand_white does not, so the ratio explodes and the loop dims itself
    // into the floor. The 14-bit values keep the ratio meaningful, and the
    // dither means they are what the light averages out to anyway.
    *demand += r + g + b;

    return ((r + d) >> 4) | (((g + d) >> 4) << 10) | (((b + d) >> 4) << 20);
}

// Take the frame Python has finished drawing, then render it.
void __no_inline_not_in_flash_func(ArtMatrix::update(void)) {
    for(uint32_t i = 0u; i < width * height; i++) committed_buffer[i] = front_buffer[i];
    repaint();
}

// Render the committed frame through the current gamma table. Safe to call at
// any time: nothing else writes committed_buffer.
void __no_inline_not_in_flash_func(ArtMatrix::repaint(void)) {
    const uint32_t *p = committed_buffer;
    uint32_t demand = 0u;

    for(uint32_t y = 0u; y < height; y++) {
        // We are swapping X and Y to achieve a 90 degree rotation
        // Mirror along the Y axis (top to bottom)
        const uint32_t sx = height - 1 - y;
        const uint8_t *dither = &BAYER_4X4[(y & 3) << 2];

        // Source x picks the panel, so walk each half separately. Within a
        // half the destination just advances one row (`width`) per pixel.
        // width / 2 is a multiple of 4, so the dither phase carries over.
        Pixel *b = &back_buffer[panel_b_offset + sx];
        for(uint32_t x = 0u; x < width / 2; x++, b += width) {
            *b = gamma_pack(*p++, dither[x & 3], &demand);
        }

        Pixel *a = &back_buffer[sx];
        for(uint32_t x = 0u; x < width / 2; x++, a += width) {
            *a = gamma_pack(*p++, dither[x & 3], &demand);
        }
    }

    frame_demand = demand;

    // Captured from the same gamma_lut as `demand`, in the same pass. The
    // servo changes brightness between flips, which rescales the whole table:
    // pairing a fresh demand_white with a stale demand skews the ratio by
    // exactly that change, and the loop oscillates at the frame rate.
    frame_demand_white = 3u * width * height * (uint32_t)gamma_lut[255];
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

// Perceptual 0.0 - 1.0 to a fraction of full light. Equal steps of the input
// should look equally spaced, which CIE L* gives us.
static float perceptual_to_light(float value) {
    const float l = value * 100.0f;
    const float t = (l + 16.0f) / 116.0f;
    return l > 8.0f ? t * t * t : l / 903.3f;
}

// Takes a fraction of full light, NOT a perceptual value. The servo regulates
// current and voltage, both linear in light, so it must control light. Driving
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
    // back_buffer still holds the last frame at its old scale, and nothing
    // repaints it until Python next calls update().
    for(uint32_t i = 0u; i < width * height; i++) back_buffer[i] = 0;
    apply_gamma_scale(0);

    gpio_drive();
    start(irq_handler);

    brightness_allowed = 0.0f;
    brightness_applied = -1.0f;
    vled_absent_count = 0;
    vled_open_counts = 0;
    vled_open_avg = 0.0f;
    cur_idle_counts = 0;
    cur_idle_avg = 0.0f;
    gain_i = 0.0f;
    gain_v = 0.0f;
    oe_index = 0;

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
    blank = true;
    panel_powered = false;
}

// Core1. Cheap to call often; the work only happens once a capture lands.
void ArtMatrix::power_tick(void) {
    if(dma_channel_is_busy(dma_channel_adc)) return;
    adc_run(false);

    uint32_t sum[3] = {0, 0, 0};
    uint32_t vled_sag = 4095;

    for(uint32_t i = 0u; i < ADC_SAMPLES; i++) {
        const uint32_t sample = adc_buffer[i];
        sum[i % 3] += sample;

        // Worst instantaneous sag across the window, not the mean. Panel
        // current swings hard within a frame - a bright generation of Life
        // drags the rail under the trip point and back before a mean ever
        // moves. Averaging hides precisely the transient we must react to.
        if((i % 3) == 2u && sample < vled_sag) vled_sag = sample;
    }

    const uint32_t cur_counts = sum[0] / ADC_PER_INPUT;
    const uint32_t vled_counts = sum[2] / ADC_PER_INPUT;

    // Keep the sub-count resolution the averaging bought us. Rounding 128
    // summed samples down to an integer throws away seven bits, and cur_led is
    // only single-digit counts on sparse content - the extrapolation then
    // multiplies each lost count by the content ratio, tenfold or more.
    const float cur_avg = (float)sum[0] / (float)ADC_PER_INPUT;
    const float vled_avg = (float)sum[2] / (float)ADC_PER_INPUT;

    sensor_counts = (sum[1] / ADC_PER_INPUT) << 4; // 12-bit -> u16, as machine.ADC
    last_vled_mean = vled_counts;
    last_vled_min = vled_sag;
    last_cur = cur_counts;

    if(!panel_powered) {
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

    if(vled_counts >= VLED_ABSENT_COUNTS) vled_absent_count = 0;

    // LEDs only ever add to the panel's draw, so the smallest total we have
    // ever seen is the idle draw. Sampling it once at power_up() catches the
    // inrush instead, latching it above the real current - cur_led then clamps
    // to zero, the budget reads headroom that isn't there, and nothing stops
    // the ramp until the rail folds.
    if(cur_idle_avg <= 0.0f || cur_avg < cur_idle_avg) cur_idle_avg = cur_avg;
    cur_idle_counts = (uint32_t)cur_idle_avg;

    // Learn what this supply gives us with the LEDs off. Everything below is
    // measured against that, not against a nominal 5V. Track it both ways: a
    // running maximum would never notice the supply getting worse, and would
    // leave the panel chasing a target it can no longer reach.
    if(blank || brightness_applied <= 0.0f) {
        // Learn the open rail from the same statistic the servo compares
        // against: the mean. The minimum of a window wanders with wherever
        // bitplane 9's OEn pulse lands in it - tens of counts of ripple on a
        // panel drawing no LED current at all - and the content ratio then
        // multiplies that noise into a droop that was never there.
        vled_open_avg = vled_open_avg > 0.0f
            ? vled_open_avg + (vled_avg - vled_open_avg) * 0.125f
            : vled_avg;
        vled_open_counts = (uint32_t)vled_open_avg;

        // The panel's own draw, with the LEDs off. Extrapolating to white has
        // to scale the LED current only; folding this in would over-predict a
        // dark screen badly, since it is most of what we measure there.
        if(cur_idle_avg <= 0.0f) cur_idle_avg = cur_avg;
    }

    if(vled_sag < VLED_TRIP_COUNTS) {
        if(!blank) {
            // Blank before judging the supply. With the LEDs off an overload
            // recovers and a removal doesn't, and we can't tell them apart
            // while we're the thing pulling the rail down. Tearing the PIO
            // down here instead would unload the rail, read 5V, restart, and
            // collapse again - a flashing panel.
            blank = true;

            // Halve rather than zero. Zeroing makes the panel re-ramp from
            // black, which is the visible pump; the servo below should keep
            // us from ever getting here.
            brightness_allowed *= 0.5f;
        } else if(vled_counts < VLED_ABSENT_COUNTS && ++vled_absent_count >= VLED_DEBOUNCE) {
            // Still dead with the LEDs off, so the supply is gone rather than
            // overloaded. Stop driving before we leak into the panel again.
            power_down();
        }
    } else if(blank) {
        // Only resume once the rail is comfortably back, not the instant it
        // crosses the trip point. Blanking also covers the first tick after
        // power_up(), so nothing lights until a real frame has been clocked
        // in behind a deasserted OEn.
        // On the mean: the minimum runs tens of counts below it on ripple
        // alone, so a minimum-vs-GOOD test can never be satisfied and the
        // panel stays dark on a rail that has already recovered.
        if(vled_counts >= VLED_GOOD_COUNTS) blank = false;
    } else {
        // Everything the supply cares about is linear in light: the LED current
        // and the droop both scale with it, and with how much of the screen is
        // lit. So rather than nudging brightness around by a normalised error,
        // measure the two plant gains and solve for the light that reaches each
        // limit. Slew toward the smaller. One step, no tuning.
        //
        // demand and demand_white come from the same repaint; their ratio is
        // the reciprocal of the lit fraction, and is free of brightness.
        const uint32_t demand_white = frame_demand_white;
        const uint32_t demand = frame_demand;
        last_demand_white = demand_white;

        const float light = brightness_applied;

        // Below a sliver of light, or of screen lit, the LED current is down in
        // the ADC noise and dividing by it invents headroom. Keep the last
        // gains instead - they describe the supply, which has not changed.
        if(light >= LIGHT_MIN_ESTIMATE && demand > 0u
           && (uint64_t)demand * FRAME_DEMAND_MIN_RECIP >= demand_white) {

            const float ratio = (float)demand_white / (float)demand;

            // What a white screen would draw, and droop, at the light we are
            // emitting right now - then per unit of light.
            const float led = cur_avg > cur_idle_avg ? cur_avg - cur_idle_avg : 0.0f;

            float sag = vled_open_avg > vled_avg ? vled_open_avg - vled_avg : 0.0f;
            sag = sag > (float)VLED_SAG_NOISE_COUNTS ? sag - (float)VLED_SAG_NOISE_COUNTS : 0.0f;

            const float gain_i_raw = led * ratio / light;
            const float gain_v_raw = sag * ratio / light;

            // The gains track content and supply, both slow. Filtering here
            // rather than downstream keeps the noise out of the answer without
            // putting lag between the limit and our response to it.
            gain_i += (gain_i_raw - gain_i) * POWER_GAIN_FILTER;
            gain_v += (gain_v_raw - gain_v) * POWER_GAIN_FILTER;
        }

        // Never target a rail below the trip point plus its guard.
        uint32_t vled_target = VLED_TRIP_COUNTS + VLED_TRIP_MARGIN_COUNTS;
        if(vled_open_counts > vled_target + VLED_DROOP_COUNTS) {
            vled_target = vled_open_counts - VLED_DROOP_COUNTS;
        }

        const float sag_budget = vled_open_avg > (float)vled_target ? vled_open_avg - (float)vled_target : 0.0f;
        const float led_budget = (float)CUR_BUDGET_COUNTS > cur_idle_avg ? (float)CUR_BUDGET_COUNTS - cur_idle_avg : 0.0f;

        // Invert each gain for the light it permits. An unmeasured gain permits
        // everything; the slew limit below keeps that honest.
        const float light_i = gain_i > 0.0f ? led_budget / gain_i : 1.0f;
        const float light_v = gain_v > 0.0f ? sag_budget / gain_v : 1.0f;

        last_headroom_v = light_v;
        last_headroom_i = light_i;

        float light_max = light_i < light_v ? light_i : light_v;
        if(light_max > 1.0f) light_max = 1.0f;

        // Slew, so a bad estimate or a step change in content can't jump the
        // panel. Retreat faster than we advance.
        if(light_max > brightness_allowed) {
            brightness_allowed += BRIGHTNESS_SLEW_UP;
            if(brightness_allowed > light_max) brightness_allowed = light_max;
        } else {
            brightness_allowed -= BRIGHTNESS_SLEW_DOWN;
            if(brightness_allowed < light_max) brightness_allowed = light_max;
        }

        if(brightness_allowed < 0.0f) brightness_allowed = 0.0f;
    }

    // brightness_allowed is light; the target the user set is perceptual.
    const float target_light = perceptual_to_light(brightness_target);
    const float value = brightness_allowed < target_light ? brightness_allowed : target_light;

    if(value != brightness_applied) {
        apply_brightness(value);
        brightness_applied = value;

        // The OEn multiplier lands on the next row, but the gamma scale only
        // reaches the panel through back_buffer - which update() writes, and
        // which Python only calls when it draws. On a static screen the fine
        // half of the brightness never applies at all, leaving the twelve
        // coarse OEn steps to quantise it: a 3-14% jump in light each time the
        // multiplier ticks over, which is exactly what a steady picture shows
        // as flicker. Repaint it ourselves - from the committed frame, never
        // from front_buffer, which Python may be part way through drawing.
        // This also refreshes frame_demand, which would otherwise be pinned to
        // whatever brightness was in force the last time the application drew.
        repaint();
    }

    adc_capture_start();
}

}