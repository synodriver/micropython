// Included before the shared BLE method table by make_bindings.py.
#if MICROPY_ESP32_BLE5
#include "esp_bt.h"

#if CONFIG_IDF_TARGET_ESP32C6
#define ESP32_BLE5_TX_POWER_MIN ESP_PWR_LVL_N15
#else
#define ESP32_BLE5_TX_POWER_MIN ESP_PWR_LVL_N24
#endif

static int esp32_ble5_handle(mp_obj_t value) {
    mp_int_t handle = mp_obj_get_int(value);
    if (handle < 0 || handle > UINT16_MAX) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid connection handle"));
    }
    return handle;
}

static mp_obj_t bluetooth_ble5_features(mp_obj_t self_in) {
    mp_obj_t result = mp_obj_new_dict(0);
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_phys), MP_OBJ_NEW_SMALL_INT(esp32_ble5_phy_mask()));
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_extended_advertising), mp_obj_new_bool(MICROPY_ESP32_BLE5_EXT_ADV));
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_periodic_advertising), mp_obj_new_bool(MICROPY_ESP32_BLE5_PERIODIC_ADV));
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_tx_power_set), mp_const_true);
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_tx_power_get), mp_const_true);
    #if MICROPY_ESP32_BLE5_EXT_ADV
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_advertising_instances), MP_OBJ_NEW_SMALL_INT(BLE_ADV_INSTANCES));
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_max_adv_data_len), MP_OBJ_NEW_SMALL_INT(MYNEWT_VAL(BLE_EXT_ADV_MAX_SIZE)));
    #endif
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_1(bluetooth_ble5_features_obj, bluetooth_ble5_features);

static mp_obj_t bluetooth_ble5_phy(mp_obj_t self_in, mp_obj_t handle_in) {
    uint8_t tx_phy, rx_phy;
    bluetooth_handle_errno(esp32_ble5_phy(esp32_ble5_handle(handle_in), &tx_phy, &rx_phy));
    mp_obj_t values[] = {MP_OBJ_NEW_SMALL_INT(tx_phy), MP_OBJ_NEW_SMALL_INT(rx_phy)};
    return mp_obj_new_tuple(MP_ARRAY_SIZE(values), values);
}
static MP_DEFINE_CONST_FUN_OBJ_2(bluetooth_ble5_phy_obj, bluetooth_ble5_phy);

static mp_obj_t bluetooth_ble5_set_phy(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_conn_handle, ARG_tx_phys, ARG_rx_phys, ARG_coded };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_conn_handle, MP_ARG_OBJ | MP_ARG_REQUIRED, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_tx_phys, MP_ARG_INT | MP_ARG_REQUIRED, {.u_int = 0} },
        { MP_QSTR_rx_phys, MP_ARG_INT | MP_ARG_REQUIRED, {.u_int = 0} },
        { MP_QSTR_coded, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    int handle = args[ARG_conn_handle].u_obj == mp_const_none ? -1 : esp32_ble5_handle(args[ARG_conn_handle].u_obj);
    return bluetooth_handle_errno(esp32_ble5_set_phy(handle, args[ARG_tx_phys].u_int, args[ARG_rx_phys].u_int, args[ARG_coded].u_int));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(bluetooth_ble5_set_phy_obj, 1, bluetooth_ble5_set_phy);

static mp_obj_t bluetooth_ble5_set_tx_power(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_power_type, ARG_handle, ARG_power_level };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_power_type, MP_ARG_INT | MP_ARG_REQUIRED, {.u_int = 0} },
        { MP_QSTR_handle, MP_ARG_INT | MP_ARG_REQUIRED, {.u_int = 0} },
        { MP_QSTR_power_level, MP_ARG_INT | MP_ARG_REQUIRED, {.u_int = 0} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    mp_int_t power_type = args[ARG_power_type].u_int;
    mp_int_t handle = args[ARG_handle].u_int;
    mp_int_t power_level = args[ARG_power_level].u_int;
    if (power_type < ESP_BLE_ENHANCED_PWR_TYPE_DEFAULT || power_type >= ESP_BLE_ENHANCED_PWR_TYPE_MAX
        || handle < 0 || handle > UINT16_MAX
        || power_level < ESP32_BLE5_TX_POWER_MIN || power_level > ESP_PWR_LVL_P20
        || (power_type != ESP_BLE_ENHANCED_PWR_TYPE_ADV && power_type != ESP_BLE_ENHANCED_PWR_TYPE_CONN && handle != 0)) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid TX power arguments"));
    }
    if (!mp_bluetooth_is_active()) {
        mp_raise_OSError(MP_ENODEV);
    }
    check_esp_err(esp_ble_tx_power_set_enhanced((esp_ble_enhanced_power_type_t)power_type, (uint16_t)handle, (esp_power_level_t)power_level));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(bluetooth_ble5_set_tx_power_obj, 1, bluetooth_ble5_set_tx_power);

