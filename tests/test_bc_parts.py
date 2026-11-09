"""Compile and run the BC3 upload parts (vk_texture.cpp's upload_astc_gpu in runs of layers; no game/GPU required)."""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def check_source():
    """The upload still takes its runs from bc_parts.h, and the next recordings still continue what it left"""
    tex = (ROOT / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    for needle in ('#include "bc_parts.h"', "bc_parts::run_end(", "bc_parts::run_fits(", 'getenv("RX_VK_BC_PART_MTEXELS")',
                   "void texture_bc_continue()", "bc_part_texels = 0;"):
        if needle not in tex:
            raise SystemExit("vk_texture.cpp no longer has: " + needle)
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    for needle in ("texture_bc_continue();", "texture_bc_pending()"):
        if needle not in replay:
            raise SystemExit("vk_replay.cpp no longer has: " + needle)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="bc-parts-") as tmp:
        exe = Path(tmp) / "bc-parts"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests/test_bc_parts.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
