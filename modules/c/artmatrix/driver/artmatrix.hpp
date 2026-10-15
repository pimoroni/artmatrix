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

/*
Captures the power loop averages before regulating on the result. One capture
spans 4ms against a frame of 3.15ms at the shortest OEn multiplier, and the scan
is bitplane-major, so bitplane 9 - half the frame's lit time - falls inside the
window once or twice depending on phase. Measured, that is 6% of scatter on the
mean current, which the idle-draw subtraction turns into tens of percent on the
LED current at low light.

Eight captures span about ten frames, taking the worst-case phase error to under
1%, and give the loop a 32ms period, which is still far faster than anything it
regulates. The trip is unaffected - it runs on every capture.
*/
const uint POWER_AVERAGE_CAPTURES = 8;

/*
Calibration lights splash.py's grid of squares in white, a column at a time from
the left, until the grid is full or the supply says stop. The gain is taken per
unit of all-white light from the lit fraction, so the ceiling is still that of an
all-white screen, extrapolated from the grid's share of it. The squares are then
tinted to the colours splash.py starts from.
*/
constexpr float CALIBRATE_LIGHT = 0.5f;
const uint CALIBRATE_SQUARES_PER_CAPTURE = 2;
constexpr float CALIBRATE_PRESENT_STEP = 0.01f;
const uint CALIBRATE_TINT_CAPTURES = 100;

const uint GRID_SIZE = 18;
const uint GRID_PITCH = 7;
const uint GRID_INSET = 2;
const uint GRID_SQUARE = 5;
const uint GRID_SQUARES = GRID_SIZE * GRID_SIZE;
const uint GRID_HUE_PER_PIXEL = 2;
const uint GRID_SATURATION = 255;
const uint GRID_VALUE = 102;

// Averaged ticks to discard after each step, so that what the next one measures
// is entirely the panel at its new drive rather than its approach to it.
const uint CALIBRATE_DWELL = 1;

// Averaged ticks to hold at the stopping point and take the gain from. A single
// tick came in 3.4% either side of sustained draw across two calibrations, and
// that lands directly on the ceiling: the panel came up 0.427 one boot and 0.412
// the next. Three ticks cost 96ms against a walk of some 800ms.
const uint CALIBRATE_MEASURE_TICKS = 3;

// Averaged ticks to hold at black before stepping, so the open-circuit rail and
// the panel's idle draw are learned before anything is measured against them.
const uint CALIBRATE_OPEN_DWELL = 2;

/*
Back off the point calibration stopped at.

Not for warm-up: an hour of all-white at the ceiling moved the panel's current
by +0.06%, against a noise floor of +/-0.18% on the mean. The drivers are
constant current, so forward voltage falling as the panel heats does not change
what it draws.

What this covers is calibration's own error. Its gain is measured over a short
run of ticks at the stopping point, and against the same panel in sustained use
a single tick came in 3.4% either side across two calibrations. Averaging pulls
that in; this is what is left, with a little over. It has to cover the
optimistic direction in particular, since a low gain gives a high ceiling.
*/
constexpr float CALIBRATE_MARGIN = 0.95f;

/*
The ceiling already assumes every pixel is white, so no picture should reach it.
The backstop is for what calibration could not see: a panel that has warmed up,
a supply that has aged, a thinner cable swapped in.

It takes the ceiling itself down a notch and leaves it there until the next
power-up. Tracking the load instead would put brightness back under the control
of the picture, which is the whole thing calibration exists to avoid: bright
scenes would dim the panel and dark ones would let it climb again.
*/
constexpr float BACKSTOP_DERATE_STEP = 0.02f;   // fraction of the ceiling per bite
constexpr float BACKSTOP_DERATE_MIN = 0.25f;    // never take more than three quarters

// Consecutive over-budget captures before the backstop bites. Nothing puts the
// ceiling back, so an isolated ADC outlier must not be able to ratchet it down.
const uint BACKSTOP_DEBOUNCE = 3;

// Light per power_tick() while brightness catches up with a ceiling that has
// moved, so a derate reads as a dip and not a step.
constexpr float BACKSTOP_STEP_DOWN = 0.05f;
constexpr float BACKSTOP_STEP_UP = 0.0005f;

// Reaching the trip means the ceiling itself was wrong for this supply. Take a
// real bite out of it, or brightness climbs straight back into the collapse.
constexpr float TRIP_DERATE = 0.75f;

