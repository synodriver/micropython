// One physical scanner, independent Mesh and MicroPython subscriptions.
// Never call Python, Mesh callbacks, or wait for the GIL while holding lock.
#include <string.h>
#include "blemesh.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nimble/nimble_port.h"

typedef struct {
    bool active;
    uint8_t own_addr_type;
    bool has_uncoded, has_coded;
    struct ble_gap_ext_disc_params uncoded, coded;
    ble_gap_event_fn *cb;
    void *arg;
} scan_request_t;

static SemaphoreHandle_t lock;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static scan_request_t requests[2]; // 0: application, 1: Mesh
static struct ble_npl_callout timeout;
static struct ble_npl_callout retry;
static bool initialized, paused;
static uint32_t generation;
static uint32_t deadline;
static bool timed;
static volatile int last_error;

static int scan_cb(struct ble_gap_event *event, void *arg);

// A continuous window satisfies both users' receive windows; the shortest
// interval and active scanning win. PHY selection remains per subscriber.
static void merge_phy(struct ble_gap_ext_disc_params *out,
    const struct ble_gap_ext_disc_params *in, bool first) {
    if (first) {
        *out = *in;
    } else {
        out->passive &= in->passive;
        out->itvl = out->itvl < in->itvl ? out->itvl : in->itvl;
        out->window = out->itvl;
    }
}

// Caller holds lock. Real SDK calls occur outside the short critical section.
static int apply(void) {
    scan_request_t snapshot[2];
    portENTER_CRITICAL(&mux);
    memcpy(snapshot, requests, sizeof(snapshot));
    uint32_t epoch = ++generation;
    portEXIT_CRITICAL(&mux);
    int err = ble_gap_disc_active() ? ble_gap_disc_cancel() : 0;
    if (err && err != BLE_HS_EALREADY) {
        return err;
    }
    if (paused || (!snapshot[0].active && !snapshot[1].active)) {
        return 0;
    }
    struct ble_gap_ext_disc_params uncoded = {0}, coded = {0};
    bool has_uncoded = false, has_coded = false;
    for (unsigned i = 0; i < 2; ++i) {
        if (!snapshot[i].active) {
            continue;
        }
        if (snapshot[i].has_uncoded) {
            merge_phy(&uncoded, &snapshot[i].uncoded, !has_uncoded);
            has_uncoded = true;
        }
        if (snapshot[i].has_coded) {
            merge_phy(&coded, &snapshot[i].coded, !has_coded);
            has_coded = true;
        }
    }
    // Application address/privacy policy takes precedence while it scans.
    uint8_t address = snapshot[snapshot[0].active ? 0 : 1].own_addr_type;
    return ble_gap_ext_disc(address, 0, 0, 0, 0, 0,
        has_uncoded ? &uncoded : NULL, has_coded ? &coded : NULL,
        scan_cb, (void *)(uintptr_t)epoch);
}

static void retry_cb(struct ble_npl_event *event) {
    (void)event;
    xSemaphoreTake(lock, portMAX_DELAY);
    last_error = apply();
    if (last_error) {
        ble_npl_callout_reset(&retry, ble_npl_time_ms_to_ticks32(100));
    }
    xSemaphoreGive(lock);
}

static int cancel(unsigned owner) {
    xSemaphoreTake(lock, portMAX_DELAY);
    portENTER_CRITICAL(&mux);
    bool was_active = requests[owner].active;
    requests[owner].active = false;
    portEXIT_CRITICAL(&mux);
    if (!was_active) {
        xSemaphoreGive(lock);
        return 0;
    }
    if (owner == 0) {
        timed = false;
        ble_npl_callout_stop(&timeout);
    }
    // Cancellation always removes this subscription. A remaining subscriber
    // is retried if the controller is temporarily busy; expose that failure.
    last_error = apply();
    if (last_error) {
        ble_npl_callout_reset(&retry, ble_npl_time_ms_to_ticks32(100));
    }
    xSemaphoreGive(lock);
    return 0;
}

