"""DLSS's jitter (gpu/vk/vk_draw.cpp dlss_jitter_of, RX_DLSS_JITTER): the wiring, and the Halton sequence.

Ocarina's projection has no jitter (its view-projection at the scene context's +0x30 is the same to six digits from frame to frame
while the camera holds still, 09/11), so DLSS could only smooth the upscale from 3/4 of the output: soft in motion. The scene
camera's draws now take a sub-pixel viewport offset, Halton(2,3), only those whose vertex constant buffer 3 is the scene context
and whose target is the scene's size (full-screen passes, the HUD and shadow maps keep the game's viewport: NX2navi jittered every
pass with the scene depth in Ocarina and the picture went white), only after a frame DLSS made, held off for 8 frames after a
jittered frame it did not make (60 when another came within two seconds). The jitter is decided before dynamic_state's shortcut
and is part of its key, so a draw never keeps the viewport of the one before it (the review of 09/11). DLSS and
the motion pass get the offset this frame's draws took; the motion pass's handling of it is checked on scenes by
tests/test_dlss_objmv.py (jittered_draw) and on the GPU by tests/test_dlss_objmv_gpu.py (jittered).

    python -I -B tests/test_dlss_jitter.py [--cxx clang++]
"""
import argparse
import re
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


def halton(i, b):   # (vk_draw.cpp halton, the same loop)
    f, r = 1.0, 0.0
    while i:
        f /= b
        r += f * (i % b)
        i //= b
    return r


