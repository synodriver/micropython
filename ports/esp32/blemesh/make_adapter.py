"""Generate a build-local IDF adapter; never change the SDK checkout.

The first-stage profile is PB-ADV only, IDF 5.5.x (>=5.5.3) / 6.1.x.
Fail closed on source drift.
"""
import argparse
from pathlib import Path


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise ValueError("ESP-BLE-MESH adapter changed: " + old)
    return source.replace(old, new, 1)


def generate(source):
    source = '#include "blemesh.h"\n' + source
    # All discovery operations in this translation unit belong to Mesh.
    # Provisioner/GATT/Proxy are rejected by the profile's CMake checks.
    if source.count("ble_gap_ext_disc(") != 2 or source.count("ble_gap_disc_cancel();") != 2:
        raise ValueError("ESP-BLE-MESH discovery call sites changed")
    source = source.replace("ble_gap_ext_disc(", "esp32_blemesh_mesh_scan(")
    source = source.replace("ble_gap_disc_cancel();", "esp32_blemesh_mesh_scan_cancel();")
    # The SDK Kconfig limits its Mesh role ID to 0..3. Keep role 3 internally
    # but use physical instance 4 at the HCI edge.
    # Application handles 0..3 then remain contiguous without changing BLE5 IRQs.
    anchor = ("int bt_le_ext_adv_start(const uint8_t inst_id,\n"
              "                        const struct bt_mesh_adv_param *param,\n"
              "                        const struct bt_mesh_adv_data *ad, size_t ad_len,\n"
              "                        const struct bt_mesh_adv_data *sd, size_t sd_len)\n{\n")
    public_start = anchor
    source = replace_once(source, anchor, anchor.replace("int bt_le_ext_adv_start(", "static int mesh_adv_start(").replace(
        "const uint8_t inst_id", "const uint8_t mesh_inst_id").replace("size_t sd_len)", "size_t sd_len, uintptr_t token)") +
        "    if (mesh_inst_id != CONFIG_BLE_MESH_ADV_INST_ID) { return -EINVAL; }\n"
        "    const uint8_t inst_id = ESP32_BLE_MESH_ADV_INSTANCE;\n")
    source = replace_once(source, "int bt_le_ext_adv_stop(uint8_t inst_id)\n{",
        "int bt_le_ext_adv_stop(uint8_t mesh_inst_id)\n{\n"
        "    if (mesh_inst_id != CONFIG_BLE_MESH_ADV_INST_ID) { return -EINVAL; }\n"
        "    const uint8_t inst_id = ESP32_BLE_MESH_ADV_INSTANCE;")
    anchor = "static int gap_event_cb(struct ble_gap_event *event, void *arg)\n{"
    if source.count(anchor) != 2:
        raise ValueError("ESP-BLE-MESH advertising callback layout changed")
    source = source.replace(anchor,
        "static int gap_event_cb(struct ble_gap_event *event, void *arg)\n{\n"
        "    if (event->type == BLE_GAP_EVENT_ADV_COMPLETE) {\n"
        "        if (event->adv_complete.instance != ESP32_BLE_MESH_ADV_INSTANCE) { return 0; }\n"
        "        esp32_blemesh_adv_complete((uintptr_t)arg, event->adv_complete.reason);\n"
        "        return 0; // Never notify the SDK task handle from a Host callback.\n"
        "    }")
    # IDF indexes this cache by controller instance, not by Mesh role count.
    source = replace_once(source, "last_param[BLE_MESH_ADV_INST_TYPES_NUM]",
                          "last_param[BLE_ADV_INSTANCES]")
    source = replace_once(source, "ble_gap_ext_adv_configure(inst_id, &adv_params, NULL, gap_event_cb, NULL)",
                          "ble_gap_ext_adv_configure(inst_id, &adv_params, NULL, gap_event_cb, (void *)token)")
    # An unsuccessful configuration must be retried even with identical params.
    source = replace_once(source, "if (memcmp(&adv_params, &last_param[inst_id].param,",
                          "if (!last_param[inst_id].set || memcmp(&adv_params, &last_param[inst_id].param,")
    source = replace_once(source, "        last_param[inst_id].set = true;\n",
                          "        last_param[inst_id].set = false;\n")
    source = replace_once(source, "        memcpy(&last_param[inst_id].param, &adv_params,",
                          "        last_param[inst_id].set = true;\n        memcpy(&last_param[inst_id].param, &adv_params,")
    source += "\nvoid esp32_blemesh_adapter_reset(void) {\n    memset(last_param, 0, sizeof(last_param));\n}\n"
    source += "\n" + public_start + """    uintptr_t token = esp32_blemesh_adv_begin();
    if (!token) { return -EIO; }
    int err = mesh_adv_start(inst_id, param, ad, ad_len, sd, sd_len, token);
    esp32_blemesh_adv_end(err);
    return err;
}
"""
    return source


def generate_adv(source):
    # SDK task notifications can also mean "packet queued", and carry no
    # termination status. Use a private completion that survives SDK teardown.
    old = """    if (!bt_mesh_adv_task_wait(UINT32_MAX, K_FOREVER, NULL)) {
        BT_WARN("Advertising didn't finish on time");
        bt_le_ext_adv_stop(CONFIG_BLE_MESH_ADV_INST_ID);
    }"""
    return '#include "blemesh.h"\n' + replace_once(source, old,
        "    err = esp32_blemesh_adv_wait();")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--adv-input", type=Path)
    parser.add_argument("--adv-output", type=Path)
    args = parser.parse_args()
    result = generate(args.input.read_text(encoding="utf-8"))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_text(encoding="utf-8") != result:
        args.output.write_text(result, encoding="utf-8", newline="\n")
    if args.adv_input:
        result = generate_adv(args.adv_input.read_text(encoding="utf-8"))
        if not args.adv_output:
            parser.error("--adv-input requires --adv-output")
        args.adv_output.parent.mkdir(parents=True, exist_ok=True)
        if not args.adv_output.exists() or args.adv_output.read_text(encoding="utf-8") != result:
            args.adv_output.write_text(result, encoding="utf-8", newline="\n")
