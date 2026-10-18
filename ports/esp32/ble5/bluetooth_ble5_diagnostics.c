// This file is compiled only for diagnostic firmware. Never print or allocate
// from the receive/allocator probes: either can run while Python holds the GIL.
#include <string.h>
#include "bluetooth_ble5_diagnostics.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "host/ble_gap.h"
#include "os/os.h"

static DRAM_ATTR portMUX_TYPE diag_mux = portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR esp32_ble5_diag_snapshot_t diag;
static DRAM_ATTR struct os_mempool *diag_pools[ESP32_BLE5_DIAG_POOLS];

static uint32_t IRAM_ATTR diag_time(void) {
    return (uint32_t)esp_timer_get_time();
}

// Caller holds diag_mux. Times and counters wrap modulo 2**32.
static void IRAM_ATTR diag_trace(unsigned int event, unsigned int arg, unsigned int handle, uint32_t value) {
    esp32_ble5_diag_trace_t *entry = &diag.trace[diag.trace_written++ % ESP32_BLE5_DIAG_TRACE];
    entry->time_us = diag_time();
    entry->event = (uint16_t)event;
    entry->arg = (uint16_t)arg;
    entry->handle = (uint16_t)handle;
    entry->value = value;
}

os_error_t __real_os_mempool_init(struct os_mempool *, uint16_t, uint32_t, void *, const char *);
void *__real_os_memblock_get(struct os_mempool *);

os_error_t __wrap_os_mempool_init(struct os_mempool *mp, uint16_t blocks, uint32_t size, void *buffer, const char *name) {
    os_error_t result = __real_os_mempool_init(mp, blocks, size, buffer, name);
    if (result != 0) {
        return result;
    }
    static const char *const names[ESP32_BLE5_DIAG_POOLS] = {
        "lec_hevt_lomp", "lec_hevt_himp", "transport_pool_evt_lo",
        "transport_pool_evt", "ble_hs_hci_ev_pool",
    };
    int match = -1;
    for (unsigned int i = 0; name != NULL && i < ESP32_BLE5_DIAG_POOLS; ++i) {
        if (strcmp(name, names[i]) == 0) {
            match = i;
            break;
        }
    }
    portENTER_CRITICAL_SAFE(&diag_mux);
    for (unsigned int i = 0; i < ESP32_BLE5_DIAG_POOLS; ++i) {
        // A new pool may reuse the address of a deinitialized pool.
        if (diag_pools[i] == mp) {
            diag_pools[i] = NULL;
            // Do not report a stale pool as detected if this address is later
            // initialized with an untracked name.
            diag.pools[i].detected = false;
            diag.pools[i].blocks = 0;
            diag.pools[i].last_free = 0;
            diag.pools[i].sdk_min_free = 0;
        }
    }
    if (match >= 0) {
        diag_pools[match] = mp;
        esp32_ble5_diag_pool_t *pool = &diag.pools[match];
        pool->detected = true;
        pool->blocks = blocks;
        pool->last_free = blocks;
        pool->sdk_min_free = blocks;
        ++pool->initializations;
    }
    portEXIT_CRITICAL_SAFE(&diag_mux);
    return result;
}

void *IRAM_ATTR __wrap_os_memblock_get(struct os_mempool *mp) {
    // The SDK accepts a NULL pool and returns NULL. Preserve that behavior
    // before comparing or sampling any pool fields.
    if (mp == NULL) {
        return __real_os_memblock_get(mp);
    }
    // Never call the allocator while holding the diagnostic lock.
    void *result = __real_os_memblock_get(mp);
    portENTER_CRITICAL_SAFE(&diag_mux);
    if (diag.enabled) {
        for (unsigned int i = 0; i < ESP32_BLE5_DIAG_POOLS; ++i) {
            if (diag_pools[i] != mp) {
                continue;
            }
            esp32_ble5_diag_pool_t *pool = &diag.pools[i];
            ++pool->allocations;
            // Public SDK fields, sampled after allocation. Another core can
            // already have returned a block; these are not atomic free counts.
            pool->last_free = mp->mp_num_free;
            pool->sdk_min_free = mp->mp_min_free;
            if (result == NULL) {
                ++pool->failures;
                diag_trace(ESP32_BLE5_DIAG_POOL_FAILURE, i, 0, pool->last_free);
            }
            break;
        }
    }
    portEXIT_CRITICAL_SAFE(&diag_mux);
    return result;
}

