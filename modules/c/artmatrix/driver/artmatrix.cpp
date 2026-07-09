#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/structs/bus_ctrl.h"

#include "artmatrix.hpp"

namespace pimoroni {

static uint32_t __attribute__ ((aligned (4))) front_buffer[width * height];
static Pixel __attribute__ ((aligned (4))) back_buffer[width * height];

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
    GPIO_HIZ_BOTH(pin_oe);
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

        // Claim the PIO so we can clean it upon soft restart
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

        dma_channel_a1 = dma_claim_unused_channel(true);
        dma_channel_config config_a1 = dma_channel_get_default_config(dma_channel_a1);
        channel_config_set_transfer_data_size(&config_a1, DMA_SIZE_32);
        channel_config_set_bswap(&config_a1, false);
        channel_config_set_dreq(&config_a1, pio_get_dreq(pio_a, sm_data_a1, true));
        dma_channel_configure(dma_channel_a1, &config_a1, &pio_a->txf[sm_data_a1], NULL, 0, false);

        dma_channel_a2 = dma_claim_unused_channel(true);
        dma_channel_config config_a2 = dma_channel_get_default_config(dma_channel_a2);
        channel_config_set_transfer_data_size(&config_a2, DMA_SIZE_32);
        channel_config_set_bswap(&config_a2, false);
        channel_config_set_dreq(&config_a2, pio_get_dreq(pio_a, sm_data_a2, true));
        dma_channel_configure(dma_channel_a2, &config_a2, &pio_a->txf[sm_data_a2], NULL, 0, false);

        dma_channel_b1 = dma_claim_unused_channel(true);
        dma_channel_config config_b1 = dma_channel_get_default_config(dma_channel_b1);
        channel_config_set_transfer_data_size(&config_b1, DMA_SIZE_32);
        channel_config_set_bswap(&config_b1, false);
        channel_config_set_dreq(&config_b1, pio_get_dreq(pio_b, sm_data_b1, true));
        dma_channel_configure(dma_channel_b1, &config_b1, &pio_b->txf[sm_data_b1], NULL, 0, false);

        dma_channel_b2 = dma_claim_unused_channel(true);
        dma_channel_config config_b2 = dma_channel_get_default_config(dma_channel_b2);
        channel_config_set_transfer_data_size(&config_b2, DMA_SIZE_32);
        channel_config_set_bswap(&config_b2, false);
        channel_config_set_dreq(&config_b2, pio_get_dreq(pio_b, sm_data_b2, true));
        dma_channel_configure(dma_channel_b2, &config_b2, &pio_b->txf[sm_data_b2], NULL, 0, false);

        // Same handler for all four DMA channels
        irq_set_exclusive_handler(DMA_IRQ_1, handler);
        irq_set_priority(DMA_IRQ_1, PICO_HIGHEST_IRQ_PRIORITY);

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
    irq_set_enabled(DMA_IRQ_1, false);

    if(dma_channel_a1 != -1 &&  dma_channel_is_claimed(dma_channel_a1)) {
        dma_channel_set_irq1_enabled(dma_channel_a1, false);
        dma_channel_wait_for_finish_blocking(dma_channel_a1);
        dma_channel_cleanup(dma_channel_a1);
        dma_channel_unclaim(dma_channel_a1);
    }

    if(dma_channel_a2 != -1 &&  dma_channel_is_claimed(dma_channel_a2)) {
        dma_channel_wait_for_finish_blocking(dma_channel_a2);
        dma_channel_cleanup(dma_channel_a2);
        dma_channel_unclaim(dma_channel_a2);
    }

    if(dma_channel_b1 != -1 &&  dma_channel_is_claimed(dma_channel_b1)) {
        dma_channel_wait_for_finish_blocking(dma_channel_b1);
        dma_channel_cleanup(dma_channel_b1);
        dma_channel_unclaim(dma_channel_b1);
    }

