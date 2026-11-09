"""A parallel replay initializes each new image before any segment's use of it, in queue order.

A parallel replay (RX_VK_PARALLEL) records a big command buffer's segments on several threads and submits them in order.
A new image's first layout change (UNDEFINED -> GENERAL, and its clear) was recorded into the upload command buffer of
the worker that made it. That is whichever worker got there first, not the first segment in the queue. An earlier
segment that used the image too (a copy into it again, the BC3 encoder's dispatch) was submitted first and found it
UNDEFINED (VUID-vkCmdDraw-None-09600). The BC3 scratch's first layout change had the same problem.

Cuts the production code (the upload command buffer state, create()'s initialization, bc_scratch_layout, the segment
hooks in vk_texture.cpp, and replay_handle's segment jobs and submits in vk_replay.cpp) into
tests/parallel_init_order_test.cpp. Vulkan is a spy that plays the submitted command buffers in queue order. Each case
runs the segments' workers in a set order, one after another on threads of their own: every use of an image must come
after its one UNDEFINED -> GENERAL change in the queue, and the serial path must record exactly the same calls as before.

python -I -B tests/test_parallel_init_order.py --cxx clang++ --headers path/to/Vulkan-Headers/include
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CASES = ("later_makes", "three", "scratch", "in_order", "serial")


def block(source, marker):
    if source.count(marker) != 1:
        raise SystemExit("marker not found once: " + marker)
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def cut(source, start, end, after=""):
    a = source.index(after) if after else 0
    if source.count(start, a) != 1:
        raise SystemExit("marker not found once: " + start)
    a = source.index(start, a)
    return source[a:source.index(end, a)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--headers", required=True, type=Path, help="the Vulkan headers' include folder")
    args = parser.parse_args()
    vk = ROOT / "gpu" / "vk"
    texture = (vk / "vk_texture.cpp").read_text(encoding="utf-8")
    replay = (vk / "vk_replay.cpp").read_text(encoding="utf-8")

    create = cut(texture, "total_bytes += x.bytes + x.pad;", "    static uint32_t ids;", after="Texture* create(")
    create = create[create.index("\n") + 1:]   # (from the line after: the image's initialization, to its Texture*)
    if "// into GENERAL, and cleared" not in create or "Texture* p = tex.get();" not in create:
        raise SystemExit("create()'s initialization moved: update this test")
    jobs = cut(replay, "    struct Job {", "    seg_ms += std::chrono::duration")
    for step in ("record_segment(c, j->g);", "texture_begin_recording(j->r->upload, j->r->value);", "vkQueueSubmit2(",
                 "j->uploads = texture_uploads_recorded();", "par_pool.cv.notify_all();"):
        if step not in jobs:
            raise SystemExit("the parallel block no longer has: " + step)
    hooks = [block(texture, m) for m in (
        "void texture_set_segment(uint64_t seg) {", "void texture_begin_recording(VkCommandBuffer upload, uint64_t value) {",
        "bool texture_uploads_recorded() {")]
    hooks += [block(texture, m) for m in ("void texture_segments_begin(", "void texture_segment_worker(", "bool texture_segment_end(")
              if m in texture]
    inc = {
        "init_upload.inc": cut(texture, "// ---- the recording's upload command buffer ----", "// ---- staging:"),
        "init_create.inc": create,
        "init_scratch.inc": block(texture, "bool bc_scratch_layout("),
        "init_hooks.inc": "\n".join(hooks),
        "init_recording.inc": block(replay, "struct Recording {") + ";",
        "init_jobs.inc": jobs,
    }
    compiler = shutil.which(args.cxx)
    if not compiler:
        raise SystemExit("C++ compiler not found: " + args.cxx)
    failed = []
    with tempfile.TemporaryDirectory(prefix="parallel-init-order-") as tmp:
        folder = Path(tmp)
        for name, text in inc.items():
            (folder / name).write_text(text + "\n", encoding="utf-8")
        binary = folder / "fixture.exe"
        command = [compiler, "-std=c++20", "-O1", "-w", "-I" + str(folder), "-I" + str(args.headers),
                   str(ROOT / "tests" / "parallel_init_order_test.cpp"), "-o", str(binary)]
        command += [] if os.name == "nt" else ["-pthread"]
        r = subprocess.run(command, capture_output=True, text=True, timeout=180)
        if r.returncode:
            print(r.stdout + r.stderr)
            raise SystemExit("the fixture did not build")
        for case in CASES:
            r = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=60)
            print(r.stdout, end="")
            print(r.stderr, end="", file=sys.stderr)
            if r.returncode:
                failed.append(case)
    if failed:
        raise SystemExit("FAIL: a segment used a new image before its initialization in the queue: " + ", ".join(failed))
    print("PASS: each new image is initialized once, before every use in queue order; serial calls are unchanged")


if __name__ == "__main__":
    main()
