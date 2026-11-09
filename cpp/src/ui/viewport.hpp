/**
 * @file
 * @brief Camera coordinates, zoom, responsive panel layout, and strategic-map viewport state
 * (`w2::Viewport`).
 */
#pragma once

#include <array>
#include <functional>
#include <optional>
#include <utility>

#include "front/font.hpp"
#include "front/layout.hpp"
#include "front/menu.hpp"
#include "front/screen.hpp"
#include "platform/display.hpp"
#include "platform/gfx.hpp"
#include "warlords/types.hpp"

namespace w2 {

/**
 * @brief Owns map camera coordinates (`cx_`, `cy_`, `zoom_`), computed screen `layout::Layout`,
 * placed panel rectangles, measured `menu::Layout`, visible tile bounds, and the cached strategic
 * map texture.
 *
 * Converts between logical UI pixel coordinates, device map offsets, and fractional or integral
 * map tile positions while keeping the viewport centred across window resizes and UI scale changes.
 *
 * Invariants:
 * - `zoom_ >= 1`.
 * - After `SyncLayout()`, `have_layout_` is true and `layout_.w == display.GetState().w`,
 *   `layout_.h == display.GetState().h`.
 * - `strat_image_` is invalidated (`nullptr`, `strat_viewer_ == -99`) whenever `StratDirty()` runs.
 *
 * Thread safety: Not thread-safe; mutated and queried exclusively on the main UI thread.
 */
class Viewport {
  public:
    using TileSpanPair = std::pair<double, double>;
    using IntPair = std::pair<int, int>;
    using OptionalIntPair = std::optional<IntPair>;
    using ViewBounds = std::array<int, 4>;
    using RelayoutAction = std::function<void()>;

    static constexpr int kTilePixels = screen::kTile;

    Viewport() = default;
    ~Viewport() = default;

    Viewport(const Viewport&) = delete;
    Viewport& operator=(const Viewport&) = delete;

    double Cx() const { return cx_; }
    double Cy() const { return cy_; }
    int Zoom() const { return zoom_; }
    void SetInitialZoom(int zoom) { zoom_ = zoom; }

    const layout::Layout& GetLayout() const { return layout_; }
    bool HaveLayout() const { return have_layout_; }
    void InvalidateLayout() { have_layout_ = false; }

    const layout::Rect& MapRect() const { return map_rect_; }
    const layout::Rect& StratRect() const { return strat_rect_; }
    const layout::Rect& BarRect() const { return bar_rect_; }
    const layout::Rect& MenuRect() const { return menu_rect_; }
    const layout::Rect& PanelRect() const { return panel_rect_; }
    const menu::Layout& MenuLayout() const { return menu_layout_; }
    const ViewBounds& View() const { return view_; }
    int StratScroll() const { return strat_scroll_; }

    /** @brief Marks the cached strategic-map image stale so it rebuilds on next draw. */
    void StratDirty();

    /** @brief Returns (rebuilding if dirty or viewer changed) the 224x312 strategic-map texture. */
    const gfx::ImageP& EnsureStratImage(gfx::Renderer& renderer, screen::Screen& main_screen,
                                        const Game& game, int viewer, bool match_viewer = true);

    /** @brief Returns how many tiles fit across and down `map_rect_` at `ui_scale` and `zoom_`. */
    TileSpanPair ViewSize(int ui_scale) const;

    /** @brief Returns the camera's sub-tile origin in device pixels, rounded to nearest pixel. */
    IntPair CamDev() const;

    /** @brief Converts logical UI coordinates `(x, y)` into fractional map tile coordinates. */
    TileSpanPair UiToMap(double x, double y, int ui_scale) const;

    /** @brief Converts fractional map tile coordinates `(tile_x, tile_y)` into logical UI pixels.
     */
    TileSpanPair MapToUi(double tile_x, double tile_y, int ui_scale) const;

    /** @brief Returns the integral map tile at the centre of `map_rect_`. */
    IntPair ViewCentre(int ui_scale) const;

    /** @brief Recomputes and stores `view_` (`x0, y0, x1, y1`) clipped to `game`'s map bounds. */
    const ViewBounds& UpdateViewTiles(const Game& game, int ui_scale);

