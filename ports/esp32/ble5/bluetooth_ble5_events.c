// Appended after the shared synchronous IRQ implementation.
#if MICROPY_ESP32_BLE5
void esp32_ble5_on_phy_update(const struct ble_gap_event *event) {
    mp_int_t values[] = {event->phy_updated.conn_handle, event->phy_updated.status, event->phy_updated.tx_phy, event->phy_updated.rx_phy};
    invoke_irq_handler(ESP32_BLE5_IRQ_PHY_UPDATE, values, MP_ARRAY_SIZE(values), 0, NULL, NULL, NULL, NULL, 0);
}

void esp32_ble5_on_event(const struct ble_gap_event *event) {
    #if MICROPY_ESP32_BLE5_EXT_ADV
    if (event->type == BLE_GAP_EVENT_EXT_DISC) {
        const struct ble_gap_ext_disc_desc *r = &event->ext_disc;
        mp_int_t values[] = {r->addr.type, r->props, r->prim_phy, r->sec_phy, r->sid, r->periodic_adv_itvl, r->data_status, r->rssi, r->tx_power};
        uint8_t addr[6];
        for (size_t i = 0; i < sizeof(addr); ++i) {
            addr[i] = r->addr.val[5 - i];
        }
        const uint8_t *data = r->data;
        uint16_t len = r->length_data;
        invoke_irq_handler(ESP32_BLE5_IRQ_SCAN_RESULT, values, 7, 2, addr, NULL, &data, &len, 1);
    } else if (event->type == BLE_GAP_EVENT_ADV_COMPLETE) {
        mp_int_t values[] = {event->adv_complete.instance, event->adv_complete.reason,
            event->adv_complete.reason == 0 ? event->adv_complete.conn_handle : UINT16_MAX};
        invoke_irq_handler(ESP32_BLE5_IRQ_ADV_COMPLETE, values, MP_ARRAY_SIZE(values), 0, NULL, NULL, NULL, NULL, 0);
    }
    #endif
    #if MICROPY_ESP32_BLE5_PERIODIC_ADV
    if (event->type == BLE_GAP_EVENT_PERIODIC_SYNC) {
        // Apart from status, NimBLE's fields are only valid on success.
        mp_int_t values[] = {event->periodic_sync.status, 0, 0, 0, 0, 0};
        uint8_t addr[6] = {0};
        if (event->periodic_sync.status == 0) {
            values[1] = event->periodic_sync.sync_handle;
            values[2] = event->periodic_sync.sid;
            values[3] = event->periodic_sync.per_adv_ival;
            values[4] = event->periodic_sync.adv_phy;
            values[5] = event->periodic_sync.adv_addr.type;
            for (size_t i = 0; i < sizeof(addr); ++i) {
                addr[i] = event->periodic_sync.adv_addr.val[5 - i];
            }
        }
        invoke_irq_handler(ESP32_BLE5_IRQ_PERIODIC_SYNC, values, MP_ARRAY_SIZE(values), 0, addr, NULL, NULL, NULL, 0);
    } else if (event->type == BLE_GAP_EVENT_PERIODIC_REPORT) {
        mp_int_t values[] = {event->periodic_report.sync_handle, event->periodic_report.data_status, event->periodic_report.rssi, event->periodic_report.tx_power};
        const uint8_t *data = event->periodic_report.data;
        uint16_t len = event->periodic_report.data_length;
        invoke_irq_handler(ESP32_BLE5_IRQ_PERIODIC_REPORT, values, 2, 2, NULL, NULL, &data, &len, 1);
    } else if (event->type == BLE_GAP_EVENT_PERIODIC_SYNC_LOST) {
        mp_int_t values[] = {event->periodic_sync_lost.sync_handle, event->periodic_sync_lost.reason};
        invoke_irq_handler(ESP32_BLE5_IRQ_PERIODIC_SYNC_LOST, values, MP_ARRAY_SIZE(values), 0, NULL, NULL, NULL, NULL, 0);
    }
    #endif
}
#endif
