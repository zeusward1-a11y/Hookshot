// DLSS Frame Generation's decisions (gpu/vk/dlssg_logic.h), with no GPU, no Streamline and no game. tests/test_dlssg.py builds it.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "dlssg_logic.h"

using namespace mkwvk::dlssg;

static int failures = 0;
#define CHECK(x)                                                                    \
    do {                                                                            \
        if (!(x)) std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x), ++failures; \
    } while (0)
static bool near(double a, double b, double eps) { return std::fabs(a - b) <= eps * (1.0 + std::fabs(b)); }

// ---- the multiplier ----
static void multiplier() {
    CHECK(parse_multiplier(nullptr, 3) == 3);
    CHECK(parse_multiplier("", 3) == 3);
    CHECK(parse_multiplier("x", 3) == 3);
    CHECK(parse_multiplier("2x", 3) == 3);   // (not a whole number: the menu's)
    CHECK(parse_multiplier("0", 3) == 0);
    CHECK(parse_multiplier("-4", 3) == 0);
    CHECK(parse_multiplier("1", 0) == 2);
    CHECK(parse_multiplier("2", 0) == 2);
    CHECK(parse_multiplier("3", 0) == 3);
    CHECK(parse_multiplier("4", 0) == 4);
    CHECK(parse_multiplier("9", 0) == 4);

    CHECK(parse_reflex(nullptr, 1) == 1);
    CHECK(parse_reflex("abc", 1) == 1);
    CHECK(parse_reflex("0", 1) == 0);
    CHECK(parse_reflex("2", 1) == 2);
    CHECK(parse_reflex("7", 1) == 2);
    CHECK(parse_reflex("-1", 1) == 0);
    CHECK(reflex_mode(0, true) == 1);    // (DLSS-G needs Reflex while it generates)
    CHECK(reflex_mode(0, false) == 0);
    CHECK(reflex_mode(2, true) == 2);
    CHECK(reflex_mode(1, false) == 1);

    // the row: Off, 2x, 3x, 4x and round, both ways
    CHECK(menu_step(0, true) == 2);
    CHECK(menu_step(2, true) == 3);
    CHECK(menu_step(3, true) == 4);
    CHECK(menu_step(4, true) == 0);
    CHECK(menu_step(0, false) == 4);
    CHECK(menu_step(4, false) == 3);
    CHECK(menu_step(2, false) == 0);
    CHECK(menu_step(1, true) == 2);      // (not a row value: from Off)
    CHECK(normalize_multiplier(1) == 2);
    CHECK(normalize_multiplier(0) == 0);
    CHECK(normalize_multiplier(3) == 3);
    CHECK(normalize_multiplier(8) == 4);

    // numFramesToGenerate: the multiplier less one, within the GPU's maximum (unknown: 1)
    CHECK(frames_to_generate(0, 3) == 0);
    CHECK(frames_to_generate(2, 3) == 1);
    CHECK(frames_to_generate(3, 3) == 2);
    CHECK(frames_to_generate(4, 3) == 3);
    CHECK(frames_to_generate(4, 1) == 1);   // (an RTX 40: 2x at most)
    CHECK(frames_to_generate(3, 0) == 1);   // (not known yet)
    CHECK(frames_to_generate(4, 5) == 3);
}

// ---- frames ----
static void frames() {
    CHECK(frame_of(0) == 1);   // (the first frame, before any present)
    CHECK(frame_of(1) == 2);
    CHECK(frame_of(599) == 600);
    CHECK(frame_of(0xffffffffull) == 1);   // (past 32 bits: never 0)
    CHECK(frame_of(0x100000000ull) == 1);

    FrameStart s;
    CHECK(s.begin(1));
    CHECK(!s.begin(1));   // (a second acquire before the present: no second sleep)
    CHECK(s.begin(2));
    CHECK(!s.begin(2));
    CHECK(s.begin(3));

    const int64_t ms = 1000000;
    CHECK(!hold(1000 * ms, 0, 200 * ms));               // (nothing presented yet)
    CHECK(hold(1000 * ms, 990 * ms, 200 * ms));
    CHECK(!hold(1000 * ms, 700 * ms, 200 * ms));        // (minimized: no present for 300 ms)
    CHECK(!hold(1000 * ms, 1100 * ms, 200 * ms));       // (a clock that went back)

    CHECK(reset_history(false, 11, 10) == false);
    CHECK(reset_history(true, 11, 10) == true);         // (DLSS's own reset: a camera cut)
    CHECK(reset_history(false, 12, 10) == true);        // (frame 11 had no tags)
    CHECK(reset_history(false, 1, 0) == true);          // (the first)
}

