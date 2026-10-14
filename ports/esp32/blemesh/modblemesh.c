// First-stage PB-ADV node. Protocol state belongs to ESP-BLE-MESH, not Python.
#include <string.h>
#include "py/runtime.h"
#include "py/mphal.h"
#include "py/mpthread.h"
#include "py/mperrno.h"
#include "extmod/modbluetooth.h"
#include "blemesh.h"
#include "ble5/bluetooth_ble5.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_local_data_operation_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_health_model_api.h"

enum { MESH_STOPPED, MESH_STARTING, MESH_RUNNING, MESH_STOPPING, MESH_FAILED };
enum { EVENT_READY = 1, EVENT_PROVISIONED, EVENT_RESET, EVENT_MESSAGE,
    EVENT_SENT, EVENT_STOPPED, EVENT_ERROR, EVENT_CONFIG };
#define QUEUE_LEN (16)
#define PAYLOAD_MAX (192)
typedef struct {
    uint8_t event;
    uint16_t src, dst, net_idx, app_idx, length;
    uint32_t opcode;
    int status;
    uint8_t payload[PAYLOAD_MAX];
} mesh_event_t;

static portMUX_TYPE queue_mux = portMUX_INITIALIZER_UNLOCKED;
static mesh_event_t queue[QUEUE_LEN];
static unsigned head, count, dropped;
static volatile int state, operation_error;
static bool can_deinit;
static SemaphoreHandle_t completion;
static mp_sched_node_t notify_node;
static uint8_t uuid[16];
static esp_ble_mesh_prov_t provision;
static esp_ble_mesh_cfg_srv_t config_server;
static esp_ble_mesh_health_srv_t health_server;
static const uint8_t health_test_ids[] = {0};
ESP_BLE_MESH_HEALTH_PUB_DEFINE(health_pub, 0, ROLE_NODE);
static esp_ble_mesh_model_op_t operations[3];
static esp_ble_mesh_model_t vendor[1];
static esp_ble_mesh_model_t models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&config_server),
    ESP_BLE_MESH_MODEL_HEALTH_SRV(&health_server, &health_pub),
};
static esp_ble_mesh_elem_t elements[] = {
    ESP_BLE_MESH_ELEMENT(0, models, vendor),
};
static esp_ble_mesh_comp_t composition;

void esp32_blemesh_vm_init(void) {
    // mp_init() has reset the scheduler queue. Never clear a linked node during
    // normal stop/start; only at this VM-lifetime boundary.
    memset(&notify_node, 0, sizeof(notify_node));
    MP_STATE_PORT(blemesh_handler) = MP_OBJ_NULL;
}

static void notify(mp_sched_node_t *node) {
    (void)node;
    mp_obj_t handler = MP_STATE_PORT(blemesh_handler);
    if (handler != MP_OBJ_NULL && handler != mp_const_none) {
        mp_call_function_1_protected(handler, mp_const_none);
    }
}

static void enqueue(const mesh_event_t *item) {
    portENTER_CRITICAL(&queue_mux);
    if (count == QUEUE_LEN) {
        ++dropped;
    } else {
        queue[(head + count) % QUEUE_LEN] = *item;
        ++count;
    }
    portEXIT_CRITICAL(&queue_mux);
    // Static scheduler node cannot exhaust the ordinary schedule queue.
    mp_sched_schedule_node(&notify_node, notify);
    mp_hal_wake_main_task_if_suspended();
}

void esp32_blemesh_adv_failed(int reason) {
    mesh_event_t item = {.event = EVENT_ERROR, .status = reason};
    enqueue(&item);
}

static void provisioning_cb(esp_ble_mesh_prov_cb_event_t event, esp_ble_mesh_prov_cb_param_t *param) {
    mesh_event_t item = {0};
    switch (event) {
        case ESP_BLE_MESH_PROV_REGISTER_COMP_EVT:
            operation_error = param->prov_register_comp.err_code;
            xSemaphoreGive(completion);
            return;
        case ESP_BLE_MESH_DEINIT_MESH_COMP_EVT:
            operation_error = param->deinit_mesh_comp.err_code;
            return;
        case ESP_BLE_MESH_NODE_PROV_ENABLE_COMP_EVT:
            item.status = param->node_prov_enable_comp.err_code;
            operation_error = item.status;
            item.event = item.status ? EVENT_ERROR : EVENT_READY;
            break;
        case ESP_BLE_MESH_NODE_PROV_COMPLETE_EVT:
            item.event = EVENT_PROVISIONED;
            item.src = param->node_prov_complete.addr;
            item.net_idx = param->node_prov_complete.net_idx;
            break;
        case ESP_BLE_MESH_NODE_PROV_RESET_EVT:
            item.event = EVENT_RESET;
            break;
        default:
            return;
    }
    enqueue(&item);
}

