// NVIDIA DLSS Frame Generation's decisions (vk_dlss.cpp, vk_menu.cpp, vk_main.cpp), kept apart from Streamline and Vulkan so that
// tests/test_dlssg.py checks them with no GPU, no game and no NVIDIA DLL: how frames are numbered, the menu's multiplier and the
// environment's, when generation is turned off and why, the device's queues for Streamline's plugins, the camera Streamline wants
// from the game's view-projection, and the log line every 600 presents.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mkwvk::dlssg {

// ---- the multiplier: the Insert menu's row and RX_DLSSG ----

/// RX_DLSSG: 0 off, 2, 3 or 4 times the game's frames (1 is 2x, more than 4 is 4x). Unset, empty or not a whole number: the menu's
inline int parse_multiplier(const char* v, int menu) {
    if (!v || !*v) return menu;
    char* end = nullptr;
    const long n = strtol(v, &end, 10);
    if (end == v || *end) return menu;
    return n <= 0 ? 0 : n == 1 ? 2 : n > 4 ? 4 : (int)n;
}

/// RX_REFLEX: 0 off, 1 low latency, 2 low latency with boost (above 2: 2, below 0: 0). Unset or not a number: fallback
inline int parse_reflex(const char* v, int fallback) {
    if (!v || !*v) return fallback;
    char* end = nullptr;
    const long n = strtol(v, &end, 10);
    if (end == v || *end) return fallback;
    return n < 0 ? 0 : n > 2 ? 2 : (int)n;
}

/// Reflex's mode for what was asked: DLSS-G needs Reflex on while it generates (its status is eFailReflexNotDetectedAtRuntime
/// otherwise), so an asked 0 is 1 then
inline int reflex_mode(int asked, bool generating) {
    return generating && asked == 0 ? 1 : asked < 0 ? 0 : asked > 2 ? 2 : asked;
}

/// The menu row goes Off, 2x, 3x, 4x and round (1 is skipped, as the Frame generation row skips it): the value after v, to the right
/// or the left. A value that is none of those starts from Off
inline int menu_step(int v, bool right) {
    static const int order[4] = {0, 2, 3, 4};
    int i = 0;
    while (i < 4 && order[i] != v) i++;
    if (i == 4) i = 0;
    return order[right ? (i + 1) % 4 : (i + 3) % 4];
}

/// A value read back from the options file: 1 (never a row value) is 2x, anything past the row's ends is clamped
inline int normalize_multiplier(int v) { return v <= 0 ? 0 : v == 1 ? 2 : v > 4 ? 4 : v; }

/// Streamline's numFramesToGenerate for a multiplier (2x: 1), within what the GPU can (DLSSGState::numFramesToGenerateMax; 0, not
/// known yet: 1). 0: the multiplier is off
inline uint32_t frames_to_generate(int multiplier, uint32_t max_generate) {
    if (multiplier < 2) return 0;
    const uint32_t want = (uint32_t)(multiplier - 1), cap = max_generate ? max_generate : 1;
    return want > cap ? cap : want;
}

// ---- frames ----

/// Frames are numbered by the game's presents: the frame being made has the number of the present that ends it, one past the presents
/// so far. The first is 1 (0 is never a frame); Streamline's index is 32 bits, and a count past it starts again from 1.
inline uint32_t frame_of(uint64_t presents_done) {
    const uint32_t f = (uint32_t)(presents_done + 1);
    return f ? f : 1;
}

/// Reflex's sleep and the frame's first markers, once a frame: the game may acquire a window texture again before it presents
struct FrameStart {
    uint32_t started = 0;
    bool begin(uint32_t frame) {
        if (frame == started) return false;
        started = frame;
        return true;
    }
};

/// Reflex's sleep holds the frame's start only while presents go out: with none for idle_ns (a minimized window) the driver has no
/// present to time the frame by, and each sleep would wait out Streamline's timeout
inline bool hold(int64_t now_ns, int64_t last_present_ns, int64_t idle_ns) {
    return last_present_ns > 0 && now_ns >= last_present_ns && now_ns - last_present_ns < idle_ns;
}

