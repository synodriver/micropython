/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 MicroPython contributors
 */

#include <string.h>
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_idf_version.h"
#include "soc/gpio_periph.h"
#include "soc/gpio_struct.h"
#include "soc/io_mux_reg.h"
#if CONFIG_IDF_TARGET_ESP32C5
#include "soc/io_mux_struct.h"
#endif
#include "py/runtime.h"
#include "py/mphal.h"
#include "machine_rtc.h"

// Wake resources owned by this port. User configuration remains in
// machine_rtc_config; this structure describes the currently installed SDK
// state and is the only state machine_sleep_clear() is allowed to remove.
typedef struct {
    uint32_t sources;
    uint64_t gpio_pins;
    uint64_t deep_gpio_pins;
    uint64_t rtc_pins;
    uint64_t ext1_pins;
    uint64_t temporary_pins;
} machine_sleep_resources_t;

static machine_sleep_resources_t sleep_resources;
static gpio_int_type_t saved_irq_type[GPIO_NUM_MAX];
static uint32_t saved_sleep_config[GPIO_NUM_MAX];
static gpio_int_type_t installed_irq_type;

static uint64_t machine_sleep_rtc_pins(const machine_rtc_config_t *config, wake_type_t wake_type) {
    uint64_t pins = 0;
    #if SOC_PM_SUPPORT_EXT0_WAKEUP
    if (config->ext0_pin != -1 && (config->ext0_wake_types & wake_type)) {
        pins |= 1ULL << config->ext0_pin;
    }
    #endif
    #if SOC_PM_SUPPORT_EXT1_WAKEUP
    pins |= config->ext1_pins;
    #endif
    return pins;
}

#if CONFIG_PM_ENABLE
static esp_pm_lock_handle_t config_lock;
#endif

esp_err_t machine_sleep_lock(void) {
    #if CONFIG_PM_ENABLE
    if (config_lock == NULL) {
        esp_err_t err = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "mp_wakeup", &config_lock);
        if (err != ESP_OK) {
            return err;
        }
    }
    return esp_pm_lock_acquire(config_lock);
    #else
    return ESP_OK;
    #endif
}

void machine_sleep_unlock(void) {
    #if CONFIG_PM_ENABLE
    ESP_ERROR_CHECK(esp_pm_lock_release(config_lock));
    #endif
}

void machine_sleep_restore_pins(void) {
    #if SOC_RTCIO_PIN_COUNT > 0
    for (int pin = 0; pin < GPIO_NUM_MAX; ++pin) {
        if (sleep_resources.rtc_pins & (1ULL << pin)) {
            #if SOC_RTCIO_HOLD_SUPPORTED
            rtc_gpio_hold_dis(pin);
            #endif
            rtc_gpio_deinit(pin);
        }
    }
    #endif
}

#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
static esp_err_t machine_sleep_enter(int64_t expected_us, void *arg) {
    (void)expected_us;
    (void)arg;
    if (machine_auto_lightsleep_enabled()) {
        machine_pin_irq_sleep_enter();
    }
    return ESP_OK;
}

// IDF invokes this after esp_light_sleep_start() has restored the cache. The
// callback runs in the idle task, makes no Python calls and does not block.
static esp_err_t machine_sleep_exit(int64_t slept_us, void *arg) {
    (void)arg;
    if (slept_us > 0) {
        machine_sleep_restore_pins();
    }
    // Entry callbacks also run when IDF subsequently skips/rejects sleep.
    // Always undo the temporary GPIO interrupt/wakeup configuration.
    machine_pin_irq_sleep_exit();
    return ESP_OK;
}
#endif

uint32_t machine_sleep_gpio_config(int pin) {
    #if CONFIG_IDF_TARGET_ESP32C5
    // IDF 5.5.5 declares but does not define GPIO_PIN_MUX_REG for C5.
    return (IO_MUX.gpio[pin].mcu_ie ? SLP_IE : 0)
        | (IO_MUX.gpio[pin].mcu_oe ? SLP_OE : 0)
        | (IO_MUX.gpio[pin].slp_sel ? SLP_SEL : 0);
    #else
    return REG_READ(GPIO_PIN_MUX_REG[pin]) & (SLP_IE | SLP_OE | SLP_SEL);
    #endif
}

