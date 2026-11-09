// Memory priorities (VK_EXT_memory_priority; VK_EXT_pageable_device_local_memory to change one later): what the driver moves
// out of VRAM first when the process holds more than it allows. Everything is made at the default, as without the
// extensions; a texture unused for a while (a course left behind, an area's terrain arrays) drops to the lowest, and is
// back at the default once it is used again. vk_main.cpp (the extensions), vk_vma.cpp (VMA), vk_texture.cpp (the textures).
// OPT-IN (RX_VK_MEMORY_PRIORITY=1) until testers on 6-8 GB cards confirm it helps (issues 993, 995): not yet tried in game.
// tests/test_memory_priority.py
#pragma once
#include <cstdint>
#include <cstdlib>

namespace memory_priority {

// (Vulkan's default, and VMA's for its own pools. VMA passes a priority of 0 for a create-info left at {} once the
// extension is on: every VmaAllocationCreateInfo and VmaPoolCreateInfo sets this)
constexpr float kNormal = 0.5f;
constexpr float kIdle = 0.1f;               // (a texture unused for kIdlePresents)
constexpr uint64_t kIdlePresents = 600;     // (10 s at 60 fps)

/// RX_VK_MEMORY_PRIORITY: any number but 0, on (where the device has the extensions); unset, empty or 0: off
inline bool wanted(const char* env) {
    return env && *env && std::atoi(env) != 0;
}

/// RX_VK_PRIORITY_IDLE=<presents>: how long a texture goes unused before it drops to kIdle; 0: never.
/// Unset, empty, negative or not a number: kIdlePresents
inline uint64_t idle_presents(const char* env) {
    if (!env || !*env) return kIdlePresents;
    char* end = nullptr;
    const long long v = std::strtoll(env, &end, 10);
    if (end == env || *end || v < 0) return kIdlePresents;
    return (uint64_t)v;
}

/// A texture's priority at present `now`, last used at `last_used`: kIdle once unused for `idle` presents (idle 0: never)
inline float for_texture(uint64_t now, uint64_t last_used, uint64_t idle) {
    if (!idle || last_used >= now) return kNormal;
    return now - last_used >= idle ? kIdle : kNormal;
}

} // namespace memory_priority