// OE_DUTY is calculated, not measured, so the gamma scale cannot interpolate a
// multiplier change away perfectly. Don't step back down until well clear of
// the boundary, or it chatters and each toggle is visible.
constexpr float OE_STEP_HYSTERESIS = 0.06f;

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
VLED_SENSE hangs off a 10k/10k divider. A count is 0.806mV at the pin (3.3V
over 12 bits) and so 1.61mV of VLED. 5V reads 3102 of 4095.
*/
const uint32_t VLED_GOOD_COUNTS = 2668;     // 4.3V, panel is happy
const uint32_t VLED_TRIP_COUNTS = 2482;     // 4.0V, blank the output
const uint32_t VLED_ABSENT_COUNTS = 1241;   // 2.0V, supply is gone, go hi-z
const uint VLED_DEBOUNCE = 3;               // captures, so ~12ms

/*
How far the rail may sag below its own open-circuit voltage before we stop
ramping. Measured against what this supply actually delivers unloaded rather
than against 5V, because a thin cable's IR drop shows up as a lower loaded
rail at a current far below any sensible budget - absolute thresholds would
either never engage or never let go.
*/
// Droop is measured minimum-to-minimum, so the panel's own ripple cancels and
// this only has to cover the scatter left on the minimum statistic itself.
const uint32_t VLED_SAG_NOISE_COUNTS = 8;      // ~13mV

// How far the loaded minimum may fall below the open-circuit minimum. Sized
// from the panel rather than from a guess: an open rail of 5.03V less 700mV
// lands on 4.33V, which is where VLED_GOOD_COUNTS says the panel is happy.
const uint32_t VLED_DROOP_COUNTS = 434;        // 700mV

// Guard band between where calibration stops and where the trip fires.
// Calibration walks up until the rail reaches its droop target, so without this
// the last step of that walk would be a brownout.
const uint32_t VLED_TRIP_MARGIN_COUNTS = 124;  // 200mV

/*
CUR_SENSE is the TPS22811's IMON output. 0.1906 V/A appears across the whole
2k (R18 + R17), but CURRENT_SENSE taps their midpoint, so the ADC sees half of
it: 0.0953 V/A, and a count is 8.46mA rather than 4.23mA.

Soft start ramps VLED over 15ms,
which pushes 5000uF * 0.33V/ms = 1.65A of inrush (195 counts) - and that is
exactly what CUR_SENSE reads at power_up, so it must not be mistaken for the
panel's idle draw.
*/
constexpr float CUR_COUNTS_PER_AMP = 118.26f;  // 1A / 8.46mA

/*
Total panel draw allowed, LEDs and idle together.

The load switch is good for 10A continuous, so it is not the constraint. The
shipped supply is 5A and tested to deliver it, and this leaves it half an amp.

Peaks run about 2.8x the mean, since BCM puts half of each frame's lit time in
bitplane 9 and the scan is bitplane-major, so this implies roughly 12A of
sub-millisecond peak. VLED's bulk capacitance carries that between pulses; the
rail dip it costs is what the droop budget above regulates.

set_current_limit() lowers this for a weaker supply.
*/
const uint32_t CUR_LIMIT_COUNTS = 532;    // 4.5A, the most we may ever ask for
const uint32_t CUR_BUDGET_COUNTS = CUR_LIMIT_COUNTS;

// Soft start is 15ms; ignore four captures so neither the idle current nor the
// open-circuit rail is learned from the middle of the ramp.
const uint POWER_SETTLE_CAPTURES = 5;

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

enum CalibratePhase {
    CALIBRATE_OPEN,
    CALIBRATE_FILL,
    CALIBRATE_MEASURE
};

// Ambient LEDs: four WS2812s lit from the top AMBIENT_ROWS of the frame, split
// into LED_COUNT zones left to right. Pixels with every channel at or below
// AMBIENT_BLACK are left out of a zone's average, so a shape on black lights
// its LED in its own colour rather than a dim smear of it.
const uint LED_COUNT = 4;
const uint32_t AMBIENT_ROWS = 32;
const uint32_t AMBIENT_BLACK = 16;

// 60Hz, far longer than the 120us shift-out plus the 280us latch.
const uint32_t LED_PERIOD_US = 16667;