static esp_err_t machine_sleep_irq_validate(const machine_rtc_config_t *config, uint64_t pins) {
    if (!pins) {
        return ESP_OK;
    }
    #if !CONFIG_PM_LIGHT_SLEEP_CALLBACKS
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
    #else
    uint64_t explicit_pins = config->gpio_pins | machine_sleep_rtc_pins(config, MACHINE_WAKE_SLEEP);
    if (pins & explicit_pins) {
        return ESP_ERR_INVALID_ARG;
    }
    #if CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP
    #if SOC_PM_SUPPORT_EXT1_WAKEUP
    for (int pin = 0; pin < GPIO_NUM_MAX; ++pin) {
        if ((pins & (1ULL << pin)) && !esp_sleep_is_valid_wakeup_gpio(pin)) {
            return ESP_ERR_NOT_SUPPORTED;
        }
    }
    #if !SOC_PM_SUPPORT_EXT1_WAKEUP_MODE_PER_PIN
    // The wake level follows each input's sampled state (including release).
    // A shared polarity cannot represent arbitrary future input combinations.
    // ALL_LOW on ESP32 must not become an AND of independent IRQ inputs.
    if ((pins & (pins - 1)) || config->ext1_pins) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    #endif
    #else
    return ESP_ERR_NOT_SUPPORTED;
    #endif
    #endif
    return ESP_OK;
    #endif
}

static esp_err_t machine_sleep_irq_source(const machine_rtc_config_t *config, uint64_t pins) {
    esp_err_t err = ESP_OK;
    #if CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP
    // EXT1 is armed with the freshly sampled levels in the entry callback.
    pins = 0;
    #endif
    if (pins || config->gpio_pins) {
        err = esp_sleep_enable_gpio_wakeup();
        if (err == ESP_OK) {
            sleep_resources.sources |= 1U << ESP_SLEEP_WAKEUP_GPIO;
        }
    } else if (sleep_resources.sources & (1U << ESP_SLEEP_WAKEUP_GPIO)) {
        err = esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
        if (err == ESP_ERR_INVALID_STATE) {
            err = ESP_OK;
        }
        if (err == ESP_OK) {
            sleep_resources.sources &= ~(1U << ESP_SLEEP_WAKEUP_GPIO);
        }
    }
    return err;
}

esp_err_t machine_sleep_irq_update(uint64_t pins) {
    esp_err_t err = machine_sleep_irq_validate(&machine_rtc_config, pins);
    return err == ESP_OK ? machine_sleep_irq_source(&machine_rtc_config, pins) : err;
}

void machine_sleep_irq_prepare(int pin, bool wake_high) {
    #if CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP && SOC_PM_SUPPORT_EXT1_WAKEUP
    #if CONFIG_IDF_TARGET_ESP32
    esp_sleep_ext1_wakeup_mode_t low = ESP_EXT1_WAKEUP_ALL_LOW;
    #else
    esp_sleep_ext1_wakeup_mode_t low = ESP_EXT1_WAKEUP_ANY_LOW;
    #endif
    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(1ULL << pin,
        wake_high ? ESP_EXT1_WAKEUP_ANY_HIGH : low));
    #else
    ESP_ERROR_CHECK(gpio_wakeup_enable(pin, wake_high ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL));
    #endif
    sleep_resources.temporary_pins |= 1ULL << pin;
}

void machine_sleep_irq_finish(int pin) {
    if (!(sleep_resources.temporary_pins & (1ULL << pin))) {
        return;
    }
    #if CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP && SOC_PM_SUPPORT_EXT1_WAKEUP
    // Undo even a rejected/skipped sleep; restore the digital mux before
    // machine_pin reads the input and re-enables its CPU interrupt.
    #if SOC_RTCIO_HOLD_SUPPORTED
    ESP_ERROR_CHECK(rtc_gpio_hold_dis(pin));
    #endif
    ESP_ERROR_CHECK(rtc_gpio_deinit(pin));
    ESP_ERROR_CHECK(esp_sleep_disable_ext1_wakeup_io(1ULL << pin));
    #else
    ESP_ERROR_CHECK(gpio_wakeup_disable(pin));
    #endif
    sleep_resources.temporary_pins &= ~(1ULL << pin);
}