/// Generation's history is reset when DLSS's is (a camera cut, a new size) and when the frame before was not one generation had: a
/// frame after a gap has nothing to be interpolated from
inline bool reset_history(bool dlss_reset, uint32_t frame, uint32_t last_tagged) {
    return dlss_reset || !last_tagged || frame != last_tagged + 1;
}

// ---- on or off, each present ----

enum Off : int {
    ON = 0,
    OFF_UNAVAILABLE,   // (not started, not supported, or its functions missing)
    OFF_USER,          // (the row is Off)
    OFF_WINDOW,        // (the window is minimized)
    OFF_SWAPCHAIN,     // (a swapchain is made again, or was just now)
    OFF_TOO_SMALL,     // (the window's image under DLSS-G's minWidthOrHeight, or eFailResolutionTooLow at this size)
    OFF_STATUS,        // (DLSS-G said its status was not ok: off for a while, then tried again)
    OFF_MENU,          // (the Insert menu is open: it is drawn into the window's image)
    OFF_NO_IMAGE,      // (the game presented nothing: a black frame)
    OFF_NO_DLSS,       // (DLSS did not draw this frame: the game's own upscale, a movie, a loading screen, DLSS turned off)
    OFF_COUNT
};
inline const char* off_name(int r) {
    static const char* const names[OFF_COUNT] = {"on", "unavailable", "user", "minimized", "swapchain", "too small", "status", "menu",
                                                 "no image", "no DLSS frame"};
    return r >= 0 && r < OFF_COUNT ? names[r] : "?";
}

struct PresentState {
    bool available = false;   // (DLSS-G started, supported, its functions found)
    int multiplier = 0;       // (the row: 0 off, 2 to 4)
    bool minimized = false;
    int swapchain_hold = 0;   // (presents left before generation after a swapchain change)
    bool too_small = false;   // (SizeGate: the window's image is too small for DLSS-G)
    int status_hold = 0;      // (presents left after a status that was not ok)
    bool menu_open = false;
    bool image = false;       // (the game handed a picture)
    bool tagged = false;      // (this frame's depth and motion were tagged: DLSS drew it)
};
/// Why generation is off for a present (in this order: the first that holds), or ON
inline int decide(const PresentState& p) {
    if (!p.available) return OFF_UNAVAILABLE;
    if (p.multiplier < 2) return OFF_USER;
    if (p.minimized) return OFF_WINDOW;
    if (p.swapchain_hold > 0) return OFF_SWAPCHAIN;
    if (p.too_small) return OFF_TOO_SMALL;
    if (p.status_hold > 0) return OFF_STATUS;
    if (p.menu_open) return OFF_MENU;
    if (!p.image) return OFF_NO_IMAGE;
    if (!p.tagged) return OFF_NO_DLSS;
    return ON;
}

/// slDLSSGSetOptions' mode for a decision. Off keeps DLSS-G's memory (eRetainResourcesWhenOff: without it every off and on again
/// cost a 150-260 ms hitch in the OptiScaler work), except when the player turned it off or it cannot run: then it is freed.
/// numFramesToGenerate stays the multiplier's while off, so that turning on again changes nothing else.
struct Mode {
    bool on = false;
    uint32_t frames = 1;
    bool retain = false;
};
inline Mode mode_for(int off, int multiplier, uint32_t max_generate) {
    Mode m;
    m.on = off == ON;
    const uint32_t n = frames_to_generate(multiplier, max_generate);
    m.frames = n ? n : 1;
    m.retain = off != OFF_USER && off != OFF_UNAVAILABLE;
    return m;
}
inline bool same_mode(const Mode& a, const Mode& b) { return a.on == b.on && a.frames == b.frames && a.retain == b.retain; }

/// The player's Off (or the built-in interpolation taking over): an eOff without eRetainResourcesWhenOff must go out, so that
/// DLSS-G's ~500 MB are freed (the guide's section 18), also when the last options sent were "off, kept" (the menu was open)
inline bool frees(bool sent, const Mode& last) { return sent && (last.on || last.retain); }

/// Off at once, before anything else of the present (an eOff that keeps the resources): a swapchain about to be made again (every
/// create_swapchain, the out-of-date acquire and present included: the guide's section 17, DLSS-G off before any window change) or a
/// minimized window (also when the acquire then fails and no present follows). ON: nothing to do (generation was not on)
inline int early_off(bool generating, bool minimized, bool swapchain_change) {
    if (!generating) return ON;
    if (swapchain_change) return OFF_SWAPCHAIN;
    if (minimized) return OFF_WINDOW;
    return ON;
}

