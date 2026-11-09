// Own motion for DLSS and DLSS Frame Generation (gpu/vk/dlss_objmv.h, the motion pass on the CPU) on scenes drawn here by casting
// rays: a wall far behind, and a box standing for Link that the camera follows. No GPU, no game. tests/test_dlss_objmv.py builds it.
#include <cmath>
#include <cstdio>
#include <vector>

#include "objmv_scene.h"

using namespace mkwvk;
using namespace objmv_scene;

static int failures = 0;
#define CHECK(x)                                                                    \
    do {                                                                            \
        if (!(x)) std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x), ++failures; \
    } while (0)

/// The box's pixels at least m pixels inside it in both frames, and the wall's at least m pixels from the box in both and m from the
/// screen's border
static bool inside_box(const Run& r, int x, int y, int m) {
    for (int dy = -m; dy <= m; dy++)
        for (int dx = -m; dx <= m; dx++) {
            const int qx = x + dx, qy = y + dy;
            if (qx < 0 || qy < 0 || qx >= W || qy >= H || !r.box0[qy * W + qx] || !r.box1[qy * W + qx]) return false;
        }
    return true;
}
static bool clear_wall(const Run& r, int x, int y, int m) {
    if (x < m || y < m || x >= W - m || y >= H - m) return false;
    for (int dy = -m; dy <= m; dy++)
        for (int dx = -m; dx <= m; dx++)
            if (r.box0[(y + dy) * W + x + dx] || r.box1[(y + dy) * W + x + dx]) return false;
    return true;
}
static float len(const std::vector<float>& mv, int i) { return std::hypot(mv[i * 2], mv[i * 2 + 1]); }

static void depth_terms() {
    Cam c{{3, 2, 9}, {1, 1.5, -2}};
    float vp[16], a, b;
    view_projection(c, vp);
    CHECK(objmv::depth_terms(vp, a, b));
    // a point 12.5 in front of the camera, through the projection: its distance back
    double fw[3] = {c.at[0] - c.eye[0], c.at[1] - c.eye[1], c.at[2] - c.eye[2]};
    normalize(fw);
    const double p[3] = {c.eye[0] + fw[0] * 12.5, c.eye[1] + fw[1] * 12.5, c.eye[2] + fw[2] * 12.5};
    double clip[4];
    for (int r = 0; r < 4; r++) clip[r] = vp[r * 4] * p[0] + vp[r * 4 + 1] * p[1] + vp[r * 4 + 2] * p[2] + vp[r * 4 + 3];
    CHECK(std::fabs(objmv::view_distance((float)(clip[2] / clip[3]), a, b) - 12.5f) < 1e-3f);
    CHECK(std::fabs(objmv::view_distance(-1.0f, a, b) - 0.5f) < 1e-4f);    // (near)
    CHECK(std::fabs(objmv::view_distance(1.0f, a, b) - 800.0f) < 0.5f);    // (far)
    const float ortho[16] = {0.1f, 0, 0, 0, 0, 0.1f, 0, 0, 0, 0, -0.01f, -1, 0, 0, 0, 1};
    CHECK(!objmv::depth_terms(ortho, a, b));
}

/// Link runs away from the camera, which follows him at the same distance: on screen he stands still, and the camera's motion alone
/// gave his pixels the camera's forward motion (the trail). The check finds them all and makes their motion zero; the wall keeps its
static void runner() {
    Scene s0, s1;
    s0.has_box = s1.has_box = true;
    s0.box = {{-0.5, 0.0, 3.5}, {0.5, 1.8, 4.5}};   // (4 units ahead of the camera at z 8)
    s1.box = {{-0.5, 0.0, 3.0}, {0.5, 1.8, 4.0}};   // (0.5 further: as the camera)
    const Cam c0{{0, 1.2, 8.0}, {0, 1.0, -20}}, c1{{0, 1.2, 7.5}, {0, 1.0, -20.5}};
    const Run r = two_frames(s0, c0, s1, c1);
    int box = 0, box_zero = 0, box_cam_moving = 0, wall = 0, wall_kept = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const int i = y * W + x;
            if (inside_box(r, x, y, 2)) {
                box++;
                box_zero += len(r.mv, i) < 1e-6f;
                box_cam_moving += len(r.cam_mv, i) > 0.2f;   // (the camera's motion alone moved him: the trail)
            } else if (clear_wall(r, x, y, 3)) {
                wall++;
                wall_kept += std::fabs(r.mv[i * 2] - r.cam_mv[i * 2]) < 1e-6f && std::fabs(r.mv[i * 2 + 1] - r.cam_mv[i * 2 + 1]) < 1e-6f;
            }
        }
    std::printf("runner: box %d px, %d zero (camera-only moved %d), wall %d px, %d as the camera's; moved %u held %u of %u\n", box,
                box_zero, box_cam_moving, wall, wall_kept, r.counts.moved, r.counts.held, r.counts.checked);
    CHECK(box > 200);
    CHECK(box_cam_moving > box / 2);           // (the trail was there: what the check is for)
    CHECK(box_zero >= box * 99 / 100);         // (gone)
    CHECK(wall > 2000);
    CHECK(wall_kept >= wall * 99 / 100);       // (the wall keeps the camera's motion)
    CHECK(r.counts.held > 0 && r.counts.held <= r.counts.moved && r.counts.moved <= r.counts.checked);
}

