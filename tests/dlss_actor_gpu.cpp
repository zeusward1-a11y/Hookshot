// DLSS's actor motion (gpu/vk/shaders/dlss_actor.vert and .frag, their SPIR-V in shaders/dlss_actor_*_spv.h) run on a Vulkan
// device the way vk_draw.cpp dlss_actor_pass draws it in this branch, with what gpu/vk/dlss_actor_plan.h works out for it (the
// pose of each draw placed in two buffers over two frames by Poses, the push constants by make_push, the cameras' buffer by head):
// the camera unjittered (dlss_note_camera's), the scene's jitter a viewport offset of ux, uy render pixels (dlss_jitter_of), the
// scene's depth D32 sampled in GENERAL, the motion R16G16F loaded and drawn over. Skinned draws (a rigid bone that
// turns, two bones blended over two sets, the game's vertex formats, 8/16/32-bit indices at unaligned starts, a base vertex) over
// a scene depth drawn with the same jitter, against the camera's motion pass on the CPU (gpu/vk/dlss_objmv.h motion_pass, the
// model of shaders/dlss_motion.comp) of a world that moved as each draw's pose did: an actor's vectors are the camera's of a world
// that moved with it, in the same units and with the same jitter, or DLSS gets two conventions in one picture. The scene's depth
// a few steps off the pass's (the game's own shader draws it: depth bias here) is still the draw's, far off it is not. Then three
// wrong wirings, which must fail: the pass drawn without the scene's offset, the jitter's y the wrong way, the jitter not taken off.
// Needs a GPU with buffer device addresses, 64-bit integers in shaders and dynamic rendering (tests/run_all.py skips it; run it
// by name). No game.
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
#include <vector>

#include "dlss_actor.h"
#include "dlss_actor_plan.h"
#include "dlss_objmv.h"
#include "shaders/dlss_actor_frag_spv.h"
#include "shaders/dlss_actor_vert_spv.h"

namespace objmv = mkwvk::objmv;

static int failures = 0;
#define CHECK(x)                                                                    \
    do {                                                                            \
        if (!(x)) std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x), ++failures; \
    } while (0)

#define FNS(X)                                                                                                                       \
    X(vkCreateInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties)   \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFeatures2) X(vkCreateDevice) X(vkGetDeviceQueue) X(vkCreateImage)    \
    X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkBindImageMemory) X(vkCreateImageView) X(vkCreateBuffer)                  \
    X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkMapMemory) X(vkGetBufferDeviceAddress) X(vkCreateSampler)            \
    X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkCreateShaderModule) X(vkCreateGraphicsPipelines)                    \
    X(vkCreateDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateCommandPool)                           \
    X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdClearColorImage)        \
    X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants)                \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetDepthBias) X(vkCmdDraw) X(vkCmdCopyImageToBuffer) X(vkQueueSubmit) X(vkQueueWaitIdle)               \
    X(vkDestroyDevice) X(vkDestroyInstance)
#define DECL(f) static PFN_##f f;
FNS(DECL)

constexpr int W = 256, H = 160;   // (the render size)
constexpr double kPi = 3.14159265358979323846;
constexpr float kSentinel[2] = {100.0f, -50.0f};   // (the camera's motion under the pass, exact in half: what no actor covers keeps it)

// ---- 4 x 4 matrices, row major, column vectors (clip = M * position, as the game's view-projection at its cbuf 0x30)
using M4 = std::array<double, 16>;
static M4 ident() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }
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
static M4 perspective(double fovy, double aspect, double n, double f) {   // (OpenGL's: y up, depth -1 to 1, w = -z)
    const double t = 1 / std::tan(fovy * kPi / 360);
    return {t / aspect, 0, 0, 0, 0, t, 0, 0, 0, 0, (f + n) / (n - f), 2 * f * n / (n - f), 0, 0, -1, 0};
}
static void to_float(const M4& m, float* out, int n = 16) {
    for (int i = 0; i < n; i++) out[i] = (float)m[i];
}

static uint16_t to_half(float f) {   // (round to nearest; the normal range is all this needs)
    uint32_t u;
    std::memcpy(&u, &f, 4);
    const uint32_t s = (u >> 16) & 0x8000u;
    const int e = (int)((u >> 23) & 0xff) - 127 + 15;
    uint32_t m = u & 0x7fffffu;
    if (e <= 0) return (uint16_t)s;
    uint32_t h = s | ((uint32_t)e << 10) | (m >> 13);
    if ((m & 0x1fffu) > 0x1000u || ((m & 0x1fffu) == 0x1000u && (h & 1))) h++;
    return (uint16_t)h;
}
static float half_to_float(uint16_t h) {
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? std::ldexp((float)m, -24) : e == 31 ? (m ? NAN : INFINITY) : std::ldexp((float)(m | 1024), (int)e - 25);
    return s ? -v : v;
}

