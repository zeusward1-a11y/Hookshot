"""gpu/vk/spirv_xfb.h: a vertex stage's SPIR-V made to capture its clip position (transform feedback) for DLSS's captured
motion. On the two shapes of a Position output: a variable of its own (as the game's translator, eden's spirv_emit_context.cpp
DefineOutput, makes it; assembled here) and a member of glslang's gl_PerVertex block (when glslangValidator is here): the
capability once among the capabilities, the Xfb mode on the vertex entry point after the entry points and modes, XfbBuffer 0,
XfbStride 16 and Offset 0 on the output (or its block member) in the annotations, every other word as it was; and what is
refused: not SPIR-V, no vertex entry point, no Position, already capturing. tests/dlss_capture_gpu.cpp runs the patched
shaders on a GPU.

python -I -B tests/test_spirv_xfb.py [--cxx g++] [--glslang glslangValidator]
"""
import argparse
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OpName, OpEntryPoint, OpExecutionMode, OpCapability, OpMemoryModel = 5, 15, 16, 17, 14
OpTypeVoid, OpTypeFloat, OpTypeVector, OpTypeFunction, OpTypePointer, OpVariable = 19, 22, 23, 33, 32, 59
OpFunction, OpLabel, OpLoad, OpStore, OpReturn, OpFunctionEnd, OpDecorate, OpMemberDecorate = 54, 248, 61, 62, 253, 56, 71, 72


def words_of(text):
    w = [ord(c) for c in text.encode()] if False else list(text.encode()) + [0]
    w += [0] * (-len(w) % 4)
    return [int.from_bytes(bytes(w[i:i + 4]), "little") for i in range(0, len(w), 4)]


def ins(o, *operands):
    return [(len(operands) + 1) << 16 | o, *operands]


def plain_position(model=0, position=True):
    """A vertex shader whose Position output is a variable of its own (the game translator's shape): gl_Position = in0"""
    main, void, fn, f32, v4, pout, pin, pos, inp, label, x = range(1, 12)
    m = [0x07230203, 0x00010000, 0, 12, 0]
    m += ins(OpCapability, 1)                       # Shader
    m += ins(OpMemoryModel, 0, 1)                   # Logical GLSL450
    m += ins(OpEntryPoint, model, main, *words_of("main"), pos, inp)
    m += ins(OpName, main, *words_of("main"))
    if position:
        m += ins(OpDecorate, pos, 11, 0)            # BuiltIn Position
    m += ins(OpDecorate, inp, 30, 0)                # Location 0
    m += ins(OpTypeVoid, void) + ins(OpTypeFunction, fn, void) + ins(OpTypeFloat, f32, 32) + ins(OpTypeVector, v4, f32, 4)
    m += ins(OpTypePointer, pout, 3, v4) + ins(OpTypePointer, pin, 1, v4)
    m += ins(OpVariable, pout, pos, 3) + ins(OpVariable, pin, inp, 1)
    m += ins(OpFunction, void, main, 0, fn) + ins(OpLabel, label) + ins(OpLoad, v4, x, inp) + ins(OpStore, pos, x)
    m += ins(OpReturn) + ins(OpFunctionEnd)
    return m


def instructions(words):
    out, i = [], 5
    while i < len(words):
        n, o = words[i] >> 16, words[i] & 0xffff
        out.append((i, o, words[i + 1:i + n]))
        i += n
    return out


def check(cond, what, failures):
    if not cond:
        failures.append(what)


