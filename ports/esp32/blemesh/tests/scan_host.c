// Deterministic host tests for the real scan.c, with simulated controller/NPL.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CONFIG_BLE_MESH 1
#define BLE_HS_EALREADY 2
#define BLE_HS_EBUSY 15
#define BLE_HS_EINVAL 3
#define BLE_GAP_EVENT_EXT_DISC 1
#define BLE_GAP_EVENT_DISC_COMPLETE 2
#define BLE_HCI_LE_PHY_CODED 3
#define BLE_HCI_ADV_SCAN_RSP_MASK 8
#define portMAX_DELAY 0xffffffff
#define configASSERT(x) assert(x)
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
typedef int portMUX_TYPE;
typedef int *SemaphoreHandle_t;
static int mutex;
static SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &mutex; }
static void xSemaphoreTake(SemaphoreHandle_t m, unsigned delay) { (void)delay; assert(!*m); *m = 1; }
static void xSemaphoreGive(SemaphoreHandle_t m) { assert(*m); *m = 0; }
struct ble_gap_ext_disc_params { uint16_t itvl, window; uint8_t passive; };
struct ble_gap_event {
    uint8_t type;
    struct {uint8_t prim_phy; uint16_t props, length_data; const uint8_t *data;} ext_disc;
    struct {int reason;} disc_complete;
};
typedef int ble_gap_event_fn(struct ble_gap_event *, void *);
struct ble_npl_event { int unused; };
struct ble_npl_callout { void (*fn)(struct ble_npl_event *); unsigned due; bool armed; };
static uint32_t test_ticks;
static unsigned advertising_aborts;
static void esp32_blemesh_adv_abort(int reason) { assert(reason == 12); ++advertising_aborts; }
#define BLE_HS_ECONTROLLER 12
static int nimble_port_get_dflt_eventq(void) { return 0; }
static uint32_t ble_npl_time_get(void) { return test_ticks; }
static uint32_t ble_npl_time_ms_to_ticks32(uint32_t ms) { return ms; }
static void ble_npl_callout_init(struct ble_npl_callout *c, int q, void (*fn)(struct ble_npl_event *), void *arg) {
    (void)q; (void)arg; c->fn = fn; c->armed = false;
}
static void ble_npl_callout_reset(struct ble_npl_callout *c, uint32_t delay) { c->due = test_ticks + delay; c->armed = true; }
static void ble_npl_callout_stop(struct ble_npl_callout *c) { c->armed = false; }
static void ble_npl_callout_deinit(struct ble_npl_callout *c) { c->armed = false; }
static bool physical;
static unsigned starts, cancels;
static int start_error, cancel_error;
static ble_gap_event_fn *physical_cb;
static void *physical_arg;
static struct ble_gap_ext_disc_params physical_uncoded, physical_coded;
static bool physical_has_coded;
static int ble_gap_disc_active(void) { return physical; }
static int ble_gap_disc_cancel(void) {
    ++cancels;
    if (cancel_error) { int err = cancel_error; cancel_error = 0; return err; }
    physical = false; return 0;
}
static int ble_gap_ext_disc(uint8_t address, uint16_t duration, uint16_t period, uint8_t dup,
    uint8_t policy, uint8_t limited, const struct ble_gap_ext_disc_params *u,
    const struct ble_gap_ext_disc_params *c, ble_gap_event_fn *cb, void *arg) {
    (void)address; (void)dup; (void)policy; (void)limited;
    assert(!duration && !period && !physical);
    ++starts;
    if (start_error) { int err = start_error; start_error = 0; return err; }
    if (u) { physical_uncoded = *u; }
    if (c) { physical_coded = *c; }
    physical_has_coded = c != NULL;
    physical = true; physical_cb = cb; physical_arg = arg; return 0;
}

// SCAN_IMPLEMENTATION

