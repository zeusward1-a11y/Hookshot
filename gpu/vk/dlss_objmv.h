// (hookshot) Own motion for what moves by itself, in the motion vectors DLSS and DLSS Frame Generation read (vk_draw.cpp
// dlss_motion_pass, shaders/dlss_motion.comp). After NX2navi's d9fb0fe ("Give characters in Ocarina's frame their own motion", its
// dlaa_mv.comp): Ocarina's frame has no per-object motion, so the vectors were the camera's alone, reconstructed from depth, and the
// character the camera follows (Link) dragged a copy of itself through DLSS's history and doubled in generated frames.
//
// Each pixel's view distance is kept for the next frame. The camera's vector then says where the pixel's surface was last frame and
// how far away it was: where last frame's distance there disagrees (by more than kTolerance, the closest of 3x3 pixels so that a
// silhouette reprojected a pixel off is not taken for motion), the surface moved by itself. When last frame's distance at the same
// pixel agrees, it kept its place on screen, as the followed character does: its vector is zero. Otherwise it keeps the camera's.
//
// This is the shader's logic on the CPU, line for line (tests/test_dlss_objmv.py checks it on drawn scenes; the shader is kept
// beside it). No Vulkan here.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace mkwvk::objmv {

constexpr float kTolerance = 0.04f;   // (a still surface reprojected lands within ~1%; a character a step from the wall: 10% and more)

/// The game's projection depth terms from its view-projection (column vectors, m[r*4+c], OpenGL's clip): view distance = b / (z_ndc
/// + a), with a = -(f + n) / (f - n) and b = -2 f n / (f - n) of the rows (row 2 against row 3, which is -back for a perspective).
/// False when it is not one (orthographic, degenerate)
inline bool depth_terms(const float m[16], float& a, float& b) {
    const double s = std::sqrt((double)m[12] * m[12] + (double)m[13] * m[13] + (double)m[14] * m[14]);
    if (!std::isfinite(s) || s < 1e-9) return false;
    const double f[3] = {m[12] / s, m[13] / s, m[14] / s};
    const double A = -((double)m[8] / s * f[0] + (double)m[9] / s * f[1] + (double)m[10] / s * f[2]);
    const double B = (double)m[11] / s + A * (double)m[15] / s;
    if (!std::isfinite(A) || !std::isfinite(B) || std::fabs(A - 1.0) < 1e-9 || B >= 0.0) return false;
    a = (float)A, b = (float)B;
    return true;
}

/// View distance of a pixel of OpenGL ndc depth zn (the depth buffer's value * 2 - 1); 0 where there is none (past far, degenerate)
inline float view_distance(float zn, float a, float b) {
    const float den = zn + a;
    if (std::fabs(den) < 1e-12f) return 0.0f;
    const float w = b / den;
    return w > 0.0f && w < 1e7f ? w : 0.0f;
}

struct Counts {
    uint32_t moved = 0, held = 0, checked = 0;
};

/// The motion pass for one frame, as the shader runs it: depth (the depth buffer's values, w x h), clip_to_prev (vk_draw.cpp
/// clip_to_previous), the depth terms; dist_prev (last frame's distances, prev_ok when they are there and at this size); on: the
/// check (RX_DLSS_OBJMV). Writes the vectors (render pixels, 2 a pixel) and this frame's distances
inline Counts motion_pass(int w, int h, const float* depth, const float clip_to_prev[16], float a, float b, bool on, bool prev_ok,
                          const float* dist_prev, std::vector<float>& mv, std::vector<float>& dist_out) {
    Counts c;
    mv.assign((size_t)w * h * 2, 0.0f);
    dist_out.assign((size_t)w * h, 0.0f);
    const auto miss = [&](float wx, float wy, float expect) {   // (the closest of 3x3; 0 off screen; 1e9 where last frame had none)
        const int qx = (int)std::floor(wx), qy = (int)std::floor(wy);
        if (qx < 0 || qy < 0 || qx > w - 1 || qy > h - 1) return 0.0f;
        float best = 1e9f;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                const int x = std::min(std::max(qx + dx, 0), w - 1), y = std::min(std::max(qy + dy, 0), h - 1);
                const float was = dist_prev[(size_t)y * w + x];
                if (was > 0.0f) best = std::min(best, std::fabs(was - expect) / expect);
            }
        return best;
    };
    const float* M = clip_to_prev;
    for (int py = 0; py < h; py++)
        for (int px = 0; px < w; px++) {
            const float d = depth[(size_t)py * w + px];
            const float sx = (px + 0.5f) / w, sy = (py + 0.5f) / h;
            const float clip[4] = {sx * 2 - 1, sy * -2 + 1, d * 2 - 1, 1};
            float prev[4];
            for (int r = 0; r < 4; r++) prev[r] = M[r * 4] * clip[0] + M[r * 4 + 1] * clip[1] + M[r * 4 + 2] * clip[2] + M[r * 4 + 3] * clip[3];
            float mx = 0, my = 0;
            if (d >= 0 && d <= 1 && prev[3] > 1e-6f) {
                const float dx = (prev[0] / prev[3] - clip[0]) * 0.5f * w, dy = (prev[1] / prev[3] - clip[1]) * -0.5f * h;
                if (std::fabs(dx) <= 65504 && std::fabs(dy) <= 65504) mx = dx, my = dy;
            }
            const float here = on ? view_distance(clip[2], a, b) : 0.0f;
            dist_out[(size_t)py * w + px] = here;
            if (on && prev_ok && here > 0.0f && d >= 0 && d <= 1 && prev[3] > 1e-6f) {
                const float there = prev[3] * here;   // (last frame's distance the camera's motion predicts: w_prev / w_cur * w_cur)
                c.checked++;
                const float wx = px + 0.5f, wy = py + 0.5f;
                if (miss(wx + mx, wy + my, there) > kTolerance) {
                    c.moved++;
                    if (miss(wx, wy, here) < kTolerance) mx = my = 0, c.held++;   // (it kept its place on screen: the followed one)
                }
            }
            mv[((size_t)py * w + px) * 2] = mx, mv[((size_t)py * w + px) * 2 + 1] = my;
        }
    return c;
}

} // namespace mkwvk::objmv
