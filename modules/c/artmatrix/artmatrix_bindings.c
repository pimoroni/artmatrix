#include "artmatrix_bindings.h"


/***** Methods *****/
MP_DEFINE_CONST_FUN_OBJ_1(artmatrix___del___obj, artmatrix___del__);
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(artmatrix_update_obj, 1, 2, artmatrix_update);
MP_DEFINE_CONST_FUN_OBJ_2(artmatrix_surface_obj, artmatrix_surface);
MP_DEFINE_CONST_FUN_OBJ_1(artmatrix_scratch_obj, artmatrix_scratch);
MP_DEFINE_CONST_FUN_OBJ_1(artmatrix_is_busy_obj, artmatrix_is_busy);
MP_DEFINE_CONST_FUN_OBJ_2(artmatrix_set_blocking_obj, artmatrix_set_blocking);
MP_DEFINE_CONST_FUN_OBJ_2(artmatrix_set_hold_obj, artmatrix_set_hold);
MP_DEFINE_CONST_FUN_OBJ_2(artmatrix_set_brightness_obj, artmatrix_set_brightness);
MP_DEFINE_CONST_FUN_OBJ_2(artmatrix_set_ambient_obj, artmatrix_set_ambient);
MP_DEFINE_CONST_FUN_OBJ_1(artmatrix_sensor_obj, artmatrix_sensor);
MP_DEFINE_CONST_FUN_OBJ_2(artmatrix_hear_obj, artmatrix_hear);
MP_DEFINE_CONST_FUN_OBJ_1(artmatrix_power_stats_obj, artmatrix_power_stats);
MP_DEFINE_CONST_FUN_OBJ_1(artmatrix_calibrate_obj, artmatrix_calibrate);
MP_DEFINE_CONST_FUN_OBJ_1(artmatrix_calibrated_obj, artmatrix_calibrated);
MP_DEFINE_CONST_FUN_OBJ_2(artmatrix_set_current_limit_obj, artmatrix_set_current_limit);
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(artmatrix_force_on_obj, 1, 2, artmatrix_force_on);


/***** Binding of Methods *****/
static const mp_rom_map_elem_t artmatrix_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&artmatrix___del___obj) },
    { MP_ROM_QSTR(MP_QSTR_update), MP_ROM_PTR(&artmatrix_update_obj) },
    { MP_ROM_QSTR(MP_QSTR_surface), MP_ROM_PTR(&artmatrix_surface_obj) },
    { MP_ROM_QSTR(MP_QSTR_scratch), MP_ROM_PTR(&artmatrix_scratch_obj) },
    { MP_ROM_QSTR(MP_QSTR_is_busy), MP_ROM_PTR(&artmatrix_is_busy_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_blocking), MP_ROM_PTR(&artmatrix_set_blocking_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_hold), MP_ROM_PTR(&artmatrix_set_hold_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_brightness), MP_ROM_PTR(&artmatrix_set_brightness_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_ambient), MP_ROM_PTR(&artmatrix_set_ambient_obj) },
    { MP_ROM_QSTR(MP_QSTR_sensor), MP_ROM_PTR(&artmatrix_sensor_obj) },
    { MP_ROM_QSTR(MP_QSTR_hear), MP_ROM_PTR(&artmatrix_hear_obj) },
    { MP_ROM_QSTR(MP_QSTR_power_stats), MP_ROM_PTR(&artmatrix_power_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_calibrate), MP_ROM_PTR(&artmatrix_calibrate_obj) },
    { MP_ROM_QSTR(MP_QSTR_calibrated), MP_ROM_PTR(&artmatrix_calibrated_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_current_limit), MP_ROM_PTR(&artmatrix_set_current_limit_obj) },
    { MP_ROM_QSTR(MP_QSTR_force_on), MP_ROM_PTR(&artmatrix_force_on_obj) },
};

static MP_DEFINE_CONST_DICT(artmatrix_locals_dict, artmatrix_locals_dict_table);

/***** Class Definition *****/
MP_DEFINE_CONST_OBJ_TYPE(
    artmatrix_type,
    MP_QSTR_artmatrix,
    MP_TYPE_FLAG_NONE,
    make_new, artmatrix_make_new,
    buffer, artmatrix_get_framebuffer,
    print, artmatrix_print,
    locals_dict, (mp_obj_dict_t*)&artmatrix_locals_dict
);

/***** Globals Table *****/

static const mp_map_elem_t artmatrix_globals_table[] = {
    { MP_OBJ_NEW_QSTR(MP_QSTR___name__), MP_OBJ_NEW_QSTR(MP_QSTR_artmatrix) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_artmatrix), (mp_obj_t)&artmatrix_type },

    { MP_ROM_QSTR(MP_QSTR_WIDTH), MP_ROM_INT(128) },
    { MP_ROM_QSTR(MP_QSTR_HEIGHT), MP_ROM_INT(128) },
    { MP_ROM_QSTR(MP_QSTR_SCRATCH_WIDTH), MP_ROM_INT(192) },
    { MP_ROM_QSTR(MP_QSTR_SCRATCH_HEIGHT), MP_ROM_INT(128) }
};
static MP_DEFINE_CONST_DICT(mp_module_artmatrix_globals, artmatrix_globals_table);

/***** Module Definition *****/
const mp_obj_module_t artmatrix_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t*)&mp_module_artmatrix_globals,
};
MP_REGISTER_MODULE(MP_QSTR__artmatrix, artmatrix_user_cmodule);