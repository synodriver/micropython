"""Check BLE 5 C syntax/types against NimBLE headers, without building firmware.

Requires pcpp, tree-sitter and tree-sitter-c. Runtime functions are declarations
only; SDK structures and GAP prototypes come from the specified actual headers.
MSVC uses /Zs; GCC/Clang use -fsyntax-only. No objects are produced or executed.
"""

import argparse
import io
import re
import subprocess
import tempfile
from pathlib import Path

import make_bindings
import tree_sitter_c
from pcpp import Preprocessor
from tree_sitter import Language, Parser

COMMON = """
typedef unsigned char uint8_t;
typedef signed char int8_t;
typedef unsigned short uint16_t;
typedef short int16_t;
typedef unsigned int uint32_t;
typedef int int32_t;
typedef unsigned long long uint64_t;
typedef unsigned long long size_t;
typedef unsigned long long uintptr_t;
typedef int bool;
typedef int mp_int_t;
typedef void *mp_obj_t;
#define true 1
#define false 0
#define NULL ((void *)0)
#define UINT16_MAX 65535
#define MP_ENODEV 19
#define MP_EINVAL 22
#define MP_ENOMEM 12
#define MP_EBUSY 16
#define MP_EOPNOTSUPP 95
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define MP_ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define MP_STATE_PORT(x) fake_state.x
#define m_new(t,n) ((t *)fake_alloc(sizeof(t)*(n)))
#define m_new0(t,n) ((t *)fake_alloc(sizeof(t)*(n)))
#define m_del(t,p,n) fake_free(p)
#define MP_REGISTER_ROOT_POINTER(x)
struct _esp32_ble5_state_t;
struct {
    struct _esp32_ble5_state_t *bluetooth_ble5_state;
    void *bluetooth_nimble_root_pointers;
} fake_state;
void *fake_alloc(size_t);
void fake_free(void *);
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
struct os_mbuf;
typedef struct {uint8_t type; uint8_t val[6];} ble_addr_t;
struct ble_gap_event;
typedef int ble_gap_event_fn(struct ble_gap_event *, void *);
typedef struct {int unused;} mp_state_thread_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) fake_enter_critical(m)
#define portEXIT_CRITICAL(m) fake_exit_critical(m)
#define MP_THREAD_GIL_ENTER() fake_gil_enter()
#define MP_THREAD_GIL_EXIT() fake_gil_exit()
#define MICROPY_PY_BLUETOOTH_SYNC_EVENT_STACK_SIZE 5120
void fake_enter_critical(portMUX_TYPE *);
void fake_exit_critical(portMUX_TYPE *);
void fake_gil_enter(void);
void fake_gil_exit(void);
mp_state_thread_t *mp_thread_get_state(void);
void mp_thread_set_state(mp_state_thread_t *);
void mp_thread_init_state(mp_state_thread_t *,size_t,void *,void *);
typedef void *TaskHandle_t;
typedef int esp_err_t;
TaskHandle_t xTaskGetCurrentTaskHandle(void);
void nimble_port_freertos_init(void (*)(void *));
void nimble_port_freertos_deinit(void);
#define DEBUG_printf(...) ((void)0)
#define MP_BLUETOOTH_NIMBLE_BLE_STATE_OFF 0
#define MP_BLUETOOTH_NIMBLE_BLE_STATE_ACTIVE 1
int mp_bluetooth_nimble_ble_state;
#define assert(x) fake_assert(x)
void fake_assert(int);
"""

BACKEND = """
int ble_hs_err_to_errno(int);
void reverse_addr_byte_order(uint8_t *,const uint8_t *);
ble_addr_t create_nimble_addr(uint8_t,const uint8_t *);
int central_gap_event_cb(struct ble_gap_event *,void *);
int peripheral_gap_event_cb(struct ble_gap_event *,void *);
static int gap_scan_cb(struct ble_gap_event *,void *);
static uint8_t nimble_address_mode;
bool mp_bluetooth_is_active(void);
void mp_bluetooth_gap_on_scan_complete(void);
void mp_bluetooth_gap_on_scan_result(uint8_t,const uint8_t *,uint8_t,int8_t,const uint8_t *,size_t);
int mp_bluetooth_gap_scan_stop(void);
void mp_bluetooth_gap_advertise_stop(void);
int mp_bluetooth_nimble_port_shutdown(void);
void mp_bluetooth_nimble_port_hci_deinit(void);
struct os_mbuf *ble_hs_mbuf_from_flat(const void *,uint16_t);
int ble_hs_id_copy_addr(uint8_t,uint8_t *,int *);
"""

