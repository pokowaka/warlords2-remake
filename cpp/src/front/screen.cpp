#include "front/screen.hpp"

#include <algorithm>
#include <cmath>

#include "front/pckimage.hpp"
#include "front/stonetile.hpp"
#include "util/util.hpp"
#include "warlords/game.hpp"
#include "warlords/scn.hpp"

namespace screen {

using w2::uidata::Control;
using w2::uidata::Region;

namespace {
/** @brief Finds a .pck by name, wherever it lives: PICS/, TERRAIN<n>/ or the root; "" if nowhere.
 */
std::filesystem::path FindArt(const std::filesystem::path& data_dir, const std::string& name,
                              int terrain) {
    std::string upper_name = w2::Upper(name);
    for (const std::filesystem::path& path :
         {data_dir / "PICS" / upper_name,
          data_dir / ("TERRAIN" + std::to_string(terrain)) / upper_name, data_dir / upper_name}) {
        if (w2::FileExists(path)) return path;
    }
    return {};
}

/** @brief Sets the drawing colour to the screen palette's entry `palette_index`. */
void SetPal(const Screen& main_screen, int palette_index) {
    const auto& colour = main_screen.palette[palette_index];
    gfx::SetColor(static_cast<float>(colour[0]), static_cast<float>(colour[1]),
                  static_cast<float>(colour[2]));
}

const int kLight = 2;  ///< The palette index of a bevel's lit edge.
const int kDark = 4;   ///< The palette index of a bevel's shadowed edge.

/** @brief A framed piece of the original screen that a bigger screen cuts out and moves. */
struct Panel {
    const char* group;  ///< The layout group it moves with.
    int x;              ///< Left edge on the original screen.
    int y;              ///< Top edge on the original screen.
    int w;              ///< Width.
    int h;              ///< Height.
};
/** The pieces of the original background a bigger screen keeps. */
const Panel kPanels[] = {
    {"strat", 399, 29, 226, 314},
    {"panel", 399, 354, 226, 116},
    {"bar", 15, 402, 362, 68},
};

/**
 * @brief Makes the art a bigger screen is drawn with, once.
 *
 * Puts the four background quadrants together into one 640x480 image, and
 * cuts the stone tile out of it (see stonetile.hpp).
 */
void ComposedArt(Screen& main_screen) {
    if (main_screen.bg_image) return;
    w2::pck::Pixels whole;
    whole.w = 640;
    whole.h = 480;
    whole.px.assign(640UZ * 480, 0);
    for (int i = 0; i < 4; i++) {
        auto& quadrant = main_screen.background_px[i];
        int quadrant_x = (i % 2) * 320, quadrant_y = (i / 2) * 240;
        if (quadrant) {
            for (int y = 0; y < 240 && y < quadrant->h; y++)
                for (int x = 0; x < 320 && x < quadrant->w; x++)
                    whole.px[(quadrant_y + y) * 640 + quadrant_x + x] =
                            quadrant->px[y * quadrant->w + x];
        }
    }
    main_screen.bg_image = pckimage::FromPixels(whole, main_screen.palette);
    w2::pck::Pixels tile;
    tile.w = stonetile::kWidth;
    tile.h = stonetile::kHeight;
    tile.px.assign(static_cast<size_t>(stonetile::kWidth) * stonetile::kHeight, 0);
    for (int rect_index = 0; rect_index < stonetile::kCount; rect_index++) {
        const int* rect = stonetile::kRects[rect_index];
        int tile_x = rect[0], tile_y = rect[1], width = rect[2], height = rect[3],
            source_x = rect[4], source_y = rect[5];
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++)
                tile.px[(tile_y + y) * stonetile::kWidth + tile_x + x] =
                        whole.px[(source_y + y) * 640 + source_x + x];
    }
    main_screen.stone_img = pckimage::FromPixels(tile, main_screen.palette);
    main_screen.stone_img->wrap = true;
}

/** @brief A one-pixel bevel just outside the rect: dark above and left, light below and right. */
void Bevel(const Screen& main_screen, int x, int y, int width, int height) {
    SetPal(main_screen, kDark);
    gfx::Rectangle(gfx::kFill, x - 1, y - 1, width + 2, 1);
    gfx::Rectangle(gfx::kFill, x - 1, y, 1, height + 1);
    SetPal(main_screen, kLight);
    gfx::Rectangle(gfx::kFill, x, y + height, width + 1, 1);
    gfx::Rectangle(gfx::kFill, x + width, y, 1, height);
}

/** @brief True when two controls have exactly the same rect. */
bool SameRect(const Control& first, const Control& second) {
    return first.x == second.x && first.y == second.y && first.w == second.w && first.h == second.h;
}

/**
 * @brief True when another control on the same rect hides control `index`.
 *
 * Some controls share a rect exactly -- 183/184/185, 240/241. The live one
 * is what shows and what a click reaches; with none live, the first.
 */
bool IsCovered(const Screen& main_screen, const Control& control, size_t index) {
    bool live = main_screen.StateOf(control.id) != w2::uidata::kDisabled;
    const auto& controls = main_screen.dialog.controls;
    for (size_t i = 0; i < controls.size(); i++) {
        const Control& other = controls[i];
        if (i != index && SameRect(other, control)) {
            bool other_live = main_screen.StateOf(other.id) != w2::uidata::kDisabled;
            if (other_live && (!live || i < index)) return true;
            if (!other_live && !live && i < index) return true;
        }
    }
    return false;
}

/** @brief Draws the two-pixel black ring, with cut corners, round a default button. */
void DefaultRing(const Screen& main_screen, const Control& control) {
    SetPal(main_screen, 0);
    auto across = [](int x, int y, int n) { gfx::Rectangle(gfx::kFill, x, y, n, 1); };
    auto down = [](int x, int y, int n) { gfx::Rectangle(gfx::kFill, x, y, 1, n); };
    int ring_x = control.x - 2, ring_y = control.y - 2, ring_w = control.w + 3,
        ring_h = control.h + 3;
    for (int pass = 0; pass < 2; pass++) {
        across(ring_x + 1, ring_y, ring_w - 2);
        down(ring_x + ring_w - 1, ring_y, 2);
        across(ring_x + ring_w - 1, ring_y + 1, 2);
        down(ring_x + ring_w, ring_y + 1, ring_h - 2);
        across(ring_x + ring_w - 1, ring_y + ring_h - 1, 2);
        down(ring_x + ring_w - 1, ring_y + ring_h - 1, 2);
        across(ring_x + 1, ring_y + ring_h, ring_w - 2);
        down(ring_x + 1, ring_y + ring_h - 1, 2);
        across(ring_x, ring_y + ring_h - 1, 2);
        down(ring_x, ring_y + 1, ring_h - 2);
        across(ring_x, ring_y + 1, 2);
        down(ring_x + 1, ring_y, 2);
        ring_x--;
        ring_y--;
        ring_w += 2;
        ring_h += 2;
    }
    gfx::SetColor(1, 1, 1);
}
}  // namespace