    if(dma_channel_b2 != -1 &&  dma_channel_is_claimed(dma_channel_b2)) {
        dma_channel_wait_for_finish_blocking(dma_channel_b2);
        dma_channel_cleanup(dma_channel_b2);
        dma_channel_unclaim(dma_channel_b2);
    }

    irq_remove_handler(DMA_IRQ_1, handler);

    if(pio_sm_is_claimed(pio_a, sm_data_a1)) {
        pio_sm_set_enabled(pio_a, sm_data_a1, false);
        pio_sm_clear_fifos(pio_a, sm_data_a1);
        pio_sm_unclaim(pio_a, sm_data_a1);
    }

    if(pio_sm_is_claimed(pio_a, sm_data_a2)) {
        pio_sm_set_enabled(pio_a, sm_data_a2, false);
        pio_sm_clear_fifos(pio_a, sm_data_a2);
        pio_remove_program(pio_a, &artmatrix_data_rgb888_program, data_prog_offs_a);
        pio_sm_unclaim(pio_a, sm_data_a2);
    }

    if(pio_sm_is_claimed(pio_a, sm_row_a)) {
        pio_sm_set_enabled(pio_a, sm_row_a, false);
        pio_sm_clear_fifos(pio_a, sm_row_a);
        pio_remove_program(pio_a, &artmatrix_row_program, row_prog_offs_a);
        pio_sm_unclaim(pio_a, sm_row_a);
    }

    if(pio_sm_is_claimed(pio_b, sm_data_b1)) {
        pio_sm_set_enabled(pio_b, sm_data_b1, false);
        pio_sm_clear_fifos(pio_b, sm_data_b1);
        pio_sm_unclaim(pio_b, sm_data_b1);
    }

    if(pio_sm_is_claimed(pio_b, sm_data_b2)) {
        pio_sm_set_enabled(pio_b, sm_data_b2, false);
        pio_sm_clear_fifos(pio_b, sm_data_b2);
        pio_remove_program(pio_b, &artmatrix_data_rgb888_program, data_prog_offs_b);
        pio_sm_unclaim(pio_b, sm_data_b2);
    }

    if(pio_sm_is_claimed(pio_b, sm_row_b)) {
        pio_sm_set_enabled(pio_b, sm_row_b, false);
        pio_sm_clear_fifos(pio_b, sm_row_b);
        pio_remove_program(pio_b, &artmatrix_row_program, row_prog_offs_b);
        pio_sm_unclaim(pio_b, sm_row_b);
    }

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
    adc_run(false);

    if(dma_channel_adc != -1 && dma_channel_is_claimed(dma_channel_adc)) {
        dma_channel_cleanup(dma_channel_adc);
        dma_channel_unclaim(dma_channel_adc);
        dma_channel_adc = -1;
    }
}

