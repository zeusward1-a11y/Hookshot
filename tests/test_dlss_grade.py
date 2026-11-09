"""The colour grade on the scene DLSS takes (gpu/vk/dlss_grade.h, shaders/dlss_color.comp; RX_DLSS_GRADE_SAT and
RX_DLSS_GRADE_CONTRAST): the arithmetic (tests/dlss_grade_test.cpp), the shader's being the header's, the SPIR-V header made from
the source as it is now (when glslangValidator is here), and the wiring in vk_draw.cpp.

    python -I -B tests/test_dlss_grade.py [--cxx clang++] [--glslang glslangValidator]
"""
import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"


def squeeze(text):
    return re.sub(r"\s+", "", re.sub(r"//[^\n]*", "", text))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default=shutil.which("clang++") or shutil.which("g++"))
    p.add_argument("--glslang", default=shutil.which("glslangValidator"))
    a = p.parse_args()
    failures = []

    def need(cond, what):
        if not cond:
            failures.append(what)

    head = (VK / "dlss_grade.h").read_text(encoding="utf-8")
    comp = (VK / "shaders" / "dlss_color.comp").read_text(encoding="utf-8")
    draw = (VK / "vk_draw.cpp").read_text(encoding="utf-8")
    h, c, d = squeeze(head), squeeze(comp), squeeze(draw)
    # the shader's arithmetic is the header's: the same two lines, in the same order, the same pivot
    need("constexprfloatkPivot=0.435f;" in h, "the header's pivot is not 0.435")
    need("c[i]=std::max(top-(top-c[i])*(1.0f+sat),0.0f);" in h and
         "constfloatdark=1.0f-density*(top>0.0f?(top-low)/top:0.0f);" in h and
         "constfloatv=(c[i]*dark-kPivot)*(1.0f+contrast)+kPivot;c[i]=std::max(v,0.0f);" in h,
         "the header's grade is not saturation, density, contrast, then the clamp")
    need("constexprfloatkCeiling=65504.0f;" in h and
         "for(inti=0;i<3;i++)c[i]=std::min(c[i],kCeiling);constfloattop=" in h and
         "vec3grade(vec3c){c=min(c,vec3(65504.0));constfloattop=" in c, "the grade does not start at the target's ceiling, in both")
    need("c=max(top-(top-c)*(1.0+pc.sat),vec3(0.0));constfloatlow=min(c.r,min(c.g,c.b));"
         "constfloatdark=1.0-pc.density*(top>0.0?(top-low)/top:0.0);c=(c*dark-0.435)*(1.0+pc.contrast)+0.435;returnmax(c,vec3(0.0));" in c,
         "the shader's grade is not the header's")
    need("constfloattop=max(c.r,max(c.g,c.b));" in c, "the shader's highest channel is not the three channels'")
    # off, the colour is written as it came (not through the grade: no rounding of its own)
    need("pc.sat!=0.0||pc.contrast!=0.0||pc.density!=0.0?grade(e):e" in c, "the shader grades with both factors 0")
    need("constvec3e=pc.encode!=0?sqrt(c):c;" in c, "the grade is not on the encoded colour")
    need("uniformP{intencode;floatsat;floatcontrast;floatdensity;}pc;" in c, "the push constants are not encode, sat, contrast, density")
    # the wiring: the two percents, read once, pushed after encode, to the colour pass
    need('grade::factor_of(getenv("RX_DLSS_GRADE_SAT"))' in d and 'grade::factor_of(getenv("RX_DLSS_GRADE_CONTRAST"))' in d,
         "the environment's percents are not read through factor_of")
    need("std::memcpy(&push[1],&sat,4),std::memcpy(&push[2],&contrast,4),std::memcpy(&push[3],&density,4);" in d,
         "the factors are not pushed after encode")
    need('density=grade::given(getenv("RX_DLSS_GRADE_DENSITY"))?grade::factor_of(getenv("RX_DLSS_GRADE_DENSITY")):sat;' in d,
         "the density is not the environment's, or the saturation's when unset")
    at = d.find("std::memcpy(&push[1],&sat,4)")
    nxt = d.find("dlss_pass(cmd,ds.color_pipe,", at)
    need(0 <= at < nxt < at + 500, "the factors are not pushed to the colour pass")
    # (names with these prefixes are part of the shader cache's key: a new one would drop the cache)
    for name in re.findall(r'"(\w*GRADE\w*)"', draw):
        need(name.startswith("RX_DLSS_GRADE_"), f"{name}: not an RX_DLSS_GRADE_ name (other prefixes are in the shader cache's key)")
    if a.glslang:
        with tempfile.TemporaryDirectory(prefix="dlss-grade-") as tmp:
            out = Path(tmp) / "dlss_color_comp_spv.h"
            subprocess.run([a.glslang, "-V", "--target-env", "vulkan1.3", "--vn", "DLSS_COLOR_COMP_SPV", "-o", str(out),
                            str(VK / "shaders" / "dlss_color.comp")], check=True, capture_output=True)
            need(out.read_text().split() == (VK / "shaders" / "dlss_color_comp_spv.h").read_text().split(),
                 "dlss_color_comp_spv.h is not made from dlss_color.comp as it is now")
    else:
        print("glslangValidator not found: the SPIR-V header not checked")
    if a.cxx:
        with tempfile.TemporaryDirectory(prefix="dlss-grade-") as tmp:
            exe = Path(tmp) / "grade.exe"
            subprocess.run([a.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I" + str(VK),
                            str(ROOT / "tests/dlss_grade_test.cpp"), "-o", str(exe)], check=True)
            r = subprocess.run([str(exe)], capture_output=True, text=True)
            print(r.stdout, end="")
            need(r.returncode == 0, "tests/dlss_grade_test.cpp failed")
    else:
        failures.append("no C++ compiler: the arithmetic not run")
    for f in failures:
        print("FAIL:", f)
    print("test_dlss_grade:", "FAILED" if failures else "ok")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
