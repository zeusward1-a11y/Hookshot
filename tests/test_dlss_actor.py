"""DLSS's actor motion (gpu/vk/vk_draw.cpp dlss_actor_pass, gpu/vk/dlss_actor.h; after #841's 8b925b6): the CPU side's draw names
and index addresses (tests/test_dlss_actor.cpp), this branch's plan (gpu/vk/dlss_actor_plan.h:
tests/test_dlss_actor_plan.cpp), the push constants of shaders/dlss_actor.vert and .frag against
dlss_actor::Push, the SPIR-V headers made from their sources as they are now (when glslangValidator is here), and the wiring in
this branch's DLSS, which differs from #841's: the camera unjittered (the jitter is a viewport offset, dlss_jitter_of), the
scene's depth and camera of the frame DLSS takes (ds.used), the pass between the motion pass and DLSS's evaluation.
tests/test_dlss_actor_gpu.py draws a skinned mesh through the shaders on a GPU.

python -I -B tests/test_dlss_actor.py [--cxx g++] [--glslang glslangValidator]
"""
import argparse
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SHADERS = ROOT / "gpu" / "vk" / "shaders"


def fields(text, start, end):
    """The field names of a block: uint a, b[2]; -> ['a', 'b[2]']"""
    body = text[text.index(start) + len(start):]
    body = body[:body.index(end)]
    body = re.sub(r"//[^\n]*", "", body)
    out = []
    for decl in body.split(";"):
        decl = decl.strip()
        if not decl:
            continue
        names = re.sub(r"^\s*(uint64_t|uint32_t|int32_t|uint|int)\s+", "", decl)
        out += [n.strip() for n in names.split(",")]
    return out


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


