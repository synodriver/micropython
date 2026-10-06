// Appended to the shared NimBLE bindings by make_bindings.py.
#if MICROPY_ESP32_BLE5
#include "host/ble_hs_mbuf.h"
#include "py/mpthread.h"

#if MICROPY_ESP32_BLE5_PERIODIC_ADV
static void esp32_ble5_periodic_reset(void);
static uint32_t esp32_ble5_gap_generation(void);
static int esp32_ble5_gap_cb(struct ble_gap_event *event, void *arg);
#endif

void esp32_ble5_reset(void) {
    // This may run on the host task without the GIL. Drop the GC root; let GC
    // reclaim the state once any in-flight Python calls have also released it.
    MP_STATE_PORT(bluetooth_ble5_state) = NULL;
    #if MICROPY_ESP32_BLE5_PERIODIC_ADV
    esp32_ble5_periodic_reset();
    #endif
}

uint8_t esp32_ble5_phy_mask(void) {
    uint8_t mask = BLE_GAP_LE_PHY_1M_MASK;
    #if MYNEWT_VAL(BLE_LL_CFG_FEAT_LE_2M_PHY)
    mask |= BLE_GAP_LE_PHY_2M_MASK;
    #endif
    #if MYNEWT_VAL(BLE_LL_CFG_FEAT_LE_CODED_PHY)
    mask |= BLE_GAP_LE_PHY_CODED_MASK;
    #endif
    return mask;
}

int esp32_ble5_phy(uint16_t conn_handle, uint8_t *tx_phy, uint8_t *rx_phy) {
    if (!mp_bluetooth_is_active()) {
        return MP_ENODEV;
    }
    return ble_hs_err_to_errno(ble_gap_read_le_phy(conn_handle, tx_phy, rx_phy));
}

int esp32_ble5_set_phy(int conn_handle, int tx_mask, int rx_mask, int options) {
    if (!mp_bluetooth_is_active()) {
        return MP_ENODEV;
    }
    int supported = esp32_ble5_phy_mask();
    if (tx_mask <= 0 || rx_mask <= 0 || (tx_mask & ~supported) || (rx_mask & ~supported)
        || options < BLE_GAP_LE_PHY_CODED_ANY || options > BLE_GAP_LE_PHY_CODED_S8) {
        return MP_EINVAL;
    }
    if (conn_handle < 0) {
        if (options != BLE_GAP_LE_PHY_CODED_ANY) {
            return MP_EINVAL;
        }
        return ble_hs_err_to_errno(ble_gap_set_prefered_default_le_phy(tx_mask, rx_mask));
    }
    return ble_hs_err_to_errno(ble_gap_set_prefered_le_phy(conn_handle, tx_mask, rx_mask, options));
}

#if MICROPY_ESP32_BLE5_EXT_ADV
typedef struct {
    bool configured;
    bool periodic;
    struct ble_gap_ext_adv_params params;
    uint8_t *adv_data;
    size_t adv_len;
    uint8_t *resp_data;
    size_t resp_len;
} esp32_ble5_instance_t;

typedef struct _esp32_ble5_state_t {
    esp32_ble5_instance_t instances[BLE_ADV_INSTANCES];
    bool scan_active;
    bool scan_extended;
    uint32_t scan_generation;
    int scan_remaining_ms;
    int scan_phys;
    struct ble_gap_ext_disc_params scan_params;
} esp32_ble5_state_t;

// Never reset this counter when dropping the GC state: queued callbacks may
// still refer to a scan from an earlier stack lifetime.
static uint32_t esp32_ble5_scan_generation;

static esp32_ble5_state_t *esp32_ble5_state(void) {
    if (MP_STATE_PORT(bluetooth_ble5_state) == NULL) {
        MP_STATE_PORT(bluetooth_ble5_state) = m_new0(esp32_ble5_state_t, 1);
    }
    return MP_STATE_PORT(bluetooth_ble5_state);
}

static bool esp32_ble5_valid_instance(int instance, bool legacy) {
    return instance >= (legacy ? 0 : 1) && instance < BLE_ADV_INSTANCES;
}

