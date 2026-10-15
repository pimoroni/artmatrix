#include <cstdio>
#include "driver/artmatrix.hpp"
#include "pico/multicore.h"
#include "pico/runtime.h"
#include "hardware/timer.h"

#include "micropython/modules/util.hpp"


using namespace pimoroni;

// #define DEBUG 1

extern "C" {
#include "artmatrix_bindings.h"
#include "py/builtin.h"
#include "py/mpthread.h"

// HACK: Shim up missing rp2_tud functions from our MSC patches
bool rp2_tud_set_msc_ready() {
    return false;
}

bool rp2_tud_is_msc_busy() {
    return false;
}

void __printf_debug_flush() {
    for(auto i = 0u; i < 10; i++) {
        sleep_ms(2);
        mp_event_handle_nowait();
    }
}

int mp_vprintf(const mp_print_t *print, const char *fmt, va_list args);

#if DEBUG
void artmatrix_debug(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = mp_vprintf(&mp_plat_print, fmt, ap);
    va_end(ap);
    __printf_debug_flush();
    (void)ret;
}
#else
#define artmatrix_debug(fmt, ...)
#endif

void mp_pico_panic(const char *fmt, ...) {
    va_list args;

    if (fmt) {
        va_start(args, fmt);
        int ret = mp_vprintf(&mp_plat_print, fmt, args);
        (void)ret;
        va_end(args);
    }

    while (1) {
        mp_event_handle_nowait();
        sleep_ms(1);
    }
}

typedef struct _mp_obj_float_t {
    mp_obj_base_t base;
    mp_float_t value;
} mp_obj_float_t;

const uint ARTMATRIX_WIDTH = 128;
const uint ARTMATRIX_HEIGHT = 128;

/********** WS2812 **********/

/***** Variables Struct *****/
typedef struct _artmatrix_obj_t {
    mp_obj_base_t base;
    ArtMatrix* artmatrix;
    void *buf;
    volatile bool exit_core1;
    volatile bool flip;
    bool blocking;
    // While set, update() does nothing. The application holds this over a
    // plugin's draw call so the plugin cannot flip a half-drawn surface, or
    // present the wrong one part way through a transition. Suppressed calls are
    // counted so the caller can report a plugin that tries.
    bool hold;
    uint32_t held_calls;
    // Mix of the two drawing surfaces for the next flip. Set before flip, so
    // core1 reads a value that belongs to the frame it is about to commit.
    volatile uint8_t blend;
} _artmatrix_obj_t;

static _artmatrix_obj_t artmatrix_obj;

void __isr __not_in_flash_func(dma_complete)() {
    if(artmatrix_obj.artmatrix) artmatrix_obj.artmatrix->dma_complete();
}

#define stack_size 8192u
#define ARTMATRIX_STARTUP_DELAY_MS 250u
static uint32_t core1_stack[stack_size] = {0};


// ArtMatrix must be allocated into SRAM (not PSRAM), so reserve a buffer
// for it here - Presto_make_new will placement new into this buffer.
__attribute__((section(".uninitialized_data"))) static uint32_t artmatrix_buffer[sizeof(ArtMatrix) / sizeof(uint32_t)];


void artmatrix_core1_entry() {
    // Installs the SDK's SIO FIFO lockout handler. Since pico-sdk 2.2.0 the lockout
    // release is signalled by mutating multicore.c's private lockout_request_id, so a
    // downstream copy of the handler cannot see it; use the SDK's own.
    multicore_lockout_victim_init();

    artmatrix_obj.artmatrix->begin(dma_complete);

    multicore_fifo_push_blocking(0); // TODO: handle issues here?

    // artmatrix runs the display from interrupts on this core, once
    // power_tick() sees a supply on VLED_SENSE and brings the panel up.
    while (!artmatrix_obj.exit_core1) {
        artmatrix_obj.artmatrix->power_tick();

        // Hold the flip through calibration instead of dropping it: back_buffer
        // is the calibration grid, and the boot screen is drawn once and then
        // left up while the application connects to wifi.
        if (artmatrix_obj.flip && !artmatrix_obj.artmatrix->is_calibrating()) {
            artmatrix_obj.artmatrix->update(artmatrix_obj.blend);
            artmatrix_obj.flip = false;
            continue;
        }

        if (artmatrix_obj.artmatrix->is_powered()) {
            // Core0 __sev()s after setting either flag, and the display DMA IRQ
            // wakes us regardless, so don't spin on the bus the DMA is using.
            __wfe();
        } else {
            // No display DMA to wake us, so keep polling for the supply.
            sleep_us(500);
        }
    }

    artmatrix_obj.artmatrix->power_down();

    multicore_lockout_victim_deinit();

    multicore_fifo_push_blocking(0);
}

void artmatrix_core1_reset() {
    multicore_reset_core1();
    artmatrix_obj.exit_core1 = false;
    artmatrix_obj.flip = false;
}

void artmatrix_core1_start() {
    artmatrix_debug("launch core1\n");
    // Micropython uses all of both scratch memory (and more!) for core0 stack, 
    // so we must supply our own small stack for core1 here.
    multicore_launch_core1_with_stack(artmatrix_core1_entry, core1_stack, stack_size);
    artmatrix_debug("launched core1\n");

    int res = multicore_fifo_pop_blocking();
    artmatrix_debug("core1 returned\n");

    if(res != 0) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("ArtMatrix Duo: failed to start Core1."));
    }
}

