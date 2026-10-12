"""Check sleep wakeup C paths against IDF headers; never builds firmware.

Uses the dependencies and compiler options of check_lightsleep.py. The SDK
types/prototypes and GPIO register layout are real; MicroPython glue is stubbed.
"""

import argparse
import functools
import re
import subprocess
import tempfile
from pathlib import Path

import check_lightsleep
import tree_sitter_c
from check_lightsleep import check_sdk
from tree_sitter import Language, Parser

PREFIX = r"""
typedef unsigned char uint8_t;
typedef signed char int8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef long long int64_t;
typedef unsigned long long size_t;
typedef int bool;
typedef int esp_err_t;
typedef int gpio_num_t;
typedef int mp_int_t;
typedef unsigned int mp_uint_t;
typedef void *mp_obj_t;
typedef struct { size_t used; } mp_map_t;
typedef union { mp_obj_t u_obj; mp_int_t u_int; bool u_bool; } mp_arg_val_t;
typedef struct { int qstr; int flags; mp_arg_val_t defval; } mp_arg_t;
typedef struct { int unused; } machine_pin_irq_obj_t;
typedef struct { struct { void *type; } base; machine_pin_irq_obj_t irq; } machine_pin_obj_t;
typedef struct _mp_sched_node_t mp_sched_node_t;
struct _mp_sched_node_t { void (*callback)(mp_sched_node_t *); mp_sched_node_t *next; };
#define MICROPY_BEGIN_ATOMIC_SECTION() 0
#define MICROPY_END_ATOMIC_SECTION(state) ((void)(state))
#define GPIO_PIN_COUNT SOC_GPIO_PIN_COUNT
#define PIN_OBJ_PTR_FROM_IRQ_OBJ_PTR(self) ((machine_pin_obj_t *)(self))
#define MP_ARG_OBJ 1
#define MP_ARG_INT 2
#define MP_ARG_BOOL 4
#define MP_ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define MP_OBJ_NEW_SMALL_INT(x) ((mp_obj_t)(size_t)(x))
#define MP_OBJ_TO_PTR(x) (x)
#define MP_OBJ_FROM_PTR(x) ((mp_obj_t)(x))
#define MP_OBJ_NULL NULL
#define PIN_OBJ_PTR_INDEX(x) 4
#define MP_STATE_PORT(x) x
#define mp_const_none ((mp_obj_t)1)
mp_obj_t machine_pin_irq_handler[64];
static machine_pin_obj_t machine_pin_obj_table[64];
static void machine_pin_isr_handler(void *);
bool mp_sched_schedule_node(mp_sched_node_t *, void (*)(mp_sched_node_t *));
bool mp_sched_schedule(mp_obj_t, mp_obj_t);
void mp_call_function_1_protected(mp_obj_t, mp_obj_t);
void mp_hal_wake_main_task_from_isr(void);
void mp_arg_check_num(size_t, size_t, size_t, size_t, bool);
bool mp_obj_is_true(mp_obj_t);
bool mp_obj_get_int_maybe(mp_obj_t, mp_int_t *);
void mp_obj_get_array(mp_obj_t, size_t *, mp_obj_t **);
void mp_arg_parse_all(size_t, const mp_obj_t *, mp_map_t *, size_t, const mp_arg_t *, mp_arg_val_t *);
gpio_num_t machine_pin_get_id(mp_obj_t);
#define true 1
#define false 0
#define NULL ((void *)0)
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_SUPPORTED 0x106
#define MP_ERROR_TEXT(x) x
#define ESP_ERROR_CHECK(x) check_esp_err(x)
#define GPIO_NUM_MAX SOC_GPIO_PIN_COUNT
#define REG_READ(addr) (*(volatile uint32_t *)(size_t)(addr))
#define BIT(n) (1U << (n))
#define BIT0 BIT(0)
#define BIT1 BIT(1)
#define BIT2 BIT(2)
#define MICROPY_HW_RTC_USER_MEM_MAX 16
#define __attribute__(...)
void check_esp_err(esp_err_t);
void *memset(void *, int, size_t);
mp_int_t mp_obj_get_int(mp_obj_t);
void mp_raise_ValueError(const char *);
static bool auto_lightsleep_enabled;
typedef int mp_obj_type_t;
"""