static int esp32_ble5_set_data(int instance, const uint8_t *data, size_t len, bool response) {
    static const uint8_t empty = 0;
    struct os_mbuf *buffer = ble_hs_mbuf_from_flat(len ? data : &empty, len);
    if (buffer == NULL) {
        return BLE_HS_ENOMEM;
    }
    // Even an empty payload needs a packet header for OS_MBUF_PKTLEN.
    // NimBLE consumes the mbuf on both success and failure.
    return response ? ble_gap_ext_adv_rsp_set_data(instance, buffer) : ble_gap_ext_adv_set_data(instance, buffer);
}

static int esp32_ble5_update_data(int instance, uint8_t **saved, size_t *saved_len, const uint8_t *data, size_t len, bool response) {
    if (data == NULL) {
        return esp32_ble5_set_data(instance, *saved, *saved_len, response);
    }
    // Allocate before submitting, and only cache data accepted by the controller.
    uint8_t *copy = len ? m_new(uint8_t, len) : NULL;
    if (len) {
        memcpy(copy, data, len);
    }
    int err = esp32_ble5_set_data(instance, copy, len, response);
    if (err) {
        if (copy != NULL) {
            m_del(uint8_t, copy, len);
        }
        return err;
    }
    if (*saved != NULL) {
        m_del(uint8_t, *saved, *saved_len);
    }
    *saved = copy;
    *saved_len = len;
    return 0;
}

static int esp32_ble5_advertise_cb(struct ble_gap_event *event, void *arg) {
    if (!mp_bluetooth_is_active()) {
        return 0;
    }
    if (event->type == BLE_GAP_EVENT_ADV_COMPLETE) {
        // Legacy advertisements previously did not expose an ADV_COMPLETE IRQ.
        if (event->adv_complete.instance != 0) {
            esp32_ble5_on_event(event);
        }
        return 0;
    }
    // Connections inherit the same callback and the existing GATT/security IRQs.
    return central_gap_event_cb(event, arg);
}

static int esp32_ble5_advertise_start(const esp32_ble5_adv_params_t *p, bool legacy) {
    if (!mp_bluetooth_is_active()) {
        return MP_ENODEV;
    }
    int interval = p->interval_us / BLE_HCI_ADV_ITVL;
    // The legacy API accepts zero ticks to select NimBLE's default interval.
    if (!esp32_ble5_valid_instance(p->instance, legacy) || (legacy && p->instance != 0)
        || p->interval_us < 0 || (interval < BLE_HCI_ADV_ITVL_MIN && !(legacy && interval == 0))
        || (legacy && interval > BLE_HCI_ADV_ITVL_MAX)
        || p->sid < 0 || p->sid > 15
        || p->timeout_ms < 0 || p->timeout_ms > UINT16_MAX * 10
        || (p->primary_phy != BLE_HCI_LE_PHY_1M && p->primary_phy != BLE_HCI_LE_PHY_CODED)
        || p->secondary_phy < BLE_HCI_LE_PHY_1M || p->secondary_phy > BLE_HCI_LE_PHY_CODED
        || (!legacy && p->connectable && p->scannable)) {
        return MP_EINVAL;
    }
    int phy_mask = esp32_ble5_phy_mask();
    if (!(phy_mask & (1 << (p->primary_phy - 1))) || !(phy_mask & (1 << (p->secondary_phy - 1)))) {
        return MP_EOPNOTSUPP;
    }
    size_t max_len = legacy ? 31 : MIN(MYNEWT_VAL(BLE_EXT_ADV_MAX_SIZE), UINT16_MAX);
    if (p->adv_data_len > max_len || p->resp_data_len > max_len) {
        return MP_EINVAL;
    }
    esp32_ble5_instance_t *instance = &esp32_ble5_state()->instances[p->instance];
    size_t adv_len = p->adv_data ? p->adv_data_len : instance->adv_len;
    size_t resp_len = p->resp_data ? p->resp_data_len : instance->resp_len;
    if ((!legacy && p->scannable && adv_len) || (!p->scannable && resp_len)) {
        return MP_EINVAL;
    }
    if (instance->periodic) {
        return MP_EBUSY;
    }
    if (ble_gap_ext_adv_active(p->instance)) {
        int err = ble_gap_ext_adv_stop(p->instance);
        if (err) {
            return ble_hs_err_to_errno(err);
        }
    }
    if (!legacy && instance->configured && instance->params.scannable != p->scannable) {
        // Clear the previous mode's data while its setter is still permitted.
        // Set Advertising Data is not permitted after switching to scannable.
        bool response = instance->params.scannable;
        int err = esp32_ble5_update_data(p->instance,
            response ? &instance->resp_data : &instance->adv_data,
            response ? &instance->resp_len : &instance->adv_len,
            (const uint8_t *)"", 0, response);
        if (err) {
            return ble_hs_err_to_errno(err);
        }
    }
    struct ble_gap_ext_adv_params params = {
        .connectable = p->connectable,
        .scannable = p->scannable,
        .legacy_pdu = legacy,
        .itvl_min = interval,
        .itvl_max = interval,
        .channel_map = 7,
        .own_addr_type = nimble_address_mode,
        .primary_phy = p->primary_phy,
        .secondary_phy = p->secondary_phy,
        .tx_power = 127, // Let the controller select its transmit power.
        .sid = p->sid,
    };
    int err = ble_gap_ext_adv_configure(p->instance, &params, NULL, esp32_ble5_advertise_cb, NULL);
    if (err) {
        return ble_hs_err_to_errno(err);
    }
    instance->configured = true;
    instance->params = params;
    if (nimble_address_mode == BLE_OWN_ADDR_RANDOM || nimble_address_mode == BLE_OWN_ADDR_RPA_RANDOM_DEFAULT) {
        ble_addr_t addr = {.type = BLE_ADDR_RANDOM};
        err = ble_hs_id_copy_addr(BLE_ADDR_RANDOM, addr.val, NULL);
        if (!err) {
            err = ble_gap_ext_adv_set_addr(p->instance, &addr);
        }
        if (err) {
            return ble_hs_err_to_errno(err);
        }
    }
    if (legacy || !p->scannable) {
        err = esp32_ble5_update_data(p->instance, &instance->adv_data, &instance->adv_len, p->adv_data, p->adv_data_len, false);
    }
    if (!err && p->scannable) {
        err = esp32_ble5_update_data(p->instance, &instance->resp_data, &instance->resp_len, p->resp_data, p->resp_data_len, true);
    }
    if (!err) {
        int duration = p->timeout_ms ? (p->timeout_ms + 9) / 10 : 0;
        err = ble_gap_ext_adv_start(p->instance, duration, 0);
    }
    return ble_hs_err_to_errno(err);
}

