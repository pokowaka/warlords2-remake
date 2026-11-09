#include <algorithm>
#include <array>
#include <cmath>
#include <set>

#include "front/map_screen_controller.hpp"
#include "ui/kit.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/game.hpp"
#include "warlords/site.hpp"

namespace w2 {

namespace {

constexpr std::array<int, 7> kVectorMarkX = {0, 32, 48, 16, 64, 80, 96};

int OwnerOrNeutral(int owner) {
    return owner == kNone ? 8 : owner;
}

void LinePixels(gfx::Renderer& ren, int start_x, int start_y, int end_x, int end_y) {
    int dx = std::abs(end_x - start_x);
    int dy = -std::abs(end_y - start_y);
    int step_x = start_x < end_x ? 1 : -1;
    int step_y = start_y < end_y ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        ren.Rectangle(gfx::kFill, start_x, start_y, 1, 1);
        if (start_x == end_x && start_y == end_y) break;
        int err2 = 2 * err;
        if (err2 >= dy) {
            err += dy;
            start_x += step_x;
        }
        if (err2 <= dx) {
            err += dx;
            start_y += step_y;
        }
    }
}

}  // namespace

void MapScreenController::DrawStrategicMap(int x, int y, const City* mark, bool no_cities,
                                           const OwnerOverrideList* owners, int scroll, int rows) {
    UiContext& ctx = Ctx();
    Game& game = ctx.ActiveGame();
    gfx::Renderer& ren = ctx.Renderer();
    int viewer = game::ViewerSide(game, ctx.Player());
    const gfx::ImageP& strat_img =
            ctx.GetViewport().EnsureStratImage(ren, ctx.Assets().MainScreen(), game, viewer, true);

    ren.SetScissor(x, y, 224, rows);
    y -= scroll;
    ren.SetColor(1, 1, 1);
    ren.Draw(strat_img, x, y);
    if (!no_cities) {
        auto cities = game.GetMap().Cities();
        for (size_t i = 0; i < cities.size(); i++) {
            const City& city = cities[i];
            int owner = owners && i < owners->size() ? (*owners)[i] : kNone;
            bool gone = owners ? owner == 0xff : city.IsRazed();
            if (!gone && game::Seen(game, viewer, city.X(), city.Y())) {
                int side = owners ? owner : OwnerOrNeutral(city.OwnerIndex());
                ren.Draw(ctx.Assets().AtransShields(), gfx::NewQuad(side * 16, 30, 8, 8),
                         x + city.X() * 2 - 1, y + city.Y() * 2 - 1);
            }
        }
    }
    if (mark) {
        int mark_x = x + mark->X() * 2 - 1;
        int mark_y = y + mark->Y() * 2 - 1;
        kit::SetPal(ctx, 15);
        ren.Rectangle(gfx::kFill, mark_x - 1, mark_y - 1, 10, 1);
        ren.Rectangle(gfx::kFill, mark_x - 1, mark_y + 9, 10, 1);
        ren.Rectangle(gfx::kFill, mark_x - 1, mark_y - 1, 1, 10);
        ren.Rectangle(gfx::kFill, mark_x + 9, mark_y - 1, 1, 10);
    }
    if (ctx.ModernUi() && !no_cities && ctx.Player() && viewer != kNone) {
        for (int ally = 0; ally < 8; ally++) {
            if (ally != ctx.Player()->Index() &&
                diplomacy::IsAllied(game, ctx.Player()->Index(), ally)) {
                int target_index = diplomacy::AlliedTargetCity(game, ally);
                if (const City* target = game.GetMap().CityAt(target_index)) {
                    if (!target->IsRazed() && game::Seen(game, viewer, target->X(), target->Y())) {
                        int target_x = x + target->X() * 2 - 1;
                        int target_y = y + target->Y() * 2 - 1;
                        kit::SetPal(ctx, 11);
                        ren.Rectangle(gfx::kFill, target_x - 2, target_y - 2, 12, 1);
                        ren.Rectangle(gfx::kFill, target_x - 2, target_y + 10, 12, 1);
                        ren.Rectangle(gfx::kFill, target_x - 2, target_y - 2, 1, 12);
                        ren.Rectangle(gfx::kFill, target_x + 10, target_y - 2, 1, 12);
                    }
                }
            }
        }
    }
    ren.SetScissor();
}