def declarations(parser, source, names):
    result = []
    for name in names:
        typedefs = []
        for node in check_sdk.walk(parser.parse(source.encode()).root_node):
            if node.type == "type_definition":
                declarator = node.child_by_field_name("declarator")
                if declarator is not None and re.search(
                    r"\b" + name + r"\b", declarator.text.decode()
                ):
                    typedefs.append(node.text.decode())
        if len(typedefs) == 1:
            result.extend(typedefs)
            continue
        try:
            result.append(check_lightsleep.declarations(parser, source, (name,)))
        except ValueError:
            matches = re.findall(r"\b(?:bool|void|int)\s+" + name + r"\s*\([^;{}]*\)\s*;", source)
            if len(matches) != 1:
                raise
            result.extend(matches)
    return "\n".join(result)


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--idf", type=Path, required=True)
    cli.add_argument("--idf-tag", required=True)
    cli.add_argument("--compiler", required=True)
    args = cli.parse_args()
    port = Path(__file__).resolve().parent
    parser = Parser(Language(tree_sitter_c.language()))
    major = int(args.idf_tag.split(".")[0][1:])

    @functools.lru_cache(None)
    def header(path):
        return subprocess.check_output(
            ["git", "-C", str(args.idf), "show", args.idf_tag + ":" + path]
        ).decode()

    hal = "components/hal" if major == 5 else "components/esp_hal_gpio"
    files = {
        "gpio": header("components/esp_driver_gpio/include/driver/gpio.h"),
        "rtc": header("components/esp_driver_gpio/include/driver/rtc_io.h"),
        "types": header(hal + "/include/hal/gpio_types.h"),
        "pm": header("components/esp_pm/include/esp_pm.h"),
        "sleep": header("components/esp_hw_support/include/esp_sleep.h"),
    }
    runtime = (port / "modmachine.h").read_text() + (port / "machine_rtc.h").read_text()
    implementation = (port / "machine_sleep.c").read_text()
    implementation += check_sdk.function(
        parser, (port / "modmachine.c").read_text(), "machine_sleep_helper"
    )
    pin_source = (port / "machine_pin.c").read_text()
    implementation += (
        pin_source.split("// Automatic-sleep IRQ adapter.", 1)[1]
        .split("static const machine_pin_obj_t *machine_pin_find_named", 1)[0]
        .split("\n", 1)[1]
    )
    for name in (
        "machine_pins_init",
        "machine_pins_deinit",
        "machine_pin_isr_handler",
        "machine_pin_irq_configure",
        "machine_pin_irq",
        "machine_pin_irq_call",
        "machine_pin_irq_trigger",
    ):
        implementation += check_sdk.function(parser, pin_source, name)
    for name, condition in (
        ("esp32_wake_on_ext0", "SOC_PM_SUPPORT_EXT0_WAKEUP"),
        ("esp32_wake_on_ext1", "SOC_PM_SUPPORT_EXT1_WAKEUP"),
        ("esp32_wake_on_touch", "SOC_TOUCH_SENSOR_SUPPORTED"),
        ("esp32_wake_on_ulp", "SOC_ULP_SUPPORTED"),
        ("esp32_wake_on_gpio", "1"),
    ):
        implementation += (
            "\n#if "
            + condition
            + "\n"
            + check_sdk.function(parser, (port / "modesp32.c").read_text(), name)
            + "\n#endif\n"
        )
    qstrs = "\n".join(
        f"#define {name} {value}"
        for value, name in enumerate(sorted(set(re.findall(r"MP_QSTR_\w+", implementation))), 1)
    )
    options = (
        ["/nologo", "/Zs", "/std:c11", "/TC", "/WX", "/we4013"]
        if Path(args.compiler).stem.lower() == "cl"
        else ["-fsyntax-only", "-std=c11", "-Werror=implicit-function-declaration"]
    )
    checks = 0
    with tempfile.TemporaryDirectory(prefix="mp-wakeup-types-") as tmp:
        path = Path(tmp) / "check.c"
        cases = [
            (chip, None)
            for chip in (
                "esp32",
                "esp32s2",
                "esp32s3",
                "esp32c2",
                "esp32c3",
                "esp32c5",
                "esp32c6",
                "esp32h2",
            )
        ]
        # P4 register directories were split by hardware revision in IDF 5.5.2.
        p4_paths = (
            subprocess.check_output(
                [
                    "git",
                    "-C",
                    str(args.idf),
                    "ls-tree",
                    "-r",
                    "--name-only",
                    args.idf_tag,
                    "components/soc/esp32p4",
                ]
            )
            .decode()
            .splitlines()
        )
        for path_name in p4_paths:
            if path_name.endswith("/soc/gpio_struct.h"):
                revision = Path(path_name).parent.parent.name
                cases.append(("esp32p4", None if revision == "register" else revision))
        for chip, hw in cases:
            caps = header(f"components/soc/{chip}/include/soc/soc_caps.h")
            regdir = "register/" + (hw + "/" if hw else "")
            registers = header(f"components/soc/{chip}/{regdir}soc/gpio_struct.h")
            iomux = header(f"components/soc/{chip}/{regdir}soc/io_mux_reg.h")
            iomux_struct = (
                header(f"components/soc/{chip}/{regdir}soc/io_mux_struct.h")
                if chip == "esp32c5"
                else ""
            )
            gpio_periph = header("components/soc/include/soc/gpio_periph.h")
            # Use SDK field masks and the real declaration of the IOMUX table.
            sleep_masks = "\n".join(
                re.findall(r"^#define SLP_(?:IE|OE|SEL)\s+[^\n]+", iomux, re.MULTILINE)
            )
            if len(sleep_masks.splitlines()) != 3:
                raise ValueError(f"SDK sleep direction/selection masks missing: {chip}")
            mux_table = re.search(r"extern const uint32_t GPIO_PIN_MUX_REG\[[^;]+;", gpio_periph)
            if mux_table is None:
                raise ValueError("SDK GPIO_PIN_MUX_REG declaration missing")
            for pm, callbacks, peripheral_pd, ulp in (
                (1, 1, 0, "FSM"),
                (1, 0, 1, "FSM"),
                (0, 0, 0, "FSM"),
                (1, 1, 0, "RISCV"),
                (1, 1, 0, "LP_CORE"),
            ):
                defs = [
                    f"CONFIG_IDF_TARGET_{chip.upper()} 1",
                    f"CONFIG_PM_ENABLE {pm}",
                    f"CONFIG_PM_LIGHT_SLEEP_CALLBACKS {callbacks}",
                    f"CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP {peripheral_pd}",
                    f"ESP_IDF_VERSION_MAJOR {major}",
                    "CONFIG_FREERTOS_USE_TICKLESS_IDLE 1",
                    f"CONFIG_FREERTOS_NUMBER_OF_CORES {2 if chip in ('esp32', 'esp32s3', 'esp32p4') else 1}",
                    f"SOC_CAPS_ECO_VER {0 if hw == 'hw_ver1' else 3}",
                    f"CONFIG_ESP32P4_SELECTS_REV_LESS_V3 {int(hw == 'hw_ver1')}",
                    "__attribute__(...) ",
                    f"CONFIG_ULP_COPROC_TYPE_{ulp} 1",
                ]

                def sdk(name, symbols, caps=caps, defs=defs):
                    source = check_sdk.preprocess(caps + files[name], defs)
                    return declarations(parser, source, symbols)

                sdk_types = sdk("types", ("gpio_mode_t", "gpio_int_type_t"))
                sdk_types += sdk(
                    "pm",
                    (
                        "esp_pm_lock_type_t",
                        "esp_pm_lock_handle_t",
                        "esp_pm_lock_create",
                        "esp_pm_lock_acquire",
                        "esp_pm_lock_release",
                    ),
                )
                if callbacks:
                    sdk_types += sdk(
                        "pm",
                        (
                            "esp_pm_light_sleep_cb_t",
                            "esp_pm_sleep_cbs_register_config_t",
                            "esp_pm_light_sleep_register_cbs",
                            "esp_pm_light_sleep_unregister_cbs",
                        ),
                    )
                sdk_types += sdk(
                    "sleep",
                    (
                        "esp_sleep_source_t",
                        "esp_sleep_disable_wakeup_source",
                        "esp_sleep_is_valid_wakeup_gpio",
                        "esp_sleep_enable_gpio_wakeup",
                        "esp_sleep_enable_timer_wakeup",
                        "esp_light_sleep_start",
                        "esp_deep_sleep_start",
                    ),
                )
                sdk_types += sdk(
                    "gpio",
                    (
                        "gpio_isr_t",
                        "gpio_isr_handler_remove",
                        "gpio_isr_handler_add",
                        "gpio_install_isr_service",
                        "gpio_intr_enable",
                        "gpio_intr_disable",
                        "gpio_get_level",
                        "gpio_set_intr_type",
                        "gpio_sleep_set_direction",
                        "gpio_sleep_sel_en",
                        "gpio_sleep_sel_dis",
                        "gpio_wakeup_enable",
                        "gpio_wakeup_disable",
                    ),
                )

                # Select optional declarations using the same SOC macros as C.
                def capable(macro, caps=caps, defs=defs):
                    return (
                        check_sdk.preprocess(caps + f"\n#if {macro}\nYES\n#endif\n", defs)
                        .strip()
                        .endswith("YES")
                    )

                if capable("SOC_PM_SUPPORT_EXT0_WAKEUP"):
                    sdk_types += sdk("sleep", ("esp_sleep_enable_ext0_wakeup",))
                if capable("SOC_PM_SUPPORT_EXT1_WAKEUP"):
                    sdk_types += sdk(
                        "sleep", ("esp_sleep_ext1_wakeup_mode_t", "esp_sleep_enable_ext1_wakeup")
                    )
                if capable("SOC_RTCIO_PIN_COUNT > 0"):
                    sdk_types += sdk("rtc", ("rtc_gpio_deinit",))
                if capable("SOC_RTCIO_HOLD_SUPPORTED"):
                    sdk_types += sdk("rtc", ("rtc_gpio_hold_dis",))
                if capable("SOC_TOUCH_SENSOR_SUPPORTED"):
                    sdk_types += sdk("sleep", ("esp_sleep_enable_touchpad_wakeup",))
                if capable("SOC_ULP_SUPPORTED"):
                    sdk_types += sdk("sleep", ("esp_sleep_enable_ulp_wakeup",))
                if capable("SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP"):
                    if major >= 6:
                        sdk_types += sdk(
                            "sleep",
                            (
                                "esp_sleep_gpio_wake_up_mode_t",
                                "esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown",
                            ),
                        )
                        sdk_types += sdk(
                            "gpio", ("gpio_wakeup_disable_on_hp_periph_powerdown_sleep",)
                        )
                    else:
                        sdk_types += sdk(
                            "sleep",
                            (
                                "esp_deepsleep_gpio_wake_up_mode_t",
                                "esp_deep_sleep_enable_gpio_wakeup",
                            ),
                        )
                        sdk_types += sdk("gpio", ("gpio_deep_sleep_wakeup_disable",))
                # Sleep rejection constants are aliases in these IDF releases.
                constants = "\n#define ESP_ERR_SLEEP_REJECT ESP_ERR_INVALID_STATE\n#define ESP_ERR_SLEEP_TOO_SHORT_SLEEP_DURATION ESP_ERR_INVALID_ARG\n"
                layout = check_sdk.preprocess(caps + registers, defs)
                # Keep register subtypes as well as gpio_dev_t (IDF 6 uses both).
                layout = re.sub(r"_Static_assert\([^;]*;", "", layout)
                ll = header(f"{hal}/{chip}/include/hal/gpio_ll.h")
                for name in ("gpio_ll_get_intr_status", "gpio_ll_get_intr_status_high"):
                    signature = check_sdk.function(parser, ll, name).split("{", 1)[0]
                    layout += signature.replace("static inline ", "") + ";\n"
                iomux_layout = iomux_struct
                iomux_layout = re.sub(r"_Static_assert\([^;]*;", "", iomux_layout)
                code = check_sdk.preprocess(
                    f"{caps}\n{PREFIX}\n{sleep_masks}\n{mux_table.group()}\n{qstrs}\n{sdk_types}\n{layout}\n{iomux_layout}\n{constants}\n{runtime}\n{implementation}",
                    defs,
                )
                path.write_text(code, encoding="utf-8")
                result = subprocess.run(
                    [args.compiler, *options, str(path)], capture_output=True, check=False
                )
                if result.returncode:
                    raise RuntimeError(
                        f"{chip} pm={pm} callbacks={callbacks} peripheral_pd={peripheral_pd} ulp={ulp}\n"
                        + result.stdout.decode(errors="replace")
                        + result.stderr.decode(errors="replace")
                    )
                checks += 1
            print("PASS", chip, hw or "", flush=True)
    print(checks, "wakeup syntax/type checks passed; no firmware or executable built.")


if __name__ == "__main__":
    main()