// ---- the scene
constexpr int kBones = 4;   // (one palette for every draw: 0 the rigid one's, 1 and 2 the blended one's, 3 still)
struct Pose {
    std::array<M4, kBones> bones;
    double origin[4][3];   // (each draw's origin, the object's 0x40)
};
enum Draw { kOccluder, kWall, kRigid, kBlended, kDraws };
// (each draw's view distance this frame, the camera at the origin looking down -z: the classes of the read-back depth)
static const double kNear[kDraws] = {2.5, 3.7, 5.8, 9.0}, kFar[kDraws] = {3.5, 4.8, 8.3, 12.3};

static Pose pose_now() {
    Pose p{};
    p.bones[0] = mul(translate(0.2, 0.1, -0.3), mul(rot(2, 10), rot(1, 5)));
    p.bones[1] = mul(translate(-0.1, 0.2, 0.1), rot(0, 6));
    p.bones[2] = mul(translate(0.1, -0.15, 0.05), rot(1, -4));
    p.bones[3] = ident();
    const double o[4][3] = {{0, 0, 0}, {0, 0, 0}, {1.0, 0.5, -2.0}, {-0.5, 0.2, -3.0}};
    std::memcpy(p.origin, o, sizeof o);
    return p;
}
static Pose pose_before() {
    Pose p{};
    p.bones[0] = mul(translate(0.05, 0.3, -0.25), mul(rot(2, 4), rot(1, 2)));
    p.bones[1] = mul(translate(0.15, 0.05, 0.0), rot(0, 2));
    p.bones[2] = mul(translate(-0.2, -0.1, 0.1), rot(1, 3));
    p.bones[3] = ident();
    const double o[4][3] = {{0, 0, 0}, {0, 0, 0}, {0.9, 0.55, -2.05}, {-0.35, 0.3, -3.1}};
    std::memcpy(p.origin, o, sizeof o);
    return p;
}
static const double kW1 = 128.0 / 255, kW2 = 127.0 / 255;   // (the blended draw's weights, set 0 and set 1: unorm8, 1 in all)
/// A draw's skin as one matrix (the weights are the same over all its vertices) with its origin: world = M * model
static M4 skin(const Pose& p, int d) {
    M4 m{};
    if (d == kBlended)
        for (int i = 0; i < 16; i++) m[i] = kW1 * p.bones[1][i] + kW2 * p.bones[2][i];
    else
        m = p.bones[d == kRigid ? 0 : 3];
    m[12] = m[13] = m[14] = 0, m[15] = 1;
    return mul(translate(p.origin[d][0], p.origin[d][1], p.origin[d][2]), m);
}