/// A swapchain made again (a resize, fullscreen): DLSS-G must be off for it (the guide: off before any window change, or the
/// present can deadlock). With generation on, the change waits for one present that carries the off; then it is made, and generation
/// waits `settle` presents more
struct SwapchainGate {
    int hold = 0;
    /// A change is waiting: true while it must wait for this present (generating: the last options sent were on)
    bool defer(bool generating, int settle) {
        if (hold < settle) hold = settle;
        return generating;
    }
    void changed(int settle) {
        if (hold < settle) hold = settle;
    }
    void presented() {
        if (hold > 0) hold--;
    }
};

/// DLSS-G's status after a present: one that is not ok (eFailResolutionTooLow at a small window, eFailReflexNotDetectedAtRuntime...)
/// turns generation off for `retry` presents, then it is tried again
struct StatusGate {
    int hold = 0;
    uint32_t last = 0;
    /// After a present: generating, what DLSS-G said. True when the status is new (for the log)
    bool update(bool generating, uint32_t status, int retry) {
        if (hold > 0) hold--;
        if (!generating) return false;
        const bool changed = status != last;
        last = status;
        if (status & ~kResolutionTooLow) hold = retry;   // (too small: SizeGate keeps it off until the size changes, no retries)
        return changed;
    }
    static constexpr uint32_t kResolutionTooLow = 1u << 0;   // (sl::DLSSGStatus::eFailResolutionTooLow)
    /// The log's words for a new status: too small alone is the size gate's (no retry), anything else the 300-present hold's
    static const char* text(uint32_t status) {
        if (!status) return ": ok";
        if (!(status & ~kResolutionTooLow)) return ": the window is too small for DLSS-G: generation off until its size changes";
        return ": generation off for 300 presents, then tried again (the reason is in Streamline's lines above)";
    }
};

/// The window's image too small for DLSS-G: under DLSSGState::minWidthOrHeight (the guide: DLSS-G must be off then), or a size at
/// which DLSS-G said eFailResolutionTooLow. Off until the size changes, said once a size, never retried at the same size
struct SizeGate {
    uint32_t min = 0, failed_w = 0, failed_h = 0, told_w = 0, told_h = 0;
    bool too_small(uint32_t w, uint32_t h) const {
        return (min && (w < min || h < min)) || (failed_w && failed_w == w && failed_h == h);
    }
    void failed(uint32_t w, uint32_t h) { failed_w = w, failed_h = h; }
    /// True once for each size found too small (for the log)
    bool tell(uint32_t w, uint32_t h) {
        if (!too_small(w, h) || (told_w == w && told_h == h)) return false;
        told_w = w, told_h = h;
        return true;
    }
};

// ---- the present mode and the frame limit ----

enum PresentMode : int { PRESENT_FIFO = 0, PRESENT_MAILBOX, PRESENT_IMMEDIATE };
/// The swapchain's mode, as vk_main.cpp create_swapchain chose it (mailbox when the driver has it; on Windows, with no mailbox or
/// RX_VK_IMMEDIATE, immediate; RX_VK_FIFO: FIFO), except that RX_VK_FIFO is not taken while DLSS-G runs: VSync with frame generation
/// is D3D12's only (the DLSS-G guide, 22.5), so mailbox, else immediate (G-SYNC through the driver, held under the refresh by
/// Reflex's frame limit: frame_limit_us). fifo_ignored: RX_VK_FIFO was asked for and not taken
inline int pick_present_mode(bool fifo_asked, bool immediate_asked, bool dlssg, bool has_mailbox, bool has_immediate, bool windows,
                             bool* fifo_ignored = nullptr) {
    if (fifo_ignored) *fifo_ignored = false;
    if (fifo_asked && !dlssg) return PRESENT_FIFO;
    if (fifo_asked) {
        if (fifo_ignored) *fifo_ignored = has_mailbox || has_immediate;
        return has_mailbox ? PRESENT_MAILBOX : has_immediate ? PRESENT_IMMEDIATE : PRESENT_FIFO;
    }
    int mode = has_mailbox ? PRESENT_MAILBOX : PRESENT_FIFO;
    if (has_immediate && (immediate_asked || (windows && mode == PRESENT_FIFO))) mode = PRESENT_IMMEDIATE;
    return mode;
}