// ---- on or off ----
static void decisions() {
    PresentState p;
    p.available = true, p.multiplier = 2, p.image = true, p.tagged = true;
    CHECK(decide(p) == ON);
    {
        PresentState q = p;
        q.available = false;
        CHECK(decide(q) == OFF_UNAVAILABLE);
    }
    {
        PresentState q = p;
        q.multiplier = 0;
        CHECK(decide(q) == OFF_USER);
    }
    {
        PresentState q = p;
        q.menu_open = true;
        CHECK(decide(q) == OFF_MENU);
    }
    {
        PresentState q = p;
        q.tagged = false;   // (the game's own upscale drew the frame)
        CHECK(decide(q) == OFF_NO_DLSS);
    }
    {
        PresentState q = p;
        q.image = false;
        CHECK(decide(q) == OFF_NO_IMAGE);
    }
    {
        PresentState q = p;
        q.swapchain_hold = 1;
        CHECK(decide(q) == OFF_SWAPCHAIN);
    }
    {
        PresentState q = p;
        q.status_hold = 5;
        CHECK(decide(q) == OFF_STATUS);
    }
    {
        PresentState q = p;
        q.minimized = true;
        CHECK(decide(q) == OFF_WINDOW);
    }
    {   // (the order: the player's Off wins over the menu, a resize over a missing frame)
        PresentState q = p;
        q.multiplier = 0, q.menu_open = true;
        CHECK(decide(q) == OFF_USER);
        q.multiplier = 3, q.tagged = false, q.swapchain_hold = 2;
        CHECK(decide(q) == OFF_SWAPCHAIN);
    }
    CHECK(!strcmp(off_name(ON), "on"));
    CHECK(!strcmp(off_name(OFF_NO_DLSS), "no DLSS frame"));
    CHECK(!strcmp(off_name(OFF_MENU), "menu"));
    CHECK(!strcmp(off_name(99), "?"));

    // the options: off keeps the resources except for the player's Off and an unavailable DLSS-G
    Mode m = mode_for(ON, 3, 3);
    CHECK(m.on && m.frames == 2 && m.retain);
    m = mode_for(OFF_MENU, 3, 3);
    CHECK(!m.on && m.frames == 2 && m.retain);
    m = mode_for(OFF_NO_DLSS, 4, 1);
    CHECK(!m.on && m.frames == 1 && m.retain);
    m = mode_for(OFF_USER, 0, 3);
    CHECK(!m.on && m.frames == 1 && !m.retain);
    m = mode_for(OFF_UNAVAILABLE, 2, 0);
    CHECK(!m.on && !m.retain);
    CHECK(same_mode(mode_for(ON, 2, 3), mode_for(ON, 2, 3)));
    CHECK(!same_mode(mode_for(ON, 2, 3), mode_for(ON, 3, 3)));
    CHECK(!same_mode(mode_for(ON, 2, 3), mode_for(OFF_MENU, 2, 3)));
    CHECK(!same_mode(mode_for(OFF_USER, 2, 3), mode_for(OFF_MENU, 2, 3)));

    // a resize while generating waits one present (which turns generation off), then generation waits the settle
    SwapchainGate g;
    CHECK(g.defer(true, 2));    // (generating: this present carries the off first)
    CHECK(g.hold == 2);
    PresentState q = p;
    q.swapchain_hold = g.hold;
    CHECK(decide(q) == OFF_SWAPCHAIN);
    g.presented();
    CHECK(!g.defer(false, 2));  // (the off went out: made now)
    CHECK(g.hold == 2);
    g.presented(), g.presented();
    CHECK(g.hold == 0);
    g.changed(2);               // (an out-of-date swapchain made at once)
    CHECK(g.hold == 2);
    g.presented(), g.presented(), g.presented();
    CHECK(g.hold == 0);

    StatusGate s;
    CHECK(!s.update(true, 0, 300));
    CHECK(s.hold == 0);
    CHECK(s.update(true, 2, 300));   // (Reflex not detected: new)
    CHECK(s.hold == 300);
    CHECK(!s.update(false, 0, 300)); // (while off the status is not judged)
    CHECK(s.hold == 299);
    for (int i = 0; i < 400; i++) s.update(false, 0, 300);
    CHECK(s.hold == 0);
    CHECK(!s.update(true, 2, 300));  // (the same status again: not new, held again)
    CHECK(s.hold == 300);
}

