// A parallel replay's new images in queue order: the production upload command buffer state, create()'s initialization,
// bc_scratch_layout, the texture segment hooks and replay_handle's segment jobs and submits, included from the init_*.inc
// files tests/test_parallel_init_order.py cuts from the source. Vulkan is a spy: commands are recorded per command
// buffer, and the submitted ones are played in queue order, the image layouts tracked as the GPU would see them.
// The workers run one after another on threads of their own, in the order a case sets (the one that makes an image
// first need not be the first segment in the queue).
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct Op {
    char kind;   // F: full barrier, T: UNDEFINED -> GENERAL, B: another image barrier, C: clear, U: a use (copy, dispatch, draw)
    VkImage image;
};
struct VkCommandBuffer_T {
    std::string name;
    std::vector<Op> ops;
    bool begun = false, ended = false, submitted = false;
};
std::vector<VkCommandBuffer_T*> all_cbs, queue;
int failures;
#define CHECK(v) do { if (!(v)) { std::printf("FAIL:%d %s\n", __LINE__, #v); std::exit(1); } } while (0)

const VkImage kTex = reinterpret_cast<VkImage>(uintptr_t(0x10)), kScratch = reinterpret_cast<VkImage>(uintptr_t(0x20));
const char* image_name(VkImage i) {
    return i == kTex ? "t" : i == kScratch ? "s" : "?";
}
void record(VkCommandBuffer cb, char kind, VkImage image = VK_NULL_HANDLE) {
    CHECK(cb && cb->begun && !cb->ended);
    cb->ops.push_back({kind, image});
}
void vkCmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo* di) {
    for (uint32_t i = 0; i < di->imageMemoryBarrierCount; i++) {
        const VkImageMemoryBarrier2& b = di->pImageMemoryBarriers[i];
        record(cb, b.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && b.newLayout == VK_IMAGE_LAYOUT_GENERAL ? 'T' : 'B', b.image);
    }
}
void vkCmdClearColorImage(VkCommandBuffer cb, VkImage i, VkImageLayout, const VkClearColorValue*, uint32_t, const VkImageSubresourceRange*) {
    record(cb, 'C', i);
}
void vkCmdClearDepthStencilImage(VkCommandBuffer cb, VkImage i, VkImageLayout, const VkClearDepthStencilValue*, uint32_t,
                                 const VkImageSubresourceRange*) {
    record(cb, 'C', i);
}
VkResult vkResetCommandPool(VkDevice, VkCommandPool, VkCommandPoolResetFlags) {
    return VK_SUCCESS;
}
VkResult vkBeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo*) {
    cb->ops.clear(), cb->begun = true, cb->ended = false;
    return VK_SUCCESS;
}
VkResult vkEndCommandBuffer(VkCommandBuffer cb) {
    CHECK(cb->begun && !cb->ended);
    cb->ended = true;
    return VK_SUCCESS;
}
VkResult vkQueueSubmit2(VkQueue, uint32_t n, const VkSubmitInfo2* si, VkFence) {
    for (uint32_t s = 0; s < n; s++)
        for (uint32_t i = 0; i < si[s].commandBufferInfoCount; i++) {
            VkCommandBuffer cb = si[s].pCommandBufferInfos[i].commandBuffer;
            CHECK(cb && cb->ended && !cb->submitted);
            cb->submitted = true;
            queue.push_back(cb);
        }
    return VK_SUCCESS;
}
VkCommandBuffer make_cb(const char* what, int k) {
    auto* cb = new VkCommandBuffer_T;
    cb->name = std::string(what) + "_" + std::to_string(k);
    all_cbs.push_back(cb);
    return cb;
}

