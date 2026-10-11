/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 MicroPython contributors
 */

#include <stdio.h>
#include <string.h>
#include "py/runtime.h"
#include "py/mphal.h"
#include "py/mperrno.h"
#include "py/misc.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#if !MICROPY_ENABLE_FINALISER
#error "esp32.PMLock requires MICROPY_ENABLE_FINALISER"
#endif

typedef struct _esp32_pm_lock_obj_t {
    mp_obj_base_t base;
    esp_pm_lock_handle_t handle;
    size_t count;
    // IDF retains the name pointer. Keep a copy in the finalised allocation,
    // so GC and soft-reset finaliser ordering cannot invalidate it first.
    char name[];
} esp32_pm_lock_obj_t;

static void esp32_pm_lock_check_context(void) {
    // All Python operations retain the GIL (when threads are enabled). IDF
    // create/delete must not run in an ISR; the Python API excludes ISR use.
    if (xPortInIsrContext()) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("PMLock cannot be used in an ISR"));
    }
}

static esp32_pm_lock_obj_t *esp32_pm_lock_get(mp_obj_t self_in) {
    esp32_pm_lock_check_context();
    esp32_pm_lock_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->handle == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("PMLock is deinitialized"));
    }
    return self;
}

static mp_obj_t esp32_pm_lock_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    esp32_pm_lock_check_context();
    enum { ARG_kind, ARG_name };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_kind, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_name, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    mp_int_t kind = args[ARG_kind].u_int;
    // Invalid types can abort inside IDF. Validate before casting the enum.
    if (kind != ESP_PM_CPU_FREQ_MAX && kind != ESP_PM_APB_FREQ_MAX && kind != ESP_PM_NO_LIGHT_SLEEP) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid PM lock type"));
    }
    size_t name_len = 0;
    const char *name = NULL;
    if (args[ARG_name].u_obj != mp_const_none) {
        if (!mp_obj_is_str(args[ARG_name].u_obj)) {
            mp_raise_TypeError(MP_ERROR_TEXT("name must be str or None"));
        }
        name = mp_obj_str_get_data(args[ARG_name].u_obj, &name_len);
        if (memchr(name, '\0', name_len) != NULL) {
            mp_raise_ValueError(MP_ERROR_TEXT("name contains NUL"));
        }
        // IDF prints at most 15 bytes. Give it a complete UTF-8 prefix so a
        // multibyte character cannot be split and make pm_dump_locks fail.
        if (name_len > 15) {
            name_len = 15;
            while (name_len > 0 && UTF8_IS_CONT(name[name_len])) {
                --name_len;
            }
        }
    }
    esp32_pm_lock_obj_t *self = mp_obj_malloc_var_with_finaliser(esp32_pm_lock_obj_t, name, char, name_len + 1, type);
    // Initialise before the SDK allocation, so construction failure is safe
    // to finalise. No Python allocations follow successful SDK creation.
    self->handle = NULL;
    self->count = 0;
    if (name != NULL) {
        memcpy(self->name, name, name_len);
    }
    self->name[name_len] = '\0';
    check_esp_err(esp_pm_lock_create((esp_pm_lock_type_t)kind, 0, name == NULL ? NULL : self->name, &self->handle));
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t esp32_pm_lock_acquire(mp_obj_t self_in) {
    esp32_pm_lock_obj_t *self = esp32_pm_lock_get(self_in);
    if (self->count == SIZE_MAX) {
        mp_raise_msg(&mp_type_OverflowError, MP_ERROR_TEXT("PM lock count overflow"));
    }
    check_esp_err(esp_pm_lock_acquire(self->handle));
    ++self->count;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(esp32_pm_lock_acquire_obj, esp32_pm_lock_acquire);

static mp_obj_t esp32_pm_lock_release(mp_obj_t self_in) {
    esp32_pm_lock_obj_t *self = esp32_pm_lock_get(self_in);
    if (self->count == 0) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("PMLock is not acquired"));
    }
    check_esp_err(esp_pm_lock_release(self->handle));
    --self->count;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(esp32_pm_lock_release_obj, esp32_pm_lock_release);

static mp_obj_t esp32_pm_lock_deinit(mp_obj_t self_in) {
    esp32_pm_lock_check_context();
    esp32_pm_lock_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->count != 0) {
        mp_raise_OSError(MP_EBUSY);
    }
    if (self->handle != NULL) {
        check_esp_err(esp_pm_lock_delete(self->handle));
        self->handle = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(esp32_pm_lock_deinit_obj, esp32_pm_lock_deinit);

static mp_obj_t esp32_pm_lock_finalise(mp_obj_t self_in) {
    esp32_pm_lock_check_context();
    esp32_pm_lock_obj_t *self = MP_OBJ_TO_PTR(self_in);
    // No global roots: unreachable objects can be collected. On soft reset,
    // main.c stops Python threads and calls gc_sweep_all() before heap reuse.
    while (self->count != 0) {
        esp32_pm_lock_release(self_in);
    }
    return esp32_pm_lock_deinit(self_in);
}
static MP_DEFINE_CONST_FUN_OBJ_1(esp32_pm_lock_finalise_obj, esp32_pm_lock_finalise);

static mp_obj_t esp32_pm_lock_enter(mp_obj_t self_in) {
    esp32_pm_lock_acquire(self_in);
    return self_in;
}
static MP_DEFINE_CONST_FUN_OBJ_1(esp32_pm_lock_enter_obj, esp32_pm_lock_enter);

static mp_obj_t esp32_pm_lock_exit(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return esp32_pm_lock_release(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esp32_pm_lock_exit_obj, 4, 4, esp32_pm_lock_exit);

static const mp_rom_map_elem_t esp32_pm_lock_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_acquire), MP_ROM_PTR(&esp32_pm_lock_acquire_obj) },
    { MP_ROM_QSTR(MP_QSTR_release), MP_ROM_PTR(&esp32_pm_lock_release_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&esp32_pm_lock_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&esp32_pm_lock_enter_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&esp32_pm_lock_exit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&esp32_pm_lock_finalise_obj) },
};
static MP_DEFINE_CONST_DICT(esp32_pm_lock_locals_dict, esp32_pm_lock_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    esp32_pm_lock_type,
    MP_QSTR_PMLock,
    MP_TYPE_FLAG_NONE,
    make_new, esp32_pm_lock_make_new,
    locals_dict, &esp32_pm_lock_locals_dict
    );
