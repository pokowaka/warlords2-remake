#include "front/layout.hpp"

#include <algorithm>

namespace layout {

static const Rect kMap{16, 30, 360, 360, 2};     // region 2
static const Rect kPanel{0, 17, 392, 386, 13};   // region 13, the map's frame
static const Rect kStrat{400, 30, 224, 312, 1};  // region 1
static const Rect kBar{16, 408, 360, 56, 9};     // region 9

const Offset& Layout::Offset(const std::string& g) const {
    if (g == "strat") return strat;
    if (g == "panel") return panel;
    if (g == "bar") return bar;
    return map;
}

Layout Compute(int w, int h) {
    Layout l;
    l.w = std::max(kW, w);
    l.h = std::max(kShortH, h);
    l.ex = l.w - kW;
    l.ey = l.h - kH;
    l.cut = std::max(0, -l.ey);
    l.classic = l.ex == 0 && l.ey == 0;
    l.strat = {l.ex, 0};
    l.panel = {l.ex, l.ey};
    l.bar = {l.ex / 2, l.ey};
    l.map = {0, 0};
    l.dialog = {l.ex / 2, l.ey / 2};
    l.map_rect = Rect{kMap.x, kMap.y, kMap.w + l.ex, kMap.h + l.ey, 2};
    l.panel_rect = Rect{kPanel.x, kPanel.y, kPanel.w + l.ex, kPanel.h + l.ey, 13};
    l.strat_rect = Region(l, kStrat);
    l.bar_rect = Move(l, "bar", kBar);
    return l;
}

std::string Group(int x, int y) {
    if (x >= 392) return y < 348 ? "strat" : "panel";
    if (y >= 396) return "bar";
    return "map";
}

Rect Move(const Layout& l, const std::string& g, Rect r) {
    const Offset& o = l.Offset(g);
    r.x += o.x;
    r.y += o.y;
    return r;
}

Rect Region(const Layout& l, Rect r) {
    Rect out = Move(l, Group(r.x, r.y), r);
    if (r.id == 2) {
        out = l.map_rect;
        out.id = 2;
    } else if (r.id == 13) {
        out = l.panel_rect;
        out.id = 13;
    } else if (r.id == 3) {
        out.x = r.x;
        out.y = r.y;
        out.w = l.w;
    } else if (r.id == 1)
        out.h = std::max(0, r.h - l.cut);
    return out;
}

}  // namespace layout