// The panel isn't started here: power_tick() brings it up once VLED_SENSE
// reports a supply, and takes it back down to hi-z if that supply goes.
void ArtMatrix::begin(irq_handler_t handler) {
    irq_handler = handler;
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
static inline uint32_t gamma_pack(uint32_t rgb, uint32_t d) {
    return (((gamma_lut[rgb & 0xff] + d) >> 4) << 0)
         | (((gamma_lut[(rgb >> 8) & 0xff] + d) >> 4) << 10)
         | (((gamma_lut[(rgb >> 16) & 0xff] + d) >> 4) << 20);
}

void __no_inline_not_in_flash_func(ArtMatrix::update(void)) {
    const uint32_t *p = front_buffer;
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
            *b = gamma_pack(*p++, dither[x & 3]);
        }

        Pixel *a = &back_buffer[sx];
        for(uint32_t x = 0u; x < width / 2; x++, a += width) {
            *a = gamma_pack(*p++, dither[x & 3]);
        }
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

// Perceptual 0.0 - 1.0. Core1 only: it writes the table update() reads.
void ArtMatrix::apply_brightness(float value) {
    // Equal steps of `value` should look equally spaced, so go via CIE L*
    // rather than scaling light directly.
    const float l = value * 100.0f;
    const float t = (l + 16.0f) / 116.0f;
    float light = l > 8.0f ? t * t * t : l / 903.3f;

    // Take the lowest OEn multiplier whose duty covers the request, since a
    // higher one only costs refresh rate, then let the gamma scale make up the
    // shortfall exactly. The coarse steps are 3-14% of light on their own, so
    // interpolating between them is what keeps a ramp from visibly stepping.
    uint i = 0;
    while(i < BRIGHTNESS_STEPS - 1 && OE_DUTY[i] < light) i++;
    brightness_next = BRIGHTNESS_MIN + i;

    float scale = light / OE_DUTY[i];
    if(scale > 1.0f) scale = 1.0f;

    apply_gamma_scale((uint32_t)(scale * 1024.0f + 0.5f));
}

// Any core. Core1's power loop picks this up and ramps toward it.
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
    brightness_ceiling = 1.0f;
    brightness_applied = -1.0f;
    vled_absent_count = 0;
    blank = false;
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
    blank = true;
    panel_powered = false;
}

// Core1. Cheap to call often; the work only happens once a capture lands.
void ArtMatrix::power_tick(void) {
    if(dma_channel_is_busy(dma_channel_adc)) return;
    adc_run(false);

    uint32_t sum[3] = {0, 0, 0};
    for(uint32_t i = 0u; i < ADC_SAMPLES; i++) sum[i % 3] += adc_buffer[i];

    const uint32_t cur_counts = sum[0] / ADC_PER_INPUT;
    const uint32_t vled_counts = sum[2] / ADC_PER_INPUT;

    sensor_counts = (sum[1] / ADC_PER_INPUT) << 4; // 12-bit -> u16, as machine.ADC

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

    if(vled_counts >= VLED_ABSENT_COUNTS) vled_absent_count = 0;

    if(vled_counts < VLED_TRIP_COUNTS) {
        if(!blank) {
            // Blank before judging the supply. With the LEDs off an overload
            // recovers and a removal doesn't, and we can't tell them apart
            // while we're the thing pulling the rail down. Tearing the PIO
            // down here instead would unload the rail, read 5V, restart, and
            // collapse again - a flashing panel.
            blank = true;
            brightness_allowed = 0.0f;

            // Don't let the ramp walk straight back into the same sag.
            brightness_ceiling *= 0.75f;
        } else if(vled_counts < VLED_ABSENT_COUNTS && ++vled_absent_count >= VLED_DEBOUNCE) {
            // Still dead with the LEDs off, so the supply is gone rather than
            // overloaded. Stop driving before we leak into the panel again.
            power_down();
        }
    } else if(vled_counts >= VLED_GOOD_COUNTS) {
        blank = false;

        if(cur_counts > CUR_BUDGET_COUNTS) {
            brightness_allowed *= 0.9f;
        } else if(cur_counts < (CUR_BUDGET_COUNTS - CUR_BUDGET_HYSTERESIS)) {
            brightness_allowed += BRIGHTNESS_RAMP_STEP;
            if(brightness_allowed > brightness_ceiling) brightness_allowed = brightness_ceiling;

            // Only earn the ceiling back where the rail has real headroom.
            // Recovering inside the TRIP..GOOD band would creep into the sag
            // that cost us the ceiling in the first place, once per second.
            if(vled_counts >= VLED_HEADROOM_COUNTS) {
                brightness_ceiling += BRIGHTNESS_CEILING_RECOVER;
                if(brightness_ceiling > 1.0f) brightness_ceiling = 1.0f;
            }
        }
    }

    const float value = brightness_allowed < brightness_target ? brightness_allowed : brightness_target;
    if(value != brightness_applied) {
        apply_brightness(value);
        brightness_applied = value;
    }

    adc_capture_start();
}

}