int mp_bluetooth_gap_advertise_start(bool connectable, int32_t interval_us, const uint8_t *adv_data, size_t adv_data_len, const uint8_t *sr_data, size_t sr_data_len) {
    const esp32_ble5_adv_params_t params = {
        .instance = 0,
        .interval_us = interval_us,
        .primary_phy = BLE_HCI_LE_PHY_1M,
        .secondary_phy = BLE_HCI_LE_PHY_1M,
        .connectable = connectable,
        // Matches the shared binding's BLE_GAP_DISC_MODE_GEN, including the
        // nonconnectable but scannable case (ADV_SCAN_IND).
        .scannable = true,
        .adv_data = adv_data,
        .adv_data_len = adv_data_len,
        .resp_data = sr_data,
        .resp_data_len = sr_data_len,
    };
    return esp32_ble5_advertise_start(&params, true);
}

void mp_bluetooth_gap_advertise_stop(void) {
    if (ble_gap_ext_adv_active(0)) {
        ble_gap_ext_adv_stop(0);
    }
}

int esp32_ble5_advertise(const esp32_ble5_adv_params_t *params) {
    return esp32_ble5_advertise_start(params, false);
}

int esp32_ble5_advertise_stop(int instance_id, bool remove) {
    if (!mp_bluetooth_is_active()) {
        return MP_ENODEV;
    }
    if (!esp32_ble5_valid_instance(instance_id, false)) {
        return MP_EINVAL;
    }
    esp32_ble5_instance_t *instance = &esp32_ble5_state()->instances[instance_id];
    int err = 0;
    #if MICROPY_ESP32_BLE5_PERIODIC_ADV
    if (instance->periodic) {
        err = ble_gap_periodic_adv_stop(instance_id);
        if (!err) {
            instance->periodic = false;
        }
    }
    #endif
    if (!err && ble_gap_ext_adv_active(instance_id)) {
        err = ble_gap_ext_adv_stop(instance_id);
    }
    if (!err && remove && instance->configured) {
        err = ble_gap_ext_adv_remove(instance_id);
        if (!err) {
            if (instance->adv_data != NULL) {
                m_del(uint8_t, instance->adv_data, instance->adv_len);
            }
            if (instance->resp_data != NULL) {
                m_del(uint8_t, instance->resp_data, instance->resp_len);
            }
            memset(instance, 0, sizeof(*instance));
        }
    }
    return ble_hs_err_to_errno(err);
}

