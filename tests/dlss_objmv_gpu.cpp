// The motion pass's real shader (gpu/vk/shaders/dlss_motion_comp_spv.h, built from dlss_motion.comp) run on a Vulkan device against its
// CPU model (gpu/vk/dlss_objmv.h) on the same drawn frames (tests/objmv_scene.h): the vectors, the distances kept for the next frame,
// which pixels were made still, and the counts. Needs a GPU (tests/run_all.py skips it; run it by name). No game.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include <cstdint>
#include <cstring>

#include "objmv_scene.h"
#include "shaders/dlss_motion_comp_spv.h"

using namespace objmv_scene;

static int failures = 0;
#define CHECK(x)                                                                    \
    do {                                                                            \
        if (!(x)) std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x), ++failures; \
    } while (0)

#define FNS(X)                                                                                                                        \
    X(vkCreateInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties)    \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFeatures) X(vkCreateDevice) X(vkGetDeviceQueue) X(vkCreateImage)      \
    X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkBindImageMemory) X(vkCreateImageView) X(vkCreateBuffer)                   \
    X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateSampler) X(vkCreateDescriptorSetLayout)          \
    X(vkCreatePipelineLayout) X(vkCreateShaderModule) X(vkCreateComputePipelines) X(vkCreateDescriptorPool)                          \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer)  \
    X(vkEndCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdBindPipeline)           \
    X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDispatch) X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkDestroyDevice)          \
    X(vkDestroyInstance)
#define DECL(f) static PFN_##f f;
FNS(DECL)

static VkPhysicalDeviceMemoryProperties mem_props;
static VkDevice device;

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++)
        if ((bits >> i & 1) && (mem_props.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}
struct Img {
    VkImage image;
    VkImageView view;
};
static Img image(VkFormat f, VkImageUsageFlags usage) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D, ci.format = f, ci.extent = {(uint32_t)W, (uint32_t)H, 1}, ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT, ci.tiling = VK_IMAGE_TILING_OPTIMAL, ci.usage = usage;
    Img i{};
    vkCreateImage(device, &ci, nullptr, &i.image);
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(device, i.image, &r);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = r.size, ai.memoryTypeIndex = memory_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory m;
    vkAllocateMemory(device, &ai, nullptr, &m);
    vkBindImageMemory(device, i.image, m, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = i.image, vi.viewType = VK_IMAGE_VIEW_TYPE_2D, vi.format = f, vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(device, &vi, nullptr, &i.view);
    return i;
}
struct Buf {
    VkBuffer buffer;
    void* map;
};
static Buf buffer(VkDeviceSize size, VkBufferUsageFlags usage) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size, bi.usage = usage;
    Buf b{};
    vkCreateBuffer(device, &bi, nullptr, &b.buffer);
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(device, b.buffer, &r);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = r.size;
    ai.memoryTypeIndex = memory_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory m;
    vkAllocateMemory(device, &ai, nullptr, &m);
    vkBindBufferMemory(device, b.buffer, m, 0);
    vkMapMemory(device, m, 0, VK_WHOLE_SIZE, 0, &b.map);
    return b;
}
static void layout(VkCommandBuffer cb, VkImage i, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from, b.newLayout = to, b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = i, b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}
static float half_to_float(uint16_t h) {
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? std::ldexp((float)m, -24) : e == 31 ? (m ? NAN : INFINITY) : std::ldexp((float)(m | 1024), (int)e - 25);
    return s ? -v : v;
}

struct Gpu {
    VkQueue queue;
    VkCommandPool pool;
    VkPipeline pipe;
    VkPipelineLayout pl;
    VkDescriptorSet set;
    Img depth, motion, prev, out, mask;
    Buf up, down, counts;
    VkSampler sampler;
};

