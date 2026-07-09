#include <stdint.h>
#include "pico/stdlib.h"

#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"

#ifndef NO_QSTR
#include "artmatrix.pio.h"
#endif

namespace pimoroni {
const uint DATA_BASE_PIN = 0;
const uint DATA_N_PINS = 6;
const uint ROWSEL_BASE_PIN = 6;
const uint ROWSEL_N_PINS = 5;
const uint BIT_DEPTH = 10;

/*
OEn pulse width multiplier. Bitplane 0's pulse is `brightness - 4` cycles, so
anything below 5 underflows the 27-bit width field and lights the whole row.
The ceiling trades refresh rate for duty cycle: 5 gives ~31% at ~318Hz, 16
gives ~65% at ~208Hz. Only ~2.1x, since the low bitplanes are bound by the
data shift-out, not by OEn.
*/
const uint BRIGHTNESS_MIN = 5;
const uint BRIGHTNESS_MAX = 16;

/*
14-bit gamma table, gamma correcting our 8-bit colour values into four bits
more than the panel can show. update() dithers the surplus away with a 4x4
ordered pattern, so we get those four bits back as spatial resolution.

The panel only has 10 bitplanes and can't gain more: the PIO eats one 32-bit
RGB word per pixel, and 3 x 11 bits doesn't fit. Bit depth is also expensive,
since BCM frame time is dominated by the MSB's OEn pulse - 14 real bitplanes
would drop the refresh rate from ~318Hz to ~56Hz.

Those four bits are what set_brightness() dims into. A 10-bit table has none
to spare: gamma 2.2 rises by less than one code per step below input 42, so a
10-bit table has to be flattened onto a ramp there, and any scaling crushes
it. At 14 bits the curve is monotone on its own, and scaling to 1/16 still
resolves 233 of 256 levels (against 65 for a 10-bit table).

Calculated with:

top = 1023 << 4  # (top + 15) >> 4 == 1023, so the dither can't overflow
gamma_lut = [int(round(top * (x / (256 - 1)) ** 2.2)) for x in range(256)]

Floored at 1 for non-zero inputs. The dither resolves 1/16 of an LSB, so a 1
here shows as one lit pixel in sixteen rather than going black.

This is the reference table only; set_brightness() scales it into the SRAM
copy that update() actually reads.
*/
static constexpr uint16_t GAMMA_14BIT[256] = {
    0, 1, 1, 1, 2, 3, 4, 6, 8, 10, 13, 16,
    20, 23, 28, 32, 37, 42, 48, 54, 61, 67, 75, 82,
    90, 99, 108, 117, 127, 137, 148, 159, 170, 182, 194, 207,
    221, 234, 248, 263, 278, 294, 310, 326, 343, 360, 378, 396,
    415, 435, 454, 475, 495, 516, 538, 560, 583, 606, 630, 654,
    678, 704, 729, 755, 782, 809, 837, 865, 894, 923, 952, 983,
    1013, 1045, 1076, 1109, 1141, 1175, 1208, 1243, 1278, 1313, 1349, 1385,
    1422, 1460, 1498, 1537, 1576, 1615, 1656, 1696, 1738, 1779, 1822, 1865,
    1908, 1952, 1997, 2042, 2087, 2134, 2180, 2228, 2276, 2324, 2373, 2422,
    2473, 2523, 2574, 2626, 2678, 2731, 2785, 2839, 2893, 2949, 3004, 3061,
    3117, 3175, 3233, 3292, 3351, 3410, 3471, 3532, 3593, 3655, 3718, 3781,
    3845, 3909, 3974, 4040, 4106, 4172, 4240, 4308, 4376, 4445, 4515, 4585,
    4656, 4727, 4799, 4872, 4945, 5019, 5093, 5168, 5244, 5320, 5397, 5474,
    5552, 5631, 5710, 5790, 5870, 5951, 6033, 6115, 6198, 6282, 6366, 6450,
    6536, 6622, 6708, 6795, 6883, 6971, 7060, 7150, 7240, 7331, 7422, 7514,
    7607, 7700, 7794, 7889, 7984, 8080, 8176, 8273, 8371, 8469, 8568, 8667,
    8767, 8868, 8970, 9072, 9174, 9278, 9381, 9486, 9591, 9697, 9803, 9911,
    10018, 10127, 10236, 10345, 10456, 10566, 10678, 10790, 10903, 11016, 11131, 11245,
    11361, 11477, 11593, 11711, 11829, 11947, 12067, 12186, 12307, 12428, 12550, 12673,
    12796, 12919, 13044, 13169, 13295, 13421, 13548, 13676, 13804, 13933, 14063, 14193,
    14324, 14456, 14588, 14721, 14855, 14989, 15124, 15260, 15396, 15533, 15670, 15808,
    15947, 16087, 16227, 16368,
};

struct Pixel {
    uint32_t color;
    constexpr Pixel() : color(0) {};
    constexpr Pixel(uint32_t color) : color(color) {};
};

const uint32_t width = 128;
const uint32_t height = 128;

class ArtMatrix {
    public:

    private:
    const uint32_t panel_b_offset = (width * height / 2);

    // DMA & PIO
    int dma_channel_a1 = -1;
    int dma_channel_a2 = -1;
    int dma_channel_b1 = -1;
    int dma_channel_b2 = -1;

    uint bit_a = 0;
    uint row_a = 0;

    const PIO pio_a = pio1;
    const PIO pio_b = pio2;

    const uint sm_data_a1 = 0;
    const uint sm_data_a2 = 1;
    const uint sm_row_a = 2;

    const uint sm_data_b1 = 0;
    const uint sm_data_b2 = 1;
    const uint sm_row_b = 2;

    uint data_prog_offs_a = 0;
    uint row_prog_offs_a = 0;

    uint data_prog_offs_b = 0;
    uint row_prog_offs_b = 0;

    // Owned by the DMA IRQ; latched from brightness_next at the frame boundary
    // so a frame never mixes bitplane weights.
    uint brightness = BRIGHTNESS_MIN;
    volatile uint brightness_next = BRIGHTNESS_MIN;

    // Top half of display - 16 rows on a 32x32 panel
    uint pin_r0 = 0;
    uint pin_g0 = 1;
    uint pin_b0 = 2;

    // Bottom half of display - 16 rows on a 64x64 panel
    uint pin_r1 = 3;
    uint pin_g1 = 4;
    uint pin_b1 = 5;

    // Address pins, 5 lines = 2^5 = 32 values (max 64x64 display)
    uint pin_row_a = 6;
    uint pin_row_b = 7;
    uint pin_row_c = 8;
    uint pin_row_d = 9;
    uint pin_row_e = 10;

    // Sundry things
    uint pin_clk = 11;    // Clock
    uint pin_stb = 12;    // Strobe/Latch
    uint pin_oe = 13;     // Output Enable

    const bool clk_polarity = 1;
    const bool stb_polarity = 1;
    const bool oe_polarity = 0;

    public:
    ArtMatrix(void);
    ~ArtMatrix();

    void start(irq_handler_t handler);
    void stop(irq_handler_t handler);
    void dma_complete();

    uint32_t* get_framebuffer(void);
    void update(void);

    void set_brightness(float value);
    void set_full_brightness(bool full);

    };
}