// ---- the log ----
static void logs() {
    Stats st;
    for (int i = 0; i < 590; i++) st.add(ON, 2, 0);
    for (int i = 0; i < 6; i++) st.add(OFF_MENU, 1, 0);
    for (int i = 0; i < 4; i++) st.add(OFF_NO_DLSS, 0, 0);   // (0 frames reported: the game's own was shown)
    CHECK(st.presents == 600);
    CHECK(st.shown == 1190);
    CHECK(st.on == 590);
    char line[400];
    st.line(line, sizeof line, 2);
    const char* want = "vk: dlssg: 600 presents: 1.98 frames shown per game frame (1190 shown), generating 590 at 2x, status ok; off: user 0, "
                       "menu 6, no DLSS frame 4, no image 0, swapchain 0, minimized 0, status 0, unavailable 0";
    if (strcmp(line, want)) std::printf("FAIL log line:\n  got  %s\n  want %s\n", line, want), ++failures;
    st.add(OFF_STATUS, 1, 0x2);
    st.line(line, sizeof line, 3);
    CHECK(strstr(line, "status 0x2;") != nullptr);
    st.clear();
    CHECK(st.presents == 0 && st.shown == 0 && st.off[OFF_MENU] == 0 && st.status == 0);

    LogLimit lim;
    int shown = 0;
    for (int i = 0; i < 10; i++) shown += lim.allow(3, 0);
    CHECK(shown == 3);
    LogLimit lim2;
    shown = 0;
    for (int i = 0; i < 1003; i++) shown += lim2.allow(3, 100);
    CHECK(shown == 3 + 10);
}

// ---- queues: as Streamline's own vkCreateDevice proxy plans them ----
static void queues() {
    // an RTX 5070's families (vulkaninfo): 0 graphics+compute+transfer 16, 1 transfer 2, 2 compute+transfer 8, 3 video decode,
    // 4 video encode, 5 optical flow 1
    const Family nv[6] = {{0x1 | 0x2 | 0x4 | 0x8, 16}, {0x4 | 0x8, 2}, {0x2 | 0x4 | 0x8, 8}, {0x20, 1}, {0x40, 1}, {0x100, 1}};
    QueuePlan p = plan_queues(nv, 6, 0, 1, 1, 1, true);
    CHECK(p.ok);
    CHECK(p.graphics_family == 0 && p.graphics_index == 1);   // (after the host's queue)
    CHECK(p.compute_family == 2 && p.compute_index == 0);
    CHECK(p.native_of && p.of_family == 5);
    CHECK(p.n == 3);
    CHECK(p.asks[0].family == 0 && p.asks[0].count == 2);
    CHECK(p.asks[1].family == 2 && p.asks[1].count == 1);
    CHECK(p.asks[2].family == 5 && p.asks[2].count == 1);

    // no optical flow on the device (or its feature off): interop, no such queue
    p = plan_queues(nv, 6, 0, 1, 1, 1, false);
    CHECK(p.ok && !p.native_of && p.n == 2);

    // DLSS super resolution alone asks for nothing: the host's queue, everything at its family and index 0 (as before)
    p = plan_queues(nv, 6, 0, 0, 0, 0, true);
    CHECK(p.ok && p.n == 1 && p.asks[0].count == 1 && p.graphics_family == 0 && p.graphics_index == 0 && p.compute_family == 0 &&
          p.compute_index == 0 && !p.native_of);

    // two graphics queues
    p = plan_queues(nv, 6, 0, 2, 0, 0, false);
    CHECK(p.ok && p.graphics_index == 1 && p.asks[0].count == 3);

    // no compute-only family: compute in the graphics family, after the graphics queues
    const Family one[1] = {{0x1 | 0x2 | 0x4, 4}};
    p = plan_queues(one, 1, 0, 1, 1, 0, false);
    CHECK(p.ok && p.compute_family == 0 && p.compute_index == 1 && p.graphics_family == 0 && p.graphics_index == 2);
    CHECK(p.n == 1 && p.asks[0].count == 3);

    // too few queues: not ok
    const Family small[1] = {{0x1 | 0x2, 1}};
    p = plan_queues(small, 1, 0, 1, 0, 0, false);
    CHECK(!p.ok);
    CHECK(p.asks[0].count == 1);   // (the host's queue all the same)

    // a host family out of range
    p = plan_queues(small, 1, 3, 0, 0, 0, false);
    CHECK(!p.ok);
}

