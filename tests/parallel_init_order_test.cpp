// A parallel replay's new images in queue order: the production upload command buffer state, create()'s initialization,
// bc_scratch_layout, the texture segment hooks, replay_handle's segment jobs and submits and the init pools' startup,
// included from the init_*.inc files tests/test_parallel_init_order.py cuts from the source. Vulkan is a spy: commands
// are recorded per command buffer (never into one not begun, ended or submitted), and the submitted ones are played in
// queue order, the image layouts tracked as the GPU would see them. The workers run on threads of their own: one after
// another in the order a case sets (the one that makes an image first need not be the first segment in the queue), or
// all at once, held at fixed points by latches (a worker waits for another to be inside its init recording, or for an
// earlier segment's submit).
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
#include <set>
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
std::atomic<int> failures{0};
#define CHECK(v) do { if (!(v)) { std::printf("FAIL:%d %s\n", __LINE__, #v); std::fflush(stdout); std::_Exit(1); } } while (0)

struct Latch {
    std::mutex mu;
    std::condition_variable cv;
    bool set = false;
    void post() {
        {
            std::lock_guard l(mu);
            set = true;
        }
        cv.notify_all();
    }
    void wait() {
        std::unique_lock l(mu);
        CHECK(cv.wait_for(l, std::chrono::seconds(10), [&] { return set; }));
    }
};
Latch latch[128];   // (by letter. x: a worker is inside the texture's initialization, y: let it go on, z: segment 0 submitted)

const VkImage kTex = reinterpret_cast<VkImage>(uintptr_t(0x10)), kScratch = reinterpret_cast<VkImage>(uintptr_t(0x20)),
              kScratch2 = reinterpret_cast<VkImage>(uintptr_t(0x30));