def run_hold_test(cxx):
    """tests/dlss_jitter_test.cpp: the hold frame by frame (menu, size change, misses)"""
    with tempfile.TemporaryDirectory(prefix="dlss-jitter-") as d:
        exe = Path(d) / "jitter.exe"
        subprocess.run([cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "gpu/vk"),
                        str(ROOT / "tests/dlss_jitter_test.cpp"), "-o", str(exe)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        print(r.stdout, end="")
        return r.returncode == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cxx", default="clang++")
    a = ap.parse_args()
    failures = []

    def need(cond, what):
        if not cond:
            failures.append(what)

    draw = read("gpu/vk/vk_draw.cpp")
    dyn = code(block(draw, "void dynamic_state(VkCommandBuffer cmd,"))
    vp = dyn.index("d.vp = {vx, vy + vh, vw, -vh,")
    decided = dyn.find("const bool jittered = dlss_jitter_of(s, at, jx, jy);")
    keyed = dyn.find("df[0] += jx, df[1] += jy;")
    fast = dyn.find("dyn_same(df, kDynF")
    j = dyn.find("if (jittered) d.vp.x += jx, d.vp.y += jy;")
    need(0 <= decided < keyed < fast, "the jitter is not decided before dynamic_state's shortcut, or not in its key (a draw keeps "
         "the viewport of the one before it)")
    need("fp[0] = s.viewport[0][0], fp[1] = s.viewport[0][1]" in code(block(draw, "int dyn_pack(")),
         "the shortcut's key does not start with the viewport's x and y (where the jitter is added)")
    need(vp < j, "the draws' viewport does not take the jitter after it is set")
    need(dyn.count("dlss_jitter_of(") == 1, "the jitter is asked twice for one draw")

    of = code(block(draw, "bool dlss_jitter_of(const NvnState& s, const Targets& at, float& x, float& y) {"))
    need("!jitter_mode() || !dlss_on()" in of, "the jitter does not stop with RX_DLSS_JITTER=0 or DLSS off")
    need("at.depth_format != VK_FORMAT_D32_SFLOAT" in of, "draws without the scene's depth format are jittered")
    need("at.width != ds.used.w || at.height != ds.used.h" in of, "draws into targets of another size (shadow maps) can be jittered")
    need("parallel_replay()" in of and 'atoi(getenv("RX_VK_PARALLEL")) >= 2' in code(block(draw, "bool parallel_replay() {")), "the jitter runs with the parallel replay (ds.jit is the replay thread's)")
    # the scene context only: dlss_note_camera's signature, the target's size at +0x120 and -1, 0 at +0xa8
    need("ub_read(u.addr + 0xa8, ab, sizeof ab)" in of and "ub_read(u.addr + 0x120, wh, sizeof wh)" in of and
         "ab[0] == -1.0f && ab[1] == 0.0f" in of and "wh[0] == (float)at.width && wh[1] == (float)at.height" in of,
         "the jittered draws are not the scene context's alone")
    need("const nvn_p_bind_buf& u = s.ubo[0][0];" in of, "the block checked is not the vertex stage's constant buffer 3")
    need("ds.last_frame == ~0ull || ds.last_frame + 1 != now" in of, "a frame is jittered without DLSS having made the one before")
    # (the hold's logic is dlss_jitter.h, run by tests/dlss_jitter_test.cpp below: here, that vk_draw.cpp drives it as the test does)
    start, held = of.find("j.hold.frame_start(now)"), of.find("j.hold.held()")
    need(0 <= start < held, "the hold is not counted at the frame's start before it is asked")
    need("if (j.used_frame != now) j.used_frame = now, j.ux = x, j.uy = y;" in of and "j.hold.used(now);" in of,
         "the offset a frame took is not kept for DLSS, or the hold does not know the frame was jittered")

    nx = code(block(draw, "void next_jitter(uint32_t rw, uint32_t ow) {"))
    need("std::clamp((uint32_t)std::ceil(8.0 * r * r - 1e-6), 16u, 72u)" in nx, "the phases are not NVIDIA's 8 x ratio^2 within 16-72")
    need("halton(i, 2) - 0.5f" in nx and "halton(i, 3) - 0.5f" in nx and "% ds.jit.phases + 1" in nx,
         "the offsets are not Halton(2,3) from index 1, centred")

    dispatch = code(block(draw, "bool dlss_dispatch(VkCommandBuffer cmd, const NvnState& s, const Program& p) {"))
    ev = dispatch.index("ok = dlss_evaluate(cmd")
    give = dispatch.find("view.jitter[0] = sign * ds.jit.ux, view.jitter[1] = sign * ds.jit.uy;")
    need(0 <= give < ev and "if (ds.jit.used_frame == now) {" in dispatch[:ev],
         "DLSS is not given the offset this frame's draws took (and only that frame's)")
    nxt = dispatch.find("next_jitter(rw, ow);")
    need(ev < nxt and "if (ok) {" in dispatch[ev:nxt], "the jitter moves on without a frame DLSS made")
    made = dispatch.find("ds.jit.hold.made(now);")
    need(ev < made and "if (ok) {" in dispatch[ev:made], "the hold is not told the frames DLSS made (every jittered one is a miss)")

    passc = code(block(draw, "void dlss_motion_pass("))
    need("if (ds.jit.used_frame == now) push[20] = ds.jit.ux, push[21] = ds.jit.uy;" in passc,
         "the motion pass does not take the offset this frame's draws took")
    need("VK_SHADER_STAGE_COMPUTE_BIT, 0, 112}" in draw, "the motion pass's layout does not push 112 bytes (the jitter, the mask, last frame's jitter)")
    shader = read("gpu/vk/shaders/dlss_motion.comp")
    need("vec2 screen = (vec2(pixel) + 0.5 - params.jitter.xy) / size;" in code(shader),
         "the motion pass does not take each pixel where an unjittered draw puts it")

    dlss = code(read("gpu/vk/vk_dlss.cpp"))
    need("c.jitterOffset = {view ? view->jitter[0] : 0.0f, view ? view->jitter[1] : 0.0f};" in dlss,
         "Streamline's constants do not carry the jitter")
    need("c.motionVectorsJittered = sl::Boolean::eFalse;" in dlss, "the vectors are not said to be unjittered")
    need('getenv("RX_DLSS_JITTER")' in code(block(draw, "int jitter_mode() {")), "RX_DLSS_JITTER does not set it")

    # Halton(2,3) from 1: a low-discrepancy cover of the pixel, centred (-0.5..0.5), 15 phases at Quality's 4/3 -> 16
    pts = [(halton(i, 2) - 0.5, halton(i, 3) - 0.5) for i in range(1, 17)]
    need(all(-0.5 <= x < 0.5 and -0.5 <= y < 0.5 for x, y in pts), "the offsets leave the pixel")
    need(abs(sum(x for x, _ in pts) / 16) < 0.05 and abs(sum(y for _, y in pts) / 16) < 0.05, "the offsets are not centred")
    quads = {(x >= 0, y >= 0) for x, y in pts[:4]}
    need(len(quads) >= 3, "the first offsets do not spread over the pixel")
    need(max(16, min(72, -(-int(8 * (4 / 3) ** 2 * 1000 - 1) // 1000))) == 16, "Quality's 4/3 does not give 16 phases")

    need(run_hold_test(a.cxx), "the hold's frame-by-frame test failed (tests/dlss_jitter_test.cpp)")

    if failures:
        for f in failures:
            print("FAIL:", f)
        raise SystemExit(f"{len(failures)} failures")
    print("PASS: DLSS jitter wiring and sequence")


if __name__ == "__main__":
    main()
