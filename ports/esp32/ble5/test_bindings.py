"""Regression checks for ESP32-only binding generation; no firmware build."""

import hashlib
import re
import shutil
import tempfile
import unittest
from pathlib import Path

import make_bindings


class BindingGenerationTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(__file__).resolve().parents[3]
        self.temporary = tempfile.TemporaryDirectory(prefix="micropython-ble5-generator-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.output = self.directory / "generated"

    def test_shared_sources_unchanged_and_generation_idempotent(self):
        sources = [
            self.root / "extmod/modbluetooth.c",
            self.root / "extmod/nimble/modbluetooth_nimble.c",
        ]
        original = [hashlib.sha256(path.read_bytes()).digest() for path in sources]
        make_bindings.generate(self.root, self.output)
        timestamps = {path.name: path.stat().st_mtime_ns for path in self.output.iterdir()}
        make_bindings.generate(self.root, self.output)
        self.assertEqual(
            timestamps, {path.name: path.stat().st_mtime_ns for path in self.output.iterdir()}
        )
        self.assertEqual(
            original, [hashlib.sha256(path.read_bytes()).digest() for path in sources]
        )
        module = (self.output / "modbluetooth_esp32.c").read_text(encoding="utf-8")
        backend = (self.output / "modbluetooth_nimble_esp32.c").read_text(encoding="utf-8")
        self.assertEqual(module.count("MP_REGISTER_ROOT_POINTER(mp_obj_t bluetooth)"), 1)
        self.assertEqual(
            module.count("MP_REGISTER_EXTENSIBLE_MODULE(MP_QSTR_bluetooth, mp_module_bluetooth)"),
            1,
        )
        self.assertEqual(
            backend.count(
                "MP_REGISTER_ROOT_POINTER(struct _esp32_ble5_state_t *bluetooth_ble5_state)"
            ),
            1,
        )

    def test_upstream_changes_fail_before_writing_output(self):
        for filename in ("extmod/modbluetooth.c", "extmod/nimble/modbluetooth_nimble.c"):
            destination = self.directory / filename
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(self.root / filename, destination)
        source = self.directory / "extmod/modbluetooth.c"
        source.write_text(
            source.read_text(encoding="utf-8").replace(
                "MICROPY_PY_BLUETOOTH_MAX_EVENT_DATA_TUPLE_LEN 5",
                "MICROPY_PY_BLUETOOTH_MAX_EVENT_DATA_TUPLE_LEN 6",
            ),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(ValueError, "source anchor changed"):
            make_bindings.generate(self.directory, self.output)
        self.assertFalse(self.output.exists())

    def test_missing_backend_function_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "Bluetooth function changed"):
            make_bindings.keep_without_ext_adv("", "gap_scan_cb")

    def test_lifecycle_guards_are_generated_before_stack_changes(self):
        make_bindings.generate(self.root, self.output)
        backend = (self.output / "modbluetooth_nimble_esp32.c").read_text(encoding="utf-8")
        for name, operations in (
            (
                "mp_bluetooth_deinit",
                (
                    "mp_bluetooth_gap_advertise_stop();",
                    "mp_bluetooth_gap_scan_stop();",
                    "mp_bluetooth_nimble_port_shutdown();",
                    "MP_STATE_PORT(bluetooth_nimble_root_pointers) = NULL;",
                ),
            ),
            (
                "mp_bluetooth_init",
                (
                    "mp_bluetooth_deinit();",
                    "nimble_reset_gatts_bss();",
                    "mp_bluetooth_nimble_ble_state = MP_BLUETOOTH_NIMBLE_BLE_STATE_STARTING;",
                    "nimble_port_init()",
                ),
            ),
        ):
            lifecycle = re.search(r"(?ms)^int " + name + r"\(void\) \{.*?^}", backend)[0]
            guard = lifecycle.index("if (esp32_ble5_is_host_task())")
            rejection = lifecycle.index("return MP_EBUSY;", guard)
            for operation in operations:
                self.assertLess(rejection, lifecycle.index(operation))

    def test_both_cmake_gates_select_native_ble5_targets(self):
        expected = {"esp32s3", "esp32c2", "esp32c3", "esp32c5", "esp32c6", "esp32h2"}
        for filename in ("ports/esp32/CMakeLists.txt", "ports/esp32/esp32_common.cmake"):
            source = (self.root / filename).read_text(encoding="utf-8")
            matches = re.findall(r'IDF_TARGET MATCHES "\^\((.*?)\)\$"', source)
            self.assertEqual(len(matches), 1)
            self.assertEqual(set(matches[0].split("|")), expected)


if __name__ == "__main__":
    unittest.main()