FRONTEND = """
#define MP_OBJ_NULL ((void *)0)
#define MP_OBJ_NEW_SMALL_INT(x) ((void *)(size_t)(x))
#define MP_OBJ_NEW_QSTR(x) ((void *)(size_t)(x))
#define MP_ROM_QSTR(x) (x)
#define MP_ROM_INT(x) (x)
#define MP_ROM_NONE ((void *)0)
#define MP_ERROR_TEXT(x) (x)
#define MP_ARG_OBJ 1
#define MP_ARG_REQUIRED 2
#define MP_ARG_INT 4
#define MP_ARG_KW_ONLY 8
#define MP_ARG_BOOL 16
#define MP_DEFINE_CONST_FUN_OBJ_1(obj,fun) const int obj=0
#define MP_DEFINE_CONST_FUN_OBJ_2(obj,fun) const int obj=0
#define MP_DEFINE_CONST_FUN_OBJ_KW(obj,n,fun) const int obj=0
typedef struct {int qstr;int flags;union {mp_int_t u_int;mp_obj_t u_obj;void *u_rom_obj;bool u_bool;} defval;} mp_arg_t;
typedef union {mp_int_t u_int;mp_obj_t u_obj;bool u_bool;} mp_arg_val_t;
typedef struct {int unused;} mp_map_t;
typedef struct {void *buf;size_t len;} mp_buffer_info_t;
#define MP_BUFFER_READ 1
mp_obj_t mp_const_none;
mp_obj_t mp_const_true;
int mp_obj_get_int(mp_obj_t);
void mp_raise_ValueError(const char *);
void mp_raise_OSError(int);
void check_esp_err(esp_err_t);
mp_obj_t bluetooth_handle_errno(int);
mp_obj_t mp_obj_new_dict(size_t);
void mp_obj_dict_store(mp_obj_t,mp_obj_t,mp_obj_t);
mp_obj_t mp_obj_new_bool(bool);
mp_obj_t mp_obj_new_tuple(size_t,const mp_obj_t *);
void mp_get_buffer_raise(mp_obj_t,mp_buffer_info_t *,int);
void mp_arg_parse_all(size_t,const mp_obj_t *,mp_map_t *,size_t,const mp_arg_t *,mp_arg_val_t *);
mp_obj_t invoke_irq_handler(uint16_t,const mp_int_t *,size_t,size_t,const uint8_t *,const void *,const uint8_t **,uint16_t *,size_t);
"""


def walk(node):
    yield node
    for child in node.children:
        yield from walk(child)


def preprocess(text, definitions):
    processor = Preprocessor()
    for definition in definitions:
        processor.define(definition)
    processor.parse(re.sub(r"^\s*#include[^\n]*", "", text, flags=re.MULTILINE))
    output = io.StringIO()
    processor.write(output)
    if processor.return_code:
        raise ValueError("C preprocessing failed")
    return output.getvalue()


def structure(parser, text, name):
    for node in walk(parser.parse(text.encode()).root_node):
        identifier = node.child_by_field_name("name")
        if (
            node.type == "struct_specifier"
            and identifier is not None
            and identifier.text.decode() == name
            and node.child_by_field_name("body") is not None
        ):
            return node.text.decode() + ";\n"
    raise ValueError("SDK structure missing: " + name)


