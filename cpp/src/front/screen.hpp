/**
 * @file
 * @brief The original's 640x480 screen: background, controls and hit regions.
 *
 * Everything here is driven by the game's own layout files (uidata) and its
 * own art, so the chrome is the original's rather than a lookalike. See
 * docs/re/ui.md and docs/formats/screens.md.
 */
#pragma once

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "front/layout.hpp"
#include "platform/gfx.hpp"
#include "warlords/pal.hpp"
#include "warlords/pck.hpp"
#include "warlords/types.hpp"
#include "warlords/uidata.hpp"

namespace screen {

constexpr int kWidth = 640;   ///< The original screen's width.
constexpr int kHeight = 480;  ///< The original screen's height.
constexpr int kTile = 40;     ///< A map tile's size in pixels.

// The main screen's regions, by the id AREA.DAT gives them.
constexpr int kStrategic = 1;  ///< The strategic map.
constexpr int kMap = 2;        ///< The map.
constexpr int kMenubar = 3;    ///< The menu bar.
constexpr int kBottombar = 9;  ///< The bar under the map.
constexpr int kMappanel = 13;  ///< The map's frame.

/** @brief A control bitmap, loaded once. */
struct Art {
    gfx::ImageP image;  ///< The bitmap, null when it could not be found.
    int w = 0;          ///< Its width.
    int h = 0;          ///< Its height.
};

/** @brief Another dialog's layout, sharing the screen's art and bitmap table. */
struct View {
    w2::uidata::Dialog dialog;  ///< The dialog's controls and regions.
    std::map<int, int> state;   ///< Each control's state by id; kNormal when missing.
    int id = 0;                 ///< The dialog's id.
    bool screen = false;        ///< A whole screen, not a popup: no default button rings.
    /** @brief The state of control `control_id`: kNormal unless set. */
    int StateOf(int control_id) const {
        auto found = state.find(control_id);
        return found == state.end() ? w2::uidata::kNormal : found->second;
    }
};

/** @brief The main screen: its layout, art and the live state of its controls. */
struct Screen {
    w2::uidata::UI ui;            ///< Every dialog, bitmap and string the game's files hold.
    w2::uidata::Dialog dialog;    ///< The main screen's controls and regions, placed.
    w2::uidata::Dialog original;  ///< The same as the files place them, for 640x480.
    std::string data_dir;         ///< Where the game's DATA folder is.
    w2::pal::Palette palette;     ///< The palette the art is drawn in.
    int terrain = 0;              ///< Which TERRAIN<n> folder the art comes from.
    std::map<int, Art> art;       ///< Control bitmaps by id, loaded as they are asked for.
    std::array<gfx::ImageP, 4> background;  ///< The background's four 320x240 quadrants.
    std::array<std::shared_ptr<const w2::pck::Pixels>, 4> background_px;  ///< Their indices.
    gfx::ImageP bg_image;   ///< The background as one 640x480 image, for a bigger screen.
    gfx::ImageP stone_img;  ///< The stone tile a bigger screen is papered with.
    /** @brief MAPCOLOR.DAT: the strategic map's colour for each tile. */
    struct MapColour {
        std::array<std::array<int, 256>, 4> tile{};  ///< Per pixel of a tile, by tile id.
        std::array<std::array<int, 20>, 4> road{};   ///< Per pixel of a tile, by road id.
        std::array<int, 16> remap{};                 ///< Map colour to palette index.
        bool loaded = false;                         ///< MAPCOLOR.DAT was found.
    } map_colour;                                    ///< The strategic map's colours.
    std::map<int, int> state;                        ///< The live state of each control.

    /** @brief The bitmap with id `bitmap_id`, loaded on first use; null when missing. */
    const Art* ArtFor(int bitmap_id);
    /** @brief The state of control `control_id`: kNormal unless set. */
    int StateOf(int control_id) const {
        auto found = state.find(control_id);
        return found == state.end() ? w2::uidata::kNormal : found->second;
    }
};

/** @brief Loads the layout and the background the given dialog needs. */
std::unique_ptr<Screen> Load(const std::string& data_dir, const w2::pal::Palette& palette,
                             int dialog_id = 0, int terrain = 0);
/** @brief Places the main screen's controls and regions for `screen_layout`. */
void Relayout(Screen& main_screen, const layout::Layout& screen_layout);
/**
 * @brief Draws the main screen's ground for `screen_layout`.
 *
 * At 640x480 that is the original's background; on a bigger screen it is
 * papered with stone and the original's frames are cut out of the background.
 */
void DrawBackground(Screen& main_screen, const layout::Layout& screen_layout);
/** @brief Draws the main screen's controls, each in its live state. */
void DrawControls(Screen& main_screen);
/** @brief Another dialog's layout, every control in its normal state. */
View MakeView(Screen& main_screen, int dialog_id);
/** The default buttons: the controls Enter presses. */
extern const std::set<int> kDefaultIds;
/**
 * @brief Draws the controls of a loaded dialog, each default button with its ring.
 *
 * Controls in `hidden` are left out.
 */
void DrawDialogControls(Screen& main_screen, const View& view,
                        const std::set<int>* hidden = nullptr);
/** @brief The control of `view` with id `control_id`, or null. */
const w2::uidata::Control* DialogControl(const View& view, int control_id);
/** @brief The control of `view` under a point, disabled ones included; or null. */
const w2::uidata::Control* DialogControlAt(const View& view, int x, int y);
/**
 * @brief The strategic map as one 224x312 image, two pixels a tile each way.
 *
 * Painted from MAPCOLOR.DAT as the original paints it. Tiles `side` has not
 * seen are black.
 */
gfx::ImageP StrategicImage(Screen& main_screen, const w2::Game& game, int side);
/** @brief The region under a point, topmost first, as the original hit-tests. */
const w2::uidata::Region* RegionAt(const Screen& main_screen, int x, int y);
/** @brief The main screen's region with id `region_id`, or null. */
const w2::uidata::Region* FindRegion(const Screen& main_screen, int region_id);
/** @brief The main screen's control with id `control_id`, or null. */
const w2::uidata::Control* FindControl(const Screen& main_screen, int control_id);
/** @brief The control under a point, or null. */
const w2::uidata::Control* ControlAt(const Screen& main_screen, int x, int y);

}  // namespace screen