static void timeout_cb(struct ble_npl_event *event) {
    (void)event;
    scan_request_t old = {0};
    xSemaphoreTake(lock, portMAX_DELAY);
    if (timed) {
        int32_t remaining = (int32_t)(deadline - ble_npl_time_get());
        if (remaining > 0) {
            // A stopped callout may already have been queued by FreeRTOS.
            ble_npl_callout_reset(&timeout, remaining);
        } else {
            portENTER_CRITICAL(&mux);
            old = requests[0];
            requests[0].active = false;
            portEXIT_CRITICAL(&mux);
            timed = false;
            last_error = apply();
            if (last_error) {
                ble_npl_callout_reset(&retry, ble_npl_time_ms_to_ticks32(100));
            }
        }
    }
    xSemaphoreGive(lock);
    if (old.active) {
        struct ble_gap_event done = {.type = BLE_GAP_EVENT_DISC_COMPLETE};
        old.cb(&done, old.arg);
    }
}

static int scan_cb(struct ble_gap_event *event, void *arg) {
    scan_request_t snapshot[2];
    portENTER_CRITICAL(&mux);
    bool current = (uint32_t)(uintptr_t)arg == generation;
    memcpy(snapshot, requests, sizeof(snapshot));
    portEXIT_CRITICAL(&mux);
    if (!current) {
        return 0;
    }
    if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        // The physical scan is indefinite. Recover controller/preemption
        // termination without generating an application timeout early.
        ble_npl_callout_reset(&retry, ble_npl_time_ms_to_ticks32(10));
        return 0;
    }
    if (event->type != BLE_GAP_EVENT_EXT_DISC) {
        return 0;
    }
    // Mesh first: Python may wait for the GIL. Filter per subscriber PHY and
    // do not expose scan responses to a passive-only application request.
    for (int i = 1; i >= 0; --i) {
        scan_request_t *r = &snapshot[i];
        bool coded = event->ext_disc.prim_phy == BLE_HCI_LE_PHY_CODED;
        const struct ble_gap_ext_disc_params *p = coded ? &r->coded : &r->uncoded;
        if (r->active && (coded ? r->has_coded : r->has_uncoded)
            && !(p->passive && (event->ext_disc.props & BLE_HCI_ADV_SCAN_RSP_MASK))) {
            if (i == 1) {
                // This profile uses legacy Mesh PDUs only. Give the SDK its
                // own writable bytes so protocol parsing cannot alter the
                // raw advertisement later delivered to the application.
                uint8_t data[31];
                if (event->ext_disc.length_data > sizeof(data)) {
                    continue;
                }
                struct ble_gap_event copy = *event;
                if (event->ext_disc.length_data) {
                    memcpy(data, event->ext_disc.data, event->ext_disc.length_data);
                }
                copy.ext_disc.data = data;
                r->cb(&copy, r->arg);
            } else {
                r->cb(event, r->arg);
            }
        }
    }
    return 0;
}

static int start(unsigned owner, uint8_t address, uint16_t duration,
    uint16_t period, uint8_t duplicates, uint8_t policy, uint8_t limited,
    const struct ble_gap_ext_disc_params *uncoded,
    const struct ble_gap_ext_disc_params *coded, ble_gap_event_fn *cb, void *arg) {
    if (period || policy || limited || !cb || (!uncoded && !coded)) {
        return BLE_HS_EINVAL;
    }
    (void)duplicates; // Mesh must see retransmissions; BLE5 already requests 0.
    xSemaphoreTake(lock, portMAX_DELAY);
    scan_request_t next = {.active = true, .own_addr_type = address,
        .has_uncoded = uncoded != NULL, .has_coded = coded != NULL, .cb = cb, .arg = arg};
    if (uncoded) {
        next.uncoded = *uncoded;
    }
    if (coded) {
        next.coded = *coded;
    }
    portENTER_CRITICAL(&mux);
    scan_request_t old = requests[owner];
    if (!old.active) {
        requests[owner] = next;
    }
    portEXIT_CRITICAL(&mux);
    int err = old.active ? BLE_HS_EALREADY : apply();
    if (err && !old.active) {
        portENTER_CRITICAL(&mux);
        requests[owner] = old;
        portEXIT_CRITICAL(&mux);
        last_error = apply();
        if (last_error) {
            ble_npl_callout_reset(&retry, ble_npl_time_ms_to_ticks32(100));
        }
    } else if (!err && owner == 0) {
        timed = duration != 0;
        if (timed) {
            uint32_t ticks = ble_npl_time_ms_to_ticks32((uint32_t)duration * 10);
            deadline = ble_npl_time_get() + ticks;
            ble_npl_callout_reset(&timeout, ticks);
        }
    }
    xSemaphoreGive(lock);
    return err;
}

