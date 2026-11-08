/**
 * @file
 * @brief The original's screen layout, read from its own data files.
 *
 * See docs/formats/screens.md. DATA/JOIN.DAT names, for each dialog, one
 * group of controls in BUTTON.DAT and one set of clickable regions in
 * AREA.DAT. DATA/FILE.DAT group 3 turns a control's bitmap id into a .pck
 * file name.
 */
#pragma once

#include <array>
#include <map>
#include <string>
#include <vector>

namespace w2::uidata {

/** @brief Groups of strings, both numbered from 0. */
using Strings = std::vector<std::vector<std::string>>;

/**
 * @brief Reads a file in STRING.DAT's layout: groups of strings, both from 0
 * (docs/formats/string.md).
 */
Strings LoadStrings(const std::string& path);

/** @brief A JOIN.DAT entry: which controls and regions make up one dialog. */
struct Join {
    int button = 0;  ///< The control group's id in BUTTON.DAT.
    int area = 0;    ///< The region set's id in AREA.DAT.
};
/** @brief A clickable rectangle of a dialog, from AREA.DAT. */
struct Region {
    int id = 0;  ///< The region's id, which the code checks for.
    int x = 0;   ///< Left edge, in screen pixels.
    int y = 0;   ///< Top edge, in screen pixels.
    int w = 0;   ///< Width in pixels.
    int h = 0;   ///< Height in pixels.
};
/** @brief One AREA.DAT record: a set of regions. */
struct Area {
    int id = 0;                   ///< The set's id, which JOIN.DAT refers to.
    int enabled = 0;              ///< The record's enabled word, as stored.
    std::vector<Region> regions;  ///< The regions, in file order.
};
/** @brief A point in a sheet or on screen, in pixels. */
struct Point {
    int x = 0;  ///< Column.
    int y = 0;  ///< Row.
};
// A control's three source rects are indexed by its state: 1 is the resting
// look, 0 the lit one and 2 the disabled one.
constexpr int kActive = 0;    ///< The lit (pressed or highlighted) look.
constexpr int kNormal = 1;    ///< The resting look.
constexpr int kDisabled = 2;  ///< The greyed-out look.
/** @brief A button or other control of a dialog, from BUTTON.DAT. */
struct Control {
    int id = 0;                ///< The control's id, which the code checks for.
    int x = 0;                 ///< Left edge on screen.
    int y = 0;                 ///< Top edge on screen.
    int w = 0;                 ///< Width in pixels.
    int h = 0;                 ///< Height in pixels.
    std::array<Point, 3> src;  ///< Top-left of its art in the bitmap, by state (kActive...).
    int bitmap = 0;            ///< The bitmap id; FILE.DAT group 3 names its .pck file.
};
/** @brief One BUTTON.DAT record: the controls of one dialog. */
struct ButtonGroup {
    int id = 0;                     ///< The group's id, which JOIN.DAT refers to.
    std::vector<Control> controls;  ///< The controls, in file order.
};
/** @brief A menu item that may be put on one of the four configurable buttons. */
struct ShortcutItem {
    std::string name;          ///< The menu item's name.
    int w = 0;                 ///< Width of its art.
    int h = 0;                 ///< Height of its art.
    std::array<Point, 3> src;  ///< Its art in MENUBUTT.PCK: lit, resting, greyed.
};

/** @brief Reads JOIN.DAT: each dialog's control group and region set, by dialog id. */
std::map<int, Join> Joins(const std::string& path);
/** @brief Reads AREA.DAT: the region sets by id. */
std::map<int, Area> Areas(const std::string& path);
/** @brief Reads BUTTON.DAT: the control groups by id. */
std::map<int, ButtonGroup> Buttons(const std::string& path);
/**
 * @brief Reads UDB/UDB.DAT: the menu items that may be put on the four
 * configurable buttons.
 */
std::map<int, ShortcutItem> ShortcutItems(const std::string& path);
/** @brief Reads UDB/UDB.CUR: which menu item sits on each of the four buttons. */
std::map<int, int> Shortcuts(const std::string& path);

/// The control id of the first of the four configurable buttons; the rest follow.
constexpr int kShortcutFirst = 179;
constexpr int kShortcutCount = 4;  ///< How many configurable buttons there are.
/// Their art is bitmap 43, MENUBUTT.PCK, at the rect the item carries.
constexpr int kShortcutBitmap = 43;

/** @brief Everything the layout files say, loaded once. */
struct UI {
    std::map<int, int> shortcuts;                ///< Button number -> menu item id (UDB.CUR).
    std::map<int, ShortcutItem> shortcut_items;  ///< The menu items by id (UDB.DAT).
    std::map<int, std::string> shortcut_names;   ///< The menu items' names by id.
    std::map<int, Join> joins;                   ///< JOIN.DAT by dialog id.
    std::map<int, Area> areas;                   ///< AREA.DAT by id.
    std::map<int, ButtonGroup> buttons;          ///< BUTTON.DAT by id.
    std::map<int, std::string> bitmaps;          ///< Bitmap id -> lower-cased file name.
    Strings files;                               ///< DATA/FILE.DAT.
    /// DATA/STRING.DAT: the game's whole text corpus. The original's string
    /// lookup by group and index is Text(ui, group, index) here, both from 0.
    Strings text;
};

/** @brief Loads every layout file under `data_dir`. */
UI Load(const std::string& data_dir);
/** @brief One string, numbered as the original numbers them (both from 0); "" if absent. */
std::string Text(const UI& layout, int group, int index);

/** @brief The controls and regions of one dialog, gathered through JOIN.DAT. */
struct Dialog {
    int id = 0;                     ///< The dialog's id.
    std::vector<Control> controls;  ///< Its controls, from BUTTON.DAT.
    std::vector<Region> regions;    ///< Its clickable regions, from AREA.DAT.
    int enabled = 0;                ///< The region set's enabled word.
    bool found = false;             ///< False when JOIN.DAT has no such dialog.
};
/** @brief The controls and regions of one dialog, following JOIN.DAT. */
Dialog FindDialog(const UI& layout, int dialog_id);

constexpr int kMainScreen = 0;  ///< The dialog id of the main game screen.

}  // namespace w2::uidata
