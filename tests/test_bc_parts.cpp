#include "../gpu/vk/bc_parts.h"

#include <cstdio>
#include <vector>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

using namespace mkwvk::bc_parts;

// The upload as vk_texture.cpp's upload_astc_gpu and texture_bc_continue run it: each recording takes runs of layers while its
// share lasts (at least one each call), the rest goes to the next recording. Returns the recordings it took, and checks every
// layer went once and in order, and no recording went past its share by more than the one run each call may always take.
int simulate(const std::vector<uint32_t>& all, uint64_t layer_texels, uint64_t budget, size_t max_run, int* recordings) {
    std::vector<uint32_t> pending = all, done;
    *recordings = 0;
    while (!pending.empty()) {
        ++*recordings;
        uint64_t used = 0;
        std::vector<uint32_t> rest;
        for (size_t r0 = 0; r0 < pending.size();) {
            const size_t r1 = run_end(pending.data(), pending.size(), r0, max_run);
            const uint64_t run = layer_texels * (r1 - r0);
            if (!run_fits(used, run, budget, r0 == 0)) {
                rest.assign(pending.begin() + (long)r0, pending.end());
                break;
            }
            if (r0 > 0 && used + run > budget && budget) return 1;
            used += run;
            for (size_t k = r0; k < r1; k++) done.push_back(pending[k]);
            r0 = r1;
        }
        pending = rest;
        if (*recordings > 10000) return 1;
    }
    return done == all ? 0 : 1;
}

int main() {
    const uint32_t a[] = {0, 1, 2, 3, 7, 8, 9, 20};
    CHECK("a run ends at a gap", run_end(a, 8, 0, 8) == 4);
    CHECK("a run ends at the scratch's layers", run_end(a, 8, 0, 2) == 2);
    CHECK("a run after a gap", run_end(a, 8, 4, 8) == 7);
    CHECK("the last layer alone", run_end(a, 8, 7, 8) == 8);
    CHECK("no budget: everything", run_fits(1u << 30, 1u << 30, 0, false));
    CHECK("the first run always goes", run_fits(100, 1000, 10, true));
    CHECK("the others while they fit", run_fits(10, 6, 16, false) && !run_fits(11, 6, 16, false));

    // (Hyrule Field's terrain array: 90 layers of 1365 x 1365 decoded and 1536 x 1536 encoded, 8 levels: ~5.6 M texels a layer;
    // 8 layers a run, 16 M texels a recording)
    std::vector<uint32_t> terrain;
    for (uint32_t l = 0; l < 90; l++) terrain.push_back(l);
    const uint64_t layer = 5600000;
    int recs = 0;
    CHECK("the terrain array, in parts", simulate(terrain, layer, 16ull << 20, 8, &recs) == 0 && recs == 12);
    CHECK("all at once without a budget", simulate(terrain, layer, 0, 8, &recs) == 0 && recs == 1);
    CHECK("a big budget takes several runs a recording", simulate(terrain, layer, 256ull << 20, 8, &recs) == 0 && recs == 3);
    std::vector<uint32_t> dirty = {3, 4, 5, 40, 41, 89};
    CHECK("dirty layers only", simulate(dirty, layer, 16ull << 20, 8, &recs) == 0 && recs == 3);
    std::puts("bc parts: all checks passed");
}
