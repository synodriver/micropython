// Host regression harness for the actual diagnostic implementation.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "bluetooth_ble5_diagnostics.h"
#include "host/ble_gap.h"
#include "os/os.h"

int test_lock_level;
static uint64_t test_time;
static char block;
static esp32_ble5_diag_snapshot_t snapshot;

int64_t esp_timer_get_time(void) {
    return test_time;
}

os_error_t __real_os_mempool_init(struct os_mempool *mp, uint16_t blocks, uint32_t size, void *buffer, const char *name) {
    assert(test_lock_level == 0);
    (void)size;
    (void)buffer;
    (void)name;
    if (blocks == 0) {
        return 7;
    }
    mp->mp_num_blocks = (uint16_t)blocks;
    mp->mp_num_free = (uint16_t)blocks;
    mp->mp_min_free = (uint16_t)blocks;
    return 0;
}

void *__real_os_memblock_get(struct os_mempool *mp) {
    assert(test_lock_level == 0);
    if (mp == NULL) {
        return NULL;
    }
    if (!mp->mp_num_free) {
        return NULL;
    }
    --mp->mp_num_free;
    if (mp->mp_num_free < mp->mp_min_free) {
        mp->mp_min_free = mp->mp_num_free;
    }
    return &block;
}

os_error_t __wrap_os_mempool_init(struct os_mempool *, uint16_t, uint32_t, void *, const char *);
void *__wrap_os_memblock_get(struct os_mempool *);

static void read_snapshot(void) {
    esp32_ble5_diag_snapshot(-1, false, &snapshot);
    assert(test_lock_level == 0);
}

