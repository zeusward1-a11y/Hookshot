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

int main() {
    depth_terms();
    runner();
    runner_game_scale();
    strafe();
    still_world();
    crossing();
    off_and_first();
    sky_turn();
    failures += scene_failures;
    std::printf("dlss own motion: %d failures\n", failures);
    return failures ? 1 : 0;
}