/// Reflex's frameLimitUs that holds the game's frames times the multiplier at or under fraction of the monitor's refresh (rounded up,
/// so the frames shown never pass it): 164 Hz at 2x, 0.97: 12573 us, 79.5 game frames, 159.1 shown. 0 (no limit): refresh unknown
inline uint32_t frame_limit_us(uint32_t refresh_hz, int multiplier, double fraction) {
    if (refresh_hz < 2 || fraction <= 0.0) return 0;
    const int m = multiplier < 1 ? 1 : multiplier;
    return (uint32_t)std::ceil(1e6 * (double)m / (fraction * (double)refresh_hz));
}

/// What Reflex's frame limit counts (RX_DLSSG_LIMIT): Streamline hands frameLimitUs to VK_NV_low_latency2's minimumIntervalUs, which
/// may count the game's presents or every present DLSS-G makes. game (the default): the game's frames, the limit times the frames
/// shown per game frame; present: every present, 97% of the refresh whatever the multiplier; 0: no limit
enum LimitBasis : int { LIMIT_NONE = 0, LIMIT_GAME, LIMIT_PRESENT };
inline int parse_limit(const char* v) {
    if (!v || !*v) return LIMIT_GAME;
    if (!strcmp(v, "present")) return LIMIT_PRESENT;
    if (!strcmp(v, "0") || !strcmp(v, "none") || !strcmp(v, "off")) return LIMIT_NONE;
    return LIMIT_GAME;
}
inline const char* limit_name(int basis) {
    return basis == LIMIT_PRESENT ? "present" : basis == LIMIT_NONE ? "0" : "game";
}
/// Reflex's frameLimitUs for a present, from what DLSS-G actually does then (the options last sent), not the row: generating with
/// `generated` frames per game frame, the game's frames times (generated + 1) at 97% of the refresh; not generating (the menu, a frame
/// DLSS did not draw, a hold, DLSS off), no limit on the game basis (a 4x row in a menu held a 60 Hz game at 14.5 fps). 0 too while
/// RX_VK_FIFO was not set aside (fifo_cap) or DLSS-G cannot run (usable: available, the row at 2x and up)
inline uint32_t reflex_limit_us(int basis, bool fifo_cap, bool usable, bool generating, uint32_t generated, uint32_t refresh_hz) {
    if (!fifo_cap || !usable || basis == LIMIT_NONE) return 0;
    if (basis == LIMIT_PRESENT) return frame_limit_us(refresh_hz, 1, 0.97);
    const int shown = generating ? (int)generated + 1 : 1;
    return shown > 1 ? frame_limit_us(refresh_hz, shown, 0.97) : 0;
}

/// The frame limit's cap, swapchain by swapchain: on while RX_VK_FIFO is set aside for DLSS-G, off again as soon as a swapchain takes
/// FIFO (DLSS failed or DLSS-G went off) or DLSS-G cannot run. +1: turned on, -1: turned off, 0: as it was
struct FifoCap {
    bool on = false;
    int update(bool fifo_ignored, bool usable) {
        const bool want = fifo_ignored && usable;
        if (want == on) return 0;
        on = want;
        return want ? 1 : -1;
    }
};

/// The player's Off freed DLSS-G's memory (eOff without eRetainResourcesWhenOff); slFreeResources follows, as the guide's 6.4 asks after
/// the flag was used, `after` presents later (the present that carried the off has gone through DLSS-G's own presenting by then);
/// cancelled when generation came back on meanwhile
struct FreeLater {
    int left = 0;
    void ask(int after) { left = after; }
    /// After a present: true when it is time to free
    bool presented(bool on) {
        if (on) return left = 0, false;
        return left > 0 && --left == 0;
    }
};

// ---- the log ----