struct Mesh {
    std::vector<uint8_t> vertices, indices;
    int vertex_count = 0, index_shift = 0;   // (index_shift: the draw's first index, bytes off 4-alignment)
    dlss_actor::Layout l;                   // (as dlss_skin_check notes it)
};
/// A tilted plane of 4 x 4 quads where it is this frame (center, size, turned about y by tilt), its vertices in model space
/// (the inverse of this frame's skin), in the draw's vertex format
static Mesh mesh(int d, const Pose& now) {
    static const double center[kDraws][3] = {{-0.9, 0.3, -3.0}, {0.9, -0.2, -4.2}, {-0.6, 0.1, -7.0}, {1.4, -0.3, -10.5}};
    static const double size[kDraws][2] = {{0.8, 0.8}, {0.9, 0.9}, {3.0, 2.4}, {4.0, 3.2}};
    static const double tilt[kDraws] = {30, -25, 35, -30};
    constexpr int n = 4;
    const M4 place = mul(translate(center[d][0], center[d][1], center[d][2]), rot(1, tilt[d])), to_model = inverse(skin(now, d));
    Mesh m;
    dlss_actor::Layout& l = m.l;
    const int junk = d == kRigid ? 3 : 0;   // (vertices before the draw's: its base vertex)
    l.base_vertex = junk, l.sets = 1;
    if (d == kBlended) l.stride = 28, l.pos = 0, l.sets = 2, l.weights[0] = 8, l.weights[1] = 12, l.joints[0] = 16, l.joints[1] = 20;
    else if (d == kRigid) l.stride = 28, l.pos = 0, l.weights[0] = 12, l.joints[0] = 16;
    else l.stride = 24, l.pos = 0, l.weights[0] = 12, l.joints[0] = 16;
    if (d == kBlended) l.formats = dlss_actor::kHalfPosition | dlss_actor::kByteJoints;   // (set 0's joints bytes, set 1's 16-bit)
    m.vertex_count = junk + (n + 1) * (n + 1);
    m.vertices.assign((size_t)m.vertex_count * l.stride, 0xcd);   // (what the draw does not read: noise)
    l.vsize = m.vertices.size();
    for (int i = 0; i <= n; i++)
        for (int j = 0; j <= n; j++) {
            const auto w = apply(place, ((double)j / n - 0.5) * size[d][0], ((double)i / n - 0.5) * size[d][1], 0);
            const auto p = apply(to_model, w[0], w[1], w[2]);
            uint8_t* v = m.vertices.data() + (size_t)(junk + i * (n + 1) + j) * l.stride;
            if (l.formats & dlss_actor::kHalfPosition) {
                const uint16_t h[4] = {to_half((float)p[0]), to_half((float)p[1]), to_half((float)p[2]), to_half(1.0f)};
                std::memcpy(v + l.pos, h, 8);
            } else {
                const float f[3] = {(float)p[0], (float)p[1], (float)p[2]};
                std::memcpy(v + l.pos, f, 12);
            }
            if (d == kBlended) {   // (set 0, bytes: bone 1 at 128/255, junk joints with no weight; set 1, 16-bit: bone 2 at 127/255 in
                                   // lane 3, joint 9 past the palette in the others: read as bytes, lane 3 would be joint 9)
                const uint8_t w0[4] = {128, 0, 0, 0}, w1[4] = {0, 0, 127, 0}, j0[4] = {1, 9, 9, 9};
                const uint16_t j1[4] = {9, 9, 2, 9};
                std::memcpy(v + 8, w0, 4), std::memcpy(v + 12, w1, 4), std::memcpy(v + 16, j0, 4), std::memcpy(v + 20, j1, 8);
            } else if (d == kRigid) {   // (bone 0 alone, in lane 4: the second word's high half)
                const uint8_t w0[4] = {0, 0, 0, 255};
                const uint16_t j0[4] = {7, 7, 7, 0};
                std::memcpy(v + 12, w0, 4), std::memcpy(v + 16, j0, 8);
            } else {   // (bone 3 by the second slot: a weight pairs with its own joint. Joint 12 past the palette with no weight;
                       // joint 4, the palette's size, with one: skipped all the same, as the shader reads no bone past nbones)
                const uint8_t w0[4] = {0, 255, 40, 0};
                const uint16_t j0[4] = {12, 3, 4, 0};
                std::memcpy(v + 12, w0, 4), std::memcpy(v + 16, j0, 8);
            }
        }
    l.itype = d == kBlended ? 0 : d == kRigid ? 1 : 2;   // (8, 16, 32-bit)
    m.index_shift = d == kBlended ? 3 : d == kRigid ? 2 : 0;
    std::vector<uint32_t> idx;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            const uint32_t a = i * (n + 1) + j, b = a + 1, c = a + n + 1, e = c + 1;
            for (uint32_t x : {a, b, e, a, e, c}) idx.push_back(x);
        }
    l.count = (int)idx.size();
    m.indices.assign((size_t)m.index_shift, 0xee);
    for (uint32_t x : idx)
        for (int k = 0; k < (1 << l.itype); k++) m.indices.push_back((uint8_t)(x >> (8 * k)));
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
    VkDeviceAddress address;
};
static Buf buffer(VkDeviceSize size, VkBufferUsageFlags usage) {   // (host visible; by its device address when asked)
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size, bi.usage = usage;
    Buf b{};
    vkCreateBuffer(device, &bi, nullptr, &b.buffer);
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(device, b.buffer, &r);
    VkMemoryAllocateFlagsInfo fi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) ? &fi : nullptr;
    ai.allocationSize = r.size;
    ai.memoryTypeIndex = memory_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory m;
    vkAllocateMemory(device, &ai, nullptr, &m);
    vkBindBufferMemory(device, b.buffer, m, 0);
    void* p = nullptr;
    vkMapMemory(device, m, 0, VK_WHOLE_SIZE, 0, &p);
    b.map = (uint8_t*)p;
    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        da.buffer = b.buffer;
        b.address = vkGetBufferDeviceAddress(device, &da);
    }
    return b;
}
static void layout(VkCommandBuffer cb, VkImage i, VkImageAspectFlags aspect) {   // (undefined to general: the contents dropped)
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

struct Gpu {
    VkQueue queue;
    VkCommandPool pool;
    VkPipelineLayout layout;
    VkPipeline scene, actor;
    VkDescriptorSet set;
    Img depth, motion;
    Buf head, geometry, poses[2], readback;
    VkDeviceSize vertex_at[kDraws], index_at[kDraws];
};

/// The game's scene pass, for its depth: the draws as they were drawn this frame (the same vertex stage, which is the game's
/// skin and camera), depth test, the jitter's viewport offset, no fragment stage. And the actor pass as vk_draw.cpp makes it
/// (make_actor_pipeline): no vertex input, no culling, R16G16F with R and G written, no depth attachment (the fragment compares)
static VkPipeline pipeline(VkPipelineLayout pl, bool actor) {
    VkShaderModule m[2]{};
    const uint32_t* code[2] = {DLSS_ACTOR_VERT_SPV, DLSS_ACTOR_FRAG_SPV};
    const size_t size[2] = {sizeof DLSS_ACTOR_VERT_SPV, sizeof DLSS_ACTOR_FRAG_SPV};
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (int i = 0; i < 2; i++) {
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = size[i], mi.pCode = code[i];
        vkCreateShaderModule(device, &mi, nullptr, &m[i]);
        stages[i] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        stages[i].module = m[i], stages[i].pName = "main";
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL, rs.cullMode = VK_CULL_MODE_NONE, rs.lineWidth = 1.0f;
    rs.depthBiasEnable = actor ? VK_FALSE : VK_TRUE;   // (the scene's: its depth a few steps off the pass's, as the game's shader may)
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo dsi{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    dsi.depthTestEnable = dsi.depthWriteEnable = VK_TRUE, dsi.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = actor ? 1 : 0, cb.pAttachments = &ba;
    const VkDynamicState dyn[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = actor ? 2 : 3, dy.pDynamicStates = dyn;
    const VkFormat motion = VK_FORMAT_R16G16_SFLOAT;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    if (actor) ri.colorAttachmentCount = 1, ri.pColorAttachmentFormats = &motion;
    else ri.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &ri;
    gi.stageCount = actor ? 2 : 1, gi.pStages = stages;
    gi.pVertexInputState = &vi, gi.pInputAssemblyState = &ia, gi.pViewportState = &vp, gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms, gi.pDepthStencilState = actor ? nullptr : &dsi, gi.pColorBlendState = &cb;
    gi.pDynamicState = &dy, gi.layout = pl;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gi, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    return p;
}

/// One frame: how the scene was drawn (jx, jy: its viewport offset) and how the actor pass is (its offset, the jitter it takes off
/// in clip space), which draws the scene has and which the actor pass draws again
enum Head { kRight, kYSign, kNoJitter };   // (the cameras' buffer: dlss_actor::head's, its jitter's y the wrong way, no jitter)
struct Frame {
    const char* what;
    float jx, jy;              // (the scene's jitter, render pixels)
    float ax, ay;              // (the actor pass's viewport offset: the scene's when right)
    Head head;
    bool still;                // (the poses the frame before the same as this frame's)
    bool in_scene[kDraws], in_actor[kDraws];
    float bias = 0;            // (the scene's depth this many steps (D32 ulps at its depths) farther than the pass's)
};
struct Result {
    std::vector<float> depth, motion;   // (W x H; W x H x 2)
};

static const Mesh& the_mesh(int d) {
    static const Pose now = pose_now();
    static const Mesh m[kDraws] = {mesh(kOccluder, now), mesh(kWall, now), mesh(kRigid, now), mesh(kBlended, now)};
    return m[d];
}
static const M4& camera_now() {
    static const M4 c = perspective(60, (double)W / H, 0.5, 100);   // (at the origin, looking down -z)
    return c;
}
static const M4& camera_before() {   // (a step to the side and back, turned a little: the camera's own motion)
    static const M4 c = mul(perspective(60, (double)W / H, 0.5, 100), inverse(mul(translate(-0.12, 0.08, 0.15), mul(rot(1, 1.2), rot(0, -0.6)))));
    return c;
}

/// A frame's pose of each draw where dlss_actor_pass copies it: placed by Poses, its origin then its bones
static void write_poses(Gpu& g, dlss_actor::Poses& poses, const Pose& p, uint64_t frame, uint32_t at[kDraws]) {
    poses.begin();
    for (int d = 0; d < kDraws; d++) {
        const uint64_t key = dlss_actor::key(g.vertex_at[d], g.index_at[d], the_mesh(d).l.count, the_mesh(d).l.base_vertex, 7, 0);
        CHECK(poses.place(key, kBones, frame, &at[d]));
        float* slot = (float*)(g.poses[poses.here()].map + at[d]);
        for (int k = 0; k < 3; k++) slot[k] = (float)p.origin[d][k];
        slot[3] = 0;
        for (int b = 0; b < kBones; b++) to_float(p.bones[b], slot + 4 + b * 12, 12);
    }
}

static Result run(Gpu& g, const Frame& f) {
    const Pose now = pose_now(), before = f.still ? pose_now() : pose_before();
    // the frame before (frame 1), then this one (frame 2): each one's poses in the buffer Poses gives it, this frame's draws with
    // the frame before's pose Poses finds, their push constants by make_push, the cameras by head: dlss_actor_pass's sequence
    dlss_actor::Poses poses;
    uint32_t at_before[kDraws], at_now[kDraws];
    write_poses(g, poses, before, 1, at_before);
    poses.end();
    write_poses(g, poses, now, 2, at_now);
    dlss_actor::Push push[kDraws]{};
    for (int d = 0; d < kDraws; d++) {
        const Mesh& m = the_mesh(d);
        const uint64_t key = dlss_actor::key(g.vertex_at[d], g.index_at[d], m.l.count, m.l.base_vertex, 7, 0);
        const dlss_actor::Pose* was = poses.before_of(key, kBones, 2, true);
        CHECK(was && was->at == at_before[d]);
        CHECK(dlss_actor::make_push(m.l, g.geometry.address + g.vertex_at[d], g.geometry.address + g.index_at[d] + m.index_shift,
                                    g.poses[poses.here()].address, at_now[d], g.poses[poses.there()].address, was ? was->at : 0,
                                    kBones, dlss_actor::tolerance_of(nullptr), &push[d]));
    }
    poses.end();
    float cur[16], prev[16];
    to_float(camera_now(), cur);
    to_float(camera_before(), prev);
    float* head = (float*)g.head.map;
    dlss_actor::head(cur, prev, f.jx, f.jy, W, H, head);
    if (f.head == kYSign) head[33] = -head[33];
    if (f.head == kNoJitter) head[32] = head[33] = 0;
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = g.pool, cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, cai.commandBufferCount = 1;
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(device, &cai, &cb);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cb, &begin);
    layout(cb, g.depth.image, VK_IMAGE_ASPECT_DEPTH_BIT);
    layout(cb, g.motion.image, VK_IMAGE_ASPECT_COLOR_BIT);
    VkClearColorValue sentinel{};
    sentinel.float32[0] = kSentinel[0], sentinel.float32[1] = kSentinel[1];
    const VkImageSubresourceRange color{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, g.motion.image, VK_IMAGE_LAYOUT_GENERAL, &sentinel, 1, &color);
    const VkRect2D scissor{{0, 0}, {(uint32_t)W, (uint32_t)H}};
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 1, &g.set, 0, nullptr);
    {   // (the scene)
        VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        da.imageView = g.depth.view, da.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, da.storeOp = VK_ATTACHMENT_STORE_OP_STORE, da.clearValue.depthStencil = {1.0f, 0};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = scissor, ri.layerCount = 1, ri.pDepthAttachment = &da;
        vkCmdBeginRendering(cb, &ri);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.scene);
        const VkViewport v{f.jx, f.jy, (float)W, (float)H, 0.0f, 1.0f};
        vkCmdSetViewport(cb, 0, 1, &v);
        vkCmdSetScissor(cb, 0, 1, &scissor);
        vkCmdSetDepthBias(cb, f.bias, 0.0f, 0.0f);   // (D32 at depths in [0.5, 1): a constant of n is n ulps)
        for (int d = 0; d < kDraws; d++)
            if (f.in_scene[d]) {
                vkCmdPushConstants(cb, g.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push[d], &push[d]);
                vkCmdDraw(cb, (uint32_t)the_mesh(d).l.count, 1, 0, 0);
            }
        vkCmdEndRendering(cb);
    }
    full_barrier(cb);
    {   // (the actor pass, as dlss_actor_pass draws it)
        VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        ca.imageView = g.motion.view, ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = scissor, ri.layerCount = 1, ri.colorAttachmentCount = 1, ri.pColorAttachments = &ca;
        vkCmdBeginRendering(cb, &ri);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.actor);
        const VkViewport v{f.ax, f.ay, (float)W, (float)H, 0.0f, 1.0f};
        vkCmdSetViewport(cb, 0, 1, &v);
        vkCmdSetScissor(cb, 0, 1, &scissor);
        for (int d = 0; d < kDraws; d++)
            if (f.in_actor[d]) {
                vkCmdPushConstants(cb, g.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push[d], &push[d]);
                vkCmdDraw(cb, (uint32_t)the_mesh(d).l.count, 1, 0, 0);
            }
        vkCmdEndRendering(cb);
    }
    full_barrier(cb);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, c.imageExtent = {(uint32_t)W, (uint32_t)H, 1};
    vkCmdCopyImageToBuffer(cb, g.depth.image, VK_IMAGE_LAYOUT_GENERAL, g.readback.buffer, 1, &c);
    c.bufferOffset = (VkDeviceSize)W * H * 4, c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vkCmdCopyImageToBuffer(cb, g.motion.image, VK_IMAGE_LAYOUT_GENERAL, g.readback.buffer, 1, &c);
    full_barrier(cb);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1, si.pCommandBuffers = &cb;
    vkQueueSubmit(g.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(g.queue);
    Result r;
    r.depth.resize((size_t)W * H), r.motion.resize((size_t)W * H * 2);
    std::memcpy(r.depth.data(), g.readback.map, r.depth.size() * 4);
    const uint16_t* h = (const uint16_t*)(g.readback.map + (size_t)W * H * 4);
    for (size_t i = 0; i < r.motion.size(); i++) r.motion[i] = half_to_float(h[i]);
    return r;
}

/// Which draw the scene shows at each pixel, by its view distance (the draws sit in ranges of their own); -1: nothing drawn, -2:
/// none of them
static std::vector<int> classes(const Result& r) {
    float a = 0, b = 0;
    float cur[16];
    to_float(camera_now(), cur);
    std::vector<int> out((size_t)W * H, -1);
    if (!objmv::depth_terms(cur, a, b)) return out;
    for (size_t i = 0; i < out.size(); i++) {
        if (r.depth[i] >= 1.0f) continue;
        const float w = objmv::view_distance(r.depth[i] * 2 - 1, a, b);
        out[i] = -2;
        for (int d = 0; d < kDraws; d++)
            if (w >= kNear[d] && w <= kFar[d]) out[i] = d;
    }
    return out;
}

struct Score {
    int covered = 0, written = 0, bad = 0, bad_kept = 0, other = 0;
    int per[kDraws]{};
    double worst = 0;
};
/// The actor pass against the camera's motion pass (dlss_objmv.h) of a world that moved as each draw did: at a pixel the scene shows
/// one of the actor pass's draws, its motion; anywhere else, what was under the pass (kSentinel)
static Score score(const Frame& f, const Result& r) {
    const Pose now = pose_now(), before = f.still ? pose_now() : pose_before();
    float cur[16], a = 0, b = 0;
    to_float(camera_now(), cur);
    CHECK(objmv::depth_terms(cur, a, b));
    const std::vector<int> cls = classes(r);
    std::vector<float> want[kDraws];
    for (int d = 0; d < kDraws; d++) {   // (clip to the frame before's clip of what moved with draw d)
        const M4 moved = mul(skin(before, d), inverse(skin(now, d)));
        float clip[16];
        to_float(mul(mul(camera_before(), moved), inverse(camera_now())), clip);
        std::vector<float> dist;
        objmv::motion_pass(W, H, r.depth.data(), clip, a, b, false, false, nullptr, want[d], dist, f.jx, f.jy);
    }
    Score s;
    for (size_t i = 0; i < cls.size(); i++) {
        const float gx = r.motion[i * 2], gy = r.motion[i * 2 + 1];
        const bool kept = gx == kSentinel[0] && gy == kSentinel[1];
        s.written += !kept;
        const int d = cls[i];
        if (d == -2) s.other++;
        if (d >= 0) s.per[d]++;
        if (d < 0 || !f.in_actor[d]) {
            s.bad_kept += !kept;
            continue;
        }
        s.covered++;
        const float ex = want[d][i * 2], ey = want[d][i * 2 + 1];
        const double err = std::max(std::fabs(gx - ex), std::fabs(gy - ey));
        const double tol = 0.02 + std::max(std::fabs(ex), std::fabs(ey)) / 400;   // (half's rounding, float against double)
        if (err > tol) s.bad++;
        else s.worst = std::max(s.worst, err);
    }
    std::printf("%-28s covered %5d written %5d (occluder %d wall %d rigid %d blended %d) off %d, kept wrongly %d, unclassed %d, "
                "worst kept error %.4f px\n",
                f.what, s.covered, s.written, s.per[0], s.per[1], s.per[2], s.per[3], s.bad, s.bad_kept, s.other, s.worst);
    return s;
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
    VkPhysicalDeviceVulkan13Features h13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features h12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    h12.pNext = &h13;
    VkPhysicalDeviceFeatures2 have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    have.pNext = &h12;
    vkGetPhysicalDeviceFeatures2(phys, &have);
    if (!h12.bufferDeviceAddress || !have.features.shaderInt64 || !h13.dynamicRendering)
        return std::printf("%s: no buffer device addresses, 64-bit integers or dynamic rendering: skipped\n", props.deviceName), 0;
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> fams(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, fams.data());
    uint32_t family = 0;
    while (family < nq && !(fams[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) family++;
    if (family == nq) return std::printf("no graphics queue: skipped\n"), 0;
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family, qi.queueCount = 1, qi.pQueuePriorities = &prio;
    VkPhysicalDeviceVulkan13Features w13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    w13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceVulkan12Features w12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    w12.bufferDeviceAddress = VK_TRUE, w12.pNext = &w13;
    VkPhysicalDeviceFeatures2 want{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    want.features.shaderInt64 = VK_TRUE, want.pNext = &w12;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &want, dci.queueCreateInfoCount = 1, dci.pQueueCreateInfos = &qi;
    if (vkCreateDevice(phys, &dci, nullptr, &device) != VK_SUCCESS) return std::printf("device not made: skipped\n"), 0;
    std::printf("device: %s\n", props.deviceName);

    Gpu g{};
    vkGetDeviceQueue(device, family, 0, &g.queue);
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.queueFamilyIndex = family;
    vkCreateCommandPool(device, &cpi, nullptr, &g.pool);
    VkDescriptorSetLayoutBinding bind[2]{};   // (make_actor_pipeline's, as a plain set: the fixture has no push descriptors)
    bind[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bind[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 2, li.pBindings = bind;
    VkDescriptorSetLayout set_layout;
    vkCreateDescriptorSetLayout(device, &li, nullptr, &set_layout);
    const VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(dlss_actor::Push)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1, pli.pSetLayouts = &set_layout, pli.pushConstantRangeCount = 1, pli.pPushConstantRanges = &pr;
    vkCreatePipelineLayout(device, &pli, nullptr, &g.layout);
    g.scene = pipeline(g.layout, false), g.actor = pipeline(g.layout, true);
    CHECK(g.scene && g.actor);
    if (!g.scene || !g.actor) return 1;
    g.depth = image(VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    g.motion = image(VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    const VkBufferUsageFlags by_address = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    g.head = buffer(dlss_actor::kHeadWords * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    g.poses[0] = buffer(64 << 10, by_address), g.poses[1] = buffer(64 << 10, by_address);
    g.readback = buffer((VkDeviceSize)W * H * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkDeviceSize at = 0;   // (the game's vertex and index streams, in one buffer, each 256-aligned)
    for (int d = 0; d < kDraws; d++) {
        CHECK(dlss_actor::fits(the_mesh(d).l));
        g.vertex_at[d] = at, at += (the_mesh(d).vertices.size() + 255) & ~255ull;
        g.index_at[d] = at, at += (the_mesh(d).indices.size() + 255) & ~255ull;
    }
    g.geometry = buffer(at, by_address);
    for (int d = 0; d < kDraws; d++) {
        const Mesh& m = the_mesh(d);
        std::memcpy(g.geometry.map + g.vertex_at[d], m.vertices.data(), m.vertices.size());
        std::memcpy(g.geometry.map + g.index_at[d], m.indices.data(), m.indices.size());
    }
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};   // (ds.nearest)
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler nearest;
    vkCreateSampler(device, &sci, nullptr, &nearest);
    const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 1, dpi.poolSizeCount = 2, dpi.pPoolSizes = sizes;
    VkDescriptorPool dpool;
    vkCreateDescriptorPool(device, &dpi, nullptr, &dpool);
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = dpool, dsai.descriptorSetCount = 1, dsai.pSetLayouts = &set_layout;
    vkAllocateDescriptorSets(device, &dsai, &g.set);
    const VkDescriptorBufferInfo bi{g.head.buffer, 0, dlss_actor::kHeadWords * 4};
    const VkDescriptorImageInfo ii{nearest, g.depth.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    w[0].dstSet = g.set, w[0].dstBinding = 0, w[0].descriptorCount = 1, w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[0].pBufferInfo = &bi;
    w[1].dstSet = g.set, w[1].dstBinding = 1, w[1].descriptorCount = 1, w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[1].pImageInfo = &ii;
    vkUpdateDescriptorSets(device, 2, w, 0, nullptr);

    // ---- the frames
    const float jx = 0.37f, jy = -0.29f;
    const auto right = [](const char* what, float x, float y, bool still) {   // (as vk_draw.cpp: dlss_actor_pass's head and viewport)
        Frame f{what, x, y, x, y, kRight, still, {true, true, true, true}, {true, false, true, true}};
        return f;
    };
    // the actors moving, the camera too, the scene jittered: every pixel of theirs is the motion of the world that moved with them
    const Frame moving = right("moving, jittered", jx, jy, false);
    const Result rm = run(g, moving);
    const Score sm = score(moving, rm);
    CHECK(sm.other == 0);
    for (int d = 0; d < kDraws; d++) CHECK(sm.per[d] > 400);   // (each draw is on screen)
    CHECK(sm.covered > 3000 && sm.bad * 1000 <= sm.covered && sm.bad_kept == 0);
    CHECK(sm.written == sm.covered);
    // still actors: the camera's motion, as the motion pass gives it; and the other jitter, unjittered
    const Frame still = right("still, jittered the other way", -0.41f, 0.23f, true);
    const Score ss = score(still, run(g, still));
    CHECK(ss.covered > 3000 && ss.bad * 1000 <= ss.covered && ss.bad_kept == 0 && ss.other == 0);
    const Frame flat = right("moving, no jitter", 0, 0, false);
    const Score sf = score(flat, run(g, flat));
    CHECK(sf.covered > 3000 && sf.bad * 1000 <= sf.covered && sf.bad_kept == 0 && sf.other == 0);
    // what is in front is the scene's: the occluder hides part of the rigid one (an actor drawn before it), the wall part of the
    // blended one (not an actor: the camera's motion stays), the rigid one part of the blended one (drawn after it). Without them
    // in the scene, those pixels are the hidden ones'
    Frame bare = moving;
    bare.what = "moving, nothing in front", bare.in_scene[kOccluder] = bare.in_scene[kWall] = false;
    bare.in_actor[kOccluder] = false;
    const Result rb = run(g, bare);
    const Score sb = score(bare, rb);
    CHECK(sb.covered > 3000 && sb.bad * 1000 <= sb.covered && sb.bad_kept == 0 && sb.other == 0);
    Frame alone = bare;
    alone.what = "moving, the blended one alone", alone.in_scene[kRigid] = alone.in_actor[kRigid] = false;
    const Result ra = run(g, alone);
    const Score sa = score(alone, ra);
    CHECK(sa.covered > 1000 && sa.bad * 1000 <= sa.covered && sa.bad_kept == 0 && sa.other == 0);
    {
        const std::vector<int> cm = classes(rm), cb = classes(rb), ca = classes(ra);
        int under_occluder = 0, under_wall = 0, under_rigid = 0;
        for (size_t i = 0; i < cm.size(); i++) {
            under_occluder += cm[i] == kOccluder && cb[i] == kRigid;
            under_wall += cm[i] == kWall && cb[i] == kBlended;
            under_rigid += cb[i] == kRigid && ca[i] == kBlended;
        }
        std::printf("hidden: %d of the rigid one's pixels under the occluder, %d of the blended one's under the wall, %d under the "
                    "rigid one\n", under_occluder, under_wall, under_rigid);
        CHECK(under_occluder > 100 && under_wall > 100 && under_rigid > 100);
    }
    // the scene's depth a few steps off the pass's (the game's shader is not the pass's): still the draw's; far off: not
    Frame off16 = moving;
    off16.what = "scene depth 16 steps off", off16.bias = 16;
    const Score s16 = score(off16, run(g, off16));
    CHECK(s16.covered > 3000 && s16.bad * 1000 <= s16.covered && s16.bad_kept == 0 && s16.written == s16.covered);
    Frame off400 = moving;
    off400.what = "scene depth 400 steps off", off400.bias = 400;
    const Score s400 = score(off400, run(g, off400));
    CHECK(s400.written * 10 < sm.written);
    // three wrong wirings must fail. The pass without the scene's viewport offset: its depths do not meet the scene's (tilted
    // surfaces), nearly nothing is drawn
    Frame no_offset = moving;
    no_offset.what = "wrong: no viewport offset", no_offset.ax = no_offset.ay = 0;
    const Score sn = score(no_offset, run(g, no_offset));
    CHECK(sn.written * 10 < sm.written);
    // the jitter's y taken off the wrong way (window y down, the game's clip y up): every vector off by 2 jy
    Frame y_sign = moving;
    y_sign.what = "wrong: jitter y sign", y_sign.head = kYSign;
    const Score sy = score(y_sign, run(g, y_sign));
    CHECK(sy.bad * 10 > sy.covered * 9);
    // the jitter not taken off (#841's head with this branch's unjittered camera would be this): every vector off by the jitter
    Frame kept = moving;
    kept.what = "wrong: jitter not taken off", kept.head = kNoJitter;
    const Score sk = score(kept, run(g, kept));
    CHECK(sk.bad * 10 > sk.covered * 9);

    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(inst, nullptr);
    if (failures) return std::printf("%d failures\n", failures), 1;
    std::printf("PASS: the actor pass's vectors are the camera motion pass's of a world that moved with each draw, jittered or not\n");
    return 0;
}
