// (hookshot) DLSS's actor motion in this branch (vk_draw.cpp dlss_skin_check and dlss_actor_pass, after #841's 8b925b6): what
// the CPU works out, kept apart so the tests run it without the game (tests/test_dlss_actor_plan.cpp checks it alone,
// tests/dlss_actor_gpu.cpp draws on a GPU with what it makes). Which draw of a frame is a model's n-th, where each draw's pose
// goes in this frame's pose buffer and where the frame before's is, the push constants, the cameras' buffer. Header only, no
// Vulkan.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <unordered_map>
#include <vector>

#include "dlss_actor.h"

namespace dlss_actor {

/// A skinned draw's vertex layout and indices, as dlss_skin_check reads them (vk_draw.cpp DlssSkin has them)
struct Layout {
    uint64_t vsize = 0;                 // (vertex stream 0's bytes)
    uint32_t stride = 0;
    uint32_t pos = 0;                   // (byte offsets in a vertex: the position, RGB32F or RGBA16F)
    uint32_t joints[2]{}, weights[2]{}; // (RGBA16UI or RGBA8UI joints and RGBA8 weights, four a set)
    int sets = 0;                       // (1 or 2)
    uint32_t formats = 0;               // (Push::formats: kHalfPosition, kByteJoints << set)
    int itype = 0, count = 0, base_vertex = 0;   // (0, 1, 2: 8, 16, 32-bit indices; the indices drawn)
};

/// Every attribute the shader reads inside one vertex, each a whole word in: false when it would read past the vertex (into the
/// next one's, or past the stream's end at the last), which a device address read does not stop
inline bool fits(const Layout& l) {
    if (!l.stride || l.stride % 4 || l.pos % 4 || l.sets < 1 || l.sets > 2) return false;
    if (l.pos + ((l.formats & kHalfPosition) ? 8u : 12u) > l.stride) return false;
    for (int s = 0; s < l.sets; s++) {
        const uint32_t joint_bytes = (l.formats & (kByteJoints << s)) ? 4u : 8u;
        if (l.weights[s] % 4 || l.joints[s] % 4 || l.weights[s] + 4 > l.stride || l.joints[s] + joint_bytes > l.stride) return false;
    }
    return true;
}

/// The skinned draws of one frame, in the order they were drawn. A draw's key names its model and which draw of that model it is
/// in the frame (the n-th is matched with the n-th of the frame before). Noting a draw of another frame drops the list first: a
/// frame DLSS did not make leaves its draws behind, and counted with the next frame's they shift its n-ths, so a draw takes
/// another one's pose for the frame before's (the review of 09/11). At most kMost a frame: past them a frame's draws are not
/// noted (dropping the list mid-frame would restart the n-ths)
template <class T> struct Noted {
    static constexpr size_t kMost = 512;
    std::vector<T> draws;
    uint64_t frame = ~0ull;
    /// The key of the next draw of `model` (key(..., 0)) in frame `now`; false when the frame has kMost already
    bool next_key(uint64_t model, uint64_t now, uint64_t* out) {
        if (frame != now) draws.clear(), frame = now;
        if (draws.size() >= kMost) return false;
        int nth = 0;
        for (const T& d : draws) nth += same_model(d.key, model);
        *out = (model & ~0xffull) | (uint64_t)std::clamp(nth, 0, 255);
        return true;
    }
    /// Frame `now`'s draws, for its pass: taken (the list is empty after); none when they are another frame's
    std::vector<T> take(uint64_t now) {
        std::vector<T> out;
        if (frame == now) out = std::move(draws);
        draws.clear();
        return out;
    }
};

/// Where a draw's pose is: bytes into its frame's pose buffer (the origin, 12 bytes, then the bones from at + 16)
struct Pose {
    uint32_t at = 0, nbones = 0;
    uint64_t frame = 0;
};
constexpr uint32_t kPoseBytes = 4u << 20;   // (a pose buffer: 280 bones of 48 bytes are 13 KB a draw, 300 of them)

/// The two pose buffers taking turns: a frame's poses are copied into one (here) while the frame before's are read from the
/// other (there); the next frame swaps them
struct Poses {
    std::unordered_map<uint64_t, Pose> now, before;
    int at = 0;          // (this frame's buffer: 0 or 1)
    uint32_t used = 0;   // (bytes of it this frame)
    int here() const { return at; }
    int there() const { return at ^ 1; }
    /// A frame's pass starts: the poses of the last are the frame before's
    void begin() {
        before = std::move(now);
        now.clear();
        used = 0;
    }
    /// A place in this frame's buffer for a draw's pose (at: its origin, the bones 16 on); false when the buffer is full
    bool place(uint64_t key, uint32_t nbones, uint64_t frame, uint32_t* at_out) {
        const uint64_t bytes = 16 + (uint64_t)nbones * 48;
        if (used + bytes > kPoseBytes) return false;
        *at_out = used;
        now[key] = {used, nbones, frame};
        used += (uint32_t)((bytes + 15) & ~15ull);
        return true;
    }
    /// The frame before's pose of a draw, when it can be taken: DLSS made the frame before (history), the draw was in it, as many
    /// bones. Null otherwise (the draw keeps the camera's motion this frame)
    const Pose* before_of(uint64_t key, uint32_t nbones, uint64_t frame, bool history) const {
        if (!history) return nullptr;
        const auto it = before.find(key);
        return it == before.end() || it->second.frame + 1 != frame || it->second.nbones != nbones ? nullptr : &it->second;
    }
    /// The frame's copies recorded: the next frame's go into the other buffer
    void end() { at ^= 1; }
};

/// A draw's push constants: its vertices and indices by their device addresses (va, ia: 0 when not found), its pose in this frame's
/// buffer (here + mine) and the frame before's (there + theirs), the depth tolerance. False when it cannot be drawn
inline bool make_push(const Layout& l, uint64_t va, uint64_t ia, uint64_t here, uint32_t mine, uint64_t there, uint32_t theirs,
                      uint32_t nbones, uint32_t tolerance, Push* a) {
    if (!va || !ia || va % 4 || l.count <= 0 || l.itype < 0 || l.itype > 2 || !nbones || !fits(l)) return false;
    *a = Push{};
    a->vertices = va;
    a->count = (uint32_t)std::min<uint64_t>(l.vsize / l.stride, UINT32_MAX);   // (the stream's vertices: an index past them draws nothing)
    index_start(ia, l.itype, &a->indices, &a->first);
    a->origins[0] = here + mine, a->origins[1] = there + theirs;
    a->bones[0] = a->origins[0] + 16, a->bones[1] = a->origins[1] + 16;
    a->nbones = nbones;
    a->stride = l.stride / 4;   // (in words)
    a->itype = (uint32_t)l.itype, a->base_vertex = l.base_vertex;
    a->pos = l.pos, a->sets = (uint32_t)l.sets;
    for (int i = 0; i < 2; i++) a->joints[i] = l.joints[i], a->weights[i] = l.weights[i];
    a->formats = l.formats;
    a->tolerance = tolerance;
    return true;
}

/// The cameras' buffer (kHeadWords): this frame's camera and the frame before's as the scene's cbuf 3 has them at 0x30 (row major,
/// clip = M * position; unjittered in this branch, whose jitter is the scene draws' viewport offset, dlss_jitter_of); this
/// frame's viewport offset ux, uy (render pixels, window y down) in the game's clip space (y up): 2ux/w, -2uy/h, which the
/// fragment takes off; the render size. The pass is drawn with the same viewport offset, so its depths meet the scene's
inline void head(const float cur[16], const float prev[16], float ux, float uy, uint32_t w, uint32_t h, float out[kHeadWords]) {
    for (int i = 0; i < 16; i++) out[i] = cur[i], out[16 + i] = prev[i];
    out[32] = 2.0f * ux / (float)w, out[33] = -2.0f * uy / (float)h, out[34] = (float)w, out[35] = (float)h;
}

/// RX_DLSS_ACTOR_TOL: depth steps (float ulps) from the scene's within which a pixel of a draw drawn again takes its motion; 64
/// when unset, not a number or out of 0..65536
inline uint32_t tolerance_of(const char* env) {
    if (!env || !*env) return 64;
    char* end = nullptr;
    const long v = std::strtol(env, &end, 10);
    return end == env || *end || v < 0 || v > 65536 ? 64u : (uint32_t)v;
}

} // namespace dlss_actor
