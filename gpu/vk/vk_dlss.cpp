// NVIDIA DLSS super resolution and DLSS Frame Generation through NVIDIA Streamline, manual hooking (vk_dlss.h). Super resolution
// ported from Lichii (Eden lineage; Duck912's Eglantine work), GPL-3.0-or-later: video_core/vulkan_common/streamline.cpp (the loader)
// and renderer_vulkan/vk_streamline_dlss.cpp (the evaluation). Frame generation follows Streamline 2.14.16's guides
// (ProgrammingGuideDLSS_G.md, ProgrammingGuideManualHooking.md) and NX2navi's working integration (runtime/dlssg/nx2_sl.cpp: Reflex
// from the frame's start, frames numbered by the game's presents). Streamline's headers are MIT (third_party/streamline/license.txt).
#include "vk_dlss.h"
#include "vk_internal.h"

#ifdef _WIN32

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

#include <sl.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_helpers_vk.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <softpub.h>
#include <wintrust.h>

#include "dlss_result.h"
#include "dlssg_logic.h"
#include "vk_menu.h"

namespace mkwvk {
HWND app_window();   // (vk_main.cpp)
void menu_note_extra_frames(uint32_t n);   // (vk_menu.cpp: frames DLSS-G showed beyond the game's, for the FPS counter)
namespace {

namespace fs = std::filesystem;

// (Lichii's: a custom engine names itself with its own project id; kept fixed, the driver may key behaviour off it)
constexpr const char* PROJECT_ID = "8d3e2f9a-6b41-4c7e-9a52-1f0c3b7d5e64";
constexpr unsigned MIN_DLSS_MAJOR = 310;   // (the first DLSS with preset K)
using Version = std::array<unsigned, 4>;
constexpr int kSettle = 2;            // (presents with generation off after a swapchain is made)
constexpr int kStatusRetry = 300;     // (presents with generation off after a status that was not ok, then it is tried again)
constexpr int64_t kHoldIdleNs = 200000000;   // (no Reflex sleep once nothing was presented for this long)
constexpr uint32_t kLogEvery = 600;   // (presents between the frame generation log lines)

// the functions Streamline's manual hooking wants routed through its proxies
#define DLSS_HOOKED(X) \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) \
    X(vkDeviceWaitIdle)
// (DLSS-G) and the window surface's: Streamline learns which window a swapchain shows from vkCreateWin32SurfaceKHR, and without it
// NX2navi's DLSS-G found "no window corresponding to this application" and made no frames (sl_hooks.h lists both as mandatory)
#define DLSSG_HOOKED(X) X(vkCreateWin32SurfaceKHR) X(vkDestroySurfaceKHR)

struct State {
    HMODULE module = nullptr;
    PFun_slInit* init = nullptr;
    PFun_slShutdown* shutdown = nullptr;
    PFun_slIsFeatureSupported* is_feature_supported = nullptr;
    PFun_slGetFeatureRequirements* get_feature_requirements = nullptr;
    PFun_slGetFeatureVersion* get_feature_version = nullptr;
    PFun_slGetFeatureFunction* get_feature_function = nullptr;
    PFun_slGetNewFrameToken* get_new_frame_token = nullptr;
    PFun_slSetConstants* set_constants = nullptr;
    PFun_slEvaluateFeature* evaluate_feature = nullptr;
    PFun_slFreeResources* free_resources = nullptr;
    PFun_slSetVulkanInfo* set_vulkan_info = nullptr;
    PFun_slDLSSSetOptions* set_options = nullptr;
    PFun_slDLSSGetOptimalSettings* get_optimal = nullptr;
    std::wstring plugin_dir, model_dir;
    std::vector<std::string> inst_names, dev_names;
    std::vector<const char*> inst, devx;
    bool tried = false, started = false, attached = false, failed = false, hooked = false, surface_hooked = false;
    const char* reason = "not started";
    struct {
#define DLSS_ORIG(f) PFN_##f f = nullptr;
        DLSS_HOOKED(DLSS_ORIG)
        DLSSG_HOOKED(DLSS_ORIG)
#undef DLSS_ORIG
    } orig;   // (put back at stop)
    bool options_set = false;
    uint32_t rw = 0, rh = 0, ow = 0, oh = 0;   // (the sizes the options were last set for)
    sl::DLSSPreset preset{};                    // (and the preset asked for)
    uint32_t over_budget = 0;                   // (frames drawn with Streamline's eWarnOutOfVRAM: dlss_result.h)

    // (DLSS Frame Generation, Reflex, PCL)
    PFun_slSetTagForFrame* set_tag = nullptr;
    PFun_slIsFeatureLoaded* is_loaded = nullptr;
    PFun_slSetFeatureLoaded* set_loaded = nullptr;
    PFun_slDLSSGSetOptions* fg_options = nullptr;
    PFun_slDLSSGGetState* fg_state = nullptr;
    PFun_slReflexSetOptions* reflex_options = nullptr;
    PFun_slReflexSleep* reflex_sleep = nullptr;
    PFun_slPCLSetMarker* marker = nullptr;
    bool fg_asked = false;   // (the row or RX_DLSSG at the start: DLSS-G, Reflex and PCL loaded with DLSS)
    bool fg_init = false;    // (slInit was asked for DLSS-G: unloaded again at attach when it cannot run)
    bool fg_ok = false;      // (supported here, its functions found: dlssg_available)
    const char* fg_reason = "not asked for";
    uint32_t fg_max = 0;     // (numFramesToGenerateMax, 0 until DLSS-G says)
    bool fg_announced = false;
    uint32_t extra_graphics = 0, extra_compute = 0, extra_of = 0;
    std::vector<std::string> feat12, feat13;   // (the Vulkan 1.2 and 1.3 features the plugins asked for)
    dlssg::QueuePlan plan{};
    bool planned = false;
    std::vector<std::vector<float>> priorities;
    DlssQueues queues;
    int reflex_asked = 1, reflex_applied = -1;
    uint32_t limit_applied = ~0u;   // (Reflex's frameLimitUs last sent)
    int limit_basis = dlssg::LIMIT_GAME;   // (RX_DLSSG_LIMIT=game|present|0: what the limit counts, dlssg_logic.h)
    int player_fps = 0;             // (RX_DLSSG_FPS=N: the player's cap on the game's frames while DLSS-G can run, dlssg_logic.h)
    bool mailbox_asked = false;     // (RX_DLSSG_MAILBOX=1: mailbox before immediate while DLSS-G runs)
    uint32_t refresh_hz = 0;        // (the window's monitor, at the last swapchain)
};
State st;

// The frames (vk_dlss.h). The game's thread: game_presents, starter. The replay (the thread that records and presents):
// replay_presents, the frame's constants and tags, and everything a present decides.
struct Frames {
    std::atomic<uint64_t> game_presents{0}, replay_presents{0};
    std::atomic<int64_t> last_present_ns{0};
    dlssg::FrameStart starter;
    std::atomic<uint32_t> constants_frame{0}, tagged_frame{0};
    uint32_t last_tagged = 0, tag_w = 0, tag_h = 0;
    dlssg::Session session;   // (the options last sent, the frame limit's cap, the free after the Off: dlssg_logic.h)
    dlssg::LineGate lines;    // (the "generation on/off" lines: on a change only)
    std::atomic<bool> generating{false};
    int last_off = dlssg::OFF_UNAVAILABLE;
    dlssg::SwapchainGate gate;
    dlssg::StatusGate status;
    dlssg::SizeGate size;

    uint32_t last_sw = 0, last_sh = 0;   // (the window's image at the last present)
    dlssg::Stats stats;

};
Frames fr;

int env_int(const char* name, int fallback) {
    const char* v = getenv(name);
    return v ? atoi(v) : fallback;
}

int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string u8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

// RX_DLSS_LOG=1: Streamline's info lines too, 2: verbose. Its warnings and errors: the first 400, then one in 1000 (a warning a
// present, with frame generation, filled the log), all of them with RX_DLSS_LOG
std::mutex sl_log_mu;
dlssg::LogLimit sl_log_limit;
void sl_log(sl::LogType type, const char* msg) {
    static const int level = env_int("RX_DLSS_LOG", 0);
    const bool loud = type == sl::LogType::eError || type == sl::LogType::eWarn;
    if (!loud && level <= 0) return;
    if (loud && level <= 0) {
        std::lock_guard lk(sl_log_mu);
        if (!sl_log_limit.allow(400, 1000)) return;
    }
    std::string s = msg ? msg : "";
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    logf("vk: dlss: sl: %s", s.c_str());
}

bool off(const char* why) {
    st.reason = why;
    logf("vk: dlss: off: %s", why);
    return false;
}

void fg_off(const char* why) {
    st.fg_ok = false;
    st.fg_reason = why;
    logf("vk: dlssg: off: %s", why);
}

template <typename T>
bool resolve(const char* name, T*& f) {
    f = reinterpret_cast<T*>(GetProcAddress(st.module, name));
    if (!f) logf("vk: dlss: sl.interposer.dll has no %s", name);
    return f != nullptr;
}

template <typename T>
bool feature_function(sl::Feature feature, const char* name, T*& f) {
    void* p = nullptr;
    const sl::Result r = st.get_feature_function(feature, name, p);
    f = r == sl::Result::eOk ? reinterpret_cast<T*>(p) : nullptr;
    if (!f) logf("vk: dlssg: no %s (result %d)", name, (int)r);
    return f != nullptr;
}

fs::path exe_dir() {
    wchar_t b[4096];
    const DWORD n = GetModuleFileNameW(nullptr, b, (DWORD)std::size(b));
    return fs::path(std::wstring(b, n)).parent_path();
}

std::optional<Version> read_version(const fs::path& path) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (!size) return std::nullopt;
    std::vector<uint8_t> data(size);
    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()) ||
        !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info)
        return std::nullopt;
    return Version{HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS),
                   LOWORD(info->dwFileVersionLS)};
}

