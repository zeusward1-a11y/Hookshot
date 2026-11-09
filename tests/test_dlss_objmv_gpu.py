"""The own-motion shader (gpu/vk/shaders/dlss_motion_comp_spv.h) run on a Vulkan device against its CPU model (gpu/vk/dlss_objmv.h)
on the drawn scenes of tests/objmv_scene.h: vectors, distances, which pixels were made still, and the counts the log shows.

Needs a GPU and its Vulkan driver: tests/run_all.py skips it (run it by name, or with --no-skip). No game.

    python -I -B tests/test_dlss_objmv_gpu.py [--cxx clang++] --headers <Vulkan-Headers include folder>
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="clang++")
    p.add_argument("--headers", required=True)
    a = p.parse_args()
    with tempfile.TemporaryDirectory(prefix="dlss-objmv-gpu-") as tmp:
        exe = Path(tmp) / ("objmv_gpu.exe" if os.name == "nt" else "objmv_gpu")
        cmd = [a.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
               "-I" + str(ROOT / "gpu/vk"), "-I" + str(ROOT / "tests"), "-I" + a.headers, str(ROOT / "tests/dlss_objmv_gpu.cpp"),
               "-o", str(exe)]
        if os.name != "nt":
            cmd.append("-ldl")
        subprocess.run(cmd, check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
