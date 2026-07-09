/*
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// -----------------------------------------------------
// NOTE: THIS HEADER IS ALSO INCLUDED BY ASSEMBLER SO
//       SHOULD ONLY CONSIST OF PREPROCESSOR DIRECTIVES
// -----------------------------------------------------

// This header may be included by other board headers as "boards/pimoroni_artmatrix.h"

#ifndef _BOARDS_PIMORONI_ARTMATRIX_H
#define _BOARDS_PIMORONI_ARTMATRIX_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

// --- BOARD SPECIFIC ---
#define PIMORONI_ARTMATRIX_PSRAM_CS_PIN (47)

#define PIMORONI_ARTMATRIX_SW_A (26)
#define PIMORONI_ARTMATRIX_SW_B (27)
#define PIMORONI_ARTMATRIX_SW_C (28)

#define PICO_PANIC_FUNCTION mp_pico_panic

// BW_VREG_VOLTAGE is applied by the crt0 patch before the clock boost, so the
// core is already at voltage by the time the PLL comes up.

// 399 MHz - Untested on ArtMatrix.
// ./build/micropython/lib/pico-sdk/src/rp2_common/hardware_clocks/scripts/vcocalc.py --cmake 399
/*
#define PLL_SYS_REFDIV   (1)
#define PLL_SYS_VCO_FREQ_HZ (1596000000)
#define PLL_SYS_POSTDIV1 (4)
#define PLL_SYS_POSTDIV2 (1)
#define SYS_CLK_HZ       (399000000)
#define BW_VREG_VOLTAGE  (0b01111)  // 1.30V
*/

// 266 MHz - Untested on ArtMatrix. On Tufty this was the maximum before a drop
// in PSRAM freq; not verified here.
// ./build/micropython/lib/pico-sdk/src/rp2_common/hardware_clocks/scripts/vcocalc.py --cmake 266
/*
#define PLL_SYS_REFDIV   (1)
#define PLL_SYS_VCO_FREQ_HZ (1596000000)
#define PLL_SYS_POSTDIV1 (6)
#define PLL_SYS_POSTDIV2 (1)
#define SYS_CLK_HZ       (266000000)
#define BW_VREG_VOLTAGE  (0b01101)  // 1.20V
*/

// 250 MHz - Working, but not tested on enough boards to fully verify.
// ./build/micropython/lib/pico-sdk/src/rp2_common/hardware_clocks/scripts/vcocalc.py --cmake 250
#define PLL_SYS_REFDIV   (1)
#define PLL_SYS_VCO_FREQ_HZ (1500000000)
#define PLL_SYS_POSTDIV1 (6)
#define PLL_SYS_POSTDIV2 (1)
#define SYS_CLK_HZ       (250000000)
#define BW_VREG_VOLTAGE  (0b01101)  // 1.20V

// 200 MHz - Most of our testing was done at 200MHz.
// ./build/micropython/lib/pico-sdk/src/rp2_common/hardware_clocks/scripts/vcocalc.py --cmake 200
/*
#define PLL_SYS_REFDIV   (1)
#define PLL_SYS_VCO_FREQ_HZ (1200000000)
#define PLL_SYS_POSTDIV1 (6)
#define PLL_SYS_POSTDIV2 (1)
#define SYS_CLK_HZ       (200000000)
#define BW_VREG_VOLTAGE  (0b01011)  // 1.10V
*/

// Support 250MHz if user manually overclocks
#define CYW43_PIO_CLOCK_DIV_INT 3

// --- RP2350 VARIANT ---
// not PICO_RP2350A

// --- UART ---
// no PICO_DEFAULT_UART
// no PICO_DEFAULT_UART_TX_PIN
// no PICO_DEFAULT_UART_RX_PIN

// --- LED ---
// no PICO_DEFAULT_LED_PIN
#define PICO_DEFAULT_WS2812_PIN 16

// --- I2C ---
#ifndef PICO_DEFAULT_I2C
#define PICO_DEFAULT_I2C 1
#endif
#ifndef PICO_DEFAULT_I2C_SDA_PIN
#define PICO_DEFAULT_I2C_SDA_PIN 14
#endif
#ifndef PICO_DEFAULT_I2C_SCL_PIN
#define PICO_DEFAULT_I2C_SCL_PIN 15
#endif

