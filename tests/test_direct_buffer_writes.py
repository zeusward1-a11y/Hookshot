"""Actual direct-buffer binding/write-page bookkeeping with authored pools; no Vulkan/game required."""
import argparse
import re
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CASES = ("mirror_off", "upload_off", "refused", "virtual_write", "large_write", "no_recording",
         "wait_disabled", "readonly", "mirror_pending", "mirror_on", "invalid_pool", "invalid_mapping")


def block(source, signature, semicolon=False):
    start = source.index(signature)
    first = source.index("{", start)
    depth, at = 1, first + 1
    while depth:
        if source[at] == "{":
            depth += 1
        elif source[at] == "}":
            depth -= 1
        at += 1
    if semicolon:
        assert source[at] == ";"
        at += 1
    return source[start:at]


def without_rb_direct(bind):
    """gpu_buffer_bind without #1150's read-back ranges bound in guest memory (tests/test_rb_direct.py's): here as with
    RX_VK_RB_DIRECT=0, its block left out"""
    head = "    if (rb_direct_on()) {"
    if head not in bind:
        return bind
    start = bind.index(head)
    end = bind.index("{", start) + 1
    depth = 1
    while depth:
        depth += (bind[end] == "{") - (bind[end] == "}")
        end += 1
    return bind[:start] + "    /* (#1150's read-back direct binding: tests/test_rb_direct.py) */" + bind[end:]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++"), help="GCC/Clang C++ compiler")
    parser.add_argument("--source", type=Path, default=ROOT / "gpu/vk/vk_memory.cpp",
                        help="Public vk_memory.cpp override for an original-source negative control")
    args = parser.parse_args()
    cxx = shutil.which(args.cxx)
    if not cxx:
        raise SystemExit("C++ compiler not found: " + args.cxx)
    source = args.source.read_text(encoding="utf-8")
    header = (ROOT / "gpu/vk/vk_internal.h").read_text(encoding="utf-8")
    types = "\n\n".join([block(header, "struct BufferRef {", True), block(source, "struct Pool {", True),
                             block(source, "struct TlPool {", True), block(source, "struct WriteBack {", True)])
    pages = source[source.index("std::mutex wb_pages_mu;"):source.index("void wb_pages_note(")]
    if "kChunkPages" in pages:   # (#1036: the table's sizes, vk_memory.cpp's page constants)
        chunk = re.search(r"kChunkPages = ([^,;]+)[,;]", source).group(1)
        whole = re.search(r"constexpr uint64_t kPages = ([^;]+);", source).group(1)
        pages = "constexpr uint64_t kChunkPages = %s, kPages = %s;\n" % (chunk, whole) + pages
    actual = "\n\n".join([pages, block(source, "void wb_pages_note("), block(source, "uint64_t memory_writeback_value("),
        block(source, "uint64_t gpu_bind_stamp("), block(source, "uint32_t bind_slot("), block(source, "bool bind_memo_hit("),
        block(source, "struct BindMemo {", True), "thread_local BindMemo bind_memo[256];",
        without_rb_direct(block(source, "BufferRef gpu_buffer_bind("))])
    flags = subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS if os.name == "nt" else 0
    env = dict(os.environ)
    env["PATH"] = str(Path(cxx).parent) + os.pathsep + env.get("PATH", "")
    with tempfile.TemporaryDirectory(prefix="hookshot-direct-writes-") as folder:
        tmp = Path(folder)
        (tmp / "direct_write_types.inc").write_text(types + "\n", encoding="utf-8")
        (tmp / "direct_write_actual.inc").write_text(actual + "\n", encoding="utf-8")
        exe = tmp / ("direct_writes.exe" if os.name == "nt" else "direct_writes")
        command = [cxx, "-std=c++20", "-O0", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS",
                   "-I" + str(tmp), str(ROOT / "tests/direct_buffer_writes_test.cpp"), "-o", str(exe)]
        result = subprocess.run(command, cwd=tmp, env=env, capture_output=True, text=True, timeout=60,
                                creationflags=flags)
        if result.returncode:
            print(result.stdout + result.stderr)
            return result.returncode
        failed = 0
        for case in CASES:
            result = subprocess.run([str(exe), case], cwd=tmp, env=env, capture_output=True, text=True,
                                    timeout=10, creationflags=flags)
            print((result.stdout + result.stderr).strip())
            failed += result.returncode != 0
        print("direct buffer writes: %d cases, %d failed" % (len(CASES), failed))
        return int(failed != 0)


if __name__ == "__main__":
    raise SystemExit(main())