static int esp32_ble5_scan_next(esp32_ble5_state_t *state) {
    // Extended HCI scan durations are 16-bit units of 10ms. Split long legacy
    // scans into successive periods instead of truncating their duration.
    int ticks = state->scan_remaining_ms > 0 ? MIN((state->scan_remaining_ms - 1) / 10 + 1, UINT16_MAX) : 0;
    int err = ble_gap_ext_disc(nimble_address_mode, ticks, 0, 0, BLE_HCI_CONN_FILT_NO_WL, 0,
        (state->scan_phys & BLE_GAP_LE_PHY_1M_MASK) ? &state->scan_params : NULL,
        (state->scan_phys & BLE_GAP_LE_PHY_CODED_MASK) ? &state->scan_params : NULL,
        gap_scan_cb, (void *)(uintptr_t)state->scan_generation);
    if (!err) {
        state->scan_remaining_ms = MAX(0, state->scan_remaining_ms - ticks * 10);
    }
    return err;
}

static int gap_scan_cb(struct ble_gap_event *event, void *arg) {
    // Serialise state access and segment submission with Python start/stop.
    // The SDK releases its host lock before these scan callbacks.
    mp_state_thread_t *ts_orig = mp_thread_get_state();
    mp_state_thread_t ts;
    if (ts_orig == NULL) {
        mp_thread_init_state(&ts, MICROPY_PY_BLUETOOTH_SYNC_EVENT_STACK_SIZE, NULL, NULL);
        MP_THREAD_GIL_ENTER();
    }
    uint32_t generation = (uint32_t)(uintptr_t)arg;
    esp32_ble5_state_t *state = MP_STATE_PORT(bluetooth_ble5_state);
    if (mp_bluetooth_is_active() && state != NULL && state->scan_active && state->scan_generation == generation) {
        if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
            if (!(event->disc_complete.reason == 0 && state->scan_remaining_ms > 0 && esp32_ble5_scan_next(state) == 0)) {
                state->scan_active = false;
                state->scan_remaining_ms = 0;
                mp_bluetooth_gap_on_scan_complete();
            }
        } else if (event->type == BLE_GAP_EVENT_EXT_DISC) {
            const struct ble_gap_ext_disc_desc *report = &event->ext_disc;
            if (report->props & BLE_HCI_ADV_LEGACY_MASK) {
                uint8_t addr[6];
                reverse_addr_byte_order(addr, report->addr.val);
                mp_bluetooth_gap_on_scan_result(report->addr.type, addr, report->legacy_event_type, report->rssi, report->data, report->length_data);
            }
            // The legacy IRQ can cancel or replace the scan. Re-read its state.
            state = MP_STATE_PORT(bluetooth_ble5_state);
            if (mp_bluetooth_is_active() && state != NULL && state->scan_active
                && state->scan_generation == generation && state->scan_extended) {
                esp32_ble5_on_event(event);
            }
        }
    }
    if (ts_orig == NULL) {
        MP_THREAD_GIL_EXIT();
        mp_thread_set_state(ts_orig);
    }
    return 0;
}