def wiring(failures):
    def need(cond, what):
        if not cond:
            failures.append(what)

    def order(text, marks, what):
        at = [text.find(m) for m in marks]
        need(all(x >= 0 for x in at) and at == sorted(at), what)

    draw = (ROOT / "gpu" / "vk" / "vk_draw.cpp").read_text(encoding="utf-8")
    passc = code(block(draw, "int dlss_actor_pass("))
    need("using ActorPush = dlss_actor::Push;" in draw and '#include "dlss_actor_plan.h"' in draw, "vk_draw.cpp does not use the plan")
    # this frame's draws, the two pose buffers taking turns (dlss_actor_plan.h: tests/test_dlss_actor_plan.cpp checks them)
    need("ds.skins.take(now)" in passc, "the pass takes draws noted in another frame (a fallback's: the n-ths shift)")
    need("here = ds.pose_buffers[ds.poses.here()]" in passc and "there = ds.pose_buffers[ds.poses.there()]" in passc,
         "this frame's poses and the frame before's are not the buffers Poses gives")
    order(passc, ["ds.poses.begin();", "ds.poses.place(sk.key, sk.nbones, now, &mine)", "ds.poses.end();"],
          "the poses are not begun, placed and ended in that order (the buffers would not take turns)")
    need("ds.poses.before_of(sk.key, sk.nbones, now, history)" in passc, "the frame before's pose is not taken by draw, bones and history")
    need("if (!ds.poses.place(sk.key, sk.nbones, now, &mine)) break;" in passc,
         "a full pose buffer leaves the pass before Poses::end (the next frame would read this frame's buffer as the one before)")
    need("vkCmdDraw(cmd, counts[i], 1, 0, 0);" in passc and "counts.push_back((uint32_t)k.count);" in passc,
         "a draw is not drawn with all of its indices")
    # the copies: this frame's pose, from where the game has it (the bones, the origin at the object's 0x40), where it was placed
    need("gpu_buffer_bind(k.bones, (uint64_t)sk.nbones * 48, false)" in passc and "gpu_buffer_bind(k.object + 0x40, 12, false)" in passc,
         "the pose is not copied from the palette and the object's 0x40")
    need("c0{origin.offset, mine, 12}" in passc and "c1{bones.offset, mine + 16, (uint64_t)sk.nbones * 48}" in passc and
         "vkCmdCopyBuffer(cmd, origin.buffer, here.buffer, 1, &c0);" in passc and
         "vkCmdCopyBuffer(cmd, bones.buffer, here.buffer, 1, &c1);" in passc,
         "the pose is not copied into this frame's buffer where it was placed (origin, bones 16 on)")
    order(passc, ["vkCmdCopyBuffer(cmd, bones.buffer", "full_barrier(cmd);", "vkCmdBeginRendering(cmd, &ri);"],
          "the draws may read the poses before their copies are done")
    need("dlss_actor::make_push(k, va, ia, here.address, mine, there.address, before->at, sk.nbones, tolerance, &a)" in passc,
         "the push constants are not make_push's with this frame's pose here and the frame before's there")
    need("const uint32_t tolerance = actor_tolerance();" in passc and 'dlss_actor::tolerance_of(getenv("RX_DLSS_ACTOR_TOL"))' in draw,
         "the depth tolerance is not RX_DLSS_ACTOR_TOL's (64)")
    # the cameras: unjittered (the scene's cbuf), this frame's jitter, in the recording's stream; the pass drawn with the scene's offset
    need("const bool jittered = ds.jit.used_frame == now;" in passc, "the pass takes a jitter this frame was not drawn with")
    need("dlss_actor::head(ds.used.cur.data(), ds.used.prev.data(), ux, uy, rw, rh," in passc,
         "the cameras are not the frame DLSS takes (this one's and the one before), or the jitter is not the viewport's")
    need("stream_alloc(dlss_actor::kHeadWords * 4, salign)" in passc and "bi{cur->stream, head_at," in passc,
         "the cameras' buffer is not in the recording's stream (a frame in flight could read it rewritten)")
    need("const VkViewport viewport{ux, uy, (float)rw, (float)rh, 0.0f, 1.0f};" in passc,
         "the pass is not drawn with the scene's jitter offset (its depths would not meet the scene's)")
    need("if (k.depth != ds.used.depth) continue;" in passc, "draws into another depth target are drawn again")
    need("ds.used.depth_view" in passc, "the scene's depth the fragment compares with is not DLSS's")
    need("last_dyn_valid = false, last_desc_valid = false, last_vb_valid = false, last_pipeline = VK_NULL_HANDLE;" in passc and
         "vertex_set.on = false, last_ib.on = false;" in passc,
         "the game's next draw may keep the actor pass's pipeline, viewport or descriptors")
    need("ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD" in passc, "the pass clears the camera's motion under it")
    pipe = code(block(draw, "bool make_actor_pipeline() {"))
    need("if (!dev.vk12.bufferDeviceAddress || !dev.features.shaderInt64) {" in pipe,
         "the pipeline is made on features the card has, not the ones the device was made with")
    need("ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;" in pipe and
         "rs.cullMode = VK_CULL_MODE_NONE" in pipe and "ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;" in pipe and
         "const VkFormat format = VK_FORMAT_R16G16_SFLOAT;" in pipe,
         "the pass's pipeline is not tests/dlss_actor_gpu.cpp's (a triangle list, no culling, R and G of R16G16F written)")
    # the motion image takes the pass, the pass sits between the motion pass and DLSS
    need("VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, cmd) ||   // (COLOR_ATTACHMENT: the actor pass draws into it)" in draw,
         "the motion image cannot be drawn into")
    dispatch = code(block(draw, "bool dlss_dispatch(VkCommandBuffer cmd, const NvnState& s, const Program& p) {"))
    mp, ap, ev = (dispatch.find(x) for x in ("dlss_motion_pass(cmd", "dlss_actor_pass(cmd", "ok = dlss_evaluate(cmd"))
    need(0 <= mp < ap < ev, "the actor pass is not between the motion pass and DLSS's evaluation")
    need("ds.last_frame != ~0ull && ds.last_frame + 1 == now" in dispatch[ap - 120:ap + 200], "the pass's history is not DLSS's")
    # the draws noted: in draw(), after the pipeline is known; not with the parallel replay; this frame's list
    d = code(block(draw, "void draw(VkCommandBuffer cmd, NvnState& s, const DrawArgs& d) {"))
    need(d.find("cur_program = &p;") < d.find("dlss_skin_check(s, p, d, at);"), "draw() does not note the skinned draws")
    chk = code(block(draw, "void dlss_skin_check(const NvnState& s, const Program& p, const DrawArgs& d, const Targets& at) {"))
    need("parallel_replay()" in chk and "!dlss_on()" in chk and "!actors_on()" in chk,
         "skinned draws are noted with DLSS off, the actors off or the parallel replay (ds is the replay thread's)")
    need("s.ubo[0][3]" in chk and "attr(4 + set), j = attr(6 + set)" in chk, "the skinned layout is not #841's (bones at binding 3)")
    need("ds.skins.next_key(model, now, &sk.key)" in chk and "const uint64_t now = texture_frame_no();" in chk,
         "a draw's n-th is not counted in its own frame's list")
    need("dlss_actor::fits(k)" in chk, "a layout whose attributes reach past a vertex is drawn (reads by address are not bounded)")
    need("if (d.prim != NVN_DRAW_PRIMITIVE_TRIANGLES) {" in chk, "strips or quads are drawn again as triangle lists")
    need("k.object = s.ubo[0][1].addr" in chk and "k.bones = bones.addr" in chk and "k.vbuf = s.vbuf[0].address" in chk and
         "k.ibuf = d.ibuf" in chk, "the origin is not binding 1's, or the vertices, indices or bones not the draw's own")
    need("if (j.fmt == NVN_FORMAT_RGBA8UI) k.formats |= dlss_actor::kByteJoints << set;" in chk,
         "a set's byte joints are not marked for that set")
    need("std::min<uint64_t>(k.bones_size / 48, 280)" in chk,
         "more bones are copied than the palette's buffer holds (a copy past the bound range)")
    bu = code(block(draw, "uint32_t bones_used(const DlssSkin& k) {"))
    need("most = std::max(most, joint + 1);" in bu and "if (!q[k.weights[set] + j]) continue;" in bu,
         "a model's bones are not its largest weighted joint plus one")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--glslang", default=shutil.which("glslangValidator"))
    args = parser.parse_args()
    header = (ROOT / "gpu" / "vk" / "dlss_actor.h").read_text(encoding="utf-8")
    want = fields(header, "struct Push {", "};")
    if len(want) != 16:
        raise SystemExit("dlss_actor::Push: %d fields read: %s" % (len(want), want))
    for name in ("dlss_actor.vert", "dlss_actor.frag"):
        got = fields((SHADERS / name).read_text(encoding="utf-8"), "uniform P {", "} pc;")
        if got != want:
            raise SystemExit("%s: its push constants %s are not dlss_actor::Push's %s" % (name, got, want))
    failures = []
    wiring(failures)
    with tempfile.TemporaryDirectory(prefix="dlss-actor-") as tmp:
        if args.glslang:
            for src in ("dlss_actor.vert", "dlss_actor.frag"):
                var = src.replace(".", "_").upper() + "_SPV"
                out = Path(tmp) / (src.replace(".", "_") + "_spv.h")
                subprocess.run([args.glslang, "-V", "--target-env", "vulkan1.3", "--vn", var, "-o", str(out), str(SHADERS / src)],
                               check=True, capture_output=True)
                words = lambda t: re.findall(r"0x[0-9a-f]{8}", t)
                if words(out.read_text()) != words((SHADERS / out.name).read_text()):
                    failures.append("shaders/%s is not made from %s as it is now" % (out.name, src))
        else:
            print("glslangValidator not found: the SPIR-V headers not checked")
        for name, inc in (("test_dlss_actor.cpp", []), ("test_dlss_actor_plan.cpp", ["-I" + str(ROOT / "gpu" / "vk")])):
            exe = Path(tmp) / name.replace(".cpp", ".exe")
            subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", *inc, str(ROOT / "tests" / name), "-o", str(exe)],
                           check=True)
            if subprocess.run([str(exe)]).returncode:
                failures.append("tests/%s failed" % name)
    if failures:
        for f in failures:
            print("FAIL:", f)
        raise SystemExit(f"{len(failures)} failures")
    print("PASS: DLSS actor motion's header, shaders and wiring")


if __name__ == "__main__":
    main()
