// gpu/vk/dlss_actor_plan.h without the game or a GPU: the n-th draw of a model across frames and after frames DLSS did not make
// (the review of 09/11: a list left by a fallback shifted the n-ths and gave a draw another one's pose), the two pose buffers taking
// turns, when the frame before's pose is taken, the push constants, the cameras' buffer, RX_DLSS_ACTOR_TOL. Built and run by
// tests/test_dlss_actor.py.
#include <set>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "dlss_actor_plan.h"

using namespace dlss_actor;

static int failures = 0;
#define CHECK(x)                                                                    \
    do {                                                                            \
        if (!(x)) std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x), ++failures; \
    } while (0)

struct Item {
    uint64_t key = 0;
};
/// One frame's draws through Noted as dlss_skin_check notes them: their keys, in order
static std::vector<uint64_t> frame_of(Noted<Item>& n, uint64_t frame, const std::vector<uint64_t>& models) {
    std::vector<uint64_t> keys;
    for (uint64_t m : models) {
        Item it;
        if (!n.next_key(m, frame, &it.key)) break;
        n.draws.push_back(it);
        keys.push_back(it.key);
    }
    return keys;
}

static void noted() {
    const uint64_t a = key(0x1000, 0x2000, 300, 0, 7, 0), b = key(0x3000, 0x4000, 90, 2, 7, 0);
    CHECK(same_model(a, key(0x1000, 0x2000, 300, 0, 7, 3)) && !same_model(a, b));
    Noted<Item> n;
    // (the same model three times with another between: 0, 1, 2 for it, 0 for the other)
    const std::vector<uint64_t> f1 = frame_of(n, 10, {a, b, a, a});
    CHECK(f1.size() == 4 && f1[0] == key(0x1000, 0x2000, 300, 0, 7, 0) && f1[1] == b && f1[2] == key(0x1000, 0x2000, 300, 0, 7, 1) &&
          f1[3] == key(0x1000, 0x2000, 300, 0, 7, 2));
    CHECK(n.take(10).size() == 4 && n.draws.empty());
    // (the next frame starts its n-ths again: the same keys)
    CHECK(frame_of(n, 11, {a, b, a, a}) == f1);
    // a frame DLSS did not make: its draws are not taken (no pass), and the next frame's n-ths are its own, not shifted by them
    // (before: the left list counted, the n-th draw took the (n + left)-th's pose)
    frame_of(n, 12, {a, a, a});
    CHECK(frame_of(n, 13, {a, b, a, a}) == f1);
    CHECK(n.take(13).size() == 4);
    // what is noted in one frame is not another frame's to take
    frame_of(n, 14, {a, b});
    CHECK(n.take(15).empty() && n.draws.empty());
    // at most kMost a frame; the next frame notes again
    std::vector<uint64_t> many(Noted<Item>::kMost + 20, b);
    CHECK(frame_of(n, 16, many).size() == Noted<Item>::kMost);
    CHECK(frame_of(n, 17, {b}).size() == 1);
    // (the n-th held at 255: the 300th draw of a model is not a key of another model)
    Noted<Item> m;
    const std::vector<uint64_t> k300 = frame_of(m, 1, std::vector<uint64_t>(300, a));
    CHECK(k300.size() == 300 && same_model(k300[299], a) && (k300[299] & 0xff) == 255);
}

static void poses() {
    Poses p;
    CHECK(p.here() == 0 && p.there() == 1);
    // frame 1: two draws placed in buffer 0, 16-aligned, origin then bones
    p.begin();
    uint32_t at1 = 99, at2 = 99;
    CHECK(p.place(0xa00, 3, 1, &at1) && p.place(0xb00, 2, 1, &at2));
    CHECK(at1 == 0 && at2 == ((16 + 3 * 48 + 15) & ~15u));
    CHECK(!p.before_of(0xa00, 3, 1, true));   // (nothing before frame 1)
    const int buffer1 = p.here();
    p.end();
    // frame 2: its poses in the other buffer, the frame before's read from frame 1's
    p.begin();
    CHECK(p.here() != buffer1 && p.there() == buffer1);
    uint32_t mine = 99;
    CHECK(p.place(0xa00, 3, 2, &mine) && mine == 0);
    const Pose* before = p.before_of(0xa00, 3, 2, true);
    CHECK(before && before->at == at1 && before->frame == 1);
    const Pose* b2 = p.before_of(0xb00, 2, 2, true);
    CHECK(b2 && b2->at == at2);
    CHECK(!p.before_of(0xa00, 3, 2, false));   // (DLSS did not make the frame before: no history)
    CHECK(!p.before_of(0xa00, 4, 2, true));    // (as many bones, or not the same draw)
    CHECK(!p.before_of(0xc00, 3, 2, true));    // (not in the frame before)
    p.end();
    // frame 4 after a frame with no pass (3): frame 2's poses are not the frame before's; this frame's start empty (no map growing)
    p.begin();
    CHECK(p.now.empty() && p.before.size() == 1 && p.used == 0);
    CHECK(!p.before_of(0xa00, 3, 4, true));
    p.end();
    CHECK(p.here() == (buffer1 ^ 1));   // (three ends: frame 1's buffer, frame 2's, frame 1's, and now frame 2's again)
    // the buffer full: no place past kPoseBytes
    Poses q;
    q.begin();
    uint32_t at = 0;
    int placed = 0;
    while (q.place(0x100 + placed, 280, 1, &at)) placed++;
    CHECK(placed == (int)(kPoseBytes / ((16 + 280 * 48 + 15) & ~15u)) && at + 16 + 280 * 48 <= kPoseBytes);
}