void machine_sleep_clear(void) {
    // Restore per-attempt pin state through its owner, including CPU IRQ and
    // sleep selection, before removing the persistent wake sources. The PM
    // configuration lock serialises this with entry/exit callbacks.
    machine_pin_irq_sleep_exit();
    // Also undo per-pin wake enables: disabling the global GPIO source alone
    // leaves them armed, causing removed pins to wake a subsequent sleep.
    for (int pin = 0; pin < GPIO_NUM_MAX; ++pin) {
        if (sleep_resources.gpio_pins & (1ULL << pin)) {
            ESP_ERROR_CHECK(gpio_wakeup_disable(pin));
            // The SDK wake APIs change sleep selection, and installation changes
            // sleep direction. Undo both, including after a failed installation.
            gpio_mode_t direction = GPIO_MODE_DISABLE;
            if (saved_sleep_config[pin] & SLP_IE) {
                direction |= GPIO_MODE_INPUT;
            }
            if (saved_sleep_config[pin] & SLP_OE) {
                direction |= GPIO_MODE_OUTPUT;
            }
            ESP_ERROR_CHECK(gpio_sleep_set_direction(pin, direction));
            ESP_ERROR_CHECK(saved_sleep_config[pin] & SLP_SEL
                ? gpio_sleep_sel_en(pin) : gpio_sleep_sel_dis(pin));
            // Respect an IRQ subsequently reconfigured by the application.
            if (GPIO.pin[pin].int_type == installed_irq_type) {
                ESP_ERROR_CHECK(gpio_set_intr_type(pin, saved_irq_type[pin]));
            }
        }
        #if SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP
        if (sleep_resources.deep_gpio_pins & (1ULL << pin)) {
            #if ESP_IDF_VERSION_MAJOR >= 6
            ESP_ERROR_CHECK(gpio_wakeup_disable_on_hp_periph_powerdown_sleep(pin));
            #else
            ESP_ERROR_CHECK(gpio_deep_sleep_wakeup_disable(pin));
            #endif
        }
        #endif
    }
    #if SOC_PM_SUPPORT_EXT1_WAKEUP
    if (sleep_resources.ext1_pins) {
        ESP_ERROR_CHECK(esp_sleep_disable_ext1_wakeup_io(sleep_resources.ext1_pins));
        sleep_resources.sources &= ~(1U << ESP_SLEEP_WAKEUP_EXT1);
    }
    #endif
    for (int source = 0; source < 32; ++source) {
        if (sleep_resources.sources & (1U << source)) {
            // Manual sleep may already have disabled all SDK wakeup sources.
            esp_err_t err = esp_sleep_disable_wakeup_source(source);
            if (err != ESP_ERR_INVALID_STATE) {
                ESP_ERROR_CHECK(err);
            }
        }
    }
    memset(&sleep_resources, 0, sizeof(sleep_resources));
}

