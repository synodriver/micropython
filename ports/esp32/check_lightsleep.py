"""Check automatic light sleep against IDF headers without building firmware.

Requires the same pcpp/tree-sitter dependencies as ble5/check_sdk.py.
Uses MSVC /Zs or GCC/Clang -fsyntax-only, with declarations for the MP runtime.
"""

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "ble5"))
import check_sdk
import tree_sitter_c
from tree_sitter import Language, Parser

PREFIX = r"""
typedef unsigned int uint32_t;
typedef unsigned char uint8_t;
typedef long long int64_t;
typedef unsigned int TickType_t;
typedef unsigned int mp_uint_t;
typedef int mp_int_t;
typedef unsigned long long size_t;
typedef void *mp_obj_t;
typedef int esp_err_t;
typedef int bool;
typedef bool (*skip_light_sleep_cb_t)(void);
#define true 1
#define false 0
#define NULL ((void *)0)
#define ESP_OK 0
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_INVALID_STATE 0x103
#define TU_ATTR_FAST_FUNC
#define MICROPY_HW_UART_REPL 0
#define MP_ERROR_TEXT(x) x
#define MP_THREAD_GIL_EXIT() fake_gil(0)
#define MP_THREAD_GIL_ENTER() fake_gil(1)
#define MP_SCHED_PENDING 1
#define MP_STATE_VM(x) fake_sched_state
#define MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS 1
#define MICROPY_PY_WAIT_FOR_INTERRUPT fake_wfi()
#define pdFALSE 0
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define ESP_ERROR_CHECK(x) check_esp_err(x)
#if MICROPY_PY_SOCKET_EVENTS
#define MICROPY_PY_SOCKET_EVENTS_HANDLER socket_events_handler();
#else
#define MICROPY_PY_SOCKET_EVENTS_HANDLER
#endif
static volatile bool auto_lightsleep_enabled;
static volatile bool usb_bus_active;
static volatile uint32_t last_sof_us;
int fake_sched_state;
mp_int_t mp_obj_get_int(mp_obj_t);
bool mp_obj_is_true(mp_obj_t);
void mp_raise_ValueError(const char *);
void check_esp_err(esp_err_t);
void fake_gil(int);
void fake_wfi(void);
void mp_handle_pending(int);
void socket_events_handler(void);
bool socket_events_active(void);
void vTaskDelay(TickType_t);
int esp_rom_get_cpu_ticks_per_us(void);
int64_t esp_timer_get_time(void);
uint32_t ulTaskNotifyTake(int, TickType_t);
bool usb_device_active(void);
bool usb_serial_jtag_connected(void);
esp_err_t mp_hal_prepare_auto_lightsleep(void);
esp_err_t uart_stdout_set_wakeup(bool);
void esp32_tud_event_hook_cb(uint8_t, uint32_t, bool);
"""


def declarations(parser, source, names):
    source = re.sub(r'__attribute__\s*\(\(deprecated\("[^"]*"\)\)\)', "", source)
    result = []
    for name in names:
        matches = []
        for node in check_sdk.walk(parser.parse(source.encode()).root_node):
            if node.type == "type_definition":
                declarator = node.child_by_field_name("declarator")
                if declarator is not None and declarator.text.decode() == name:
                    matches.append(node.text.decode())
        if not matches:
            matches = re.findall(r"\besp_err_t\s+" + name + r"\s*\([^;{}]*\)\s*;", source)
        if len(matches) != 1:
            raise ValueError("SDK declaration missing or ambiguous: " + name)
        result.extend(matches)
    return "\n".join(result)