static void model_cb(esp_ble_mesh_model_cb_event_t event, esp_ble_mesh_model_cb_param_t *param) {
    mesh_event_t item = {0};
    const esp_ble_mesh_msg_ctx_t *ctx;
    if (event == ESP_BLE_MESH_MODEL_OPERATION_EVT) {
        item.event = EVENT_MESSAGE;
        item.opcode = param->model_operation.opcode;
        ctx = param->model_operation.ctx;
        item.length = param->model_operation.length;
        if (item.length > PAYLOAD_MAX) {
            item.event = EVENT_ERROR;
            item.status = MP_E2BIG;
            item.length = 0;
        } else if (item.length && !param->model_operation.msg) {
            item.event = EVENT_ERROR;
            item.status = MP_ENOMEM;
            item.length = 0;
        } else if (item.length) {
            memcpy(item.payload, param->model_operation.msg, item.length);
        }
    } else if (event == ESP_BLE_MESH_MODEL_SEND_COMP_EVT) {
        item.event = EVENT_SENT;
        item.opcode = param->model_send_comp.opcode;
        item.status = param->model_send_comp.err_code;
        // A reset/preemption can race a previously queued SDK completion.
        if (!item.status && esp32_blemesh_adv_error()) {
            item.status = MP_EIO;
        }
        ctx = param->model_send_comp.ctx;
    } else {
        return;
    }
    // SDK deep-copy allocation failure can deliver a null context.
    if (!ctx) {
        item.event = EVENT_ERROR;
        item.status = MP_ENOMEM;
        item.length = 0;
        enqueue(&item);
        return;
    }
    item.src = item.event == EVENT_SENT ? esp_ble_mesh_get_primary_element_address() : ctx->addr;
    item.dst = item.event == EVENT_SENT ? ctx->addr : ctx->recv_dst;
    item.net_idx = ctx->net_idx;
    item.app_idx = ctx->app_idx;
    enqueue(&item);
}

static void config_cb(esp_ble_mesh_cfg_server_cb_event_t event, esp_ble_mesh_cfg_server_cb_param_t *param) {
    if (event == ESP_BLE_MESH_CFG_SERVER_STATE_CHANGE_EVT) {
        mesh_event_t item = {.event = EVENT_CONFIG, .opcode = param->ctx.recv_op};
        // Never copy key material into Python events or logs.
        enqueue(&item);
    }
}

static void check_error(int err) {
    if (err) {
        mp_raise_OSError(err == ESP_ERR_NO_MEM ? MP_ENOMEM : MP_EIO);
    }
}

static void check_vm_context(void) {
    if (esp32_ble5_is_host_task()) {
        mp_raise_OSError(MP_EBUSY);
    }
}