int esp32_ble5_scan(int duration_ms, int interval_us, int window_us, bool active, int phys, bool extended) {
    if (!mp_bluetooth_is_active()) {
        return MP_ENODEV;
    }
    int supported = esp32_ble5_phy_mask() & (BLE_GAP_LE_PHY_1M_MASK | BLE_GAP_LE_PHY_CODED_MASK);
    if (phys <= 0 || (phys & ~supported) || duration_ms < -1) {
        return MP_EINVAL;
    }
    if (extended && (interval_us < BLE_HCI_SCAN_ITVL_MIN * BLE_HCI_SCAN_ITVL
        || interval_us > BLE_HCI_SCAN_ITVL_MAX_EXT * BLE_HCI_SCAN_ITVL
        || window_us < BLE_HCI_SCAN_WINDOW_MIN * BLE_HCI_SCAN_ITVL
        || window_us > BLE_HCI_SCAN_WINDOW_MAX_EXT * BLE_HCI_SCAN_ITVL
        || window_us > interval_us)) {
        return MP_EINVAL;
    }
    // Do not disturb an existing aioble scan when a second scan is requested.
    if (ble_gap_disc_active()) {
        return MP_EBUSY;
    }
    const struct ble_gap_ext_disc_params params = {
        .passive = !active,
        .itvl = MAX(BLE_HCI_SCAN_ITVL_MIN, MIN(extended ? BLE_HCI_SCAN_ITVL_MAX_EXT : BLE_HCI_SCAN_ITVL_MAX, interval_us / BLE_HCI_SCAN_ITVL)),
        .window = MAX(BLE_HCI_SCAN_WINDOW_MIN, MIN(extended ? BLE_HCI_SCAN_WINDOW_MAX_EXT : BLE_HCI_SCAN_WINDOW_MAX, window_us / BLE_HCI_SCAN_ITVL)),
    };
    if (params.window > params.itvl) {
        return MP_EINVAL;
    }
    esp32_ble5_state_t *state = esp32_ble5_state();
    state->scan_generation = ++esp32_ble5_scan_generation;
    state->scan_active = true;
    state->scan_extended = extended;
    state->scan_remaining_ms = MAX(0, duration_ms);
    state->scan_phys = phys;
    state->scan_params = params;
    int err = esp32_ble5_scan_next(state);
    if (err) {
        state->scan_active = false;
        state->scan_remaining_ms = 0;
    }
    return ble_hs_err_to_errno(err);
}

int mp_bluetooth_gap_scan_start(int32_t duration_ms, int32_t interval_us, int32_t window_us, bool active_scan) {
    return esp32_ble5_scan(duration_ms, interval_us, window_us, active_scan, BLE_GAP_LE_PHY_1M_MASK, false);
}

int mp_bluetooth_gap_scan_stop(void) {
    if (!mp_bluetooth_is_active()) {
        return MP_ENODEV;
    }
    esp32_ble5_state_t *state = MP_STATE_PORT(bluetooth_ble5_state);
    int err = ble_gap_disc_active() ? ble_gap_disc_cancel() : 0;
    // A timeout may clear the SDK state between the active check and cancel.
    if (err == BLE_HS_EALREADY) {
        err = 0;
    }
    if (!err && state != NULL && state->scan_active) {
        state->scan_active = false;
        state->scan_remaining_ms = 0;
        mp_bluetooth_gap_on_scan_complete();
    }
    return ble_hs_err_to_errno(err);
}

int esp32_ble5_connect(uint8_t addr_type, const uint8_t *addr, int duration_ms, int min_interval_us, int max_interval_us, int phys) {
    if (!mp_bluetooth_is_active()) {
        return MP_ENODEV;
    }
    if (phys <= 0 || (phys & ~esp32_ble5_phy_mask()) || duration_ms < -1
        || min_interval_us < 0 || max_interval_us < 0) {
        return MP_EINVAL;
    }
    int min_interval = min_interval_us ? min_interval_us / BLE_HCI_CONN_ITVL : BLE_GAP_INITIAL_CONN_ITVL_MIN;
    int max_interval = max_interval_us ? max_interval_us / BLE_HCI_CONN_ITVL : BLE_GAP_INITIAL_CONN_ITVL_MAX;
    if (min_interval < BLE_HCI_CONN_ITVL_MIN || max_interval > BLE_HCI_CONN_ITVL_MAX || min_interval > max_interval) {
        return MP_EINVAL;
    }
    if (ble_gap_disc_active()) {
        int err = mp_bluetooth_gap_scan_stop();
        if (err) {
            return err;
        }
    }
    const struct ble_gap_conn_params params = {
        .scan_itvl = 0x0010,
        .scan_window = 0x0010,
        .itvl_min = min_interval,
        .itvl_max = max_interval,
        .latency = BLE_GAP_INITIAL_CONN_LATENCY,
        .supervision_timeout = BLE_GAP_INITIAL_SUPERVISION_TIMEOUT,
        .min_ce_len = BLE_GAP_INITIAL_CONN_MIN_CE_LEN,
        .max_ce_len = BLE_GAP_INITIAL_CONN_MAX_CE_LEN,
    };
    ble_addr_t peer = create_nimble_addr(addr_type, addr);
    ble_gap_event_fn *callback = peripheral_gap_event_cb;
    void *callback_arg = NULL;
    #if MICROPY_ESP32_BLE5_PERIODIC_ADV
    // SDK connection and periodic-sync retries share cb/cb_arg. Use identical
    // routing for both so an overlapping request cannot overwrite the handler.
    callback = esp32_ble5_gap_cb;
    callback_arg = (void *)(uintptr_t)esp32_ble5_gap_generation();
    #endif
    int err = ble_gap_ext_connect(nimble_address_mode, &peer, duration_ms, phys,
        (phys & BLE_GAP_LE_PHY_1M_MASK) ? &params : NULL,
        (phys & BLE_GAP_LE_PHY_2M_MASK) ? &params : NULL,
        (phys & BLE_GAP_LE_PHY_CODED_MASK) ? &params : NULL,
        callback, callback_arg);
    return ble_hs_err_to_errno(err);
}

