"""DLSS's captured motion (gpu/vk/vk_draw.cpp dlss_capture_draw and dlss_capture_pass, gpu/vk/spirv_xfb.h, shaders/dlss_capture.vert,
dlss_actor_plan.h CaptureRing): the wiring in vk_draw.cpp, vk_main.cpp, vk_loader.h and vk_shader.cpp, the capture shader's push
constants against dlss_actor::Push (dlss_actor.frag reads tolerance from them), its SPIR-V header made from its source as it is
now (when glslangValidator is here). tests/test_spirv_xfb.py checks the patch, tests/test_dlss_actor_plan.cpp the ring,
tests/test_dlss_capture_gpu.py all of it on a GPU.

python -I -B tests/test_dlss_capture.py [--glslang glslangValidator]
"""
import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
import test_dlss_actor as actor  # noqa: E402  (its readers of blocks and push constant fields)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--glslang", default=shutil.which("glslangValidator"))
    a = p.parse_args()
    failures = []

    def need(cond, what):
        if not cond:
            failures.append(what)

    def order(text, marks, what):
        at = [text.find(m) for m in marks]
        need(all(x >= 0 for x in at) and at == sorted(at), what)

    vk = ROOT / "gpu" / "vk"
    draw = (vk / "vk_draw.cpp").read_text(encoding="utf-8")
    # the shader's push constants are dlss_actor::Push's (the fragment stage is dlss_actor.frag)
    want = actor.fields((vk / "dlss_actor.h").read_text(encoding="utf-8"), "struct Push {", "};")
    got = actor.fields((vk / "shaders" / "dlss_capture.vert").read_text(encoding="utf-8"), "uniform P {", "} pc;")
    need(got == want, f"dlss_capture.vert's push constants {got} are not dlss_actor::Push's {want}")
    # the draw recorded: after the game's indexed and plain draws, the same draw, transform feedback around it
    d = actor.code(actor.block(draw, "void draw(VkCommandBuffer cmd, NvnState& s, const DrawArgs& d) {"))
    need(d.count("dlss_capture_draw(cmd, s, p, d, at, pe, topo);") == 2, "the capture is not after both of the game's direct draws")
    calls = [m.start() for m in re.finditer(re.escape("dlss_capture_draw(cmd, s, p, d, at, pe, topo);"), d)]
    di, dp = d.find("vkCmdDrawIndexed(cmd, (uint32_t)d.count"), d.find("vkCmdDraw(cmd, (uint32_t)d.count")
    need(len(calls) == 2 and 0 <= di < calls[0] < dp < calls[1], "the capture is not right after each of the game's draws")
    cap = actor.code(actor.block(draw, "/// From draw(), right after the game's draw: the same draw again"))
    for cond, what in (("!capture_on()", "RX_DLSS_CAPTURE off"), ("!dev.xfb", "a device without transform feedback"),
                       ("skin_noted", "the skinned draws (the actor pass's)"),
                       ("p.mesh || p.fallback || d.indirect", "mesh programs and indirect draws (a guard)"),
                       ("if (p.vertex_spirv.empty()) return void(cs.no_stage++);", "a program without its vertex stage kept"),
                       ("topo != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST", "strips and fans (their records' count)"),
                       ("parallel_replay()", "the parallel replay (the ring is the replay thread's)"), ("!dlss_on()", "DLSS off")):
        need(cond in cap, f"the capture is not kept from {what}")
    # (every draw of the scene takes its n-th before the checks that leave one out for a while: the next ones keep theirs)
    order(cap, ["if (!at.depth || at.depth != ds.cam.depth) {", "const uint64_t key = cs.ring.key_of(",
                "if (topo != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST) return void(cs.not_list++);",
                "if (!capture_camera_draw(s, at)) return void(cs.not_camera++);", "cs.waiting++;", "if (!cp) return void(cs.no_pipeline++);",
                "cs.ring.place(key, count, now, &first, (uint64_t)(uintptr_t)at.depth)"],
          "a draw's key is not taken before the checks that may leave it out (the n-ths of the next ones shift)")
    need("jobs_push_serial([job] {" in cap, "the capture's pipelines are built on the game's workers (its own builds wait)")
    need("~Done() { e->capture_state.store(2, std::memory_order_release); }" in cap,
         "a capture pipeline whose build throws stays waiting for ever")
    need("cs.ring.place(key, count, now, &first, (uint64_t)(uintptr_t)at.depth)" in cap, "the records are not tagged with their target")
    # (the real exits of what cannot be recorded, counted for the log: the mesh draws (the field's grass), indirect, quads)
    for mark, what in (("if (d.mesh) {\n        dlss_capture_left_out(at, 0);", "mesh draws"),
                       ("if (d.indirect) {\n        dlss_capture_left_out(at, 1);", "indirect draws"),
                       ("dlss_capture_left_out(at, 2);", "quads")):
        need(mark in d, f"the {what} into the scene's depth are not counted where draw() leaves them out")
    # (off by default: RX_DLSS_CAPTURE=1 turns it on)
    need('static const bool on = getenv("RX_DLSS_CAPTURE") && std::strcmp(getenv("RX_DLSS_CAPTURE"), "0");' in draw,
         "the capture is not off by default")
    need("if (!at.depth || at.depth != ds.cam.depth) {" in cap and "return void(cs.not_scene++);" in cap and "other_too" not in cap,
         "draws into another target than the scene's are captured")
    need("if (!capture_camera_draw(s, at)) return void(cs.not_camera++);" in cap,
         "draws without the scene camera's block (full-screen passes: the sky's corners do not move) are captured")
    order(cap, ["vkCmdSetRasterizerDiscardEnable(cmd, VK_TRUE);", "vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cp);",
                "vkCmdBindTransformFeedbackBuffersEXT(cmd, 0, 1, &cs.buffer, &off, &size);", "vkCmdBeginTransformFeedbackEXT(cmd",
                "vkCmdDrawIndexed(cmd, (uint32_t)d.count", "vkCmdEndTransformFeedbackEXT(cmd",
                "vkCmdSetRasterizerDiscardEnable(cmd, last_dyn_valid ? last_dyn.discard : VK_FALSE);", "last_pipeline = VK_NULL_HANDLE;"],
          "the recorded draw is not: rasterizer off, its pipeline, the buffer, feedback on, the game's draw again, feedback off, "
          "the rasterizer and pipeline back")
    need("cs.ring.place(key, count, now, &first, (uint64_t)(uintptr_t)at.depth)" in cap and "off = (VkDeviceSize)first * 16, size = (VkDeviceSize)count * 16" in cap,
         "the draw's records are not where the ring placed them")
    need("const uint32_t count = (uint32_t)(d.count - d.count % 3) * (uint32_t)std::max(1, d.instances);" in cap,
         "the records' count is not the draw's whole triangles (what transform feedback writes of each instance)")
    need("build_graphics(*job->p, job->s, job->at, job->topo, -1, m)" in cap and "jobs_push_serial(" in cap,
         "the capture's pipeline is not the game's built whole, in the background, with the patched vertex stage")
    bg = actor.code(actor.block(draw, "VkPipeline build_graphics(const Program& p, const NvnState& s, const Targets& at, VkPrimitiveTopology topo, int part,"))
    need("si.module = vs_override && st.vk_stage == VK_SHADER_STAGE_VERTEX_BIT ? vs_override : st.module;" in bg,
         "build_graphics does not take the capture's vertex stage")
    mod = actor.code(actor.block(draw, "VkShaderModule capture_module(const Program& p) {"))
    need("spirv_xfb::patch(p.vertex_spirv, xfb, &why)" in mod and "std::lock_guard lk(cs.modules_mu);" in mod,
         "the vertex stage is not patched once a program under its lock")
    # the pass: after the actor pass, before DLSS; the scene's viewport and the jitter; the frame before's records
    dispatch = actor.code(actor.block(draw, "bool dlss_dispatch(VkCommandBuffer cmd, const NvnState& s, const Program& p) {"))
    order(dispatch, ["dlss_motion_pass(cmd", "dlss_actor_pass(cmd", "dlss_capture_pass(cmd, rw, rh, now, clip)", "ok = dlss_evaluate(cmd"],
          "the capture pass is not after the actor pass and before DLSS's evaluation")
    need("if (dlss_capture_pass(cmd, rw, rh, now, clip)) full_barrier(cmd);" in dispatch,
         "no barrier after the pass (the next frame's records overwrite the half it read)")
    ps = actor.code(actor.block(draw, "int dlss_capture_pass(VkCommandBuffer cmd, uint32_t rw, uint32_t rh, uint64_t now, const Mat& to_prev) {"))
    need("cs.ring.before_of(e.key, e.count, now)" in ps and "a.count = e.count, a.first = e.at, a.base_vertex = (int32_t)was->at" in ps,
         "the pass does not draw this frame's records with the frame before's of the same draw")
    need("if (e.target != target) {" in ps and "if (!was || was->target != target) {" in ps and
         "const uint64_t target = (uint64_t)(uintptr_t)ds.used.depth;" in ps,
         "the pass draws records of draws into another depth image than DLSS's")
    need("const VkViewport viewport{ux, uy + (float)rh, (float)rw, -(float)rh, 0.0f, 1.0f};" in ps,
         "the pass is not drawn with the scene's viewport (negative height, the jitter's offset)")
    need("dlss_actor::capture_head(ds.used.cur.data(), ds.used.prev.data(), ux, uy, rw, rh, to_prev.data(), capture_jump()," in ps and
         "const bool jittered = ds.jit.used_frame == now;" in ps,
         "the jitter the fragment takes off is not the scene's, or the jumps' test has not the cameras' motion and its limit")
    need("bi{cur->stream, head_at, dlss_actor::kCaptureHeadWords * 4}" in ps, "the head bound is not the capture's whole head")
    order(ps, ["full_barrier(cmd);", "vkCmdBeginRendering(cmd, &ri);"], "the records may be read before they are written")
    need("ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD" in ps and "ds.used.depth_view" in ps, "the pass clears the motion, or compares another depth")
    need("last_dyn_valid = false, last_desc_valid = false, last_vb_valid = false, last_pipeline = VK_NULL_HANDLE;" in ps,
         "the game's next draw may keep the pass's state")
    # the device, the loader, the kept vertex stages
    main_ = (vk / "vk_main.cpp").read_text(encoding="utf-8")
    need("optional(VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME, dev.xfb);" in main_ and "dev.xfb = dev.xfb && xfbf.transformFeedback;" in main_ and
         "xfbf.geometryStreams = VK_FALSE;" in main_, "transform feedback not asked of the device as the capture needs it")
    need('if (opts.dlss && getenv("RX_DLSS_CAPTURE") && strcmp(getenv("RX_DLSS_CAPTURE"), "0"))' in main_,
         "transform feedback asked for without RX_DLSS_CAPTURE=1, or with DLSS off")
    loader = (vk / "vk_loader.h").read_text(encoding="utf-8")
    need(all(f in loader for f in ("vkCmdBindTransformFeedbackBuffersEXT", "vkCmdBeginTransformFeedbackEXT", "vkCmdEndTransformFeedbackEXT")),
         "the transform feedback commands are not loaded")
    shader = (vk / "vk_shader.cpp").read_text(encoding="utf-8")
    need(shader.count("capture_keeps_spirv()") >= 3 and "p->vertex_spirv = std::move(spirv);" in shader and "p.vertex_spirv = vspirv;" in shader,
         "the vertex stages are not kept (from the cache and as translated)")
    need("return keep && dev.xfb && dlss_available();" in shader, "the vertex stages are kept where the capture cannot run")
    if a.glslang:
        with tempfile.TemporaryDirectory(prefix="dlss-capture-") as tmp:
            out = Path(tmp) / "dlss_capture_vert_spv.h"
            subprocess.run([a.glslang, "-V", "--target-env", "vulkan1.3", "--vn", "DLSS_CAPTURE_VERT_SPV", "-o", str(out),
                            str(vk / "shaders" / "dlss_capture.vert")], check=True, capture_output=True)
            words = lambda t: re.findall(r"0x[0-9a-f]{8}", t)
            need(words(out.read_text()) == words((vk / "shaders" / "dlss_capture_vert_spv.h").read_text()),
                 "shaders/dlss_capture_vert_spv.h is not made from dlss_capture.vert as it is now")
    else:
        print("glslangValidator not found: the SPIR-V header not checked")
    if failures:
        for f in failures:
            print("FAIL:", f)
        raise SystemExit(f"{len(failures)} failures")
    print("PASS: DLSS's captured motion: its wiring and shader")


if __name__ == "__main__":
    main()
