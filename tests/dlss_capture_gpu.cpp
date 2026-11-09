// DLSS's captured motion on a Vulkan device (gpu/vk/spirv_xfb.h, gpu/vk/shaders/dlss_capture.vert with dlss_actor.frag), the way
// vk_draw.cpp draws it: a vertex stage patched to record its clip position by transform feedback, run as the game's draw is;
// then this frame's and the frame before's records drawn again with the scene's viewport (negative height, the jitter's
// offset). Three parts:
//  1. the game translator's shape of a Position output (a variable of its own; assembled here), patched: the driver takes it
//     and records exactly the positions the shader writes;
//  2. glslang's shape (gl_PerVertex), a "wind" shader (args: its SPIR-V), patched: records what the shader computes;
//  3. a tilted patch of grass swaying (the wind uniform differs between two frames, the camera moves): recorded in both
//     frames, the scene's depth drawn with this frame's, and the motion pass's vectors against the camera motion pass's CPU
//     model (dlss_objmv.h motion_pass) of a world that moved by the wind's change; then wrong wirings that must fail.
// Needs a GPU with transform feedback, 64-bit shader integers and dynamic rendering (tests/run_all.py skips it; run it by name).
#define _CRT_SECURE_NO_WARNINGS   // (fopen of the shader file the runner made)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "dlss_actor.h"
#include "dlss_actor_plan.h"
#include "dlss_objmv.h"
#include "shaders/dlss_actor_frag_spv.h"
#include "shaders/dlss_capture_vert_spv.h"
#include "spirv_xfb.h"

namespace objmv = mkwvk::objmv;

static int failures = 0;
#define CHECK(x)                                                                    \
    do {                                                                            \
        if (!(x)) std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x), ++failures; \
    } while (0)

#define FNS(X)                                                                                                                       \
    X(vkCreateInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties)   \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFeatures2) X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice)  \
    X(vkGetDeviceProcAddr) X(vkGetDeviceQueue) X(vkCreateImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory)                 \
    X(vkBindImageMemory) X(vkCreateImageView) X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory)              \
    X(vkMapMemory) X(vkCreateSampler) X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkCreateShaderModule)              \
    X(vkCreateGraphicsPipelines) X(vkCreateDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets)                    \
    X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdPipelineBarrier)        \
    X(vkCmdClearColorImage) X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets)             \
    X(vkCmdPushConstants) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdBindVertexBuffers)         \
    X(vkCmdBindIndexBuffer) X(vkCmdCopyImageToBuffer) X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkDestroyDevice) X(vkDestroyInstance)
#define DECL(f) static PFN_##f f;
FNS(DECL)
static PFN_vkCmdBindTransformFeedbackBuffersEXT vkCmdBindTransformFeedbackBuffersEXT;
static PFN_vkCmdBeginTransformFeedbackEXT vkCmdBeginTransformFeedbackEXT;
static PFN_vkCmdEndTransformFeedbackEXT vkCmdEndTransformFeedbackEXT;

constexpr int W = 256, H = 160;
constexpr double kPi = 3.14159265358979323846;
constexpr float kSentinel[2] = {100.0f, -50.0f};

// ---- matrices (row major, column vectors), as the actor fixture
using M4 = std::array<double, 16>;
static M4 mul(const M4& a, const M4& b) {
    M4 r{};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 4; k++) r[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
    return r;
}
static M4 translate(double x, double y, double z) { return {1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z, 0, 0, 0, 1}; }
static M4 rot(int axis, double deg) {
    const double c = std::cos(deg * kPi / 180), s = std::sin(deg * kPi / 180);
    if (axis == 0) return {1, 0, 0, 0, 0, c, -s, 0, 0, s, c, 0, 0, 0, 0, 1};
    if (axis == 1) return {c, 0, s, 0, 0, 1, 0, 0, -s, 0, c, 0, 0, 0, 0, 1};
    return {c, -s, 0, 0, s, c, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}
static M4 inverse(const M4& m) {
    double a[4][8]{};
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) a[i][j] = m[i * 4 + j];
        a[i][i + 4] = 1;
    }
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++)
            if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        for (int k = 0; k < 8; k++) std::swap(a[c][k], a[p][k]);
        const double d = a[c][c];
        for (double& v : a[c]) v /= d;
        for (int r = 0; r < 4; r++)
            if (r != c) {
                const double f = a[r][c];
                for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k];
            }
    }
    M4 r{};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) r[i * 4 + j] = a[i][j + 4];
    return r;
}
static std::array<double, 4> apply(const M4& m, double x, double y, double z) {
    std::array<double, 4> r{};
    for (int i = 0; i < 4; i++) r[i] = m[i * 4] * x + m[i * 4 + 1] * y + m[i * 4 + 2] * z + m[i * 4 + 3];
    return r;
}
static M4 perspective(double fovy, double aspect, double n, double f) {
    const double t = 1 / std::tan(fovy * kPi / 360);
    return {t / aspect, 0, 0, 0, 0, t, 0, 0, 0, 0, (f + n) / (n - f), 2 * f * n / (n - f), 0, 0, -1, 0};
}
static void to_float(const M4& m, float* out) {
    for (int i = 0; i < 16; i++) out[i] = (float)m[i];
}
static float half_to_float(uint16_t h) {
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? std::ldexp((float)m, -24) : e == 31 ? (m ? NAN : INFINITY) : std::ldexp((float)(m | 1024), (int)e - 25);
    return s ? -v : v;
}