/// The runner at the game's scale: near 0.1 and far 25005 (the game's projection, the log's "near 0.100010954, far 25005.932"),
/// Link 50 units tall 300 units ahead, the camera following him. kSkyDepth leaves the check to everything nearer than ~7100 units:
/// he is still found. (A threshold of 0.999 would cut the check at ~100 units and leave him with the camera's motion; the scenes
/// above, 0.5 to 800 units, would not see it)
static void runner_game_scale() {
    Scene s0, s1;
    s0.wall_z = s1.wall_z = -3000.0;                 // (the far side of the field)
    s0.has_box = s1.has_box = true;
    s0.box = {{-15.0, 0.0, -15.0}, {15.0, 50.0, 15.0}};
    s1.box = {{-15.0, 0.0, -35.0}, {15.0, 50.0, -5.0}};   // (20 units on: a run step)
    Cam c0{{0, 80, 300}, {0, 40, 0}}, c1{{0, 80, 280}, {0, 40, -20}};
    c0.n = c1.n = 0.1, c0.f = c1.f = 25005.0;
    const Run r = two_frames(s0, c0, s1, c1);
    int box = 0, box_zero = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (inside_box(r, x, y, 2)) box++, box_zero += len(r.mv, y * W + x) < 1e-6f;   // (2: off his outline, as runner)
    std::printf("runner at the game's scale: box %d px, %d zero; moved %u held %u of %u\n", box, box_zero, r.counts.moved,
                r.counts.held, r.counts.checked);
    CHECK(box > 30);
    CHECK(box_zero >= box * 9 / 10);
}

/// The camera strafes beside Link, who keeps his place on screen. A box seen face on, it moves without changing its distance: the
/// check sees it only where the camera's motion lands past his outline (the band as wide as that motion along the edge it trails).
/// Its middle keeps the camera's motion: this method's limit (a body's own relief helps in the game)
static void strafe() {
    Scene s0, s1;
    s0.has_box = s1.has_box = true;
    s0.box = {{-0.6, 0.0, 3.8}, {0.6, 1.8, 4.0}};
    s1.box = {{-0.3, 0.0, 3.8}, {0.9, 1.8, 4.0}};   // (0.3 to the right, with the camera)
    const Cam c0{{0, 1.0, 8.0}, {0, 1.0, -20}}, c1{{0.3, 1.0, 8.0}, {0.3, 1.0, -20}};
    const Run r = two_frames(s0, c0, s1, c1);
    int band = 0, band_zero = 0, middle = 0, middle_kept = 0, wall = 0, wall_kept = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const int i = y * W + x;
            if (!inside_box(r, x, y, 1)) {
                if (clear_wall(r, x, y, 3)) {
                    wall++;
                    wall_kept += std::fabs(r.mv[i * 2] - r.cam_mv[i * 2]) < 1e-6f;
                }
                continue;
            }
            const int reach = (int)std::ceil(std::fabs(r.cam_mv[i * 2])) + 2;   // (where the camera's vector lands past the outline)
            const bool near_trail = x + (int)std::floor(r.cam_mv[i * 2]) - 1 < 0 || !r.box0[y * W + std::max(0, x + (int)std::floor(r.cam_mv[i * 2]) - 1)];
            if (near_trail) {
                band++;
                band_zero += len(r.mv, i) < 1e-6f;
            } else if (!inside_box(r, x, y, reach)) {
                continue;   // (the transition: either way)
            } else {
                middle++;
                middle_kept += std::fabs(r.mv[i * 2] - r.cam_mv[i * 2]) < 1e-6f;
            }
        }
    std::printf("strafe: band %d px, %d zero; middle %d px, %d as the camera's; wall %d, %d kept\n", band, band_zero, middle, middle_kept,
                wall, wall_kept);
    CHECK(band > 50 && band_zero >= band * 9 / 10);
    CHECK(middle > 100 && middle_kept >= middle * 9 / 10);   // (the documented limit, so that a change of it shows)
    CHECK(wall_kept >= wall * 99 / 100);
}