namespace mkwvk {
void logf(const char*, ...) {}
void full_barrier(VkCommandBuffer cmd, const char* = "") {
    record(cmd, 'F');
}
void memory_table_begin() {}
bool memory_table_flush() { return false; }
bool memory_rb_refresh(VkCommandBuffer) { return false; }
void memory_rb_recording_begun() {}
VkCommandBuffer last_barrier_cmd = VK_NULL_HANDLE;
bool is_bc(VkFormat) { return false; }
bool is_astc(VkFormat) { return false; }
#include "init_upload.inc"
#include "init_hooks.inc"

// ---- create()'s initialization of a new image ----
struct Texture {
    VkImage image;
};
std::vector<std::unique_ptr<Texture>> textures;
std::mutex par_lock;   // (texture_get's ParLock: an image is made once, by whichever worker gets there first)
Texture* create(VkImage image) {
    struct { VkImage image; VkFormat format; VkImageAspectFlags aspect; } x{image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT};
    const bool depth = false;
    auto tex = std::make_unique<Texture>(Texture{image});
#include "init_create.inc"
    textures.push_back(std::move(tex));
    return p;
}
void texture_use() {   // (texture_get, and texture_touch's upload again: a copy into it in this thread's uploads)
    std::lock_guard lk(par_lock);
    if (textures.empty()) create(kTex);
    record(upload_cmd(), 'U', kTex);
}

// ---- the BC3 scratch ----
std::mutex bc_mu;
std::chrono::steady_clock::time_point bc_used_at;
uint64_t bc_last_value = 0;
std::atomic<bool> bc_have{false};
constexpr uint32_t kBcLayers = 8;
VkImage bc_scratch = VK_NULL_HANDLE, bc_scratch2 = VK_NULL_HANDLE;
bool bc_scratch_fresh = false, bc_scratch2_fresh = false;
bool bc_scratch_make() {
    bc_scratch = kScratch, bc_scratch_fresh = true, bc_have = true;
    return true;
}
bool bc_scratch2_make() { return false; }
#include "init_scratch.inc"
void scratch_use() {   // (an ASTC/BC3 upload: the scratch decoded into, in this thread's uploads)
    std::lock_guard lk(par_lock);
    CHECK(bc_scratch_layout(upload_cmd()));
    record(upload_cmd(), 'U', kScratch);
}

// ---- replay_handle's segments ----
#include "init_recording.inc"
struct Segment {
    std::string script;   // t: the texture (made if new, uploaded, drawn with), s: the scratch, -: nothing
};
void record_segment(VkCommandBuffer& cmd, const Segment& g) {
    for (char a : g.script) {
        if (a == 't') texture_use(), record(cmd, 'U', kTex);
        if (a == 's') scratch_use();
    }
}
void draw_begin_recording(int, VkCommandBuffer, uint64_t) {}
void queue_flush_locked() {}
void checkpoints_report() {}
struct {
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::mutex present_mu;
} dev;
std::atomic<int64_t> par_lock_wait_ns{0}, par_locks{0};
std::atomic<bool> par_active{false};
std::vector<int> run_order;   // (the order the workers run in, one after another)
struct RunJobs {
    void notify_all();
};
struct {
    std::mutex mu;
    std::vector<std::function<void()>> jobs;
    int pending = 0;
    RunJobs cv;
    std::condition_variable done;
} par_pool;
void RunJobs::notify_all() {
    std::vector<std::function<void()>> jobs;
    {
        std::lock_guard lk(par_pool.mu);
        jobs.swap(par_pool.jobs);
    }
    CHECK(jobs.size() == run_order.size());
    for (int k : run_order) std::thread(jobs[k]).join();
    std::lock_guard lk(par_pool.mu);
    par_pool.pending = 0;
}
Recording recordings[8];
int recording_count = 0;
uint64_t rec_cur_value = 1;
Recording& begin_segment_recording(uint64_t) {
    const int k = recording_count++;
    Recording& r = recordings[k];
    r.cmd = make_cb("cmd", k), r.upload = make_cb("upload", k);
    [k](auto& x) {   // (the fixed code's own command buffer for its new images)
        if constexpr (requires { x.init; }) x.init = make_cb("init", k);
    }(r);
    vkBeginCommandBuffer(r.cmd, nullptr);   // (as begin_segment_recording does; the job begins the uploads)
    r.value = 100 + k, r.slot = k;
    return r;
}
void submit_segments(std::vector<Segment> seg, std::vector<int> order) {
    run_order = order;
    const uint64_t base = 1;
#include "init_jobs.inc"
}
}  // namespace mkwvk