static esp_err_t machine_sleep_install(const machine_rtc_config_t *config, wake_type_t wake_type) {
    esp_err_t err;
    #if SOC_PM_SUPPORT_EXT0_WAKEUP
    if (config->ext0_pin != -1 && (config->ext0_wake_types & wake_type)) {
        err = esp_sleep_enable_ext0_wakeup(config->ext0_pin, config->ext0_level);
        if (err != ESP_OK) {
            return err;
        }
        sleep_resources.sources |= 1U << ESP_SLEEP_WAKEUP_EXT0;
        sleep_resources.rtc_pins |= 1ULL << config->ext0_pin;
    }
    #endif
    #if SOC_PM_SUPPORT_EXT1_WAKEUP
    if (config->ext1_pins) {
        #if CONFIG_IDF_TARGET_ESP32
        esp_sleep_ext1_wakeup_mode_t low = ESP_EXT1_WAKEUP_ALL_LOW;
        #else
        esp_sleep_ext1_wakeup_mode_t low = ESP_EXT1_WAKEUP_ANY_LOW;
        #endif
        err = esp_sleep_enable_ext1_wakeup_io(config->ext1_pins,
            config->ext1_level ? ESP_EXT1_WAKEUP_ANY_HIGH : low);
        if (err != ESP_OK) {
            return err;
        }
        sleep_resources.sources |= 1U << ESP_SLEEP_WAKEUP_EXT1;
        sleep_resources.rtc_pins |= config->ext1_pins;
        sleep_resources.ext1_pins = config->ext1_pins;
    }
    #endif
    #if SOC_TOUCH_SENSOR_SUPPORTED
    if (config->wake_on_touch) {
        err = esp_sleep_enable_touchpad_wakeup();
        if (err != ESP_OK) {
            return err;
        }
        sleep_resources.sources |= 1U << ESP_SLEEP_WAKEUP_TOUCHPAD;
    }
    #endif
    #if SOC_ULP_SUPPORTED
    if (config->wake_on_ulp) {
        err = esp_sleep_enable_ulp_wakeup();
        if (err != ESP_OK) {
            return err;
        }
        sleep_resources.sources |= 1U << ESP_SLEEP_WAKEUP_ULP;
    }
    #endif
    if (config->gpio_pins) {
        if (wake_type == MACHINE_WAKE_DEEPSLEEP) {
            #if SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP
            sleep_resources.deep_gpio_pins = config->gpio_pins;
            #if ESP_IDF_VERSION_MAJOR >= 6
            err = esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(config->gpio_pins,
                config->gpio_level ? ESP_GPIO_WAKEUP_GPIO_HIGH : ESP_GPIO_WAKEUP_GPIO_LOW);
            #else
            err = esp_deep_sleep_enable_gpio_wakeup(config->gpio_pins,
                config->gpio_level ? ESP_GPIO_WAKEUP_GPIO_HIGH : ESP_GPIO_WAKEUP_GPIO_LOW);
            #endif
            if (err != ESP_OK) {
                return err;
            }
            #else
            return ESP_ERR_NOT_SUPPORTED;
            #endif
        } else {
            #if CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP
            // This API needs the digital GPIO domain. Use EXT1 instead when
            // the build powers it down; do not silently install an inert source.
            return ESP_ERR_NOT_SUPPORTED;
            #else
            installed_irq_type = config->gpio_level ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL;
            for (int pin = 0; pin < GPIO_NUM_MAX; ++pin) {
                if (config->gpio_pins & (1ULL << pin)) {
                    saved_irq_type[pin] = GPIO.pin[pin].int_type;
                    saved_sleep_config[pin] = machine_sleep_gpio_config(pin);
                    sleep_resources.gpio_pins |= 1ULL << pin;
                    err = gpio_sleep_set_direction(pin, GPIO_MODE_INPUT);
                    if (err == ESP_OK) {
                        err = gpio_wakeup_enable(pin, installed_irq_type);
                    }
                    if (err != ESP_OK) {
                        return err;
                    }
                }
            }
            err = esp_sleep_enable_gpio_wakeup();
            if (err != ESP_OK) {
                return err;
            }
            #endif
        }
        sleep_resources.sources |= 1U << ESP_SLEEP_WAKEUP_GPIO;
    }
    return ESP_OK;
}

// Validate ownership/capabilities without changing any installed SDK state.
static esp_err_t machine_sleep_validate(const machine_rtc_config_t *config, wake_type_t wake_type, bool automatic) {
    if (automatic) {
        esp_err_t err = machine_sleep_irq_validate(config, machine_pin_irq_mask());
        if (err != ESP_OK) {
            return err;
        }
    }
    #if SOC_ULP_SUPPORTED && (CONFIG_ULP_COPROC_TYPE_RISCV || CONFIG_ULP_COPROC_TYPE_LP_CORE)
    // IDF 5.5/6.x cannot individually disable these ULP trigger bits. Automatic
    // updates must not use WAKEUP_ALL, which would also remove radio wakeups.
    if (automatic && config->wake_on_ulp) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    #endif
    // A pin cannot simultaneously use RTC muxing and digital GPIO wakeup.
    uint64_t rtc_pins = machine_sleep_rtc_pins(config, wake_type);
    if (rtc_pins & config->gpio_pins) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->gpio_pins) {
        if (wake_type == MACHINE_WAKE_DEEPSLEEP) {
            #if SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP
            for (int pin = 0; pin < GPIO_NUM_MAX; ++pin) {
                if ((config->gpio_pins & (1ULL << pin)) && !esp_sleep_is_valid_wakeup_gpio(pin)) {
                    return ESP_ERR_INVALID_ARG;
                }
            }
            #else
            return ESP_ERR_NOT_SUPPORTED;
            #endif
        } else {
            #if CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP
            return ESP_ERR_NOT_SUPPORTED;
            #endif
        }
    }
    #if !CONFIG_PM_LIGHT_SLEEP_CALLBACKS
    if (automatic && rtc_pins) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    #endif
    return ESP_OK;
}