static mp_obj_t bluetooth_ble5_get_tx_power(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_power_type, ARG_handle };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_power_type, MP_ARG_INT | MP_ARG_REQUIRED, {.u_int = 0} },
        { MP_QSTR_handle, MP_ARG_INT | MP_ARG_REQUIRED, {.u_int = 0} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    mp_int_t power_type = args[ARG_power_type].u_int;
    mp_int_t handle = args[ARG_handle].u_int;
    if (power_type < ESP_BLE_ENHANCED_PWR_TYPE_DEFAULT || power_type >= ESP_BLE_ENHANCED_PWR_TYPE_MAX
        || handle < 0 || handle > UINT16_MAX
        || (power_type != ESP_BLE_ENHANCED_PWR_TYPE_ADV && power_type != ESP_BLE_ENHANCED_PWR_TYPE_CONN && handle != 0)) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid TX power arguments"));
    }
    if (!mp_bluetooth_is_active()) {
        mp_raise_OSError(MP_ENODEV);
    }
    esp_power_level_t power_level = esp_ble_tx_power_get_enhanced(
        (esp_ble_enhanced_power_type_t)power_type, (uint16_t)handle
    );
    // Some controllers return a negative error cast to esp_power_level_t.
    // An unsigned range check rejects them regardless of enum signedness.
    if ((unsigned int)power_level - ESP32_BLE5_TX_POWER_MIN > ESP_PWR_LVL_P20 - ESP32_BLE5_TX_POWER_MIN) {
        return mp_const_none;
    }
    return MP_OBJ_NEW_SMALL_INT(power_level);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(bluetooth_ble5_get_tx_power_obj, 1, bluetooth_ble5_get_tx_power);

#if MICROPY_ESP32_BLE5_EXT_ADV
static mp_obj_t bluetooth_ble5_advertise(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_interval_us, ARG_adv_data, ARG_resp_data, ARG_instance, ARG_connectable, ARG_scannable, ARG_primary_phy, ARG_secondary_phy, ARG_sid, ARG_timeout_ms };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_interval_us, MP_ARG_OBJ | MP_ARG_REQUIRED, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_adv_data, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_resp_data, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_instance, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 1} },
        { MP_QSTR_connectable, MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = true} },
        { MP_QSTR_scannable, MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = false} },
        { MP_QSTR_primary_phy, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 1} },
        { MP_QSTR_secondary_phy, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 1} },
        { MP_QSTR_sid, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
        { MP_QSTR_timeout_ms, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    if (args[ARG_interval_us].u_obj == mp_const_none) {
        return bluetooth_handle_errno(esp32_ble5_advertise_stop(args[ARG_instance].u_int, false));
    }
    mp_buffer_info_t adv = {0}, resp = {0};
    if (args[ARG_adv_data].u_obj != mp_const_none) {
        mp_get_buffer_raise(args[ARG_adv_data].u_obj, &adv, MP_BUFFER_READ);
        if (adv.len == 0) {
            adv.buf = (void *)"";
        }
    }
    if (args[ARG_resp_data].u_obj != mp_const_none) {
        mp_get_buffer_raise(args[ARG_resp_data].u_obj, &resp, MP_BUFFER_READ);
        if (resp.len == 0) {
            resp.buf = (void *)"";
        }
    }
    const esp32_ble5_adv_params_t params = {
        .instance = args[ARG_instance].u_int,
        .interval_us = mp_obj_get_int(args[ARG_interval_us].u_obj),
        .primary_phy = args[ARG_primary_phy].u_int,
        .secondary_phy = args[ARG_secondary_phy].u_int,
        .sid = args[ARG_sid].u_int,
        .timeout_ms = args[ARG_timeout_ms].u_int,
        .connectable = args[ARG_connectable].u_bool,
        .scannable = args[ARG_scannable].u_bool,
        .adv_data = adv.buf,
        .adv_data_len = adv.len,
        .resp_data = resp.buf,
        .resp_data_len = resp.len,
    };
    return bluetooth_handle_errno(esp32_ble5_advertise(&params));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(bluetooth_ble5_advertise_obj, 1, bluetooth_ble5_advertise);

static mp_obj_t bluetooth_ble5_advertise_stop(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_instance, ARG_remove };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_instance, MP_ARG_INT, {.u_int = 1} },
        { MP_QSTR_remove, MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = false} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    return bluetooth_handle_errno(esp32_ble5_advertise_stop(args[ARG_instance].u_int, args[ARG_remove].u_bool));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(bluetooth_ble5_advertise_stop_obj, 1, bluetooth_ble5_advertise_stop);

static mp_obj_t bluetooth_ble5_scan(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_duration_ms, ARG_interval_us, ARG_window_us, ARG_active, ARG_phys };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_duration_ms, MP_ARG_OBJ, {.u_obj = MP_OBJ_NEW_SMALL_INT(0)} },
        { MP_QSTR_interval_us, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 1280000} },
        { MP_QSTR_window_us, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 11250} },
        { MP_QSTR_active, MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = false} },
        { MP_QSTR_phys, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 1} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    if (args[ARG_duration_ms].u_obj == mp_const_none) {
        return bluetooth_handle_errno(mp_bluetooth_gap_scan_stop());
    }
    return bluetooth_handle_errno(esp32_ble5_scan(mp_obj_get_int(args[ARG_duration_ms].u_obj), args[ARG_interval_us].u_int, args[ARG_window_us].u_int, args[ARG_active].u_bool, args[ARG_phys].u_int, true));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(bluetooth_ble5_scan_obj, 1, bluetooth_ble5_scan);

