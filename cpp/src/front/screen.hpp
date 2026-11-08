// The original's 640x480 screen: background, controls and hit regions.
//
// Everything here is driven by the game's own layout files (uidata) and its
// own art, so the chrome is the original's rather than a lookalike.
// docs/re/ui.md and docs/formats/screens.md.
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

constexpr int kWidth = 640, kHeight = 480;
constexpr int kTile = 40;

// The main screen's regions, by the id AREA.DAT gives them.
constexpr int kStrategic = 1, kMap = 2, kMenubar = 3, kBottombar = 9, kMappanel = 13;

struct Art {
    gfx::ImageP image;
    int w = 0, h = 0;
};

/** Another dialog's layout, sharing the screen's art and bitmap table. */
struct View {
    w2::uidata::Dialog dialog;
    std::map<int, int> state;
    int id = 0;
    bool screen = false;
    int StateOf(int id) const {
        auto it = state.find(id);
        return it == state.end() ? w2::uidata::kNormal : it->second;
    }
};

struct Screen {
    w2::uidata::UI ui;
    w2::uidata::Dialog dialog, original;
    std::string data_dir;
    w2::pal::Palette palette;
    int terrain = 0;
    std::map<int, Art> art;
    std::array<gfx::ImageP, 4> background;
    std::array<std::shared_ptr<const w2::pck::Pixels>, 4> background_px;
    gfx::ImageP bg_image, stone_img;
    struct MapColour {
        std::array<std::array<int, 256>, 4> tile{};
        std::array<std::array<int, 20>, 4> road{};
        std::array<int, 16> remap{};
        bool loaded = false;
    } map_colour;
    std::map<int, int> state;  // the live state of each control

    const Art* ArtFor(int bitmap_id);
    int StateOf(int id) const {
        auto it = state.find(id);
        return it == state.end() ? w2::uidata::kNormal : it->second;
    }
};

/** Load the layout and the background the given dialog needs. */
std::unique_ptr<Screen> Load(const std::string& data_dir, const w2::pal::Palette& palette,
                             int dialog_id = 0, int terrain = 0);
/** Place the main screen's controls and regions for layout `L`. */
void Relayout(Screen& s, const layout::Layout& l);
/** The main screen's ground for layout `L`. */
void DrawBackground(Screen& s, const layout::Layout& l);
void DrawControls(Screen& s);
/** Another dialog's layout. */
View MakeView(Screen& s, int id);
// The default buttons: the controls Enter presses (DS:155c, 17be:0064).
extern const std::set<int> kDefaultIds;
/** The controls of a loaded dialog, each default button with its ring;
 *  controls in `hidden` are left out. */
void DrawDialogControls(Screen& s, const View& v, const std::set<int>* hidden = nullptr);
const w2::uidata::Control* DialogControl(const View& v, int id);
const w2::uidata::Control* DialogControlAt(const View& v, int x, int y);
/** The strategic map as one 224x312 image, two pixels a tile each way, as
 *  834b:2785 paints it from MAPCOLOR.DAT. Tiles `side` has not seen are black. */
gfx::ImageP StrategicImage(Screen& s, const w2::Game& g, int side);
/** The region under a point, topmost first, as the original hit-tests. */
const w2::uidata::Region* RegionAt(const Screen& s, int x, int y);
const w2::uidata::Region* FindRegion(const Screen& s, int id);
const w2::uidata::Control* FindControl(const Screen& s, int id);
/** The control under a point, or null. */
const w2::uidata::Control* ControlAt(const Screen& s, int x, int y);

}  // namespace screen