// ---- the camera ----
static void mul4(const float a[16], const float b[16], float o[16]) {   // (column vectors: o = a * b)
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            double s = 0;
            for (int k = 0; k < 4; k++) s += (double)a[r * 4 + k] * b[k * 4 + c];
            o[r * 4 + c] = (float)s;
        }
}
static void apply(const float m[16], const double p[4], double o[4]) {   // (column vector)
    for (int r = 0; r < 4; r++) o[r] = m[r * 4] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3] * p[3];
}
static void apply_row(const double p[4], const float m[16], double o[4]) {   // (row vector: o = p * m)
    for (int c = 0; c < 4; c++) o[c] = p[0] * m[c] + p[1] * m[4 + c] + p[2] * m[8 + c] + p[3] * m[12 + c];
}
static void normalize3(double v[3]) {
    const double l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    for (int i = 0; i < 3; i++) v[i] /= l;
}
static void cross3(const double a[3], const double b[3], double o[3]) {
    o[0] = a[1] * b[2] - a[2] * b[1], o[1] = a[2] * b[0] - a[0] * b[2], o[2] = a[0] * b[1] - a[1] * b[0];
}
/// OpenGL's look-at and perspective, column vectors, m[r*4+c]
static void gl_camera(const double eye[3], const double at[3], const double upv[3], double fovy, double aspect, double n, double f,
                      float vp[16], double right[3], double up[3], double fwd[3]) {
    for (int i = 0; i < 3; i++) fwd[i] = at[i] - eye[i];
    normalize3(fwd);
    cross3(fwd, upv, right);
    normalize3(right);
    cross3(right, fwd, up);
    float V[16] = {(float)right[0], (float)right[1], (float)right[2], 0, (float)up[0], (float)up[1], (float)up[2], 0,
                   (float)-fwd[0], (float)-fwd[1], (float)-fwd[2], 0, 0, 0, 0, 1};
    V[3] = (float)-(right[0] * eye[0] + right[1] * eye[1] + right[2] * eye[2]);
    V[7] = (float)-(up[0] * eye[0] + up[1] * eye[1] + up[2] * eye[2]);
    V[11] = (float)(fwd[0] * eye[0] + fwd[1] * eye[1] + fwd[2] * eye[2]);
    const double ys = 1.0 / std::tan(fovy / 2), xs = ys / aspect;
    const float P[16] = {(float)xs, 0, 0, 0, 0, (float)ys, 0, 0, 0, 0, (float)(-(f + n) / (f - n)), (float)(-2 * f * n / (f - n)),
                         0, 0, -1, 0};
    mul4(P, V, vp);
}
static void camera() {
    const double eye[3] = {120.5, 34.25, -880.0}, at[3] = {100.0, 30.0, -900.0}, upw[3] = {0, 1, 0};
    double right[3], up[3], fwd[3];
    float vp[16];
    const double fovy = 0.8, aspect = 16.0 / 9.0, n = 7.5, f = 12800.0;
    gl_camera(eye, at, upw, fovy, aspect, n, f, vp, right, up, fwd);
    for (int k = 0; k < 16; k++) vp[k] *= 3.0f;   // (a homogeneous scale changes nothing)
    Camera c;
    CHECK(camera_from_view_projection(vp, c));
    for (int i = 0; i < 3; i++) {
        CHECK(near(c.pos[i], eye[i], 1e-3));
        CHECK(near(c.right[i], right[i], 1e-4));
        CHECK(near(c.up[i], up[i], 1e-4));
        CHECK(near(c.fwd[i], fwd[i], 1e-4));
    }
    CHECK(near(c.near_z, n, 1e-3));
    CHECK(near(c.far_z, f, 2e-2));
    CHECK(near(c.fov_y, fovy, 1e-4));
    CHECK(near(c.aspect, aspect, 1e-4));
    // orthonormal: what DLSS-G's scene change detection needs (the transpose is the inverse)
    const float* b[3] = {c.right, c.up, c.fwd};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            const double d = b[i][0] * b[j][0] + b[i][1] * b[j][1] + b[i][2] * b[j][2];
            CHECK(std::fabs(d - (i == j ? 1.0 : 0.0)) < 1e-5);
        }
    // view_to_clip gives the depth buffer's value and the picture's x and y for a point the game draws
    const double world[4] = {95.0, 31.0, -1100.0, 1.0};
    double gl[4];
    apply(vp, world, gl);
    const double depth = (gl[2] / gl[3] + 1.0) / 2.0;
    double rel[3] = {world[0] - eye[0], world[1] - eye[1], world[2] - eye[2]};
    const double view[4] = {rel[0] * c.right[0] + rel[1] * c.right[1] + rel[2] * c.right[2],
                            rel[0] * c.up[0] + rel[1] * c.up[1] + rel[2] * c.up[2],
                            rel[0] * c.fwd[0] + rel[1] * c.fwd[1] + rel[2] * c.fwd[2], 1.0};
    double sl[4];
    apply_row(view, c.view_to_clip, sl);
    CHECK(near(sl[2] / sl[3], depth, 1e-4));
    CHECK(near(sl[0] / sl[3], gl[0] / gl[3], 1e-4));
    CHECK(near(sl[1] / sl[3], gl[1] / gl[3], 1e-4));
    // clip_to_view undoes it
    double back[4];
    apply_row(sl, c.clip_to_view, back);
    for (int i = 0; i < 3; i++) CHECK(near(back[i] / back[3], view[i], 1e-3));

    // not a perspective: refused
    float ortho[16] = {0.01f, 0, 0, 0, 0, 0.01f, 0, 0, 0, 0, -0.001f, -1, 0, 0, 0, 1};
    CHECK(!camera_from_view_projection(ortho, c));
    float zero[16] = {};
    CHECK(!camera_from_view_projection(zero, c));
}