int mp_bluetooth_gap_peripheral_connect(uint8_t addr_type, const uint8_t *addr, int32_t duration_ms, int32_t min_conn_interval_us, int32_t max_conn_interval_us) {
    return esp32_ble5_connect(addr_type, addr, duration_ms, min_conn_interval_us, max_conn_interval_us, BLE_GAP_LE_PHY_1M_MASK);
}

#if MICROPY_ESP32_BLE5_PERIODIC_ADV
// NimBLE can call PERIODIC_SYNC while holding the host lock, and can still
// clear the old callback after it returns. Only copy/enqueue here; enter Python
// after that SDK callback has returned, on the same host event queue.
static struct ble_npl_event esp32_ble5_sync_event;
static struct ble_gap_event esp32_ble5_sync_result;
static portMUX_TYPE esp32_ble5_sync_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t esp32_ble5_sync_generation;
static bool esp32_ble5_sync_enabled;
static bool esp32_ble5_sync_initialized;
static bool esp32_ble5_sync_busy;
static bool esp32_ble5_sync_pending;

static uint32_t esp32_ble5_gap_generation(void) {
    portENTER_CRITICAL(&esp32_ble5_sync_mux);
    uint32_t generation = esp32_ble5_sync_generation;
    portEXIT_CRITICAL(&esp32_ble5_sync_mux);
    return generation;
}

static void esp32_ble5_periodic_reset(void) {
    portENTER_CRITICAL(&esp32_ble5_sync_mux);
    ++esp32_ble5_sync_generation;
    esp32_ble5_sync_busy = false;
    esp32_ble5_sync_pending = false;
    portEXIT_CRITICAL(&esp32_ble5_sync_mux);
}

static void esp32_ble5_periodic_deliver(const struct ble_gap_event *event, uint32_t generation) {
    // Shutdown may invalidate this completion while the host task waits for
    // the GIL. Check and copy the result only after acquiring it.
    mp_state_thread_t *ts_orig = mp_thread_get_state();
    mp_state_thread_t ts;
    if (ts_orig == NULL) {
        mp_thread_init_state(&ts, MICROPY_PY_BLUETOOTH_SYNC_EVENT_STACK_SIZE, NULL, NULL);
        MP_THREAD_GIL_ENTER();
    }
    struct ble_gap_event result;
    portENTER_CRITICAL(&esp32_ble5_sync_mux);
    bool current = esp32_ble5_sync_enabled && (event == NULL || generation == esp32_ble5_sync_generation);
    bool pending = current && esp32_ble5_sync_pending;
    if (pending) {
        result = esp32_ble5_sync_result;
        esp32_ble5_sync_pending = false;
        // Reserve one slot from request submission through IRQ delivery. Free
        // it before invoking Python so immediate retries from the IRQ work.
        esp32_ble5_sync_busy = false;
    }
    portEXIT_CRITICAL(&esp32_ble5_sync_mux);
    if (pending && mp_bluetooth_is_active()) {
        esp32_ble5_on_event(&result);
    }
    // Other Python threads may disable BLE while the IRQ releases the GIL.
    // Revalidate the incoming report before passing it to Python.
    if (event != NULL) {
        portENTER_CRITICAL(&esp32_ble5_sync_mux);
        current = esp32_ble5_sync_enabled && generation == esp32_ble5_sync_generation;
        portEXIT_CRITICAL(&esp32_ble5_sync_mux);
        if (current && mp_bluetooth_is_active()) {
            esp32_ble5_on_event(event);
        }
    }
    if (ts_orig == NULL) {
        MP_THREAD_GIL_EXIT();
        mp_thread_set_state(ts_orig);
    }
}