struct Stats {
    uint64_t presents = 0, shown = 0, on = 0;
    uint32_t off[OFF_COUNT] = {};
    uint32_t status = 0;   // (every status seen, or'd)
    /// One present: why it was off (or ON), the frames DLSS-G said it showed for it (numFramesActuallyPresented; 0 is taken as 1, the
    /// game's own frame is always shown), its status
    void add(int reason, uint32_t frames_shown, uint32_t st) {
        presents++;
        shown += frames_shown ? frames_shown : 1;
        if (reason == ON) on++;
        if (reason >= 0 && reason < OFF_COUNT) off[reason]++;
        status |= st;
    }
    void clear() { *this = Stats{}; }
    /// The log line, e.g. "vk: dlssg: 600 presents: 2.00 frames shown per game frame (1200 shown), generating 600 at 2x, status ok;
    /// off: user 0, menu 0, no DLSS frame 0, no image 0, swapchain 0, minimized 0, too small 0, status 0, unavailable 0"
    int line(char* out, size_t n, int multiplier) const {
        char st[24];
        if (status) snprintf(st, sizeof st, "%#x", (unsigned)status);
        else snprintf(st, sizeof st, "ok");
        return snprintf(out, n,
                        "vk: dlssg: %llu presents: %.2f frames shown per game frame (%llu shown), generating %llu at %dx, status %s; "
                        "off: user %u, menu %u, no DLSS frame %u, no image %u, swapchain %u, minimized %u, too small %u, status %u, "
                        "unavailable %u",
                        (unsigned long long)presents, presents ? (double)shown / (double)presents : 0.0, (unsigned long long)shown,
                        (unsigned long long)on, multiplier, st, off[OFF_USER], off[OFF_MENU], off[OFF_NO_DLSS], off[OFF_NO_IMAGE],
                        off[OFF_SWAPCHAIN], off[OFF_WINDOW], off[OFF_TOO_SMALL], off[OFF_STATUS], off[OFF_UNAVAILABLE]);
    }
};

/// Streamline's warnings and errors in the game's log: the first `first`, then one in `every` (a warning a present filled the log)
struct LogLimit {
    uint64_t seen = 0;
    bool allow(uint32_t first, uint32_t every) {
        const uint64_t k = seen++;
        return k < first || (every && (k - first) % every == every - 1);
    }
};

// ---- the device's queues for Streamline's plugins (slGetFeatureRequirements) ----

constexpr uint32_t kQueueGraphics = 0x1, kQueueCompute = 0x2, kQueueOpticalFlow = 0x100;   // (VK_QUEUE_*_BIT, ..._OPTICAL_FLOW_BIT_NV)
struct Family {
    uint32_t flags, count;
};
struct QueuePlan {
    struct Ask {
        uint32_t family, count;
    };
    Ask asks[8]{};
    uint32_t n = 0;   // (the device's queue create infos: the host's family first, then the others in family order)
    uint32_t graphics_family = 0, graphics_index = 0, compute_family = 0, compute_index = 0, of_family = 0;
    bool native_of = false;   // (an optical flow queue: VK_NV_optical_flow, first queue of the first family that has it)
    bool ok = true;           // (false: a family had too few queues for what was asked)
};
/// As Streamline's own vkCreateDevice proxy does it (sl.interposer/vulkan/wrapper.cpp): its graphics queues in the last family with
/// graphics, after the host's queue when that is the family; its compute queues in the last compute family without graphics (the
/// graphics family when there is none); its optical flow queue, when the device can (VK_NV_optical_flow and its feature), as queue 0 of
/// the first optical flow family, which must not be the host's. With nothing asked a kind stays on the host's family at index 0, as
/// vk_dlss.cpp told Streamline before. host: the host's family, one queue
inline QueuePlan plan_queues(const Family* fams, uint32_t nfam, uint32_t host, uint32_t extra_graphics, uint32_t extra_compute,
                             uint32_t extra_of, bool of_supported) {
    QueuePlan p;
    uint32_t count[64] = {};
    if (nfam > 64) nfam = 64;
    if (host >= nfam) return p.ok = false, p;
    count[host] = 1;
    uint32_t of = nfam;
    if (extra_of && of_supported)
        for (uint32_t i = 0; i < nfam && of == nfam; i++)
            if (fams[i].flags & kQueueOpticalFlow) of = i;
    if (of == host) of = nfam;
    uint32_t gf = nfam, cf = nfam;
    for (uint32_t i = 0; i < nfam; i++) {
        if (i == of) continue;
        if (fams[i].flags & kQueueGraphics) gf = i;
        else if (fams[i].flags & kQueueCompute) cf = i;
    }
    if (gf == nfam) gf = host;
    if (cf == nfam) cf = gf;
    p.graphics_family = p.compute_family = host;
    if (extra_compute) {
        if (count[cf] + extra_compute <= fams[cf].count) {
            p.compute_family = cf, p.compute_index = count[cf];
            count[cf] += extra_compute;
        } else {
            p.ok = false;
        }
    }
    if (extra_graphics) {
        if (count[gf] + extra_graphics <= fams[gf].count) {
            p.graphics_family = gf, p.graphics_index = count[gf];
            count[gf] += extra_graphics;
        } else {
            p.ok = false;
        }
    }
    if (of < nfam && extra_of <= fams[of].count) {
        p.native_of = true, p.of_family = of;
        count[of] += extra_of;
    }
    p.asks[p.n++] = {host, count[host]};
    for (uint32_t i = 0; i < nfam && p.n < 8; i++)
        if (i != host && count[i]) p.asks[p.n++] = {i, count[i]};
    return p;
}

