"""Run real scan.c with a simulated controller. Builds only a host test binary.

GCC/Clang: python test_scan.py --compiler cc
MSVC: run from a VS developer prompt, then python test_scan.py --compiler cl
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def run(compiler):
    here = Path(__file__).resolve().parent
    implementation = re.sub(r"(?m)^#include[^\n]*", "", (here.parent / "scan.c").read_text())
    source = (here / "scan_host.c").read_text().replace("// SCAN_IMPLEMENTATION", implementation)
    with tempfile.TemporaryDirectory(prefix="blemesh-scan-test-") as temp:
        temp = Path(temp)
        path = temp / "test.c"
        path.write_text(source, encoding="utf-8")
        exe = temp / "test.exe"
        if Path(compiler).stem.lower() == "cl":
            command = [compiler, "/nologo", "/std:c11", "/W4", "/WX", str(path), f"/Fe:{exe}", f"/Fo:{temp / 'test.obj'}"]
        else:
            command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", str(path), "-o", str(exe)]
        subprocess.run(command, check=True, cwd=temp)
        subprocess.run([str(exe)], check=True, cwd=temp)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default="cc")
    run(parser.parse_args().compiler)