const Art* Screen::ArtFor(int bitmap_id) {
    auto cached = art.find(bitmap_id);
    if (cached != art.end()) return cached->second.image ? &cached->second : nullptr;
    Art loaded;
    auto name = ui.bitmaps.find(bitmap_id);
    if (name != ui.bitmaps.end()) {
        std::filesystem::path path = FindArt(data_dir, name->second, terrain);
        if (!path.empty()) {
            if (auto pixels = w2::pck::Decode(path); pixels.ok()) {
                loaded.image = pckimage::FromPixels(**pixels, palette);
                loaded.w = (*pixels)->w;
                loaded.h = (*pixels)->h;
            }
        }
    }
    art[bitmap_id] = loaded;
    return loaded.image ? &art[bitmap_id] : nullptr;
}

absl::StatusOr<std::unique_ptr<Screen>> Load(const std::filesystem::path& data_dir,
                                             const w2::pal::Palette& palette, int dialog_id,
                                             int terrain) {
    auto loaded = std::make_unique<Screen>();
    auto ui_data = w2::uidata::Load(data_dir);
    if (!ui_data.ok()) return ui_data.status();
    loaded->ui = std::move(*ui_data);
    loaded->dialog = w2::uidata::FindDialog(loaded->ui, dialog_id);
    loaded->original = loaded->dialog;
    loaded->data_dir = data_dir;
    loaded->palette = palette;
    loaded->terrain = terrain;
    // The background is four 320x240 quadrants of one 640x480 image.
    for (int i = 0; i < 4; i++) {
        std::filesystem::path path =
                FindArt(data_dir, "screen" + std::to_string(i) + ".pck", terrain);
        if (!path.empty()) {
            auto quad_px = w2::pck::Decode(path);
            if (!quad_px.ok()) return quad_px.status();
            loaded->background_px[i] = std::move(*quad_px);
            loaded->background[i] = pckimage::FromPixels(*loaded->background_px[i], palette);
        }
    }
    // TERRAIN<n>/MAPCOLOR.DAT, the strategic map's own colour table: a colour
    // per tile id for each of a tile's four pixels, a remap, and the same four
    // pixels for a tile with a road on it.
    if (auto colour_file =
                w2::ReadBytes(data_dir / ("TERRAIN" + std::to_string(terrain)) / "MAPCOLOR.DAT");
        colour_file.ok()) {
        // a byte of the file, 0 past its end
        auto byte_at = [&](size_t offset) {
            return offset < colour_file->size() ? static_cast<int>((*colour_file)[offset]) : 0;
        };
        for (int pixel = 0; pixel < 4; pixel++) {
            for (int i = 0; i < 256; i++)
                loaded->map_colour.tile[pixel][i] = byte_at(static_cast<size_t>(pixel * 256 + i));
            for (int i = 0; i < 20; i++)
                loaded->map_colour.road[pixel][i] =
                        byte_at(static_cast<size_t>(1120 + pixel * 20 + i));
        }
        for (int i = 0; i < 16; i++) {
            loaded->map_colour.remap[i] = byte_at(static_cast<size_t>(1024 + i * 2));
        }
        loaded->map_colour.loaded = true;
    } else {
        for (int i = 0; i < 16; i++) loaded->map_colour.remap[i] = i;
    }
    for (auto& control : loaded->dialog.controls) loaded->state[control.id] = w2::uidata::kNormal;
    return loaded;
}