// ---- the game translator's shape: gl_Position = in0, Position a variable of its own (eden's DefineOutput)
static std::vector<uint32_t> plain_position_shader() {
    const auto ins = [](std::vector<uint32_t>& m, uint32_t op, std::initializer_list<uint32_t> operands) {
        m.push_back((uint32_t)(operands.size() + 1) << 16 | op);
        m.insert(m.end(), operands);
    };
    enum : uint32_t { main_ = 1, void_, fn, f32, v4, pout, pin, pos, inp, label, x, bound };
    std::vector<uint32_t> m = {0x07230203, 0x00010000, 0, bound, 0};
    ins(m, 17, {1});                                   // OpCapability Shader
    ins(m, 14, {0, 1});                                // OpMemoryModel Logical GLSL450
    ins(m, 15, {0, main_, 0x6e69616d, 0, pos, inp});   // OpEntryPoint Vertex %main "main" %pos %inp
    ins(m, 71, {pos, 11, 0});                          // OpDecorate %pos BuiltIn Position
    ins(m, 71, {inp, 30, 0});                          // OpDecorate %inp Location 0
    ins(m, 19, {void_});
    ins(m, 33, {fn, void_});
    ins(m, 22, {f32, 32});
    ins(m, 23, {v4, f32, 4});
    ins(m, 32, {pout, 3, v4});
    ins(m, 32, {pin, 1, v4});
    ins(m, 59, {pout, pos, 3});
    ins(m, 59, {pin, inp, 1});
    ins(m, 54, {void_, main_, 0, fn});
    ins(m, 248, {label});
    ins(m, 61, {v4, x, inp});
    ins(m, 62, {pos, x});
    ins(m, 253, {});
    ins(m, 56, {});
    return m;
}

// ---- Vulkan
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
static Img image(VkFormat f, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
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
    vi.image = i.image, vi.viewType = VK_IMAGE_VIEW_TYPE_2D, vi.format = f, vi.subresourceRange = {aspect, 0, 1, 0, 1};
    vkCreateImageView(device, &vi, nullptr, &i.view);
    return i;
}
struct Buf {
    VkBuffer buffer;
    uint8_t* map;
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
    void* p = nullptr;
    vkMapMemory(device, m, 0, VK_WHOLE_SIZE, 0, &p);
    b.map = (uint8_t*)p;
    std::memset(b.map, 0, size);
    return b;
}
static void layout(VkCommandBuffer cb, VkImage i, VkImageAspectFlags aspect) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = i, b.subresourceRange = {aspect, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}
static void full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier m{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    m.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, m.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &m, 0, nullptr, 0, nullptr);
}
static VkShaderModule module(const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = bytes, mi.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &mi, nullptr, &m);
    return m;
}

