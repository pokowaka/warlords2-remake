/**
 * @file
 * @brief The original's menu bar.
 *
 * The menu lives in WARLORD2.EXE's data segment, not in a data file, so it is
 * written out here (docs/re/ui.md > The menu). Nothing is hard-coded about
 * its layout, so the bar measures itself for its font, as the original does.
 */
#pragma once

#include <string>
#include <vector>

#include "front/font.hpp"

namespace menu {

constexpr int kBarX = 8;  ///< Where the first title starts.
constexpr int kBarY = 1;  ///< Where the titles' text starts.
/** The bar is 17 pixels deep and white. */
constexpr int kBarH = 17;

/**
 * @brief One entry of a dropdown.
 *
 * The key is the accelerator, and is what the front end dispatches on;
 * `act` is what an item with no accelerator does.
 */
struct Item {
    std::string label;  ///< What the row says; "-" is a separator.
    std::string key;    ///< The accelerator as shown ("alt X", "^Q", "m"), "" for none.
    std::string act;    ///< The action of an item with no accelerator ("map zoom 2").
};
/** @brief A title on the bar and its dropdown. */
struct Menu {
    std::string title;        ///< The title on the bar.
    std::vector<Item> items;  ///< The dropdown's entries, top to bottom.
};
/** The original's menus, in bar order. */
extern const std::vector<Menu> kMenus;

/**
 * @brief The menus with the screen's zooms added to View, and the music's
 *        synthesizer and the AI to Game -- not the original's.
 *
 * `smooth_ui`, when not 0, is the smooth view's scale, offered after the
 * whole ones. The map's zooms are named for their size on the screen:
 * `map_shrink` times their own.
 */
std::vector<Menu> WithZooms(int max_ui, int max_map, double smooth_ui = 0, double map_shrink = 1);

/** @brief A dropdown row placed on the screen. */
struct Row {
    std::string label;  ///< As Item::label.
    std::string key;    ///< As Item::key.
    std::string act;    ///< As Item::act.
    int x = 0;          ///< Left edge.
    int y = 0;          ///< Top edge.
    int w = 0;          ///< Width: the whole dropdown's.
    int h = 0;          ///< Height: 2 for a separator, else a line and 2.
};
/** @brief An open dropdown placed on the screen. */
struct Drop {
    int x = 0;           ///< Left edge, kept on the screen.
    int y = 0;           ///< Top edge, under the bar.
    int w = 0;           ///< Width, as wide as its widest row needs.
    int h = 0;           ///< Height.
    int key_col = 0;     ///< Where the accelerators (and setting ticks) start, from x.
    std::vector<Row> rows;  ///< The rows, top to bottom.
};
/** @brief A title placed on the bar, with its dropdown. */
struct Title {
    std::string title;  ///< The text on the bar.
    int x = 0;          ///< Left edge of the title's hit area.
    int w = 0;          ///< Width of the title's hit area.
    Drop drop;          ///< Its dropdown.
};
/** The whole bar, placed: one Title per menu. */
using Layout = std::vector<Title>;

/** @brief Measures the bar and every dropdown for a font, as the original does. */
Layout MakeLayout(const Font& font, int screen_width, const std::vector<Menu>& menus);
/** @brief Which menu title is at this point (an index from 0), or -1. */
int TitleAt(const Layout& bar, int x, int y);
/** @brief Which row of open menu `index` is at this point, or null. */
const Row* RowAt(const Layout& bar, int index, int x, int y);

}  // namespace menu
