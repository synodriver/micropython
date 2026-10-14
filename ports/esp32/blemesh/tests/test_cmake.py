"""Exercise source replacement and guards in a synthetic CMake target.

Does not configure ESP-IDF, run Kconfig, or build any firmware.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parents[1]


class CMakeTests(unittest.TestCase):
    def configure(self, changes="", expected=True):
        required = ("SOC_BLE_MESH_SUPPORTED BLE_MESH BT_NIMBLE_ENABLED BT_NIMBLE_EXT_ADV BT_NIMBLE_EXT_SCAN "
                    "BLE_MESH_NODE BLE_MESH_PB_ADV BLE_MESH_USE_BLE_50 BLE_MESH_DEINIT "
                    "BLE_MESH_SETTINGS BLE_MESH_HEALTH_SRV").split()
        definitions = "\n".join("set(CONFIG_{} ON)".format(name) for name in required)
        source = IDF / "components/bt/esp_ble_mesh/core/nimble_host/adapter.c"
        cmake = f'''cmake_minimum_required(VERSION 3.20)
project(mesh_harness NONE)
# No compiler is enabled. These rules deliberately fail if someone builds
# this synthetic target; only its source/include properties are tested.
set(CMAKE_C_ARCHIVE_CREATE "${{CMAKE_COMMAND}} -E false")
set(CMAKE_C_ARCHIVE_FINISH "${{CMAKE_COMMAND}} -E false")
add_library(bt STATIC "{source.as_posix()}" "{(source.parents[1] / 'adv.c').as_posix()}")
set_target_properties(bt PROPERTIES LINKER_LANGUAGE C)
function(idf_component_get_property out component property)
  set(${{out}} bt PARENT_SCOPE)
endfunction()
set(IDF_VERSION_MAJOR {VERSION[0]})
set(IDF_VERSION_MINOR {VERSION[1]})
set(IDF_VERSION_PATCH {VERSION[2]})
set(IDF_TARGET esp32c6)
{definitions}
set(CONFIG_BLE_MESH_ADV_INST_ID 3)
set(CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES 4)
set(CONFIG_BT_CTRL_BLE_MAX_ACT 10)
{changes}
include("{(HERE / 'idf.cmake').as_posix()}")
get_target_property(result bt SOURCES)
if(NOT result STREQUAL "${{CMAKE_BINARY_DIR}}/blemesh/adapter.c;${{CMAKE_BINARY_DIR}}/blemesh/adv.c")
  message(FATAL_ERROR "Adapter was not replaced")
endif()
'''
        with tempfile.TemporaryDirectory(prefix="blemesh-cmake-") as temp:
            temp = Path(temp)
            (temp / "CMakeLists.txt").write_text(cmake)
            env = dict(os.environ, IDF_PATH=str(IDF))
            result = subprocess.run([CMAKE, "-S", str(temp), "-B", str(temp / "build"),
                                     "-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + NINJA,
                                     "-DPython3_EXECUTABLE=" + sys.executable],
                                    env=env, capture_output=True, text=True)
            if expected:
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertTrue((temp / "build/blemesh/adapter.c").exists())
                self.assertTrue((temp / "build/blemesh/adv.c").exists())
            else:
                self.assertNotEqual(result.returncode, 0)
            # CMake wraps diagnostics according to terminal width.
            return " ".join((result.stdout + result.stderr).split())

    def test_replace_exact_adapter(self):
        self.configure()

    def test_board_variants_enable_mesh(self):
        variants = [(chip, "BLEMESH") for chip in ("C3", "C5", "C6", "H2", "S3")]
        variants.append(("S3", "BLEMESH_SPIRAM_OCT"))
        for chip, variant in variants:
            with self.subTest(chip=chip, variant=variant):
                path = HERE.parent / "boards" / ("ESP32_GENERIC_" + chip) / ("mpconfigvariant_" + variant + ".cmake")
                self.configure(f'include("{path.as_posix()}")\n'
                               'if(NOT MICROPY_ESP32_BLEMESH)\n'
                               '  message(FATAL_ERROR "Board variant did not enable Mesh")\nendif()')

    def test_supported_targets_and_versions(self):
        for target in ("esp32c3", "esp32c5", "esp32c6", "esp32h2", "esp32s3"):
            for major, minor, patch in ((5, 5, 3), (5, 5, 4), (5, 5, 5), (6, 1, 0)):
                if target == "esp32c5" and major == 5 and patch < 5:
                    continue
                with self.subTest(target=target, version=(major, minor, patch)):
                    self.configure(f"set(IDF_TARGET {target})\n"
                                   f"set(IDF_VERSION_MAJOR {major})\n"
                                   f"set(IDF_VERSION_MINOR {minor})\n"
                                   f"set(IDF_VERSION_PATCH {patch})")

    def test_unsupported_versions_rejected(self):
        for major, minor, patch in ((5, 5, 0), (5, 5, 1), (5, 5, 2), (5, 4, 5), (6, 0, 1), (6, 2, 0)):
            with self.subTest(version=(major, minor, patch)):
                self.assertIn("requires ESP-IDF 5.5.x (>=5.5.3) or 6.1.x", self.configure(
                    f"set(IDF_VERSION_MAJOR {major})\nset(IDF_VERSION_MINOR {minor})\n"
                    f"set(IDF_VERSION_PATCH {patch})", expected=False))

    def test_conflicting_configuration_rejected(self):
        for change, message in (
            ("set(CONFIG_BLE_MESH_PB_GATT ON)", "does not support"),
            ("set(CONFIG_BLE_MESH_USE_DUPLICATE_SCAN ON)", "does not support"),
            ("set(CONFIG_BLE_MESH OFF)", "requires CONFIG_BLE_MESH"),
            ("set(CONFIG_SOC_BLE_MESH_SUPPORTED OFF)", "requires CONFIG_SOC_BLE_MESH_SUPPORTED"),
            ("set(CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES 5)", "requires role 3"),
            ("set(IDF_TARGET esp32c2)", "ESP32-C2 lacks SDK Mesh support"),
            ("set(IDF_TARGET esp32p4)", "targets ESP32-C3/C5/C6/H2/S3"),
            ("set(IDF_TARGET esp32c3)\nset(CONFIG_BT_CTRL_BLE_MAX_ACT 6)", "requires CONFIG_BT_CTRL_BLE_MAX_ACT=10"),
            ("set(IDF_TARGET esp32s3)\nunset(CONFIG_BT_CTRL_BLE_MAX_ACT)", "requires CONFIG_BT_CTRL_BLE_MAX_ACT=10"),
            ("set(IDF_TARGET esp32c5)\nset(IDF_VERSION_MAJOR 5)\nset(IDF_VERSION_MINOR 5)\nset(IDF_VERSION_PATCH 4)",
             "ESP32-C5 Mesh requires ESP-IDF 5.5.5"),
        ):
            with self.subTest(change=change):
                self.assertIn(message, self.configure(change, expected=False))

    def test_missing_adapter_rejected(self):
        self.assertIn("expected one Mesh NimBLE adapter", self.configure(
            'set_property(TARGET bt PROPERTY SOURCES "${CMAKE_CURRENT_LIST_DIR}/dummy.c")',
            expected=False))

    def test_missing_advertising_worker_rejected(self):
        source = IDF / "components/bt/esp_ble_mesh/core/nimble_host/adapter.c"
        self.assertIn("expected one Mesh advertising worker", self.configure(
            f'set_property(TARGET bt PROPERTY SOURCES "{source.as_posix()}")', expected=False))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf", type=Path, required=True)
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--ninja", default="ninja")
    parser.add_argument("--idf-version", default="6.1.0")
    args, rest = parser.parse_known_args()
    IDF, CMAKE, NINJA = args.idf.resolve(), args.cmake, args.ninja
    VERSION = tuple(map(int, args.idf_version.split(".")))
    unittest.main(argv=[sys.argv[0], *rest])