/// No own motion anywhere: the camera moves and turns over still things. Nothing but the screen's edge may be taken for motion
static void still_world() {
    Scene s;
    s.has_box = true;
    s.box = {{-2.0, 0.0, -6.0}, {1.0, 3.0, -4.0}};   // (a still block before the wall)
    const Cam c0{{0, 1.5, 6.0}, {0, 1.2, -20}}, c1{{0.25, 1.55, 5.7}, {1.5, 1.0, -20}};   // (a step and a turn)
    const Run r = two_frames(s, c0, s, c1);
    uint32_t interior = 0, changed = 0;
    for (int y = 4; y < H - 4; y++)
        for (int x = 4; x < W - 4; x++) {
            const int i = y * W + x;
            interior++;
            changed += std::fabs(r.mv[i * 2] - r.cam_mv[i * 2]) > 1e-6f || std::fabs(r.mv[i * 2 + 1] - r.cam_mv[i * 2 + 1]) > 1e-6f;
        }
    std::printf("still world: %u of %u pixels changed, %u moved, %u held\n", changed, interior, r.counts.moved, r.counts.held);
    CHECK(changed * 200 <= interior);   // (at most 0.5%: silhouettes revealed by the turn)
    // (and little taken for motion at all, which the log counts: the 3x3 keeps silhouettes reprojected a pixel off out, 1x1 had 4x)
    CHECK(r.counts.moved * 1000 <= r.counts.checked * 3);
}

/// Something moves across the screen by itself, not followed (an NPC walking past): it moved, but did not keep its place on screen,
/// so it keeps the camera's motion, as the wall it uncovered does; only pixels held still may differ from the camera's motion
static void crossing() {
    Scene s0, s1;
    s0.has_box = s1.has_box = true;
    s0.box = {{-1.5, 0.0, -3.0}, {-0.5, 1.8, -2.0}};
    s1.box = {{0.0, 0.0, -3.0}, {1.0, 1.8, -2.0}};   // (1.5 to the right)
    const Cam c0{{0, 1.0, 8.0}, {0, 1.0, -20}}, c1{{0.1, 1.0, 8.0}, {0.1, 1.0, -20}};
    const Run r = two_frames(s0, c0, s1, c1);
    uint32_t differ = 0, differ_not_still = 0;
    for (int i = 0; i < W * H; i++)
        if (r.mv[i * 2] != r.cam_mv[i * 2] || r.mv[i * 2 + 1] != r.cam_mv[i * 2 + 1]) differ++, differ_not_still += len(r.mv, i) != 0.0f;
    std::printf("crossing: moved %u, held %u, %u pixels differ from the camera's motion\n", r.counts.moved, r.counts.held, differ);
    CHECK(r.counts.moved > 200);                    // (the walker and what it uncovered: found, 400 here)
    CHECK(r.counts.held * 10 < r.counts.moved);     // (but not made still: it did not keep its place on screen)
    CHECK(differ == r.counts.held && differ_not_still == 0);
}

