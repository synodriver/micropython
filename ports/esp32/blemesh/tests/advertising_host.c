// Deterministic interleavings for the real gate and generated adapter entry points.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>

#define portMAX_DELAY 0xffffffffU
#define pdMS_TO_TICKS(x) (x)
#define configASSERT(x) assert(x)
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define BLE_HS_ETIMEOUT 13
#define BLE_HS_EPREEMPTED 16
#define BLE_HS_ECONTROLLER 12
#define BLE_HS_EUNKNOWN 17
#define BLE_GAP_EVENT_ADV_COMPLETE 7
#define ESP32_BLE_MESH_ADV_INSTANCE 4
#define CONFIG_BLE_MESH_ADV_INST_ID 3
typedef int portMUX_TYPE;
typedef unsigned TickType_t;
typedef int *SemaphoreHandle_t;
static int semaphore;
static unsigned ticks, failures, last_failure, start_calls;
static bool fail_alloc, fail_start, complete_inside_start;
static void (*delay_hook)(void);
static SemaphoreHandle_t xSemaphoreCreateBinary(void) { return fail_alloc ? NULL : &semaphore; }
static int xSemaphoreTake(SemaphoreHandle_t s, unsigned timeout) {
    if (timeout == portMAX_DELAY) { assert(*s); } // Any lost wake fails here.
    int got = *s;
    *s = 0;
    return got;
}
static void xSemaphoreGive(SemaphoreHandle_t s) { *s = 1; }
static TickType_t xTaskGetTickCount(void) { return ticks; }
static void vTaskDelay(TickType_t n) {
    ticks += n;
    if (delay_hook) { delay_hook(); }
}
static void esp32_blemesh_adv_failed(int reason) { ++failures; last_failure = reason; }

// ADVERTISING_IMPLEMENTATION

struct ble_gap_event { int type; struct {int instance, reason;} adv_complete; };
struct bt_mesh_adv_param { int unused; };
struct bt_mesh_adv_data { int unused; };
static uintptr_t configured_token;

// GENERATED_CALLBACK

static int mesh_adv_start(uint8_t id, const struct bt_mesh_adv_param *param,
    const struct bt_mesh_adv_data *ad, size_t ad_len,
    const struct bt_mesh_adv_data *sd, size_t sd_len, uintptr_t token) {
    (void)param; (void)ad; (void)ad_len; (void)sd; (void)sd_len;
    assert(id == CONFIG_BLE_MESH_ADV_INST_ID);
    ++start_calls;
    configured_token = token;
    if (complete_inside_start) {
        struct ble_gap_event event = {BLE_GAP_EVENT_ADV_COMPLETE, {4, BLE_HS_ETIMEOUT}};
        gap_event_cb(&event, (void *)token);
    }
    return fail_start ? -EIO : 0;
}

// GENERATED_START

static void finish_inflight_start(void) {
    assert(esp32_blemesh_adv_begin() == 0); // Closing forbids a second start.
    esp32_blemesh_adv_end(0);
    delay_hook = NULL;
}

static void emit(uintptr_t token, int instance, int reason) {
    struct ble_gap_event event = {BLE_GAP_EVENT_ADV_COMPLETE, {instance, reason}};
    gap_event_cb(&event, (void *)token);
    assert(event.adv_complete.instance == instance); // Host event not mutated.
}

static int worker_finish(void) {
    int err = 0;
    // GENERATED_WAIT
    return err; // SDK forwards this same err to BLE_MESH_SEND_END_CB.
}

enum { MESH_STOPPED, MESH_STARTING, MESH_RUNNING, MESH_STOPPING, MESH_FAILED };
#define MP_EIO EIO
#define MP_EBUSY EBUSY
#define BLE_HS_EALREADY 2
#define MP_THREAD_GIL_EXIT() ((void)0)
#define MP_THREAD_GIL_ENTER() ((void)0)
#define EVENT_STOPPED 6
typedef struct { bool erase_flash; } esp_ble_mesh_deinit_param_t;
typedef struct { int event; } mesh_event_t;
static int state, operation_error, deinit_error, deinit_calls, removed;
static bool can_deinit;
static void enqueue(const mesh_event_t *item) { assert(item->event == EVENT_STOPPED); }
static void esp32_blemesh_adapter_reset(void) { assert(users == 0 && !enabled); }
static int esp_ble_mesh_deinit(esp_ble_mesh_deinit_param_t *param) {
    (void)param;
    assert(users == 0 && !enabled);
    ++deinit_calls;
    emit(configured_token, 4, BLE_HS_ETIMEOUT); // SDK task has been deleted.
    return deinit_error;
}
static int ble_gap_ext_adv_stop(int instance) { assert(instance == 4); return 0; }
static int ble_gap_ext_adv_remove(int instance) { assert(instance == 4); ++removed; return 0; }

// NATIVE_STOP