    /** @brief True when tile `(x, y)` lies inside `view_` computed for the current frame. */
    bool Shown(int x, int y) const;

    /** @brief True when the centre of tile `(x, y)` lies inside the current viewport. */
    bool InView(int x, int y, int ui_scale) const;

    /** @brief Clamps `(cx_, cy_)` to `game`'s map bounds (or centres smaller maps). */
    void ClampCamera(const Game* game, int ui_scale);

    /** @brief Centres the viewport on tile `(x, y)`, clamped to `game`'s map edges. */
    void CentreOn(int x, int y, const Game* game, int ui_scale);

    /** @brief Maximum map zoom allowed for `disp_state`. */
    static int MaxZoom(const display::State& disp_state);

    /**
     * @brief Zooms the map to `zoom` around UI point `(x, y)` (or viewport centre when `centre`).
     * @return True when `zoom_` changed (so callers may persist the new zoom preference).
     */
    bool SetZoomAt(int zoom, double x, double y, bool centre, const display::State& disp_state,
                   const Game* game);

    /** @brief Zooms the map to `zoom` around the centre of `map_rect_`. */
    bool SetZoom(int zoom, const display::State& disp_state, const Game* game);

    /** @brief Pans `(cx_, cy_)` by `(dx_tiles, dy_tiles)` and clamps to `game`'s map bounds. */
    void PanTiles(double dx_tiles, double dy_tiles, const Game* game, int ui_scale);

    /** @brief Sets `(cx_, cy_)` directly and clamps to `game`'s map bounds. */
    void SetCameraPos(double cam_x, double cam_y, const Game* game, int ui_scale);

    /** @brief Converts UI pixel `(point_x, point_y)` in `map_rect_` to a tile, or `std::nullopt`.
     */
    OptionalIntPair TileAtPoint(int point_x, int point_y, int ui_scale) const;

    /** @brief Updates `strat_scroll_` to keep the camera box centred on short screens. */
    void FollowView(const Game& game, int ui_scale);

    /** @brief Sizes `display_win`'s logical frame and recomputes `layout_` when needed. */
    void SyncLayout(display::DisplayWindow& display_win, screen::Screen& main_screen,
                    const Font& menu_font, const Game* game, bool starting);

    /** @brief Runs `change()` (e.g. fullscreen/scale switch) and relayouts while preserving centre.
     */
    void RelayoutPreservingCentre(display::DisplayWindow& display_win, screen::Screen& main_screen,
                                  const Font& menu_font, const Game* game, bool starting,
                                  const RelayoutAction& change);

  private:
    void ApplyLayout(const layout::Layout& new_layout, const display::State& disp_state,
                     screen::Screen& main_screen, const Font& menu_font, const Game* game);

    double cx_ = 0.0;             ///< Fractional map tile x at `map_rect_` top-left.
    double cy_ = 0.0;             ///< Fractional map tile y at `map_rect_` top-left.
    int zoom_ = 1;                ///< Device pixels per map pixel (`1..MaxZoom`).
    layout::Layout layout_{};     ///< Placed main-screen group offsets and rects.
    bool have_layout_ = false;    ///< True once `layout_` matches the current UI frame size.
    layout::Rect map_rect_{};     ///< Placed main map viewport rectangle.
    layout::Rect strat_rect_{};   ///< Placed strategic-map rectangle.
    layout::Rect bar_rect_{};     ///< Placed bottom-bar rectangle.
    layout::Rect menu_rect_{};    ///< Placed menu-bar rectangle.
    layout::Rect panel_rect_{};   ///< Placed map-frame panel rectangle.
    menu::Layout menu_layout_{};  ///< Measured menu titles and dropdown rows.
    ViewBounds view_{};           ///< Visible inclusive tile bounds `(x0, y0, x1, y1)`.
    int strat_scroll_ = 0;        ///< Vertical strategic-map scroll offset on short screens.
    int strat_viewer_ = -99;      ///< Side index `strat_image_` was rendered for (`-99` dirty).
    gfx::ImageP strat_image_;     ///< Cached 224x312 strategic-map texture.
};

}  // namespace w2