static mp_obj_t mesh_start(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_uuid, ARG_company_id, ARG_model_id };
    static const mp_arg_t allowed[] = {
        {MP_QSTR_uuid, MP_ARG_OBJ | MP_ARG_REQUIRED, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_company_id, MP_ARG_INT | MP_ARG_REQUIRED | MP_ARG_KW_ONLY, {.u_int = 0}},
        {MP_QSTR_model_id, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 1}},
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);
    mp_buffer_info_t buf;
    mp_get_buffer_raise(args[ARG_uuid].u_obj, &buf, MP_BUFFER_READ);
    if (buf.len != sizeof(uuid) || args[ARG_company_id].u_int < 0 || args[ARG_company_id].u_int >= 0xffff
        || args[ARG_model_id].u_int < 0 || args[ARG_model_id].u_int > 0xffff) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid Mesh identity"));
    }
    check_vm_context();
    if (state != MESH_STOPPED) {
        mp_raise_OSError(MP_EBUSY);
    }
    if (!mp_bluetooth_is_active()) {
        mp_raise_OSError(MP_ENODEV);
    }
    if (!completion) {
        completion = xSemaphoreCreateBinary();
        if (!completion) {
            mp_raise_OSError(MP_ENOMEM);
        }
    }
    xSemaphoreTake(completion, 0);
    memcpy(uuid, buf.buf, sizeof(uuid));
    provision = (esp_ble_mesh_prov_t) {.uuid = uuid};
    uint16_t cid = args[ARG_company_id].u_int;
    const esp_ble_mesh_model_op_t new_operations[] = {
        ESP_BLE_MESH_MODEL_OP(ESP_BLE_MESH_MODEL_OP_3(0, cid), 0),
        ESP_BLE_MESH_MODEL_OP(ESP_BLE_MESH_MODEL_OP_3(1, cid), 0),
        ESP_BLE_MESH_MODEL_OP_END,
    };
    memcpy(operations, new_operations, sizeof(operations));
    const esp_ble_mesh_model_t new_vendor[] = {
        ESP_BLE_MESH_VENDOR_MODEL(cid, args[ARG_model_id].u_int, operations, NULL, NULL),
    };
    memcpy(vendor, new_vendor, sizeof(vendor));
    config_server = (esp_ble_mesh_cfg_srv_t) {
        .net_transmit = ESP_BLE_MESH_TRANSMIT(2, 20),
        .relay = ESP_BLE_MESH_RELAY_NOT_SUPPORTED,
        .beacon = ESP_BLE_MESH_BEACON_ENABLED,
        .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_NOT_SUPPORTED,
        .friend_state = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
        .default_ttl = 7,
    };
    memset(&health_server, 0, sizeof(health_server));
    health_server.health_test.company_id = cid;
    health_server.health_test.id_count = MP_ARRAY_SIZE(health_test_ids);
    health_server.health_test.test_ids = health_test_ids;
    const esp_ble_mesh_comp_t new_composition = {
        .cid = cid, .element_count = MP_ARRAY_SIZE(elements), .elements = elements,
    };
    memcpy(&composition, &new_composition, sizeof(composition));
    portENTER_CRITICAL(&queue_mux);
    head = count = dropped = 0;
    portEXIT_CRITICAL(&queue_mux);
    if (esp32_blemesh_adv_open()) {
        mp_raise_OSError(MP_ENOMEM);
    }
    state = MESH_STARTING;
    can_deinit = false;
    operation_error = 0;
    esp32_blemesh_adapter_reset();
    esp_ble_mesh_register_prov_callback(provisioning_cb);
    esp_ble_mesh_register_custom_model_callback(model_cb);
    esp_ble_mesh_register_config_server_callback(config_cb);
    // SDK waits for its BTC worker. No Python code runs on that worker.
    MP_THREAD_GIL_EXIT();
    int err = esp_ble_mesh_init(&provision, &composition);
    if (!err) {
        if (xSemaphoreTake(completion, pdMS_TO_TICKS(5000)) != pdTRUE) {
            err = ESP_ERR_TIMEOUT;
        } else {
            err = operation_error;
        }
    }
    MP_THREAD_GIL_ENTER();
    if (err) {
        MP_THREAD_GIL_EXIT();
        esp32_blemesh_adv_close();
        MP_THREAD_GIL_ENTER();
        state = MESH_FAILED;
        operation_error = err;
        check_error(err);
    }
    can_deinit = true;
    if (esp32_blemesh_adv_error()) {
        state = MESH_FAILED;
        check_error(esp32_blemesh_adv_error());
    }
    state = MESH_RUNNING;
    if (!esp_ble_mesh_node_is_provisioned()) {
        err = esp_ble_mesh_node_prov_enable(ESP_BLE_MESH_PROV_ADV);
        if (err) {
            operation_error = err;
            check_error(err);
        }
    } else {
        mesh_event_t item = {.event = EVENT_READY};
        enqueue(&item);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(mesh_start_obj, 1, mesh_start);

static int stop(bool erase) {
    if (state == MESH_STOPPED) {
        return 0;
    }
    if (state == MESH_STARTING || state == MESH_STOPPING) {
        return MP_EBUSY;
    }
    // IDF cannot unwind a partially failed init, and deinit tears down BTC
    // even when its completion reports failure. Do not call into a missing
    // BTC worker on a retry: keep FAILED and require a hardware reset.
    if (!can_deinit) {
        return MP_EIO;
    }
    state = MESH_STOPPING;
    esp_ble_mesh_deinit_param_t param = {.erase_flash = erase};
    operation_error = 0;
    MP_THREAD_GIL_EXIT();
    int err = esp32_blemesh_adv_close();
    if (!err) {
        can_deinit = false;
        err = esp_ble_mesh_deinit(&param);
    }
    MP_THREAD_GIL_ENTER();
    if (err || operation_error) {
        if (err) {
            operation_error = err;
        }
        state = MESH_FAILED;
        return MP_EIO;
    }
    // Remove only the private controller instance. Ordinary/periodic BLE5
    // advertisers and all application GATT handles remain untouched.
    ble_gap_ext_adv_stop(ESP32_BLE_MESH_ADV_INSTANCE);
    err = ble_gap_ext_adv_remove(ESP32_BLE_MESH_ADV_INSTANCE);
    if (err && err != BLE_HS_EALREADY) {
        operation_error = err;
        state = MESH_FAILED;
        return MP_EIO;
    }
    esp32_blemesh_adapter_reset();
    state = MESH_STOPPED;
    mesh_event_t item = {.event = EVENT_STOPPED};
    enqueue(&item);
    return 0;
}

int esp32_blemesh_shutdown(void) {
    int err = stop(false);
    if (!err) {
        MP_STATE_PORT(blemesh_handler) = MP_OBJ_NULL;
    }
    return err;
}

static mp_obj_t mesh_stop(void) {
    check_vm_context();
    int err = stop(false);
    if (err) {
        mp_raise_OSError(err);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mesh_stop_obj, mesh_stop);

static mp_obj_t mesh_reset(void) {
    check_vm_context();
    if (state != MESH_RUNNING || esp32_blemesh_adv_error()) {
        mp_raise_OSError(MP_ENODEV);
    }
    int err = stop(true);
    if (err) {
        mp_raise_OSError(err);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mesh_reset_obj, mesh_reset);

static mp_obj_t mesh_status(void) {
    mp_obj_t result = mp_obj_new_dict(0);
    int adv_error = state == MESH_STOPPED ? 0 : esp32_blemesh_adv_error();
    int visible_state = state == MESH_RUNNING && adv_error ? MESH_FAILED : state;
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_state), MP_OBJ_NEW_SMALL_INT(visible_state));
    bool running = visible_state == MESH_RUNNING;
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_provisioned), mp_obj_new_bool(running && esp_ble_mesh_node_is_provisioned()));
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_address), MP_OBJ_NEW_SMALL_INT(running ? esp_ble_mesh_get_primary_element_address() : 0));
    portENTER_CRITICAL(&queue_mux);
    unsigned lost = dropped;
    portEXIT_CRITICAL(&queue_mux);
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_dropped), mp_obj_new_int_from_uint(lost));
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_error), mp_obj_new_int(adv_error ? adv_error : operation_error));
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_radio_error), mp_obj_new_int(esp32_blemesh_radio_error()));
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mesh_status_obj, mesh_status);

