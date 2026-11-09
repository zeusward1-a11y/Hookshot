// (hookshot) A colour grade on the scene DLSS takes (shaders/dlss_color.comp, which has this same arithmetic): subtractive
// saturation and contrast, on the square-root encoded, tonemapped colour, before DLSS, so DLSS, frame generation and the game's
// later passes all see one picture. The interface drawn afterwards is not touched. Off (0, 0) the colour is written as it came.
//   RX_DLSS_GRADE_SAT=<percent>       subtractive saturation: each channel moves away from the pixel's highest one by this much
//                                     (the highest stays, the others fall; grey stays)
//   RX_DLSS_GRADE_DENSITY=<percent>   how much darker a fully saturated colour gets (a grey not at all, the rest by its
//                                     saturation): the dye's density, what makes the saturation film's and not a vivid one.
//                                     Unset: the saturation's percent
//   RX_DLSS_GRADE_CONTRAST=<percent>  contrast about 0.435 of the encoded range (DaVinci Resolve's default pivot)
// No Vulkan here: tests/dlss_grade_test.cpp runs it (tests/test_dlss_grade.py).
#pragma once

#include <algorithm>
#include <cstdlib>

namespace mkwvk::grade {

constexpr float kPivot = 0.435f;
constexpr float kMaxPercent = 100.0f;
constexpr float kCeiling = 65504.0f;   // (the target's highest value, a 16-bit float: an infinite channel would grade to NaN)

/// Whether the environment gives a value at all (set but empty is unset: the default stands)
inline bool given(const char* text) { return text && *text; }

/// A percent as the environment gives it (null, empty or not a number: 0), kept within -100..100, as a factor
inline float factor_of(const char* text) {
    if (!text || !*text) return 0.0f;
    char* end = nullptr;
    const double v = std::strtod(text, &end);
    if (end == text || !(v == v)) return 0.0f;
    return (float)(std::clamp(v, (double)-kMaxPercent, (double)kMaxPercent) / 100.0);
}

/// The grade of one encoded colour, in place (dlss_color.comp's grade())
inline void apply(float c[3], float sat, float density, float contrast) {
    for (int i = 0; i < 3; i++) c[i] = std::min(c[i], kCeiling);
    const float top = std::max(c[0], std::max(c[1], c[2]));
    for (int i = 0; i < 3; i++) c[i] = std::max(top - (top - c[i]) * (1.0f + sat), 0.0f);
    const float low = std::min(c[0], std::min(c[1], c[2]));
    const float dark = 1.0f - density * (top > 0.0f ? (top - low) / top : 0.0f);   // (the colour's saturation, 0..1, after the step above)
    for (int i = 0; i < 3; i++) {
        const float v = (c[i] * dark - kPivot) * (1.0f + contrast) + kPivot;
        c[i] = std::max(v, 0.0f);
    }
}

}   // namespace mkwvk::grade
