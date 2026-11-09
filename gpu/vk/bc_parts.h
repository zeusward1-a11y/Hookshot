/* ASTC textures re-encoded as BC3, uploaded a few runs of layers a recording (vk_texture.cpp's upload_astc_gpu). */
#pragma once

#include <cstddef>
#include <cstdint>

namespace mkwvk::bc_parts {

/// The end of the run of consecutive layers that starts at layers[r0], max_run layers at most
inline size_t run_end(const uint32_t* layers, size_t n, size_t r0, size_t max_run) {
    size_t r1 = r0 + 1;
    while (r1 < n && r1 - r0 < max_run && layers[r1] == layers[r1 - 1] + 1) r1++;
    return r1;
}

/// Whether a run of `run` texels goes into a recording whose upload holds `used` already: the first run of an upload always
/// does (each one makes progress), the others while they fit the budget (0: no budget, all at once)
inline bool run_fits(uint64_t used, uint64_t run, uint64_t budget, bool first) {
    return !budget || first || used + run <= budget;
}

} // namespace mkwvk::bc_parts
