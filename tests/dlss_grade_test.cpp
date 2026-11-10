// gpu/vk/dlss_grade.h: the grade's arithmetic and the environment's percents (tests/test_dlss_grade.py builds and runs this)
#include <cmath>
#include <cstdio>

#include "dlss_grade.h"

using namespace mkwvk::grade;

static int failures;
static void need(bool ok, const char* what) {
    if (!ok) failures++, std::printf("FAIL: %s\n", what);
}
static bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }
static float sat_of(const float c[3]) {
    const float hi = std::fmax(c[0], std::fmax(c[1], c[2])), lo = std::fmin(c[0], std::fmin(c[1], c[2]));
    return hi > 0 ? (hi - lo) / hi : 0;
}

int main() {
    // the percents
    need(factor_of(nullptr) == 0 && factor_of("") == 0 && factor_of("abc") == 0 && factor_of("nan") == 0, "no number is 0");
    need(near(factor_of("10"), 0.10f) && near(factor_of("3"), 0.03f) && near(factor_of("-5.5"), -0.055f), "a percent is a factor");
    need(near(factor_of("1000"), 1.0f) && near(factor_of("-1000"), -1.0f), "the percent is kept within -100..100");
    need(!given(nullptr) && !given("") && given("0") && given("abc"), "set but empty is not given");
    // an infinite channel (nothing writes one, but inf - inf would be NaN): the target's ceiling, no NaN
    {
        float c[3] = {INFINITY, 1.0f, 1.0f};
        apply(c, 0.10f, 0.10f, 0.03f);
        need(std::isfinite(c[0]) && std::isfinite(c[1]) && std::isfinite(c[2]) && c[1] >= 0 && c[2] >= 0, "an infinite channel grades to finite values");
        float a[3] = {INFINITY, INFINITY, INFINITY};
        apply(a, 0.10f, 0.10f, 0.03f);
        need(std::isfinite(a[0]) && a[0] == a[1] && a[1] == a[2], "an infinite grey stays a finite grey");
    }
    // off: as it came
    for (float v : {0.0f, 0.2f, 0.435f, 0.9f, 3.0f}) {
        float c[3] = {v, v * 0.5f, v * 0.25f};
        apply(c, 0, 0, 0);
        need(near(c[0], v) && near(c[1], v * 0.5f) && near(c[2], v * 0.25f), "off changes nothing");
    }
    // saturation alone: the highest channel stays, the others fall by 10% of their distance to it, grey stays grey
    {
        float c[3] = {0.8f, 0.5f, 0.2f};
        apply(c, 0.10f, 0, 0);
        need(near(c[0], 0.8f) && near(c[1], 0.47f) && near(c[2], 0.14f), "subtractive saturation, +10%");
        float g[3] = {0.6f, 0.6f, 0.6f};
        apply(g, 0.10f, 0.10f, 0);
        need(near(g[0], 0.6f) && near(g[1], 0.6f) && near(g[2], 0.6f), "grey keeps its level with saturation and density");
        const float before[3] = {0.8f, 0.5f, 0.2f};
        need(c[0] + c[1] + c[2] < before[0] + before[1] + before[2], "more saturation is less light (subtractive)");
        need(sat_of(c) > sat_of(before), "the colour is more saturated");
        float n[3] = {0.8f, 0.5f, 0.2f};
        apply(n, -0.10f, 0, 0);
        need(near(n[0], 0.8f) && near(n[1], 0.53f) && near(n[2], 0.26f), "a negative percent takes colour off");
    }
    // contrast alone: the pivot stays, the rest moves away from it by 3%, grey stays grey, nothing below 0
    {
        float p[3] = {kPivot, kPivot, kPivot};
        apply(p, 0, 0, 0.03f);
        need(near(p[0], kPivot) && near(p[1], kPivot) && near(p[2], kPivot), "the pivot stays");
        float c[3] = {0.935f, 0.435f, 0.0f};
        apply(c, 0, 0, 0.03f);
        need(near(c[0], 0.95f) && near(c[1], 0.435f) && c[2] == 0.0f, "contrast +3% about the pivot, clamped at 0");
        float h[3] = {2.435f, 2.435f, 2.435f};   // (above 1: the target is a float one)
        apply(h, 0, 0, 0.03f);
        need(near(h[0], 2.495f) && h[0] == h[1] && h[1] == h[2], "values above 1 follow the same line");
    }
    // density: a saturated colour is darker by its saturation, hue and saturation kept; with the saturation, in the shader's order
    {
        float c[3] = {0.8f, 0.4f, 0.0f};   // (saturation 1)
        apply(c, 0, 0.10f, 0);
        need(near(c[0], 0.72f) && near(c[1], 0.36f) && c[2] == 0.0f, "a fully saturated colour is 10% darker");
        float m[3] = {0.8f, 0.6f, 0.4f}, was[3] = {0.8f, 0.6f, 0.4f};   // (saturation 0.5)
        apply(m, 0, 0.10f, 0);
        need(near(m[0], 0.76f) && near(m[1], 0.57f) && near(m[2], 0.38f) && near(sat_of(m), sat_of(was)), "half saturated: 5% darker, as saturated");
        float b[3] = {0.8f, 0.5f, 0.2f};   // (saturation 0.75 -> 0.825 with +10%, then 8.25% darker)
        apply(b, 0.10f, 0.10f, 0);
        need(near(b[0], 0.8f * 0.9175f) && near(b[1], 0.47f * 0.9175f) && near(b[2], 0.14f * 0.9175f), "saturation, then its density");
        need(b[0] < 0.8f, "with density the highest channel falls too (the colour is deeper, not brighter)");
        float z[3] = {0, 0, 0};
        apply(z, 0.10f, 0.10f, 0);
        need(z[0] == 0 && z[1] == 0 && z[2] == 0, "black stays black");
        float o[3] = {0.9f, 0.05f, 0.0f};   // (the saturation step would take a channel below 0: kept at 0, saturation 1)
        apply(o, 0.50f, 0.10f, 0);
        need(near(o[0], 0.81f) && o[1] == 0.0f && o[2] == 0.0f, "a channel pushed below 0 stays at 0");
    }
    // both, in the shader's order: saturation, then contrast
    {
        float c[3] = {0.8f, 0.5f, 0.2f};
        apply(c, 0.10f, 0, 0.03f);
        need(near(c[0], (0.8f - kPivot) * 1.03f + kPivot) && near(c[1], (0.47f - kPivot) * 1.03f + kPivot) &&
                 near(c[2], (0.14f - kPivot) * 1.03f + kPivot), "saturation first, then contrast");
    }
    if (failures) std::printf("%d failed\n", failures);
    else std::printf("dlss_grade: ok\n");
    return failures != 0;
}