void MapScreenController::DrawSiteMarkers(int x, int y, const Site* highlight) {
    UiContext& ctx = Ctx();
    Game& game = ctx.ActiveGame();
    gfx::Renderer& ren = ctx.Renderer();
    int viewer = game::ViewerSide(game, ctx.Player());
    ren.SetScissor(x, y, 224, 312);
    for (const auto& site : game.GetMap().Sites()) {
        if (!(viewer == kNone || (ctx.Player() && site::ShownTo(site, *ctx.Player()) &&
                                  game::Seen(game, viewer, site.x, site.y)))) {
            continue;
        }
        int src_x = 112;
        int src_y = 0;
        if (site.content == site::kTemple) {
            src_y = 20;
        } else if (site.searched) {
            src_y = 10;
        } else if (site.rich) {
            src_x = 128;
        }
        int marker_x = (site.x * 2 - 1 + 4) / 8 * 8;
        int marker_y = site.y * 2 - 1;
        ren.SetColor(1, 1, 1);
        ren.Draw(ctx.Assets().AtransShields(), gfx::NewQuad(src_x, src_y, 16, 10), x + marker_x,
                 y + marker_y);
        if (&site == highlight) {
            kit::SetPal(ctx, 15);
            kit::Outline(ctx, x + marker_x, y + marker_y, 12, 10);
        }
    }
    ren.SetScissor();
}

void MapScreenController::DrawStrategic() {
    UiContext& ctx = Ctx();
    Viewport& viewport = ctx.GetViewport();
    gfx::Renderer& ren = ctx.Renderer();
    int ui_scale = ctx.Display().GetState().scale;
    viewport.FollowView(ctx.ActiveGame(), ui_scale);
    const auto& rect = viewport.StratRect();
    DrawStrategicMap(rect.x, rect.y, nullptr, false, nullptr, viewport.StratScroll(), rect.h);
    ren.SetScissor(rect.x, rect.y, rect.w, rect.h);
    kit::SetPal(ctx, 15);
    double scale = ui_scale;
    auto snap = [scale](double value) { return std::floor(value * scale + 0.5) / scale; };
    auto [view_w, view_h] = viewport.ViewSize(ui_scale);
    double box_x = rect.x + snap(viewport.Cx() * 2.0);
    double box_y = rect.y - viewport.StratScroll() + snap(viewport.Cy() * 2.0);
    double box_w = snap(view_w * 2.0);
    double box_h = snap(view_h * 2.0);
    ren.Rectangle(gfx::kFill, box_x, box_y, box_w, 2);
    ren.Rectangle(gfx::kFill, box_x, box_y + box_h - 2, box_w, 2);
    ren.Rectangle(gfx::kFill, box_x, box_y, 2, box_h);
    ren.Rectangle(gfx::kFill, box_x + box_w - 2, box_y, 2, box_h);
    ren.SetScissor();
}

