// Optional, fixed-memory diagnostics. No Python objects are used by the probes.
#ifndef MICROPY_INCLUDED_ESP32_BLUETOOTH_BLE5_DIAGNOSTICS_H
#define MICROPY_INCLUDED_ESP32_BLUETOOTH_BLE5_DIAGNOSTICS_H

#ifndef MICROPY_ESP32_BLE5_DIAGNOSTICS
#define MICROPY_ESP32_BLE5_DIAGNOSTICS (0)
#endif

#if MICROPY_ESP32_BLE5_DIAGNOSTICS
#include <stdbool.h>
#include <stdint.h>

#define ESP32_BLE5_DIAG_POOLS (5)
#define ESP32_BLE5_DIAG_TRACE (32)
#define ESP32_BLE5_DIAG_SCAN (0)
#define ESP32_BLE5_DIAG_PERIODIC (1)
#define ESP32_BLE5_DIAG_SLOW_US (5000)

enum {
    ESP32_BLE5_DIAG_POOL_FAILURE = 1,
    ESP32_BLE5_DIAG_SYNC,
    ESP32_BLE5_DIAG_SCAN_STATUS,
    ESP32_BLE5_DIAG_PERIODIC_STATUS,
    ESP32_BLE5_DIAG_SYNC_LOST,
    ESP32_BLE5_DIAG_GIL_SLOW,
    ESP32_BLE5_DIAG_CALLBACK_SLOW,
};

typedef struct {
    bool detected;
    uint16_t blocks, last_free, sdk_min_free;
    uint32_t initializations, allocations, failures;
} esp32_ble5_diag_pool_t;

typedef struct {
    uint32_t reports, bytes, status[4], timed;
    uint32_t gil_wait_max_us, callback_max_us;
    uint64_t gil_wait_us, callback_us;
} esp32_ble5_diag_rx_t;

typedef struct {
    uint32_t time_us, value;
    uint16_t event, arg, handle;
} esp32_ble5_diag_trace_t;

typedef struct {
    bool enabled;
    uint32_t epoch, now_us, trace_written;
    esp32_ble5_diag_pool_t pools[ESP32_BLE5_DIAG_POOLS];
    esp32_ble5_diag_rx_t rx[2];
    esp32_ble5_diag_trace_t trace[ESP32_BLE5_DIAG_TRACE];
} esp32_ble5_diag_snapshot_t;

typedef struct {
    bool active;
    uint32_t epoch, start_us, acquired_us;
    unsigned int kind;
} esp32_ble5_diag_token_t;

struct ble_gap_event;
void esp32_ble5_diag_event(const struct ble_gap_event *event);
void esp32_ble5_diag_begin(esp32_ble5_diag_token_t *token, unsigned int kind);
void esp32_ble5_diag_acquired(esp32_ble5_diag_token_t *token);
void esp32_ble5_diag_end(const esp32_ble5_diag_token_t *token);
// enable: -1 = unchanged; clear resets counters, not SDK pool watermarks.
void esp32_ble5_diag_snapshot(int enable, bool clear, esp32_ble5_diag_snapshot_t *snapshot);
#endif
#endif
