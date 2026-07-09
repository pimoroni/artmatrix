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
const uint BRIGHTNESS_STEPS = BRIGHTNESS_MAX - BRIGHTNESS_MIN + 1;

// Duty cycle at each multiplier, relative to BRIGHTNESS_MAX. Not linear in the
// multiplier: the low bitplanes are bound by the data shift-out, so widening
// their OEn pulse buys nothing while still lengthening the frame.
static constexpr float OE_DUTY[BRIGHTNESS_STEPS] = {
    0.4765f, 0.5550f, 0.6199f, 0.6793f, 0.7339f, 0.7843f,
    0.8310f, 0.8744f, 0.9097f, 0.9421f, 0.9721f, 1.0000f
};

// Perceptual units per power_tick(), so ~0.4s from black to target.
constexpr float BRIGHTNESS_RAMP_STEP = 0.01f;

// A sag knocks the ceiling to 0.75x; this earns it back over roughly 8s, so a
// transient doesn't dim the panel for the rest of the session.
constexpr float BRIGHTNESS_CEILING_RECOVER = 0.0005f;

// The SDK's ADC_BASE_PIN is 40 on RP2350B, putting ADC0-7 on GPIO40-47.
const uint ADC_CUR_SENSE = 4;   // GPIO44
const uint ADC_SENSOR_IN = 5;   // GPIO45, the SENS header
const uint ADC_VLED_SENSE = 6;  // GPIO46

/*
384 samples at 96kSPS over three round-robin inputs spans 4ms, comfortably more
than one 3.15ms frame. Panel current is pulsed at the OEn duty cycle, with
structure at both the row slot and the frame, so a shorter window would just
catch one arbitrary corner of one bitplane.
*/
const uint ADC_SAMPLES = 384;
const uint ADC_PER_INPUT = ADC_SAMPLES / 3;
constexpr float ADC_CLKDIV = 499.0f; // 48MHz / (499 + 1)

/*
VLED_SENSE hangs off a 10k/10k divider, so a count is 1.61mV at the pin and
3.22mV of VLED. 5V reads 3102 of 4095.
*/
const uint32_t VLED_HEADROOM_COUNTS = 2854; // 4.6V, room to earn brightness back
const uint32_t VLED_GOOD_COUNTS = 2668;     // 4.3V, panel is happy
const uint32_t VLED_TRIP_COUNTS = 2482;     // 4.0V, blank the output
const uint32_t VLED_ABSENT_COUNTS = 1241;   // 2.0V, supply is gone, go hi-z
const uint VLED_DEBOUNCE = 3;               // captures, so ~12ms

/*
CUR_SENSE is the eFuse IMON output at 0.1906 V/A, so a count is 4.23mA. The
part limits at 3.3A (781 counts); stay below that so it never trips.
*/
const uint32_t CUR_BUDGET_COUNTS = 591;      // 2.5A
const uint32_t CUR_BUDGET_HYSTERESIS = 47;   // 200mA, so we don't hunt

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

    // Power management
    irq_handler_t irq_handler = nullptr;
    int dma_channel_adc = -1;
    bool panel_powered = false;
    bool blank = true;
    uint vled_good_count = 0;
    uint vled_absent_count = 0;
    uint16_t sensor_counts = 0;
    volatile float brightness_target = 1.0f; // written by core0
    float brightness_allowed = 0.0f;         // core1, what the supply will hold
    float brightness_ceiling = 1.0f;         // core1, what it held last time
    float brightness_applied = -1.0f;

    void gpio_hiz(void);
    void gpio_drive(void);
    void adc_capture_start(void);
    void power_up(void);
    void apply_gamma_scale(uint32_t scale);
    void apply_brightness(float value);

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

    void begin(irq_handler_t handler);
    void power_tick(void);
    void power_down(void);
    bool is_powered(void);

    void set_brightness(float value);
    uint16_t get_sensor(void);

    };
}