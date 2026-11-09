#include "ui/viewport.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>

#include "warlords/game.hpp"

namespace w2 {

void Viewport::StratDirty() {
    strat_image_.reset();
    strat_viewer_ = -99;
}

const gfx::ImageP& Viewport::EnsureStratImage(gfx::Renderer& renderer, screen::Screen& main_screen,
                                              const Game& game, int viewer, bool match_viewer) {
    if (!strat_image_ || (match_viewer && strat_viewer_ != viewer)) {
        strat_image_ = screen::StrategicImage(renderer, main_screen, game, viewer);
        if (match_viewer) strat_viewer_ = viewer;
    }
    return strat_image_;
}

Viewport::TileSpanPair Viewport::ViewSize(int ui_scale) const {
    double tiles_per_ui = static_cast<double>(ui_scale) / (kTilePixels * zoom_);
    return {map_rect_.w * tiles_per_ui, map_rect_.h * tiles_per_ui};
}

Viewport::IntPair Viewport::CamDev() const {
    double tile_size = kTilePixels * zoom_;
    return {static_cast<int>(std::floor(cx_ * tile_size + 0.5)),
            static_cast<int>(std::floor(cy_ * tile_size + 0.5))};
}

Viewport::TileSpanPair Viewport::UiToMap(double x, double y, int ui_scale) const {
    double scale = ui_scale;
    double tile_size = kTilePixels * zoom_;
    auto [cam_x, cam_y] = CamDev();
    return {(cam_x + (x - map_rect_.x) * scale) / tile_size,
            (cam_y + (y - map_rect_.y) * scale) / tile_size};
}

Viewport::TileSpanPair Viewport::MapToUi(double tile_x, double tile_y, int ui_scale) const {
    double scale = ui_scale;
    double tile_size = kTilePixels * zoom_;
    auto [cam_x, cam_y] = CamDev();
    return {map_rect_.x + (tile_x * tile_size - cam_x) / scale,
            map_rect_.y + (tile_y * tile_size - cam_y) / scale};
}

Viewport::IntPair Viewport::ViewCentre(int ui_scale) const {
    auto [view_w, view_h] = ViewSize(ui_scale);
    return {static_cast<int>(std::floor(cx_ + view_w / 2.0)),
            static_cast<int>(std::floor(cy_ + view_h / 2.0))};
}

const Viewport::ViewBounds& Viewport::UpdateViewTiles(const Game& game, int ui_scale) {
    auto [view_w, view_h] = ViewSize(ui_scale);
    int map_w = game.GetMap().Width();
    int map_h = game.GetMap().Height();
    view_ = {std::max(0, static_cast<int>(std::floor(cx_))),
             std::max(0, static_cast<int>(std::floor(cy_))),
             std::min(map_w - 1, static_cast<int>(std::ceil(cx_ + view_w)) - 1),
             std::min(map_h - 1, static_cast<int>(std::ceil(cy_ + view_h)) - 1)};
    return view_;
}

bool Viewport::Shown(int x, int y) const {
    return x >= view_[0] && y >= view_[1] && x <= view_[2] && y <= view_[3];
}

bool Viewport::InView(int x, int y, int ui_scale) const {
    auto [view_w, view_h] = ViewSize(ui_scale);
    return x + 0.5 > cx_ && x + 0.5 < cx_ + view_w && y + 0.5 > cy_ && y + 0.5 < cy_ + view_h;
}

void Viewport::ClampCamera(const Game* game, int ui_scale) {
    if (!game || !game->HasMap()) return;
    auto [view_w, view_h] = ViewSize(ui_scale);
    int map_w = game->GetMap().Width();
    int map_h = game->GetMap().Height();
    cx_ = view_w >= map_w ? (map_w - view_w) / 2.0 : std::max(0.0, std::min(cx_, map_w - view_w));
    cy_ = view_h >= map_h ? (map_h - view_h) / 2.0 : std::max(0.0, std::min(cy_, map_h - view_h));
}

void Viewport::CentreOn(int x, int y, const Game* game, int ui_scale) {
    auto [view_w, view_h] = ViewSize(ui_scale);
    cx_ = x + 0.5 - view_w / 2.0;
    cy_ = y + 0.5 - view_h / 2.0;
    ClampCamera(game, ui_scale);
}

int Viewport::MaxZoom(const display::State& disp_state) {
    return std::max(disp_state.max_scale, disp_state.scale) + 2;
}

bool Viewport::SetZoomAt(int zoom, double x, double y, bool centre,
                         const display::State& disp_state, const Game* game) {
    zoom = std::max(1, std::min(MaxZoom(disp_state), zoom));
    if (zoom == zoom_) return false;
    if (centre) {
        x = map_rect_.x + map_rect_.w / 2.0;
        y = map_rect_.y + map_rect_.h / 2.0;
    }
    auto [tile_x, tile_y] = UiToMap(x, y, disp_state.scale);
    zoom_ = zoom;
    double tiles_per_ui = static_cast<double>(disp_state.scale) / (kTilePixels * zoom_);
    cx_ = tile_x - (x - map_rect_.x) * tiles_per_ui;
    cy_ = tile_y - (y - map_rect_.y) * tiles_per_ui;
    ClampCamera(game, disp_state.scale);
    return true;
}

bool Viewport::SetZoom(int zoom, const display::State& disp_state, const Game* game) {
    return SetZoomAt(zoom, 0.0, 0.0, /*centre=*/true, disp_state, game);
}

void Viewport::PanTiles(double dx_tiles, double dy_tiles, const Game* game, int ui_scale) {
    cx_ += dx_tiles;
    cy_ += dy_tiles;
    ClampCamera(game, ui_scale);
}

void Viewport::SetCameraPos(double cam_x, double cam_y, const Game* game, int ui_scale) {
    cx_ = cam_x;
    cy_ = cam_y;
    ClampCamera(game, ui_scale);
}

Viewport::OptionalIntPair Viewport::TileAtPoint(int point_x, int point_y, int ui_scale) const {
    if (point_x < map_rect_.x || point_y < map_rect_.y || point_x >= map_rect_.x + map_rect_.w ||
        point_y >= map_rect_.y + map_rect_.h) {
        return std::nullopt;
    }
    auto [tile_x, tile_y] = UiToMap(point_x, point_y, ui_scale);
    return std::make_pair(static_cast<int>(std::floor(tile_x)),
                          static_cast<int>(std::floor(tile_y)));
}

void Viewport::FollowView(const Game& game, int ui_scale) {
    int full = game.GetMap().Height() * 2;
    auto [view_w, view_h] = ViewSize(ui_scale);
    int scroll = static_cast<int>(std::lround(cy_ + view_h / 2.0 - strat_rect_.h / 4.0)) * 2;
    strat_scroll_ = std::clamp(scroll, 0, std::max(0, full - strat_rect_.h));
}

void Viewport::ApplyLayout(const layout::Layout& new_layout, const display::State& disp_state,
                           screen::Screen& main_screen, const Font& menu_font, const Game* game) {
    std::optional<TileSpanPair> mid;
    if (game != nullptr && have_layout_) {
        auto [view_w, view_h] = ViewSize(disp_state.scale);
        mid = std::make_pair(cx_ + view_w / 2.0, cy_ + view_h / 2.0);
    }
    layout_ = new_layout;
    have_layout_ = true;
    screen::Relayout(main_screen, new_layout);
    auto reg = [&](int region_id) {
        const auto* region = screen::FindRegion(main_screen, region_id);
        return region ? layout::Rect{region->x, region->y, region->w, region->h, region->id}
                      : layout::Rect{};
    };
    map_rect_ = reg(screen::kMap);
    strat_rect_ = reg(screen::kStrategic);
    bar_rect_ = reg(screen::kBottombar);
    menu_rect_ = reg(screen::kMenubar);
    panel_rect_ = reg(screen::kMappanel);
    int max_zoom = MaxZoom(disp_state);
    menu_layout_ = menu::MakeLayout(menu_font, new_layout.w,
                                    menu::WithZooms(disp_state.max_short, max_zoom,
                                                    disp_state.smooth_scale, disp_state.shrink));
    zoom_ = std::max(1, std::min(max_zoom, zoom_ != 0 ? zoom_ : disp_state.scale));
    if (mid) {
        auto [view_w, view_h] = ViewSize(disp_state.scale);
        cx_ = mid->first - view_w / 2.0;
        cy_ = mid->second - view_h / 2.0;
        ClampCamera(game, disp_state.scale);
    }
}

void Viewport::SyncLayout(display::DisplayWindow& display_win, screen::Screen& main_screen,
                          const Font& menu_font, const Game* game, bool starting) {
    int width = layout::kScreenW;
    int height = layout::kScreenH;
    if (!starting) std::tie(width, height) = display_win.UiSize();
    display_win.SetFrame(std::max(width, layout::kScreenW),
                         std::max(height, starting ? layout::kScreenH : layout::kShortH));
    const display::State& disp_state = display_win.GetState();
    if (!have_layout_ || layout_.w != disp_state.w || layout_.h != disp_state.h) {
        ApplyLayout(layout::Compute(disp_state.w, disp_state.h), disp_state, main_screen, menu_font,
                    game);
    }
}

void Viewport::RelayoutPreservingCentre(display::DisplayWindow& display_win,
                                        screen::Screen& main_screen, const Font& menu_font,
                                        const Game* game, bool starting,
                                        const RelayoutAction& change) {
    std::optional<TileSpanPair> mid;
    int prev_scale = display_win.GetState().scale;
    if (game != nullptr && !starting && have_layout_) {
        auto [view_w, view_h] = ViewSize(prev_scale);
        mid = std::make_pair(cx_ + view_w / 2.0, cy_ + view_h / 2.0);
    }
    change();
    have_layout_ = false;
    SyncLayout(display_win, main_screen, menu_font, game, starting);
    if (mid) {
        int new_scale = display_win.GetState().scale;
        auto [view_w, view_h] = ViewSize(new_scale);
        cx_ = mid->first - view_w / 2.0;
        cy_ = mid->second - view_h / 2.0;
        ClampCamera(game, new_scale);
    }
}

}  // namespace w2