// ---- the camera, for slSetConstants ----

/// 4x4 inverse (Gauss-Jordan in double): false when singular or not finite
inline bool invert4(const float in[16], float out[16]) {
    double a[4][8];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++) a[i][j] = j < 4 ? in[i * 4 + j] : (j - 4 == i ? 1.0 : 0.0);
    for (int col = 0; col < 4; col++) {
        int pivot = col;
        for (int r = col + 1; r < 4; r++)
            if (std::fabs(a[r][col]) > std::fabs(a[pivot][col])) pivot = r;
        if (pivot != col)
            for (int j = 0; j < 8; j++) {
                const double t = a[col][j];
                a[col][j] = a[pivot][j], a[pivot][j] = t;
            }
        const double d = a[col][col];
        if (!std::isfinite(d) || std::fabs(d) < 1e-12) return false;
        for (int j = 0; j < 8; j++) a[col][j] /= d;
        for (int r = 0; r < 4; r++)
            if (r != col) {
                const double k = a[r][col];
                for (int j = 0; j < 8; j++) a[r][j] -= k * a[col][j];
            }
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            out[i * 4 + j] = (float)a[i][j + 4];
            if (!std::isfinite(out[i * 4 + j])) return false;
        }
    return true;
}

/// Streamline's clip-to-previous-clip from the game's (vk_draw.cpp clip_to_previous: column vectors, previous = m * current, m[r*4+c],
/// OpenGL's clip with z in [-w, w], +y the top of the picture). Streamline's clip has z in [0, w] (the depth buffer's value, as D3D's)
/// and its matrices are row-major for row vectors (p' = p * M, sl_matrix_helpers.h): C^-1 * m * C, where z_gl = 2 z - w, transposed
inline void sl_clip_to_prev(const float gl[16], float out[16]) {
    // C^-1 * m * C, worked out: rows 0, 1, 3 keep their x, y, w columns; z's column is 2 m[.][2]; w's column gains -m[.][2];
    // the z row is (m[2][.] + m[3][.]) / 2 of that
    double t[4][4];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            const double mc = c == 2 ? 2.0 * gl[r * 4 + 2] : c == 3 ? gl[r * 4 + 3] - gl[r * 4 + 2] : gl[r * 4 + c];
            t[r][c] = mc;
        }
    for (int c = 0; c < 4; c++) t[2][c] = 0.5 * (t[2][c] + t[3][c]);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) out[r * 4 + c] = (float)t[c][r];
}

struct Camera {
    float pos[3]{}, right[3]{}, up[3]{}, fwd[3]{};   // (world space, orthonormal)
    float near_z = 0, far_z = 0, fov_y = 0, aspect = 0;
    float view_to_clip[16]{}, clip_to_view[16]{};    // (Streamline's: row vectors, view +z forward, clip z in [0, w])
};
inline float dot3(const double* a, const double* b) { return (float)(a[0] * b[0] + a[1] * b[1] + a[2] * b[2]); }