/// A valid Authenticode signature from NVIDIA Corporation (sl.interposer.dll and the DLSS models). The plain WinVerifyTrust
/// API: Streamline's sl_security.h needs WINTRUST_SIGNATURE_SETTINGS, which older mingw headers do not have
bool signed_by_nvidia(const fs::path& path) {
    std::error_code ec;
    const std::wstring file_path = fs::absolute(path, ec).wstring();
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = file_path.c_str();
    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    bool valid = WinVerifyTrust(nullptr, &action, &data) == ERROR_SUCCESS;
    if (valid) {
        CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(data.hWVTStateData);
        CRYPT_PROVIDER_SGNR* signer = provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
        CRYPT_PROVIDER_CERT* cert = signer ? WTHelperGetProvCertFromChain(signer, 0) : nullptr;
        wchar_t name[128]{};
        valid = cert &&
                CertGetNameStringW(cert->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, (DWORD)std::size(name)) > 1 &&
                std::wstring(name) == L"NVIDIA Corporation";
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &data);
    return valid;
}

/// The NVIDIA app keeps every DLSS it downloads under ProgramData: the newest of major >= 310
fs::path find_app_model() {
    const wchar_t* program_data = _wgetenv(L"ProgramData");
    if (!program_data) return {};
    const fs::path root = fs::path(program_data) / L"NVIDIA" / L"NGX" / L"models" / L"dlss" / L"versions";
    fs::path best_path;
    Version best{};
    std::error_code ec;
    for (const auto& version_dir : fs::directory_iterator(root, ec)) {
        std::error_code files_ec;
        for (const auto& file : fs::directory_iterator(version_dir.path() / L"files", files_ec)) {
            if (file.path().extension() != L".bin") continue;
            const std::optional<Version> v = read_version(file.path());
            if (v && (*v)[0] >= MIN_DLSS_MAJOR && *v > best) best = *v, best_path = file.path();
        }
    }
    return best_path;
}

/// The NVIDIA app's DLSS copied (signature checked) to <exe dir>\streamline\nvidia-app\nvngx_dlss.dll, the name
/// Streamline loads: that folder, or empty
fs::path use_app_model(const fs::path& app) {
    const fs::path dir = exe_dir() / L"streamline" / L"nvidia-app", target = dir / L"nvngx_dlss.dll";
    std::error_code ec;
    const bool current = fs::is_regular_file(target, ec) && fs::file_size(target, ec) == fs::file_size(app, ec) &&
                         read_version(target) == read_version(app);
    if (!current) {
        if (!signed_by_nvidia(app)) {
            logf("vk: dlss: the NVIDIA app's DLSS %s is not signed by NVIDIA", u8(app).c_str());
            return {};
        }
        fs::create_directories(dir, ec);
        fs::copy_file(app, target, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            logf("vk: dlss: could not copy the NVIDIA app's DLSS to %s: %s", u8(target).c_str(), ec.message().c_str());
            return {};
        }
    }
    if (!signed_by_nvidia(target)) {
        logf("vk: dlss: %s is not signed by NVIDIA", u8(target).c_str());
        return {};
    }
    return dir;
}

void unhook() {
    if (!st.hooked) return;
#define DLSS_RESTORE(f) vkx::f = st.orig.f;
    DLSS_HOOKED(DLSS_RESTORE)
    if (st.surface_hooked) {
        DLSSG_HOOKED(DLSS_RESTORE)
    }
#undef DLSS_RESTORE
    st.hooked = st.surface_hooked = false;
}

/// Attach failed: Streamline stops, DLSS stays off
void shut(const char* why) {
    unhook();
    st.shutdown();
    st.started = st.attached = false;
    st.fg_ok = false;
    st.fg_reason = "DLSS is off";
    off(why);
}

// DLSS checks the render size against the mode's range: the mode for this ratio
sl::DLSSMode mode_for(uint32_t render_w, uint32_t output_w) {
    const float ratio = (float)render_w / (float)output_w;
    return ratio >= 0.99f ? sl::DLSSMode::eDLAA :
           ratio >= 0.65f ? sl::DLSSMode::eMaxQuality :
           ratio >= 0.57f ? sl::DLSSMode::eBalanced :
           ratio >= 0.49f ? sl::DLSSMode::eMaxPerformance : sl::DLSSMode::eUltraPerformance;
}

// The menu's DLSS preset row, one preset for every mode: M by default (the owner's choice; Lichii kept K, as NVIDIA's own
// M and L for Performance/Ultra Performance drew thin horizontal lines there). RX_DLSS_PRESET=J|K|L|M sets the row
// (menu_init), =default: NVIDIA's choice per mode
sl::DLSSPreset preset_now() {
    static const char* v = getenv("RX_DLSS_PRESET");
    if (v && !strcmp(v, "default")) return sl::DLSSPreset::eDefault;
    return (sl::DLSSPreset)((uint32_t)sl::DLSSPreset::ePresetJ + (uint32_t)opts.dlss_preset);
}
std::string preset_name(sl::DLSSPreset p) {
    return p == sl::DLSSPreset::eDefault ? "default" : std::string(1, (char)('A' + (uint32_t)p - 1));
}

sl::float4x4 identity() {
    sl::float4x4 m;
    m.setRow(0, {1.0f, 0.0f, 0.0f, 0.0f});
    m.setRow(1, {0.0f, 1.0f, 0.0f, 0.0f});
    m.setRow(2, {0.0f, 0.0f, 1.0f, 0.0f});
    m.setRow(3, {0.0f, 0.0f, 0.0f, 1.0f});
    return m;
}
/// A Streamline matrix from 16 floats already in its order (rows of a row-vector matrix: dlssg_logic.h)
sl::float4x4 rows(const float* v) {
    sl::float4x4 m;
    for (uint32_t r = 0; r < 4; r++) m.setRow(r, {v[r * 4], v[r * 4 + 1], v[r * 4 + 2], v[r * 4 + 3]});
    return m;
}

/// The frame's common constants (one slSetConstants a frame, DLSS super resolution's and DLSS-G's). DLSS-SR reads only the jitter,
/// motion and depth conventions; DLSS-G the camera too: from the game's view-projection when it is a perspective one (dlssg_logic.h
/// camera_from_view_projection: orthonormal vectors, the clip-to-previous-clip converted from OpenGL's z), identity otherwise
sl::Constants frame_constants(const DlssView* view, bool reset, float rw, float rh) {
    sl::Constants c{};
    c.cameraViewToClip = c.clipToCameraView = c.clipToLensClip = c.clipToPrevClip = c.prevClipToClip = identity();
    c.jitterOffset = {0.0f, 0.0f};
    c.mvecScale = {1.0f / rw, 1.0f / rh};   // (motion in render pixels; Streamline wants [-1,1] screen units)
    c.cameraPinholeOffset = {0.0f, 0.0f};
    c.cameraPos = {0.0f, 0.0f, 0.0f};
    c.cameraUp = {0.0f, 1.0f, 0.0f};
    c.cameraRight = {1.0f, 0.0f, 0.0f};
    c.cameraFwd = {0.0f, 0.0f, 1.0f};
    c.cameraNear = 0.1f, c.cameraFar = 1000.0f, c.cameraFOV = 1.0f, c.cameraAspectRatio = rw / rh;
    c.depthInverted = sl::Boolean::eFalse;
    c.cameraMotionIncluded = sl::Boolean::eTrue;
    c.motionVectors3D = sl::Boolean::eFalse;
    c.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    c.motionVectorsJittered = sl::Boolean::eFalse;
    dlssg::Camera cam;
    if (view && view->view_projection && dlssg::camera_from_view_projection(view->view_projection, cam)) {
        c.cameraViewToClip = rows(cam.view_to_clip);
        c.clipToCameraView = rows(cam.clip_to_view);
        c.cameraPos = {cam.pos[0], cam.pos[1], cam.pos[2]};
        c.cameraRight = {cam.right[0], cam.right[1], cam.right[2]};
        c.cameraUp = {cam.up[0], cam.up[1], cam.up[2]};
        c.cameraFwd = {cam.fwd[0], cam.fwd[1], cam.fwd[2]};
        c.cameraNear = cam.near_z, c.cameraFar = cam.far_z, c.cameraFOV = cam.fov_y, c.cameraAspectRatio = cam.aspect;
        float to_prev[16], from_prev[16];
        if (view->clip_to_previous) {
            dlssg::sl_clip_to_prev(view->clip_to_previous, to_prev);
            if (dlssg::invert4(to_prev, from_prev)) c.clipToPrevClip = rows(to_prev), c.prevClipToClip = rows(from_prev);
        }
    }
    return c;
}

