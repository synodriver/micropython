#ifndef MICROPY_INCLUDED_ESP32_MODESP32_H
#define MICROPY_INCLUDED_ESP32_MODESP32_H

#include "driver/rmt_tx.h"

extern bool esp32_rmt_bitstream_enabled;

extern const mp_obj_type_t esp32_nvs_type;
extern const mp_obj_type_t esp32_partition_type;
extern const mp_obj_type_t esp32_rmt_type;
extern const mp_obj_type_t esp32_ulp_type;
extern const mp_obj_type_t esp32_ldo_type;

#if MICROPY_PY_ESP32_PCNT
extern const mp_obj_type_t esp32_pcnt_type;

void esp32_ldo_deinit_all(void);
void esp32_pcnt_deinit_all(void);
#endif

#endif // MICROPY_INCLUDED_ESP32_MODESP32_H