void MapScreenController::DrawVectorMap(int x, int y, City* city, int filter, bool see_all) {
    UiContext& ctx = Ctx();
    Game& game = ctx.ActiveGame();
    gfx::Renderer& ren = ctx.Renderer();
    const gfx::ImageP& strat_img = ctx.GetViewport().EnsureStratImage(
            ren, ctx.Assets().MainScreen(), game, ctx.Player()->Index(), false);

    ren.SetScissor(x, y, 224, 312);
    ren.SetColor(1, 1, 1);
    ren.Draw(strat_img, x, y);
    auto marker = [&](const City* target_city, int mark_idx) {
        ren.SetColor(1, 1, 1);
        ren.Draw(ctx.Assets().AtransShields(),
                 gfx::NewQuad(kVectorMarkX[static_cast<size_t>(mark_idx)], 94, 16, 10),
                 x + target_city->X() * 2 - 2, y + target_city->Y() * 2 - 1);
    };
    auto draw_line = [&](const City* from_city, const City* to_city, int colour) {
        kit::SetPal(ctx, colour);
        LinePixels(ren, x + from_city->X() * 2 + 2, y + from_city->Y() * 2 + 2,
                   x + to_city->X() * 2 + 2, y + to_city->Y() * 2 + 2);
    };
    auto mine = game::SideCities(game, *ctx.Player());
    auto building = [](const City* target_city) { return game::Producing(*target_city) ? 0 : 3; };
    auto [std_x, std_y] = game::StandardAt(game, *ctx.Player());
    auto to_standard = [&, flag_x = std_x, flag_y = std_y](const City* src_city, int colour) {
        kit::SetPal(ctx, colour);
        LinePixels(ren, x + src_city->X() * 2 + 2, y + src_city->Y() * 2 + 2, x + flag_x * 2,
                   y + flag_y * 2);
    };
    if (see_all) {
        std::set<const City*> done;
        for (City* owned_city : mine) {
            if (done.count(owned_city)) continue;
            done.insert(owned_city);
            auto incoming = game::VectoredTo(game, owned_city);
            int mark_idx = building(owned_city);
            if (owned_city->IsVectored()) mark_idx = 2;
            if (!incoming.empty()) mark_idx = game::Producing(*owned_city) ? 1 : 5;
            marker(owned_city, mark_idx);
            City* dest = game.GetMap().CityById(owned_city->VectoredCityId());
            if (dest) {
                done.insert(dest);
                marker(dest, game::Producing(*dest) ? 1 : 5);
                draw_line(dest, owned_city, 7);
            } else if (owned_city->IsVectoredToStandard() && std_x != kNone) {
                to_standard(owned_city, 8);
            }
            for (City* src : incoming) {
                done.insert(src);
                marker(src, 2);
                draw_line(src, owned_city, 8);
            }
        }
        kit::SetPal(ctx, 15);
        int box_x = x + (city ? city->X() : -10) * 2 - 3;
        int box_y = y + (city ? city->Y() : -10) * 2 - 2;
        ren.Rectangle(gfx::kFill, box_x, box_y, 11, 1);
        ren.Rectangle(gfx::kFill, box_x, box_y + 11, 11, 1);
        ren.Rectangle(gfx::kFill, box_x, box_y, 1, 11);
        ren.Rectangle(gfx::kFill, box_x + 11, box_y, 1, 11);
    } else if (city) {
        for (City* owned_city : mine) {
            int vec_count = static_cast<int>(game::VectoredTo(game, owned_city).size());
            bool full =
                    (filter == -1 && vec_count + 1 > game::kMaxVectoredTo) ||
                    (filter >= 0 && filter != kNone && vec_count + filter > game::kMaxVectoredTo);
            if (owned_city == city || !full) {
                int mark_idx = building(owned_city);
                if (owned_city == city) mark_idx = game::Producing(*owned_city) ? 4 : 6;
                marker(owned_city, mark_idx);
            }
        }
        City* dest = game.GetMap().CityById(city->VectoredCityId());
        if (dest) {
            marker(dest, game::Producing(*dest) ? 1 : 5);
            draw_line(city, dest, 7);
        } else if (city->IsVectoredToStandard() && std_x != kNone) {
            to_standard(city, 7);
        }
        for (City* src : game::VectoredTo(game, city)) {
            marker(src, 2);
            draw_line(src, city, 8);
        }
    }
    if (std_x != kNone) {
        ren.SetColor(1, 1, 1);
        ren.Draw(ctx.Assets().AtransShields(), gfx::NewQuad(96, 15, 16, 15),
                 x + std::max(0, (std_x * 2 - 2) / 8 * 8), y + std::max(0, std_y * 2 - 6));
    }
    ren.SetScissor();
}

void MapScreenController::DrawHeroFigure(int x, int y, int tile_x, int tile_y, bool dark) {
    UiContext& ctx = Ctx();
    gfx::Renderer& ren = ctx.Renderer();
    ren.SetScissor(x, y, 224, 312);
    ren.SetColor(1, 1, 1);
    ren.Draw(ctx.Assets().AtransShields(), gfx::NewQuad(96, dark ? 15 : 0, 16, 15),
             x + std::max(0, (tile_x * 2 - 2) / 8 * 8), y + std::max(0, tile_y * 2 - 6));
    ren.SetScissor();
}

void MapScreenController::DrawStrategicPanel(int x, int y, const City* city) {
    DrawStrategicMap(x, y, city);
}

}  // namespace w2
