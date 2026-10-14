"""Manual ESP32 C3/C5/C6/H2/S3 Mesh smoke test, not an RF interoperability test.

Run on a dedicated board (replaces its BLE IRQ and GATT services):
  import coexistence_smoke
  coexistence_smoke.run(unique_uuid_16_bytes, company_id=your_company_id)
An external observer must separately verify advertising/periodic continuity.
No keys are erased; use the existing node identity if already provisioned.
"""
import bluetooth
import blemesh
import time


def run(uuid, *, company_id, cycles=3):
    ble = bluetooth.BLE()
    ble.active(True)
    features = ble.ble5_features()
    assert features["advertising_instances"] == 4
    assert features["extended_advertising"] and features["periodic_advertising"]
    assert blemesh.status()["state"] == 0
    counts = [0, 0]

    def irq(event, data):
        if event == 6:  # Existing _IRQ_SCAN_DONE
            counts[0] += 1
        elif event == ble.IRQ_SCAN_RESULT_EXT:
            counts[1] += 1

    ble.irq(irq)
    service = (bluetooth.UUID(0xFFF0), ((bluetooth.UUID(0xFFF1), bluetooth.FLAG_READ),))
    ((handle,),) = ble.gatts_register_services((service,))
    ble.gatts_write(handle, b"unchanged")
    try:
        ble.gap_advertise(200000, b"\x02\x01\x06", connectable=False)
        for instance in (1, 2, 3):
            ble.gap_advertise_ext(200000, b"\x02\x01\x06", instance=instance,
                                  connectable=False, sid=instance)
        ble.gap_periodic_advertise(200000, b"mesh-coexist", instance=2)
        try:
            ble.gap_advertise_ext(200000, instance=4)
        except (OSError, ValueError):
            pass
        else:
            raise AssertionError("Mesh instance exposed to the application")

        for cycle in range(cycles):
            blemesh.start(uuid, company_id=company_id)
            deadline = time.ticks_add(time.ticks_ms(), 5000)
            while True:
                item = blemesh.recv()
                if item is not None:
                    assert item[0] != blemesh.ERROR, item
                    if item[0] == blemesh.READY:
                        break
                assert time.ticks_diff(deadline, time.ticks_ms()) > 0, "Mesh READY timeout"
                time.sleep_ms(10)
            assert ble.gatts_read(handle) == b"unchanged"
            assert ble.ble5_features() == features
            before = counts[0]
            ble.gap_scan_ext(300, interval_us=30000, window_us=15000)
            time.sleep_ms(600)
            assert counts[0] == before + 1
            ble.gap_scan_ext(0)
            time.sleep_ms(100)
            ble.gap_scan_ext(None)
            assert blemesh.status()["state"] == 2
            assert blemesh.status()["radio_error"] == 0
            # Stop Mesh while an application scan is still subscribed.
            before = counts[0]
            ble.gap_scan_ext(3000)
            blemesh.stop()
            assert blemesh.status()["state"] == 0
            assert ble.gatts_read(handle) == b"unchanged"
            time.sleep_ms(3300)
            assert counts[0] == before + 1
            print("PASS local coexistence cycle", cycle, "scan reports", counts[1])
        print("RF continuity, provisioning and remote GATT still require peer validation")
    finally:
        # Keep Mesh failure visible; do not erase NVS to make a test pass.
        blemesh.stop()
        ble.gap_scan_ext(None)
        ble.gap_periodic_advertise(None, instance=2)
        for instance in (1, 2, 3):
            ble.gap_advertise_ext_stop(instance, remove=True)
        ble.gap_advertise(None)
        ble.irq(None)
