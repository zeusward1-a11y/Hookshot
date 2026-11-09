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

/// How long a texture must have gone unused to be evicted, its frames: the room is soft. Over the hard budget (the cap, the
/// squeezes near what the driver allows) and not yet down to 7/8 of it, need (60 frames: as before the room); under it, over
/// the room only, cold (RX_VK_ROOM_MIN_UNUSED, 600, ~10 s). Taken as the budget itself at 75%, Hyrule Castle's textures (3 GB
/// drawn in 8 presents, more within seconds) were evicted at a second unused and made again, 20000 a minute: hitches, and
/// frame generation lost its way (09/11)
constexpr uint64_t kRoomMinUnused = 600;
inline uint64_t min_unused(bool over_hard, uint64_t left, uint64_t hard, uint64_t need, uint64_t cold) {
    return over_hard && left > hard / 8 * 7 ? need : std::max(need, cold);
}

/// RX_VK_ROOM_MIN_UNUSED=<frames>: kRoomMinUnused unset, empty, not a number or under 1
inline uint64_t room_min_unused(const char* env) {
    if (!env || !*env) return kRoomMinUnused;
    char* end = nullptr;
    const long v = std::strtol(env, &end, 10);
    return end == env || *end || v < 1 ? kRoomMinUnused : (uint64_t)v;
}

/// The textures' room: pct of allowed less the others, at least kFloor. pct 0 or nothing allowed known: no limit
inline uint64_t room(uint64_t used, uint64_t allowed, uint64_t block_free, uint64_t textures, int pct) {
    if (pct <= 0 || !allowed) return UINT64_MAX;
    const uint64_t target = allowed / 100 * (uint64_t)pct;
    const uint64_t o = others(used, textures, block_free);
    return std::max(target > o ? target - o : 0, kFloor);
}

}  // namespace vram_room