sl::FrameToken* token(uint32_t frame) {   // (Streamline's ring of six reuses its tokens: each use takes its own right before it)
    sl::FrameToken* t = nullptr;
    return frame && st.get_new_frame_token && st.get_new_frame_token(t, &frame) == sl::Result::eOk ? t : nullptr;
}
void mark(sl::PCLMarker m, uint32_t frame) {
    if (!st.marker) return;
    if (sl::FrameToken* t = token(frame)) st.marker(m, *t);
}

VkImageMemoryBarrier2 barrier(const DlssImage& i, VkAccessFlags2 src, VkAccessFlags2 dst, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = src, b.dstAccessMask = dst;
    b.oldLayout = from, b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = i.image;
    b.subresourceRange = {i.aspect, 0, 1, 0, 1};
    return b;
}

void barriers(VkCommandBuffer cmd, const std::array<VkImageMemoryBarrier2, 4>& b) {
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.imageMemoryBarrierCount = (uint32_t)b.size();
    d.pImageMemoryBarriers = b.data();
    vkCmdPipelineBarrier2(cmd, &d);
}

sl::Resource resource(const DlssImage& i, VkImageLayout layout, VkImageUsageFlags usage) {
    sl::Resource r{sl::ResourceType::eTex2d, (void*)i.image, nullptr, (void*)i.view, (uint32_t)layout};
    r.width = i.width, r.height = i.height;
    r.nativeFormat = (uint32_t)i.format;
    r.mipLevels = r.arrayLayers = 1;
    r.flags = 0;
    r.usage = usage;
    return r;
}

const char* support_text(sl::Result r) {
    switch (r) {
    case sl::Result::eErrorOSOutOfDate: return "Windows too old for it";
    case sl::Result::eErrorDriverOutOfDate: return "NVIDIA driver too old for it";
    case sl::Result::eErrorOSDisabledHWS: return "hardware-accelerated GPU scheduling is off";
    case sl::Result::eErrorNoSupportedAdapterFound:
    case sl::Result::eErrorAdapterNotSupported: return "not supported on this GPU";
    default: return "not supported here";
    }
}

/// Reflex's mode, set again after every swapchain (Streamline applies it to the swapchain it knows, VK_NV_low_latency2) and when the
/// row turns generation on or off. RX_REFLEX=0|1|2 (default 1); 0 is 1 while generating (dlssg_logic.h reflex_mode)
/// The refresh rate of the monitor the window is on (EnumDisplaySettings of its device), 0 when not known
uint32_t monitor_refresh() {
    const HWND w = app_window();
    HMONITOR mon = w ? MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST) : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    if (!mon || !GetMonitorInfoW(mon, &mi)) return 0;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    return EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1 ? dm.dmDisplayFrequency : 0;
}
void apply_reflex(bool force) {
    if (!st.fg_ok || !st.reflex_options) return;
    const int mode = dlssg::reflex_mode(st.reflex_asked, dlssg_engaged());
    // (RX_VK_FIFO not taken: what is shown at most 97% of the refresh, where G-SYNC holds the picture, from what DLSS-G does now:
    // the options last sent, so a menu or a frame DLSS did not draw does not hold the game at the row's multiplier)
    const int shown = fr.session.generating() ? (int)fr.session.applied.frames + 1 : 1;
    const uint32_t limit = fr.session.limit(st.limit_basis, dlssg_engaged(), st.refresh_hz, st.player_fps);
    if (!force && mode == st.reflex_applied && limit == st.limit_applied) return;
    sl::ReflexOptions o{};
    o.mode = mode == 2 ? sl::ReflexMode::eLowLatencyWithBoost : mode == 1 ? sl::ReflexMode::eLowLatency : sl::ReflexMode::eOff;
    o.frameLimitUs = limit;
    const sl::Result r = st.reflex_options(o);
    static int limits_logged;   // (generation on and off changes it: the first ones)
    if (limit != st.limit_applied && (fr.session.cap.on || st.player_fps || st.limit_applied != ~0u) && limits_logged++ < 20)
        logf("vk: dlssg: monitor %u Hz: Reflex frame limit %u us (basis %s, %dx shown, RX_DLSSG_FPS %d; result %d)", st.refresh_hz,
             limit, dlssg::limit_name(st.limit_basis), shown, st.player_fps, (int)r);
    st.limit_applied = limit;
    static int logged;
    if (mode != st.reflex_applied || logged < 4) {
        logged++;
        logf("vk: dlssg: Reflex %s (result %d)%s", mode == 2 ? "low latency with boost" : mode == 1 ? "low latency" : "off", (int)r,
             st.reflex_asked == 0 && mode != 0 ? " (RX_REFLEX=0, but DLSS-G needs Reflex while it generates)" : "");
    }
    st.reflex_applied = mode;
}

/// DLSS-G's state (numFramesToGenerateMax, numFramesActuallyPresented, status); false without it
/// A DLSS-G call's eWarnOutOfVRAM: counted for the 600-present line with the VRAM used and allowed then (dlssg_present_end)
bool vram_warned = false;
void note_result(sl::Result r) {
    if (r == sl::Result::eWarnOutOfVRAM) vram_warned = true;
}
bool fg_state(sl::DLSSGState& s) {
    if (!st.fg_ok || !st.fg_state) return false;
    const sl::Result r = st.fg_state(sl::ViewportHandle{0u}, s, nullptr);
    note_result(r);
    return dlss_drawn(r);   // (eOk, or eWarnOutOfVRAM: the state is there, a warning only; taken as no state, the frames shown read 1)
}
void announce() {
    if (st.fg_announced || !st.fg_max) return;
    st.fg_announced = true;
    logf("vk: dlssg: available, max %ux (%u generated per game frame), Reflex %s", st.fg_max + 1, st.fg_max,
         st.reflex_applied == 2 ? "low latency with boost" : st.reflex_applied == 1 ? "low latency" : "off");
}

/// At attach, the device known: is DLSS-G usable here. Not: unloaded again (slSetFeatureLoaded), so its present hook adds nothing
void fg_attach(VkPhysicalDevice physical) {
    if (!st.fg_init) return;
    bool loaded = false;
    sl::AdapterInfo adapter{};
    adapter.vkPhysicalDevice = physical;
    sl::Result r = sl::Result::eOk;
    if (st.is_loaded) st.is_loaded(sl::kFeatureDLSS_G, loaded);
    if (!st.fg_asked) {
        st.fg_ok = false;   // (a plugin or its requirements were missing at the start: the reason is logged there)
    } else if (!loaded) {
        fg_off("DLSS-G did not load (sl.dlss_g.dll and nvngx_dlssg.dll beside sl.interposer.dll)");
    } else if ((r = st.is_feature_supported(sl::kFeatureDLSS_G, adapter)) != sl::Result::eOk) {
        logf("vk: dlssg: slIsFeatureSupported: result %d", (int)r);
        fg_off(support_text(r));
    } else if ((r = st.is_feature_supported(sl::kFeatureReflex, adapter)) != sl::Result::eOk) {
        logf("vk: dlssg: Reflex: slIsFeatureSupported result %d", (int)r);
        fg_off("NVIDIA Reflex not supported here");
    } else if (!feature_function(sl::kFeatureDLSS_G, "slDLSSGSetOptions", st.fg_options) ||
               !feature_function(sl::kFeatureDLSS_G, "slDLSSGGetState", st.fg_state) ||
               !feature_function(sl::kFeatureReflex, "slReflexSetOptions", st.reflex_options) ||
               !feature_function(sl::kFeatureReflex, "slReflexSleep", st.reflex_sleep) ||
               !feature_function(sl::kFeaturePCL, "slPCLSetMarker", st.marker) || !st.set_tag) {
        fg_off("Streamline lacks DLSS-G's, Reflex's or PCL's functions");
    } else if (st.planned && !st.plan.ok) {
        fg_off("the GPU has too few queues for it");
    } else {
        st.fg_ok = true;
        st.fg_reason = "";
    }
    if (!st.fg_ok) {
        st.marker = nullptr, st.reflex_sleep = nullptr;
        if (loaded && st.set_loaded) {
            r = st.set_loaded(sl::kFeatureDLSS_G, false);
            logf("vk: dlssg: DLSS-G unloaded (result %d): DLSS draws without frame generation", (int)r);
        }
        return;
    }
    apply_reflex(true);
    sl::DLSSGState s{};
    if (fg_state(s)) st.fg_max = s.numFramesToGenerateMax;
    sl::FeatureVersion v{};
    st.get_feature_version(sl::kFeatureDLSS_G, v);
    logf("vk: dlssg: DLSS-G %s on Streamline %s; tags copied when tagged%s", v.versionNGX.toStr().c_str(), v.versionSL.toStr().c_str(),
         st.fg_max ? "" : " (its most frames are known at the first swapchain)");
    announce();
}

} // namespace

