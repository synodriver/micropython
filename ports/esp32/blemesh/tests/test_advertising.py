"""Run private advertising lifetime tests against generated SDK entry points.

Builds only a host test binary, not firmware. MSVC requires a developer prompt.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
from make_adapter import generate, generate_adv


def run(idf, compiler):
    core = idf / "components/bt/esp_ble_mesh/core"
    adapter = generate((core / "nimble_host/adapter.c").read_text())
    adv = generate_adv((core / "adv.c").read_text())
    # Execute the generated early dispatch verbatim. Entering the SDK's
    # original notification path for a completion is a test failure.
    callback = adapter[adapter.index("static int gap_event_cb("):]
    callback = callback[:callback.index("    struct ble_gap_conn_desc desc;")]
    callback += '\n    assert(!"entered SDK callback");\n    return 0;\n}\n'
    wrapper = adapter[adapter.rindex("int bt_le_ext_adv_start("):]
    wait = adv[adv.index("    err = esp32_blemesh_adv_wait();"):].split("\n", 1)[0]
    implementation = re.sub(r"(?m)^#include[^\n]*", "", (HERE.parent / "advertising.c").read_text())
    native = (HERE.parent / "modblemesh.c").read_text()
    native_stop = native[native.index("static int stop(bool erase)"):native.index("int esp32_blemesh_shutdown(void)")]
    code = (HERE / "advertising_host.c").read_text()
    for marker, source in (("ADVERTISING_IMPLEMENTATION", implementation),
                           ("GENERATED_CALLBACK", callback), ("GENERATED_START", wrapper),
                           ("GENERATED_WAIT", wait), ("NATIVE_STOP", native_stop)):
        code = code.replace("// " + marker, source)
    with tempfile.TemporaryDirectory(prefix="blemesh-advertising-") as directory:
        temp = Path(directory)
        path, exe = temp / "test.c", temp / "test.exe"
        path.write_text(code)
        if Path(compiler).stem.lower() == "cl":
            args = ["/nologo", "/std:c11", "/W4", "/WX", str(path), f"/Fe:{exe}", f"/Fo:{temp / 'test.obj'}"]
        else:
            args = ["-std=c11", "-Wall", "-Wextra", "-Werror", str(path), "-o", str(exe)]
        subprocess.run([compiler, *args], cwd=temp, check=True)
        subprocess.run([str(exe)], cwd=temp, check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf", type=Path, required=True)
    parser.add_argument("--compiler", default="cc")
    args = parser.parse_args()
    run(args.idf, args.compiler)