def power_declarations(parser, bt_header):
    text = bt_header.read_text(encoding="utf-8")
    declarations = []
    for name in ("esp_ble_enhanced_power_type_t", "esp_power_level_t"):
        matches = []
        for node in walk(parser.parse(text.encode()).root_node):
            declarator = node.child_by_field_name("declarator")
            if (
                node.type == "type_definition"
                and declarator is not None
                and declarator.text.decode() == name
            ):
                matches.append(node.text.decode())
        if len(matches) != 1:
            raise ValueError("SDK power enum missing or ambiguous: " + name)
        declarations.extend(matches)
    matches = re.findall(r"\besp_err_t\s+esp_ble_tx_power_set_enhanced\s*\([^;{}]*\)\s*;", text)
    if len(matches) != 1:
        raise ValueError("SDK enhanced TX power prototype missing or ambiguous")
    return "\n".join(declarations + matches) + "\n"


def function(parser, text, name):
    matches = []
    for node in walk(parser.parse(text.encode()).root_node):
        if node.type != "function_definition":
            continue
        declarator = node.child_by_field_name("declarator")
        identifier = declarator.child_by_field_name("declarator")
        if identifier is not None and identifier.text.decode() == name:
            matches.append(node.text.decode())
    if len(matches) != 1:
        raise ValueError("C function missing or ambiguous: " + name)
    return matches[0] + "\n"


def event_structure(parser, text):
    event = structure(parser, text, "ble_gap_event")
    fields = []
    wanted = {
        "type",
        "phy_updated",
        "ext_disc",
        "adv_complete",
        "disc_complete",
        "periodic_sync",
        "periodic_report",
        "periodic_sync_lost",
    }
    for node in walk(parser.parse(event.encode()).root_node):
        declarator = node.child_by_field_name("declarator")
        if (
            node.type == "field_declaration"
            and declarator is not None
            and declarator.text.decode() in wanted
        ):
            fields.append(node.text.decode())
    return "struct ble_gap_event {\n" + "\n".join(fields) + "\n};\n"


def macros(text):
    lines = iter(text.splitlines(keepends=True))
    output = []
    for line in lines:
        if line.lstrip().startswith("#define "):
            while line.rstrip().endswith("\\"):
                line += next(lines)
            output.append(line)
    return "".join(output)


def npl_declarations(parser, npl_header, port_header):
    npl = npl_header.read_text(encoding="utf-8")
    declarations = (
        structure(parser, npl, "ble_npl_event")
        + structure(parser, npl, "ble_npl_eventq")
        + "typedef void ble_npl_event_fn(struct ble_npl_event *);\n"
    )
    # ESP-IDF exports these through npl_funcs; retain the SDK's actual types.
    for name in (
        "ble_npl_event_init",
        "ble_npl_event_deinit",
        "ble_npl_eventq_put",
        "ble_npl_eventq_remove",
    ):
        matches = re.findall(r"\bvoid\s+\(\*p_" + name + r"\)\s*(\([^;{}]*\))\s*;", npl)
        if len(matches) != 1:
            raise ValueError("SDK NPL prototype missing or ambiguous: " + name)
        declarations += "void " + name + matches[0] + ";\n"
    port = port_header.read_text(encoding="utf-8")
    matches = re.findall(
        r"struct ble_npl_eventq\s*\*\s*nimble_port_get_dflt_eventq\s*\([^;{}]*\)\s*;", port
    )
    if len(matches) != 1:
        raise ValueError("SDK default event queue prototype missing or ambiguous")
    declarations += matches[0] + "\n"
    for name in ("nimble_port_run", "nimble_port_stop", "nimble_port_deinit"):
        matches = re.findall(r"\b(?:void|int|esp_err_t)\s+" + name + r"\s*\([^;{}]*\)\s*;", port)
        if len(matches) != 1:
            raise ValueError("SDK port prototype missing or ambiguous: " + name)
        declarations += matches[0] + "\n"
    return declarations


