// The scenes of the own-motion tests (tests/dlss_objmv_test.cpp on the CPU, tests/dlss_objmv_gpu.cpp through the real shader):
// a wall far behind and a box standing for Link, drawn by casting rays, two frames of them through gpu/vk/dlss_objmv.h.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "dlss_objmv.h"
#include "dlssg_logic.h"

namespace objmv_scene {
using namespace mkwvk;

inline int scene_failures = 0;
#define SCENE_CHECK(x)                                                                                          \
    do {                                                                                                        \
        if (!(x)) std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x), ++objmv_scene::scene_failures;       \
    } while (0)

constexpr int W = 192, H = 108;

struct Cam {
    double eye[3], at[3];
    double fovy = 0.9, aspect = 16.0 / 9.0, n = 0.5, f = 800.0;
};
struct Box {   // (axis-aligned)
    double lo[3], hi[3];
};
struct Scene {
    double wall_z = -40;   // (a wall facing the camera, everywhere)
    bool has_box = false;
    Box box{};
};

inline void normalize(double v[3]) {
    const double l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    for (int i = 0; i < 3; i++) v[i] /= l;
}
inline void cross(const double a[3], const double b[3], double o[3]) {
    o[0] = a[1] * b[2] - a[2] * b[1], o[1] = a[2] * b[0] - a[0] * b[2], o[2] = a[0] * b[1] - a[1] * b[0];
}
/// OpenGL's look-at and perspective, column vectors (clip = vp * world), as the game's view-projection
inline void view_projection(const Cam& c, float vp[16]) {
    double fw[3] = {c.at[0] - c.eye[0], c.at[1] - c.eye[1], c.at[2] - c.eye[2]}, up[3] = {0, 1, 0}, r[3], u[3];
    normalize(fw);
    cross(fw, up, r);
    normalize(r);
    cross(r, fw, u);
    const double V[16] = {r[0], r[1], r[2], -(r[0] * c.eye[0] + r[1] * c.eye[1] + r[2] * c.eye[2]),
                          u[0], u[1], u[2], -(u[0] * c.eye[0] + u[1] * c.eye[1] + u[2] * c.eye[2]),
                          -fw[0], -fw[1], -fw[2], fw[0] * c.eye[0] + fw[1] * c.eye[1] + fw[2] * c.eye[2], 0, 0, 0, 1};
    const double ys = 1 / std::tan(c.fovy / 2), xs = ys / c.aspect;
    const double P[16] = {xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, -(c.f + c.n) / (c.f - c.n), -2 * c.f * c.n / (c.f - c.n), 0, 0, -1, 0};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            double s = 0;
            for (int k = 0; k < 4; k++) s += P[i * 4 + k] * V[k * 4 + j];
            vp[i * 4 + j] = (float)s;
        }
}
/// The depth buffer of a scene seen through vp (the pixel centres' rays; window y down, as the game's negative viewport height),
/// and which pixels show the box
inline void draw(const Scene& s, const float vp[16], std::vector<float>& depth, std::vector<char>& on_box) {
    float inv[16];
    dlssg::invert4(vp, inv);
    depth.assign(W * H, 1.0f);
    on_box.assign(W * H, 0);
    const auto unproject = [&](double x, double y, double z, double o[3]) {
        double p[4];
        for (int r = 0; r < 4; r++) p[r] = inv[r * 4] * x + inv[r * 4 + 1] * y + inv[r * 4 + 2] * z + inv[r * 4 + 3];
        for (int i = 0; i < 3; i++) o[i] = p[i] / p[3];
    };
    for (int py = 0; py < H; py++)
        for (int px = 0; px < W; px++) {
            const double x = (px + 0.5) / W * 2 - 1, y = 1 - (py + 0.5) / H * 2;
            double a[3], b[3], d[3];
            unproject(x, y, -1, a), unproject(x, y, 1, b);
            for (int i = 0; i < 3; i++) d[i] = b[i] - a[i];
            double best = 1e30, hit[3];
            bool box = false;
            if (std::fabs(d[2]) > 1e-12) {   // (the wall)
                const double t = (s.wall_z - a[2]) / d[2];
                if (t > 0 && t < best) best = t;
            }
            if (s.has_box) {   // (slabs)
                double t0 = 0, t1 = 1e30;
                bool ok = true;
                for (int i = 0; i < 3 && ok; i++) {
                    if (std::fabs(d[i]) < 1e-12) {
                        ok = a[i] >= s.box.lo[i] && a[i] <= s.box.hi[i];
                        continue;
                    }
                    double u = (s.box.lo[i] - a[i]) / d[i], v = (s.box.hi[i] - a[i]) / d[i];
                    if (u > v) std::swap(u, v);
                    t0 = std::max(t0, u), t1 = std::min(t1, v);
                    ok = t0 <= t1;
                }
                if (ok && t0 > 0 && t0 < best) best = t0, box = true;
            }
            if (best >= 1e30) continue;
            for (int i = 0; i < 3; i++) hit[i] = a[i] + d[i] * best;
            double c[4];
            for (int r = 0; r < 4; r++) c[r] = vp[r * 4] * hit[0] + vp[r * 4 + 1] * hit[1] + vp[r * 4 + 2] * hit[2] + vp[r * 4 + 3];
            depth[py * W + px] = (float)(c[2] / c[3] * 0.5 + 0.5);
            on_box[py * W + px] = box;
        }
}

struct Run {
    std::vector<float> mv, cam_mv, dist;
    std::vector<float> depth1, dist0;   // (the checked frame's depth, last frame's distances)
    float clip[16], a1, b1;
    std::vector<char> box0, box1;
    objmv::Counts counts;
};
/// Two frames: the first gives last frame's distances, the second is checked; cam_mv is the second's with the check off
inline Run two_frames(const Scene& s0, const Cam& c0, const Scene& s1, const Cam& c1, bool on = true, bool prev_ok = true) {
    Run r;
    float vp0[16], vp1[16], inv1[16], clip[16];
    view_projection(c0, vp0), view_projection(c1, vp1);
    std::vector<float> d0, d1, mv0, dist0;
    draw(s0, vp0, d0, r.box0), draw(s1, vp1, d1, r.box1);
    float a0, b0, a1, b1;
    SCENE_CHECK(objmv::depth_terms(vp0, a0, b0) && objmv::depth_terms(vp1, a1, b1));
    const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    objmv::motion_pass(W, H, d0.data(), id, a0, b0, true, false, nullptr, mv0, dist0);
    dlssg::invert4(vp1, inv1);
    for (int i = 0; i < 4; i++)   // (vk_draw.cpp clip_to_previous: previous * current^-1)
        for (int j = 0; j < 4; j++) {
            double v = 0;
            for (int k = 0; k < 4; k++) v += (double)vp0[i * 4 + k] * inv1[k * 4 + j];
            clip[i * 4 + j] = (float)v;
        }
    r.counts = objmv::motion_pass(W, H, d1.data(), clip, a1, b1, on, prev_ok, dist0.data(), r.mv, r.dist);
    r.depth1 = d1, r.dist0 = dist0, r.a1 = a1, r.b1 = b1;
    std::copy_n(clip, 16, r.clip);
    std::vector<float> ignore;
    objmv::motion_pass(W, H, d1.data(), clip, a1, b1, false, false, nullptr, r.cam_mv, ignore);
    return r;
}

} // namespace objmv_scene
