"""Host syntax/type check against real IDF headers; does not build firmware.

OS/VM services are declarations, SDK Mesh/GAP types and prototypes are real.
Requires pcpp, tree-sitter, tree-sitter-c (same as ble5/check_sdk.py).
"""
import argparse
import importlib.util
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from tree_sitter import Language, Parser
import tree_sitter_c

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "ble5"))
spec = importlib.util.spec_from_file_location("ble5_check", HERE.parent / "ble5/check_sdk.py")
ble5 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ble5)

DEFS = [
    "CONFIG_BLE_MESH 1", "CONFIG_BLE_MESH_NODE 1", "CONFIG_BLE_MESH_DEINIT 1",
    "CONFIG_BLE_MESH_PB_ADV 1", "CONFIG_BLE_MESH_USE_BLE_50 1", "CONFIG_BLE_MESH_HEALTH_SRV 1",
    "CONFIG_BLE_MESH_MODEL_KEY_COUNT 3", "CONFIG_BLE_MESH_MODEL_GROUP_COUNT 3",
    "CONFIG_BLE_MESH_SUBNET_COUNT 2", "CONFIG_BLE_MESH_APP_KEY_COUNT 3",
    "MYNEWT_VAL(x) MYNEWT_VAL_ ## x", "MYNEWT_VAL_BLE_EXT_ADV 1",
    "MYNEWT_VAL_BLE_PERIODIC_ADV 1", "MYNEWT_VAL_BLE_CONN_REATTEMPT 1",
    "BLE_ADV_INSTANCES 5", "__attribute__(x)", "BIT(x) (1U << (x))",
    "ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))",
]

RUNTIME = r"""
#undef NULL
#define NULL 0
#define LISTIFY(n,f,sep) f(0,0), f(1,0), f(2,0)
#define BLE_MESH_ATOMIC_DEFINE(name,n) unsigned int name[1]
typedef long long int64_t;
typedef unsigned long long uint_fast32_t;
typedef unsigned int TickType_t;
typedef void *SemaphoreHandle_t;
typedef void *TimerHandle_t;
typedef unsigned int ble_npl_time_t;
typedef int ble_npl_error_t;
typedef struct {int unused;} StaticSemaphore_t;
struct k_delayed_work {int opaque;};
struct net_buf_simple {uint8_t *data; uint16_t len; uint16_t size; uint8_t *__buf;};
#define NET_BUF_SIMPLE_DEFINE_STATIC(name,n) static uint8_t name##_data[n]; static struct net_buf_simple name = {.data=name##_data,.size=n,.__buf=name##_data}
struct bt_mesh_prov;
typedef struct {void *head; void *tail;} sys_slist_t;
typedef struct {void *next;} sys_snode_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_TIMEOUT 0x107
#define MP_EIO 5
#define MP_E2BIG 7
#define EIO 5
#define ENOMEM 12
#define ECANCELED 125
#define ETIMEDOUT 110
#define portMAX_DELAY 0xffffffffU
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define configASSERT(x) fake_assert(!!(x))
SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
int xSemaphoreTake(SemaphoreHandle_t, TickType_t);
int xSemaphoreGive(SemaphoreHandle_t);
TickType_t xTaskGetTickCount(void);
void vTaskDelay(TickType_t);
typedef struct {int opaque;} mp_sched_node_t;
bool mp_sched_schedule_node(mp_sched_node_t *, void (*)(mp_sched_node_t *));
void mp_hal_wake_main_task_if_suspended(void);
void mp_call_function_1_protected(mp_obj_t, mp_obj_t);
bool mp_obj_is_callable(mp_obj_t);
void mp_raise_TypeError(const char *);
mp_obj_t mp_obj_new_bytes(const uint8_t *, size_t);
mp_obj_t mp_obj_new_int(int);
mp_obj_t mp_obj_new_int_from_uint(unsigned int);
bool mp_bluetooth_is_active(void);
bool esp32_ble5_is_host_task(void);
#define MP_DEFINE_CONST_FUN_OBJ_0(obj,fun) const int obj=0
#define MP_ROM_PTR(x) ((const void *)(x))
#undef MP_ROM_QSTR
#define MP_ROM_QSTR(x) ((const void *)(size_t)(x))
#undef MP_ROM_INT
#define MP_ROM_INT(x) ((const void *)(size_t)(x))
typedef struct {const void *key; const void *value;} mp_rom_map_elem_t;
typedef struct {int unused;} mp_obj_dict_t;
typedef struct {const void *type;} mp_obj_base_t;
typedef struct {mp_obj_base_t base; mp_obj_dict_t *globals;} mp_obj_module_t;
const int mp_type_module;
#define MP_DEFINE_CONST_DICT(name,table) const mp_obj_dict_t name={0}
#define MP_REGISTER_MODULE(qstr,module)
"""


