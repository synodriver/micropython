"""Check adapter generation against a local SDK, without building firmware."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE))
from make_adapter import generate, generate_adv


class AdapterTests(unittest.TestCase):
    def test_sdk_unchanged_and_output_stable(self):
        original = hashlib.sha256(SOURCE.read_bytes()).digest()
        with tempfile.TemporaryDirectory(prefix="blemesh-adapter-") as temp:
            output = Path(temp) / "adapter.c"
            command = [sys.executable, str(HERE / "make_adapter.py"),
                       "--input", str(SOURCE), "--output", str(output)]
            subprocess.run(command, check=True)
            timestamp = output.stat().st_mtime_ns
            subprocess.run(command, check=True)
            self.assertEqual(timestamp, output.stat().st_mtime_ns)
            self.assertEqual(output.read_text(), generate(SOURCE.read_text()))
        self.assertEqual(original, hashlib.sha256(SOURCE.read_bytes()).digest())

    def test_all_mesh_scan_calls_are_routed(self):
        output = generate(SOURCE.read_text())
        self.assertNotIn("ble_gap_ext_disc(", output)
        self.assertNotIn("ble_gap_disc_cancel();", output)
        self.assertEqual(output.count("esp32_blemesh_mesh_scan("), 2)
        self.assertEqual(output.count("esp32_blemesh_mesh_scan_cancel();"), 2)

    def test_advertiser_cache_covers_private_index_and_commits_after_success(self):
        output = generate(SOURCE.read_text())
        self.assertIn("last_param[BLE_ADV_INSTANCES]", output)
        start = output.index("if (!last_param[inst_id].set || memcmp(")
        invalid = output.index("last_param[inst_id].set = false;", start)
        configure = output.index("err = ble_gap_ext_adv_configure(", invalid)
        error = output.index("if (err != 0)", configure)
        returned = output.index("return err;", error)
        commit = output.index("last_param[inst_id].set = true;", returned)
        self.assertLess(invalid, configure)
        self.assertLess(returned, commit)

    def test_sdk_drift_is_rejected(self):
        source = SOURCE.read_text()
        for anchor in ("ble_gap_ext_disc(", "ble_gap_disc_cancel();",
                       "last_param[BLE_MESH_ADV_INST_TYPES_NUM]",
                       "        last_param[inst_id].set = true;\n",
                       "int bt_le_ext_adv_start(const uint8_t inst_id,",
                       "int bt_le_ext_adv_stop(uint8_t inst_id)\n{",
                       "static int gap_event_cb(struct ble_gap_event *event, void *arg)\n{"):
            with self.subTest(anchor=anchor), self.assertRaises(ValueError):
                generate(source.replace(anchor, "changed", 1))
        with self.assertRaises(ValueError):
            generate(source + "\nble_gap_ext_disc();\n")

    def test_mesh_completion_never_reaches_sdk_task_notification(self):
        output = generate(SOURCE.read_text())
        self.assertEqual(output.count("const uint8_t inst_id = ESP32_BLE_MESH_ADV_INSTANCE;"), 2)
        self.assertEqual(output.count("event->adv_complete.instance != ESP32_BLE_MESH_ADV_INSTANCE"), 2)
        self.assertEqual(output.count("esp32_blemesh_adv_complete((uintptr_t)arg, event->adv_complete.reason);\n"
                                      "        return 0;"), 2)
        self.assertIn("gap_event_cb, (void *)token)", output)
        self.assertIn("esp32_blemesh_adv_end(err);", output)

    def test_worker_propagates_completion_error(self):
        source = (SOURCE.parents[1] / "adv.c").read_text()
        output = generate_adv(source)
        self.assertNotIn("bt_mesh_adv_task_wait(UINT32_MAX, K_FOREVER, NULL)", output)
        self.assertLess(output.index("err = esp32_blemesh_adv_wait();"),
                        output.index("BLE_MESH_SEND_END_CB(err, cb, cb_data);"))
        with self.assertRaises(ValueError):
            generate_adv(source.replace("K_FOREVER", "changed"))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf", type=Path, required=True)
    args, rest = parser.parse_known_args()
    SOURCE = args.idf / "components/bt/esp_ble_mesh/core/nimble_host/adapter.c"
    unittest.main(argv=[sys.argv[0], *rest])
