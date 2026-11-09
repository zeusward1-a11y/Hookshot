// (hookshot) The textures' room in video memory (vk_texture.cpp's housekeeping): a share of what the driver allows the process
// (RX_VK_VRAM_TARGET, 85%), less what the process holds besides its textures: DLSS-G and the D3D12 bridge, the pools' VRAM
// mirrors, render targets, buffers. A fixed budget of 6 GB of textures beside ~4.4 GB of those passed the 10.4 GB an RTX 5070
// was allowed in Hyrule Field and Hyrule Castle (09/11): the squeeze at 90% then evicted and made textures again in play
// (hitches), and the driver paged the rest out. tests/test_vram_room.py
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace vram_room {

constexpr int kTargetPct = 85;                  // (under the squeeze at 90%: the textures leave room before it starts)
constexpr uint64_t kFloor = 1024ull << 20;      // (never less room than this: what a scene draws stays whatever the others)

/// RX_VK_VRAM_TARGET=<percent of what the driver allows>: 85 unset, empty or not a number; 0 off (the budget as before);
/// over 97, 97 (the driver's own squeeze)
inline int target_pct(const char* env) {
    if (!env || !*env) return kTargetPct;
    char* end = nullptr;
    const long v = std::strtol(env, &end, 10);
    if (end == env || *end || v < 0) return kTargetPct;
    return (int)std::min<long>(v, 97);
}

/// What the process holds besides its textures: what it uses less the textures and the free space in the allocator's blocks
/// (which new textures take before a block is made). Textures evicted or made move between the last two, not this
inline uint64_t others(uint64_t used, uint64_t textures, uint64_t block_free) {
    return used > textures + block_free ? used - textures - block_free : 0;
}

/// The textures' room: pct of allowed less the others, at least kFloor. pct 0 or nothing allowed known: no limit
inline uint64_t room(uint64_t used, uint64_t allowed, uint64_t block_free, uint64_t textures, int pct) {
    if (pct <= 0 || !allowed) return UINT64_MAX;
    const uint64_t target = allowed / 100 * (uint64_t)pct;
    const uint64_t o = others(used, textures, block_free);
    return std::max(target > o ? target - o : 0, kFloor);
}

}  // namespace vram_room