static mp_obj_t bluetooth_ble5_connect(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_addr_type, ARG_addr, ARG_scan_duration_ms, ARG_min_conn_interval_us, ARG_max_conn_interval_us, ARG_phys };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_addr_type, MP_ARG_INT | MP_ARG_REQUIRED, {.u_int = 0} },
        { MP_QSTR_addr, MP_ARG_OBJ | MP_ARG_REQUIRED, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_scan_duration_ms, MP_ARG_INT, {.u_int = 2000} },
        { MP_QSTR_min_conn_interval_us, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
        { MP_QSTR_max_conn_interval_us, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
        { MP_QSTR_phys, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 1} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    mp_buffer_info_t addr;
    mp_get_buffer_raise(args[ARG_addr].u_obj, &addr, MP_BUFFER_READ);
    if (addr.len != 6 || args[ARG_addr_type].u_int < 0 || args[ARG_addr_type].u_int > 3) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid address"));
    }
    return bluetooth_handle_errno(esp32_ble5_connect(args[ARG_addr_type].u_int, addr.buf, args[ARG_scan_duration_ms].u_int, args[ARG_min_conn_interval_us].u_int, args[ARG_max_conn_interval_us].u_int, args[ARG_phys].u_int));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(bluetooth_ble5_connect_obj, 1, bluetooth_ble5_connect);
#endif

#if MICROPY_ESP32_BLE5_PERIODIC_ADV
static mp_obj_t bluetooth_ble5_periodic_advertise(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_interval_us, ARG_adv_data, ARG_instance };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_interval_us, MP_ARG_OBJ | MP_ARG_REQUIRED, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_adv_data, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_instance, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 1} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    mp_buffer_info_t data = {0};
    if (args[ARG_adv_data].u_obj != mp_const_none) {
        mp_get_buffer_raise(args[ARG_adv_data].u_obj, &data, MP_BUFFER_READ);
        if (data.len == 0) {
            data.buf = (void *)"";
        }
    }
    int interval = args[ARG_interval_us].u_obj == mp_const_none ? -1 : mp_obj_get_int(args[ARG_interval_us].u_obj);
    return bluetooth_handle_errno(esp32_ble5_periodic_advertise(args[ARG_instance].u_int, interval, data.buf, data.len));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(bluetooth_ble5_periodic_advertise_obj, 1, bluetooth_ble5_periodic_advertise);

