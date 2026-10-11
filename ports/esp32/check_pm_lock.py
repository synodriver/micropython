"""Syntax-check PMLock with real MicroPython and IDF PM headers; no linking.

The temporary port configuration, FreeRTOS ISR probe and check_esp_err glue
are declarations only. SDK PM types/prototypes and all Python object binding
macros are read from the actual source, not replaced with test stubs.
Use a configured host C compiler, or pass --include for its system headers.
"""

import argparse
import re
import subprocess
import tempfile
from pathlib import Path

CONFIG = """
#include <stdint.h>
#include <stddef.h>
#define MICROPY_CONFIG_ROM_LEVEL MICROPY_CONFIG_ROM_LEVEL_CORE_FEATURES
#define MICROPY_ENABLE_GC (1)
#define MICROPY_ENABLE_FINALISER (1)
#define MICROPY_NLR_SETJMP (1)
#define MICROPY_NLR_SETJMP_NO_UNDERSCORE (1)
#define MP_ENDIANNESS_LITTLE (1)
#define MICROPY_PY_THREAD ({thread})
#define MICROPY_PY_THREAD_GIL ({thread})
#define MP_SSIZE_MAX INTPTR_MAX
typedef intptr_t mp_int_t;
typedef uintptr_t mp_uint_t;
typedef long mp_off_t;
#ifdef _MSC_VER
#define __attribute__(x)
#endif
"""


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--idf", type=Path, required=True)
    cli.add_argument("--idf-tag", required=True)
    cli.add_argument("--compiler", required=True)
    cli.add_argument("--include", action="append", default=[])
    args = cli.parse_args()
    root = Path(__file__).resolve().parents[2]
    source = root / "ports/esp32/esp32_pm_lock.c"

    def sdk(path):
        return subprocess.check_output(
            ["git", "-C", str(args.idf), "show", args.idf_tag + ":" + path]
        ).decode()

    pm = sdk("components/esp_pm/include/esp_pm.h")
    # Only esp_err_t is needed from the platform error header.
    err_type = re.search(
        r"typedef\s+int\s+esp_err_t;", sdk("components/esp_common/include/esp_err.h")
    )
    if err_type is None:
        raise ValueError("SDK esp_err_t declaration missing")
    # Generated qstr IDs are only needed to type-check the binding tables.
    qstrs = set(re.findall(r"MP_QSTR_\w+", source.read_text(encoding="utf-8")))
    for header in (root / "py").glob("*.h"):
        qstrs.update(re.findall(r"MP_QSTR_\w+", header.read_text(encoding="utf-8")))

    msvc = Path(args.compiler).stem.lower() == "cl"
    options = (
        ["/nologo", "/Zs", "/std:c11", "/TC", "/WX", "/we4013"]
        if msvc
        else ["-fsyntax-only", "-std=c11", "-Werror"]
    )
    with tempfile.TemporaryDirectory(prefix="mp-pmlock-headers-") as temporary:
        tmp = Path(temporary)
        (tmp / "genhdr").mkdir()
        (tmp / "freertos").mkdir()
        (tmp / "esp_pm.h").write_text(pm, encoding="utf-8")
        (tmp / "esp_err.h").write_text("#pragma once\n" + err_type.group(), encoding="utf-8")
        (tmp / "mphalport.h").write_text(
            '#include "esp_err.h"\nvoid check_esp_err(esp_err_t);\n', encoding="utf-8"
        )
        (tmp / "mpthreadport.h").write_text(
            "typedef struct { int dummy; } mp_thread_mutex_t;\n", encoding="utf-8"
        )
        (tmp / "freertos/FreeRTOS.h").write_text("", encoding="utf-8")
        (tmp / "freertos/portmacro.h").write_text(
            "int xPortInIsrContext(void);\n", encoding="utf-8"
        )
        (tmp / "genhdr/root_pointers.h").write_text("", encoding="utf-8")
        (tmp / "genhdr/qstrdefs.generated.h").write_text(
            "\n".join(f'QDEF0({name}, 0, 0, "")' for name in sorted(qstrs)), encoding="utf-8"
        )
        includes = [tmp, root, *args.include]
        include_args = [("/I" if msvc else "-I") + str(path) for path in includes]
        for thread in (0, 1):
            (tmp / "mpconfigport.h").write_text(CONFIG.format(thread=thread), encoding="utf-8")
            for pm_enabled in (0, 1):
                (tmp / "sdkconfig.h").write_text(
                    "#define CONFIG_PM_ENABLE 1\n" if pm_enabled else "", encoding="utf-8"
                )
                subprocess.run([args.compiler, *options, *include_args, str(source)], check=True)
                print(f"PASS {args.idf_tag} threads={thread} PM={pm_enabled}", flush=True)
    print("4 PMLock syntax/type checks passed; no firmware or executable built.")


if __name__ == "__main__":
    main()
