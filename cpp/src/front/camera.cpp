/**
 * @file
 * @brief Viewport camera, zoom, screen layout, UI scaling, CRT toggle, and AI mode preferences.
 */
#include <algorithm>
#include <cmath>

#include "front/internal.hpp"
#include "front/prefs.hpp"
#include "platform/display.hpp"
#include "warlords/ai.hpp"
#include "warlords/ai/belief.hpp"
#include "warlords/ai/core.hpp"
#include "warlords/uidata.hpp"

namespace front {

using w2::Army;
using w2::City;
using w2::Format;
using w2::Side;
using w2::uidata::kActive;
using w2::uidata::kDisabled;
using w2::uidata::kNormal;
namespace move = w2::move;
namespace game = w2::game;
namespace slots_mod = w2::slots;

// ------------------------------------------------------------------ camera

// The view is as many tiles as the map's rect holds at the map's zoom, and
// slides smoothly: g_app.cx, g_app.cy is the tile at its top-left corner.

/** @brief The view's width and height in tiles (fractional). */
std::pair<double, double> ViewSize() {
    double tiles_per_ui = static_cast<double>(display::state.scale) / (kTile * g_app.zoom);
    return {g_app.map_rect.w * tiles_per_ui, g_app.map_rect.h * tiles_per_ui};
}

/** @brief The camera in device pixels, rounded to one. */
std::pair<int, int> CamDev() {
    double tile_size = kTile * g_app.zoom;
    return {static_cast<int>(std::floor(g_app.cx * tile_size + 0.5)),
            static_cast<int>(std::floor(g_app.cy * tile_size + 0.5))};
}

/** @brief A UI point to the map position under it, in fractional tiles. */
std::pair<double, double> UiToMap(double x, double y) {
    const auto& rect = g_app.map_rect;
    double scale = display::state.scale, tile_size = kTile * g_app.zoom;
    auto [cam_x, cam_y] = CamDev();
    return {(cam_x + (x - rect.x) * scale) / tile_size, (cam_y + (y - rect.y) * scale) / tile_size};
}

/** @brief A map position in fractional tiles to the UI point it is drawn at. */
std::pair<double, double> MapToUi(double tile_x, double tile_y) {
    const auto& rect = g_app.map_rect;
    double scale = display::state.scale, tile_size = kTile * g_app.zoom;
    auto [cam_x, cam_y] = CamDev();
    return {rect.x + (tile_x * tile_size - cam_x) / scale,
            rect.y + (tile_y * tile_size - cam_y) / scale};
}

/** @brief The tile at the middle of the view. */
std::pair<int, int> ViewCentre() {
    auto [view_w, view_h] = ViewSize();
    return {static_cast<int>(std::floor(g_app.cx + view_w / 2)),
            static_cast<int>(std::floor(g_app.cy + view_h / 2))};
}

/** @brief The tiles the view touches, clipped to the map: x0, y0, x1, y1. */
std::array<int, 4> ViewTiles() {
    auto [view_w, view_h] = ViewSize();
    int map_w = g_app.g->map->width, map_h = g_app.g->map->height;
    return {std::max(0, static_cast<int>(std::floor(g_app.cx))),
            std::max(0, static_cast<int>(std::floor(g_app.cy))),
            std::min(map_w - 1, static_cast<int>(std::ceil(g_app.cx + view_w)) - 1),
            std::min(map_h - 1, static_cast<int>(std::ceil(g_app.cy + view_h)) - 1)};
}

/** @brief Whether the tile is among those drawn this frame (g_app.view). */
bool Shown(int x, int y) {
    return x >= g_app.view[0] && y >= g_app.view[1] && x <= g_app.view[2] && y <= g_app.view[3];
}

/** @brief Whether the tile's centre lies inside the view. */
bool InView(int x, int y) {
    auto [view_w, view_h] = ViewSize();
    return x + 0.5 > g_app.cx && x + 0.5 < g_app.cx + view_w && y + 0.5 > g_app.cy &&
           y + 0.5 < g_app.cy + view_h;
}

/** @brief Keeps the view on the map; a map smaller than the view sits in its middle. */
void ClampCamera() {
    if (!g_app.g) return;
    auto [view_w, view_h] = ViewSize();
    int map_w = g_app.g->map->width, map_h = g_app.g->map->height;
    g_app.cx = view_w >= map_w ? (map_w - view_w) / 2
                               : std::max(0.0, std::min(g_app.cx, map_w - view_w));
    g_app.cy = view_h >= map_h ? (map_h - view_h) / 2
                               : std::max(0.0, std::min(g_app.cy, map_h - view_h));
}

/** @brief Centres the view on the tile, as far as the map's edges allow. */
void CentreOn(int x, int y) {
    auto [view_w, view_h] = ViewSize();
    g_app.cx = x + 0.5 - view_w / 2;
    g_app.cy = y + 0.5 - view_h / 2;
    ClampCamera();
}

/** @brief The zoom the map may go to: up to two steps past the UI's biggest. */
static int MaxZoom() {
    return std::max(display::state.max_scale, display::state.scale) + 2;
}

/**
 * @brief Zooms the map to `zoom`, keeping the map under the UI point (x, y) put.
 * @param centre Zoom about the middle of the map's rect instead of (x, y).
 */
void SetZoomAt(int zoom, double x, double y, bool centre) {
    zoom = std::max(1, std::min(MaxZoom(), zoom));
    if (zoom == g_app.zoom) return;
    prefs::Set("zoom", zoom);
    const auto& rect = g_app.map_rect;
    if (centre) {
        x = rect.x + rect.w / 2.0;
        y = rect.y + rect.h / 2.0;
    }
    auto [tile_x, tile_y] = UiToMap(x, y);
    g_app.zoom = zoom;
    double tiles_per_ui = static_cast<double>(display::state.scale) / (kTile * zoom);
    g_app.cx = tile_x - (x - rect.x) * tiles_per_ui;
    g_app.cy = tile_y - (y - rect.y) * tiles_per_ui;
    ClampCamera();
}
/** @brief Zooms the map about the middle of the view. */
void SetZoom(int zoom) {
    SetZoomAt(zoom, 0, 0, true);
}

/**
 * @brief Takes up a new layout of the screen's regions, keeping the view
 *        centred where it was.
 */
static void ApplyLayout(const layout::Layout& new_layout) {
    std::optional<std::pair<double, double>> mid;
    if (g_app.g && g_app.have_layout) {
        auto [view_w, view_h] = ViewSize();
        mid = std::make_pair(g_app.cx + view_w / 2, g_app.cy + view_h / 2);
    }
    g_app.layout = new_layout;
    g_app.have_layout = true;
    screen::Relayout(*g_app.screen, new_layout);
    auto reg = [](int region_id) {
        const auto* region = screen::FindRegion(*g_app.screen, region_id);
        return region ? layout::Rect{region->x, region->y, region->w, region->h, region->id}
                      : layout::Rect{};
    };
    g_app.map_rect = reg(screen::kMap);
    g_app.strat_rect = reg(screen::kStrategic);
    g_app.bar_rect = reg(screen::kBottombar);
    g_app.menu_rect = reg(screen::kMenubar);
    g_app.panel_rect = reg(screen::kMappanel);
    g_app.menu_layout =
            menu::MakeLayout(*g_app.font, new_layout.w,
                             menu::WithZooms(display::state.max_short, MaxZoom(),
                                             display::state.smooth_scale, display::state.shrink));
    g_app.zoom = std::max(1, std::min(MaxZoom(), g_app.zoom ? g_app.zoom : display::state.scale));
    if (mid) {
        auto [view_w, view_h] = ViewSize();
        g_app.cx = mid->first - view_w / 2;
        g_app.cy = mid->second - view_h / 2;
        ClampCamera();
    }
}

/**
 * @brief Sizes the frame and lays the screen out anew when its size changed.
 *
 * The start screens are the original's 640x480, centred; the game has the
 * whole screen, down to SHORT_H rows. Run each frame, and each input.
 */
void SyncLayout() {
    int width = layout::kScreenW, height = layout::kScreenH;
    if (!g_app.starting) std::tie(width, height) = display::UiSize();
    display::SetFrame(std::max(width, layout::kScreenW),
                      std::max(height, g_app.starting ? layout::kScreenH : layout::kShortH));
    if (!g_app.have_layout || g_app.layout.w != display::state.w ||
        g_app.layout.h != display::state.h) {
        ApplyLayout(layout::Compute(display::state.w, display::state.h));
    }
}

/** @brief Changes how the screen is used, keeping the view centred where it was. */
static void Relayout(const std::function<void()>& change) {
    std::optional<std::pair<double, double>> mid;
    if (g_app.g && !g_app.starting && g_app.have_layout) {
        auto [view_w, view_h] = ViewSize();
        mid = std::make_pair(g_app.cx + view_w / 2, g_app.cy + view_h / 2);
    }
    change();
    g_app.have_layout = false;
    SyncLayout();
    if (mid) {
        auto [view_w, view_h] = ViewSize();
        g_app.cx = mid->first - view_w / 2;
        g_app.cy = mid->second - view_h / 2;
        ClampCamera();
    }
}

/** @brief Chooses the interface's scale (display::Choose) and remembers it. */
void SetUiScale(int scale) {
    Relayout([scale]() { display::Choose(scale); });
    if (display::state.chosen == display::kSmooth)
        prefs::Set("scale", "smooth");
    else
        prefs::Set("scale", display::state.chosen);
}

/** @brief Keeps the original's 640x480 (4:3) frame, or fits the screen, and remembers it. */
void SetOriginalSize(bool enabled) {
    Relayout([enabled]() {
        if (enabled)
            display::state.wanted = std::make_pair(layout::kScreenW, layout::kScreenH);
        else
            display::state.wanted.reset();
    });
    prefs::Set("4:3", enabled ? "on" : "off");
}

/** @brief Runs in a window or full screen, and remembers it. */
void SetScreen(bool window) {
    Relayout([window]() { display::SetWindowed(window); });
    prefs::Set("screen", window ? "window" : "full");
}

/** @brief Turns the CRT look on or off, and remembers it. */
void ToggleCrt() {
    display::state.crt = !display::state.crt;
    prefs::Set("crt", display::state.crt ? "on" : "off");
}

/**
 * @brief Whether the computer plays with the classic AI: the first computer
 *        side's setting in the game under way, else the default.
 */
static bool ActiveClassicAi() {
    if (g_app.g && g_app.g->map && !g_app.starting) {
        for (const auto& side : g_app.g->map->sides) {
            if (side.in_use && side.computer) return side.IsClassicAi();
        }
        for (const auto& side : g_app.g->map->sides) {
            if (side.in_use) return side.IsClassicAi();
        }
    }
    return w2::ai::DefaultClassicAi();
}

/**
 * @brief Switches every side, and the default, to the classic or the modern
 *        AI, and remembers it.
 */
void SetAiMode(bool classic) {
    w2::ai::SetDefaultClassicAi(classic);
    prefs::Set("ai", classic ? "classic" : "modern");
    if (g_app.g && g_app.g->map) {
        for (auto& side : g_app.g->map->sides) {
            w2::ai::SetSideClassicAi(*g_app.g, side, classic);
        }
    }
}

/**
 * @brief The menu key of the interface's scale in use.
 *
 * A short one stays ticked on the start screens, which are drawn as with
 * none chosen.
 */
static std::string UiScaleKey() {
    const auto& state = display::state;
    if (state.chosen > state.max_scale && state.chosen <= state.max_short)
        return "ui scale " + std::to_string(state.chosen);
    return state.smooth ? "ui scale smooth" : "ui scale " + std::to_string(state.scale);
}

/** @brief Whether the menu item `key` shows a tick: the settings now in force. */
bool MenuTicked(const std::string& key) {
    return key == "map zoom " + std::to_string(g_app.zoom) || key == UiScaleKey() ||
           key == std::string("screen ") + (display::state.windowed ? "window" : "full") ||
           (key == "screen 4:3" && display::state.wanted) ||
           (key == "screen fit" && !display::state.wanted) ||
           (key == "screen crt" && display::state.crt) ||
           key == "music " + std::string(sound::SynthName(sound::Synth())) ||
           key == (ActiveClassicAi() ? "ai classic" : "ai modern");
}

}  // namespace front