void esp32_ble5_diag_event(const struct ble_gap_event *event) {
    portENTER_CRITICAL_SAFE(&diag_mux);
    if (diag.enabled) {
        unsigned int kind, status, length, handle;
        switch (event->type) {
            #if MYNEWT_VAL(BLE_EXT_ADV)
            case BLE_GAP_EVENT_EXT_DISC:
                kind = ESP32_BLE5_DIAG_SCAN;
                status = event->ext_disc.data_status;
                length = event->ext_disc.length_data;
                handle = event->ext_disc.sid;
                break;
            #endif
            #if MYNEWT_VAL(BLE_PERIODIC_ADV)
            case BLE_GAP_EVENT_PERIODIC_REPORT:
                kind = ESP32_BLE5_DIAG_PERIODIC;
                status = event->periodic_report.data_status;
                length = event->periodic_report.data_length;
                handle = event->periodic_report.sync_handle;
                break;
            case BLE_GAP_EVENT_PERIODIC_SYNC:
                // A failed completion need not contain a valid sync handle.
                diag_trace(ESP32_BLE5_DIAG_SYNC, event->periodic_sync.status,
                    event->periodic_sync.status == 0 ? event->periodic_sync.sync_handle : UINT16_MAX, 0);
                goto done;
            case BLE_GAP_EVENT_PERIODIC_SYNC_LOST:
                diag_trace(ESP32_BLE5_DIAG_SYNC_LOST, event->periodic_sync_lost.reason,
                    event->periodic_sync_lost.sync_handle, 0);
                goto done;
            #endif
            default:
                goto done;
        }
        esp32_ble5_diag_rx_t *rx = &diag.rx[kind];
        ++rx->reports;
        rx->bytes += length;
        ++rx->status[status < 3 ? status : 3];
        if (status >= 2) {
            diag_trace(kind == ESP32_BLE5_DIAG_SCAN ? ESP32_BLE5_DIAG_SCAN_STATUS : ESP32_BLE5_DIAG_PERIODIC_STATUS,
                status, handle, length);
        }
    }
done:
    portEXIT_CRITICAL_SAFE(&diag_mux);
}

void esp32_ble5_diag_begin(esp32_ble5_diag_token_t *token, unsigned int kind) {
    portENTER_CRITICAL_SAFE(&diag_mux);
    token->active = diag.enabled && kind < 2;
    token->kind = kind;
    token->epoch = diag.epoch;
    token->start_us = diag_time();
    portEXIT_CRITICAL_SAFE(&diag_mux);
}

void esp32_ble5_diag_acquired(esp32_ble5_diag_token_t *token) {
    token->acquired_us = diag_time();
}

void esp32_ble5_diag_end(const esp32_ble5_diag_token_t *token) {
    if (!token->active) {
        return;
    }
    uint32_t end = diag_time();
    uint32_t wait = token->acquired_us - token->start_us;
    uint32_t callback = end - token->acquired_us;
    portENTER_CRITICAL_SAFE(&diag_mux);
    if (diag.enabled && diag.epoch == token->epoch) {
        esp32_ble5_diag_rx_t *rx = &diag.rx[token->kind];
        ++rx->timed;
        rx->gil_wait_us += wait;
        rx->callback_us += callback;
        if (wait > rx->gil_wait_max_us) {
            rx->gil_wait_max_us = wait;
        }
        if (callback > rx->callback_max_us) {
            rx->callback_max_us = callback;
        }
        if (wait >= ESP32_BLE5_DIAG_SLOW_US) {
            diag_trace(ESP32_BLE5_DIAG_GIL_SLOW, token->kind, 0, wait);
        }
        if (callback >= ESP32_BLE5_DIAG_SLOW_US) {
            diag_trace(ESP32_BLE5_DIAG_CALLBACK_SLOW, token->kind, 0, callback);
        }
    }
    portEXIT_CRITICAL_SAFE(&diag_mux);
}

void esp32_ble5_diag_snapshot(int enable, bool clear, esp32_ble5_diag_snapshot_t *snapshot) {
    portENTER_CRITICAL_SAFE(&diag_mux);
    if (clear || (enable >= 0 && diag.enabled != (bool)enable)) {
        ++diag.epoch;
    }
    if (clear) {
        memset(diag.rx, 0, sizeof(diag.rx));
        memset(diag.trace, 0, sizeof(diag.trace));
        diag.trace_written = 0;
        for (unsigned int i = 0; i < ESP32_BLE5_DIAG_POOLS; ++i) {
            diag.pools[i].allocations = 0;
            diag.pools[i].failures = 0;
            diag.pools[i].initializations = 0;
        }
    }
    if (enable >= 0) {
        diag.enabled = enable;
    }
    diag.now_us = diag_time();
    *snapshot = diag;
    portEXIT_CRITICAL_SAFE(&diag_mux);
}