def main():
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument("--idf", type=Path, required=True)
    args.add_argument("--idf-tag", help="Read SDK headers from a git tag without checking it out")
    args.add_argument("--tinyusb", type=Path, required=True, help="TinyUSB source containing src/")
    args.add_argument("--compiler", required=True)
    options = args.parse_args()
    parser = Parser(Language(tree_sitter_c.language()))
    port = Path(__file__).resolve().parent

    def sdk_header(path):
        if options.idf_tag:
            return subprocess.check_output(
                ["git", "-C", str(options.idf), "show", options.idf_tag + ":" + path]
            ).decode()
        return (options.idf / path).read_text(encoding="utf-8")

    sdk = declarations(
        parser,
        sdk_header("components/esp_pm/include/esp_pm.h"),
        ("esp_pm_config_t", "esp_pm_configure", "esp_pm_get_configuration"),
    ) + declarations(
        parser,
        sdk_header("components/esp_pm/include/esp_private/pm_impl.h"),
        ("esp_pm_register_skip_light_sleep_callback",),
    )
    uart_types = (
        "components/hal/include/hal/uart_types.h"
        if (options.idf_tag and options.idf_tag.startswith("v5."))
        or (
            not options.idf_tag
            and (options.idf / "components/hal/include/hal/uart_types.h").exists()
        )
        else "components/esp_hal_uart/include/hal/uart_types.h"
    )
    sdk += declarations(parser, sdk_header(uart_types), ("uart_port_t",))
    sdk += declarations(
        parser,
        sdk_header("components/esp_driver_uart/include/driver/uart.h"),
        ("uart_set_wakeup_threshold",),
    )
    sdk += declarations(
        parser,
        sdk_header("components/esp_hw_support/include/esp_sleep.h"),
        ("esp_sleep_source_t", "esp_sleep_enable_uart_wakeup", "esp_sleep_disable_wakeup_source"),
    )
    dcd = (options.tinyusb / "src/device/dcd.h").read_text(encoding="utf-8")
    sdk += declarations(parser, dcd, ("dcd_eventid_t",))
    functions = {
        "modmachine.c": (
            "machine_auto_lightsleep_enabled",
            "mp_machine_set_freq",
            "machine_disable_auto_lightsleep",
        ),
        "mphalport.c": (
            "mp_hal_usb_active",
            "mp_hal_prepare_auto_lightsleep",
            "mp_hal_wait_ticks",
            "mp_hal_wait_ms",
        ),
        "usb.c": ("usb_device_active", "tud_event_hook_cb"),
        "usb_serial_jtag.c": ("usb_serial_jtag_connected",),
        "uart.c": ("uart_stdout_set_wakeup",),
    }
    source = "\n".join(
        check_sdk.function(
            parser, (port / filename).read_text(encoding="utf-8").replace("IRAM_ATTR", ""), name
        )
        for filename, names in functions.items()
        for name in names
    )
    compiler_options = (
        ["/nologo", "/Zs", "/std:c11", "/TC", "/WX"]
        if Path(options.compiler).stem.lower() == "cl"
        else ["-fsyntax-only", "-std=c11", "-Werror=implicit-function-declaration"]
    )
    checks = 0
    with tempfile.TemporaryDirectory(prefix="mp-auto-sleep-types-") as tmp:
        path = Path(tmp) / "check.c"
        for chip in (
            "ESP32",
            "ESP32S2",
            "ESP32S3",
            "ESP32C2",
            "ESP32C3",
            "ESP32C5",
            "ESP32C6",
            "ESP32H2",
            "ESP32P4",
        ):
            for thread, sockets, usb, usj, uart, tickless, timer_iram in (
                (1, 1, 0, 0, 1, 1, 1),
                (1, 1, 1, 0, 0, 1, 1),
                (1, 0, 0, 1, 1, 1, 1),
                (1, 0, 0, 1, 0, 1, 0),
                (0, 0, 0, 0, 1, 1, 1),
                (0, 0, 0, 0, 0, 0, 1),
            ):
                for tick_ms in (1, 10):
                    defs = [
                        "CONFIG_IDF_TARGET_" + chip + " 1",
                        "MICROPY_PY_THREAD " + str(thread),
                        "MICROPY_PY_SOCKET_EVENTS " + str(sockets),
                        "MICROPY_HW_ENABLE_USBDEV " + str(usb),
                        "MICROPY_HW_ESP_USB_SERIAL_JTAG " + str(usj),
                        "MICROPY_HW_ENABLE_UART_REPL " + str(uart),
                        "CONFIG_FREERTOS_USE_TICKLESS_IDLE " + str(tickless),
                        "CONFIG_ESP_TIMER_IN_IRAM " + str(timer_iram),
                        "CONFIG_PM_ENABLE 1",
                        "portTICK_PERIOD_MS " + str(tick_ms),
                    ]
                    code = check_sdk.preprocess(PREFIX + sdk + source, defs)
                    path.write_text(code, encoding="utf-8")
                    result = subprocess.run(
                        [options.compiler, *compiler_options, str(path)],
                        capture_output=True,
                        check=False,
                    )
                    if result.returncode:
                        raise RuntimeError(
                            result.stdout.decode(errors="replace")
                            + result.stderr.decode(errors="replace")
                        )
                    checks += 1
            print("PASS", chip, flush=True)
    print(checks, "syntax/type checks passed; no firmware or executable built.")


if __name__ == "__main__":
    main()