const char* image_name(VkImage i) {
    return i == kTex ? "t" : i == kScratch ? "s" : i == kScratch2 ? "s2" : "?";
}
bool hold_in_init;   // (close_race: the texture's initialization waits there, its lock held, for the other worker)
void record(VkCommandBuffer cb, char kind, VkImage image = VK_NULL_HANDLE) {
    CHECK(cb && cb->begun && !cb->ended && !cb->submitted);
    cb->ops.push_back({kind, image});
}
void vkCmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo* di) {
    for (uint32_t i = 0; i < di->imageMemoryBarrierCount; i++) {
        const VkImageMemoryBarrier2& b = di->pImageMemoryBarriers[i];
        const bool first = b.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && b.newLayout == VK_IMAGE_LAYOUT_GENERAL;
        record(cb, first ? 'T' : 'B', b.image);
        if (first && b.image == kTex && hold_in_init) {
            latch['x'].post();
            latch['y'].wait();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));   // (the other worker closing its segment meanwhile)
        }
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
std::string fail_begin;   // (init_begin_fail: this command buffer cannot be begun)
VkResult vkBeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo*) {
    CHECK(cb);
    if (cb->name == fail_begin) return VK_ERROR_OUT_OF_HOST_MEMORY;
    cb->ops.clear(), cb->begun = true, cb->ended = false;
    return VK_SUCCESS;
}
VkResult vkEndCommandBuffer(VkCommandBuffer cb) {
    CHECK(cb && cb->begun && !cb->ended);
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
            if (cb->name == "cmd_0") latch['z'].post();
        }
    return VK_SUCCESS;
}
std::mutex cbs_mu;
VkCommandBuffer make_cb(const char* what, int k) {
    auto* cb = new VkCommandBuffer_T;
    cb->name = std::string(what) + "_" + std::to_string(k);
    std::lock_guard l(cbs_mu);
    all_cbs.push_back(cb);
    return cb;
}
// (the init pools' startup: each pool creation and allocation can be made to fail, its output left as garbage)
const uintptr_t kGarbage = 0xBAD0;
int alloc_calls, fail_at;
std::set<uintptr_t> live_pools;
uintptr_t next_pool = 0x1000;
VkResult vkCreateCommandPool(VkDevice, const VkCommandPoolCreateInfo*, const VkAllocationCallbacks*, VkCommandPool* p) {
    if (++alloc_calls == fail_at) {
        *p = reinterpret_cast<VkCommandPool>(kGarbage);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    live_pools.insert(next_pool);
    *p = reinterpret_cast<VkCommandPool>(next_pool++);
    return VK_SUCCESS;
}
VkResult vkAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo* ai, VkCommandBuffer* out) {
    CHECK(ai->commandBufferCount == 1 && live_pools.count(reinterpret_cast<uintptr_t>(ai->commandPool)));
    if (++alloc_calls == fail_at) {
        *out = reinterpret_cast<VkCommandBuffer>(kGarbage);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    *out = make_cb("pool", alloc_calls);
    return VK_SUCCESS;
}
void vkDestroyCommandPool(VkDevice, VkCommandPool p, const VkAllocationCallbacks*) {
    CHECK(p && live_pools.erase(reinterpret_cast<uintptr_t>(p)));   // (only a live one, once)
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

// ---- the BC3 scratch, and the second one (an upload at another size) ----
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
bool bc_scratch2_make() {
    bc_scratch2 = kScratch2, bc_scratch2_fresh = true;
    return true;
}
#include "init_scratch.inc"
void scratch_use(bool resized) {   // (an ASTC/BC3 upload: decoded into the scratch (and blitted into the second), in this thread's uploads)
    std::lock_guard lk(par_lock);
    CHECK(bc_scratch_layout(upload_cmd(), resized));
    record(upload_cmd(), 'U', kScratch);
    if (resized) record(upload_cmd(), 'U', kScratch2);
}

// ---- replay_handle's segments ----
#include "init_recording.inc"
struct Segment {
    // t: the texture (made if new, uploaded, drawn with), s: the scratch, S: both scratches, -: nothing,
    // x / z: wait for latch x (a worker inside the texture's initialization) / z (segment 0 submitted),
    // L: the initialization holds the segments' init lock (seg_init_mu) while it records; then latch y lets it go on
    std::string script;
};
void record_segment(VkCommandBuffer& cmd, const Segment& g) {
    for (char a : g.script) {
        if (a == 't') texture_use(), record(cmd, 'U', kTex);
        if (a == 's' || a == 'S') scratch_use(a == 'S');
        if (a == 'x' || a == 'z') latch[(int)a].wait();
        if (a == 'L') {
#if HAVE_SEG_INIT
            if (seg_init_mu.try_lock()) {
                seg_init_mu.unlock();
                std::printf("  FAIL: the init lock is free while another worker records a new image's initialization\n");
                failures++;
            }
#endif
            latch['y'].post();
        }
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
std::vector<int> run_order;   // (the order the workers run in, one after another; empty: all at once)
std::vector<std::thread> workers;
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
    if (run_order.empty()) {   // (at once, as par_worker_main runs them; the replay thread submits meanwhile)
        for (auto& job : jobs)
            workers.emplace_back([job] {
                job();
                {
                    std::lock_guard lk(par_pool.mu);
                    par_pool.pending--;
                }
                par_pool.done.notify_all();
            });
        return;
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
    for (auto& w : workers) w.join();
}
#include "init_pools.inc"
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
VkCommandBuffer_T* named(const char* name) {
    for (auto* cb : all_cbs)
        if (cb->name == name) return cb;
    return nullptr;
}
/// Which command buffer holds image i's UNDEFINED -> GENERAL change ("": none)
std::string init_of(VkImage i) {
    for (auto* cb : all_cbs)
        for (const Op& o : cb->ops)
            if (o.kind == 'T' && o.image == i) return cb->name;
    return "";
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
    } else if (c == "scratch2") {   // (segment 1 makes the scratch, then the second one; segment 0 uses both; then both fresh at once)
        submit_segments({{"S"}, {"sS"}}, {1, 0});
        play(c.c_str());
        queue.clear(), all_cbs.clear(), recording_count = 0;
        bc_scratch = bc_scratch2 = VK_NULL_HANDLE, bc_have = false;
        submit_segments({{"S"}, {"S"}}, {1, 0});
        play("scratch2 (both fresh)");
    } else if (c == "in_order") {   // (made by the first segment, as serial replay would)
        submit_segments({{"t"}, {"t"}}, {0, 1});
        play(c.c_str());
    } else if (c == "serial") {   // (no segments: the replay thread's uploads get exactly the calls they always did)
        VkCommandBuffer up = make_cb("upload", 0);
        vkBeginCommandBuffer(up, nullptr);
        texture_begin_recording(up, 1);
        texture_use();
        scratch_use(false);
        scratch_use(true);
        CHECK(texture_uploads_recorded());
        vkEndCommandBuffer(up);
        const std::string got = ops_text(up), want = "F Tt Ct Bt Ut Ts Us Ts2 Us Us2 F";
        std::printf("serial: %s[%s]\n", up->name.c_str(), got.c_str());
        if (got != want) std::printf("  FAIL: serial uploads changed, want [%s]\n", want.c_str()), failures++;
        for (auto* cb : all_cbs)
            if (cb != up && !cb->ops.empty()) std::printf("  FAIL: serial replay recorded into %s\n", cb->name.c_str()), failures++;
    } else if (c == "close_race") {   // (at once: segment 1 records the texture's initialization into segment 0's init command
                                      // buffer, holding the lock, while segment 0's worker finishes and closes it)
        hold_in_init = true;
        submit_segments({{"xL"}, {"t"}}, {});
        play(c.c_str());
        if (HAVE_SEG_INIT && init_of(kTex) != "init_0")
            std::printf("  FAIL: the texture's initialization is in %s, not init_0\n", init_of(kTex).c_str()), failures++;
    } else if (c == "after_submit") {   // (at once: segment 1 makes the texture only after segment 0 was submitted)
        submit_segments({{"-"}, {"zt"}}, {});
        play(c.c_str());
        if (named("init_0") && !named("init_0")->ops.empty())
            std::printf("  FAIL: init_0 (submitted) was recorded into\n"), failures++;
    } else if (c == "init_begin_fail") {   // (segment 0's init command buffer cannot be begun: its new images go to its uploads)
        fail_begin = "init_0";
        submit_segments({{"t"}, {"t"}}, {0, 1});
        play(c.c_str());
        if (init_of(kTex) != "upload_0")
            std::printf("  FAIL: the texture's initialization is in %s, not upload_0\n", init_of(kTex).c_str()), failures++;
    } else if (c == "alloc_fail") {   // (init_pools_make: each of its pool creations and allocations failing in turn)
#if HAVE_INIT_POOLS
        const int n = (int)(sizeof(recordings) / sizeof(recordings[0]));
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        for (int f = 1; f <= 2 * n + 1; f++) {   // (the last: none fails)
            alloc_calls = 0, fail_at = f <= 2 * n ? f : 0;
            const bool ok = init_pools_make(pi);
            if (ok != !fail_at) std::printf("  FAIL: step %d failing, init_pools_make returned %d\n", f, ok), failures++;
            for (const Recording& r : recordings)
                if (ok ? (!r.init_pool || !r.init) : (r.init_pool || r.init))
                    std::printf("  FAIL: step %d failing, a recording's init pool/buffer is %p/%p\n", f, (void*)r.init_pool, (void*)r.init),
                        failures++;
            if (!ok && !live_pools.empty())
                std::printf("  FAIL: step %d failing, %zu init pools left made\n", f, live_pools.size()), failures++;
        }
        if (live_pools.size() != (size_t)n) std::printf("  FAIL: %zu init pools made, want %d\n", live_pools.size(), n), failures++;
        init_pools_destroy();
        if (!live_pools.empty()) std::printf("  FAIL: init pools left after init_pools_destroy\n"), failures++;
        std::printf("alloc_fail: each of the %d steps failing gives back every pool made and leaves no handle; none failing makes %d\n",
                    2 * n, n);
#else
        std::printf("alloc_fail: no init pools in this code\n");
#endif
    } else {
        std::printf("unknown case: %s\n", c.c_str());
        return 2;
    }
    std::fflush(stdout);
    return failures ? 1 : 0;
}