std::string ops_text(const VkCommandBuffer_T* cb) {
    std::string s;
    for (const Op& o : cb->ops) s += std::string(s.empty() ? "" : " ") + o.kind + (o.image ? image_name(o.image) : "");
    return s;
}
/// The submitted command buffers in queue order: each image has one UNDEFINED -> GENERAL change, before every use
void play(const char* name) {
    std::string order;
    for (auto* cb : queue) order += " " + cb->name + "[" + ops_text(cb) + "]";
    std::printf("%s: queue%s\n", name, order.c_str());
    std::map<VkImage, int> made, early;
    for (auto* cb : queue)
        for (const Op& o : cb->ops) {
            if (!o.image) continue;
            if (o.kind == 'T') {
                if (made[o.image]++)
                    std::printf("  FAIL: image %s made UNDEFINED again in %s (what came before is discarded)\n", image_name(o.image),
                                cb->name.c_str()), failures++;
            } else if (!made[o.image] && !early[o.image]++) {
                std::printf("  FAIL: %s uses image %s (%c) while it is still UNDEFINED in the queue (VUID-vkCmdDraw-None-09600)\n",
                            cb->name.c_str(), image_name(o.image), o.kind), failures++;
            }
        }
    for (auto [image, n] : made)
        if (n != 1) std::printf("  FAIL: image %s changed from UNDEFINED %d times\n", image_name(image), n), failures++;
    for (auto* cb : all_cbs)
        for (const Op& o : cb->ops)
            if (o.kind != 'F' && !cb->submitted) {
                std::printf("  FAIL: %s has commands (%c%s) and was never submitted\n", cb->name.c_str(), o.kind, image_name(o.image));
                failures++;
                break;
            }
}

int main(int argc, char** argv) {
    using namespace mkwvk;
    const std::string c = argc > 1 ? argv[1] : "";
    if (c == "later_makes") {   // (segment 1's worker makes the texture, then segment 0's uploads it again and draws with it)
        submit_segments({{"t"}, {"t"}}, {1, 0});
        play(c.c_str());
    } else if (c == "three") {   // (segment 0 is done; 2 makes it while 1 still records, and 1 uses it)
        submit_segments({{"-"}, {"t"}, {"t"}}, {0, 2, 1});
        play(c.c_str());
    } else if (c == "scratch") {   // (the BC3 scratch: made by segment 1's worker, used by segment 0's too)
        submit_segments({{"s"}, {"s"}}, {1, 0});
        play(c.c_str());
    } else if (c == "in_order") {   // (made by the first segment, as serial replay would)
        submit_segments({{"t"}, {"t"}}, {0, 1});
        play(c.c_str());
    } else if (c == "serial") {   // (no segments: the replay thread's uploads get exactly the calls they always did)
        VkCommandBuffer up = make_cb("upload", 0);
        vkBeginCommandBuffer(up, nullptr);
        texture_begin_recording(up, 1);
        texture_use();
        scratch_use();
        CHECK(texture_uploads_recorded());
        vkEndCommandBuffer(up);
        const std::string got = ops_text(up), want = "F Tt Ct Bt Ut Ts Us F";
        std::printf("serial: %s[%s]\n", up->name.c_str(), got.c_str());
        if (got != want) std::printf("  FAIL: serial uploads changed, want [%s]\n", want.c_str()), failures++;
        for (auto* cb : all_cbs)
            if (cb != up && !cb->ops.empty()) std::printf("  FAIL: serial replay recorded into %s\n", cb->name.c_str()), failures++;
    } else {
        std::printf("unknown case: %s\n", c.c_str());
        return 2;
    }
    return failures ? 1 : 0;
}
