#include <cstdio>
#include "driver/artmatrix.hpp"
#include "pico/multicore.h"

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
} _artmatrix_obj_t;

_artmatrix_obj_t *artmatrix_obj;


void __isr dma_complete() {
    if(artmatrix_obj) artmatrix_obj->artmatrix->dma_complete();
}

#define stack_size 8192u
static uint32_t core1_stack[stack_size] = {0};

void artmatrix_core1_entry() {
    // The multicore lockout uses the FIFO, so we use just use sev and volatile flags to signal this core
    multicore_lockout_victim_init();

    artmatrix_obj->artmatrix->start(dma_complete);

    multicore_fifo_push_blocking(0); // TODO: handle issues here?

    // artmatrix is now running the display using interrupts on this core.
    // We can also drive the backlight if requested.
    while (true) {
        if (artmatrix_obj->exit_core1) {
            break;
        }
        if (artmatrix_obj->flip) {
            artmatrix_obj->artmatrix->update();
            artmatrix_obj->flip = false;
        }
    }

    artmatrix_obj->artmatrix->stop(dma_complete);

    multicore_fifo_push_blocking(0);
}

void artmatrix_core1_start() {
    artmatrix_debug("launch core1\n");
    multicore_reset_core1();
    artmatrix_obj->exit_core1 = false;
    artmatrix_obj->flip = false;

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
    artmatrix_obj->flip = false;
    artmatrix_obj->exit_core1 = true;
    __sev();

    int fifo_code;
    do {
        fifo_code = multicore_fifo_pop_blocking();
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
    (void)self_in;
    artmatrix_core1_stop();
    m_del_class(ArtMatrix, artmatrix_obj->artmatrix);
    artmatrix_obj->artmatrix = nullptr;
    artmatrix_obj = nullptr;
    return mp_const_none;
}

/***** Constructor *****/
mp_obj_t artmatrix_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    artmatrix_obj = mp_obj_malloc_with_finaliser(_artmatrix_obj_t, &artmatrix_type);
    artmatrix_obj->artmatrix = m_new_class(ArtMatrix);
    artmatrix_obj->flip = false;
    artmatrix_obj->blocking = true;

    artmatrix_core1_start();

    return MP_OBJ_FROM_PTR(artmatrix_obj);
}

mp_obj_t artmatrix_is_busy(mp_obj_t self_in) {
    (void)self_in;
    return mp_obj_new_bool(artmatrix_obj->flip);
}

mp_obj_t artmatrix_set_blocking(mp_obj_t self_in, mp_obj_t blocking_in) {
    (void)self_in;
    artmatrix_obj->blocking = mp_obj_is_true(blocking_in);
    return mp_const_none;
}

mp_obj_t artmatrix_update(mp_obj_t self_in) {
    (void)self_in;
    //artmatrix_obj->artmatrix->update();

    artmatrix_debug("wait for core1 flip\n");
    while(artmatrix_obj->flip) {
        mp_event_handle_nowait();
    };
    artmatrix_debug("trigger core1 flip\n");
    artmatrix_obj->flip = true;
    artmatrix_debug("sync to core1\n");
    __sev();

    // Block on the flip, since this is the front -> back buffer copy
    // We will almost always immediately clear the screen after doing this,
    // causing unsightly top-bottom tearing as the clear chases the flip.
    while(artmatrix_obj->blocking && artmatrix_obj->flip) {
        mp_event_handle_nowait();
    };

    return mp_const_none;
}

// Support for `memoryview(display)`
mp_int_t artmatrix_get_framebuffer(mp_obj_t self_in, mp_buffer_info_t *bufinfo, mp_uint_t flags) {
    (void)self_in;
    (void)flags;
    bufinfo->buf = artmatrix_obj->artmatrix->get_framebuffer();
    bufinfo->len = 128 * 128 * 4;
    bufinfo->typecode = 'B';
    return 0;
}

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