void artmatrix_core1_stop() {
    artmatrix_debug("signal core1\n");
    artmatrix_obj.flip = false;
    artmatrix_obj.exit_core1 = true;
    __sev();

    uint32_t fifo_code;
    do {
        while (!multicore_fifo_pop_timeout_us(1000, &fifo_code)) __sev();
        if (fifo_code == 1) {
            // TODO: LED cleanup here
        }
    } while (fifo_code != 0);

    artmatrix_debug("core1 returned\n");
}

/***** Print *****/
void artmatrix_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)self_in;
    (void)kind;
    mp_print_str(print, "artmatrix(128x128)");
}

/***** Destructor ******/
mp_obj_t artmatrix___del__(mp_obj_t self_in) {
    // artmatrix_core1_stop();
    // m_del_class(ArtMatrix, artmatrix_obj.artmatrix);
    // artmatrix_obj.artmatrix->~ArtMatrix();
    // artmatrix_obj.artmatrix = nullptr;
    // artmatrix_obj = nullptr;
    return mp_const_none;
}

/***** Constructor *****/
mp_obj_t artmatrix_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {

    // Clean up any existing instance of Art Matrix.
    if(artmatrix_obj.artmatrix) {
        artmatrix_core1_stop();
        artmatrix_obj.artmatrix->~ArtMatrix();
        artmatrix_obj.artmatrix = nullptr;
    }

    //artmatrix_obj = mp_obj_malloc_with_finaliser(_artmatrix_obj_t, &artmatrix_type);
    artmatrix_core1_reset();
    artmatrix_obj.base.type = &artmatrix_type;
    artmatrix_obj.artmatrix = new(artmatrix_buffer) ArtMatrix();
    artmatrix_obj.flip = false;
    artmatrix_obj.blocking = true;
    artmatrix_obj.hold = false;
    artmatrix_obj.held_calls = 0u;
    artmatrix_obj.blend = 0u;

    artmatrix_core1_start();

    return MP_OBJ_FROM_PTR(&artmatrix_obj);
}

mp_obj_t artmatrix_is_busy(mp_obj_t self_in) {
    (void)self_in;
    return mp_obj_new_bool(artmatrix_obj.flip);
}

mp_obj_t artmatrix_set_blocking(mp_obj_t self_in, mp_obj_t blocking_in) {
    (void)self_in;
    artmatrix_obj.blocking = mp_obj_is_true(blocking_in);
    return mp_const_none;
}

// display.set_hold(True) makes update() a no-op, so code that does not own the
// frame cannot commit one. The application wraps each plugin's draw in it.
// Returns how many update() calls were swallowed, so releasing the hold can
// report a plugin that tried rather than leaving its author to wonder.
mp_obj_t artmatrix_set_hold(mp_obj_t self_in, mp_obj_t hold_in) {
    (void)self_in;

    const uint32_t held = artmatrix_obj.held_calls;
    artmatrix_obj.hold = mp_obj_is_true(hold_in);
    artmatrix_obj.held_calls = 0u;

    return mp_obj_new_int_from_uint(held);
}

// Sets the target only. Core1 owns the gamma table, and ramps toward this as
// fast as the LED supply will allow.
mp_obj_t artmatrix_set_brightness(mp_obj_t self_in, mp_obj_t value_in) {
    (void)self_in;
    artmatrix_obj.artmatrix->set_brightness((float)mp_obj_get_float(value_in));
    return mp_const_none;
}

// (vled_open, vled_mean, vled_min, cur, cur_peak, cur_idle, calibrating,
//  headroom_v, headroom_i, ceiling, allowed, applied, gain_i, gain_v)
// Counts, not volts/amps: VLED 1.61mV per count, CUR 8.46mA per count. The
// three light values are 0.0 - 1.0; the gains are counts per unit light on an
// all-white screen, which is what calibration measured.
mp_obj_t artmatrix_power_stats(mp_obj_t self_in) {
    (void)self_in;

    uint32_t counts[7];
    float headroom[7];
    artmatrix_obj.artmatrix->get_power_stats(counts, headroom);

    mp_obj_t out[14];
    for(uint i = 0u; i < 7u; i++) out[i] = mp_obj_new_int_from_uint(counts[i]);
    for(uint i = 0u; i < 7u; i++) out[7 + i] = mp_obj_new_float(headroom[i]);

    return mp_obj_new_tuple(14, out);
}

// display.force_on() to drive past the measured brightness ceiling,
// force_on(False) to hand control back. Does not override the brownout trip or
// the VLED gate, and the trip clears it.
mp_obj_t artmatrix_force_on(size_t n_args, const mp_obj_t *args) {
    const bool force = n_args < 2 || mp_obj_is_true(args[1]);
    artmatrix_obj.artmatrix->set_force_on(force);
    return mp_const_none;
}