static unsigned app_reports, mesh_reports, app_done;
static bool cancel_in_callback;
static int app_cb(struct ble_gap_event *event, void *arg) {
    assert(arg == (void *)10 && !mutex);
    if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) { ++app_done; }
    else { assert(event->ext_disc.data[0] == 42); ++app_reports; }
    if (cancel_in_callback) { esp32_blemesh_app_scan_cancel(); }
    return 0;
}
static int mesh_cb(struct ble_gap_event *event, void *arg) {
    assert(arg == (void *)20 && !mutex);
    assert(event->type == BLE_GAP_EVENT_EXT_DISC);
    ((uint8_t *)event->ext_disc.data)[0] = 99;
    ++mesh_reports; return 0;
}
static void report(int phy, int props) {
    struct ble_gap_event event = {.type = BLE_GAP_EVENT_EXT_DISC};
    uint8_t data[] = {42};
    event.ext_disc.data = data; event.ext_disc.length_data = sizeof(data);
    event.ext_disc.prim_phy = (uint8_t)phy; event.ext_disc.props = (uint16_t)props;
    assert(physical); physical_cb(&event, physical_arg);
}
static void fire(struct ble_npl_callout *c) {
    test_ticks = c->due; c->armed = false; c->fn(NULL);
}
int main(void) {
    struct ble_gap_ext_disc_params app = {.itvl = 160, .window = 80, .passive = 1};
    struct ble_gap_ext_disc_params mesh = {.itvl = 32, .window = 32, .passive = 1};
    esp32_blemesh_radio_init();
    assert(!esp32_blemesh_app_scan(0, 100, 0, 0, 0, 0, &app, NULL, app_cb, (void *)10));
    assert(!esp32_blemesh_mesh_scan(0, 0, 0, 0, 0, 0, &mesh, NULL, mesh_cb, (void *)20));
    assert(physical_uncoded.itvl == 32 && physical_uncoded.window == 32);
    report(1, 0); assert(app_reports == 1 && mesh_reports == 1);
    fire(&timeout); assert(app_done == 1 && physical && !esp32_blemesh_app_scan_active());
    report(1, 0); assert(app_reports == 1 && mesh_reports == 2);
    unsigned before = starts;
    esp32_blemesh_app_scan_cancel(); assert(starts == before && physical);

    // Coded-only app: Mesh still receives 1M, app only receives Coded.
    assert(!esp32_blemesh_app_scan(0, 0, 0, 0, 0, 0, NULL, &app, app_cb, (void *)10));
    assert(physical_has_coded);
    report(1, 0); assert(app_reports == 1 && mesh_reports == 3);
    report(3, 0); assert(app_reports == 2 && mesh_reports == 3);
    report(3, BLE_HCI_ADV_SCAN_RSP_MASK); assert(app_reports == 2);
    esp32_blemesh_mesh_scan_cancel(); assert(physical && esp32_blemesh_app_scan_active());
    report(3, 0); assert(app_reports == 3 && mesh_reports == 3);
    esp32_blemesh_app_scan_cancel(); assert(!physical);

    // A failed new subscription rolls back to the previous Mesh scanner.
    assert(!esp32_blemesh_mesh_scan(0, 0, 0, 0, 0, 0, &mesh, NULL, mesh_cb, (void *)20));
    start_error = BLE_HS_EBUSY;
    assert(esp32_blemesh_app_scan(0, 0, 0, 0, 0, 0, &app, NULL, app_cb, (void *)10) == BLE_HS_EBUSY);
    assert(physical && !esp32_blemesh_app_scan_active());
    report(1, 0); assert(mesh_reports == 4);

    assert(!esp32_blemesh_connect_pause() && !physical);
    assert(esp32_blemesh_connect_pause() == BLE_HS_EBUSY);
    esp32_blemesh_connect_resume(); fire(&retry); assert(physical);
    report(1, 0); assert(mesh_reports == 5);

    cancel_error = BLE_HS_EBUSY;
    assert(esp32_blemesh_connect_pause() == BLE_HS_EBUSY && physical);
    report(1, 0); assert(mesh_reports == 6);

    assert(!esp32_blemesh_app_scan(0, 100, 0, 0, 0, 0, &app, NULL, app_cb, (void *)10));
    ble_gap_event_fn *stale_cb = physical_cb;
    void *stale_arg = physical_arg;
    cancel_in_callback = true;
    report(1, 0); assert(!esp32_blemesh_app_scan_active() && physical);
    cancel_in_callback = false;
    struct ble_gap_event stale = {.type = BLE_GAP_EVENT_EXT_DISC};
    before = mesh_reports;
    stale_cb(&stale, stale_arg); assert(mesh_reports == before);
    // A queued old timeout cannot terminate a replacement scan early.
    assert(!esp32_blemesh_app_scan(0, 200, 0, 0, 0, 0, &app, NULL, app_cb, (void *)10));
    timeout_cb(NULL); assert(esp32_blemesh_app_scan_active() && app_done == 1);
    cancel_error = BLE_HS_EBUSY;
    esp32_blemesh_app_scan_cancel(); assert(esp32_blemesh_radio_error() == BLE_HS_EBUSY);
    fire(&retry); assert(physical && !esp32_blemesh_radio_error());
    physical = false; // Controller reset clears its physical scanner.
    esp32_blemesh_radio_reset();
    assert(advertising_aborts == 1);
    assert(!esp32_blemesh_app_scan_active());
    esp32_blemesh_radio_sync(); fire(&retry);
    assert(physical);
    before = mesh_reports; report(1, 0); assert(mesh_reports == before + 1);
    esp32_blemesh_mesh_scan_cancel(); assert(!physical);
    esp32_blemesh_radio_deinit();
    esp32_blemesh_radio_init();
    assert(!esp32_blemesh_app_scan_active());
    esp32_blemesh_radio_deinit();
    puts("PASS scanner coexistence, rollback, timeout, cancellation, PHY, connect and restart");
    return 0;
}
