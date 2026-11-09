"""Compile the actual native lowerer against independently authored IMNMX fields; no game or GPU."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    args = parser.parse_args()
    flags = (subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS) if os.name == "nt" else 0
    with tempfile.TemporaryDirectory(prefix="imnmx-immediate-") as folder:
        exe = Path(folder) / "fixture.exe"
        for command in (
            [args.cxx, "-std=c++20", "-O1", "-DIMNMX_LOWERING_ONLY",
             "-I" + str(ROOT / "gpu/vk/compat"), "-I" + str(ROOT / "gpu/vk/eden"),
             str(ROOT / "tests/imnmx_immediate.cpp"), "-o", str(exe)],
            [str(exe)],
        ):
            result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, creationflags=flags)
            print(result.stdout, end="", flush=True)
            result.check_returncode()


if __name__ == "__main__":
    main()
