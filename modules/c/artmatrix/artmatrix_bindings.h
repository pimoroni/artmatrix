// Include MicroPython API.
#include "py/runtime.h"
#include "py/objstr.h"

/***** Extern of Class Definition *****/
extern const mp_obj_type_t artmatrix_type;

/***** Extern of Class Methods *****/
extern void artmatrix_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind);
extern mp_obj_t artmatrix_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args);
extern mp_obj_t artmatrix___del__(mp_obj_t self_in);
extern mp_obj_t artmatrix_update(mp_obj_t self_in);
extern mp_obj_t artmatrix_is_busy(mp_obj_t self_in);
extern mp_obj_t artmatrix_set_blocking(mp_obj_t self_in, mp_obj_t blocking_in);
extern mp_obj_t artmatrix_set_brightness(mp_obj_t self_in, mp_obj_t value_in);
extern mp_obj_t artmatrix_sensor(mp_obj_t self_in);

extern mp_int_t artmatrix_get_framebuffer(mp_obj_t self_in, mp_buffer_info_t *bufinfo, mp_uint_t flags);