/// The check off (RX_DLSS_OBJMV=0), or no last frame: the camera's motion exactly, nothing counted
static void off_and_first() {
    Scene s0, s1;
    s0.has_box = s1.has_box = true;
    s0.box = {{-0.5, 0.0, 3.5}, {0.5, 1.8, 4.5}};
    s1.box = {{-0.5, 0.0, 3.0}, {0.5, 1.8, 4.0}};
    const Cam c0{{0, 1.2, 8.0}, {0, 1.0, -20}}, c1{{0, 1.2, 7.5}, {0, 1.0, -20.5}};
    for (int mode = 0; mode < 2; mode++) {
        const Run r = two_frames(s0, c0, s1, c1, mode == 1, mode == 0);
        bool same = true;
        for (size_t i = 0; i < r.mv.size(); i++) same = same && r.mv[i] == r.cam_mv[i];
        CHECK(same);
        CHECK(r.counts.checked == 0 && r.counts.moved == 0);
    }
}

/// The sky (the depth buffer's clear value, nothing drawn) under a quick turn of the camera: it keeps the camera's motion. The cleared
/// depth is the far plane, a plane and not a sphere: the distance the turn predicts there misses last frame's by 11-15% at the
/// screen's sides (8 degrees, this FOV; held from ~4 degrees a frame on), more than the tolerance, and the same pixel was sky last frame too, so the check held it still on screen: the
/// sky and the horizon stuck to the screen while the camera turned (the blur the player saw with own motion on, 09/11/2026)
static void sky_turn() {
    Scene s;
    s.has_wall = false;
    s.has_box = true;
    s.box = {{-1.0, 0.0, -10.0}, {1.0, 2.0, -8.0}};   // (something drawn, so that not every pixel is sky)
    const double turn = std::tan(8.0 * 3.14159265358979 / 180.0) * 26.0;   // (8 degrees between two frames: a quick turn at 40 fps)
    const Cam c0{{0, 1.5, 6.0}, {0, 1.2, -20}}, c1{{0, 1.5, 6.0}, {turn, 1.2, -20}};
    const Run r = two_frames(s, c0, s, c1);
    int sky = 0, sky_changed = 0, sky_still = 0;
    for (int i = 0; i < W * H; i++) {
        if (r.depth1[i] < 1.0f) continue;
        sky++;
        const bool changed = r.mv[i * 2] != r.cam_mv[i * 2] || r.mv[i * 2 + 1] != r.cam_mv[i * 2 + 1];
        sky_changed += changed, sky_still += changed && len(r.mv, i) == 0.0f;
    }
    std::printf("sky turn: sky %d px, %d changed from the camera's motion (%d made still); moved %u held %u of %u\n", sky, sky_changed,
                sky_still, r.counts.moved, r.counts.held, r.counts.checked);
    CHECK(sky > W * H / 2);
    CHECK(sky_changed == 0);
}

/// DLSS's jitter (vk_draw.cpp dlss_jitter_of): the scene drawn with a sub-pixel viewport offset gives the unjittered vectors (DLSS's
/// default) when the pass takes each pixel's surface where an unjittered draw puts it, its centre less the offset. The camera walks
/// towards the wall, so the vectors grow across the screen: a pass that left the offset in, or took it the wrong way, is off by
/// their gradient times the offset. The truth: each pixel's surface point (the ray's hit) seen by last frame's camera
static void jittered_draw() {
    Scene s;
    s.has_box = false;   // (the wall alone: no silhouette, every pixel's depth exact)
    const Cam c0{{0, 1.5, 6.0}, {0, 1.2, -60}}, c1{{0.4, 1.6, -14.0}, {0.6, 1.3, -60}};
    float vp0[16], vp1[16], inv1[16], clip[16], a1 = 0, b1 = 0, a0 = 0, b0 = 0;
    view_projection(c0, vp0), view_projection(c1, vp1);
    CHECK(objmv::depth_terms(vp1, a1, b1) && objmv::depth_terms(vp0, a0, b0));
    dlssg::invert4(vp1, inv1);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            double v = 0;
            for (int k = 0; k < 4; k++) v += (double)vp0[i * 4 + k] * inv1[k * 4 + j];
            clip[i * 4 + j] = (float)v;
        }
    const float jx = 0.375f, jy = -0.2777f;   // (a Halton(2,3) point less 0.5)
    std::vector<float> d1, mv, ignore;
    std::vector<char> box;
    std::vector<double> hit;
    draw(s, vp1, d1, box, jx, jy, &hit);
    const auto worst = [&](float tx, float ty) {   // (the largest error against the truth, in render pixels)
        objmv::motion_pass(W, H, d1.data(), clip, a1, b1, false, false, nullptr, mv, ignore, tx, ty);
        double e = 0;
        for (int py = 2; py < H - 2; py++)
            for (int px = 2; px < W - 2; px++) {
                const double* q = hit.data() + (size_t)(py * W + px) * 3;
                double c[4];
                for (int r = 0; r < 4; r++) c[r] = vp0[r * 4] * q[0] + vp0[r * 4 + 1] * q[1] + vp0[r * 4 + 2] * q[2] + vp0[r * 4 + 3];
                const double wx = (c[0] / c[3] * 0.5 + 0.5) * W, wy = (0.5 - c[1] / c[3] * 0.5) * H;   // (last frame, window y down)
                const double ux = px + 0.5 - jx, uy = py + 0.5 - jy;   // (this frame, unjittered)
                const int i = py * W + px;
                e = std::max(e, std::hypot(mv[i * 2] - (wx - ux), mv[i * 2 + 1] - (wy - uy)));
            }
        return e;
    };
    const double right = worst(jx, jy), none = worst(0, 0), flipped = worst(-jx, -jy);
    std::printf("jittered draw: worst error %.5f px with the offset, %.4f without it, %.4f the wrong way\n", right, none, flipped);
    CHECK(right < 0.002);
    CHECK(none > 0.05 && none > 20 * right);
    CHECK(flipped > 1.5 * none);
}

