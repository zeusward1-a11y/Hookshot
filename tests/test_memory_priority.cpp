// memory_priority (gpu/vk/memory_priority.h): the switches and when a texture drops to the idle priority.
// tests/test_memory_priority.py
#include "../gpu/vk/memory_priority.h"

#include <cstdio>
#include <cstdlib>

static int failures = 0;
static void expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        failures++;
    }
}

int main() {
    using namespace memory_priority;
    expect(kIdle > 0.0f && kIdle < kNormal && kNormal <= 1.0f, "idle below the default, both in (0, 1]");
    expect(kNormal == 0.5f, "the default is Vulkan's (and VMA's default pools')");

    expect(!wanted(nullptr), "RX_VK_MEMORY_PRIORITY unset: off (opt-in until tested)");
    expect(!wanted(""), "RX_VK_MEMORY_PRIORITY empty: off");
    expect(!wanted("0"), "RX_VK_MEMORY_PRIORITY=0: off");
    expect(wanted("1"), "RX_VK_MEMORY_PRIORITY=1: on");

    expect(idle_presents(nullptr) == kIdlePresents, "RX_VK_PRIORITY_IDLE unset: the default");
    expect(idle_presents("") == kIdlePresents, "RX_VK_PRIORITY_IDLE empty: the default");
    expect(idle_presents("0") == 0, "RX_VK_PRIORITY_IDLE=0: never idle");
    expect(idle_presents("1800") == 1800, "RX_VK_PRIORITY_IDLE=1800");
    expect(idle_presents("-5") == kIdlePresents, "a negative value: the default");
    expect(idle_presents("abc") == kIdlePresents, "not a number: the default");
    expect(idle_presents("60x") == kIdlePresents, "trailing junk: the default");

    expect(for_texture(1000, 1000, 600) == kNormal, "used this present: the default");
    expect(for_texture(1000, 401, 600) == kNormal, "unused for 599 presents: the default");
    expect(for_texture(1000, 400, 600) == kIdle, "unused for 600 presents: idle");
    expect(for_texture(100000, 0, 600) == kIdle, "never used since it was made, long ago: idle");
    expect(for_texture(1000, 1200, 600) == kNormal, "a last use after now (another thread's): the default");
    expect(for_texture(100000, 0, 0) == kNormal, "idle 0: never idle");

    if (failures) return EXIT_FAILURE;
    std::printf("memory priority: all checks passed\n");
    return EXIT_SUCCESS;
}
