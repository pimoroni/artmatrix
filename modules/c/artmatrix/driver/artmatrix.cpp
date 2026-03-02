#include <cstring>
#include <algorithm>
#include <cmath>

#include "hardware/clocks.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/address_mapped.h"
#include "hardware/structs/usb.h"

#include "artmatrix.hpp"

static inline bool usb_connected(void) {
    return (usb_hw->sie_status & USB_SIE_STATUS_CONNECTED_BITS) >> USB_SIE_STATUS_CONNECTED_LSB;
}

namespace pimoroni {

static uint32_t __attribute__ ((aligned (4))) front_buffer[width * height];
static Pixel __attribute__ ((aligned (4))) back_buffer[width * height];

#define GPIO_INIT_BOTH(pin, initial) gpio_init(pin); gpio_set_function(pin, GPIO_FUNC_SIO); gpio_set_dir(pin, true); gpio_put(pin, initial); \
                                     gpio_init(pin + 30); gpio_set_function(pin + 30, GPIO_FUNC_SIO); gpio_set_dir(pin + 30, true); gpio_put(pin + 30, initial)

ArtMatrix::ArtMatrix()
 {
    // Set up allllll the GPIO
    GPIO_INIT_BOTH(pin_r0, 0);
    GPIO_INIT_BOTH(pin_g0, 0);
    GPIO_INIT_BOTH(pin_b0, 0);

    GPIO_INIT_BOTH(pin_r1, 0);
    GPIO_INIT_BOTH(pin_g1, 0);
    GPIO_INIT_BOTH(pin_b1, 0);

    GPIO_INIT_BOTH(pin_row_a, 0);
    GPIO_INIT_BOTH(pin_row_b, 0);
    GPIO_INIT_BOTH(pin_row_c, 0);
    GPIO_INIT_BOTH(pin_row_d, 0);
    GPIO_INIT_BOTH(pin_row_e, 0);

    GPIO_INIT_BOTH(pin_clk, !clk_polarity);
    GPIO_INIT_BOTH(pin_stb, !stb_polarity);
    GPIO_INIT_BOTH(pin_oe, !oe_polarity);
}

void gpio_put_both(uint32_t pin, bool value) {
    gpio_put(pin, value);
    gpio_put(pin + 30, value);
}

void ArtMatrix::start(irq_handler_t handler) {
    if(handler) {
        // Prevent ghosting
        uint32_t latch_cycles = clock_get_hz(clk_sys) / 4000000;

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

        // Keep PIO constant when overclocked
        const float clock_hz = SYS_CLK_MHZ * 1000000;
        pio_sm_set_clkdiv(pio_a, sm_data_a1, clock_get_hz(clk_sys) / clock_hz);
        pio_sm_set_clkdiv(pio_a, sm_data_a2, clock_get_hz(clk_sys) / clock_hz);
        pio_sm_set_clkdiv(pio_b, sm_data_b1, clock_get_hz(clk_sys) / clock_hz);
        pio_sm_set_clkdiv(pio_b, sm_data_b2, clock_get_hz(clk_sys) / clock_hz);
        pio_sm_set_clkdiv(pio_a, sm_row_a, clock_get_hz(clk_sys) / clock_hz);
        pio_sm_set_clkdiv(pio_b, sm_row_b, clock_get_hz(clk_sys) / clock_hz);

        // Let PIO run wild and free!
        pio_sm_set_clkdiv(pio_a, sm_data_a1, 1.5f);
        pio_sm_set_clkdiv(pio_a, sm_data_a2, 1.5f);
        pio_sm_set_clkdiv(pio_b, sm_data_b1, 1.5f);
        pio_sm_set_clkdiv(pio_b, sm_data_b2, 1.5f);
        pio_sm_set_clkdiv(pio_a, sm_row_a, 1.5f);
        pio_sm_set_clkdiv(pio_b, sm_row_b, 1.5f);

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
        irq_add_shared_handler(DMA_IRQ_0, handler, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);

        dma_channel_set_irq0_enabled(dma_channel_a1, true);

        irq_set_enabled(DMA_IRQ_0, true);

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
    irq_set_enabled(DMA_IRQ_0, false);

    if(dma_channel_a1 != -1 &&  dma_channel_is_claimed(dma_channel_a1)) {
        dma_channel_set_irq0_enabled(dma_channel_a1, false);
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

    irq_remove_handler(DMA_IRQ_0, handler);

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
        pio_remove_program(pio_b, &artmatrix_row_inverted_program, row_prog_offs_b);
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
}

void __no_inline_not_in_flash_func(ArtMatrix::dma_complete()) {

    if(dma_channel_get_irq0_status(dma_channel_a1)) {
        dma_channel_acknowledge_irq0(dma_channel_a1);
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

        // Latch row data, pulse output enable for new row.
        pio_sm_put_blocking(pio_a, sm_row_a, row_a | ((brightness << bit_a) - 4) << 5);
        pio_sm_put_blocking(pio_b, sm_row_b, row_a | ((brightness << bit_a) - 4) << 5);

        row_a++;

        if(row_a == height / 4) {
            row_a = 0;
            bit_a++;
            if (bit_a == BIT_DEPTH) {
                bit_a = 0;
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

void ArtMatrix::update(void) {
    uint32_t *p = front_buffer;
    for(uint32_t y = 0u; y < height; y++) {
        // We are swapping X and Y to achieve a 90 degree rotation
        // Mirror along the Y axis (top to bottom)
        uint32_t sx = height - 1 - y;
        for(uint32_t x = 0u; x < width; x++) {
            uint32_t sy = x;
            uint32_t offset = sx;

            uint32_t rgb = *p++;

            // If we're on the second panel, shift up our pixel and adjust
            // the offset to place it into the latter half of the buffer.
            if(sy >= height / 2) {
                sy -= height / 2;
            } else {
                offset += panel_b_offset;
            }

            offset += sy * width;

            back_buffer[offset] = (GAMMA_10BIT[rgb & 0xff] << 0) | (GAMMA_10BIT[(rgb >> 8) & 0xff] << 10) | (GAMMA_10BIT[(rgb >> 16) & 0xff] << 20);
        }
    }
}

}