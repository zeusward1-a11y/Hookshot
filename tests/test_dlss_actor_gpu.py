"""DLSS's actor motion on a GPU (tests/dlss_actor_gpu.cpp): the real shaders (gpu/vk/shaders/dlss_actor_*_spv.h) draw skinned
draws over a jittered scene depth the way vk_draw.cpp dlss_actor_pass does, against the camera's motion pass on the CPU
(gpu/vk/dlss_objmv.h) of a world that moved with each draw; and three wrong wirings that must fail (no viewport offset, the
jitter's y the wrong way, the jitter not taken off).

Needs a GPU with buffer device addresses, 64-bit integers in shaders and dynamic rendering: tests/run_all.py skips it (run it by
name, or with --no-skip). No game.

    python -I -B tests/test_dlss_actor_gpu.py [--cxx clang++] --headers <Vulkan-Headers include folder>
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
    with tempfile.TemporaryDirectory(prefix="dlss-actor-gpu-") as tmp:
        exe = Path(tmp) / ("actor_gpu.exe" if os.name == "nt" else "actor_gpu")
        cmd = [a.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
               "-I" + str(ROOT / "gpu/vk"), "-I" + a.headers, str(ROOT / "tests/dlss_actor_gpu.cpp"), "-o", str(exe)]
        if os.name != "nt":
            cmd.append("-ldl")
        subprocess.run(cmd, check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