def check(idf, compiler):
    parser = Parser(Language(tree_sitter_c.language()))
    nimble = idf / "components/bt/host/nimble/nimble"
    host = nimble / "nimble/host/include/host"
    gap = ble5.preprocess((host / "ble_gap.h").read_text(), DEFS)
    hci = nimble / "nimble/include/nimble"
    constants = "".join(ble5.macros(p.read_text()) for p in (
        hci / "ble.h", hci / "hci_common.h", host / "ble_hs.h", host / "ble_gap.h"))
    gap_types = "".join(ble5.structure(parser, gap, n) for n in (
        "ble_gap_ext_disc_params", "ble_gap_ext_disc_desc")) + ble5.event_structure(parser, gap)
    prototypes = []
    for name in ("ble_gap_ext_disc", "ble_gap_disc_cancel", "ble_gap_disc_active",
                 "ble_gap_ext_adv_stop", "ble_gap_ext_adv_remove"):
        matches = re.findall(r"\bint\s+" + name + r"\s*\([^;{}]*\)\s*;", gap, re.S)
        if len(matches) != 1:
            raise ValueError("GAP signature changed: " + name)
        prototypes += matches
    # NPL callout representation is OS-owned. Check its callable signatures
    # from the actual inline functions instead of duplicating those prototypes.
    # Supported chips use the FreeRTOS NPL, including IDF 6.1. The esp-idf
    # directory is selected only by the separate BT_DUAL_MODE_ARCH path.
    npl_header = nimble / "porting/npl/freertos/include/nimble/nimble_npl_os.h"
    npl = npl_header.read_text()
    npl_decls = "struct ble_npl_callout {int opaque;}; struct ble_npl_event {int opaque;}; struct ble_npl_eventq;\n"
    npl_decls += "typedef void ble_npl_event_fn(struct ble_npl_event *);\n"
    for name in ("ble_npl_callout_init", "ble_npl_callout_reset", "ble_npl_callout_stop",
                 "ble_npl_callout_deinit", "ble_npl_time_get", "ble_npl_time_ms_to_ticks32"):
        func = ble5.function(parser, npl, name)
        npl_decls += func[:func.index("{")].replace("static inline", "") + ";\n"
    npl_decls += "struct ble_npl_eventq *nimble_port_get_dflt_eventq(void);\n"
    mesh = idf / "components/bt/esp_ble_mesh"
    mesh_headers = "\n".join((mesh / path).read_text(encoding="utf-8") for path in (
        "api/esp_ble_mesh_defs.h", "api/models/include/esp_ble_mesh_config_model_api.h",
        "api/models/include/esp_ble_mesh_health_model_api.h", "api/core/include/esp_ble_mesh_common_api.h",
        "api/core/include/esp_ble_mesh_provisioning_api.h", "api/core/include/esp_ble_mesh_networking_api.h",
        "api/core/include/esp_ble_mesh_local_data_operation_api.h"))
    common = ble5.COMMON.replace("void *bluetooth_nimble_root_pointers;", "void *bluetooth_nimble_root_pointers; mp_obj_t blemesh_handler;")
    prefix = common + ble5.FRONTEND + RUNTIME + constants + gap_types + "\n".join(prototypes) + npl_decls
    prefix += (HERE / "blemesh.h").read_text()
    options = ["/nologo", "/Zs", "/std:c11", "/TC", "/WX"] if Path(compiler).stem.lower() == "cl" else [
        "-fsyntax-only", "-std=c11", "-Werror=implicit-function-declaration", "-Werror=incompatible-pointer-types"]
    with tempfile.TemporaryDirectory(prefix="blemesh-check-") as directory:
        for name in ("scan.c", "modblemesh.c", "advertising.c"):
            source = (HERE / name).read_text()
            qstrs = "\n".join(f"#define {q} {i + 1}" for i, q in enumerate(sorted(set(re.findall(r"MP_QSTR_\w+", source)))))
            code = ble5.preprocess(prefix + (mesh_headers if name == "modblemesh.c" else "") + qstrs + "\n" + source, DEFS)
            # GCC permits empty SDK conditional structs; MSVC does not. These
            # unrelated structs are not used by the bindings under test.
            code = re.sub(r"(?m)^#line[^\n]*", "", code)
            code = re.sub(r"((?:struct|union)\s*(?:\w+\s*)?)\{\s*\}", r"\1 { int unused; }", code)
            path = Path(directory) / name
            path.write_text(code, encoding="utf-8")
            result = subprocess.run([compiler, *options, str(path)], capture_output=True)
            if result.returncode:
                print(result.stdout.decode(errors="replace") + result.stderr.decode(errors="replace"))
                lines = code.splitlines()
                for number in list(dict.fromkeys(re.findall(rb"\((\d+)\):", result.stdout)))[:8]:
                    number = int(number)
                    print(number, "\n".join(lines[max(0, number - 2):number + 1]))
                raise SystemExit("FAIL " + name)
            print("PASS SDK types", name)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf", type=Path, required=True)
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    check(args.idf, args.compiler)