// display.calibrate() to discard the stored measurement and light the
// calibration grid again. Otherwise a calibration survives soft reset, so running a
// script does not flash the display every time.
mp_obj_t artmatrix_calibrate(mp_obj_t self_in) {
    (void)self_in;
    artmatrix_obj.artmatrix->recalibrate();
    return mp_const_none;
}

mp_obj_t artmatrix_calibrated(mp_obj_t self_in) {
    (void)self_in;
    return mp_obj_new_bool(!artmatrix_obj.artmatrix->is_calibrating());
}

// Total panel draw allowed, in amps. Clamped to 3.0A: the load switch limits at
// 3.3A whatever the supply can deliver.
mp_obj_t artmatrix_set_current_limit(mp_obj_t self_in, mp_obj_t amps_in) {
    (void)self_in;
    artmatrix_obj.artmatrix->set_current_limit((float)mp_obj_get_float(amps_in));
    return mp_const_none;
}

// The SENS header. Core1 owns the ADC, so this is the latest 4ms average.
mp_obj_t artmatrix_sensor(mp_obj_t self_in) {
    (void)self_in;
    return mp_obj_new_int(artmatrix_obj.artmatrix->get_sensor());
}

// display.update(blend=0). blend mixes surface 0 into surface 1, so a plugin
// transition is a core1 pass over the frame it already copies.
mp_obj_t artmatrix_update(size_t n_args, const mp_obj_t *args) {
    // Whoever owns the frame is mid-way through it; this call is not theirs.
    if(artmatrix_obj.hold) {
        artmatrix_obj.held_calls++;
        return mp_const_none;
    }

    mp_int_t blend = 0;
    if(n_args >= 2) {
        blend = mp_obj_get_int(args[1]);
        if(blend < 0 || blend > 255) {
            mp_raise_ValueError(MP_ERROR_TEXT("blend must be 0 to 255"));
        }
    }

    artmatrix_debug("wait for core1 flip\n");
    while(artmatrix_obj.flip) {
        mp_event_handle_nowait();
    };
    artmatrix_debug("trigger core1 flip\n");
    // Ordered before the flip, so core1 can't pick up the previous frame's mix.
    artmatrix_obj.blend = (uint8_t)blend;
    artmatrix_obj.flip = true;
    artmatrix_debug("sync to core1\n");
    __sev();

    // Block on the flip, since this is the front -> back buffer copy
    // We will almost always immediately clear the screen after doing this,
    // causing unsightly top-bottom tearing as the clear chases the flip.
    while(artmatrix_obj.blocking && artmatrix_obj.flip) {
        mp_event_handle_nowait();
    };

    return mp_const_none;
}

// display.surface(n) -> memoryview of drawing surface n, to wrap in an image().
// Surface 0 is the same buffer as memoryview(display).
mp_obj_t artmatrix_surface(mp_obj_t self_in, mp_obj_t index_in) {
    (void)self_in;

    mp_int_t index = mp_obj_get_int(index_in);
    if(index < 0 || index > 1) {
        mp_raise_ValueError(MP_ERROR_TEXT("surface index must be 0 or 1"));
    }

    // The RW flag matters: without it memoryview_get_buffer refuses a write
    // request, and wrapping this in an image() asks for a writable buffer.
    return mp_obj_new_memoryview('B' | MP_OBJ_ARRAY_TYPECODE_FLAG_RW,
                                 ARTMATRIX_WIDTH * ARTMATRIX_HEIGHT * 4,
                                 artmatrix_obj.artmatrix->get_surface((uint32_t)index));
}

// Support for `memoryview(display)`
mp_int_t artmatrix_get_framebuffer(mp_obj_t self_in, mp_buffer_info_t *bufinfo, mp_uint_t flags) {
    (void)self_in;
    (void)flags;
    bufinfo->buf = artmatrix_obj.artmatrix->get_framebuffer();
    bufinfo->len = 128 * 128 * 4;
    bufinfo->typecode = 'B';
    return 0;
}

static void artmatrix_startup_delay(void) {
    busy_wait_ms(ARTMATRIX_STARTUP_DELAY_MS);
}
PICO_RUNTIME_INIT_FUNC_RUNTIME(artmatrix_startup_delay, "00800");

static void __attribute__((constructor)) artmatrix_startup(void) {
    gpio_init(PIMORONI_ARTMATRIX_SW_A);
    gpio_pull_up(PIMORONI_ARTMATRIX_SW_A);
    gpio_set_dir(PIMORONI_ARTMATRIX_SW_A, GPIO_IN);

    gpio_init(PIMORONI_ARTMATRIX_SW_B);
    gpio_pull_up(PIMORONI_ARTMATRIX_SW_B);
    gpio_set_dir(PIMORONI_ARTMATRIX_SW_B, GPIO_IN);

    gpio_init(PIMORONI_ARTMATRIX_SW_C);
    gpio_pull_up(PIMORONI_ARTMATRIX_SW_C);
    gpio_set_dir(PIMORONI_ARTMATRIX_SW_C, GPIO_IN);
}


}