static void esp32_ble5_periodic_dispatch(struct ble_npl_event *event) {
    esp32_ble5_periodic_deliver(NULL, 0);
}

void esp32_ble5_periodic_init(void) {
    // Called after nimble_port_init, before the host task starts.
    ble_npl_event_init(&esp32_ble5_sync_event, esp32_ble5_periodic_dispatch, NULL);
    esp32_ble5_sync_initialized = true;
    esp32_ble5_periodic_reset();
    portENTER_CRITICAL(&esp32_ble5_sync_mux);
    esp32_ble5_sync_enabled = true;
    portEXIT_CRITICAL(&esp32_ble5_sync_mux);
}

void esp32_ble5_periodic_disable(void) {
    // Invalidate queued completions before releasing the GIL for host shutdown.
    portENTER_CRITICAL(&esp32_ble5_sync_mux);
    esp32_ble5_sync_enabled = false;
    ++esp32_ble5_sync_generation;
    esp32_ble5_sync_busy = false;
    esp32_ble5_sync_pending = false;
    portEXIT_CRITICAL(&esp32_ble5_sync_mux);
}

void esp32_ble5_periodic_deinit(void) {
    // The host task has stopped; the NPL queue and allocator still exist.
    if (esp32_ble5_sync_initialized) {
        ble_npl_eventq_remove(nimble_port_get_dflt_eventq(), &esp32_ble5_sync_event);
        ble_npl_event_deinit(&esp32_ble5_sync_event);
        esp32_ble5_sync_initialized = false;
    }
}

static int esp32_ble5_gap_cb(struct ble_gap_event *event, void *arg) {
    if (event->type != BLE_GAP_EVENT_PERIODIC_SYNC && event->type != BLE_GAP_EVENT_PERIODIC_REPORT
        && event->type != BLE_GAP_EVENT_PERIODIC_SYNC_LOST) {
        return peripheral_gap_event_cb(event, NULL);
    }
    portENTER_CRITICAL(&esp32_ble5_sync_mux);
    bool current = esp32_ble5_sync_enabled && (uint32_t)(uintptr_t)arg == esp32_ble5_sync_generation;
    if (current && event->type == BLE_GAP_EVENT_PERIODIC_SYNC) {
        memset(&esp32_ble5_sync_result, 0, sizeof(esp32_ble5_sync_result));
        esp32_ble5_sync_result.type = event->type;
        esp32_ble5_sync_result.periodic_sync.status = event->periodic_sync.status;
        if (event->periodic_sync.status == 0) {
            esp32_ble5_sync_result.periodic_sync = event->periodic_sync;
        }
        esp32_ble5_sync_pending = true;
    }
    portEXIT_CRITICAL(&esp32_ble5_sync_mux);
    if (!current) {
        return 0;
    }
    if (event->type == BLE_GAP_EVENT_PERIODIC_SYNC) {
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &esp32_ble5_sync_event);
        return 0;
    }
    // Reports or sync loss can already be ahead of the deferred event in the
    // host queue. Their SDK callbacks are unlocked; deliver completion first.
    esp32_ble5_periodic_deliver(event, (uint32_t)(uintptr_t)arg);
    return 0;
}