int main(void) {
    fail_alloc = true;
    assert(esp32_blemesh_adv_open() == -ENOMEM);
    fail_alloc = false;
    assert(esp32_blemesh_adv_open() == 0);
    complete_inside_start = true;
    assert(bt_le_ext_adv_start(3, NULL, NULL, 0, NULL, 0) == 0);
    assert(worker_finish() == 0); // Completion before wait is retained.
    complete_inside_start = false;
    fail_start = true;
    assert(bt_le_ext_adv_start(3, NULL, NULL, 0, NULL, 0) == -EIO);
    fail_start = false;
    assert(bt_le_ext_adv_start(3, NULL, NULL, 0, NULL, 0) == 0);
    uintptr_t old = configured_token; // Event already extracted by Host.
    assert(esp32_blemesh_adv_close() == 0);
    assert(worker_finish() == -ECANCELED);
    unsigned calls = start_calls;
    assert(bt_le_ext_adv_start(3, NULL, NULL, 0, NULL, 0) == -EIO);
    assert(start_calls == calls);
    // SDK task is now deleted; callback touches only persistent gate storage.
    emit(old, 4, BLE_HS_ETIMEOUT);
    assert(!semaphore);
    assert(esp32_blemesh_adv_open() == 0);
    assert(bt_le_ext_adv_start(3, NULL, NULL, 0, NULL, 0) == 0);
    emit(old, 4, BLE_HS_ETIMEOUT);
    emit(configured_token, 0, BLE_HS_ETIMEOUT);
    assert(!semaphore); // Neither old-session nor application completion wakes Mesh.
    emit(configured_token, 4, BLE_HS_ETIMEOUT);
    assert(worker_finish() == 0);
    assert(esp32_blemesh_adv_close() == 0);

    // Stop while SDK start is inside HCI: it must drain before task deletion.
    assert(esp32_blemesh_adv_open() == 0);
    assert(esp32_blemesh_adv_begin());
    delay_hook = finish_inflight_start;
    assert(esp32_blemesh_adv_close() == 0);
    assert(users == 0 && ticks == 1);
    assert(esp32_blemesh_adv_open() == 0);
    old = esp32_blemesh_adv_begin();
    assert(esp32_blemesh_adv_close() == -ETIMEDOUT);
    assert(users == 1); // Caller must not deinit SDK on this failure.
    emit(old, 4, BLE_HS_ETIMEOUT);
    esp32_blemesh_adv_end(-EIO);
    assert(esp32_blemesh_adv_close() == 0);

    const int reasons[] = {BLE_HS_EPREEMPTED, BLE_HS_ECONTROLLER, 0};
    for (unsigned i = 0; i < sizeof(reasons) / sizeof(reasons[0]); ++i) {
        assert(esp32_blemesh_adv_open() == 0);
        assert(bt_le_ext_adv_start(3, NULL, NULL, 0, NULL, 0) == 0);
        unsigned before = failures;
        emit(configured_token, 4, reasons[i]);
        assert(worker_finish() == -EIO); // Must not turn abnormal completion into success.
        assert(failures == before + 1 && last_failure != 0);
        assert(esp32_blemesh_adv_error() == (reasons[i] ? reasons[i] : BLE_HS_EUNKNOWN));
        assert(esp32_blemesh_adv_begin() == 0);
        emit(configured_token, 4, reasons[i]);
        assert(failures == before + 1); // Duplicate error is suppressed.
        assert(esp32_blemesh_adv_close() == 0);
    }
    assert(esp32_blemesh_adv_open() == 0);
    assert(esp32_blemesh_adv_begin());
    esp32_blemesh_adv_abort(BLE_HS_ECONTROLLER); // Reset during start, with no completion.
    esp32_blemesh_adv_end(0);
    assert(worker_finish() == -EIO);
    assert(esp32_blemesh_adv_close() == 0);
    assert(esp32_blemesh_adv_open() == 0);
    esp32_blemesh_adv_abort(BLE_HS_ECONTROLLER); // Idle reset is observable too.
    assert(esp32_blemesh_adv_error() == BLE_HS_ECONTROLLER);
    assert(esp32_blemesh_adv_close() == 0);
    // Execute native stop as well: timeout must not delete an in-flight task.
    assert(esp32_blemesh_adv_open() == 0);
    configured_token = esp32_blemesh_adv_begin();
    state = MESH_RUNNING;
    can_deinit = true;
    assert(stop(false) == EIO);
    assert(state == MESH_FAILED && can_deinit && deinit_calls == 0);
    esp32_blemesh_adv_end(0);
    assert(stop(false) == 0);
    assert(state == MESH_STOPPED && deinit_calls == 1 && removed == 1);
    assert(esp32_blemesh_adv_open() == 0);
    state = MESH_RUNNING;
    can_deinit = true;
    deinit_error = EIO;
    assert(stop(false) == EIO);
    assert(state == MESH_FAILED && !can_deinit && deinit_calls == 2);
    emit(configured_token, 4, BLE_HS_ETIMEOUT);
    assert(stop(false) == EIO && deinit_calls == 2); // No call into destroyed BTC.
    puts("PASS advertising close/drain, late callbacks, restart, preemption/reset and error propagation");
}