struct Gpu {
    VkQueue queue;
    VkCommandPool pool;
};
static VkCommandBuffer begin(Gpu& g) {
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = g.pool, cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, cai.commandBufferCount = 1;
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(device, &cai, &cb);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}
static void submit(Gpu& g, VkCommandBuffer cb) {
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1, si.pCommandBuffers = &cb;
    vkQueueSubmit(g.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(g.queue);
}

/// A pipeline of the "game's" kind: a vertex stage (and a fragment stage or none), vec3/vec4 positions at location 0, a uniform
/// buffer at binding 0 (wind) when asked, rasterizer discard as asked; depth D32 attachment or none; the motion pass's when
/// motion is asked (dlss_capture.vert + dlss_actor.frag, R16G16F)
struct PipeOpts {
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    uint32_t vec = 4;   // (the position's components)
    bool discard = false, depth = false, motion = false;
};
static VkPipeline pipeline(const PipeOpts& o) {
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, o.vs, "main", nullptr};
    st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, o.fs, "main", nullptr};
    const VkVertexInputBindingDescription vb{0, o.vec * 4, VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription va{0, 0, o.vec == 4 ? VK_FORMAT_R32G32B32A32_SFLOAT : VK_FORMAT_R32G32B32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (!o.motion) vi.vertexBindingDescriptionCount = 1, vi.pVertexBindingDescriptions = &vb, vi.vertexAttributeDescriptionCount = 1,
                   vi.pVertexAttributeDescriptions = &va;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL, rs.cullMode = VK_CULL_MODE_NONE, rs.lineWidth = 1.0f, rs.rasterizerDiscardEnable = o.discard;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo dsi{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    dsi.depthTestEnable = dsi.depthWriteEnable = o.depth, dsi.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = o.motion ? 1 : 0, cb.pAttachments = &ba;
    const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2, dy.pDynamicStates = dyn;
    const VkFormat motion = VK_FORMAT_R16G16_SFLOAT;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    if (o.motion) ri.colorAttachmentCount = 1, ri.pColorAttachmentFormats = &motion;
    if (o.depth) ri.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &ri, gi.stageCount = o.fs ? 2 : 1, gi.pStages = st;
    gi.pVertexInputState = &vi, gi.pInputAssemblyState = &ia, gi.pViewportState = &vp, gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms, gi.pDepthStencilState = o.depth ? &dsi : nullptr, gi.pColorBlendState = &cb, gi.pDynamicState = &dy;
    gi.layout = o.layout;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gi, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    return p;
}

/// A draw recorded by transform feedback: the captures buffer from byte at, as many vertices as the draw makes
static void capture(VkCommandBuffer cb, VkPipeline p, VkBuffer captures, VkDeviceSize at, VkDeviceSize bytes,
                    const std::function<void()>& draw) {
    const VkRect2D area{{0, 0}, {(uint32_t)W, (uint32_t)H}};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = area, ri.layerCount = 1;
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    const VkViewport v{0, (float)H, (float)W, -(float)H, 0.0f, 1.0f};
    vkCmdSetViewport(cb, 0, 1, &v);
    vkCmdSetScissor(cb, 0, 1, &area);
    vkCmdBindTransformFeedbackBuffersEXT(cb, 0, 1, &captures, &at, &bytes);
    vkCmdBeginTransformFeedbackEXT(cb, 0, 0, nullptr, nullptr);
    draw();
    vkCmdEndTransformFeedbackEXT(cb, 0, 0, nullptr, nullptr);
    vkCmdEndRendering(cb);
}

// ---- the scene of part 3: a tilted patch of grass, two frames of wind and camera
static const M4& camera_now() {
    static const M4 c = perspective(60, (double)W / H, 0.5, 100);
    return c;
}
static const M4& camera_before() {
    static const M4 c = mul(perspective(60, (double)W / H, 0.5, 100), inverse(mul(translate(-0.1, 0.05, 0.12), rot(1, 1.0))));
    return c;
}
struct Grass {
    std::vector<float> xyz;      // (vec3 a vertex)
    std::vector<uint16_t> idx;
};
/// A 3.2 x 2.4 m patch (times scale) of 6 x 6 quads, placed in the world: rows of triangles from the bottom
static Grass grass(const M4& place = mul(translate(-0.3, -0.2, -6.0), rot(1, 35)), double scale = 1.0) {
    constexpr int n = 6;
    Grass g;
    for (int i = 0; i <= n; i++)
        for (int j = 0; j <= n; j++) {
            const auto w = apply(place, ((double)j / n - 0.5) * 3.2 * scale, ((double)i / n - 0.5) * 2.4 * scale, 0);
            for (int k = 0; k < 3; k++) g.xyz.push_back((float)w[k]);
        }
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            const uint16_t a = (uint16_t)(i * (n + 1) + j), b = a + 1, c = (uint16_t)(a + n + 1), e = c + 1;
            for (uint16_t x : {a, b, e, a, e, c}) g.idx.push_back(x);
        }
    return g;
}
struct Wind {   // (the uniform buffer of the wind shader, std140: the view-projection column major, the wind's offset)
    float vp[16];
    float wind[4];
};
static Wind wind_of(const M4& vp, double wx, double wy, double wz) {
    Wind u{};
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) u.vp[c * 4 + r] = (float)vp[r * 4 + c];
    u.wind[0] = (float)wx, u.wind[1] = (float)wy, u.wind[2] = (float)wz;
    return u;
}

static std::vector<uint32_t> read_words(const char* path) {
    std::vector<uint32_t> w;
    if (FILE* f = std::fopen(path, "rb")) {
        uint32_t x;
        while (std::fread(&x, 4, 1, f) == 1) w.push_back(x);
        std::fclose(f);
    }
    return w;
}

