"""Own motion for DLSS and DLSS Frame Generation (gpu/vk/dlss_objmv.h, the motion pass's model; shaders/dlss_motion.comp, the pass).

Ocarina's frame has no per-object motion: the vectors were the camera's alone, from depth, and the character the camera follows
(Link) trailed in DLSS's history and doubled in generated frames. After NX2navi's d9fb0fe, each pixel's view distance is kept for
the next frame and the camera's vector is checked against it; what kept its place on screen gets no motion.

The fixture (tests/dlss_objmv_test.cpp) draws scenes by casting rays and runs the model: Link running away with the camera behind
him (all his pixels made still, the wall untouched), a strafe (only the trailing band: the method's limit), a still world under a
moving, turning camera (nothing taken for motion), the check off and the first frame (the camera's motion exactly). Then the
wiring in vk_draw.cpp, the shader's tolerance against the model's, and (with glslangValidator) the committed SPIR-V header against
the shader's source. tests/test_dlss_objmv_gpu.py runs the shader itself against the model on a GPU.

    python -I -B tests/test_dlss_objmv.py [--cxx clang++] [--glslang glslangValidator]
"""
import argparse
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(rel):
    return (ROOT / rel).read_text(encoding="utf-8")


def code(text):
    return re.sub(r"//[^\n]*", "", text)


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def glslang(asked):
    for c in (asked, os.environ.get("HOOKSHOT_GLSLANG"), shutil.which("glslangValidator"),
              r"C:\msys64\ucrt64\bin\glslangValidator.exe" if os.name == "nt" else None):
        if c and os.path.isfile(c):
            return c
    return None


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="clang++")
    p.add_argument("--glslang")
    a = p.parse_args()
    failures = []

    def need(cond, what):
        if not cond:
            failures.append(what)

    draw = read("gpu/vk/vk_draw.cpp")
    dispatch = code(block(draw, "bool dlss_dispatch(VkCommandBuffer cmd, const NvnState& s, const Program& p) {"))
    call = dispatch.find("dlss_motion_pass(cmd, rw, rh, clip, now, !same_camera);")
    need(0 <= call < dispatch.find("dlss_evaluate(cmd"), "the motion pass does not run before DLSS's evaluation, or without the camera cut")
    need("objmv_log();" in dispatch, "no own-motion line in the 600-frame log")
    passc = code(block(draw, "void dlss_motion_pass("))
    need("objmv::depth_terms(ds.used.cur.data(), a, b)" in passc and "objmv_on()" in passc, "the pass has no depth terms or no switch")
    need("ds.dist_frame + 1 == now && ds.dist_size == size" in passc and "!cut" in passc,
         "last frame's distances are used without being last frame's, at this size, after no cut")
    need("push[16] = a, push[17] = b, push[18] = on ? (count ? 2.0f : 1.0f) : 0.0f, push[19] = prev_ok ? 1.0f : 0.0f;" in passc,
         "the push constants are not the shader's layout")
    # the counts into host memory: only on the frames the log counts (every frame cost GPU time), and they are those frames
    need("const bool count = on && now % kObjmvCountEvery == 0;" in passc and "ds.objmv_frames += prev_ok && count;" in passc,
         "the own-motion counts are added every frame, or the log's frames are not the counted ones")
    need('getenv("RX_DLSS_OBJMV")' in code(block(draw, "bool objmv_on(")), "RX_DLSS_OBJMV does not turn it off")
    need(re.search(r"dlss_image\(ds\.dist\[0\], ds\.cam\.w, ds\.cam\.h, VK_FORMAT_R32_SFLOAT", draw) and
         re.search(r"dlss_image\(ds\.dist\[1\], ds\.cam\.w, ds\.cam\.h, VK_FORMAT_R32_SFLOAT", draw),
         "the distance images are not made with the motion image")
    shader = read("gpu/vk/shaders/dlss_motion.comp")
    tol = re.search(r"const float TOLERANCE = ([0-9.]+);", shader)
    model = re.search(r"constexpr float kTolerance = ([0-9.]+)f;", read("gpu/vk/dlss_objmv.h"))
    need(tol and model and float(tol.group(1)) == float(model.group(1)), "the shader's tolerance is not the model's")
    sky = re.search(r"const float SKY_DEPTH = ([0-9.]+);", shader)
    sky_model = re.search(r"constexpr float kSkyDepth = ([0-9.]+)f;", read("gpu/vk/dlss_objmv.h"))
    need(sky and sky_model and float(sky.group(1)) == float(sky_model.group(1)), "the shader's sky depth is not the model's")
    cshader = code(shader)
    need("valid && depth < SKY_DEPTH" in cshader, "the shader checks the sky for own motion")
    need("params.depth.z > 1.5 && gl_LocalInvocationIndex == 0" in cshader, "the shader adds to the host counts every frame")
    need("layout(binding = 2, r32f) uniform readonly image2D dist_prev;" in shader and
         "layout(binding = 4, std430) buffer Counts" in shader, "the shader's bindings are not the pass's")

    g = glslang(a.glslang)
    with tempfile.TemporaryDirectory(prefix="dlss-objmv-") as tmp:
        if g:   # (the committed header is what the shader's source makes: glslangValidator 16.6, as the others were)
            out = Path(tmp) / "dlss_motion_comp_spv.h"
            subprocess.run([g, "-V", "--target-env", "vulkan1.3", "--vn", "DLSS_MOTION_COMP_SPV", "-o", str(out),
                            str(ROOT / "gpu/vk/shaders/dlss_motion.comp")], check=True, stdout=subprocess.DEVNULL)
            want = (ROOT / "gpu/vk/shaders/dlss_motion_comp_spv.h").read_bytes().replace(b"\r\n", b"\n")
            need(out.read_bytes().replace(b"\r\n", b"\n") == want, "dlss_motion_comp_spv.h is not what dlss_motion.comp makes")
        else:
            print("(no glslangValidator: the SPIR-V header is not checked against the source)")
        exe = Path(tmp) / ("objmv.exe" if os.name == "nt" else "objmv")
        subprocess.run([a.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "gpu/vk"), "-I" + str(ROOT / "tests"),
                        str(ROOT / "tests/dlss_objmv_test.cpp"), "-o", str(exe)], check=True)
        r = subprocess.run([str(exe)])
    for f in failures:
        print("FAIL:", f)
    if failures or r.returncode:
        raise SystemExit("%d wiring failures, fixture exit %d" % (len(failures), r.returncode))
    print("PASS: own motion")


if __name__ == "__main__":
    main()
