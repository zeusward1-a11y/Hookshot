/* (hookshot) HOS_FPS_CAP=<fps>: the frame rate held at most that, on the console's display queue as nn_vi.c models it (each
   present is shown one period after the one before, and a frame's acquire waits for its texture to be shown free), so the
   game's frames come at a steady rate. For frame generation: the engine's adaptive controller let the base rate swing between
   42 and 60 under load, and frame generation doubles the swing on screen (09/11: base 46-61, 92-122 shown); a cap the game
   holds most of the time (50: 20 ms frames, 100 shown in 2x) keeps it steady (measured: 5-second windows' std 4.4 -> 1.3 fps).
   Writing the rate into the game's own limiter (+0xfc) does nothing: the controller writes it back before the limiter reads it
   (tested 09/11). RX_DLSSG_FPS (dlssg_logic.h) is the cap of Hookshot's own DLSS-G, through Streamline's Reflex limiter; with
   the frame generation of another runtime (OptiScaler's, over the D3D12 bridge) only this one holds.
   20..60: the queue models a 60 Hz display, so a cap above 60 would change nothing. With game time following the clock (the
   Insert menu's Real time, the default) the game keeps its speed; with Game speed Per frame (or HOS_GAME_INTERVAL=1) a frame
   is a 60th of a second of game time, and a cap slows the game to cap / 60. HOS_NO_PACING=1: no pacing, and so no cap.
   Kept apart so tests/test_fps_cap.py checks it without the game. */
#ifndef HOS_FPS_H
#define HOS_FPS_H

#include <stdint.h>
#include <stdlib.h>

/* The cap asked for: 20..60 fps, else 0 (none) */
static inline int hos_fps_cap_of(const char *env) {
    if (!env || !*env) return 0;
    char *end = NULL;
    const long v = strtol(env, &end, 10);
    return end == env || *end || v < 20 || v > 60 ? 0 : (int)v;
}

/* The display queue's period: the game's interval of 60 Hz vsyncs, or the cap's frame time when that is longer */
static inline int64_t hos_fps_period_ns(int interval, int cap) {
    const int64_t vsyncs = 1000000000LL / 60 * (interval < 1 ? 1 : interval);
    const int64_t capped = cap > 0 ? 1000000000LL / cap : 0;
    return capped > vsyncs ? capped : vsyncs;
}

#endif