static void clip_conversion() {
    // two frames of a moving camera; the game's clip-to-previous (vk_draw.cpp clip_to_previous: prev * cur^-1)
    double r[3], u[3], fw[3];
    float cur[16], prev[16], inv[16], m[16];
    const double e1[3] = {0, 2, 10}, a1[3] = {0, 1, 0}, e0[3] = {0.4, 2.1, 10.3}, a0[3] = {0.2, 1, 0}, upw[3] = {0, 1, 0};
    gl_camera(e1, a1, upw, 1.0, 16.0 / 9.0, 0.5, 500.0, cur, r, u, fw);
    gl_camera(e0, a0, upw, 1.0, 16.0 / 9.0, 0.5, 500.0, prev, r, u, fw);
    CHECK(invert4(cur, inv));
    mul4(prev, inv, m);
    float s[16];
    sl_clip_to_prev(m, s);
    const double world[4] = {1.5, 0.25, -3.0, 1.0};
    double gc[4], gp[4];
    apply(cur, world, gc);
    apply(prev, world, gp);
    // Streamline's clip: z the depth buffer's value times w
    const double sc[4] = {gc[0], gc[1], (gc[2] + gc[3]) / 2, gc[3]}, want[4] = {gp[0], gp[1], (gp[2] + gp[3]) / 2, gp[3]};
    double got[4];
    apply_row(sc, s, got);
    for (int i = 0; i < 4; i++) CHECK(near(got[i] / got[3], want[i] / want[3], 1e-4));
    // and its inverse is the previous-to-current
    float back[16];
    CHECK(invert4(s, back));
    double again[4];
    apply_row(want, back, again);
    for (int i = 0; i < 4; i++) CHECK(near(again[i] / again[3], sc[i] / sc[3], 1e-4));
    // the identity stays the identity
    const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    sl_clip_to_prev(id, s);
    for (int i = 0; i < 16; i++) CHECK(near(s[i], id[i], 1e-6));
    float sing[16] = {};
    CHECK(!invert4(sing, back));
}

int main() {
    multiplier();
    frames();
    decisions();
    logs();
    queues();
    camera();
    clip_conversion();
    std::printf("dlssg logic: %d failures\n", failures);
    return failures ? 1 : 0;
}