bool dlss_start() {
    if (st.tried) return st.started;
    st.tried = true;
    // where sl.interposer.dll may be: RX_DLSS_DIR, then <exe dir>\streamline
    std::vector<fs::path> dirs;
    if (const char* d = getenv("RX_DLSS_DIR"); d && *d) dirs.emplace_back(d);
    dirs.push_back(exe_dir() / L"streamline");
    std::error_code ec;
    fs::path dir;
    for (const fs::path& d : dirs)
        if (fs::is_regular_file(d / L"sl.interposer.dll", ec)) {
            dir = fs::absolute(d, ec);
            break;
        }
    st.fg_asked = opts.dlssg >= 2;
    st.fg_reason = st.fg_asked ? "DLSS did not start" : "Off at the start";
    if (dir.empty()) {
        logf("vk: dlss: no sl.interposer.dll in %s", u8(dirs.back()).c_str());
        return off("no sl.interposer.dll");
    }
    const fs::path interposer = dir / L"sl.interposer.dll", model = dir / L"nvngx_dlss.dll";
    const bool has_model = fs::is_regular_file(model, ec);
    if (has_model && !signed_by_nvidia(model)) {
        logf("vk: dlss: %s is not signed by NVIDIA", u8(model).c_str());
        return off("nvngx_dlss.dll not signed by NVIDIA");
    }
    if (!signed_by_nvidia(interposer)) {
        logf("vk: dlss: %s is not signed by NVIDIA; not loading it", u8(interposer).c_str());
        return off("sl.interposer.dll not signed by NVIDIA");
    }
    if (st.fg_asked) {   // (frame generation's model, when it is in the folder: NVIDIA's too, or DLSS-G is not loaded)
        const fs::path fg_model = dir / L"nvngx_dlssg.dll";
        if (fs::is_regular_file(fg_model, ec) && !signed_by_nvidia(fg_model)) {
            logf("vk: dlssg: %s is not signed by NVIDIA", u8(fg_model).c_str());
            st.fg_asked = false, st.fg_reason = "nvngx_dlssg.dll not signed by NVIDIA";
        }
    }
    st.module = LoadLibraryW(interposer.c_str());
    if (!st.module) {
        logf("vk: dlss: could not load %s (error %lu)", u8(interposer).c_str(), GetLastError());
        return off("sl.interposer.dll did not load");
    }
    if (!(resolve("slInit", st.init) && resolve("slShutdown", st.shutdown) &&
          resolve("slIsFeatureSupported", st.is_feature_supported) &&
          resolve("slGetFeatureRequirements", st.get_feature_requirements) &&
          resolve("slGetFeatureVersion", st.get_feature_version) &&
          resolve("slGetFeatureFunction", st.get_feature_function) &&
          resolve("slGetNewFrameToken", st.get_new_frame_token) && resolve("slSetConstants", st.set_constants) &&
          resolve("slEvaluateFeature", st.evaluate_feature) && resolve("slFreeResources", st.free_resources) &&
          resolve("slSetVulkanInfo", st.set_vulkan_info))) {
        FreeLibrary(st.module);
        st.module = nullptr;
        return off("sl.interposer.dll incomplete");
    }
    if (st.fg_asked && !(resolve("slSetTagForFrame", st.set_tag) && resolve("slIsFeatureLoaded", st.is_loaded) &&
                         resolve("slSetFeatureLoaded", st.set_loaded)))
        st.fg_asked = false, st.fg_reason = "sl.interposer.dll too old for DLSS-G";
    st.plugin_dir = dir.wstring();
    // the model: the folder's nvngx_dlss.dll, or the NVIDIA app's newest when it is newer still (a tie: the folder's)
    const fs::path app = find_app_model();
    const fs::path app_dir =
        !app.empty() && (!has_model || read_version(app) > read_version(model)) ? use_app_model(app) : fs::path();
    const fs::path used = !app_dir.empty() ? app : has_model ? model : fs::path();
    if (!app_dir.empty()) st.model_dir = app_dir.wstring();
    if (used.empty())
        logf("vk: dlss: no nvngx_dlss.dll in %s and no DLSS %u+ from the NVIDIA app", u8(dir).c_str(), MIN_DLSS_MAJOR);

    // the plugins from the interposer's folder or NVIDIA's downloads (OTA); the model from the NVIDIA app's copy first, when used.
    // (DLSS-G) with frame generation: DLSS-G, Reflex (which it needs) and PCL (the latency markers), and frame-based tagging: the
    // depth and motion are tagged for their frame (slSetTagForFrame), which Streamline refuses without the flag
    const wchar_t* plugin_paths[] = {st.model_dir.c_str(), st.plugin_dir.c_str()};
    const sl::Feature features[] = {sl::kFeatureDLSS, sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
    sl::Preferences pref{};
    pref.logLevel = env_int("RX_DLSS_LOG", 0) >= 2 ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
    pref.logMessageCallback = sl_log;
    pref.pathsToPlugins = st.model_dir.empty() ? plugin_paths + 1 : plugin_paths;
    pref.numPathsToPlugins = st.model_dir.empty() ? 1 : 2;
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking |
                 sl::PreferenceFlags::eAllowOTA | sl::PreferenceFlags::eLoadDownloadedPlugins;
    // Separate the game's Vulkan plugins from OptiScaler's D3D12 runtime. Sharing
    // an OTA PCL/common DLL lets the second slInit overwrite the first's state.
    if (env_int("RX_DLSS_NO_OTA", 0)) {
        pref.flags &= ~sl::PreferenceFlags::eAllowOTA;
        pref.flags &= ~sl::PreferenceFlags::eLoadDownloadedPlugins;
        logf("vk: dlss: OTA plugins disabled (RX_DLSS_NO_OTA), using local plugins");
    }
    if (st.fg_asked) pref.flags |= sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    st.fg_init = st.fg_asked;
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = st.fg_asked ? 4 : 1;
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "1.0";
    pref.projectId = PROJECT_ID;
    pref.renderAPI = sl::RenderAPI::eVulkan;
    sl::Result r = st.init(pref, sl::kSDKVersion);
    if (r != sl::Result::eOk) {
        logf("vk: dlss: slInit failed (result %d)", (int)r);
        return off("Streamline did not start");
    }
    // what each feature asks of the instance and the device: extensions, Vulkan 1.2/1.3 features and queues of its own (all honoured:
    // vk_main.cpp makes them, slSetVulkanInfo is told where they are)
    const auto take = [](const sl::FeatureRequirements& req) {
        const auto add = [](std::vector<std::string>& to, const char* name) {
            if (name && std::find(to.begin(), to.end(), name) == to.end()) to.emplace_back(name);
        };
        for (uint32_t i = 0; i < req.vkNumInstanceExtensions; i++) add(st.inst_names, req.vkInstanceExtensions[i]);
        for (uint32_t i = 0; i < req.vkNumDeviceExtensions; i++) add(st.dev_names, req.vkDeviceExtensions[i]);
        for (uint32_t i = 0; i < req.vkNumFeatures12; i++) add(st.feat12, req.vkFeatures12[i]);
        for (uint32_t i = 0; i < req.vkNumFeatures13; i++) add(st.feat13, req.vkFeatures13[i]);
        st.extra_graphics += req.vkNumGraphicsQueuesRequired;
        st.extra_compute += req.vkNumComputeQueuesRequired;
        st.extra_of += req.vkNumOpticalFlowQueuesRequired;
    };
    sl::FeatureRequirements req{};
    if ((r = st.get_feature_requirements(sl::kFeatureDLSS, req)) != sl::Result::eOk) {
        logf("vk: dlss: no DLSS plugin (result %d): sl.dlss.dll and sl.common.dll beside sl.interposer.dll, or the NVIDIA app",
             (int)r);
        st.shutdown();
        return off("no DLSS plugin");
    }
    take(req);
    if (st.fg_asked) {
        static const struct { sl::Feature f; const char* dll; } fg_features[] = {
            {sl::kFeatureDLSS_G, "sl.dlss_g.dll"}, {sl::kFeatureReflex, "sl.reflex.dll"}, {sl::kFeaturePCL, "sl.pcl.dll"}};
        static std::string missing;
        for (const auto& f : fg_features) {
            sl::FeatureRequirements q{};
            if ((r = st.get_feature_requirements(f.f, q)) != sl::Result::eOk) {
                logf("vk: dlssg: no %s plugin (result %d)", f.dll, (int)r);
                missing = std::string("no ") + f.dll;
                st.fg_asked = false, st.fg_reason = missing.c_str();
                continue;
            }
            if (f.f == sl::kFeatureDLSS_G && !(q.flags & sl::FeatureRequirementFlags::eVulkanSupported)) {
                st.fg_asked = false, st.fg_reason = "this DLSS-G has no Vulkan";
                continue;
            }
            if (f.f == sl::kFeatureDLSS_G)
                logf("vk: dlssg: DLSS-G asks for %u graphics, %u compute and %u optical flow queues, driver %u.%u (%u.%u needed), "
                     "OS %u.%u.%u (%u.%u.%u needed)%s%s", q.vkNumGraphicsQueuesRequired, q.vkNumComputeQueuesRequired,
                     q.vkNumOpticalFlowQueuesRequired, q.driverVersionDetected.major, q.driverVersionDetected.minor,
                     q.driverVersionRequired.major, q.driverVersionRequired.minor, q.osVersionDetected.major,
                     q.osVersionDetected.minor, q.osVersionDetected.build, q.osVersionRequired.major, q.osVersionRequired.minor,
                     q.osVersionRequired.build,
                     q.flags & sl::FeatureRequirementFlags::eHardwareSchedulingRequired ? ", hardware GPU scheduling" : "",
                     q.flags & sl::FeatureRequirementFlags::eVSyncOffRequired ? ", vsync off" : "");
            take(q);
        }
        if (!st.fg_asked) logf("vk: dlssg: off: %s", st.fg_reason);
    }
    bool khr_bda = false;   // (the KHR and EXT buffer device address cannot both be on)
    for (const std::string& s : st.dev_names) khr_bda |= s == VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME;
    if (khr_bda) st.dev_names.erase(std::remove(st.dev_names.begin(), st.dev_names.end(), VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME),
                                    st.dev_names.end());
    for (const std::string& s : st.inst_names) st.inst.push_back(s.c_str());
    for (const std::string& s : st.dev_names) st.devx.push_back(s.c_str());
    st.reflex_asked = dlssg::parse_reflex(getenv("RX_REFLEX"), 1);
    st.limit_basis = dlssg::parse_limit(getenv("RX_DLSSG_LIMIT"));
    st.player_fps = dlssg::parse_game_fps(getenv("RX_DLSSG_FPS"));
    st.mailbox_asked = env_int("RX_DLSSG_MAILBOX", 0) != 0;
    sl::FeatureVersion version{};
    st.get_feature_version(sl::kFeatureDLSS, version);
    const Version mv = read_version(used).value_or(Version{});
    logf("vk: dlss: Streamline %s started from %s (DLSS %u.%u.%u.%u: %s; Streamline reports %s), %zu instance and %zu device "
         "extensions, %u graphics, %u compute and %u optical flow queues of its own%s", version.versionSL.toStr().c_str(),
         u8(dir).c_str(), mv[0], mv[1], mv[2], mv[3], u8(used).c_str(), version.versionNGX.toStr().c_str(), st.inst.size(),
         st.devx.size(), st.extra_graphics, st.extra_compute, st.extra_of,
         st.fg_asked ? "; with DLSS Frame Generation, Reflex and PCL" : "");
    st.started = true;
    st.reason = "no device yet";
    if (st.fg_asked) st.fg_reason = "no device yet";
    return true;
}

const std::vector<const char*>& dlss_instance_exts() {
    static const std::vector<const char*> none;
    return st.started ? st.inst : none;
}

const std::vector<const char*>& dlss_device_exts() {
    static const std::vector<const char*> none;
    return st.started ? st.devx : none;
}

const DlssQueues& dlss_queues(VkPhysicalDevice physical, uint32_t host_family, const std::vector<VkQueueFamilyProperties>& families,
                              bool has_optical_flow_ext) {
    std::vector<dlssg::Family> fams;
    for (const VkQueueFamilyProperties& f : families) fams.push_back({(uint32_t)f.queueFlags, f.queueCount});
    const bool sl_queues = st.started;
    bool of_ok = false;   // (native optical flow: the extension and its feature; RX_DLSSG_NATIVE_OF=0: interop, as without)
    if (sl_queues && st.extra_of && has_optical_flow_ext && env_int("RX_DLSSG_NATIVE_OF", 1) != 0) {
        VkPhysicalDeviceOpticalFlowFeaturesNV of{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &of;
        vkGetPhysicalDeviceFeatures2(physical, &f2);
        of_ok = of.opticalFlow == VK_TRUE;
    }
    st.plan = dlssg::plan_queues(fams.data(), (uint32_t)fams.size(), host_family, sl_queues ? st.extra_graphics : 0,
                                 sl_queues ? st.extra_compute : 0, sl_queues ? st.extra_of : 0, of_ok);
    if (!st.plan.ok) {   // (too few queues: Streamline told only of the host's, its features without queues of their own)
        logf("vk: dlss: the GPU's queue families have too few queues for the %u graphics and %u compute Streamline asked for",
             st.extra_graphics, st.extra_compute);
        st.plan = dlssg::plan_queues(fams.data(), (uint32_t)fams.size(), host_family, 0, 0, 0, false);
        st.plan.ok = false;
    }
    st.planned = sl_queues;
    st.priorities.assign(st.plan.n, {});
    st.queues.infos.assign(st.plan.n, VkDeviceQueueCreateInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO});
    for (uint32_t i = 0; i < st.plan.n; i++) {
        // (Hookshot's queue first in its family at 1; Streamline's at 0, as its own proxy makes them)
        std::vector<float>& p = st.priorities[i];
        p.assign(st.plan.asks[i].count, 0.0f);
        if (st.plan.asks[i].family == host_family) p[0] = 1.0f;
        st.queues.infos[i].queueFamilyIndex = st.plan.asks[i].family;
        st.queues.infos[i].queueCount = st.plan.asks[i].count;
        st.queues.infos[i].pQueuePriorities = p.data();
    }
    st.queues.optical_flow = st.plan.native_of;
    if (sl_queues && (st.extra_graphics || st.extra_compute || st.extra_of))
        logf("vk: dlss: queues for Streamline: graphics family %u from %u, compute family %u from %u, optical flow %s", st.plan.graphics_family,
             st.plan.graphics_index, st.plan.compute_family, st.plan.compute_index,
             st.plan.native_of ? "native (VK_NV_optical_flow)" : st.extra_of ? "interop (no VK_NV_optical_flow)" : "none");
    return st.queues;
}

void dlss_check_features(const VkPhysicalDeviceVulkan12Features& f12, const VkPhysicalDeviceVulkan13Features& f13) {
    if (!st.started) return;
    // (each name alone through Streamline's own table, sl_helpers_vk.h, against the same field of what the device is made with)
    const auto check = [](const std::vector<std::string>& names, const auto& have, auto make, const VkBool32* have_first,
                          size_t count, const char* which) {
        (void)have;
        for (const std::string& n : names) {
            const char* name = n.c_str();
            const auto want = make(1u, &name);
            const VkBool32* w = reinterpret_cast<const VkBool32*>(reinterpret_cast<const char*>(&want) +
                                                                  (reinterpret_cast<const char*>(have_first) -
                                                                   reinterpret_cast<const char*>(&have)));
            bool known = false, ok = true;
            for (size_t i = 0; i < count; i++)
                if (w[i]) known = true, ok = ok && have_first[i];
            if (!known) logf("vk: dlss: Streamline asked for Vulkan %s feature %s, not one this build knows", which, name);
            else if (!ok) logf("vk: dlss: the GPU lacks Vulkan %s feature %s, which Streamline asked for", which, name);
        }
    };
    // (first to last field: the structs end in padding, sizeof would read one VkBool32 past the last)
    constexpr size_t n12 = (offsetof(VkPhysicalDeviceVulkan12Features, subgroupBroadcastDynamicId) -
                            offsetof(VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge)) / sizeof(VkBool32) + 1;
    constexpr size_t n13 = (offsetof(VkPhysicalDeviceVulkan13Features, maintenance4) -
                            offsetof(VkPhysicalDeviceVulkan13Features, robustImageAccess)) / sizeof(VkBool32) + 1;
    static_assert(n12 == 47 && n13 == 15, "Vulkan 1.2 has 47 features, 1.3 has 15");
    check(st.feat12, f12, sl::getVkPhysicalDeviceVulkan12Features, &f12.samplerMirrorClampToEdge, n12, "1.2");
    check(st.feat13, f13, sl::getVkPhysicalDeviceVulkan13Features, &f13.robustImageAccess, n13, "1.3");
}

void dlss_attach(VkInstance instance, VkPhysicalDevice physical, VkDevice device, uint32_t queue_family) {
    if (!st.started || st.attached) return;
    sl::VulkanInfo info{};
    info.device = device;
    info.instance = instance;
    info.physicalDevice = physical;
    info.computeQueueFamily = info.graphicsQueueFamily = queue_family;   // (one family for both, as Hookshot's queue, index 0)
    if (st.planned) {   // (where vk_main.cpp made Streamline's own queues: ManualHooking guide 5.2.2)
        info.graphicsQueueFamily = st.plan.graphics_family, info.graphicsQueueIndex = st.plan.graphics_index;
        info.computeQueueFamily = st.plan.compute_family, info.computeQueueIndex = st.plan.compute_index;
        if (st.plan.native_of) {
            info.opticalFlowQueueFamily = st.plan.of_family, info.opticalFlowQueueIndex = 0;
            info.useNativeOpticalFlowMode = true;
        }
    }
    sl::Result r = st.set_vulkan_info(info);
    if (r != sl::Result::eOk) {
        logf("vk: dlss: slSetVulkanInfo failed (result %d)", (int)r);
        return shut("Streamline did not take the device");
    }
    sl::AdapterInfo adapter{};
    adapter.vkPhysicalDevice = physical;
    if ((r = st.is_feature_supported(sl::kFeatureDLSS, adapter)) != sl::Result::eOk) {
        logf("vk: dlss: not supported on this GPU or driver (result %d)", (int)r);
        return shut("GPU not supported");
    }
    void* set_options = nullptr;
    if ((r = st.get_feature_function(sl::kFeatureDLSS, "slDLSSSetOptions", set_options)) != sl::Result::eOk || !set_options) {
        logf("vk: dlss: no slDLSSSetOptions (result %d)", (int)r);
        return shut("no slDLSSSetOptions");
    }
    st.set_options = reinterpret_cast<PFun_slDLSSSetOptions*>(set_options);
    void* get_optimal = nullptr;   // (the menu's DLSS mode labels and render sizes; standard ratios without it)
    if (st.get_feature_function(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", get_optimal) == sl::Result::eOk)
        st.get_optimal = reinterpret_cast<PFun_slDLSSGetOptimalSettings*>(get_optimal);
    fg_attach(physical);
    // manual hooking: Streamline does its per-frame work in these. RX_DLSS_NO_HOOKS=1: not routed (does DLSS-SR need them?)
    if (env_int("RX_DLSS_NO_HOOKS", 0) != 1) {
#define DLSS_HOOK(f)                                                                                  \
        st.orig.f = vkx::f;                                                                           \
        if (FARPROC p = GetProcAddress(st.module, #f)) vkx::f = reinterpret_cast<PFN_##f>(p);         \
        else logf("vk: dlss: sl.interposer.dll has no %s", #f);
        DLSS_HOOKED(DLSS_HOOK)
        if (st.fg_ok) {   // (before the window's surface is made: vk_main.cpp start)
            DLSSG_HOOKED(DLSS_HOOK)
            st.surface_hooked = true;
        }
#undef DLSS_HOOK
        st.hooked = true;
    } else if (st.fg_ok) {
        fg_off("RX_DLSS_NO_HOOKS: the present is not Streamline's");
    }
    st.attached = true;
    st.reason = "";
    sl::FeatureVersion version{};   // (again with the device: NGX's own number, if it differs from the start's)
    st.get_feature_version(sl::kFeatureDLSS, version);
    logf("vk: dlss: available, DLSS %s%s", version.versionNGX.toStr().c_str(),
         st.hooked ? "" : " (RX_DLSS_NO_HOOKS: swapchain calls not routed through Streamline)");
}

void dlss_render_size(int mode, uint32_t ow, uint32_t oh, uint32_t& w, uint32_t& h) {
    static const sl::DLSSMode modes[] = {sl::DLSSMode::eUltraPerformance, sl::DLSSMode::eMaxPerformance, sl::DLSSMode::eBalanced,
                                         sl::DLSSMode::eMaxQuality, sl::DLSSMode::eDLAA};
    static const float ratios[] = {1.0f / 3.0f, 0.5f, 0.58f, 2.0f / 3.0f, 1.0f};
    mode = std::min(std::max(mode, 0), 4);
    w = (uint32_t)(ow * ratios[mode] + 0.5f), h = (uint32_t)(oh * ratios[mode] + 0.5f);
    static uint32_t got_w[5], got_h[5], got_ow, got_oh;   // (asked once per output size)
    if (!st.get_optimal || !dlss_available()) return;
    if (got_ow != ow || got_oh != oh) std::fill_n(got_w, 5, 0u), got_ow = ow, got_oh = oh;
    if (!got_w[mode]) {
        sl::DLSSOptions o{};
        o.mode = modes[mode], o.outputWidth = ow, o.outputHeight = oh;
        sl::DLSSOptimalSettings s{};
        if (st.get_optimal(o, s) == sl::Result::eOk && s.optimalRenderWidth && s.optimalRenderHeight)
            got_w[mode] = s.optimalRenderWidth, got_h[mode] = s.optimalRenderHeight;
        else
            got_w[mode] = w, got_h[mode] = h;
    }
    w = got_w[mode], h = got_h[mode];
}

bool dlss_available() {
    return st.started && st.attached && !st.failed;
}

const char* dlss_reason() {
    return st.reason;
}

void dlss_stop() {
    if (!st.started) return;
    if (st.attached) {
        st.free_resources(sl::kFeatureDLSS, sl::ViewportHandle{0u});
        if (st.fg_ok) st.free_resources(sl::kFeatureDLSS_G, sl::ViewportHandle{0u});
    }
    unhook();
    const sl::Result r = st.shutdown();
    st.started = st.attached = st.fg_ok = false;
    if (!st.failed) st.reason = "stopped";
    logf("vk: dlss: stopped (result %d)", (int)r);
}

bool dlss_evaluate(VkCommandBuffer cmd, const DlssImage& color, const DlssImage& depth, const DlssImage& motion,
                   const DlssImage& output, bool reset, uint32_t frame, const DlssView& view) {
    if (!dlss_available()) return false;
    const sl::DLSSPreset preset = preset_now();   // (the menu's row: a change applies at once, history reset)
    static const char* const mode_names[] = {"off", "Performance", "Balanced", "Quality", "Ultra Performance", "Ultra Quality", "DLAA"};
    const sl::ViewportHandle viewport{0u};
    const bool changed = !st.options_set || st.preset != preset || st.rw != color.width || st.rh != color.height ||
                         st.ow != output.width || st.oh != output.height;
    reset = reset || changed;
    sl::Result r = sl::Result::eOk;
    const char* stage = nullptr;
    if (changed) {
        // LDR: the game's colour is tonemapped already, so no HDR and no auto exposure
        sl::DLSSOptions o{};
        o.mode = mode_for(color.width, output.width);
        o.outputWidth = output.width, o.outputHeight = output.height;
        o.colorBuffersHDR = sl::Boolean::eFalse;
        o.useAutoExposure = sl::Boolean::eFalse;
        sl::DLSSPreset use = preset;
        const auto set = [&] {
            o.dlaaPreset = o.qualityPreset = o.balancedPreset = o.performancePreset = o.ultraPerformancePreset =
                o.ultraQualityPreset = use;
            return st.set_options(viewport, o);
        };
        if ((r = set()) != sl::Result::eOk && use != sl::DLSSPreset::ePresetK) {   // (K: in every DLSS from 310)
            logf("vk: dlss: preset %s not accepted (result %d): K", preset_name(use).c_str(), (int)r);
            use = sl::DLSSPreset::ePresetK;
            r = set();
        }
        logf("vk: dlss: %ux%u -> %ux%u, mode %s, preset %s", color.width, color.height, output.width, output.height,
             mode_names[(uint32_t)o.mode], preset_name(use).c_str());
        if (r != sl::Result::eOk) stage = "slDLSSSetOptions";
        else st.options_set = true, st.preset = preset, st.rw = color.width, st.rh = color.height, st.ow = output.width,
             st.oh = output.height;
    }
    // the frame's constants, once a frame (DLSS-G reads the same; the guide: never twice in a frame). Generation's history is reset
    // with DLSS's, and after a frame it did not have (dlssg_logic.h reset_history)
    const bool fg_on = dlssg_engaged();   // (the row Off: no tags, no copies; turned on again, the first frame resets)
    const bool fg_reset = fg_on && (dlssg::reset_history(reset, frame, fr.last_tagged) || fr.tag_w != color.width ||
                                    fr.tag_h != color.height);
    sl::FrameToken* t = nullptr;
    if (!stage && !(t = token(frame))) r = sl::Result::eErrorInvalidParameter, stage = "slGetNewFrameToken";
    if (!stage && fr.constants_frame.load() != frame) {
        const sl::Constants c = frame_constants(&view, reset || fg_reset, (float)color.width, (float)color.height);
        if ((r = st.set_constants(c, *t, viewport)) != sl::Result::eOk) stage = "slSetConstants";
        else fr.constants_frame = frame;
    }
    if (!stage && !(t = token(frame))) r = sl::Result::eErrorInvalidParameter, stage = "slGetNewFrameToken";
    if (!stage) {
        constexpr VkImageUsageFlags out_usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        constexpr VkAccessFlags2 any = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        constexpr VkAccessFlags2 dlss_out = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
        constexpr VkImageLayout G = VK_IMAGE_LAYOUT_GENERAL, R = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        // the inputs as sampled images for DLSS; the output (GENERAL) waits for whatever used it last
        barriers(cmd, {barrier(color, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT, G, R),
                       barrier(depth, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT, G, R),
                       barrier(motion, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT, G, R),
                       barrier(output, any, dlss_out, G, G)});
        sl::Resource rc = resource(color, R, VK_IMAGE_USAGE_SAMPLED_BIT), rd = resource(depth, R, VK_IMAGE_USAGE_SAMPLED_BIT),
                     rm = resource(motion, R, VK_IMAGE_USAGE_SAMPLED_BIT), ro = resource(output, G, out_usage);
        const sl::Extent render_area{0, 0, color.width, color.height}, output_area{0, 0, output.width, output.height};
        // local tags: everything stays put until the evaluation is recorded
        sl::ResourceTag tc{&rc, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &render_area},
                        td{&rd, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate, &render_area},
                        tm{&rm, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate, &render_area},
                        to{&ro, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &output_area};
        const sl::BaseStructure* inputs[] = {&viewport, &tc, &td, &tm, &to};
        r = st.evaluate_feature(sl::kFeatureDLSS, *t, inputs, (uint32_t)std::size(inputs), reinterpret_cast<sl::CommandBuffer*>(cmd));
        if (!dlss_drawn(r)) {
            stage = "slEvaluateFeature";
        } else if (r == sl::Result::eWarnOutOfVRAM && st.over_budget++ % 600 == 0) {   // (drawn: a warning only)
            logf("vk: dlss: drawn over the VRAM budget (Streamline's eWarnOutOfVRAM, %u frames so far)", st.over_budget);
        }
        // back to GENERAL, the output written (also when the evaluation failed: the caller's layouts hold)
        barriers(cmd, {barrier(color, VK_ACCESS_2_SHADER_READ_BIT, any, R, G), barrier(depth, VK_ACCESS_2_SHADER_READ_BIT, any, R, G),
                       barrier(motion, VK_ACCESS_2_SHADER_READ_BIT, any, R, G), barrier(output, dlss_out, any, G, G)});
        // (DLSS-G) the same depth and motion for frame generation at the present, in GENERAL here. Copied by Streamline as they are
        // now (eOnlyValidNow): the game's depth target is drawn into again before the present (and may be freed), and next frame's
        // motion pass rewrites ours
        if (!stage && fg_on) {
            constexpr VkImageUsageFlags depth_usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                                      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            constexpr VkImageUsageFlags motion_usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            sl::Resource gd = resource(depth, G, depth.usage ? depth.usage : depth_usage),
                         gm = resource(motion, G, motion.usage ? motion.usage : motion_usage);
            const sl::ResourceTag tags[] = {sl::ResourceTag{&gd, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &render_area},
                                            sl::ResourceTag{&gm, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow,
                                                            &render_area}};
            sl::FrameToken* ft = token(frame);
            const sl::Result tr = ft ? st.set_tag(*ft, viewport, tags, 2, reinterpret_cast<sl::CommandBuffer*>(cmd))
                                     : sl::Result::eErrorInvalidParameter;
            if (tr == sl::Result::eOk) {
                fr.tagged_frame = frame, fr.last_tagged = frame, fr.tag_w = color.width, fr.tag_h = color.height;
            } else {
                static int told;
                if (told++ < 8) logf("vk: dlssg: frame %u: slSetTagForFrame failed (result %d): no generation for it", frame, (int)tr);
            }
        }
    }
    if (stage) {
        st.failed = true;
        st.reason = "DLSS evaluation failed";
        logf("vk: dlss: %s failed (result %d): off from now on", stage, (int)r);
        return false;
    }
    return true;
}

// ---- DLSS Frame Generation ----

bool dlssg_requested() { return st.fg_asked; }
bool dlssg_available() { return st.fg_ok && dlss_available(); }
const char* dlssg_reason() { return st.fg_ok && !dlss_available() ? "DLSS is off" : st.fg_reason; }
uint32_t dlssg_max_multiplier() { return st.fg_max ? st.fg_max + 1 : 0; }
bool dlssg_engaged() { return dlssg_available() && opts.dlssg >= 2; }
bool dlssg_generating() { return fr.generating.load(std::memory_order_relaxed); }
uint32_t dlss_frame_id() { return dlssg::frame_of(fr.replay_presents.load(std::memory_order_relaxed)); }
void dlss_note_present(uint64_t presents) { fr.replay_presents.store(presents, std::memory_order_relaxed); }
void dlssg_game_present() { fr.game_presents.fetch_add(1, std::memory_order_relaxed); }

void dlssg_frame_start() {
    if (!st.fg_ok || !st.marker) return;
    const uint32_t f = dlssg::frame_of(fr.game_presents.load(std::memory_order_relaxed));
    if (!fr.starter.begin(f)) return;
    // Reflex holds the frame's start until the time the driver computes from the measured render and queue times, so that the game's
    // CPU work ends about when the GPU is free for it (NX2navi: without this sleep DLSS-G halved the game's frame rate). The game's
    // simulation and its recording are one span here (its threads cannot be told apart): its markers at the start, the render submit
    // until the present sends the frame's work (dlssg_present_begin)
    // (in every frame, Reflex on or off: the Reflex guide wants slReflexSleep and the markers always)
    if (st.reflex_sleep && dlssg::hold(now_ns(), fr.last_present_ns.load(), kHoldIdleNs))
        if (sl::FrameToken* t = token(f)) st.reflex_sleep(*t);
    mark(sl::PCLMarker::eSimulationStart, f);
    mark(sl::PCLMarker::eSimulationEnd, f);
    mark(sl::PCLMarker::eRenderSubmitStart, f);
}

namespace {
/// eOff that keeps the resources, at once (dlssg_logic.h early_off): a swapchain about to change, a minimized window
void off_now(int why, const char* what) {
    if (dlssg::early_off(fr.generating.load(), why == dlssg::OFF_WINDOW, why == dlssg::OFF_SWAPCHAIN) == dlssg::ON) return;
    const dlssg::Mode m = dlssg::mode_for(why, opts.dlssg, st.fg_max);
    sl::DLSSGOptions o{};
    o.mode = sl::DLSSGMode::eOff;
    o.numFramesToGenerate = m.frames;
    o.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
    const sl::Result r = st.fg_options(sl::ViewportHandle{0u}, o);
    note_result(r);
    if (fr.session.sent_options(m, (int)r)) fr.generating = false;
    static int told;
    if (told++ < 20) logf("vk: dlssg: generation off at once: %s (result %d)", what, (int)r);
}
} // namespace

void dlss_swapchain_changing() {
    if (!st.fg_ok || !st.fg_options) return;
    off_now(dlssg::OFF_SWAPCHAIN, "the swapchain is made again");
    fr.gate.changed(kSettle);
}

void dlssg_window_check() {
    if (!st.fg_ok || !st.fg_options) return;
    if (app_window() && IsIconic(app_window())) off_now(dlssg::OFF_WINDOW, "the window is minimized");
}

void dlssg_present_mode(bool capped, bool fifo_ignored, bool immediate) {
    static bool told_fifo;
    if (fifo_ignored && st.fg_ok && !told_fifo) {
        told_fifo = true;
        logf("vk: dlssg: RX_VK_FIFO ignored while DLSS-G is on (G-SYNC through the driver; VSync with frame generation is D3D12-only)");
    }
    // (each swapchain's: DLSS failed or off, the next one takes FIFO or mailbox again, without a limit)
    const int change = fr.session.swapchain(capped, st.fg_ok && dlss_available());
    if (!change) return;
    if (change > 0)
        logf("vk: dlssg: present mode %s while DLSS-G is on (it wants vsync off; G-SYNC through the driver), Reflex's frame limit under the "
             "refresh, basis %s (RX_DLSSG_LIMIT=game|present|0)", immediate ? "immediate" : "mailbox", dlssg::limit_name(st.limit_basis));
    else
        logf("vk: dlssg: the present mode is the usual one again (DLSS-G not running): no Reflex frame limit");
    apply_reflex(true);
}

bool dlssg_mailbox_asked() { return st.mailbox_asked; }

bool dlss_defer_swapchain() {
    if (!st.fg_ok) return false;
    const bool wait = fr.gate.defer(fr.generating.load(), kSettle);
    static int told;   // (a window dragged to a new size resizes every frame: the first few only)
    if (wait && told++ < 10) logf("vk: dlssg: the swapchain is made again after this present, which turns generation off");
    return wait;
}

void dlss_swapchain_created() {
    if (!st.fg_ok) return;
    fr.gate.changed(kSettle);
    st.refresh_hz = monitor_refresh();   // (the window may have gone to another monitor)
    apply_reflex(true);
    sl::DLSSGState s{};
    if (!st.fg_max && fg_state(s) && s.numFramesToGenerateMax) st.fg_max = s.numFramesToGenerateMax;
    announce();
}

void dlssg_present_begin(uint32_t frame, bool image, int x, int y, uint32_t w, uint32_t h, uint32_t sw, uint32_t sh) {
    if (!st.fg_ok) return;
    const sl::ViewportHandle viewport{0u};
    if (!frame) {   // (the built-in interpolation presents: the row was turned Off at run time, so DLSS-G goes off, its memory freed)
        if (fr.session.needs_free_off()) {   // (also after "off, kept": the menu was open when the row went Off)
            const dlssg::Mode m = dlssg::mode_for(dlssg::OFF_USER, opts.dlssg, st.fg_max);
            sl::DLSSGOptions o{};
            o.mode = sl::DLSSGMode::eOff;
            o.numFramesToGenerate = m.frames;
            const sl::Result r = st.fg_options(viewport, o);
            note_result(r);
            const bool took = fr.session.sent_options(m, (int)r);   // (it asks for the free)
            static int told, failed;
            if (took ? told++ < 5 : failed++ < 10)
                logf("vk: dlssg: generation off (the built-in frame generation presents; result %d)", (int)r);
        }
        fr.generating = false;
        apply_reflex(false);   // (no limit now: DLSS-G is not what presents)
        return;
    }
    dlssg::PresentState p;
    p.available = dlssg_available() && st.fg_options;
    p.multiplier = opts.dlssg;
    p.minimized = app_window() && IsIconic(app_window());
    p.swapchain_hold = fr.gate.hold;
    p.status_hold = fr.status.hold;
    p.too_small = fr.size.too_small(sw, sh);
    fr.last_sw = sw, fr.last_sh = sh;
    if (fr.size.tell(sw, sh))
        logf("vk: dlssg: the window's %ux%u is under DLSS-G's minimum (%u, or it said too small at this size): generation off until "
             "the window's size changes", sw, sh, fr.size.min);
    p.menu_open = menu_is_open();
    p.image = image;
    p.tagged = fr.tagged_frame.load() == frame;
    const int why = dlssg::decide(p);
    fr.last_off = why;
    // generation on or off for this present (slDLSSGSetOptions takes effect at the next present: this one, on this thread)
    const dlssg::Mode m = dlssg::mode_for(why, opts.dlssg, st.fg_max);
    if (!fr.session.sent || !dlssg::same_mode(m, fr.session.applied)) {
        sl::DLSSGOptions o{};
        o.mode = m.on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
        o.numFramesToGenerate = m.frames;
        o.flags = m.retain ? sl::DLSSGFlags::eRetainResourcesWhenOff : sl::DLSSGFlags{};
        const sl::Result r = st.fg_options(viewport, o);
        note_result(r);
        fr.stats.sent++;
        if (fr.lines.changed(why, m, (int)r))   // (on a change of what was sent or of the answer: eWarnOutOfVRAM is 39)
            logf("vk: dlssg: frame %u: generation %s (%s, %u generated%s; result %d%s)", frame, m.on ? "on" : "off", dlssg::off_name(why),
                 m.on ? m.frames : 0u, m.retain ? "" : ", resources freed", (int)r,
                 r == sl::Result::eWarnOutOfVRAM ? ": over the VRAM budget, a warning" : "");
        else
            fr.stats.repeats++;
        fr.session.sent_options(m, (int)r);   // (eOk or eWarnOutOfVRAM: taken; the player's Off asks for the free)
    }
    fr.generating = fr.session.generating();
    apply_reflex(false);   // (Reflex's mode and frame limit follow what DLSS-G does for this present, set just now)
    // a frame DLSS did not draw: no depth and motion for it (null tags, as the guide asks while they are not valid) and neutral
    // constants, so that every frame has its one set
    sl::FrameToken* t = nullptr;
    if (fr.tagged_frame.load() != frame && st.set_tag && (t = token(frame))) {
        const sl::ResourceTag none[] = {sl::ResourceTag{nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent},
                                        sl::ResourceTag{nullptr, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent}};
        st.set_tag(*t, viewport, none, 2, nullptr);
    }
    if (fr.constants_frame.load() != frame && (t = token(frame))) {
        const sl::Constants c = frame_constants(nullptr, true, (float)std::max(w, 1u), (float)std::max(h, 1u));
        if (st.set_constants(c, *t, viewport) == sl::Result::eOk) fr.constants_frame = frame;
    }
    // the game's picture inside the window's image when it has bars: generation runs on that part only, the rest copied
    if (m.on && (x || y || w != sw || h != sh) && w && h && (t = token(frame))) {
        const sl::Extent part{(uint32_t)std::max(y, 0), (uint32_t)std::max(x, 0), w, h};   // (Extent: top, left, width, height)
        const sl::ResourceTag bb[] = {sl::ResourceTag{nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent, &part}};
        st.set_tag(*t, viewport, bb, 1, nullptr);
    }
    mark(sl::PCLMarker::eRenderSubmitEnd, frame);
    mark(sl::PCLMarker::ePresentStart, frame);   // (this frame's number is what DLSS-G looks its constants and tags up by)
}

void dlssg_present_end(uint32_t frame) {
    if (!st.fg_ok) return;
    // (the guide's 6.4: memory kept with eRetainResourcesWhenOff is freed with slFreeResources once the player turned it off; the
    // GPU idle first, DLSS-G's work of the last frames done)
    if (fr.session.after_present()) {
        vkDeviceWaitIdle(dev.device);
        const sl::Result r = st.free_resources(sl::kFeatureDLSS_G, sl::ViewportHandle{0u});
        static int told, failed;
        if (r == sl::Result::eOk ? told++ < 10 : failed++ < 10) logf("vk: dlssg: slFreeResources after the Off: result %d", (int)r);
    }
    if (!frame) return;
    mark(sl::PCLMarker::ePresentEnd, frame);
    fr.last_present_ns = now_ns();
    sl::DLSSGState s{};
    const bool have = fg_state(s);
    // asked after each present, numFramesActuallyPresented is what that present showed (2 with one generated frame)
    const uint32_t shown = have ? s.numFramesActuallyPresented : 1;
    if (have && s.numFramesToGenerateMax && s.numFramesToGenerateMax != st.fg_max) {
        if (st.fg_announced) logf("vk: dlssg: max now %ux", s.numFramesToGenerateMax + 1);
        st.fg_max = s.numFramesToGenerateMax;
        announce();
    }
    if (have && s.minWidthOrHeight) fr.size.min = s.minWidthOrHeight;
    if (have && fr.generating.load() && (s.status & sl::DLSSGStatus::eFailResolutionTooLow))
        fr.size.failed(fr.last_sw, fr.last_sh);   // (off at this size until it changes: no retry every 300 presents)
    if (fr.status.update(fr.generating.load(), have ? (uint32_t)s.status : 0u, kStatusRetry))
        logf("vk: dlssg: frame %u: status %#x%s", frame, (unsigned)s.status, dlssg::StatusGate::text((uint32_t)s.status));
    fr.gate.presented();
    if (shown > 1) menu_note_extra_frames(shown - 1);
    fr.stats.add(fr.last_off, shown, have ? (uint32_t)s.status : 0u);
    if (vram_warned) {   // (a DLSS-G call of this present said eWarnOutOfVRAM: what the VRAM was then)
        vram_warned = false;
        uint64_t used = 0, budget = 0;
        vram_usage(&used, &budget);
        fr.stats.warned(used >> 20, budget >> 20);
    }
    if (fr.stats.presents >= kLogEvery) {
        char line[512];
        fr.stats.line(line, sizeof line, opts.dlssg);
        logf("%s", line);
        fr.stats.clear();
    }
}

} // namespace mkwvk

#else   // (Streamline is Windows only)

namespace mkwvk {
bool dlss_start() { return false; }
const std::vector<const char*>& dlss_instance_exts() { static const std::vector<const char*> none; return none; }
const std::vector<const char*>& dlss_device_exts() { static const std::vector<const char*> none; return none; }
const DlssQueues& dlss_queues(VkPhysicalDevice, uint32_t host_family, const std::vector<VkQueueFamilyProperties>&, bool) {
    static const float one = 1.0f;
    static DlssQueues q;
    q.infos.assign(1, VkDeviceQueueCreateInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO});
    q.infos[0].queueFamilyIndex = host_family, q.infos[0].queueCount = 1, q.infos[0].pQueuePriorities = &one;
    q.optical_flow = false;
    return q;
}
void dlss_check_features(const VkPhysicalDeviceVulkan12Features&, const VkPhysicalDeviceVulkan13Features&) {}
void dlss_attach(VkInstance, VkPhysicalDevice, VkDevice, uint32_t) {}
bool dlss_available() { return false; }
void dlss_render_size(int, uint32_t ow, uint32_t oh, uint32_t& w, uint32_t& h) { w = ow, h = oh; }
const char* dlss_reason() { return "Windows only"; }
void dlss_stop() {}
bool dlss_evaluate(VkCommandBuffer, const DlssImage&, const DlssImage&, const DlssImage&, const DlssImage&, bool, uint32_t,
                   const DlssView&) {
    return false;
}
bool dlssg_requested() { return false; }
bool dlssg_available() { return false; }
const char* dlssg_reason() { return "Windows only"; }
uint32_t dlssg_max_multiplier() { return 0; }
bool dlssg_engaged() { return false; }
bool dlssg_generating() { return false; }
uint32_t dlss_frame_id() { return 1; }
void dlss_note_present(uint64_t) {}
void dlssg_game_present() {}
void dlssg_frame_start() {}
bool dlss_defer_swapchain() { return false; }
void dlss_swapchain_created() {}
void dlss_swapchain_changing() {}
void dlssg_window_check() {}
void dlssg_present_mode(bool, bool, bool) {}
bool dlssg_mailbox_asked() { return false; }
void dlssg_present_begin(uint32_t, bool, int, int, uint32_t, uint32_t, uint32_t, uint32_t) {}
void dlssg_present_end(uint32_t) {}
} // namespace mkwvk

#endif