static void pushes() {
    Layout l;
    l.vsize = 28 * 10 + 5, l.stride = 28, l.pos = 4, l.sets = 2, l.weights[0] = 16, l.weights[1] = 20, l.joints[0] = 8, l.joints[1] = 24;
    l.formats = kByteJoints << 1, l.itype = 1, l.count = 36, l.base_vertex = 7;
    CHECK(fits(l));
    Push a;
    const uint64_t va = 0x10000, ia = 0x20006, here = 0x50000, there = 0x90000;
    CHECK(make_push(l, va, ia, here, 0x40, there, 0x80, 12, 64, &a));
    CHECK(a.vertices == va && a.count == 10);   // (the stream's vertices, whole ones)
    CHECK(a.indices == 0x20004 && a.first == 1);   // (16-bit: the address 4-aligned down, one index in)
    CHECK(a.origins[0] == here + 0x40 && a.origins[1] == there + 0x80);   // (this frame's pose here, the frame before's there)
    CHECK(a.bones[0] == here + 0x40 + 16 && a.bones[1] == there + 0x80 + 16);
    CHECK(a.nbones == 12 && a.stride == 7 && a.itype == 1 && a.base_vertex == 7 && a.pos == 4 && a.sets == 2);
    CHECK(a.joints[0] == 8 && a.joints[1] == 24 && a.weights[0] == 16 && a.weights[1] == 20 && a.formats == (kByteJoints << 1));
    CHECK(a.tolerance == 64);
    // what cannot be drawn
    CHECK(!make_push(l, 0, ia, here, 0, there, 0, 12, 64, &a) && !make_push(l, va, 0, here, 0, there, 0, 12, 64, &a));
    CHECK(!make_push(l, va + 2, ia, here, 0, there, 0, 12, 64, &a));   // (vertices not on a word)
    CHECK(!make_push(l, va, ia, here, 0, there, 0, 0, 64, &a));         // (no bones)
    Layout bad = l;
    bad.itype = 3;
    CHECK(!make_push(bad, va, ia, here, 0, there, 0, 12, 64, &a));
    bad = l, bad.count = 0;
    CHECK(!make_push(bad, va, ia, here, 0, there, 0, 12, 64, &a));
    // every attribute inside a vertex
    bad = l, bad.joints[0] = 24;   // (16-bit joints, 8 bytes, at 24 of 28: past the vertex)
    CHECK(!fits(bad));
    bad = l, bad.joints[1] = 24;   // (set 1's joints are bytes: 24 + 4 fits)
    CHECK(fits(bad));
    bad = l, bad.pos = 20;         // (RGB32F at 20 of 28: 32 > 28)
    CHECK(!fits(bad));
    bad = l, bad.formats |= kHalfPosition, bad.pos = 20;   // (RGBA16F: 8 bytes, fits)
    CHECK(fits(bad));
    bad = l, bad.weights[1] = 26;
    CHECK(!fits(bad));
    bad = l, bad.stride = 30;
    CHECK(!fits(bad));
    bad = l, bad.sets = 0;
    CHECK(!fits(bad));
}

static void heads() {
    float cur[16], prev[16], out[kHeadWords];
    for (int i = 0; i < 16; i++) cur[i] = (float)i, prev[i] = 100.0f + i;
    head(cur, prev, 0.25f, -0.4f, 1920, 1080, out);
    CHECK(!std::memcmp(out, cur, 64) && !std::memcmp(out + 16, prev, 64));
    // (a viewport offset of ux, uy pixels, window y down, is 2ux/w, -2uy/h in the game's clip space, y up)
    CHECK(out[32] == 2.0f * 0.25f / 1920 && out[33] == -2.0f * -0.4f / 1080 && out[34] == 1920 && out[35] == 1080);
}

static void capture_heads() {
    float cur[16], prev[16], c2p[16], out[kCaptureHeadWords], base[kHeadWords];
    for (int i = 0; i < 16; i++) cur[i] = (float)i, prev[i] = 50.0f + i, c2p[i] = -1.0f - i;
    capture_head(cur, prev, 0.3f, -0.2f, 1920, 1080, c2p, 24.0f, out);
    head(cur, prev, 0.3f, -0.2f, 1920, 1080, base);
    CHECK(!std::memcmp(out, base, sizeof base));   // (dlss_actor.frag's 36 words, as the actor pass's)
    CHECK(!std::memcmp(out + 36, c2p, 64) && out[52] == 24.0f);
}