#define SCAN_ARGS uint8_t a, uint16_t d, uint16_t p, uint8_t f, uint8_t policy, uint8_t l, \
    const struct ble_gap_ext_disc_params *u, const struct ble_gap_ext_disc_params *c, ble_gap_event_fn *cb, void *arg
int esp32_blemesh_app_scan(SCAN_ARGS) {
    return start(0, a, d, p, f, policy, l, u, c, cb, arg);
}
int esp32_blemesh_mesh_scan(SCAN_ARGS) {
    return start(1, a, d, p, f, policy, l, u, c, cb, arg);
}
int esp32_blemesh_app_scan_cancel(void) {
    return cancel(0);
}
int esp32_blemesh_mesh_scan_cancel(void) {
    return cancel(1);
}
int esp32_blemesh_app_scan_active(void) {
    portENTER_CRITICAL(&mux);
    bool active = requests[0].active;
    portEXIT_CRITICAL(&mux);
    return active;
}

int esp32_blemesh_connect_pause(void) {
    xSemaphoreTake(lock, portMAX_DELAY);
    if (paused) {
        xSemaphoreGive(lock);
        return BLE_HS_EBUSY;
    }
    paused = true;
    int err = apply();
    if (err) {
        paused = false;
        last_error = apply();
        if (last_error) {
            ble_npl_callout_reset(&retry, ble_npl_time_ms_to_ticks32(100));
        }
    }
    xSemaphoreGive(lock);
    return err;
}
void esp32_blemesh_connect_resume(void) {
    xSemaphoreTake(lock, portMAX_DELAY);
    paused = false;
    xSemaphoreGive(lock);
    // CONNECT can be delivered before NimBLE finishes clearing its master
    // state, and periodic-sync retries also share that state. Defer restart.
    ble_npl_callout_reset(&retry, ble_npl_time_ms_to_ticks32(1));
}
int esp32_blemesh_radio_error(void) {
    return last_error;
}
void esp32_blemesh_radio_init(void) {
    if (!lock) {
        lock = xSemaphoreCreateMutex();
        configASSERT(lock);
    }
    memset(requests, 0, sizeof(requests));
    paused = timed = false;
    last_error = 0;
    ble_npl_callout_init(&timeout, nimble_port_get_dflt_eventq(), timeout_cb, NULL);
    ble_npl_callout_init(&retry, nimble_port_get_dflt_eventq(), retry_cb, NULL);
    initialized = true;
}
void esp32_blemesh_radio_reset(void) {
    if (!initialized) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    portENTER_CRITICAL(&mux);
    requests[0].active = false;
    ++generation;
    portEXIT_CRITICAL(&mux);
    timed = paused = false;
    ble_npl_callout_stop(&timeout);
    ble_npl_callout_stop(&retry);
    // Do not mutate adapter caches while an SDK advertising start is in flight.
    // Reset closes admission and releases the private send wait with an error.
    esp32_blemesh_adv_abort(BLE_HS_ECONTROLLER);
    xSemaphoreGive(lock);
}
void esp32_blemesh_radio_sync(void) {
    if (initialized) {
        ble_npl_callout_reset(&retry, ble_npl_time_ms_to_ticks32(1));
    }
}
void esp32_blemesh_radio_deinit(void) {
    // Called after the host stops, before its allocator/event queue is freed.
    if (initialized) {
        ble_npl_callout_deinit(&timeout);
        ble_npl_callout_deinit(&retry);
        initialized = false;
    }
}