void Relayout(Screen& main_screen, const layout::Layout& screen_layout) {
    main_screen.dialog = main_screen.original;
    for (auto& control : main_screen.dialog.controls) {
        layout::Rect placed =
                layout::Move(screen_layout, layout::Group(control.x, control.y),
                             layout::Rect{control.x, control.y, control.w, control.h, control.id});
        control.x = placed.x;
        control.y = placed.y;
    }
    for (auto& region : main_screen.dialog.regions) {
        layout::Rect placed = layout::Region(
                screen_layout, layout::Rect{region.x, region.y, region.w, region.h, region.id});
        region.x = placed.x;
        region.y = placed.y;
        region.w = placed.w;
        region.h = placed.h;
    }
}

void DrawBackground(Screen& main_screen, const layout::Layout& screen_layout) {
    gfx::SetColor(1, 1, 1);
    if (screen_layout.classic) {
        for (int i = 0; i < 4; i++)
            if (main_screen.background[i])
                gfx::Draw(main_screen.background[i], (i % 2) * 320, (i / 2) * 240);
        return;
    }
    ComposedArt(main_screen);
    gfx::Draw(main_screen.stone_img, gfx::NewQuad(0, 0, screen_layout.w, screen_layout.h), 0, 0);
    SetPal(main_screen, kLight);
    gfx::Rectangle(gfx::kFill, 0, 0, screen_layout.w - 1, 1);
    gfx::Rectangle(gfx::kFill, 0, 1, 1, screen_layout.h - 2);
    SetPal(main_screen, kDark);
    gfx::Rectangle(gfx::kFill, 0, screen_layout.h - 1, screen_layout.w, 1);
    gfx::Rectangle(gfx::kFill, screen_layout.w - 1, 0, 1, screen_layout.h - 1);
    const layout::Rect& map_rect = screen_layout.map_rect;
    Bevel(main_screen, map_rect.x - 1, map_rect.y - 1, map_rect.w + 2, map_rect.h + 2);
    gfx::SetColor(0, 0, 0);
    gfx::Rectangle(gfx::kFill, map_rect.x - 1, map_rect.y - 1, map_rect.w + 2, 1);
    gfx::Rectangle(gfx::kFill, map_rect.x - 1, map_rect.y + map_rect.h, map_rect.w + 2, 1);
    gfx::Rectangle(gfx::kFill, map_rect.x - 1, map_rect.y - 1, 1, map_rect.h + 2);
    gfx::Rectangle(gfx::kFill, map_rect.x + map_rect.w, map_rect.y - 1, 1, map_rect.h + 2);
    for (const Panel& panel : kPanels) {
        layout::Rect placed = layout::Move(screen_layout, panel.group,
                                           layout::Rect{panel.x, panel.y, panel.w, panel.h});
        // on a short screen the strategic map's frame stops above the panel
        int height = std::string(panel.group) == "strat" ? panel.h - screen_layout.cut : panel.h;
        Bevel(main_screen, placed.x, placed.y, panel.w, height);
        gfx::SetColor(1, 1, 1);
        gfx::Draw(main_screen.bg_image, gfx::NewQuad(panel.x, panel.y, panel.w, height), placed.x,
                  placed.y);
    }
}