/// The camera of the game's view-projection (column vectors: clip = m * world, m[r*4+c]; OpenGL's clip, z in [-w, w]), for
/// Streamline's constants: the rows of P * V are, for a symmetric perspective, xs * right, ys * up, A * back + B, -back (back = -fwd).
/// The vectors come out orthonormal (Gram-Schmidt from the rows: DLSS-G's scene change detection runs only on an orthonormal
/// camera), the position solves rows 0, 1 and 3 at zero (the camera's centre is where x, y and w are), near and far come from
/// A = -(f + n) / (f - n) and B = -2 f n / (f - n), and view_to_clip is the D3D-style perspective that gives the same depth buffer
/// values (depth = (z_ndc + 1) / 2). False when the matrix is not a perspective one (an orthographic or degenerate projection)
inline bool camera_from_view_projection(const float m[16], Camera& c) {
    double r[4][4];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) r[i][j] = m[i * 4 + j];
    const double s = std::sqrt(r[3][0] * r[3][0] + r[3][1] * r[3][1] + r[3][2] * r[3][2]);
    if (!std::isfinite(s) || s < 1e-9) return false;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) r[i][j] /= s;   // (a homogeneous scale: the same projection)
    double f[3] = {r[3][0], r[3][1], r[3][2]};
    double x[3], y[3];
    const double fx = r[0][0] * f[0] + r[0][1] * f[1] + r[0][2] * f[2];
    for (int k = 0; k < 3; k++) x[k] = r[0][k] - fx * f[k];
    const double xs = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    if (!std::isfinite(xs) || xs < 1e-9) return false;
    for (double& v : x) v /= xs;
    const double fy = r[1][0] * f[0] + r[1][1] * f[1] + r[1][2] * f[2], xy = r[1][0] * x[0] + r[1][1] * x[1] + r[1][2] * x[2];
    for (int k = 0; k < 3; k++) y[k] = r[1][k] - fy * f[k] - xy * x[k];
    const double ys = std::sqrt(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
    if (!std::isfinite(ys) || ys < 1e-9) return false;
    for (double& v : y) v /= ys;
    const double a = -(r[2][0] * f[0] + r[2][1] * f[1] + r[2][2] * f[2]), b = r[2][3] + a * r[3][3];
    if (std::fabs(a - 1.0) < 1e-9) return false;
    const double n = b / (a - 1.0);
    double fa = std::fabs(a + 1.0) < 1e-9 ? n * 1e6 : b / (a + 1.0);   // (an infinite far plane: far enough)
    if (!std::isfinite(n) || !std::isfinite(fa) || n <= 0.0 || fa <= n) return false;
    // the centre: rows 0, 1 and 3 (x, y, w) at zero, Cramer's rule
    const double M[3][3] = {{r[0][0], r[0][1], r[0][2]}, {r[1][0], r[1][1], r[1][2]}, {r[3][0], r[3][1], r[3][2]}};
    const double rhs[3] = {-r[0][3], -r[1][3], -r[3][3]};
    const auto det3 = [](const double q[3][3]) {
        return q[0][0] * (q[1][1] * q[2][2] - q[1][2] * q[2][1]) - q[0][1] * (q[1][0] * q[2][2] - q[1][2] * q[2][0]) +
               q[0][2] * (q[1][0] * q[2][1] - q[1][1] * q[2][0]);
    };
    const double d = det3(M);
    if (!std::isfinite(d) || std::fabs(d) < 1e-12) return false;
    for (int k = 0; k < 3; k++) {
        double q[3][3];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) q[i][j] = j == k ? rhs[i] : M[i][j];
        c.pos[k] = (float)(det3(q) / d);
    }
    for (int k = 0; k < 3; k++) c.right[k] = (float)x[k], c.up[k] = (float)y[k], c.fwd[k] = (float)f[k];
    c.near_z = (float)n, c.far_z = (float)fa;
    c.fov_y = (float)(2.0 * std::atan(1.0 / ys));
    c.aspect = (float)(ys / xs);
    const double zs = fa / (fa - n), zt = -n * fa / (fa - n);
    const float p[16] = {(float)xs, 0, 0, 0, 0, (float)ys, 0, 0, 0, 0, (float)zs, 1, 0, 0, (float)zt, 0};
    std::memcpy(c.view_to_clip, p, sizeof p);
    return invert4(c.view_to_clip, c.clip_to_view);
}

} // namespace mkwvk::dlssg
