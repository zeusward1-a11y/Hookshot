"""The colour grade on NVIDIA DLSS's picture (gpu/vk/dlss_grade.h, shaders/dlss_out.comp; RX_DLSS_GRADE_SAT, _DENSITY and
_CONTRAST): the arithmetic (tests/dlss_grade_test.cpp), the shader's being the header's, the SPIR-V header made from the source
as it is now (when glslangValidator is here), and the wiring in vk_dlss.cpp. No game, GPU or NVIDIA DLL.

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
    comp = (VK / "shaders" / "dlss_out.comp").read_text(encoding="utf-8")
    dlss = (VK / "vk_dlss.cpp").read_text(encoding="utf-8")
    h, c, d = squeeze(head), squeeze(comp), squeeze(dlss)
    # the shader's arithmetic is the header's: the same lines, in the same order, the same pivot
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
    need("c=pc.sat!=0.0||pc.contrast!=0.0||pc.density!=0.0?grade(c):c;" in c, "the shader grades with the three factors 0")
    # on the encoded colour: after the read's clamp, before the square that makes it linear again
    at, sq, store = c.find("?grade(c):c;"), c.find("c*=c;"), c.find("imageStore(dst,g,vec4(c,1.0));")
    need(0 <= c.find("c=max(c,vec3(0.0));c=pc.sat") < at < sq < store, "the grade is not on the encoded colour, before its square")
    # the push constants: the shader's block is the C++ struct, member for member, and the pipeline's size is the struct's
    need("uniformP{ivec2size;floatsat;floatcontrast;floatdensity;}pc;" in c, "the push constants are not size, sat, contrast, density")
    need("structOutPush{intsize[2];floatsat=0.0f,contrast=0.0f,density=0.0f;};static_assert(sizeof(OutPush)==20," in h,
         "OutPush is not the shader's block")
    # every user of the shader gives it the whole block: FSR 3.1's output pass too, with the grade's factors 0
    users = [f.name for f in VK.glob("*.cpp") if "DLSS_OUT_COMP_SPV" in f.read_text(encoding="utf-8")]
    need(sorted(users) == ["vk_dlss.cpp", "vk_fsr3.cpp"], f"dlss_out.comp's users are not the two known: {users}")
    f3 = squeeze((VK / "vk_fsr3.cpp").read_text(encoding="utf-8"))
    need("constVkPushConstantRangepr{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(grade::OutPush)};" in f3 and
         "constgrade::OutPushpc{{(int)ow,(int)oh}};vkCmdPushConstants(cmd,o.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeofpc,&pc);" in
         re.sub(r"constVkDescriptorImageInfo.*?vkCmdPushDescriptorSetKHR\([^;]*;", "", f3),
         "FSR 3.1's output pass does not push dlss_out.comp's whole block, the grade's factors 0")
    need("out_types,2,sizeof(grade::OutPush),s.out_set,s.out_layout,s.out)" in d, "the pipeline's push constant size is not OutPush's")
    need("constgrade::OutPushopc{{(int32_t)ow,(int32_t)oh},g_sat,g_contrast,g_density};" in d and
         "vkCmdPushConstants(cmd,s.out_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeofopc,&opc);" in d,
         "the size and the three factors are not pushed to the output pass")
    # the wiring: the percents, read once, the density the saturation's when unset
    need('g_sat=grade::factor_of(getenv("RX_DLSS_GRADE_SAT"))' in d and 'g_contrast=grade::factor_of(getenv("RX_DLSS_GRADE_CONTRAST"))' in d,
         "the environment's percents are not read through factor_of")
    need('g_density=grade::given(getenv("RX_DLSS_GRADE_DENSITY"))?grade::factor_of(getenv("RX_DLSS_GRADE_DENSITY")):g_sat;' in d,
         "the density is not the environment's, or the saturation's when unset")
    if a.glslang:
        with tempfile.TemporaryDirectory(prefix="dlss-grade-") as tmp:
            out = Path(tmp) / "dlss_out_comp_spv.h"
            subprocess.run([a.glslang, "-V", "--target-env", "vulkan1.3", "--vn", "DLSS_OUT_COMP_SPV", "-o", str(out),
                            str(VK / "shaders" / "dlss_out.comp")], check=True, capture_output=True)
            need(out.read_text().split() == (VK / "shaders" / "dlss_out_comp_spv.h").read_text().split(),
                 "dlss_out_comp_spv.h is not made from dlss_out.comp as it is now")
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