void DrawControls(Screen& main_screen) {
    gfx::SetColor(1, 1, 1);
    const auto& controls = main_screen.dialog.controls;
    for (size_t i = 0; i < controls.size(); i++) {
        const Control& control = controls[i];
        if (control.bitmap != 0 && control.w > 0 && control.h > 0 &&
            !IsCovered(main_screen, control, i)) {
            int state = main_screen.StateOf(control.id);
            const Art* art = main_screen.ArtFor(control.bitmap);
            auto src = control.src[state];
            if (art && src.x + control.w <= art->w && src.y + control.h <= art->h)
                gfx::Draw(art->image, gfx::NewQuad(src.x, src.y, control.w, control.h), control.x,
                          control.y);
        }
    }
}

View MakeView(Screen& main_screen, int dialog_id) {
    View view;
    view.dialog = w2::uidata::FindDialog(main_screen.ui, dialog_id);
    view.id = dialog_id;
    for (auto& control : view.dialog.controls) view.state[control.id] = w2::uidata::kNormal;
    return view;
}

const std::set<int> kDefaultIds = {123, 103, 141, 174, 189, 192, 201, 223, 287, 242, 285, 251,
                                   292, 294, 330, 331, 332, 172, 356, 359, 368, 370, 396, 421,
                                   424, 425, 457, 469, 282, 485, 484, 483, 474, 489, 490, 495};

void DrawDialogControls(Screen& main_screen, const View& view, const std::set<int>* hidden) {
    gfx::SetColor(1, 1, 1);
    for (const Control& control : view.dialog.controls) {
        if (hidden && hidden->count(control.id)) continue;
        if (control.bitmap != 0 && control.w > 0 && control.h > 0) {
            const Art* art = main_screen.ArtFor(control.bitmap);
            auto src = control.src[view.StateOf(control.id)];
            if (art && src.x + control.w <= art->w && src.y + control.h <= art->h) {
                gfx::SetColor(1, 1, 1);
                gfx::Draw(art->image, gfx::NewQuad(src.x, src.y, control.w, control.h), control.x,
                          control.y);
            }
        }
        if (kDefaultIds.count(control.id) && !view.screen && control.w > 0 && control.h > 0)
            DefaultRing(main_screen, control);
    }
}

const Control* DialogControl(const View& view, int control_id) {
    for (const Control& control : view.dialog.controls)
        if (control.id == control_id) return &control;
    return nullptr;
}

const Control* DialogControlAt(const View& view, int x, int y) {
    for (const Control& control : view.dialog.controls) {
        if (control.w > 0 && control.h > 0 && x >= control.x && x < control.x + control.w &&
            y >= control.y && y < control.y + control.h)
            return &control;
    }
    return nullptr;
}

