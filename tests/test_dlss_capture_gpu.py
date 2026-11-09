"""DLSS's captured motion on a GPU (tests/dlss_capture_gpu.cpp): a vertex stage patched by gpu/vk/spirv_xfb.h records its clip
position by transform feedback (the game translator's shape and glslang's), and the motion pass (shaders/dlss_capture.vert with
dlss_actor.frag) over two frames of a swaying patch of grass gives the motion of what the shader moved, against the camera
motion pass's CPU model; wrong wirings fail.

Needs a GPU with transform feedback and glslangValidator: tests/run_all.py skips it (run it by name). No game.

    python -I -B tests/test_dlss_capture_gpu.py [--cxx clang++] --headers <Vulkan-Headers include folder> [--glslang glslangValidator]
"""
import argparse
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# (a "game" vertex shader whose wind moves the vertices: the view-projection and the wind's offset in a uniform buffer; the depth
# converted from NVN's -1..1 as the game translator's convert_depth_mode does, y up as the game's clip space)
WIND = """#version 450
layout(location = 0) in vec3 p;
layout(binding = 0) uniform U { mat4 vp; vec4 wind; } u;
void main() {
    vec4 c = u.vp * vec4(p + u.wind.xyz, 1.0);
    gl_Position = vec4(c.x, c.y, (c.z + c.w) * 0.5, c.w);
}
"""


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="clang++")
    p.add_argument("--headers", required=True)
    p.add_argument("--glslang", default=shutil.which("glslangValidator"))
    a = p.parse_args()
    if not a.glslang:
        raise SystemExit("glslangValidator not found")
    with tempfile.TemporaryDirectory(prefix="dlss-capture-gpu-") as tmp:
        tmp = Path(tmp)
        vert, spv = tmp / "wind.vert", tmp / "wind.spv"
        vert.write_text(WIND, encoding="utf-8")
        subprocess.run([a.glslang, "-V", "--target-env", "vulkan1.3", "-o", str(spv), str(vert)], check=True, capture_output=True)
        exe = tmp / ("capture_gpu.exe" if os.name == "nt" else "capture_gpu")
        cmd = [a.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
               "-I" + str(ROOT / "gpu/vk"), "-I" + a.headers, str(ROOT / "tests/dlss_capture_gpu.cpp"), "-o", str(exe)]
        if os.name != "nt":
            cmd.append("-ldl")
        subprocess.run(cmd, check=True)
        subprocess.run([str(exe), str(spv)], check=True)


if __name__ == "__main__":
    main()