int main(void) {
    struct os_mempool pools[6] = {0};
    const char *names[] = {
        "lec_hevt_lomp", "lec_hevt_himp", "transport_pool_evt_lo",
        "transport_pool_evt", "ble_hs_hci_ev_pool", "untracked",
    };
    for (unsigned int i = 0; i < 6; ++i) {
        assert(__wrap_os_mempool_init(&pools[i], 2, 260, NULL, names[i]) == 0);
    }
    assert(__wrap_os_memblock_get(&pools[0]) == &block);
    read_snapshot();
    assert(!snapshot.enabled);
    assert(snapshot.pools[0].detected && snapshot.pools[0].blocks == 2);
    assert(snapshot.pools[0].allocations == 0);
    puts("PASS disabled probes preserve allocator results and detect initialization");

    assert(__wrap_os_mempool_init(&pools[0], 2, 260, NULL, names[0]) == 0);
    esp32_ble5_diag_snapshot(1, true, &snapshot);
    assert(__wrap_os_memblock_get(NULL) == NULL);
    read_snapshot();
    assert(snapshot.pools[0].allocations == 0 && snapshot.pools[0].failures == 0);
    for (unsigned int i = 0; i < 6; ++i) {
        assert(__wrap_os_memblock_get(&pools[i]) == &block);
        assert(__wrap_os_memblock_get(&pools[i]) == &block);
        assert(__wrap_os_memblock_get(&pools[i]) == NULL);
    }
    read_snapshot();
    for (unsigned int i = 0; i < 5; ++i) {
        assert(snapshot.pools[i].allocations == 3 && snapshot.pools[i].failures == 1);
        assert(snapshot.pools[i].last_free == 0 && snapshot.pools[i].sdk_min_free == 0);
        assert(snapshot.trace[i].event == ESP32_BLE5_DIAG_POOL_FAILURE && snapshot.trace[i].arg == i);
    }
    assert(snapshot.trace_written == 5);
    puts("PASS five independent pools, exhaustion, untracked allocations, watermarks");

    assert(__wrap_os_mempool_init(&pools[0], 0, 0, NULL, "untracked") == 7);
    assert(__wrap_os_memblock_get(&pools[0]) == NULL);
    read_snapshot();
    assert(snapshot.pools[0].failures == 2);
    assert(__wrap_os_mempool_init(&pools[0], 2, 260, NULL, "untracked") == 0);
    assert(__wrap_os_memblock_get(&pools[0]) == &block);
    read_snapshot();
    assert(!snapshot.pools[0].detected && snapshot.pools[0].blocks == 0);
    assert(snapshot.pools[0].allocations == 4);
    assert(__wrap_os_mempool_init(&pools[0], 3, 260, NULL, names[0]) == 0);
    read_snapshot();
    assert(snapshot.pools[0].blocks == 3 && snapshot.pools[0].sdk_min_free == 3);
    puts("PASS failed initialization, address reuse and pool reinitialization");

    esp32_ble5_diag_snapshot(-1, true, &snapshot);
    struct ble_gap_event event = {0};
    for (unsigned int kind = 0; kind < 2; ++kind) {
        for (unsigned int status = 0; status < 4; ++status) {
            if (kind == 0) {
                event.type = BLE_GAP_EVENT_EXT_DISC;
                event.ext_disc.data_status = (uint8_t)(status == 3 ? 4 : status);
                event.ext_disc.length_data = 100;
                event.ext_disc.sid = 3;
            } else {
                event.type = BLE_GAP_EVENT_PERIODIC_REPORT;
                event.periodic_report.data_status = (uint8_t)(status == 3 ? 4 : status);
                event.periodic_report.data_length = 100;
                event.periodic_report.sync_handle = 17;
            }
            esp32_ble5_diag_event(&event);
        }
    }
    event.type = BLE_GAP_EVENT_PERIODIC_SYNC;
    event.periodic_sync.status = 62;
    event.periodic_sync.sync_handle = 123;
    esp32_ble5_diag_event(&event);
    event.type = BLE_GAP_EVENT_PERIODIC_SYNC_LOST;
    event.periodic_sync_lost.sync_handle = 17;
    event.periodic_sync_lost.reason = 13;
    esp32_ble5_diag_event(&event);
    read_snapshot();
    for (unsigned int i = 0; i < 2; ++i) {
        assert(snapshot.rx[i].reports == 4 && snapshot.rx[i].bytes == 400);
        for (unsigned int j = 0; j < 4; ++j) {
            assert(snapshot.rx[i].status[j] == 1);
        }
    }
    assert(snapshot.trace_written == 6);
    assert(snapshot.trace[4].event == ESP32_BLE5_DIAG_SYNC && snapshot.trace[4].handle == UINT16_MAX);
    assert(snapshot.trace[5].event == ESP32_BLE5_DIAG_SYNC_LOST && snapshot.trace[5].arg == 13);
    puts("PASS report status buckets, failed sync handle and sync loss timeline");

    esp32_ble5_diag_token_t token;
    test_time = 100;
    esp32_ble5_diag_begin(&token, ESP32_BLE5_DIAG_SCAN);
    test_time += 7000;
    esp32_ble5_diag_acquired(&token);
    test_time += 5000;
    esp32_ble5_diag_end(&token);
    read_snapshot();
    assert(snapshot.rx[0].timed == 1);
    assert(snapshot.rx[0].gil_wait_us == 7000 && snapshot.rx[0].gil_wait_max_us == 7000);
    assert(snapshot.rx[0].callback_us == 5000 && snapshot.rx[0].callback_max_us == 5000);
    assert(snapshot.trace[6].event == ESP32_BLE5_DIAG_GIL_SLOW);
    assert(snapshot.trace[7].event == ESP32_BLE5_DIAG_CALLBACK_SLOW);
    puts("PASS GIL acquisition and callback wall time measured separately");

    esp32_ble5_diag_begin(&token, ESP32_BLE5_DIAG_PERIODIC);
    esp32_ble5_diag_snapshot(-1, true, &snapshot);
    esp32_ble5_diag_acquired(&token);
    esp32_ble5_diag_end(&token);
    read_snapshot();
    assert(snapshot.rx[1].timed == 0);
    assert(snapshot.pools[0].detected && snapshot.pools[0].blocks == 3);
    assert(__wrap_os_memblock_get(&pools[0]) == &block);
    read_snapshot();
    assert(snapshot.pools[0].allocations == 1);
    esp32_ble5_diag_begin(&token, ESP32_BLE5_DIAG_PERIODIC);
    esp32_ble5_diag_snapshot(0, false, &snapshot);
    esp32_ble5_diag_snapshot(1, false, &snapshot);
    esp32_ble5_diag_acquired(&token);
    esp32_ble5_diag_end(&token);
    read_snapshot();
    assert(snapshot.rx[1].timed == 0);
    puts("PASS clear and enable transitions discard in-flight timing, retain pool registration");

    esp32_ble5_diag_snapshot(-1, true, &snapshot);
    for (unsigned int i = 1; i <= 40; ++i) {
        event.periodic_sync_lost.reason = (uint16_t)i;
        test_time = i * 100;
        esp32_ble5_diag_event(&event);
    }
    read_snapshot();
    assert(snapshot.trace_written == 40);
    for (unsigned int i = 0; i < ESP32_BLE5_DIAG_TRACE; ++i) {
        unsigned int index = (snapshot.trace_written - ESP32_BLE5_DIAG_TRACE + i) % ESP32_BLE5_DIAG_TRACE;
        assert(snapshot.trace[index].arg == i + 9);
        assert(snapshot.trace[index].time_us == (i + 9) * 100);
    }
    puts("PASS bounded ring retains the last 32 events");

    esp32_ble5_diag_snapshot(-1, true, &snapshot);
    test_time = UINT32_MAX - 15ULL;
    esp32_ble5_diag_begin(&token, ESP32_BLE5_DIAG_PERIODIC);
    test_time += 20;
    esp32_ble5_diag_acquired(&token);
    test_time += 30;
    esp32_ble5_diag_end(&token);
    read_snapshot();
    assert(snapshot.rx[1].gil_wait_us == 20 && snapshot.rx[1].callback_us == 30);
    puts("PASS timing across 32-bit microsecond wrap");
    return 0;
}
