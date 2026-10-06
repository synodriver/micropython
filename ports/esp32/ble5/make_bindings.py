"""Generate port-local Bluetooth adaptations without modifying shared sources.

Run at CMake configuration time, before module/root-pointer/qstr extraction.
Every insertion requires a unique anchor so upstream changes fail explicitly.
"""

import argparse
import re
from pathlib import Path


def replace_once(source, anchor, replacement):
    if source.count(anchor) != 1:
        raise ValueError("Bluetooth source anchor changed: " + anchor)
    return source.replace(anchor, replacement, 1)


def keep_without_ext_adv(source, name):
    pattern = r"(?m)^(?:static )?(?:int|void) " + name + r"\([^\n]*\) \{\n.*?^}\n"
    matches = list(re.finditer(pattern, source, re.DOTALL))
    if len(matches) != 1:
        raise ValueError("Bluetooth function changed: " + name)
    match = matches[0]
    return (
        source[: match.start()]
        + "#if !MICROPY_ESP32_BLE5_EXT_ADV\n"
        + match.group()
        + "#endif\n"
        + source[match.end() :]
    )


def generate(micropython, output):
    directory = Path(__file__).resolve().parent
    source = (micropython / "extmod/modbluetooth.c").read_text(encoding="utf-8")
    source = replace_once(
        source,
        '#include "extmod/modbluetooth.h"',
        '#include "extmod/modbluetooth.h"\n#include "bluetooth_ble5.h"',
    )
    source = replace_once(
        source,
        "#define MICROPY_PY_BLUETOOTH_MAX_EVENT_DATA_TUPLE_LEN 5",
        "#define MICROPY_PY_BLUETOOTH_MAX_EVENT_DATA_TUPLE_LEN 11",
    )
    # An empty bytearray can expose a NULL buffer. Preserve its distinction from
    # None, which tells the backend to reuse the previous advertising data.
    for name, buffer in (("adv_data", "adv_bufinfo"), ("resp_data", "resp_bufinfo")):
        anchor = f"        mp_get_buffer_raise(args[ARG_{name}].u_obj, &{buffer}, MP_BUFFER_READ);"
        source = replace_once(
            source,
            anchor,
            anchor
            + f'\n        if ({buffer}.len == 0) {{\n            {buffer}.buf = (void *)"";\n        }}',
        )
    bindings = (directory / "bluetooth_ble5_bindings.c").read_text(encoding="utf-8")
    methods = re.search(r"// BEGIN METHODS\n(.*?)// END METHODS", bindings, re.DOTALL)
    constants = re.search(r"// BEGIN CONSTANTS\n(.*?)// END CONSTANTS", bindings, re.DOTALL)
    if methods is None or constants is None:
        raise ValueError("BLE 5 binding table markers missing")
    bindings = bindings[: methods.start()]
    anchor = "static const mp_rom_map_elem_t bluetooth_ble_locals_dict_table[] = {"
    source = replace_once(source, anchor, bindings + "\n" + anchor + "\n" + methods[1])
    anchor = "static const mp_rom_map_elem_t mp_module_bluetooth_globals_table[] = {"
    source = replace_once(source, anchor, anchor + "\n" + constants[1])
    source += "\n" + (directory / "bluetooth_ble5_events.c").read_text(encoding="utf-8")

    backend = (micropython / "extmod/nimble/modbluetooth_nimble.c").read_text(encoding="utf-8")
    backend = replace_once(
        backend,
        '#include "extmod/modbluetooth.h"',
        '#include "extmod/modbluetooth.h"\n#include "bluetooth_ble5.h"',
    )
    for name in (
        "mp_bluetooth_gap_advertise_start",
        "mp_bluetooth_gap_advertise_stop",
        "gap_scan_cb",
        "mp_bluetooth_gap_scan_start",
        "mp_bluetooth_gap_scan_stop",
        "mp_bluetooth_gap_peripheral_connect",
    ):
        backend = keep_without_ext_adv(backend, name)
    anchor = "        case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:\n"
    backend = replace_once(
        backend, anchor, anchor + "            esp32_ble5_on_phy_update(event);\n"
    )
    anchor = "static int commmon_gap_event_cb(struct ble_gap_event *event, void *arg) {\n"
    backend = replace_once(
        backend,
        anchor,
        anchor
        + "    if (event->type == BLE_GAP_EVENT_PHY_UPDATE_COMPLETE) {\n"
        + "        esp32_ble5_on_phy_update(event);\n"
        + "        return 0;\n"
        + "    }\n",
    )
    anchor = "    MP_STATE_PORT(bluetooth_nimble_root_pointers) = NULL;"
    backend = replace_once(backend, anchor, anchor + "\n    esp32_ble5_reset();")
    anchor = "static void reset_cb(int reason) {\n"
    backend = replace_once(backend, anchor, anchor + "    esp32_ble5_reset();\n")
    for name in ("mp_bluetooth_init", "mp_bluetooth_deinit"):
        anchor = f"int {name}(void) {{\n"
        backend = replace_once(
            backend,
            anchor,
            anchor
            + "    #if MICROPY_ESP32_BLE5\n"
            + "    // The host task cannot wait for its own shutdown event.\n"
            + "    if (esp32_ble5_is_host_task()) {\n"
            + "        return MP_EBUSY;\n"
            + "    }\n"
            + "    #endif\n",
        )
    backend += "\n" + (directory / "bluetooth_ble5_nimble.c").read_text(encoding="utf-8")

    output.mkdir(parents=True, exist_ok=True)
    for filename, contents in (
        ("modbluetooth_esp32.c", source),
        ("modbluetooth_nimble_esp32.c", backend),
    ):
        path = output / filename
        if not path.exists() or path.read_text(encoding="utf-8") != contents:
            with path.open("w", encoding="utf-8", newline="\n") as generated:
                generated.write(contents)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--micropython", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    generate(args.micropython.resolve(), args.output.resolve())
