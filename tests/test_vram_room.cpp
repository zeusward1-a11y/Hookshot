// vram_room (gpu/vk/vram_room.h): the textures' room in video memory, with the numbers of 09/11's logs. tests/test_vram_room.py
#include "../gpu/vk/vram_room.h"

#include <cstdio>

static int failures = 0;
static void expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        failures++;
    }
}
static constexpr uint64_t MB = 1ull << 20;

int main() {
    using namespace vram_room;
    // the switch
    expect(target_pct(nullptr) == 85 && target_pct("") == 85 && target_pct("x") == 85 && target_pct("80x") == 85, "default 85");
    expect(target_pct("80") == 80 && target_pct("0") == 0 && target_pct("99") == 97 && target_pct("-3") == 85, "set, off, capped");
    // Hyrule Field after two minutes (09/11, RTX 5070, DLSS-G): 9754 MB used of 10388 allowed, 95 free in the blocks, textures
    // 5525: the rest 4134 MB; 85% of 10388 is 8829 MB (10388/100*85 in bytes), so the textures' room is ~4695 MB, under the
    // 6144 MB they had and the 5525 they held: the least used go before the squeeze at 90% (9349 MB) begins
    const uint64_t used = 9754 * MB, allowed = 10388 * MB, free = 95 * MB, tex = 5525 * MB;
    expect(others(used, tex, free) == 4134 * MB, "the rest: used less the textures and the blocks' free space");
    const uint64_t r = room(used, allowed, free, tex, 85);
    expect(r > 4690 * MB && r < 4700 * MB, "the field's room ~4695 MB");
    expect(r + others(used, tex, free) <= allowed / 10 * 9, "textures at their room stay under the squeeze at 90%");
    // the room does not move as textures are evicted (the bytes go to the blocks' free space) or made (from it)
    expect(room(used, allowed, free + 1000 * MB, tex - 1000 * MB, 85) == r, "evicting 1000 MB of textures: the same room");
    expect(room(used + 300 * MB, allowed, free, tex + 300 * MB, 85) == r, "a new block for textures: the same room");
    // ...and shrinks when the rest grows (DLSS-G's resources made, a mirror grows)
    expect(room(used + 500 * MB, allowed, free, tex, 85) == r - 500 * MB, "the rest 500 MB more: 500 MB less room");
    // never under the floor; off; nothing known
    expect(room(10000 * MB, allowed, 0, 100 * MB, 85) == kFloor, "the rest past the target: the floor");
    expect(room(used, allowed, free, tex, 0) == UINT64_MAX && room(used, 0, free, tex, 85) == UINT64_MAX, "off, or unknown");
    // a 16 GB card with the same rest: the room past the 6 GB cap, which then holds
    expect(room(used, 15000 * MB, free, tex, 85) > 6144 * MB, "a bigger card: the cap of 6 GB holds, not the room");
    if (failures) return std::printf("vram_room: %d failures\n", failures), 1;
    std::printf("vram_room: ok\n");
    return 0;
}