/// DLSS's bias mask (RX_DLSS_OBJMV 2 and 3): the mask's value on every pixel the check found moved by itself, 0 elsewhere. With 2 the
/// camera's vectors stay (frame generation keeps them; only DLSS super resolution takes the mask), with 3 the followed character's
/// are zero as with 1. Link running away from the camera behind him: his pixels and the band he uncovered are masked
static void bias_mask() {
    Scene r0, r1;
    r0.has_box = r1.has_box = true;
    r0.box = {{-0.5, 0.0, 3.5}, {0.5, 1.8, 4.5}}, r1.box = {{-0.5, 0.0, 3.0}, {0.5, 1.8, 4.0}};
    const Cam c0{{0, 1.2, 8.0}, {0, 1.0, -20}}, c1{{0, 1.2, 7.5}, {0, 1.0, -20.5}};
    const Run plain = two_frames(r0, c0, r1, c1);                               // (1: own motion, no mask)
    const Run kept = two_frames(r0, c0, r1, c1, true, true, 0, 0, 0.65f, true);   // (2)
    const Run both = two_frames(r0, c0, r1, c1, true, true, 0, 0, 0.65f, false);  // (3)
    uint32_t masked = 0, wrong_value = 0, box = 0, box_masked = 0;
    bool kept_is_camera = true, both_is_plain = true, same_mask = true;
    for (int i = 0; i < W * H; i++) {
        const float m = kept.mask[i];
        masked += m > 0, wrong_value += m != 0 && m != 0.65f;
        same_mask = same_mask && m == both.mask[i];
        kept_is_camera = kept_is_camera && kept.mv[i * 2] == kept.cam_mv[i * 2] && kept.mv[i * 2 + 1] == kept.cam_mv[i * 2 + 1];
        both_is_plain = both_is_plain && both.mv[i * 2] == plain.mv[i * 2] && both.mv[i * 2 + 1] == plain.mv[i * 2 + 1];
        if (inside_box(kept, i % W, i / W, 2)) box++, box_masked += m > 0;
    }
    std::printf("bias mask: %u pixels masked (moved %u), Link %u of %u masked; the vectors: kept %s, with motion %s\n", masked,
                kept.counts.moved, box_masked, box, kept_is_camera ? "the camera's" : "CHANGED", both_is_plain ? "as 1" : "DIFFER");
    CHECK(masked == kept.counts.moved && masked > 0 && wrong_value == 0);
    CHECK(box > 0 && box_masked * 10 >= box * 9);   // (his inside, nine in ten at least)
    CHECK(kept_is_camera && both_is_plain && same_mask);
    bool none = true;   // (no mask asked: nothing written)
    for (float v : plain.mask) none = none && v == 0.0f;
    CHECK(none);
    // an NPC crossing, not followed: moved, never held; the mask is on what moved, not only on what was held
    Scene s0, s1;
    s0.has_box = s1.has_box = true;
    s0.box = {{-1.5, 0.0, -3.0}, {-0.5, 1.8, -2.0}};
    s1.box = {{0.0, 0.0, -3.0}, {1.0, 1.8, -2.0}};
    const Cam k0{{0, 1.0, 8.0}, {0, 1.0, -20}}, k1{{0.1, 1.0, 8.0}, {0.1, 1.0, -20}};
    const Run cross = two_frames(s0, k0, s1, k1, true, true, 0, 0, 0.65f, true);
    uint32_t cross_masked = 0;
    for (float v : cross.mask) cross_masked += v > 0;
    std::printf("bias mask, crossing: %u masked, moved %u, held %u\n", cross_masked, cross.counts.moved, cross.counts.held);
    CHECK(cross.counts.held == 0 && cross.counts.moved > 200 && cross_masked == cross.counts.moved);
}

