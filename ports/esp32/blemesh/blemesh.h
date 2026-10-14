// ESP32 BLE Mesh coexistence. No second NimBLE host or controller.
#ifndef MICROPY_INCLUDED_ESP32_BLEMESH_H
#define MICROPY_INCLUDED_ESP32_BLEMESH_H

#include "sdkconfig.h"
#if CONFIG_BLE_MESH
#include "host/ble_gap.h"

// Four public instances; the fifth controller instance belongs to Mesh.
#define ESP32_BLE_PUBLIC_ADV_INSTANCES (4)
#define ESP32_BLE_MESH_ADV_INSTANCE (4)

void esp32_blemesh_radio_init(void);
void esp32_blemesh_vm_init(void);
void esp32_blemesh_radio_deinit(void);
void esp32_blemesh_radio_reset(void);
void esp32_blemesh_adapter_reset(void);
int esp32_blemesh_adv_open(void);
uintptr_t esp32_blemesh_adv_begin(void);
void esp32_blemesh_adv_end(int err);
void esp32_blemesh_adv_complete(uintptr_t token, int reason);
void esp32_blemesh_adv_abort(int reason);
int esp32_blemesh_adv_wait(void);
int esp32_blemesh_adv_close(void);
int esp32_blemesh_adv_error(void);
void esp32_blemesh_adv_failed(int reason);
void esp32_blemesh_radio_sync(void);
int esp32_blemesh_shutdown(void);

int esp32_blemesh_app_scan(uint8_t own_addr_type, uint16_t duration, uint16_t period,
    uint8_t filter_duplicates, uint8_t filter_policy, uint8_t limited,
    const struct ble_gap_ext_disc_params *uncoded,
    const struct ble_gap_ext_disc_params *coded, ble_gap_event_fn *cb, void *arg);
int esp32_blemesh_mesh_scan(uint8_t own_addr_type, uint16_t duration, uint16_t period,
    uint8_t filter_duplicates, uint8_t filter_policy, uint8_t limited,
    const struct ble_gap_ext_disc_params *uncoded,
    const struct ble_gap_ext_disc_params *coded, ble_gap_event_fn *cb, void *arg);
int esp32_blemesh_app_scan_cancel(void);
int esp32_blemesh_mesh_scan_cancel(void);
int esp32_blemesh_app_scan_active(void);
int esp32_blemesh_connect_pause(void);
void esp32_blemesh_connect_resume(void);
int esp32_blemesh_radio_error(void);
#endif
#endif
