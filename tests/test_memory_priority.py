"""Compile and run the memory priority rules (gpu/vk/memory_priority.h; no game/GPU), and check the renderer still wires
them in: the extensions asked for with their switch, VMA told, every VMA create-info given the default priority (left at
{}, VMA passes 0, the lowest, once the extension is on), and the textures' pass."""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu/vk"

NEEDLES = {
    "vk_main.cpp": ['#include "memory_priority.h"', 'memory_priority::wanted(getenv("RX_VK_MEMORY_PRIORITY"))',
                    "VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME", "VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME",
                    "dev.memory_priority = dev.memory_priority && mprio.memoryPriority",
                    "dev.pageable_memory = dev.memory_priority && dev.pageable_memory && pageable.pageableDeviceLocalMemory"],
    "vk_vma.cpp": ['#include "memory_priority.h"', "VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT",
                   "if (!info.dedicatedMemory || !info.allocationInfo.deviceMemory) return 0;"],
    "vk_texture.cpp": ['#include "memory_priority.h"', 'memory_priority::idle_presents(getenv("RX_VK_PRIORITY_IDLE"))',
                       "if (dev.pageable_memory && frame_no % 60 == 0) texture_priorities();",
                       "set_allocation_priority(t->allocation, want)"],
    "vk_loader.h": ["X(vkSetDeviceMemoryPriorityEXT)"],
}

# (a create-info made at {} and the lines after it that must set its priority)
DECL = re.compile(r"\bVma(?:Allocation|Pool)CreateInfo\s+(\w+)\s*\{\s*\}\s*;")
WINDOW = 15


def check_source():
    for name, needles in NEEDLES.items():
        src = (VK / name).read_text(encoding="utf-8")
        for needle in needles:
            if needle not in src:
                raise SystemExit(f"{name} no longer has: {needle}")
    unset = []
    for path in sorted(VK.glob("*.cpp")):
        lines = path.read_text(encoding="utf-8").splitlines()
        for i, line in enumerate(lines):
            m = DECL.search(line)
            if not m:
                continue
            after = "\n".join(lines[i + 1:i + 1 + WINDOW])
            if not re.search(rf"\b{re.escape(m.group(1))}\.priority\s*=", after):
                unset.append(f"{path.relative_to(ROOT)}:{i + 1}: {m.group(1)}")
    if unset:
        raise SystemExit("VMA create-infos without a priority (VMA would pass 0, the lowest):\n  " + "\n  ".join(unset))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="memory-priority-") as tmp:
        exe = Path(tmp) / "memory-priority"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests/test_memory_priority.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
