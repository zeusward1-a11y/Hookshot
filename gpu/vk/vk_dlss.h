// NVIDIA DLSS super resolution and DLSS Frame Generation through NVIDIA Streamline (vk_dlss.cpp), manual hooking. Only Streamline's
// MIT headers are built in (third_party/streamline); sl.interposer.dll, its plugins and NVIDIA's DLSS models are loaded from the
// user's PC at run time and none ship with Hookshot. DLSS super resolution ported from Lichii (Eden lineage; Duck912's Eglantine
// work), GPL-3.0-or-later.
#pragma once

#include <cstdint>
#include <vector>

#include "vk_loader.h"

namespace mkwvk {

/// At start, before create_instance: sl.interposer.dll from RX_DLSS_DIR, else <exe dir>\streamline\ (signatures checked;
/// the NVIDIA app's newest DLSS when newer than the nvngx_dlss.dll there, or there is none), slInit with manual hooking. With the
/// DLSS Frame Generation row (or RX_DLSSG) at 2x and up, DLSS-G, Reflex and PCL are loaded too. True when started
bool dlss_start();
const std::vector<const char*>& dlss_instance_exts();   // (empty unless started; VK_EXT_debug_utils is the caller's)
const std::vector<const char*>& dlss_device_exts();     // (empty unless started)
/// The device's queues: Hookshot's one (host_family, index 0, priority 1) and those Streamline's features asked for
/// (slGetFeatureRequirements: extra graphics and compute queues, and an optical flow one when the device has VK_NV_optical_flow
/// with its feature), planned as Streamline's own vkCreateDevice proxy would (dlssg_logic.h plan_queues). optical_flow: the
/// extension and VkPhysicalDeviceOpticalFlowFeaturesNV::opticalFlow must be on. The pointers stay valid until the next call
struct DlssQueues {
    std::vector<VkDeviceQueueCreateInfo> infos;
    bool optical_flow = false;
};
const DlssQueues& dlss_queues(VkPhysicalDevice physical, uint32_t host_family, const std::vector<VkQueueFamilyProperties>& families,
                              bool has_optical_flow_ext);
/// The Vulkan 1.2 and 1.3 features Streamline's features asked for, against those the device is made with: the missing ones logged
void dlss_check_features(const VkPhysicalDeviceVulkan12Features& f12, const VkPhysicalDeviceVulkan13Features& f13);
/// After vkCreateDevice + load_device: the device and its queues to Streamline (slSetVulkanInfo), and the six swapchain/present/wait
/// functions routed to its proxies (RX_DLSS_NO_HOOKS=1: not); with DLSS-G available the window surface's two as well. On failure
/// Streamline stops and DLSS stays unavailable (dlss_reason); DLSS-G may be unavailable alone (dlssg_reason)
void dlss_attach(VkInstance instance, VkPhysicalDevice physical, VkDevice device, uint32_t queue_family);
bool dlss_available();       // (started, attached, no evaluation failed)
/// The render size for the menu's DLSS mode (0 Ultra Performance, 1 Performance, 2 Balanced, 3 Quality, 4 DLAA) at an output of
/// ow x oh: slDLSSGetOptimalSettings's, else NVIDIA's standard ratios (1/3, 1/2, 0.58, 2/3, 1)
void dlss_render_size(int mode, uint32_t ow, uint32_t oh, uint32_t& w, uint32_t& h);
const char* dlss_reason();   // (why not, short: the menu and the log)
void dlss_stop();            // (before the device goes; safe when never started)

struct DlssImage {
    VkImage image;
    VkImageView view;
    VkFormat format;
    uint32_t width, height;
    VkImageAspectFlags aspect;
    VkImageUsageFlags usage = 0;   // (what it was made with, for Streamline's tags; 0: what DLSS's evaluation needs)
};
/// The game's camera for the frame's constants (column vectors, m[r*4+c], OpenGL's clip): the view-projection, and its clip to the
/// previous frame's clip (vk_draw.cpp clip_to_previous). Null: identity matrices, as DLSS super resolution alone needs
struct DlssView {
    const float* view_projection = nullptr;
    const float* clip_to_previous = nullptr;
};
/// Records one DLSS evaluation on cmd: color, depth and motion at the render size, output at the output size (STORAGE |
/// SAMPLED | TRANSFER_SRC | TRANSFER_DST, the caller's). All in GENERAL before and after. frame: dlss_frame_id(), whose constants
/// (one slSetConstants a frame, shared with DLSS-G) are set here; with DLSS-G available the depth and motion are tagged for it too.
/// False when Streamline failed (then never again: dlss_available() is false from then on)
bool dlss_evaluate(VkCommandBuffer cmd, const DlssImage& color, const DlssImage& depth, const DlssImage& motion,
                   const DlssImage& output, bool reset, uint32_t frame, const DlssView& view);

// ---- DLSS Frame Generation (DLSS-G), Reflex and PCL, through the same Streamline ----
// Frames are numbered by the game's presents (dlssg_logic.h frame_of): the game's thread counts them (dlssg_game_present) and starts
// each frame at its window acquire (dlssg_frame_start: Reflex's sleep and the first latency markers); the replay counts the presents
// it shows (dlss_note_present), so the frame it records is dlss_frame_id(), whose constants and tags dlss_evaluate sets; the present
// of that frame (vk_replay.cpp present_image) turns generation on or off for it and carries its present markers
// (dlssg_present_begin/end).

bool dlssg_requested();          // (asked for at the start: Streamline loaded DLSS-G, Reflex and PCL with DLSS)
bool dlssg_available();          // (and DLSS-G is supported here, with its functions)
const char* dlssg_reason();      // (why not, short: the menu and the log)
uint32_t dlssg_max_multiplier(); // (the most frames shown per game frame on this GPU: 2 to 6; 0 until DLSS-G says)
bool dlssg_engaged();            // (available and the row at 2x and up: the built-in interpolation stays off)
bool dlssg_generating();         // (the last present went out with generation on)
uint32_t dlss_frame_id();        // (the frame the replay records now: one past the presents it showed)
void dlss_note_present(uint64_t presents);   // (vk_replay.cpp do_present: the replay's presents so far)
void dlssg_game_present();       // (the game's thread, at each of its presents, before the replay gets it)
void dlssg_frame_start();        // (the game's thread at its window acquire, the host's frame_start)
/// A swapchain is waiting to be made again (a resize): true while it must wait for this present, which turns generation off first
bool dlss_defer_swapchain();
void dlss_swapchain_created();   // (create_swapchain: Reflex's mode is the swapchain's; generation waits a few presents)
void dlss_swapchain_changing();  // (create_swapchain, first: generation off at once, before any swapchain is made or destroyed)
void dlssg_window_check();       // (present_image, first: a minimized window turns generation off, even when no present follows)
/// create_swapchain, each swapchain: whether RX_VK_FIFO was set aside because DLSS-G runs (VSync with frame generation is D3D12's
/// only). Set aside: Reflex's frame limit holds what is shown at 97% of the monitor's refresh (RX_DLSSG_LIMIT); taken again (DLSS
/// failed or went off): no limit
void dlssg_fifo_state(bool ignored);
/// A game frame's present, after its submit and before vkQueuePresentKHR (on the presenting thread): generation on or off for it,
/// null tags and neutral constants when DLSS did not draw it, the game's part of the window image when there are bars, and the
/// render-submit-end and present-start markers. image: the game handed a picture; x, y, w, h: where it is in the sw x sh image
void dlssg_present_begin(uint32_t frame, bool image, int x, int y, uint32_t w, uint32_t h, uint32_t sw, uint32_t sh);
/// After it: the present-end marker, what DLSS-G showed (the FPS counter's frames), its status, the log every 600 presents
void dlssg_present_end(uint32_t frame);

} // namespace mkwvk