static mp_obj_t mesh_irq(mp_obj_t handler) {
    if (handler != mp_const_none && !mp_obj_is_callable(handler)) {
        mp_raise_TypeError(MP_ERROR_TEXT("handler must be callable"));
    }
    MP_STATE_PORT(blemesh_handler) = handler;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mesh_irq_obj, mesh_irq);

static mp_obj_t mesh_recv(void) {
    mesh_event_t item;
    portENTER_CRITICAL(&queue_mux);
    bool available = count != 0;
    if (available) {
        item = queue[head];
        head = (head + 1) % QUEUE_LEN;
        --count;
    }
    portEXIT_CRITICAL(&queue_mux);
    if (!available) {
        return mp_const_none;
    }
    mp_obj_t values[] = {
        MP_OBJ_NEW_SMALL_INT(item.event), MP_OBJ_NEW_SMALL_INT(item.src), MP_OBJ_NEW_SMALL_INT(item.dst),
        MP_OBJ_NEW_SMALL_INT(item.net_idx), MP_OBJ_NEW_SMALL_INT(item.app_idx),
        mp_obj_new_int_from_uint(item.opcode), mp_obj_new_bytes(item.payload, item.length), mp_obj_new_int(item.status),
    };
    return mp_obj_new_tuple(MP_ARRAY_SIZE(values), values);
}
static MP_DEFINE_CONST_FUN_OBJ_0(mesh_recv_obj, mesh_recv);