// Call with the configuration PM lock held (or with automatic sleep disabled).
// Validation failure preserves installed resources. An installation failure
// removes partial resources, then the configuration setter restores the old set.
esp_err_t machine_sleep_apply(const machine_rtc_config_t *config, wake_type_t wake_type, bool automatic) {
    esp_err_t err = machine_sleep_validate(config, wake_type, automatic);
    if (err != ESP_OK) {
        return err;
    }
    #if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
    static bool callback_registered;
    if (automatic && !callback_registered) {
        esp_pm_sleep_cbs_register_config_t callbacks = {
            .enter_cb = machine_sleep_enter,
            .exit_cb = machine_sleep_exit,
        };
        esp_err_t err = esp_pm_light_sleep_register_cbs(&callbacks);
        if (err != ESP_OK) {
            // IDF may have installed the entry callback before allocation of
            // the exit callback failed. Do not leave a half-registered pair.
            ESP_ERROR_CHECK(esp_pm_light_sleep_unregister_cbs(&callbacks));
            return err;
        }
        callback_registered = true;
    }
    #endif
    machine_sleep_clear();
    err = machine_sleep_install(config, wake_type);
    if (err == ESP_OK && automatic && machine_pin_irq_mask()) {
        err = machine_sleep_irq_source(config, machine_pin_irq_mask());
    }
    if (err != ESP_OK) {
        machine_sleep_clear();
    }
    return err;
}

void machine_sleep_set_config(const machine_rtc_config_t *config) {
    if (machine_auto_lightsleep_enabled()) {
        check_esp_err(machine_sleep_lock());
        // Reject known conflicts without tearing down and reinstalling the
        // working configuration. Runtime SDK failures still use rollback below.
        esp_err_t validation = machine_sleep_validate(config, MACHINE_WAKE_SLEEP, true);
        if (validation != ESP_OK) {
            machine_sleep_unlock();
            check_esp_err(validation);
        }
        esp_err_t err = machine_sleep_apply(config, MACHINE_WAKE_SLEEP, true);
        if (err != ESP_OK) {
            // A previously installed configuration must remain usable on error.
            esp_err_t restore_err = machine_sleep_apply(&machine_rtc_config, MACHINE_WAKE_SLEEP, true);
            if (restore_err != ESP_OK) {
                // Another driver may have introduced a conflicting source.
                // Keep the previous config, but stop sleeping without it.
                machine_disable_auto_lightsleep();
                err = restore_err;
            }
        } else {
            machine_rtc_config = *config;
        }
        machine_sleep_unlock();
        check_esp_err(err);
    } else {
        machine_rtc_config = *config;
    }
}

void machine_sleep_deinit(void) {
    machine_sleep_clear();
    memset(&machine_rtc_config, 0, sizeof(machine_rtc_config));
    #if SOC_PM_SUPPORT_EXT0_WAKEUP
    machine_rtc_config.ext0_pin = -1;
    #endif
}

uint64_t machine_sleep_wake_pins(void) {
    // The SDK status spans multiple registers. Prevent another automatic sleep
    // from replacing them halfway through this Python-facing query.
    bool automatic = machine_auto_lightsleep_enabled();
    if (automatic) {
        check_esp_err(machine_sleep_lock());
    }
    uint64_t pins = 0;
    #if SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP && SOC_DEEP_SLEEP_SUPPORTED
    uint64_t status = esp_sleep_get_gpio_wakeup_status();
    #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 1, 0)
    // IDF 6.1 returns RTC IO indices for this source, not GPIO numbers. Do not
    // interpret the raw bitmap as GPIOs (e.g. RTC IO 0 is GPIO36 on ESP32).
    for (int bit = 0; bit < 64; ++bit) {
        if (status & (1ULL << bit)) {
            int pin = esp_sleep_wakeup_io_bit2num(bit);
            if (pin >= 0 && pin < GPIO_NUM_MAX) {
                pins |= 1ULL << pin;
            }
        }
    }
    #else
    pins = status;
    #endif
    #endif
    #if SOC_PM_SUPPORT_EXT1_WAKEUP && SOC_RTCIO_PIN_COUNT > 0
    // This API already converts to GPIO numbering. Multiple sources may wake
    // together; combine only the pin information the SDK can actually report.
    pins |= esp_sleep_get_ext1_wakeup_status();
    #endif
    if (automatic) {
        machine_sleep_unlock();
    }
    return pins;
}