// LED light at full panel brightness. WS2812s at full drive are glaring.
const float AMBIENT_SCALE = 0.6f;

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
    uint settle_count = 0;
    uint oe_index = 0;
    uint16_t sensor_counts = 0;
    uint32_t vled_open_counts = 0;           // core1, rail with the LEDs off
    uint32_t vled_open_min_counts = 0;       // core1, its worst sample, what droop is measured against
    uint32_t cur_idle_counts = 0;            // core1, panel draw with LEDs off
    volatile uint32_t cur_budget_counts = CUR_BUDGET_COUNTS; // written by core0

    // Core1. While either is set, back_buffer holds the calibration grid and
    // update() leaves it alone.
    bool calibrating = false;
    bool presenting = false;
    CalibratePhase calibrate_phase = CALIBRATE_OPEN;
    uint calibrate_dwell = 0;
    uint grid_lit = 0;
    uint grid_tint = 0;
    uint over_budget_count = 0;

    // A run of captures, averaged before the loop regulates on it. See
    // POWER_AVERAGE_CAPTURES.
    float calibrate_led_sum = 0.0f;
    float calibrate_sag_sum = 0.0f;
    float calibrate_light_sum = 0.0f;
    uint calibrate_measured = 0;

    float accum_cur = 0.0f;
    float accum_vled = 0.0f;
    float accum_light = 0.0f;
    uint accum_count = 0;
    uint32_t accum_vled_min = 4095;
    uint32_t accum_cur_peak = 0;

    // The last averaged tick's working values. The trip runs between ticks and
    // has nothing fresher to reach for.
    float last_led = 0.0f;
    float last_sag = 0.0f;
    float last_light = 0.0f;
    float last_led_budget = 0.0f;
    float last_sag_budget = 0.0f;

    // Last values the loop acted on, for power_stats().
    uint32_t last_vled_mean = 0;
    uint32_t last_vled_min = 0;
    uint32_t last_cur = 0;
    uint32_t last_cur_peak = 0;
    float gain_i = 0.0f;                     // all-white LED counts per light
    float gain_v = 0.0f;                     // all-white sag counts per light
    float cur_idle_avg = 0.0f;
    float vled_open_avg = 0.0f;
    float vled_open_min_avg = 0.0f;
    float last_headroom_v = 0.0f;
    float last_headroom_i = 0.0f;
    volatile float brightness_target = 1.0f; // written by core0
    volatile bool force_light = false;       // written by core0
    volatile bool recalibrate_request = false; // written by core0
    volatile bool ambient = true;            // written by core0
    float brightness_ceiling = 0.0f;         // core1, LIGHT an all-white screen may have
    float ceiling_derate = 1.0f;             // core1, what the trips have taken off it
    float brightness_allowed = 0.0f;         // core1, LIGHT the supply will hold
    float brightness_applied = -1.0f;

    void gpio_hiz(void);
    void gpio_drive(void);
    void adc_capture_start(void);
    void power_up(void);
    void release(void);
    void apply_gamma_scale(uint32_t scale);
    void apply_brightness(float light);
    void update_ceiling(float led_budget, float sag_budget);
    void apply_target(void);
    void store_calibration(void);
    void start_calibration(void);
    void finish_calibration(float led_budget, float sag_budget);
    void present_step(void);
    void fill_back_buffer(uint32_t packed);
    void grid_draw_square(uint index, uint32_t rgb);
    void grid_light(uint count);
    void grid_repaint(void);
    float grid_coverage(void);

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

    // Ambient LEDs, on whichever PIO had room. Core1 only.
    PIO led_pio = nullptr;
    uint led_sm = 0;
    uint led_prog_offs = 0;
    uint32_t led_sent_us = 0;

    void send_leds(const uint32_t grb[LED_COUNT]);

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
    uint32_t* get_surface(uint32_t index);
    void update(uint8_t blend = 0u);

    void begin(irq_handler_t handler);
    void power_tick(void);
    void power_down(void);
    bool is_powered(void);
    bool is_calibrating(void);

    void set_brightness(float value);
    void set_ambient(bool on);
    void led_tick(void);
    void leds_off(void);
    void set_current_limit(float amps);
    void set_force_on(bool force);
    void recalibrate(void);
    uint16_t get_sensor(void);

    // Raw power state, so the operating point can be inspected from Python
    // rather than inferred. Counts, not volts/amps: 3.22mV and 4.23mA each.
    void get_power_stats(uint32_t counts[7], float headroom[7]);

    };
}