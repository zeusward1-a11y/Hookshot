// NVIDIA DLSS super resolution through NVIDIA Streamline, manual hooking (vk_dlss.h). Ported from Lichii (Eden lineage;
// Duck912's Eglantine work), GPL-3.0-or-later: video_core/vulkan_common/streamline.cpp (the loader) and
// renderer_vulkan/vk_streamline_dlss.cpp (the evaluation). Streamline's headers are MIT (third_party/streamline/license.txt).
#include "vk_dlss.h"
#include "vk_internal.h"

#ifdef _WIN32

#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
#include <string>

#include <sl.h>
#include <sl_dlss.h>
#include <sl_helpers_vk.h>
#include <softpub.h>
#include <wintrust.h>

#include "vk_menu.h"

namespace mkwvk {
namespace {

namespace fs = std::filesystem;

// (Lichii's: a custom engine names itself with its own project id; kept fixed, the driver may key behaviour off it)
constexpr const char* PROJECT_ID = "8d3e2f9a-6b41-4c7e-9a52-1f0c3b7d5e64";
constexpr unsigned MIN_DLSS_MAJOR = 310;   // (the first DLSS with preset K)
using Version = std::array<unsigned, 4>;

// the functions Streamline's manual hooking wants routed through its proxies
#define DLSS_HOOKED(X) \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) \
    X(vkDeviceWaitIdle)

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
    bool tried = false, started = false, attached = false, failed = false, hooked = false;
    const char* reason = "not started";
    struct {
#define DLSS_ORIG(f) PFN_##f f = nullptr;
        DLSS_HOOKED(DLSS_ORIG)
#undef DLSS_ORIG
    } orig;   // (put back at stop)
    bool options_set = false;
    uint32_t rw = 0, rh = 0, ow = 0, oh = 0;   // (the sizes the options were last set for)
    sl::DLSSPreset preset{};                    // (and the preset asked for)
};
State st;

int env_int(const char* name, int fallback) {
    const char* v = getenv(name);
    return v ? atoi(v) : fallback;
}

std::string u8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

// RX_DLSS_LOG=1: Streamline's info lines too, 2: verbose
void sl_log(sl::LogType type, const char* msg) {
    std::string s = msg ? msg : "";
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    if (type == sl::LogType::eError || type == sl::LogType::eWarn || env_int("RX_DLSS_LOG", 0) > 0)
        logf("vk: dlss: sl: %s", s.c_str());
}

bool off(const char* why) {
    st.reason = why;
    logf("vk: dlss: off: %s", why);
    return false;
}

template <typename T>
bool resolve(const char* name, T*& f) {
    f = reinterpret_cast<T*>(GetProcAddress(st.module, name));
    if (!f) logf("vk: dlss: sl.interposer.dll has no %s", name);
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
#undef DLSS_RESTORE
    st.hooked = false;
}

/// Attach failed: Streamline stops, DLSS stays off
void shut(const char* why) {
    unhook();
    st.shutdown();
    st.started = st.attached = false;
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
    st.plugin_dir = dir.wstring();
    // the model: the folder's nvngx_dlss.dll, or the NVIDIA app's newest when it is newer still (a tie: the folder's)
    const fs::path app = find_app_model();
    const fs::path app_dir =
        !app.empty() && (!has_model || read_version(app) > read_version(model)) ? use_app_model(app) : fs::path();
    const fs::path used = !app_dir.empty() ? app : has_model ? model : fs::path();
    if (!app_dir.empty()) st.model_dir = app_dir.wstring();
    if (used.empty())
        logf("vk: dlss: no nvngx_dlss.dll in %s and no DLSS %u+ from the NVIDIA app", u8(dir).c_str(), MIN_DLSS_MAJOR);

    // the plugins from the interposer's folder or NVIDIA's downloads (OTA); the model from the NVIDIA app's copy first, when used
    const wchar_t* plugin_paths[] = {st.model_dir.c_str(), st.plugin_dir.c_str()};
    const sl::Feature features[] = {sl::kFeatureDLSS};
    sl::Preferences pref{};
    pref.logLevel = env_int("RX_DLSS_LOG", 0) >= 2 ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
    pref.logMessageCallback = sl_log;
    pref.pathsToPlugins = st.model_dir.empty() ? plugin_paths + 1 : plugin_paths;
    pref.numPathsToPlugins = st.model_dir.empty() ? 1 : 2;
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking |
                 sl::PreferenceFlags::eAllowOTA | sl::PreferenceFlags::eLoadDownloadedPlugins;
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = 1;
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "1.0";
    pref.projectId = PROJECT_ID;
    pref.renderAPI = sl::RenderAPI::eVulkan;
    sl::Result r = st.init(pref, sl::kSDKVersion);
    if (r != sl::Result::eOk) {
        logf("vk: dlss: slInit failed (result %d)", (int)r);
        return off("Streamline did not start");
    }
    sl::FeatureRequirements req{};
    if ((r = st.get_feature_requirements(sl::kFeatureDLSS, req)) != sl::Result::eOk) {
        logf("vk: dlss: no DLSS plugin (result %d): sl.dlss.dll and sl.common.dll beside sl.interposer.dll, or the NVIDIA app",
             (int)r);
        st.shutdown();
        return off("no DLSS plugin");
    }
    if (req.vkNumComputeQueuesRequired || req.vkNumGraphicsQueuesRequired) {
        logf("vk: dlss: DLSS wants %u compute and %u graphics queues of its own", req.vkNumComputeQueuesRequired,
             req.vkNumGraphicsQueuesRequired);
        st.shutdown();
        return off("DLSS wants extra queues");
    }
    for (uint32_t i = 0; i < req.vkNumInstanceExtensions; i++) st.inst_names.emplace_back(req.vkInstanceExtensions[i]);
    bool khr_bda = false;
    for (uint32_t i = 0; i < req.vkNumDeviceExtensions; i++)
        khr_bda |= !strcmp(req.vkDeviceExtensions[i], VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    for (uint32_t i = 0; i < req.vkNumDeviceExtensions; i++)   // (the KHR and EXT buffer device address cannot both be on)
        if (!khr_bda || strcmp(req.vkDeviceExtensions[i], VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME))
            st.dev_names.emplace_back(req.vkDeviceExtensions[i]);
    for (const std::string& s : st.inst_names) st.inst.push_back(s.c_str());
    for (const std::string& s : st.dev_names) st.devx.push_back(s.c_str());
    sl::FeatureVersion version{};
    st.get_feature_version(sl::kFeatureDLSS, version);
    const Version mv = read_version(used).value_or(Version{});
    logf("vk: dlss: Streamline %s started from %s (DLSS %u.%u.%u.%u: %s; Streamline reports %s), %zu instance and %zu device "
         "extensions", version.versionSL.toStr().c_str(), u8(dir).c_str(), mv[0], mv[1], mv[2], mv[3], u8(used).c_str(),
         version.versionNGX.toStr().c_str(), st.inst.size(), st.devx.size());
    st.started = true;
    st.reason = "no device yet";
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

void dlss_attach(VkInstance instance, VkPhysicalDevice physical, VkDevice device, uint32_t queue_family) {
    if (!st.started || st.attached) return;
    sl::VulkanInfo info{};
    info.device = device;
    info.instance = instance;
    info.physicalDevice = physical;
    info.computeQueueFamily = info.graphicsQueueFamily = queue_family;   // (one family for both, as Hookshot's queue)
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
    // manual hooking: Streamline does its per-frame work in these. RX_DLSS_NO_HOOKS=1: not routed (does DLSS-SR need them?)
    if (env_int("RX_DLSS_NO_HOOKS", 0) != 1) {
#define DLSS_HOOK(f)                                                                                  \
        st.orig.f = vkx::f;                                                                           \
        if (FARPROC p = GetProcAddress(st.module, #f)) vkx::f = reinterpret_cast<PFN_##f>(p);         \
        else logf("vk: dlss: sl.interposer.dll has no %s", #f);
        DLSS_HOOKED(DLSS_HOOK)
#undef DLSS_HOOK
        st.hooked = true;
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
    if (st.attached) st.free_resources(sl::kFeatureDLSS, sl::ViewportHandle{0u});
    unhook();
    const sl::Result r = st.shutdown();
    st.started = st.attached = false;
    if (!st.failed) st.reason = "stopped";
    logf("vk: dlss: stopped (result %d)", (int)r);
}

bool dlss_evaluate(VkCommandBuffer cmd, const DlssImage& color, const DlssImage& depth, const DlssImage& motion,
                   const DlssImage& output, bool reset, uint32_t frame_index) {
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
    sl::FrameToken* frame = nullptr;
    if (!stage && ((r = st.get_new_frame_token(frame, &frame_index)) != sl::Result::eOk || !frame)) stage = "slGetNewFrameToken";
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

        // DLSS-SR reads only the jitter, motion and depth conventions; the camera fields are frame generation's
        const float rw = (float)color.width, rh = (float)color.height;
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
        if ((r = st.set_constants(c, *frame, viewport)) != sl::Result::eOk) {
            stage = "slSetConstants";
        } else {
            sl::Resource rc = resource(color, R, VK_IMAGE_USAGE_SAMPLED_BIT), rd = resource(depth, R, VK_IMAGE_USAGE_SAMPLED_BIT),
                         rm = resource(motion, R, VK_IMAGE_USAGE_SAMPLED_BIT), ro = resource(output, G, out_usage);
            const sl::Extent render_area{0, 0, color.width, color.height}, output_area{0, 0, output.width, output.height};
            // local tags: everything stays put until the evaluation is recorded
            sl::ResourceTag tc{&rc, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &render_area},
                            td{&rd, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate, &render_area},
                            tm{&rm, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate, &render_area},
                            to{&ro, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &output_area};
            const sl::BaseStructure* inputs[] = {&viewport, &tc, &td, &tm, &to};
            if ((r = st.evaluate_feature(sl::kFeatureDLSS, *frame, inputs, (uint32_t)std::size(inputs),
                                         reinterpret_cast<sl::CommandBuffer*>(cmd))) != sl::Result::eOk)
                stage = "slEvaluateFeature";
        }
        // back to GENERAL, the output written (also when the evaluation failed: the caller's layouts hold)
        barriers(cmd, {barrier(color, VK_ACCESS_2_SHADER_READ_BIT, any, R, G), barrier(depth, VK_ACCESS_2_SHADER_READ_BIT, any, R, G),
                       barrier(motion, VK_ACCESS_2_SHADER_READ_BIT, any, R, G), barrier(output, dlss_out, any, G, G)});
    }
    if (stage) {
        st.failed = true;
        st.reason = "DLSS evaluation failed";
        logf("vk: dlss: %s failed (result %d): off from now on", stage, (int)r);
        return false;
    }
    return true;
}

} // namespace mkwvk

#else   // (Streamline is Windows only)

namespace mkwvk {
bool dlss_start() { return false; }
const std::vector<const char*>& dlss_instance_exts() { static const std::vector<const char*> none; return none; }
const std::vector<const char*>& dlss_device_exts() { static const std::vector<const char*> none; return none; }
void dlss_attach(VkInstance, VkPhysicalDevice, VkDevice, uint32_t) {}
bool dlss_available() { return false; }
void dlss_render_size(int, uint32_t ow, uint32_t oh, uint32_t& w, uint32_t& h) { w = ow, h = oh; }
const char* dlss_reason() { return "Windows only"; }
void dlss_stop() {}
bool dlss_evaluate(VkCommandBuffer, const DlssImage&, const DlssImage&, const DlssImage&, const DlssImage&, bool, uint32_t) {
    return false;
}
} // namespace mkwvk

#endif
