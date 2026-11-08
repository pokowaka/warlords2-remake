#include "front/layout.hpp"

#include <algorithm>

namespace layout {

static const Rect kMap{16, 30, 360, 360, 2};     ///< Region 2, the map.
static const Rect kPanel{0, 17, 392, 386, 13};   ///< Region 13, the map's frame.
static const Rect kStrat{400, 30, 224, 312, 1};  ///< Region 1, the strategic map.
static const Rect kBar{16, 408, 360, 56, 9};     ///< Region 9, the bottom bar.

const Offset& Layout::Offset(const std::string& group) const {
    if (group == "strat") return strat;
    if (group == "panel") return panel;
    if (group == "bar") return bar;
    return map;
}

Layout Compute(int width, int height) {
    Layout screen_layout;
    screen_layout.w = std::max(kW, width);
    screen_layout.h = std::max(kShortH, height);
    screen_layout.ex = screen_layout.w - kW;
    screen_layout.ey = screen_layout.h - kH;
    screen_layout.cut = std::max(0, -screen_layout.ey);
    screen_layout.classic = screen_layout.ex == 0 && screen_layout.ey == 0;
    screen_layout.strat = {screen_layout.ex, 0};
    screen_layout.panel = {screen_layout.ex, screen_layout.ey};
    screen_layout.bar = {screen_layout.ex / 2, screen_layout.ey};
    screen_layout.map = {0, 0};
    screen_layout.dialog = {screen_layout.ex / 2, screen_layout.ey / 2};
    screen_layout.map_rect = Rect{kMap.x, kMap.y, kMap.w + screen_layout.ex,
                                  kMap.h + screen_layout.ey, 2};
    screen_layout.panel_rect = Rect{kPanel.x, kPanel.y, kPanel.w + screen_layout.ex,
                                    kPanel.h + screen_layout.ey, 13};
    screen_layout.strat_rect = Region(screen_layout, kStrat);
    screen_layout.bar_rect = Move(screen_layout, "bar", kBar);
    return screen_layout;
}

std::string Group(int x, int y) {
    if (x >= 392) return y < 348 ? "strat" : "panel";
    if (y >= 396) return "bar";
    return "map";
}

Rect Move(const Layout& screen_layout, const std::string& group, Rect rect) {
    const Offset& offset = screen_layout.Offset(group);
    rect.x += offset.x;
    rect.y += offset.y;
    return rect;
}

Rect Region(const Layout& screen_layout, Rect rect) {
    Rect out = Move(screen_layout, Group(rect.x, rect.y), rect);
    if (rect.id == 2) {
        out = screen_layout.map_rect;
        out.id = 2;
    } else if (rect.id == 13) {
        out = screen_layout.panel_rect;
        out.id = 13;
    } else if (rect.id == 3) {
        out.x = rect.x;
        out.y = rect.y;
        out.w = screen_layout.w;
    } else if (rect.id == 1)
        out.h = std::max(0, rect.h - screen_layout.cut);
    return out;
}

}  // namespace layout