static void tolerances() {
    CHECK(tolerance_of(nullptr) == 64 && tolerance_of("") == 64 && tolerance_of("16") == 16 && tolerance_of("0") == 0);
    CHECK(tolerance_of("-5") == 64 && tolerance_of("abc") == 64 && tolerance_of("12x") == 64 && tolerance_of("70000") == 64);
}

static void captures() {
    CaptureRing r;
    r.capacity = 100;
    const uint64_t a = key(0x1000, 0x2000, 30, 0, 9, 0), b = key(0x3000, 0x4000, 12, 0, 9, 0);
    uint32_t at = 0;
    // frame 1: a twice, b once, in the first half they are given (frame 1 turns the halves from the start: half 1)
    const uint64_t a0 = r.key_of(a, 1), b0 = r.key_of(b, 1), a1 = r.key_of(a, 1), a2 = r.key_of(a, 1);
    CHECK(a0 != a1 && a1 != a2 && a0 != a2 && a0 != b0);   // (each draw its own key)
    CHECK(r.place(a0, 30, 1, &at, 0xd00) && at == 100 && r.now.back().target == 0xd00);   // (the target kept with it)
    CHECK(r.place(b0, 12, 1, &at) && at == 130 && r.now.back().target == 0);
    CHECK(r.place(a1, 30, 1, &at) && at == 142);
    CHECK(!r.place(a2, 30, 1, &at));       // (172 + 30 > 200: no room)
    CHECK(!r.before_of(a0, 30, 1));         // (nothing before frame 1)
    CHECK(r.most == 3);                     // (a: three draws in frame 1)
    // frame 2: the other half; the same draws the same keys (of the model and its n-th alone); the frame before's by key, count
    CHECK(r.key_of(a, 2) == a0 && !r.place(a0, 30, 1, &at) && r.place(a0, 30, 2, &at) && at == 0);   // (a key of frame 2 only)
    const CaptureRing::Entry* was = r.before_of(a0, 30, 2);
    CHECK(was && was->at == 100);
    CHECK(r.key_of(b, 2) == b0 && r.place(b0, 12, 2, &at) && at == 30);
    CHECK(r.before_of(b0, 12, 2) && r.before_of(b0, 12, 2)->at == 130);
    CHECK(!r.before_of(b0, 13, 2));         // (another count: another draw)
    CHECK(r.key_of(a, 2) == a1 && r.place(a1, 30, 2, &at) && r.before_of(a1, 30, 2) && r.before_of(a1, 30, 2)->at == 142);
    CHECK(!r.place(r.key_of(a, 2), 0, 2, &at));   // (no vertices)
    // frame 4 after a frame without captures (3): frame 2's are not the frame before's
    CHECK(r.key_of(a, 4) == a0 && r.place(a0, 30, 4, &at) && !r.before_of(a0, 30, 4));
    // frame 5: frame 4's are; the halves still take turns (frame 4 wrote half 1, frame 5 writes half 0)
    CHECK(r.key_of(a, 5) == a0 && r.place(a0, 30, 5, &at) && at == 0 && r.before_of(a0, 30, 5) && r.before_of(a0, 30, 5)->at == 100);
    // a draw bigger than a half
    CHECK(!r.place(r.key_of(b, 5), 101, 5, &at));

    // a draw left out for a frame after its key (its pipeline not made yet): the next draws of its model keep their n-ths
    CaptureRing s;
    s.capacity = 100;
    const uint64_t c0 = s.key_of(a, 7), c1 = s.key_of(a, 7);   // (frame 7: the first draw of a left out, the second recorded)
    CHECK(s.place(c1, 30, 7, &at) && at == 100);
    CHECK(s.key_of(a, 8) == c0 && s.place(c0, 30, 8, &at) && !s.before_of(c0, 30, 8));
    CHECK(s.key_of(a, 8) == c1 && s.place(c1, 30, 8, &at) && s.before_of(c1, 30, 8) && s.before_of(c1, 30, 8)->at == 100);

    // a model drawn 1000 times a frame (a grass tuft): every draw its own key, and its own frame before
    CaptureRing g;
    g.capacity = 3000;
    std::vector<uint32_t> first(1000);
    std::set<uint64_t> keys;
    for (int i = 0; i < 1000; i++) {
        const uint64_t k = g.key_of(a, 20);
        keys.insert(k);
        CHECK(g.place(k, 3, 20, &first[i]));
    }
    CHECK(keys.size() == 1000 && g.most == 1000);
    int own = 0;
    for (int i = 0; i < 1000; i++) {
        const uint64_t k = g.key_of(a, 21);
        own += g.place(k, 3, 21, &at) && g.before_of(k, 3, 21) && g.before_of(k, 3, 21)->at == first[i];
    }
    CHECK(own == 1000);
}

int main() {
    captures();
    noted();
    poses();
    pushes();
    heads();
    capture_heads();
    tolerances();
    if (failures) return std::printf("dlss_actor_plan: %d failures\n", failures), 1;
    std::printf("dlss_actor_plan: ok\n");
    return 0;
}
