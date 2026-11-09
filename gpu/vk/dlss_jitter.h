// (hookshot) DLSS's jitter (vk_draw.cpp dlss_jitter_of): when to stop it for a while. A jittered frame that DLSS did not make
// reached the screen as it was drawn, a sub-pixel off, and shook; a short stop for one such frame (the game draws the scene once
// outside the upscale chain every ~10 s, the autosave's picture), a second's when another came within two seconds (frames DLSS
// keeps missing would shake). No Vulkan here: tests/dlss_jitter_test.cpp runs it (tests/test_dlss_jitter.py).
//
// Only a frame that was jittered and then not made counts. The first version compared the last jittered frame with the last frame
// DLSS made, so any unjittered frame DLSS made after it (the first one after a menu, a load or a size change) looked like a miss
// and stopped the jitter for nothing (the review of 09/11).
#pragma once

#include <cstdint>

namespace mkwvk::jitter {

constexpr int kHoldOnce = 8, kHoldAgain = 60;
constexpr uint64_t kAgainWithin = 120;   // (frames: two misses this close make the long hold)

struct Hold {
    uint64_t pending = ~0ull;   // a frame whose draws took the jitter and that DLSS has not made yet
    uint64_t frame = ~0ull;     // the frame whose start was seen last
    uint64_t missed = ~0ull;    // the last jittered frame DLSS did not make
    int left = 0;               // frames still without jitter
    uint64_t holds = 0;

    /// Once a frame, at its first draw: the countdown, and a pending frame (jittered, not made) starts a hold. The frame it was,
    /// for the log; ~0 when none
    uint64_t frame_start(uint64_t now) {
        if (frame == now) return ~0ull;
        frame = now;
        if (left > 0) left--;
        if (pending == ~0ull || pending >= now) return ~0ull;
        const uint64_t m = pending;
        const bool again = missed != ~0ull && m - missed < kAgainWithin;
        left = again ? kHoldAgain : kHoldOnce, holds++, missed = m, pending = ~0ull;
        return m;
    }
    /// A draw of this frame took the jitter
    void used(uint64_t now) { pending = now; }
    /// DLSS made this frame (whatever it was drawn with)
    void made(uint64_t now) {
        if (pending == now) pending = ~0ull;
    }
    bool held() const { return left > 0; }
};

} // namespace mkwvk::jitter
