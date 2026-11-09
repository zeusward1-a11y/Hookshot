// DLSS's jitter hold (gpu/vk/dlss_jitter.h) frame by frame, as vk_draw.cpp drives it: frame_start at a frame's first draw, used
// when a draw takes the jitter, made when DLSS made the frame. No GPU, no game. tests/test_dlss_jitter.py builds it.
#include <cstdio>

#include "dlss_jitter.h"

using namespace mkwvk;

static int failures = 0;
#define CHECK(x)                                                                    \
    do {                                                                            \
        if (!(x)) std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x), ++failures; \
    } while (0)

/// One frame: its start, its draws (jittered unless held, when want), DLSS making it or not. True when it was jittered
static bool frame(jitter::Hold& h, uint64_t now, bool want, bool made) {
    h.frame_start(now);
    const bool jittered = want && !h.held();
    if (jittered) h.used(now);
    if (made) h.made(now);
    return jittered;
}

static void normal_play() {
    jitter::Hold h;
    int n = 0;
    for (uint64_t f = 1; f <= 1000; f++) n += frame(h, f, true, true);
    CHECK(h.holds == 0 && n == 1000);
}

/// A menu or a load: frames DLSS makes without jitter (or does not make at all), then play again. No hold: the review of 09/11
/// found the first version stopped the jitter after every such stretch
static void menu_and_load() {
    jitter::Hold h;
    for (uint64_t f = 1; f <= 10; f++) frame(h, f, true, true);
    for (uint64_t f = 11; f <= 20; f++) frame(h, f, false, f % 2 == 0);   // (unjittered: made or not, nothing pending)
    int n = 0;
    for (uint64_t f = 21; f <= 40; f++) n += frame(h, f, true, true);
    std::printf("menu and load: %llu holds, %d of 20 frames jittered after\n", (unsigned long long)h.holds, n);
    CHECK(h.holds == 0 && n == 20);
}

/// The scene's size changes: one frame unjittered (its size is new), made; no hold
static void size_change() {
    jitter::Hold h;
    for (uint64_t f = 1; f <= 5; f++) frame(h, f, true, true);
    frame(h, 6, false, true);
    int n = 0;
    for (uint64_t f = 7; f <= 12; f++) n += frame(h, f, true, true);
    CHECK(h.holds == 0 && n == 6);
}

/// A jittered frame DLSS did not make: 8 frames without jitter from the next one, then jitter again
static void one_miss() {
    jitter::Hold h;
    for (uint64_t f = 1; f <= 9; f++) frame(h, f, true, true);
    CHECK(frame(h, 10, true, false));   // (jittered, not made)
    CHECK(h.frame_start(11) == 10 && h.holds == 1 && h.left == jitter::kHoldOnce);
    int held = 0;
    for (uint64_t f = 11; f <= 30; f++) held += !frame(h, f, true, true);
    std::printf("one miss: %d frames held\n", held);
    CHECK(held == jitter::kHoldOnce && h.holds == 1);
}

/// A second miss within two seconds of the first: a second's hold; one long after: the short one again
static void misses_close_and_far() {
    jitter::Hold h;
    frame(h, 1, true, false);   // (miss at 1)
    for (uint64_t f = 2; f <= 50; f++) frame(h, f, true, true);
    frame(h, 51, true, false);   // (miss at 51: within 120 of 1)
    h.frame_start(52);
    CHECK(h.left == jitter::kHoldAgain && h.holds == 2);
    for (uint64_t f = 53; f <= 400; f++) frame(h, f, true, true);
    frame(h, 401, true, false);   // (miss at 401: 350 after 51)
    h.frame_start(402);
    CHECK(h.left == jitter::kHoldOnce && h.holds == 3);
}

/// Missed frames DLSS keeps missing every other frame: jittered at most once in kHoldAgain + 1 after the first two
static void keeps_missing() {
    jitter::Hold h;
    int shaken = 0;
    for (uint64_t f = 1; f <= 2000; f++) shaken += frame(h, f, true, f % 2 == 0) && f % 2 == 1;   // (odd frames: not made; shaken =
                                                                                              // jittered and not made)
    std::printf("keeps missing: %d of 1000 odd frames shaken\n", shaken);
    CHECK(shaken <= 2 + 2000 / (jitter::kHoldAgain + 1));
}

int main() {
    normal_play();
    menu_and_load();
    size_change();
    one_miss();
    misses_close_and_far();
    keeps_missing();
    std::printf("dlss jitter hold: %d failures\n", failures);
    return failures ? 1 : 0;
}