// --- SPI ---
#ifndef PICO_DEFAULT_SPI
#define PICO_DEFAULT_SPI 0
#endif
#ifndef PICO_DEFAULT_SPI_SCK_PIN
#define PICO_DEFAULT_SPI_SCK_PIN 18
#endif
#ifndef PICO_DEFAULT_SPI_TX_PIN
#define PICO_DEFAULT_SPI_TX_PIN 19 // SD_CMD (+ 20, 21 and 22 for 4-bit SD)
#endif
#ifndef PICO_DEFAULT_SPI_RX_PIN
#define PICO_DEFAULT_SPI_RX_PIN 16
#endif
#ifndef PICO_DEFAULT_SPI_CSN_PIN
#define PICO_DEFAULT_SPI_CSN_PIN 17
#endif

// --- FLASH ---

#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (16 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

// --- PSRAM ---

#ifndef PICO_PSRAM_CS_PIN
#define PICO_PSRAM_CS_PIN (PIMORONI_ARTMATRIX_PSRAM_CS_PIN)
#endif

pico_board_cmake_set_default(PICO_PSRAM_SIZE_BYTES, (8 * 1024 * 1024))
#ifndef PICO_PSRAM_SIZE_BYTES
#define PICO_PSRAM_SIZE_BYTES (8 * 1024 * 1024)
#endif

// Drive high to force power supply into PWM mode (lower ripple on 3V3 at light loads)
// no PICO_SMPS_MODE_PIN

// The GPIO Pin used to read VBUS to determine if the device is battery powered.
// no PICO_VBUS_PIN

// The GPIO Pin used to monitor VSYS. Typically you would use this with ADC.
// There is an example in adc/read_vsys in pico-examples.
// no PICO_VSYS_PIN

pico_board_cmake_set_default(PICO_RP2350_A2_SUPPORTED, 1)
#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

// PICO_CONFIG: CYW43_PIN_WL_DYNAMIC, flag to indicate if cyw43 SPI pins can be changed at runtime, type=bool, default=false, advanced=true, group=pico_cyw43_driver
#ifndef CYW43_PIN_WL_DYNAMIC
#define CYW43_PIN_WL_DYNAMIC 0
#endif

// PICO_CONFIG: CYW43_DEFAULT_PIN_WL_REG_ON, gpio pin to power up the cyw43 chip, type=int, default=23, advanced=true, group=pico_cyw43_driver
#ifndef CYW43_DEFAULT_PIN_WL_REG_ON
#define CYW43_DEFAULT_PIN_WL_REG_ON 23u
#endif

// PICO_CONFIG: CYW43_DEFAULT_PIN_WL_DATA_OUT, gpio pin for spi data out to the cyw43 chip, type=int, default=24, advanced=true, group=pico_cyw43_driver
#ifndef CYW43_DEFAULT_PIN_WL_DATA_OUT
#define CYW43_DEFAULT_PIN_WL_DATA_OUT 24u
#endif

// PICO_CONFIG: CYW43_DEFAULT_PIN_WL_DATA_IN, gpio pin for spi data in from the cyw43 chip, type=int, default=24, advanced=true, group=pico_cyw43_driver
#ifndef CYW43_DEFAULT_PIN_WL_DATA_IN
#define CYW43_DEFAULT_PIN_WL_DATA_IN 24u
#endif

// PICO_CONFIG: CYW43_DEFAULT_PIN_WL_HOST_WAKE, gpio (irq) pin for the irq line from the cyw43 chip, type=int, default=24, advanced=true, group=pico_cyw43_driver
#ifndef CYW43_DEFAULT_PIN_WL_HOST_WAKE
#define CYW43_DEFAULT_PIN_WL_HOST_WAKE 24u
#endif

// PICO_CONFIG: CYW43_DEFAULT_PIN_WL_CLOCK, gpio pin for the spi clock line to the cyw43 chip, type=int, default=29, advanced=true, group=pico_cyw43_driver
#ifndef CYW43_DEFAULT_PIN_WL_CLOCK
#define CYW43_DEFAULT_PIN_WL_CLOCK 29u
#endif

// PICO_CONFIG: CYW43_DEFAULT_PIN_WL_CS, gpio pin for the spi chip select to the cyw43 chip, type=int, default=25, advanced=true, group=pico_cyw43_driver
#ifndef CYW43_DEFAULT_PIN_WL_CS
#define CYW43_DEFAULT_PIN_WL_CS 25u
#endif

#endif
