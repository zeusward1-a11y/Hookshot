"""Streamline's eWarnOutOfVRAM (39) is a drawn DLSS frame, not a failure that turns DLSS off (gpu/vk/dlss_result.h).

Also checks that vk_dlss.cpp judges slEvaluateFeature's result through it. No game, GPU or Streamline DLLs.
    python -I -B tests/test_dlss_result.py [--cxx clang++]
"""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    args = parser.parse_args()
    src = (ROOT / "gpu/vk/vk_dlss.cpp").read_text(encoding="utf-8")
    # the evaluation's result goes to dlss_drawn, and only a result it rejects names the failing stage
    m = re.search(r"r = st\.evaluate_feature\([^;]*;\s*if \(!dlss_drawn\(r\)\) \{\s*stage = \"slEvaluateFeature\";", src)
    assert m, "vk_dlss.cpp: slEvaluateFeature's result is no longer judged by dlss_drawn"
    assert '#include "dlss_result.h"' in src
    with tempfile.TemporaryDirectory(prefix="dlss-result-") as tmp:
        exe = Path(tmp) / ("dlss_result.exe" if os.name == "nt" else "dlss_result")
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-w", "-I" + str(ROOT / "gpu/vk"),
                        "-I" + str(ROOT / "third_party/streamline/include"),
                        str(ROOT / "tests/dlss_result_test.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