static mp_obj_t mesh_send(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_dst, ARG_data, ARG_net_idx, ARG_app_idx, ARG_opcode, ARG_ttl };
    static const mp_arg_t allowed[] = {
        {MP_QSTR_dst, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
        {MP_QSTR_data, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_net_idx, MP_ARG_REQUIRED | MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0}},
        {MP_QSTR_app_idx, MP_ARG_REQUIRED | MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0}},
        {MP_QSTR_opcode, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0}},
        {MP_QSTR_ttl, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 7}},
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);
    mp_buffer_info_t buf;
    mp_get_buffer_raise(args[ARG_data].u_obj, &buf, MP_BUFFER_READ);
    if (args[ARG_dst].u_int <= 0 || args[ARG_dst].u_int > 0xffff
        || (args[ARG_dst].u_int >= 0x8000 && args[ARG_dst].u_int < 0xc000)
        || args[ARG_net_idx].u_int < 0 || args[ARG_net_idx].u_int > 0xfff
        || args[ARG_app_idx].u_int < 0 || args[ARG_app_idx].u_int > 0xfff
        || args[ARG_opcode].u_int < 0 || args[ARG_opcode].u_int > 1
        || args[ARG_ttl].u_int < 0 || args[ARG_ttl].u_int == 1 || args[ARG_ttl].u_int > 127
        || buf.len > PAYLOAD_MAX) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid Mesh message"));
    }
    if (state != MESH_RUNNING || esp32_blemesh_adv_error() || !esp_ble_mesh_node_is_provisioned()) {
        mp_raise_OSError(MP_ENODEV);
    }
    esp_ble_mesh_msg_ctx_t ctx = {.net_idx = args[ARG_net_idx].u_int, .app_idx = args[ARG_app_idx].u_int,
        .addr = args[ARG_dst].u_int, .send_ttl = args[ARG_ttl].u_int};
    check_error(esp_ble_mesh_server_model_send_msg(&vendor[0], &ctx,
        ESP_BLE_MESH_MODEL_OP_3(args[ARG_opcode].u_int, composition.cid), buf.len, buf.buf));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(mesh_send_obj, 2, mesh_send);

static const mp_rom_map_elem_t globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_blemesh)},
    {MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&mesh_start_obj)},
    {MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&mesh_stop_obj)},
    {MP_ROM_QSTR(MP_QSTR_reset), MP_ROM_PTR(&mesh_reset_obj)},
    {MP_ROM_QSTR(MP_QSTR_status), MP_ROM_PTR(&mesh_status_obj)},
    {MP_ROM_QSTR(MP_QSTR_irq), MP_ROM_PTR(&mesh_irq_obj)},
    {MP_ROM_QSTR(MP_QSTR_recv), MP_ROM_PTR(&mesh_recv_obj)},
    {MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&mesh_send_obj)},
    {MP_ROM_QSTR(MP_QSTR_READY), MP_ROM_INT(EVENT_READY)},
    {MP_ROM_QSTR(MP_QSTR_PROVISIONED), MP_ROM_INT(EVENT_PROVISIONED)},
    {MP_ROM_QSTR(MP_QSTR_RESET), MP_ROM_INT(EVENT_RESET)},
    {MP_ROM_QSTR(MP_QSTR_MESSAGE), MP_ROM_INT(EVENT_MESSAGE)},
    {MP_ROM_QSTR(MP_QSTR_SENT), MP_ROM_INT(EVENT_SENT)},
    {MP_ROM_QSTR(MP_QSTR_STOPPED), MP_ROM_INT(EVENT_STOPPED)},
    {MP_ROM_QSTR(MP_QSTR_ERROR), MP_ROM_INT(EVENT_ERROR)},
    {MP_ROM_QSTR(MP_QSTR_CONFIG), MP_ROM_INT(EVENT_CONFIG)},
};
static MP_DEFINE_CONST_DICT(globals, globals_table);
const mp_obj_module_t blemesh_module = {.base = {&mp_type_module}, .globals = (mp_obj_dict_t *)&globals};
MP_REGISTER_MODULE(MP_QSTR_blemesh, blemesh_module);
MP_REGISTER_ROOT_POINTER(mp_obj_t blemesh_handler);
