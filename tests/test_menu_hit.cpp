// The Insert menu's hit-test (gpu/vk/vk_menu_hit.h): where a mouse position falls on the menu, at a scale of 1 (a
// 720-line window) and 2 (1440 lines), and through a window whose pixels are not the swapchain's.
#include "../gpu/vk/vk_menu_hit.h"

#include <cstdio>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

using namespace mkwvk::menu_hit;

int main() {
    // the Graphics tab: 11 rows, the Sharpness row (7) a slider; values from x 380 at scale 1
    int value_l[11];
    bool slider[11] = {};
    for (int& v : value_l) v = 380;
    slider[7] = true;
    const int w1 = kWidth, h1 = 520;
    auto at = [&](int x, int y, int s) {
        int vl[11];
        for (int i = 0; i < 11; i++) vl[i] = value_l[i] * s;
        return options(x, y, s, kWidth * s, h1 * s, 3, 11, vl, slider);
    };
    Hit h;

    // ---- scale 1: the tabs (side 85, slots of 103 from 85) ----
    h = at(100, 60, 1);
    CHECK("tab 0", h.part == kTab && h.index == 0);
    h = at(240, 60, 1);
    CHECK("tab 1", h.part == kTab && h.index == 1);
    h = at(300, 60, 1);
    CHECK("tab 2", h.part == kTab && h.index == 2);
    CHECK("[Q / LB] is the previous tab", at(40, 60, 1).part == kTabPrev);
    CHECK("[E / RB] is the next tab", at(450, 60, 1).part == kTabNext);
    CHECK("the title is no control", at(100, 20, 1).part == kNone);

    // ---- scale 1: the rows (from 84, 28 high) ----
    h = at(30, 84 + 3 * 28 + 10, 1);
    CHECK("row 3 by its name", h.part == kRow && h.index == 3 && h.side == 0);
    h = at(390, 84 + 3 * 28 + 10, 1);
    CHECK("row 3's value, left half", h.part == kRow && h.index == 3 && h.side == -1);
    h = at(450, 84 + 3 * 28 + 10, 1);
    CHECK("row 3's value, right half", h.part == kRow && h.index == 3 && h.side == 1);
    h = at(30, 84, 1);
    CHECK("row 0's top line", h.part == kRow && h.index == 0);
    h = at(30, 84 - 1, 1);
    CHECK("above the rows", h.part == kNone);
    CHECK("the row band's left margin", at(7, 84 + 10, 1).part == kNone);
    CHECK("past the last row", at(30, 84 + 11 * 28 + 2, 1).part == kNone);
    CHECK("a tab with fewer rows", options(30, 84 + 5 * 28 + 2, 1, w1, h1, 3, 3, value_l, nullptr).part == kNone);

    // ---- scale 1: the slider's track, 200 to 330 ----
    const int sy = 84 + 7 * 28 + 14;
    h = at(265, sy, 1);
    CHECK("the track's middle", h.part == kSlider && h.index == 7 && h.permille == 500);
    h = at(196, sy, 1);
    CHECK("just before the track: its start", h.part == kSlider && h.permille == 0);
    h = at(334, sy, 1);
    CHECK("just past the track: its end", h.part == kSlider && h.permille == 1000);
    h = at(340, sy, 1);
    CHECK("past the grab: the row's name", h.part == kRow && h.side == 0);
    h = at(265, 84 + 6 * 28 + 14, 1);
    CHECK("no track on a row without one", h.part == kRow && h.index == 6);
    CHECK("a drag past the track holds to its ends", slider_drag(100, 1) == 0 && slider_drag(500, 1) == 1000);
    CHECK("a drag along it", slider_drag(265, 1) == 500);

    // ---- the slider's values ----
    CHECK("sharpness at half", slider_value(500, 0, 100, 10) == 50);
    CHECK("sharpness ends", slider_value(0, 0, 100, 10) == 0 && slider_value(1000, 0, 100, 10) == 100);
    CHECK("sensitivity at half (nearest step)", slider_value(500, 10, 500, 10) == 260);
    CHECK("sensitivity ends", slider_value(0, 10, 500, 10) == 10 && slider_value(1000, 10, 500, 10) == 500);
    CHECK("knob place", slider_permille(50, 0, 100) == 500 && slider_permille(10, 10, 500) == 0 &&
                        slider_permille(500, 10, 500) == 1000);
    CHECK("a value and its knob agree", slider_value(slider_permille(260, 10, 500), 10, 500, 10) == 260);

    // ---- scale 2 (a 1440-line window): everything twice as far ----
    h = at(480, 120, 2);
    CHECK("scale 2: tab 1", h.part == kTab && h.index == 1);
    CHECK("scale 2: [Q / LB]", at(100, 120, 2).part == kTabPrev);
    h = at(60, 168 + 2 * 56 + 5, 2);
    CHECK("scale 2: row 2", h.part == kRow && h.index == 2 && h.side == 0);
    h = at(900, 168 + 2 * 56 + 5, 2);
    CHECK("scale 2: row 2's value, right half", h.part == kRow && h.side == 1);
    h = at(530, 168 + 7 * 56 + 28, 2);
    CHECK("scale 2: the track's middle", h.part == kSlider && h.permille == 500);

    // ---- off the menu ----
    CHECK("left of the menu", at(-1, 100, 1).part == kOutside);
    CHECK("right of the menu", at(w1, 100, 1).part == kOutside);
    CHECK("below the menu", at(100, h1, 1).part == kOutside);

    // ---- the window's pixels to the menu's: a 1280x720 window, the menu centred ----
    {
        int ix, iy, x0, y0;
        to_image(400 + 300, 100 + 60, 1280, 720, 1280, 720, &ix, &iy);
        origin(1280, 720, w1, h1, &x0, &y0);
        CHECK("centred at 1280x720", x0 == 400 && y0 == 100);
        h = options(ix - x0, iy - y0, 1, w1, h1, 3, 11, value_l, slider);
        CHECK("1280x720: tab 2", h.part == kTab && h.index == 2);
    }
    {   // (the swapchain at 2560x1440 while the window still says 1280x720 pixels: a resize on its way)
        int ix, iy, x0, y0;
        to_image(640, 160, 1280, 720, 2560, 1440, &ix, &iy);
        CHECK("window to image", ix == 1280 && iy == 320);
        origin(2560, 1440, kWidth * 2, h1 * 2, &x0, &y0);
        h = at(ix - x0, iy - y0, 2);
        CHECK("2560x1440: tab 1", h.part == kTab && h.index == 1);
    }
    {   // (a 1920x1080 window: still scale 1, centred)
        int x0, y0;
        origin(1920, 1080, w1, h1, &x0, &y0);
        h = options(960 - x0, (y0 + 84 + 28 + 5) - y0, 1, w1, h1, 3, 11, value_l, slider);
        CHECK("1920x1080: row 1", h.part == kRow && h.index == 1);
    }

    // ---- the Controls page: 24 controls, then Reset; rows 24 high from 64 ----
    const int cw = kCtlWidth, ch = 720;
    h = controls(50, 64 + 2 * 24 + 5, 1, cw, ch, 24);
    CHECK("controls: row 2's name", h.part == kCtlRow && h.index == 2);
    h = controls(250, 64 + 2 * 24 + 5, 1, cw, ch, 24);
    CHECK("controls: row 2's gamepad", h.part == kCtlCell && h.index == 2 && h.side == 0);
    h = controls(400, 64 + 2 * 24 + 5, 1, cw, ch, 24);
    CHECK("controls: row 2's keys", h.part == kCtlCell && h.index == 2 && h.side == 1);
    h = controls(193, 64 + 5, 1, cw, ch, 24);
    CHECK("controls: just before the gamepad's cell", h.part == kCtlRow && h.index == 0);
    h = controls(250, 64 + 24 * 24 + 5, 1, cw, ch, 24);
    CHECK("controls: Reset to defaults is a row", h.part == kCtlRow && h.index == 24);
    CHECK("controls: past Reset", controls(250, 64 + 25 * 24 + 5, 1, cw, ch, 24).part == kNone);
    CHECK("controls: the header", controls(250, 50, 1, cw, ch, 24).part == kNone);
    h = controls(800, 128 + 2 * 48 + 10, 2, cw * 2, ch * 2, 24);
    CHECK("controls, scale 2: row 2's keys", h.part == kCtlCell && h.index == 2 && h.side == 1);

    std::puts("menu hit-test: all checks passed");
}