/// One frame through the shader: the checked frame's inputs of r, the outputs compared with the model's
static void compare(Gpu& g, const char* name, const Run& r, bool on, bool prev_ok) {
    const size_t px = (size_t)W * H;
    std::memcpy(g.up.map, r.depth1.data(), px * 4);
    std::memcpy((char*)g.up.map + px * 4, r.dist0.data(), px * 4);
    std::memset(g.counts.map, 0, 16);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = g.pool, ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(device, &ai, &cb);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cb, &bi);
    for (VkImage i : {g.depth.image, g.prev.image}) layout(cb, i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, c.imageExtent = {(uint32_t)W, (uint32_t)H, 1};
    vkCmdCopyBufferToImage(cb, g.up.buffer, g.depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    c.bufferOffset = px * 4;
    vkCmdCopyBufferToImage(cb, g.up.buffer, g.prev.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    for (VkImage i : {g.depth.image, g.prev.image}) layout(cb, i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    for (VkImage i : {g.motion.image, g.out.image, g.mask.image}) layout(cb, i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    float push[28] = {};
    std::copy_n(r.clip, 16, push);   // (vk_draw.cpp dlss_motion_pass: the clip-to-previous, a, b, on, prev_ok, the jitter)
    push[16] = r.a1, push[17] = r.b1, push[18] = on ? 2.0f : 0.0f, push[19] = prev_ok ? 1.0f : 0.0f;   // (2: on, and counted)
    push[20] = r.jx, push[21] = r.jy, push[22] = r.mask_value, push[23] = r.keep ? 1.0f : 0.0f, push[24] = r.jx0, push[25] = r.jy0;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g.pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g.pl, 0, 1, &g.set, 0, nullptr);
    vkCmdPushConstants(cb, g.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
    vkCmdDispatch(cb, (W + 7) / 8, (H + 7) / 8, 1);
    for (VkImage i : {g.motion.image, g.out.image, g.mask.image}) layout(cb, i, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    c.bufferOffset = 0;
    vkCmdCopyImageToBuffer(cb, g.motion.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.down.buffer, 1, &c);
    c.bufferOffset = px * 4;
    vkCmdCopyImageToBuffer(cb, g.out.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.down.buffer, 1, &c);
    c.bufferOffset = px * 8;
    vkCmdCopyImageToBuffer(cb, g.mask.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.down.buffer, 1, &c);
    VkMemoryBarrier hb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    hb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, nullptr, 0, nullptr);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1, si.pCommandBuffers = &cb;
    vkQueueSubmit(g.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(g.queue);

    const uint16_t* mv = (const uint16_t*)g.down.map;
    const float* dist = (const float*)((const char*)g.down.map + px * 4);
    const uint32_t* n = (const uint32_t*)g.counts.map;
    const uint16_t* mask = (const uint16_t*)((const char*)g.down.map + px * 8);
    size_t bad_mv = 0, bad_still = 0, bad_dist = 0, bad_mask = 0;
    float worst = 0;
    for (size_t i = 0; i < px; i++) {
        const float gx = half_to_float(mv[i * 2]), gy = half_to_float(mv[i * 2 + 1]), cx = r.mv[i * 2], cy = r.mv[i * 2 + 1];
        const float e = std::max(std::fabs(gx - cx), std::fabs(gy - cy)), tol = std::max(0.02f, 2e-3f * std::hypot(cx, cy));
        worst = std::max(worst, e);
        bad_mv += e > tol;
        bad_still += (gx == 0 && gy == 0) != (cx == 0 && cy == 0);
        if (on) bad_dist += std::fabs(dist[i] - r.dist[i]) > 1e-5f * std::max(1.0f, r.dist[i]);
        if (r.mask_value > 0) bad_mask += std::fabs(half_to_float(mask[i]) - r.mask[i]) > 1e-3f;   // (R16F)
    }
    const objmv::Counts& m = r.counts;
    const auto close = [](uint32_t a, uint32_t b) { return (a > b ? a - b : b - a) * 200u <= std::max(a, b) + 200u; };   // (0.5%)
    std::printf("gpu %-12s vectors off %zu (worst %.4f px), stillness differs %zu, distances off %zu, mask off %zu; moved %u/%u held %u/%u "
                "checked %u/%u (shader/model)\n", name, bad_mv, worst, bad_still, bad_dist, bad_mask, n[0], m.moved, n[1], m.held, n[2],
                m.checked);
    CHECK(bad_mv * 1000 <= px);       // (at most 0.1%: float against half, and a decision at the tolerance's edge)
    CHECK(bad_still * 1000 <= px);
    CHECK(bad_dist == 0);
    CHECK(bad_mask * 1000 <= px);
    CHECK(close(n[0], m.moved) && close(n[1], m.held) && close(n[2], m.checked));
}

int main() {
#ifdef _WIN32
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    auto gipa = lib ? (PFN_vkGetInstanceProcAddr)(void*)GetProcAddress(lib, "vkGetInstanceProcAddr") : nullptr;
#else
    void* lib = dlopen("libvulkan.so.1", RTLD_NOW);
    auto gipa = lib ? (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr") : nullptr;
#endif
    if (!gipa) return std::printf("no Vulkan loader: skipped\n"), 0;
    vkCreateInstance = (PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance inst;
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) return std::printf("no Vulkan 1.3 instance: skipped\n"), 0;
#define LOAD(f) f = (PFN_##f)gipa(inst, #f);
    FNS(LOAD)
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(inst, &count, nullptr);
    std::vector<VkPhysicalDevice> all(count);
    vkEnumeratePhysicalDevices(inst, &count, all.data());
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props{};
    for (VkPhysicalDevice p : all) {   // (a discrete GPU first)
        VkPhysicalDeviceProperties q;
        vkGetPhysicalDeviceProperties(p, &q);
        if (!phys || q.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) phys = p, props = q;
    }
    if (!phys) return std::printf("no Vulkan device: skipped\n"), 0;
    vkGetPhysicalDeviceMemoryProperties(phys, &mem_props);
    VkPhysicalDeviceFeatures have{};
    vkGetPhysicalDeviceFeatures(phys, &have);
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> fams(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, fams.data());
    uint32_t family = 0;
    while (family < nq && !(fams[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) family++;
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family, qi.queueCount = 1, qi.pQueuePriorities = &prio;
    VkPhysicalDeviceFeatures want{};
    want.shaderStorageImageExtendedFormats = have.shaderStorageImageExtendedFormats;   // (rg16f, as Hookshot's device has it)
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1, dci.pQueueCreateInfos = &qi, dci.pEnabledFeatures = &want;
    if (vkCreateDevice(phys, &dci, nullptr, &device) != VK_SUCCESS) return std::printf("device not made: skipped\n"), 0;
    std::printf("device: %s\n", props.deviceName);

    Gpu g{};
    vkGetDeviceQueue(device, family, 0, &g.queue);
    g.depth = image(VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    g.motion = image(VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    g.prev = image(VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    g.out = image(VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    g.mask = image(VK_FORMAT_R16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    g.up = buffer((VkDeviceSize)W * H * 8, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    g.down = buffer((VkDeviceSize)W * H * 10, VK_BUFFER_USAGE_TRANSFER_DST_BIT);   // (motion, distances, the mask's halves)
    g.counts = buffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};   // (vk_draw.cpp's ds.nearest)
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(device, &sci, nullptr, &g.sampler);
    // (vk_draw.cpp dlss_pipelines: the motion pass's six bindings and 112 bytes pushed; a plain set here, a pushed one there)
    VkDescriptorSetLayoutBinding b[6] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                         {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                         {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                         {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                         {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                         {5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 6, li.pBindings = b;
    VkDescriptorSetLayout sl;
    vkCreateDescriptorSetLayout(device, &li, nullptr, &sl);
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 112};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1, pli.pSetLayouts = &sl, pli.pushConstantRangeCount = 1, pli.pPushConstantRanges = &pr;
    vkCreatePipelineLayout(device, &pli, nullptr, &g.pl);
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof DLSS_MOTION_COMP_SPV, mi.pCode = DLSS_MOTION_COMP_SPV;
    VkShaderModule mod;
    CHECK(vkCreateShaderModule(device, &mi, nullptr, &mod) == VK_SUCCESS);
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", nullptr};
    cpi.layout = g.pl;
    CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, nullptr, &g.pipe) == VK_SUCCESS);
    VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 1, dpi.poolSizeCount = 3, dpi.pPoolSizes = ps;
    VkDescriptorPool dp;
    vkCreateDescriptorPool(device, &dpi, nullptr, &dp);
    VkDescriptorSetAllocateInfo sai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    sai.descriptorPool = dp, sai.descriptorSetCount = 1, sai.pSetLayouts = &sl;
    vkAllocateDescriptorSets(device, &sai, &g.set);
    const VkDescriptorImageInfo ii[6] = {{g.sampler, g.depth.view, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, g.motion.view, VK_IMAGE_LAYOUT_GENERAL},
                                         {VK_NULL_HANDLE, g.prev.view, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, g.out.view, VK_IMAGE_LAYOUT_GENERAL},
                                         {}, {VK_NULL_HANDLE, g.mask.view, VK_IMAGE_LAYOUT_GENERAL}};
    const VkDescriptorBufferInfo cbi{g.counts.buffer, 0, 16};
    VkWriteDescriptorSet wr[6];
    for (uint32_t i = 0; i < 6; i++) {
        wr[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wr[i].dstSet = g.set, wr[i].dstBinding = i, wr[i].descriptorCount = 1, wr[i].descriptorType = b[i].descriptorType;
        if (i != 4) wr[i].pImageInfo = &ii[i];
        else wr[i].pBufferInfo = &cbi;
    }
    vkUpdateDescriptorSets(device, 6, wr, 0, nullptr);
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.queueFamilyIndex = family;
    vkCreateCommandPool(device, &cpci, nullptr, &g.pool);

    // the scenes of tests/dlss_objmv_test.cpp
    Scene r0, r1;
    r0.has_box = r1.has_box = true;
    r0.box = {{-0.5, 0.0, 3.5}, {0.5, 1.8, 4.5}}, r1.box = {{-0.5, 0.0, 3.0}, {0.5, 1.8, 4.0}};
    const Cam rc0{{0, 1.2, 8.0}, {0, 1.0, -20}}, rc1{{0, 1.2, 7.5}, {0, 1.0, -20.5}};
    compare(g, "runner", two_frames(r0, rc0, r1, rc1), true, true);
    Scene t0, t1;
    t0.has_box = t1.has_box = true;
    t0.box = {{-0.6, 0.0, 3.8}, {0.6, 1.8, 4.0}}, t1.box = {{-0.3, 0.0, 3.8}, {0.9, 1.8, 4.0}};
    const Cam tc0{{0, 1.0, 8.0}, {0, 1.0, -20}}, tc1{{0.3, 1.0, 8.0}, {0.3, 1.0, -20}};
    compare(g, "strafe", two_frames(t0, tc0, t1, tc1), true, true);
    Scene s;
    s.has_box = true;
    s.box = {{-2.0, 0.0, -6.0}, {1.0, 3.0, -4.0}};
    const Cam sc0{{0, 1.5, 6.0}, {0, 1.2, -20}}, sc1{{0.25, 1.55, 5.7}, {1.5, 1.0, -20}};
    compare(g, "still world", two_frames(s, sc0, s, sc1), true, true);
    Scene k;   // (sky_turn: the cleared depth behind, a quick turn; the shader's SKY_DEPTH against the model's kSkyDepth)
    k.has_wall = false, k.has_box = true;
    k.box = {{-1.0, 0.0, -10.0}, {1.0, 2.0, -8.0}};
    const Cam kc0{{0, 1.5, 6.0}, {0, 1.2, -20}}, kc1{{0, 1.5, 6.0}, {std::tan(8.0 * 3.14159265358979 / 180.0) * 26.0, 1.2, -20}};
    compare(g, "sky turn", two_frames(k, kc0, k, kc1), true, true);
    compare(g, "off", two_frames(r0, rc0, r1, rc1, false, true), false, true);
    compare(g, "first frame", two_frames(r0, rc0, r1, rc1, true, false), true, false);
    // (DLSS's jitter: the scene drawn a sub-pixel off, the shader taking each pixel where an unjittered draw puts it, as the model)
    compare(g, "jittered", two_frames(r0, rc0, r1, rc1, true, true, 0.375f, -0.2777f), true, true);
    // (DLSS's bias mask: RX_DLSS_OBJMV 2, the camera's vectors kept, and 3, own motion too)
    compare(g, "mask kept", two_frames(r0, rc0, r1, rc1, true, true, 0, 0, 0.65f, true), true, true);
    compare(g, "mask+motion", two_frames(r0, rc0, r1, rc1, true, true, 0, 0, 0.65f, false), true, true);
    // (both frames jittered, each its own: the check's looks into last frame's distances move by the difference)
    compare(g, "both jittered", two_frames(r0, rc0, r1, rc1, true, true, 0.375f, -0.2777f, 0.65f, false, -0.25f, 0.1111f), true, true);
    // (the camera walking at the wall: the vectors grow across the screen, so a shader taking the jitter the wrong way is 0.4 px off,
    // far over the half-float tolerance; on the scenes above it was within it)
    Scene wall;
    wall.has_box = false;
    const Cam wc0{{0, 1.5, 6.0}, {0, 1.2, -60}}, wc1{{0.4, 1.6, -14.0}, {0.6, 1.3, -60}};
    compare(g, "jittered walk", two_frames(wall, wc0, wall, wc1, false, true, 0.375f, -0.2777f), false, true);
    // (the looks with last frame's jitter and this frame's, where the 3x3 closest cannot hide them: objmv_scene.h look_offset)
    compare(g, "look prev x", look_offset(0.9f, 0, 0, 0, false), true, true);
    compare(g, "look this x", look_offset(0, 0, -0.9f, 0, false), true, true);
    compare(g, "look prev y", look_offset(0, 0.9f, 0, 0, true), true, true);
    compare(g, "look this y", look_offset(0, 0, 0, -0.9f, true), true, true);

    vkDestroyDevice(device, nullptr);   // (the rest goes with the process)
    vkDestroyInstance(inst, nullptr);
    failures += scene_failures;
    std::printf("dlss own motion on the GPU: %d failures\n", failures);
    return failures ? 1 : 0;
}
