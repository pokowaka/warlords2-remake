/**
 * @file
 * @brief Where the main screen's pieces go on a screen of any size.
 *
 * Not the original's: it only ever had 640x480. This keeps every piece of
 * that screen at its own size and moves it as a whole, tied to an edge of the
 * bigger screen, and gives all the room left over to the map. The popups are
 * drawn in a 640x480 frame of their own, centred, so every dialog keeps the
 * coordinates the original gives it. At 640x480 every offset is zero.
 *
 * A screen down to kShortH rows (a big scale on a short screen: 2x on the
 * Steam Deck's 1280x800) takes the rows from the map and from the strategic
 * map's foot, which the control panel then covers; the strategic map scrolls
 * in what is left of its frame. The dialogs' frame stays 480 rows, and every
 * popup fits in what of it shows.
 */
#pragma once

#include <string>

namespace layout {

/** The original screen's width and height. main.cpp uses these names, so
 *  they are left for the central rename. */
constexpr int kScreenW = 640, kScreenH = 480;
/** The fewest rows the main screen fits in; below 480 the map and the
 *  strategic map give rows up. */
constexpr int kShortH = 400;

/** @brief A rectangle on the screen, tagged with the AREA.DAT region it stands for. */
struct Rect {
    int x = 0;   ///< Left edge.
    int y = 0;   ///< Top edge.
    int w = 0;   ///< Width.
    int h = 0;   ///< Height.
    int id = 0;  ///< The region id (screen::kMap and friends), 0 when none.
    /** @brief True when the point (point_x, point_y) lies inside the rect. */
    bool Contains(int point_x, int point_y) const {
        return point_x >= x && point_y >= y && point_x < x + w && point_y < y + h;
    }
};

/** @brief How far a group of the original screen is moved on this screen. */
struct Offset {
    int x = 0;  ///< Columns to the right.
    int y = 0;  ///< Rows down.
};

/** @brief The main screen's pieces placed for one screen size. */
struct Layout {
    int w = kScreenW;     ///< The screen's width, at least kScreenW.
    int h = kScreenH;     ///< The screen's height, at least kShortH.
    int ex = 0;           ///< Columns beyond the original 640.
    int ey = 0;           ///< Rows beyond the original 480 (negative on a short screen).
    int cut = 0;          ///< Rows under 480, taken off the strategic map's foot.
    bool classic = true;  ///< Exactly the original's 640x480.
    layout::Offset strat;   ///< The strategic map's group, tied to the right edge.
    layout::Offset panel;   ///< The control panel's group, tied to the bottom right.
    layout::Offset bar;     ///< The bottom bar's group, centred along the bottom.
    layout::Offset map;     ///< The map's group, which stays put.
    layout::Offset dialog;  ///< The dialogs' 640x480 frame, centred.
    Rect map_rect;          ///< The map, stretched over the room left over.
    Rect panel_rect;        ///< The map's frame, stretched with it.
    Rect strat_rect;        ///< The strategic map, cut short on a short screen.
    Rect bar_rect;          ///< The bottom bar.
    /** @brief The offset of the named group: "strat", "panel", "bar", else the map's. */
    const layout::Offset& Offset(const std::string& group) const;
};

/** @brief The layout of a screen `width` x `height` UI pixels. */
Layout Compute(int width, int height);
/** @brief Which group a point of the original screen belongs to: "strat", "panel", "bar" or "map".
 */
std::string Group(int x, int y);
/** @brief A copy of the original rect `rect`, moved with group `group`. */
Rect Move(const Layout& screen_layout, const std::string& group, Rect rect);
/**
 * @brief One of the main screen's regions, placed.
 *
 * The map, its frame and the menu bar stretch; the rest move with their
 * group.
 */
Rect Region(const Layout& screen_layout, Rect rect);

}  // namespace layout
