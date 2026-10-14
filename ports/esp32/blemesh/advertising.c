// Private Mesh advertiser lifetime and completion. No SDK task handles escape.
#include <errno.h>
#include "blemesh.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static portMUX_TYPE adv_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t done; // Persistent: a late callback can never use freed storage.
static uintptr_t epoch;
static unsigned users;
static bool enabled, pending;
static int result, fault;

int esp32_blemesh_adv_open(void) {
    // Called only before SDK init, after the previous SDK task was destroyed.
    if (!done) {
        done = xSemaphoreCreateBinary();
        if (!done) {
            return -ENOMEM;
        }
    }
    xSemaphoreTake(done, 0);
    portENTER_CRITICAL(&adv_mux);
    configASSERT(users == 0);
    ++epoch;
    enabled = true;
    pending = false;
    result = fault = 0;
    portEXIT_CRITICAL(&adv_mux);
    return 0;
}

uintptr_t esp32_blemesh_adv_begin(void) {
    portENTER_CRITICAL(&adv_mux);
    uintptr_t token = enabled && !pending ? epoch : 0;
    if (token) {
        ++users;
        pending = true;
        result = 0;
    }
    portEXIT_CRITICAL(&adv_mux);
    return token;
}

void esp32_blemesh_adv_end(int err) {
    portENTER_CRITICAL(&adv_mux);
    if (err) {
        pending = false;
    }
    --users;
    portEXIT_CRITICAL(&adv_mux);
}

void esp32_blemesh_adv_complete(uintptr_t token, int reason) {
    portENTER_CRITICAL(&adv_mux);
    if (!enabled || token != epoch || !pending) {
        portEXIT_CRITICAL(&adv_mux);
        return;
    }
    ++users;
    result = reason == BLE_HS_ETIMEOUT ? 0 : -EIO;
    if (result) {
        fault = reason ? reason : BLE_HS_EUNKNOWN;
        enabled = false;
    }
    int error = fault;
    portEXIT_CRITICAL(&adv_mux);
    if (error) {
        esp32_blemesh_adv_failed(error);
    }
    xSemaphoreGive(done);
    portENTER_CRITICAL(&adv_mux);
    --users;
    portEXIT_CRITICAL(&adv_mux);
}

void esp32_blemesh_adv_abort(int reason) {
    // Host reset may have no active advertisement/completion callback.
    portENTER_CRITICAL(&adv_mux);
    if (!enabled) {
        portEXIT_CRITICAL(&adv_mux);
        return;
    }
    enabled = false;
    fault = reason;
    result = -EIO;
    ++users;
    portEXIT_CRITICAL(&adv_mux);
    esp32_blemesh_adv_failed(reason);
    xSemaphoreGive(done);
    portENTER_CRITICAL(&adv_mux);
    --users;
    portEXIT_CRITICAL(&adv_mux);
}

int esp32_blemesh_adv_wait(void) {
    xSemaphoreTake(done, portMAX_DELAY);
    portENTER_CRITICAL(&adv_mux);
    int err = result;
    pending = false;
    portEXIT_CRITICAL(&adv_mux);
    return err;
}

int esp32_blemesh_adv_error(void) {
    portENTER_CRITICAL(&adv_mux);
    int err = fault;
    portEXIT_CRITICAL(&adv_mux);
    return err;
}

int esp32_blemesh_adv_close(void) {
    // VM only, with GIL released. Never wait from the Host callback.
    portENTER_CRITICAL(&adv_mux);
    enabled = false;
    ++epoch;
    result = -ECANCELED;
    portEXIT_CRITICAL(&adv_mux);
    TickType_t start = xTaskGetTickCount();
    for (;;) {
        portENTER_CRITICAL(&adv_mux);
        unsigned active = users;
        portEXIT_CRITICAL(&adv_mux);
        if (!active) {
            break;
        }
        if (xTaskGetTickCount() - start >= pdMS_TO_TICKS(5000)) {
            return -ETIMEDOUT; // Keep SDK tasks alive if a start/callback has not drained.
        }
        vTaskDelay(1);
    }
    if (done) {
        xSemaphoreGive(done);
    }
    return 0;
}
