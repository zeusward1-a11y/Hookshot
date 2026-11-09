"""Compile and run the textures' room rules (gpu/vk/vram_room.h; no game, no GPU), and check vk_texture.cpp's housekeeping
still takes them: the budget capped by the room before the squeeze at 90%, RX_VK_VRAM_TARGET read once, the 600-frame log
line with the rest and the room.

python -I -B tests/test_vram_room.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def check_source():
    tex = (ROOT / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    for needle in ('#include "vram_room.h"',
                   'static const int target_pct = vram_room::target_pct(getenv("RX_VK_VRAM_TARGET"));',
                   "const uint64_t room = known ? vram_room::room(used, allowed, block_free, total_bytes, target_pct) : UINT64_MAX;",
                   "const uint64_t hard = budget;\n    budget = std::min(budget, room);",
                   "vram_room::others(used, total_bytes, block_free)",
                   'static const uint64_t room_unused = vram_room::room_min_unused(getenv("RX_VK_ROOM_MIN_UNUSED"));',
                   "const bool over_hard = total_bytes > hard;",
                   "sweep ? need_unused : vram_room::min_unused(over_hard, total_bytes - pending_bytes, hard, need_unused, room_unused);",
                   "if (frame_no - last < unused_for && !shifted) continue;"):
        if needle not in tex:
            raise SystemExit(f"vk_texture.cpp no longer has: {needle}")
    # (the hard budget is the old one, its squeezes near the driver's limit included; the room, soft, after them)
    at = [tex.find(m) for m in ("if (known && pressure > allowed / 10 * 9) {", "if (known && !by_blocks && used > allowed / 100 * 97) {",
                                 "const uint64_t hard = budget;", "const bool over_hard = total_bytes > hard;")]
    if not all(x >= 0 for x in at) or at != sorted(at):
        raise SystemExit("vk_texture.cpp: the room is not the soft budget after the hard one's squeezes")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="vram-room-") as tmp:
        exe = Path(tmp) / "vram-room"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests/test_vram_room.cpp"), "-o", str(exe)],
                       check=True)
        subprocess.run([str(exe)], check=True)
    print("PASS: the textures' room in video memory")


if __name__ == "__main__":
    main()
