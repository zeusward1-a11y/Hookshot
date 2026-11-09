// gpu/vk/spirv_xfb.h on a file: spirv_xfb_tool <in.spv> <out.spv>. Prints "ok" or "no: <why>" (exit 0 either way; 2: I/O).
// Run by tests/test_spirv_xfb.py.
#include <cstdio>
#include <vector>

#include "spirv_xfb.h"

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) return 2;
    std::vector<uint32_t> in;
    uint32_t w;
    while (std::fread(&w, 4, 1, f) == 1) in.push_back(w);
    std::fclose(f);
    std::vector<uint32_t> out;
    std::string why;
    if (!spirv_xfb::patch(in, out, &why)) return std::printf("no: %s\n", why.c_str()), 0;
    FILE* g = std::fopen(argv[2], "wb");
    if (!g) return 2;
    std::fwrite(out.data(), 4, out.size(), g);
    std::fclose(g);
    std::printf("ok\n");
    return 0;
}