int esp32_ble5_periodic_advertise(int instance_id, int interval_us, const uint8_t *data, size_t len) {
    if (!mp_bluetooth_is_active()) {
        return MP_ENODEV;
    }
    if (!esp32_ble5_valid_instance(instance_id, false)) {
        return MP_EINVAL;
    }
    esp32_ble5_instance_t *instance = &esp32_ble5_state()->instances[instance_id];
    if (interval_us == -1) {
        if (!instance->periodic) {
            return 0;
        }
        int err = ble_gap_periodic_adv_stop(instance_id);
        if (!err) {
            instance->periodic = false;
        }
        return ble_hs_err_to_errno(err);
    }
    if (!instance->configured || instance->params.legacy_pdu || instance->params.connectable || instance->params.scannable
        || interval_us < 7500 || interval_us / 1250 > UINT16_MAX || len > MYNEWT_VAL(BLE_EXT_ADV_MAX_SIZE)) {
        return MP_EINVAL;
    }
    if (instance->periodic) {
        return MP_EBUSY;
    }
    const struct ble_gap_periodic_adv_params params = {
        .itvl_min = interval_us / 1250,
        .itvl_max = interval_us / 1250,
    };
    int err = ble_gap_periodic_adv_configure(instance_id, &params);
    if (!err && data != NULL) {
        static const uint8_t empty = 0;
        struct os_mbuf *buffer = ble_hs_mbuf_from_flat(len ? data : &empty, len);
        if (buffer == NULL) {
            return MP_ENOMEM;
        }
        #if MYNEWT_VAL(BLE_PERIODIC_ADV_ENH)
        struct ble_gap_periodic_adv_set_data_params data_params = {0};
        err = ble_gap_periodic_adv_set_data(instance_id, buffer, &data_params);
        #else
        err = ble_gap_periodic_adv_set_data(instance_id, buffer);
        #endif
    }
    if (!err) {
        #if MYNEWT_VAL(BLE_PERIODIC_ADV_ENH)
        const struct ble_gap_periodic_adv_start_params start_params = {0};
        err = ble_gap_periodic_adv_start(instance_id, &start_params);
        #else
        err = ble_gap_periodic_adv_start(instance_id);
        #endif
        instance->periodic = err == 0;
    }
    return ble_hs_err_to_errno(err);
}

int esp32_ble5_periodic_sync(uint8_t addr_type, const uint8_t *addr, int sid, int skip, int timeout_ms) {
    if (!mp_bluetooth_is_active()) {
        return MP_ENODEV;
    }
    if (sid < 0 || sid > 15 || skip < 0 || skip > 499 || timeout_ms < 100 || timeout_ms > 163840) {
        return MP_EINVAL;
    }
    #if MYNEWT_VAL(BLE_ENABLE_CONN_REATTEMPT) && ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 5, 4)
    // IDF 5.5.0-5.5.3 clears the active sync state while a retry is in flight.
    return MP_EOPNOTSUPP;
    #endif
    #if MYNEWT_VAL(BLE_ENABLE_CONN_REATTEMPT)
    // IDF's retry path always supplies an explicit address, losing list mode.
    if (addr == NULL) {
        return MP_EOPNOTSUPP;
    }
    #endif
    const struct ble_gap_periodic_sync_params params = {
        .skip = skip,
        .sync_timeout = timeout_ms / 10,
    };
    portENTER_CRITICAL(&esp32_ble5_sync_mux);
    if (!esp32_ble5_sync_enabled || esp32_ble5_sync_busy) {
        portEXIT_CRITICAL(&esp32_ble5_sync_mux);
        return MP_EBUSY;
    }
    uint32_t generation = esp32_ble5_sync_generation;
    esp32_ble5_sync_busy = true;
    portEXIT_CRITICAL(&esp32_ble5_sync_mux);
    ble_addr_t peer;
    if (addr != NULL) {
        peer = create_nimble_addr(addr_type, addr);
    }
    int err = ble_gap_periodic_adv_sync_create(addr != NULL ? &peer : NULL, sid, &params, esp32_ble5_gap_cb, (void *)(uintptr_t)generation);
    if (err) {
        portENTER_CRITICAL(&esp32_ble5_sync_mux);
        if (generation == esp32_ble5_sync_generation) {
            esp32_ble5_sync_busy = false;
        }
        portEXIT_CRITICAL(&esp32_ble5_sync_mux);
    }
    return ble_hs_err_to_errno(err);
}

int esp32_ble5_periodic_sync_cancel(void) {
    return mp_bluetooth_is_active() ? ble_hs_err_to_errno(ble_gap_periodic_adv_sync_create_cancel()) : MP_ENODEV;
}

int esp32_ble5_periodic_sync_stop(int handle) {
    return mp_bluetooth_is_active() ? ble_hs_err_to_errno(ble_gap_periodic_adv_sync_terminate(handle)) : MP_ENODEV;
}
#endif
#endif

MP_REGISTER_ROOT_POINTER(struct _esp32_ble5_state_t *bluetooth_ble5_state);
#endif
