// The original's menu bar.
//
// The menu lives in WARLORD2.EXE's data segment, not in a data file, so it is
// written out here (docs/re/ui.md > The menu). Layout follows 7ae8:0052 and
// 2372:049b; nothing is hard-coded, so the bar measures itself for its font.
#pragma once

#include <string>
#include <vector>

#include "front/font.hpp"

namespace menu {

constexpr int kBarX = 8, kBarY = 1;
// The bar is 17 pixels deep and white (7ae8:02d8).
constexpr int kBarH = 17;

/** "-" is a separator. The key is the accelerator, and is what the front end
 *  dispatches on; `act` is what an item with no accelerator does. */
struct Item {
    std::string label, key, act;
};
struct Menu {
    std::string title;
    std::vector<Item> items;
};
extern const std::vector<Menu> kMenus;

/** The menus with the screen's zooms added to View, and the music's
 *  synthesizer to Game -- not the original's. `smoothUI`, when not 0, is the
 *  smooth view's scale, offered after the whole ones. The map's zooms are
 *  named for their size on the screen: `mapShrink` times their own. */
std::vector<Menu> WithZooms(int max_ui, int max_map, double smooth_ui = 0, double map_shrink = 1);

struct Row {
    std::string label, key, act;
    int x = 0, y = 0, w = 0, h = 0;
};
struct Drop {
    int x = 0, y = 0, w = 0, h = 0, key_col = 0;
    std::vector<Row> rows;
};
struct Title {
    std::string title;
    int x = 0, w = 0;
    Drop drop;
};
using Layout = std::vector<Title>;

/** Measure the bar and every dropdown for a font (7ae8:0052, 2372:049b). */
Layout MakeLayout(const Font& font, int screen_width, const std::vector<Menu>& menus);
/** Which menu title is at this point (an index from 0), or -1. */
int TitleAt(const Layout& lay, int x, int y);
/** Which row of an open menu is at this point, or null. */
const Row* RowAt(const Layout& lay, int index, int x, int y);

}  // namespace menu