/// Both frames jittered, each with its own offset (the motion pass is given last frame's, vk_draw.cpp ds.dist_jx): the check finds
/// the same that moved and the same held as with no jitter at all; without last frame's offset its looks land a fraction of a pixel
/// off and a still world starts to look moved (the dense grass taken for motion, worse with jitter)
static void both_jittered() {
    Scene r0, r1;
    r0.has_box = r1.has_box = true;
    r0.box = {{-0.5, 0.0, 3.5}, {0.5, 1.8, 4.5}}, r1.box = {{-0.5, 0.0, 3.0}, {0.5, 1.8, 4.0}};
    const Cam c0{{0, 1.2, 8.0}, {0, 1.0, -20}}, c1{{0, 1.2, 7.5}, {0, 1.0, -20.5}};
    const Run plain = two_frames(r0, c0, r1, c1);
    const Run jit = two_frames(r0, c0, r1, c1, true, true, 0.375f, -0.2777f, 0, false, -0.25f, 0.1111f);
    std::printf("both jittered: moved %u held %u (unjittered %u, %u)\n", jit.counts.moved, jit.counts.held, plain.counts.moved,
                plain.counts.held);
    const auto near = [](uint32_t a, uint32_t b) { return (a > b ? a - b : b - a) * 50 <= b + 50; };   // (2%: the edges' pixels)
    CHECK(near(jit.counts.moved, plain.counts.moved) && near(jit.counts.held, plain.counts.held));
    // a still world under a moving camera, both frames jittered: still nothing taken for motion
    Scene s;
    s.has_box = true;
    s.box = {{-2.0, 0.0, -6.0}, {1.0, 3.0, -4.0}};
    const Cam sc0{{0, 1.5, 6.0}, {0, 1.2, -20}}, sc1{{0.25, 1.55, 5.7}, {1.5, 1.0, -20}};
    const Run still = two_frames(s, sc0, s, sc1, true, true, 0.375f, -0.2777f, 0, false, -0.25f, 0.1111f);
    std::printf("both jittered, still world: moved %u of %u\n", still.counts.moved, still.counts.checked);
    CHECK(still.counts.moved * 1000 <= still.counts.checked * 3);
    // (the 3x3 closest hides a look a fraction of a pixel off on smooth scenes, so the two above pass without last frame's jitter
    // too: the looks below do not)
    // (the 3x3 closest hides a look a fraction of a pixel off on smooth scenes: objmv_scene.h look_offset puts the right distance
    // where only the look that takes the jitters right finds it)
    for (const Look& l : kLooks) {
        const int line = l.rows ? W : H;
        const int with = look_missed(look_offset(l.pjx, l.pjy, l.jx, l.jy, l.rows), l.rows);
        const int without = look_missed(look_offset(0, 0, 0, 0, l.rows), l.rows);
        const int wrong = look_missed(look_offset(-l.pjx, -l.pjy, -l.jx, -l.jy, l.rows), l.rows);
        std::printf("look with %s: %d of %d taken for motion with it, %d without, %d the wrong way\n", l.what, with, line, without, wrong);
        CHECK(with == 0 && without == line && wrong == line);
    }
}

int main() {
    depth_terms();
    runner();
    runner_game_scale();
    strafe();
    still_world();
    crossing();
    off_and_first();
    sky_turn();
    jittered_draw();
    bias_mask();
    both_jittered();
    failures += scene_failures;
    std::printf("dlss own motion: %d failures\n", failures);
    return failures ? 1 : 0;
}