int main(int argc, char** argv) {
    if (argc != 2) return std::printf("usage: dlss_capture_gpu <wind.vert.spv>\n"), 2;
    const std::vector<uint32_t> wind_spv = read_words(argv[1]);
    if (wind_spv.empty()) return std::printf("no %s\n", argv[1]), 2;
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
    for (VkPhysicalDevice p : all) {
        VkPhysicalDeviceProperties q;
        vkGetPhysicalDeviceProperties(p, &q);
        if (!phys || q.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) phys = p, props = q;
    }
    if (!phys) return std::printf("no Vulkan device: skipped\n"), 0;
    vkGetPhysicalDeviceMemoryProperties(phys, &mem_props);
    uint32_t next = 0;
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &next, nullptr);
    std::vector<VkExtensionProperties> exts(next);
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &next, exts.data());
    const bool has_xfb = std::any_of(exts.begin(), exts.end(), [](const VkExtensionProperties& e) {
        return !std::strcmp(e.extensionName, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME);
    });
    VkPhysicalDeviceTransformFeedbackFeaturesEXT htf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT};
    VkPhysicalDeviceVulkan13Features h13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    h13.pNext = &htf;
    VkPhysicalDeviceFeatures2 have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    have.pNext = &h13;
    vkGetPhysicalDeviceFeatures2(phys, &have);
    if (!has_xfb || !htf.transformFeedback || !h13.dynamicRendering || !have.features.shaderInt64)
        return std::printf("%s: no transform feedback, dynamic rendering or 64-bit integers: skipped\n", props.deviceName), 0;
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> fams(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, fams.data());
    uint32_t family = 0;
    while (family < nq && !(fams[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) family++;
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family, qi.queueCount = 1, qi.pQueuePriorities = &prio;
    VkPhysicalDeviceTransformFeedbackFeaturesEXT wtf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT};
    wtf.transformFeedback = VK_TRUE;
    VkPhysicalDeviceVulkan13Features w13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    w13.dynamicRendering = VK_TRUE, w13.pNext = &wtf;
    VkPhysicalDeviceFeatures2 want{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    want.features.shaderInt64 = VK_TRUE, want.pNext = &w13;
    const char* ext = VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &want, dci.queueCreateInfoCount = 1, dci.pQueueCreateInfos = &qi, dci.enabledExtensionCount = 1, dci.ppEnabledExtensionNames = &ext;
    if (vkCreateDevice(phys, &dci, nullptr, &device) != VK_SUCCESS) return std::printf("device not made: skipped\n"), 0;
    vkCmdBindTransformFeedbackBuffersEXT = (PFN_vkCmdBindTransformFeedbackBuffersEXT)vkGetDeviceProcAddr(device, "vkCmdBindTransformFeedbackBuffersEXT");
    vkCmdBeginTransformFeedbackEXT = (PFN_vkCmdBeginTransformFeedbackEXT)vkGetDeviceProcAddr(device, "vkCmdBeginTransformFeedbackEXT");
    vkCmdEndTransformFeedbackEXT = (PFN_vkCmdEndTransformFeedbackEXT)vkGetDeviceProcAddr(device, "vkCmdEndTransformFeedbackEXT");
    std::printf("device: %s\n", props.deviceName);
    Gpu g{};
    vkGetDeviceQueue(device, family, 0, &g.queue);
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.queueFamilyIndex = family;
    vkCreateCommandPool(device, &cpi, nullptr, &g.pool);
    const VkBufferUsageFlags kCapture = VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

    // ---- 1. the game translator's shape: the driver takes the patched module and records exactly what it writes
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout empty_layout;
    vkCreatePipelineLayout(device, &pli, nullptr, &empty_layout);
    {
        std::vector<uint32_t> xfb;
        std::string why;
        const bool patched = spirv_xfb::patch(plain_position_shader(), xfb, &why);
        CHECK(patched);
        PipeOpts o;
        o.vs = module(xfb.data(), xfb.size() * 4), o.layout = empty_layout, o.vec = 4, o.discard = true;
        const VkPipeline p = o.vs ? pipeline(o) : VK_NULL_HANDLE;
        CHECK(p != VK_NULL_HANDLE);
        Buf vin = buffer(6 * 16, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT), cap = buffer(4096, kCapture);
        float* v = (float*)vin.map;
        for (int i = 0; i < 6; i++) v[i * 4] = 0.1f * i, v[i * 4 + 1] = -0.2f * i, v[i * 4 + 2] = 0.3f + 0.01f * i, v[i * 4 + 3] = 1.0f + i;
        if (p) {
            VkCommandBuffer cb = begin(g);
            capture(cb, p, cap.buffer, 0, 6 * 16, [&] {
                const VkDeviceSize zero = 0;
                vkCmdBindVertexBuffers(cb, 0, 1, &vin.buffer, &zero);
                vkCmdDraw(cb, 6, 1, 0, 0);
            });
            submit(g, cb);
        }
        const bool same = !std::memcmp(cap.map, vin.map, 6 * 16);
        std::printf("1. the translator's shape: %s, %s\n", p ? "the driver took the patched module" : "NOT TAKEN",
                    same ? "recorded exactly" : "RECORDED WRONG");
        CHECK(same);
    }

    // ---- 2. glslang's block shape, a wind shader: records what it computes (the depth converted as the game's)
    VkDescriptorSetLayoutBinding ub{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo uli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    uli.bindingCount = 1, uli.pBindings = &ub;
    VkDescriptorSetLayout wind_set_layout;
    vkCreateDescriptorSetLayout(device, &uli, nullptr, &wind_set_layout);
    VkPipelineLayoutCreateInfo wpli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    wpli.setLayoutCount = 1, wpli.pSetLayouts = &wind_set_layout;
    VkPipelineLayout wind_layout;
    vkCreatePipelineLayout(device, &wpli, nullptr, &wind_layout);
    const VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4},
                                           {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 8, dpi.poolSizeCount = 3, dpi.pPoolSizes = sizes;
    VkDescriptorPool dpool;
    vkCreateDescriptorPool(device, &dpi, nullptr, &dpool);
    const auto wind_set = [&](const Buf& u) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = dpool, ai.descriptorSetCount = 1, ai.pSetLayouts = &wind_set_layout;
        VkDescriptorSet set;
        vkAllocateDescriptorSets(device, &ai, &set);
        const VkDescriptorBufferInfo bi{u.buffer, 0, sizeof(Wind)};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set, w.dstBinding = 0, w.descriptorCount = 1, w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
        return set;
    };
    std::vector<uint32_t> wind_xfb;
    {
        std::string why;
        CHECK(spirv_xfb::patch(wind_spv, wind_xfb, &why));
    }
    const VkShaderModule wind_vs = module(wind_spv.data(), wind_spv.size() * 4);
    const VkShaderModule wind_xfb_vs = module(wind_xfb.data(), wind_xfb.size() * 4);
    PipeOpts wo;
    wo.vs = wind_xfb_vs, wo.layout = wind_layout, wo.vec = 3, wo.discard = true;
    const VkPipeline wind_capture = pipeline(wo);
    CHECK(wind_capture != VK_NULL_HANDLE);
    {
        Buf u = buffer(sizeof(Wind), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT), vin = buffer(4 * 12, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT),
            cap = buffer(4096, kCapture);
        const Wind w = wind_of(camera_now(), 0.13, -0.07, 0.02);
        std::memcpy(u.map, &w, sizeof w);
        const float pts[4][3] = {{0, 0, -5}, {1, 0.5, -6}, {-1.2, -0.3, -4}, {0.4, 0.9, -8}};
        std::memcpy(vin.map, pts, sizeof pts);
        const VkDescriptorSet set = wind_set(u);
        if (wind_capture) {
            VkCommandBuffer cb = begin(g);
            capture(cb, wind_capture, cap.buffer, 0, 4 * 16, [&] {
                const VkDeviceSize zero = 0;
                vkCmdBindVertexBuffers(cb, 0, 1, &vin.buffer, &zero);
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, wind_layout, 0, 1, &set, 0, nullptr);
                vkCmdDraw(cb, 3, 1, 0, 0);   // (a triangle: the fourth point is not drawn, and stays 0)
            });
            submit(g, cb);
        }
        int bad = 0;
        const float* got = (const float*)cap.map;
        for (int i = 0; i < 4; i++) {
            auto c = apply(camera_now(), pts[i][0] + 0.13, pts[i][1] - 0.07, pts[i][2] + 0.02);
            const float want[4] = {(float)c[0], (float)c[1], (float)((c[2] + c[3]) * 0.5), (float)c[3]};
            for (int k = 0; k < 4; k++) bad += std::fabs(got[i * 4 + k] - (i < 3 ? want[k] : 0.0f)) > 1e-4f * std::max(1.0f, std::fabs(want[k]));
        }
        std::printf("2. glslang's shape (a wind shader): %s\n", bad ? "RECORDED WRONG" : "recorded what it computes");
        CHECK(bad == 0);
    }

    // ---- 3. a patch of grass in two frames of wind: the motion pass against the CPU model
    VkDescriptorSetLayoutBinding mb[3]{};
    mb[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    mb[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    mb[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo mli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    mli.bindingCount = 3, mli.pBindings = mb;
    VkDescriptorSetLayout motion_set_layout;
    vkCreateDescriptorSetLayout(device, &mli, nullptr, &motion_set_layout);
    const VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(dlss_actor::Push)};
    VkPipelineLayoutCreateInfo mpli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    mpli.setLayoutCount = 1, mpli.pSetLayouts = &motion_set_layout, mpli.pushConstantRangeCount = 1, mpli.pPushConstantRanges = &pr;
    VkPipelineLayout motion_layout;
    vkCreatePipelineLayout(device, &mpli, nullptr, &motion_layout);
    PipeOpts so;   // (the scene's depth: the wind shader unpatched, no fragment stage)
    so.vs = wind_vs, so.layout = wind_layout, so.vec = 3, so.depth = true;
    const VkPipeline scene = pipeline(so);
    PipeOpts mo;
    mo.vs = module(DLSS_CAPTURE_VERT_SPV, sizeof DLSS_CAPTURE_VERT_SPV), mo.fs = module(DLSS_ACTOR_FRAG_SPV, sizeof DLSS_ACTOR_FRAG_SPV);
    mo.layout = motion_layout, mo.motion = true;
    const VkPipeline motion = pipeline(mo);
    CHECK(scene != VK_NULL_HANDLE && motion != VK_NULL_HANDLE);
    if (!scene || !motion || !wind_capture) return std::printf("%d failures\n", failures), 1;

    const Grass gr = grass();
    const uint32_t n = (uint32_t)gr.idx.size();
    Buf vin = buffer(gr.xyz.size() * 4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT), ib = buffer(gr.idx.size() * 2, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    std::memcpy(vin.map, gr.xyz.data(), gr.xyz.size() * 4);
    std::memcpy(ib.map, gr.idx.data(), gr.idx.size() * 2);
    // (the same patch 16 times bigger at 99.85 m, square to the camera: the scene's depth there is over 0.99999, the sky's and
    // the far plane's, which keep the camera's motion (dlss_actor.frag): a sky drawn at the far plane is not given its own)
    // (the patch with its middle vertex (3, 3) 1.6 m off: the frame before's record of the six triangles around it jumped at one
    // corner only)
    Grass corner_gr = gr;
    corner_gr.xyz[(3 * 7 + 3) * 3] += 1.6f;
    Buf vcorner = buffer(corner_gr.xyz.size() * 4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    std::memcpy(vcorner.map, corner_gr.xyz.data(), corner_gr.xyz.size() * 4);
    const Grass far_gr = grass(translate(-0.3 * 16, -0.2 * 16, -99.85), 16.0);
    Buf vfar = buffer(far_gr.xyz.size() * 4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    std::memcpy(vfar.map, far_gr.xyz.data(), far_gr.xyz.size() * 4);
    Buf u_before = buffer(sizeof(Wind), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT), u_now = buffer(sizeof(Wind), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    const double wb[3] = {-0.18, 0.06, 0.03}, wn[3] = {0.21, -0.04, -0.05};   // (the wind's offset, the frame before and now)
    const Wind w_before = wind_of(camera_before(), wb[0], wb[1], wb[2]), w_now = wind_of(camera_now(), wn[0], wn[1], wn[2]);
    std::memcpy(u_before.map, &w_before, sizeof w_before);
    std::memcpy(u_now.map, &w_now, sizeof w_now);
    const VkDescriptorSet set_before = wind_set(u_before), set_now = wind_set(u_now);
    Buf u_jump = buffer(sizeof(Wind), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    const Wind w_jump = wind_of(camera_before(), wb[0] + 1.6, wb[1], wb[2]);
    std::memcpy(u_jump.map, &w_jump, sizeof w_jump);
    const VkDescriptorSet set_jump = wind_set(u_jump);
    Buf u_still = buffer(sizeof(Wind), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);   // (the frame before with this frame's wind)
    const Wind w_still = wind_of(camera_before(), wn[0], wn[1], wn[2]);
    std::memcpy(u_still.map, &w_still, sizeof w_still);
    const VkDescriptorSet set_still = wind_set(u_still);
    Buf captures = buffer((VkDeviceSize)n * 16 * 2, kCapture), head = buffer(dlss_actor::kCaptureHeadWords * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Buf readback = buffer((VkDeviceSize)W * H * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    Img depth = image(VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                      VK_IMAGE_ASPECT_DEPTH_BIT);
    Img mot = image(VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT);
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler nearest;
    vkCreateSampler(device, &sci, nullptr, &nearest);
    VkDescriptorSetAllocateInfo mai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    mai.descriptorPool = dpool, mai.descriptorSetCount = 1, mai.pSetLayouts = &motion_set_layout;
    VkDescriptorSet mset;
    vkAllocateDescriptorSets(device, &mai, &mset);
    {
        const VkDescriptorBufferInfo hb{head.buffer, 0, dlss_actor::kCaptureHeadWords * 4}, cbi{captures.buffer, 0, VK_WHOLE_SIZE};
        const VkDescriptorImageInfo ii{nearest, depth.view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w[3]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        w[0].dstSet = mset, w[0].dstBinding = 0, w[0].descriptorCount = 1, w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, w[0].pBufferInfo = &hb;
        w[1].dstSet = mset, w[1].dstBinding = 1, w[1].descriptorCount = 1, w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, w[1].pImageInfo = &ii;
        w[2].dstSet = mset, w[2].dstBinding = 2, w[2].descriptorCount = 1, w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, w[2].pBufferInfo = &cbi;
        vkUpdateDescriptorSets(device, 3, w, 0, nullptr);
    }
    struct Frame {
        const char* what;
        float jx, jy;        // (the scene's jitter)
        bool flip;           // (the motion pass with the scene's viewport: negative height. false: a wrong wiring)
        bool swap;           // (this frame's and the frame before's records swapped: a wrong wiring)
        bool jump;           // (the frame before's record 1.6 m off: another draw's, or a jump: left to the camera's motion)
        float limit = 24.0f; // (the jumps' limit, render pixels)
        bool mixed = false;  // (the frame before's draw in two: the first half as before, the second half jumped)
        bool at_far = false; // (the patch at the far plane; not "far": a Windows macro)
        bool still = false;  // (no wind between the frames: the camera's motion alone)
        bool corner = false; // (the frame before's middle vertex 1.6 m off)
    };
    const auto run = [&](const Frame& f, std::vector<float>& dpt, std::vector<float>& mv) {
        float cur[16], prev[16];
        to_float(camera_now(), cur);
        to_float(camera_before(), prev);
        float c2p[16];   // (the cameras' motion, as vk_draw.cpp clip_to_previous gives it: the jumps' test)
        to_float(mul(camera_before(), inverse(camera_now())), c2p);
        dlss_actor::capture_head(cur, prev, f.jx, f.jy, W, H, c2p, f.limit, (float*)head.map);
        const VkBuffer verts = f.at_far ? vfar.buffer : vin.buffer;
        VkCommandBuffer cb = begin(g);
        layout(cb, depth.image, VK_IMAGE_ASPECT_DEPTH_BIT);
        layout(cb, mot.image, VK_IMAGE_ASPECT_COLOR_BIT);
        VkClearColorValue sentinel{};
        sentinel.float32[0] = kSentinel[0], sentinel.float32[1] = kSentinel[1];
        const VkImageSubresourceRange color{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, mot.image, VK_IMAGE_LAYOUT_GENERAL, &sentinel, 1, &color);
        const VkDeviceSize zero = 0;
        for (int frame = 0; frame < 2; frame++)   // (the frame before's record, then this frame's: the game's draw, recorded)
            capture(cb, wind_capture, captures.buffer, (VkDeviceSize)frame * n * 16, (VkDeviceSize)n * 16, [&] {
                vkCmdBindVertexBuffers(cb, 0, 1, !frame && f.corner ? &vcorner.buffer : &verts, &zero);
                vkCmdBindIndexBuffer(cb, ib.buffer, 0, VK_INDEX_TYPE_UINT16);
                const VkDescriptorSet s = frame ? set_now : f.jump ? set_jump : f.still ? set_still : set_before;
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, wind_layout, 0, 1, &s, 0, nullptr);
                if (frame || !f.mixed) return vkCmdDrawIndexed(cb, n, 1, 0, 0, 0);
                vkCmdDrawIndexed(cb, n / 2, 1, 0, 0, 0);   // (the records follow on: the second half's after the first's)
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, wind_layout, 0, 1, &set_jump, 0, nullptr);
                vkCmdDrawIndexed(cb, n - n / 2, 1, n / 2, 0, 0);
            });
        full_barrier(cb);
        const VkRect2D area{{0, 0}, {(uint32_t)W, (uint32_t)H}};
        {   // (the scene's depth: this frame's draw, the game's viewport: negative height, the jitter's offset)
            VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            da.imageView = depth.view, da.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, da.storeOp = VK_ATTACHMENT_STORE_OP_STORE, da.clearValue.depthStencil = {1.0f, 0};
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = area, ri.layerCount = 1, ri.pDepthAttachment = &da;
            vkCmdBeginRendering(cb, &ri);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, scene);
            const VkViewport v{f.jx, (float)H + f.jy, (float)W, -(float)H, 0.0f, 1.0f};
            vkCmdSetViewport(cb, 0, 1, &v);
            vkCmdSetScissor(cb, 0, 1, &area);
            vkCmdBindVertexBuffers(cb, 0, 1, &verts, &zero);
            vkCmdBindIndexBuffer(cb, ib.buffer, 0, VK_INDEX_TYPE_UINT16);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, wind_layout, 0, 1, &set_now, 0, nullptr);
            vkCmdDrawIndexed(cb, n, 1, 0, 0, 0);
            vkCmdEndRendering(cb);
        }
        full_barrier(cb);
        {   // (the captured motion pass, as vk_draw.cpp dlss_capture_pass draws it)
            VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            ca.imageView = mot.view, ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = area, ri.layerCount = 1, ri.colorAttachmentCount = 1, ri.pColorAttachments = &ca;
            vkCmdBeginRendering(cb, &ri);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, motion);
            const VkViewport v = f.flip ? VkViewport{f.jx, (float)H + f.jy, (float)W, -(float)H, 0.0f, 1.0f}
                                        : VkViewport{f.jx, f.jy, (float)W, (float)H, 0.0f, 1.0f};
            vkCmdSetViewport(cb, 0, 1, &v);
            vkCmdSetScissor(cb, 0, 1, &area);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, motion_layout, 0, 1, &mset, 0, nullptr);
            dlss_actor::Push push{};
            push.count = n, push.first = f.swap ? 0 : n, push.base_vertex = f.swap ? (int32_t)n : 0, push.tolerance = 64;
            vkCmdPushConstants(cb, motion_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, &push);
            vkCmdDraw(cb, n, 1, 0, 0);
            vkCmdEndRendering(cb);
        }
        full_barrier(cb);
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, c.imageExtent = {(uint32_t)W, (uint32_t)H, 1};
        vkCmdCopyImageToBuffer(cb, depth.image, VK_IMAGE_LAYOUT_GENERAL, readback.buffer, 1, &c);
        c.bufferOffset = (VkDeviceSize)W * H * 4, c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vkCmdCopyImageToBuffer(cb, mot.image, VK_IMAGE_LAYOUT_GENERAL, readback.buffer, 1, &c);
        full_barrier(cb);
        submit(g, cb);
        dpt.resize((size_t)W * H), mv.resize((size_t)W * H * 2);
        std::memcpy(dpt.data(), readback.map, dpt.size() * 4);
        const uint16_t* hh = (const uint16_t*)(readback.map + (size_t)W * H * 4);
        for (size_t i = 0; i < mv.size(); i++) mv[i] = half_to_float(hh[i]);
    };
    struct Score {
        int covered = 0, written = 0, bad = 0, bad_kept = 0, bad_written = 0;   // (bad_written: written, and wrong)
        double worst = 0;
        float nearest = 1.0f;   // (the least depth covered)
    };
    const auto score = [&](const Frame& f) {
        std::vector<float> dpt, mv, want, dist;
        run(f, dpt, mv);
        float curf[16], a = 0, b = 0, clip[16];
        to_float(camera_now(), curf);
        objmv::depth_terms(curf, a, b);
        // (the world moved by the wind's change: before = now + (wind before - wind now))
        const double* was = f.still ? wn : wb;
        to_float(mul(mul(camera_before(), translate(was[0] - wn[0], was[1] - wn[1], was[2] - wn[2])), inverse(camera_now())), clip);
        objmv::motion_pass(W, H, dpt.data(), clip, a, b, false, false, nullptr, want, dist, f.jx, f.jy);
        Score s;
        for (size_t i = 0; i < dpt.size(); i++) {
            const float gx = mv[i * 2], gy = mv[i * 2 + 1];
            const bool kept = gx == kSentinel[0] && gy == kSentinel[1];
            s.written += !kept;
            if (dpt[i] >= 1.0f) {
                s.bad_kept += !kept;
                continue;
            }
            s.covered++, s.nearest = std::min(s.nearest, dpt[i]);
            const float ex = want[i * 2], ey = want[i * 2 + 1];
            const double err = std::max(std::fabs(gx - ex), std::fabs(gy - ey));
            if (err > 0.02 + std::max(std::fabs(ex), std::fabs(ey)) / 400) s.bad++, s.bad_written += !kept;
            else s.worst = std::max(s.worst, err);
        }
        std::printf("3. %-38s covered %5d written %5d off %4d (written %4d) kept wrongly %d worst %.4f px\n", f.what, s.covered,
                    s.written, s.bad, s.bad_written, s.bad_kept, s.worst);
        return s;
    };
    const Score right = score({"swaying, jittered", 0.37f, -0.29f, true, false, false});
    CHECK(right.covered > 2000 && right.written == right.covered && right.bad * 1000 <= right.covered && right.bad_kept == 0);
    const Score flat = score({"swaying, no jitter", 0, 0, true, false, false});
    CHECK(flat.covered > 2000 && flat.written == flat.covered && flat.bad * 1000 <= flat.covered && flat.bad_kept == 0);
    // a jump (the frame before's record of another place: a draw matched with another one): nothing written, the camera's stays
    const Score jumped = score({"a jump of 1.6 m: left to the camera", 0.37f, -0.29f, true, false, true});
    CHECK(jumped.covered > 2000 && jumped.written * 100 <= jumped.covered && jumped.bad_kept == 0);
    const Score up = score({"wrong: the viewport not the scene's", 0.37f, -0.29f, false, false, false});
    CHECK(up.written * 10 < right.written || up.bad * 10 > up.covered * 9);
    const Score swapped = score({"wrong: now and before swapped", 0.37f, -0.29f, true, true, false});
    CHECK(swapped.bad * 10 > swapped.covered * 9);

    // still grass, the camera moving: its own motion is nothing, and all is kept under a limit of a fraction of a pixel. The
    // jumps' test takes the records' depth back to NVN's (z * 2 - w) before the cameras' motion: the depth as recorded puts the
    // vertices where the camera did not take them, over a pixel off (the CPU model of dlss_capture.vert's jumped(), both ways)
    const auto own_most = [&](bool nvn) {
        const M4 c2p = mul(camera_before(), inverse(camera_now()));
        double most = 0;
        for (size_t v = 0; v + 2 < gr.xyz.size(); v += 3) {
            const double x = gr.xyz[v], y = gr.xyz[v + 1], z = gr.xyz[v + 2];
            const auto c = apply(camera_now(), x + wn[0], y + wn[1], z + wn[2]);
            const auto t = apply(camera_before(), x + wn[0], y + wn[1], z + wn[2]);
            const double rz = (c[2] + c[3]) * 0.5;   // (the record's depth, as the wind shader writes it)
            const double in[4] = {c[0], c[1], nvn ? rz * 2 - c[3] : rz, c[3]};
            double m[4] = {};
            for (int r = 0; r < 4; r++)
                for (int k = 0; k < 4; k++) m[r] += c2p[r * 4 + k] * in[k];
            most = std::max({most, std::fabs((t[0] / t[3] - m[0] / m[3]) * 0.5 * W), std::fabs((t[1] / t[3] - m[1] / m[3]) * 0.5 * H)});
        }
        return most;
    };
    const double own = own_most(true), own_raw = own_most(false);
    std::printf("3. still grass's own motion: at most %.4f px (%.3f px with the depth as recorded)\n", own, own_raw);
    CHECK(own < 0.01 && own_raw > 0.8);   // (else the case below does not tell them apart)
    const Score still = score({"still, a limit of 0.4 px", 0.37f, -0.29f, true, false, false, 0.4f, false, false, true});
    CHECK(still.covered > 2000 && still.written == still.covered && still.bad * 1000 <= still.covered && still.bad_kept == 0);
    // half the draw jumped: its triangles (threes of records) left to the camera's, the others' written right; a vertex that
    // tested the next triangle's records gave the last kept triangle a "before" of zero at some of its corners
    const Score mixed = score({"half of it jumped", 0.37f, -0.29f, true, false, false, 24.0f, true});
    CHECK(mixed.covered > 2000 && mixed.written * 10 > mixed.covered * 3 && mixed.written * 10 < mixed.covered * 7 &&
          mixed.bad_written * 1000 <= mixed.covered && mixed.bad_kept == 0);
    // one corner jumped: its six triangles left to the camera's whole (all three corners test all three records), the rest right
    const Score corner = score({"one corner jumped", 0.37f, -0.29f, true, false, false, 24.0f, false, false, false, true});
    CHECK(corner.covered > 2000 && corner.written < corner.covered && corner.written * 10 > corner.covered * 7 &&
          corner.bad_written * 1000 <= corner.covered && corner.bad_kept == 0);
    // at the far plane (the sky's depth): nothing written
    const Score sky = score({"at the far plane (the sky)", 0.37f, -0.29f, true, false, false, 24.0f, false, true});
    std::printf("3. at the far plane: its nearest depth %.7f\n", sky.nearest);
    CHECK(sky.covered > 1000 && sky.nearest >= 0.99999f && sky.written == 0);

    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(inst, nullptr);
    if (failures) return std::printf("%d failures\n", failures), 1;
    std::printf("PASS: the game's vertex shader recorded by transform feedback, and the captured motion pass's vectors are the "
                "motion of what it moved\n");
    return 0;
}
