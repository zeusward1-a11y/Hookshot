// slEvaluateFeature: eOk and eWarnOutOfVRAM are drawn frames; every error is not (gpu/vk/dlss_result.h)
#include <cstdio>

#include "dlss_result.h"

int main() {
    int failures = 0;
    const int last = (int)sl::Result::eWarnOutOfVRAM;   // (the last value of Streamline 2.14.16's list)
    for (int v = 0; v <= last; v++) {
        const sl::Result r = (sl::Result)v;
        const bool want = r == sl::Result::eOk || r == sl::Result::eWarnOutOfVRAM;
        if (mkwvk::dlss_drawn(r) != want) {
            std::printf("FAIL: result %d drawn=%d, want %d\n", v, (int)mkwvk::dlss_drawn(r), (int)want);
            ++failures;
        }
    }
    if (last != 39) std::printf("FAIL: eWarnOutOfVRAM is %d, the game's log said 39\n", last), ++failures;
    std::printf("dlss result: %d failures\n", failures);
    return failures ? 1 : 0;
}
