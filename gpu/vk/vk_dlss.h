// NVIDIA DLSS super resolution through NVIDIA Streamline (vk_dlss.cpp), manual hooking. Only Streamline's MIT headers are
// built in (third_party/streamline); sl.interposer.dll and NVIDIA's DLSS are loaded from the user's PC at run time and none
// ship with Hookshot. Ported from Lichii (Eden lineage; Duck912's Eglantine work), GPL-3.0-or-later.
#pragma once

#include <cstdint>
#include <vector>

#include "vk_loader.h"

namespace mkwvk {

/// At start, before create_instance: sl.interposer.dll from RX_DLSS_DIR, else <exe dir>\streamline\ (signatures checked;
/// the NVIDIA app's newest DLSS when newer than the nvngx_dlss.dll there, or there is none), slInit with manual hooking.
/// True when started
bool dlss_start();
const std::vector<const char*>& dlss_instance_exts();   // (empty unless started)
const std::vector<const char*>& dlss_device_exts();     // (empty unless started)
/// After vkCreateDevice + load_device: the device to Streamline, and the six swapchain/present/wait functions routed to
/// its proxies (RX_DLSS_NO_HOOKS=1: not). On failure Streamline stops and DLSS stays unavailable (dlss_reason)
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
};
/// Records one DLSS evaluation on cmd: color, depth and motion at the render size, output at the output size (STORAGE |
/// SAMPLED | TRANSFER_SRC | TRANSFER_DST, the caller's). All in GENERAL before and after. False when Streamline failed (then
/// never again: dlss_available() is false from then on)
bool dlss_evaluate(VkCommandBuffer cmd, const DlssImage& color, const DlssImage& depth, const DlssImage& motion,
                   const DlssImage& output, bool reset, uint32_t frame_index);

} // namespace mkwvk