gfx::ImageP StrategicImage(Screen& main_screen, const w2::Game& game, int side) {
    int width = game.map->width, height = game.map->height;
    const auto& colours = main_screen.map_colour;
    long long seed = 12345;
    // Tile ids 0x50-0x5f get a random grey, 2-4, per pixel. Matches the
    // reference: the greys come from a generator of their own, so that
    // painting the map never moves the game's dice.
    auto grey = [&]() {
        seed = (seed * 1103515245 + 12345) % 2147483648LL;
        return 2 + static_cast<int>(seed / 65536) % 3;
    };
    uint8_t rgb[16][3];
    for (int i = 0; i < 16; i++) {
        int palette_index = colours.remap[i];
        w2::pal::Colour colour =
                palette_index >= 0 && palette_index < static_cast<int>(main_screen.palette.size())
                        ? main_screen.palette[palette_index]
                        : w2::pal::Colour{0, 0, 0};
        for (int j = 0; j < 3; j++) rgb[i][j] = static_cast<uint8_t>(std::floor(colour[j] * 255));
    }
    std::vector<uint8_t> out(static_cast<size_t>(width) * 2 * height * 2 * 4, 0);
    // one pixel of the image in map colour `colour_index`, black when negative
    auto put = [&](int x, int y, int colour_index) {
        size_t offset = (static_cast<size_t>(y) * width * 2 + x) * 4;
        out[offset + 3] = 255;
        if (colour_index < 0) return;
        colour_index &= 15;
        out[offset] = rgb[colour_index][0];
        out[offset + 1] = rgb[colour_index][1];
        out[offset + 2] = rgb[colour_index][2];
    };
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int pixel_colour[4];
            if (!w2::game::Seen(game, side, x, y)) {
                pixel_colour[0] = pixel_colour[1] = pixel_colour[2] = pixel_colour[3] = -1;
            } else {
                int road = w2::scn::RoadAt(*game.map, x, y) % 32;
                int tile_id = road != 0 ? road - 1 : (w2::scn::TileAt(*game.map, x, y) % 256);
                if (road == 0 && tile_id >= 0x50 && tile_id <= 0x5f) {
                    for (int pixel = 0; pixel < 4; pixel++) pixel_colour[pixel] = grey();
                } else {
                    for (int pixel = 0; pixel < 4; pixel++)
                        pixel_colour[pixel] =
                                road != 0 ? (tile_id < 20 ? colours.road[pixel][tile_id] : 0)
                                          : colours.tile[pixel][tile_id];
                }
            }
            put(x * 2, y * 2, pixel_colour[0]);
            put(x * 2 + 1, y * 2, pixel_colour[1]);
            put(x * 2, y * 2 + 1, pixel_colour[2]);
            put(x * 2 + 1, y * 2 + 1, pixel_colour[3]);
        }
    }
    return gfx::NewImageRgba(width * 2, height * 2, out.data());
}

const Region* RegionAt(const Screen& main_screen, int x, int y) {
    const auto& regions = main_screen.dialog.regions;
    for (int i = static_cast<int>(regions.size()) - 1; i >= 0; i--) {
        const Region& region = regions[i];
        if (region.w > 0 && region.h > 0 && x >= region.x && x < region.x + region.w &&
            y >= region.y && y < region.y + region.h)
            return &region;
    }
    return nullptr;
}

const Region* FindRegion(const Screen& main_screen, int region_id) {
    for (const Region& region : main_screen.dialog.regions)
        if (region.id == region_id) return &region;
    return nullptr;
}

const Control* FindControl(const Screen& main_screen, int control_id) {
    for (const Control& control : main_screen.dialog.controls)
        if (control.id == control_id) return &control;
    return nullptr;
}

const Control* ControlAt(const Screen& main_screen, int x, int y) {
    const auto& controls = main_screen.dialog.controls;
    for (size_t i = 0; i < controls.size(); i++) {
        const Control& control = controls[i];
        if (control.w > 0 && control.h > 0 && !IsCovered(main_screen, control, i) &&
            x >= control.x && x < control.x + control.w && y >= control.y &&
            y < control.y + control.h)
            return &control;
    }
    return nullptr;
}

}  // namespace screen