static mp_obj_t bluetooth_ble5_periodic_sync(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_addr_type, ARG_addr, ARG_sid, ARG_skip, ARG_timeout_ms };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_addr_type, MP_ARG_OBJ | MP_ARG_REQUIRED, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_addr, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_sid, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
        { MP_QSTR_skip, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
        { MP_QSTR_timeout_ms, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 10000} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    if (args[ARG_addr_type].u_obj == mp_const_none) {
        return bluetooth_handle_errno(esp32_ble5_periodic_sync_cancel());
    }
    int addr_type = mp_obj_get_int(args[ARG_addr_type].u_obj);
    if (addr_type < 0 || addr_type > 1) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid address"));
    }
    mp_buffer_info_t addr = {0};
    if (args[ARG_addr].u_obj != mp_const_none) {
        mp_get_buffer_raise(args[ARG_addr].u_obj, &addr, MP_BUFFER_READ);
        if (addr.len != 6) {
            mp_raise_ValueError(MP_ERROR_TEXT("invalid address"));
        }
    }
    return bluetooth_handle_errno(esp32_ble5_periodic_sync(addr_type, addr.buf, args[ARG_sid].u_int, args[ARG_skip].u_int, args[ARG_timeout_ms].u_int));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(bluetooth_ble5_periodic_sync_obj, 1, bluetooth_ble5_periodic_sync);

static mp_obj_t bluetooth_ble5_periodic_sync_stop(mp_obj_t self_in, mp_obj_t handle_in) {
    return bluetooth_handle_errno(esp32_ble5_periodic_sync_stop(esp32_ble5_handle(handle_in)));
}
static MP_DEFINE_CONST_FUN_OBJ_2(bluetooth_ble5_periodic_sync_stop_obj, bluetooth_ble5_periodic_sync_stop);
#endif
#endif

// BEGIN METHODS
    #if MICROPY_ESP32_BLE5
    { MP_ROM_QSTR(MP_QSTR_ble5_features), MP_ROM_PTR(&bluetooth_ble5_features_obj) },
    { MP_ROM_QSTR(MP_QSTR_gap_phy), MP_ROM_PTR(&bluetooth_ble5_phy_obj) },
    { MP_ROM_QSTR(MP_QSTR_gap_set_phy), MP_ROM_PTR(&bluetooth_ble5_set_phy_obj) },
    { MP_ROM_QSTR(MP_QSTR_gap_set_tx_power), MP_ROM_PTR(&bluetooth_ble5_set_tx_power_obj) },
    { MP_ROM_QSTR(MP_QSTR_gap_get_tx_power), MP_ROM_PTR(&bluetooth_ble5_get_tx_power_obj) },
    #if MICROPY_ESP32_BLE5_EXT_ADV
    { MP_ROM_QSTR(MP_QSTR_gap_advertise_ext), MP_ROM_PTR(&bluetooth_ble5_advertise_obj) },
    { MP_ROM_QSTR(MP_QSTR_gap_advertise_ext_stop), MP_ROM_PTR(&bluetooth_ble5_advertise_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_gap_scan_ext), MP_ROM_PTR(&bluetooth_ble5_scan_obj) },
    { MP_ROM_QSTR(MP_QSTR_gap_connect_ext), MP_ROM_PTR(&bluetooth_ble5_connect_obj) },
    #endif
    #if MICROPY_ESP32_BLE5_PERIODIC_ADV
    { MP_ROM_QSTR(MP_QSTR_gap_periodic_advertise), MP_ROM_PTR(&bluetooth_ble5_periodic_advertise_obj) },
    { MP_ROM_QSTR(MP_QSTR_gap_periodic_sync), MP_ROM_PTR(&bluetooth_ble5_periodic_sync_obj) },
    { MP_ROM_QSTR(MP_QSTR_gap_periodic_sync_stop), MP_ROM_PTR(&bluetooth_ble5_periodic_sync_stop_obj) },
    #endif
    #endif