def verify(before, after, member_form, failures, name):
    a, b = instructions(before), instructions(after)
    added = len(after) - len(before)
    check(added == 2 + 3 + 4 + 4 + (5 if member_form else 4), f"{name}: {added} words added", failures)
    caps = [x[0] for _, o, x in b if o == OpCapability]
    check(caps.count(53) == 1 and caps[:len(caps) - 1] == [c for c in caps if c != 53] and caps[-1] == 53,
          f"{name}: the capability not once, after the others: {caps}", failures)
    last_cap = max(k for k, (_, o, _) in enumerate(b) if o == OpCapability)
    check(all(o == OpCapability for _, o, _ in b[:last_cap + 1]), f"{name}: the capabilities not first", failures)
    entry = next(x for _, o, x in b if o == OpEntryPoint and x[0] == 0)[1]
    modes = [(k, x) for k, (_, o, x) in enumerate(b) if o == OpExecutionMode]
    check(any(x == [entry, 11] for _, x in modes), f"{name}: no Xfb mode on the vertex entry point", failures)
    last_entry = max(k for k, (_, o, _) in enumerate(b) if o == OpEntryPoint)
    check(all(k > last_entry for k, _ in modes), f"{name}: the mode before an entry point", failures)
    decs = [(o, x) for _, o, x in b if o in (OpDecorate, OpMemberDecorate)]
    pos = next((x[0] for o, x in decs if o == OpDecorate and x[1:] == [11, 0]), None)
    if member_form:
        block, member = next((x[0], x[1]) for o, x in decs if o == OpMemberDecorate and x[2:] == [11, 0])
        offset = (OpMemberDecorate, [block, member, 35, 0])
        ptr = next(x[0] for _, o, x in b if o == OpTypePointer and x[1] == 3 and x[2] == block)
        pos = next(x[1] for _, o, x in b if o == OpVariable and x[0] == ptr)
    else:
        offset = (OpDecorate, [pos, 35, 0])
    want = [(OpCapability, [53]), (OpExecutionMode, [entry, 11]), (OpDecorate, [pos, 36, 0]), (OpDecorate, [pos, 37, 16]), offset]
    for w in want[2:]:
        check(w in decs, f"{name}: no {w}", failures)
    last_dec = max(k for k, (_, o, _) in enumerate(b) if o in (OpDecorate, OpMemberDecorate))
    first_type = min(k for k, (_, o, _) in enumerate(b) if o in (OpTypeVoid, OpTypeFloat, OpTypeFunction, OpTypePointer, 21, 30))
    check(last_dec < first_type, f"{name}: a decoration after the types", failures)
    # every instruction that was there, in order: the patched module less exactly the five added
    rest = [(o, x) for _, o, x in b]
    for w in want:
        if w in rest:
            rest.remove(w)
    check(rest == [(o, x) for _, o, x in a], f"{name}: other words changed", failures)
    check(after[:5] == before[:5], f"{name}: the header changed", failures)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--glslang", default=shutil.which("glslangValidator"))
    a = p.parse_args()
    failures = []
    with tempfile.TemporaryDirectory(prefix="spirv-xfb-") as tmp:
        tmp = Path(tmp)
        tool = tmp / "spirv_xfb_tool.exe"
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "gpu" / "vk"),
                        str(ROOT / "tests" / "spirv_xfb_tool.cpp"), "-o", str(tool)], check=True)

        def run(words, name):
            src, dst = tmp / (name + ".spv"), tmp / (name + ".xfb.spv")
            src.write_bytes(struct.pack("<%dI" % len(words), *words))
            out = subprocess.run([str(tool), str(src), str(dst)], capture_output=True, text=True, check=True).stdout.strip()
            return out, (list(struct.unpack("<%dI" % (dst.stat().st_size // 4), dst.read_bytes())) if out == "ok" else None)

        plain = plain_position()
        out, patched = run(plain, "plain")
        check(out == "ok", "the game's shape (a Position variable) not patched: " + out, failures)
        if patched:
            verify(plain, patched, False, failures, "plain")
            again, _ = run(patched, "again")
            check(again == "no: it already captures", "a patched module patched again: " + again, failures)
        for words, name, want in ((plain_position(model=4), "fragment", "no: no vertex entry point"),
                                  (plain_position(position=False), "noposition", "no: no Position output"),
                                  ([1, 2, 3, 4, 5, 6], "junk", "no: not SPIR-V")):
            out, _ = run(words, name)
            check(out == want, f"{name}: {out} (wanted {want})", failures)
        if a.glslang:   # (glslang's gl_PerVertex block)
            vert = tmp / "block.vert"
            vert.write_text("#version 450\nlayout(location = 0) in vec4 p;\nlayout(binding = 0) uniform U { mat4 m; float t; } u;\n"
                            "void main() { gl_Position = u.m * (p + vec4(sin(u.t), 0, 0, 0)); gl_PointSize = 1.0; }\n",
                            encoding="utf-8")
            spv = tmp / "block.spv"
            subprocess.run([a.glslang, "-V", "--target-env", "vulkan1.3", "-o", str(spv), str(vert)], check=True, capture_output=True)
            block = list(struct.unpack("<%dI" % (spv.stat().st_size // 4), spv.read_bytes()))
            out, patched = run(block, "block")
            check(out == "ok", "glslang's block shape not patched: " + out, failures)
            if patched:
                verify(block, patched, True, failures, "block")
        else:
            print("glslangValidator not found: the block shape not checked")
    if failures:
        for f in failures:
            print("FAIL:", f)
        raise SystemExit(f"{len(failures)} failures")
    print("PASS: a vertex stage's Position captured (transform feedback), the game's shape and glslang's")


if __name__ == "__main__":
    main()
