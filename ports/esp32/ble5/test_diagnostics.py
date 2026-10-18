"""Compile/run the production diagnostic probes with a deterministic host SDK.

Pass --compiler cl.exe (in a VS environment), gcc or clang. This does not build
firmware or validate controller ABI; check_sdk.py checks actual GAP declarations.
"""

import argparse
import subprocess
import tempfile
from pathlib import Path

HEADERS = {
    "esp_attr.h": "#define IRAM_ATTR\n#define DRAM_ATTR\n",
    "esp_timer.h": "#include <stdint.h>\nint64_t esp_timer_get_time(void);\n",
    "freertos/FreeRTOS.h": "#include <assert.h>\ntypedef int portMUX_TYPE;\n",
    "freertos/portmacro.h": """
extern int test_lock_level;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL_SAFE(m) ((void)(m), assert(test_lock_level++ == 0))
#define portEXIT_CRITICAL_SAFE(m) ((void)(m), assert(--test_lock_level == 0))
""",
    "os/os.h": """
#include <stdint.h>
typedef int os_error_t;
struct os_mempool {uint16_t mp_num_blocks, mp_num_free, mp_min_free;};
""",
    "host/ble_gap.h": """
#include <stdint.h>
#define MYNEWT_VAL(x) 1
#define BLE_GAP_EVENT_EXT_DISC 1
#define BLE_GAP_EVENT_PERIODIC_REPORT 2
#define BLE_GAP_EVENT_PERIODIC_SYNC 3
#define BLE_GAP_EVENT_PERIODIC_SYNC_LOST 4
struct ble_gap_event {
    int type;
    union {
        struct {uint8_t data_status, sid; uint16_t length_data;} ext_disc;
        struct {uint8_t data_status; uint16_t data_length, sync_handle;} periodic_report;
        struct {uint16_t status, sync_handle;} periodic_sync;
        struct {uint16_t reason, sync_handle;} periodic_sync_lost;
    };
};
""",
}


def run(compiler):
    directory = Path(__file__).resolve().parent
    msvc = Path(compiler).stem.lower() == "cl"
    with tempfile.TemporaryDirectory(prefix="micropython-ble5-diag-") as temporary:
        output = Path(temporary)
        for name, content in HEADERS.items():
            path = output / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
        executable = output / "diagnostics.exe"
        if msvc:
            options = [
                "/nologo",
                "/std:c11",
                "/TC",
                "/W4",
                "/WX",
                "/D_MICROPY_UNUSED=1",
                "/DMICROPY_ESP32_BLE5_DIAGNOSTICS=1",
                f"/I{output}",
                f"/I{directory}",
                f"/Fe{executable}",
            ]
        else:
            options = [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-DMICROPY_ESP32_BLE5_DIAGNOSTICS=1",
                f"-I{output}",
                f"-I{directory}",
                "-o",
                str(executable),
            ]
        subprocess.run(
            [
                compiler,
                *options,
                str(directory / "bluetooth_ble5_diagnostics.c"),
                str(directory / "test_diagnostics.c"),
            ],
            cwd=output,
            check=True,
        )
        subprocess.run([str(executable)], cwd=output, check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    run(parser.parse_args().compiler)