// END METHODS
// BEGIN CONSTANTS
    #if MICROPY_ESP32_BLE5
    { MP_ROM_QSTR(MP_QSTR_PHY_1M), MP_ROM_INT(1) },
    { MP_ROM_QSTR(MP_QSTR_PHY_2M), MP_ROM_INT(2) },
    { MP_ROM_QSTR(MP_QSTR_PHY_CODED), MP_ROM_INT(3) },
    { MP_ROM_QSTR(MP_QSTR_PHY_1M_MASK), MP_ROM_INT(1) },
    { MP_ROM_QSTR(MP_QSTR_PHY_2M_MASK), MP_ROM_INT(2) },
    { MP_ROM_QSTR(MP_QSTR_PHY_CODED_MASK), MP_ROM_INT(4) },
    { MP_ROM_QSTR(MP_QSTR_TX_POWER_TYPE_DEFAULT), MP_ROM_INT(ESP_BLE_ENHANCED_PWR_TYPE_DEFAULT) },
    { MP_ROM_QSTR(MP_QSTR_TX_POWER_TYPE_ADV), MP_ROM_INT(ESP_BLE_ENHANCED_PWR_TYPE_ADV) },
    { MP_ROM_QSTR(MP_QSTR_TX_POWER_TYPE_SCAN), MP_ROM_INT(ESP_BLE_ENHANCED_PWR_TYPE_SCAN) },
    { MP_ROM_QSTR(MP_QSTR_TX_POWER_TYPE_INIT), MP_ROM_INT(ESP_BLE_ENHANCED_PWR_TYPE_INIT) },
    { MP_ROM_QSTR(MP_QSTR_TX_POWER_TYPE_CONN), MP_ROM_INT(ESP_BLE_ENHANCED_PWR_TYPE_CONN) },
    #if !CONFIG_IDF_TARGET_ESP32C6
    { MP_ROM_QSTR(MP_QSTR_TX_POWER_N24), MP_ROM_INT(ESP_PWR_LVL_N24) },
    { MP_ROM_QSTR(MP_QSTR_TX_POWER_N21), MP_ROM_INT(ESP_PWR_LVL_N21) },
    { MP_ROM_QSTR(MP_QSTR_TX_POWER_N18), MP_ROM_INT(ESP_PWR_LVL_N18) },
    #endif
    { MP_ROM_QSTR(MP_QSTR_TX_POWER_N15), MP_ROM_INT(ESP_PWR_LVL_N15) },
    { MP_ROM_QSTR(MP_QSTR_TX_POWER_N12), MP_ROM_INT(ESP_PWR_LVL_N12) },
    { MP_ROM_QSTR(MP_QSTR_TX_POWER_N9), MP_ROM_INT(ESP_PWR_LVL_N9) },
    { MP_ROM_QSTR(MP_QSTR_TX_POWER_N6), MP_ROM_INT(ESP_PWR_LVL_N6) },
    { MP_ROM_QSTR(MP_QSTR_TX_POWER_N3), MP_ROM_INT(ESP_PWR_LVL_N3) },
    { MP_ROM_QSTR(MP_QSTR_TX_POWER_N0), MP_ROM_INT(ESP_PWR_LVL_N0) },
    { MP_ROM_QSTR(MP_QSTR_TX_POWER_P3), MP_ROM_INT(ESP_PWR_LVL_P3) },
    { MP_ROM_QSTR(MP_QSTR_TX_POWER_P6), MP_ROM_INT(ESP_PWR_LVL_P6) },
    { MP_ROM_QSTR(MP_QSTR_TX_POWER_P9), MP_ROM_INT(ESP_PWR_LVL_P9) },
    { MP_ROM_QSTR(MP_QSTR_TX_POWER_P12), MP_ROM_INT(ESP_PWR_LVL_P12) },
    { MP_ROM_QSTR(MP_QSTR_TX_POWER_P15), MP_ROM_INT(ESP_PWR_LVL_P15) },
    { MP_ROM_QSTR(MP_QSTR_TX_POWER_P18), MP_ROM_INT(ESP_PWR_LVL_P18) },
    { MP_ROM_QSTR(MP_QSTR_TX_POWER_P20), MP_ROM_INT(ESP_PWR_LVL_P20) },
    { MP_ROM_QSTR(MP_QSTR_IRQ_PHY_UPDATE), MP_ROM_INT(ESP32_BLE5_IRQ_PHY_UPDATE) },
    #if MICROPY_ESP32_BLE5_EXT_ADV
    { MP_ROM_QSTR(MP_QSTR_IRQ_SCAN_RESULT_EXT), MP_ROM_INT(ESP32_BLE5_IRQ_SCAN_RESULT) },
    { MP_ROM_QSTR(MP_QSTR_IRQ_ADV_COMPLETE_EXT), MP_ROM_INT(ESP32_BLE5_IRQ_ADV_COMPLETE) },
    #endif
    #if MICROPY_ESP32_BLE5_PERIODIC_ADV
    { MP_ROM_QSTR(MP_QSTR_IRQ_PERIODIC_SYNC), MP_ROM_INT(ESP32_BLE5_IRQ_PERIODIC_SYNC) },
    { MP_ROM_QSTR(MP_QSTR_IRQ_PERIODIC_REPORT), MP_ROM_INT(ESP32_BLE5_IRQ_PERIODIC_REPORT) },
    { MP_ROM_QSTR(MP_QSTR_IRQ_PERIODIC_SYNC_LOST), MP_ROM_INT(ESP32_BLE5_IRQ_PERIODIC_SYNC_LOST) },
    #endif
    #endif
// END CONSTANTS
