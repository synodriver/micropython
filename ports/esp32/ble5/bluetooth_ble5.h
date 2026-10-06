// ESP32-only BLE 5 additions to the existing bluetooth.BLE singleton.
#ifndef MICROPY_INCLUDED_ESP32_BLUETOOTH_BLE5_H
#define MICROPY_INCLUDED_ESP32_BLUETOOTH_BLE5_H

#include "extmod/modbluetooth.h"
#include "soc/soc_caps.h"

#if MICROPY_PY_BLUETOOTH && SOC_BLE_50_SUPPORTED && CONFIG_BT_NIMBLE_50_FEATURE_SUPPORT && !CONFIG_IDF_TARGET_ESP32P4
#define MICROPY_ESP32_BLE5 (1)
#include "esp_idf_version.h"
#include "host/ble_hs.h"
#include "nimble/ble.h"
#define MICROPY_ESP32_BLE5_EXT_ADV MYNEWT_VAL(BLE_EXT_ADV)
#define MICROPY_ESP32_BLE5_PERIODIC_ADV MYNEWT_VAL(BLE_PERIODIC_ADV)

// Keep existing IRQ numbers and tuple layouts unchanged.
#define ESP32_BLE5_IRQ_PHY_UPDATE (40)
#define ESP32_BLE5_IRQ_SCAN_RESULT (41)
#define ESP32_BLE5_IRQ_ADV_COMPLETE (42)
#define ESP32_BLE5_IRQ_PERIODIC_SYNC (43)
#define ESP32_BLE5_IRQ_PERIODIC_REPORT (44)
#define ESP32_BLE5_IRQ_PERIODIC_SYNC_LOST (45)

void esp32_ble5_on_phy_update(const struct ble_gap_event *event);
void esp32_ble5_on_event(const struct ble_gap_event *event);
void esp32_ble5_reset(void);
bool esp32_ble5_is_host_task(void);
int esp32_ble5_phy(uint16_t conn_handle, uint8_t *tx_phy, uint8_t *rx_phy);
int esp32_ble5_set_phy(int conn_handle, int tx_mask, int rx_mask, int options);
uint8_t esp32_ble5_phy_mask(void);

#if MICROPY_ESP32_BLE5_EXT_ADV
typedef struct {
    int instance;
    int interval_us;
    int primary_phy;
    int secondary_phy;
    int sid;
    int timeout_ms;
    bool connectable;
    bool scannable;
    const uint8_t *adv_data;
    size_t adv_data_len;
    const uint8_t *resp_data;
    size_t resp_data_len;
} esp32_ble5_adv_params_t;

int esp32_ble5_advertise(const esp32_ble5_adv_params_t *params);
int esp32_ble5_advertise_stop(int instance, bool remove);
int esp32_ble5_scan(int duration_ms, int interval_us, int window_us, bool active, int phys, bool extended);
int esp32_ble5_connect(uint8_t addr_type, const uint8_t *addr, int duration_ms, int min_interval_us, int max_interval_us, int phys);
#endif
#if MICROPY_ESP32_BLE5_PERIODIC_ADV
void esp32_ble5_periodic_init(void);
void esp32_ble5_periodic_disable(void);
void esp32_ble5_periodic_deinit(void);
int esp32_ble5_periodic_advertise(int instance, int interval_us, const uint8_t *data, size_t len);
int esp32_ble5_periodic_sync(uint8_t addr_type, const uint8_t *addr, int sid, int skip, int timeout_ms);
int esp32_ble5_periodic_sync_cancel(void);
int esp32_ble5_periodic_sync_stop(int handle);
#endif
#else
#define MICROPY_ESP32_BLE5 (0)
#define MICROPY_ESP32_BLE5_EXT_ADV (0)
#define MICROPY_ESP32_BLE5_PERIODIC_ADV (0)
#endif
#endif