def check(
    headers, hci_header, bt_header, compiler, idf_version, npl_header=None, port_header=None
):
    parser = Parser(Language(tree_sitter_c.language()))
    power = power_declarations(parser, bt_header)
    directory = Path(__file__).resolve().parent
    header = (directory / "bluetooth_ble5.h").read_text(encoding="utf-8")
    backend = (directory / "bluetooth_ble5_nimble.c").read_text(encoding="utf-8")
    port = (directory.parent / "mpnimbleport.c").read_text(encoding="utf-8")
    bindings = (directory / "bluetooth_ble5_bindings.c").read_text(encoding="utf-8")
    frontend = bindings.split("// BEGIN METHODS")[0]
    binding_constants = bindings.split("// BEGIN CONSTANTS")[1].split("// END CONSTANTS")[0]
    frontend += (
        "\nconst struct {int key; int value;} fake_constants[] = {\n"
        + binding_constants
        + "\n};\n"
    )
    events = (directory / "bluetooth_ble5_events.c").read_text(encoding="utf-8")
    gap = (headers / "ble_gap.h").read_text(encoding="utf-8")
    sdk = hci_header.parents[3]
    npl = npl_declarations(
        parser,
        npl_header or sdk / "porting/npl/freertos/include/nimble/nimble_npl_os.h",
        port_header or sdk / "porting/nimble/include/nimble/nimble_port.h",
    )
    constants = (
        macros(hci_header.read_text(encoding="utf-8"))
        + macros(hci_header.with_name("ble.h").read_text(encoding="utf-8"))
        + macros((headers / "ble_hs.h").read_text(encoding="utf-8"))
        + macros(gap)
    )
    qstrs = "".join(
        f"#define {name} {index + 1}\n"
        for index, name in enumerate(sorted(set(re.findall(r"MP_QSTR_\w+", frontend))))
    )
    options = (
        ["/nologo", "/Zs", "/std:c11", "/TC", "/WX"]
        if Path(compiler).stem.lower() == "cl"
        else [
            "-fsyntax-only",
            "-std=c11",
            "-Werror=implicit-function-declaration",
            "-Werror=incompatible-pointer-types",
        ]
    )
    version = tuple(int(part) for part in idf_version.split("."))
    if len(version) == 2:
        version += (0,)
    if len(version) != 3:
        raise ValueError("IDF version must have major.minor[.patch] form")
    idf_version_value = (version[0] << 16) | (version[1] << 8) | version[2]
    checks = 0
    with tempfile.TemporaryDirectory(prefix="micropython-ble5-types-") as temporary:
        output = Path(temporary) / "generated"
        make_bindings.generate(directory.parents[2], output)
        generated = (output / "modbluetooth_nimble_esp32.c").read_text(encoding="utf-8")
        shutdown = function(parser, generated, "mp_bluetooth_deinit")
        for extended, periodic, enhancements, reattempt in (
            (0, 0, 0, 0),
            (1, 0, 0, 0),
            (1, 1, 0, 0),
            (1, 1, 1, 0),
            (1, 1, 0, 1),
            (1, 1, 1, 1),
        ):
            definitions = [
                "MYNEWT_VAL(x) MYNEWT_VAL_ ## x",
                "ESP_IDF_VERSION_VAL(a,b,c) (((a)<<16)|((b)<<8)|(c))",
                "ESP_IDF_VERSION " + str(idf_version_value),
                "MICROPY_PY_BLUETOOTH 1",
                "MICROPY_BLUETOOTH_NIMBLE 1",
                "MICROPY_BLUETOOTH_NIMBLE_BINDINGS_ONLY 1",
                "MICROPY_PY_BLUETOOTH_ENABLE_CENTRAL_MODE 1",
                "MICROPY_PY_BLUETOOTH_USE_SYNC_EVENTS_WITH_INTERLOCK 1",
                "SOC_BLE_50_SUPPORTED 1",
                "CONFIG_BT_NIMBLE_50_FEATURE_SUPPORT 1",
                "CONFIG_IDF_TARGET_ESP32P4 0",
                "CONFIG_IDF_TARGET_ESP32C6 " + str(int(bt_header.parents[1].name == "esp32c6")),
                "MYNEWT_VAL_BLE_EXT_ADV " + str(extended),
                "MYNEWT_VAL_BLE_PERIODIC_ADV " + str(periodic),
                "MYNEWT_VAL_BLE_PERIODIC_ADV_ENH " + str(enhancements),
                "MYNEWT_VAL_BLE_ENABLE_CONN_REATTEMPT " + str(reattempt),
                "MYNEWT_VAL_BLE_EXT_ADV_MAX_SIZE 1650",
                "MYNEWT_VAL_BLE_MULTI_ADV_INSTANCES 2",
                "MYNEWT_VAL_BLE_LL_CFG_FEAT_LE_2M_PHY 1",
                "MYNEWT_VAL_BLE_LL_CFG_FEAT_LE_CODED_PHY 1",
                "BLE_ADV_INSTANCES 3",
            ]
            if periodic:
                backend_code = preprocess(header + backend, definitions)
                sync_function = function(parser, backend_code, "esp32_ble5_periodic_sync")
                expected_unsupported = int(bool(reattempt)) + int(
                    bool(reattempt) and version < (5, 5, 4)
                )
                if sync_function.count("return MP_EOPNOTSUPP;") != expected_unsupported:
                    raise ValueError(
                        "periodic sync retry restriction does not match IDF " + idf_version
                    )
            active = preprocess(gap, definitions)
            shapes = event_structure(parser, active)
            for name in (
                "ble_gap_conn_params",
                "ble_gap_ext_adv_params",
                "ble_gap_ext_disc_params",
                "ble_gap_ext_disc_desc",
                "ble_gap_periodic_adv_params",
                "ble_gap_periodic_sync_params",
                "ble_gap_periodic_adv_start_params",
                "ble_gap_periodic_adv_set_data_params",
            ):
                try:
                    shapes = structure(parser, active, name) + shapes
                except ValueError:
                    pass  # Optional structure absent in this configuration.
            prototypes = []
            for name in sorted(
                set(re.findall(r"\b(ble_gap_\w+)\s*\(", preprocess(backend, definitions)))
            ):
                found = re.findall(r"\bint\s+" + name + r"\s*\([^;{}]*\)\s*;", active, re.DOTALL)
                if len(found) != 1:
                    raise ValueError("SDK GAP prototype missing or ambiguous: " + name)
                prototypes.extend(found)
            prefix = (
                COMMON
                + power
                + constants
                + shapes
                + npl
                + "\n".join(prototypes)
                + "\n"
                + BACKEND
                + "\n"
                + header
                + "\n"
            )
            for kind, source in (
                ("backend", backend + shutdown),
                ("frontend", FRONTEND + qstrs + frontend + events),
                ("port", port),
            ):
                code = preprocess(prefix + source, definitions)
                # tree-sitter does not accept #line directives inside initializers.
                syntax = parser.parse(re.sub(r"(?m)^#line[^\n]*", "", code).encode()).root_node
                errors = [node for node in walk(syntax) if node.type == "ERROR" or node.is_missing]
                if errors:
                    raise ValueError(
                        "C parse failed: "
                        + str([(node.start_point, node.text[:80]) for node in errors[:5]])
                    )
                path = (
                    Path(temporary) / f"{kind}-{extended}-{periodic}-{enhancements}-{reattempt}.c"
                )
                path.write_text(code, encoding="utf-8")
                result = subprocess.run(
                    [compiler, *options, str(path)], capture_output=True, check=False
                )
                if result.returncode:
                    print(
                        result.stdout.decode(errors="replace")
                        + result.stderr.decode(errors="replace")
                    )
                    raise ValueError("C type check failed: " + path.name)
                checks += 1
                print("PASS", "IDF " + idf_version, path.name)
    return checks


if __name__ == "__main__":
    arguments = argparse.ArgumentParser(description=__doc__)
    arguments.add_argument("--headers", type=Path, action="append", required=True)
    arguments.add_argument("--hci-header", type=Path, required=True)
    arguments.add_argument("--bt-header", type=Path, required=True)
    arguments.add_argument("--compiler", required=True)
    arguments.add_argument("--idf-version", required=True)
    arguments.add_argument("--npl-header", type=Path)
    arguments.add_argument("--port-header", type=Path)
    args = arguments.parse_args()
    count = sum(
        check(
            headers,
            args.hci_header,
            args.bt_header,
            args.compiler,
            args.idf_version,
            args.npl_header,
            args.port_header,
        )
        for headers in args.headers
    )
    print(f"{count} static syntax/type checks passed; no firmware built.")
