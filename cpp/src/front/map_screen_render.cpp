#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <vector>

#include "front/map_screen_controller.hpp"
#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/game.hpp"
#include "warlords/scn.hpp"
#include "warlords/uidata.hpp"

namespace w2 {

namespace {

constexpr int kNavyTypeIndex = 5;
constexpr std::array<int, 4> kFlagY = {29, 38, 47, 56};
constexpr std::array<int, 2> kWalkRing = {496, 31};
constexpr std::array<int, 2> kWalkCrossed = {496, 48};
constexpr int kWalkRingW = 16;
constexpr int kWalkRingH = 14;
constexpr std::array<int, 2> kWalkRingAt = {16, 13};
constexpr std::array<int, 2> kWalkGhostAt = {4, 4};

int OwnerOrNeutral(int owner) {
    return owner == kNone ? 8 : owner;
}

gfx::Quad TileQuad(int tile) {
    int cell = tile % 96;
    return gfx::NewQuad((cell % 16) * MapScreenController::kTile,
                        (cell / 16) * MapScreenController::kTile, MapScreenController::kTile,
                        MapScreenController::kTile);
}

gfx::Quad RoadQuad(int road) {
    return gfx::NewQuad((road % 13) * 48, (road / 13) * MapScreenController::kTile,
                        MapScreenController::kTile, MapScreenController::kTile);
}

}  // namespace

Army* MapScreenController::TopArmy(const ArmyList& stack) const {
    const Map& map = Ctx().ActiveGame().GetMap();
    Army* best = nullptr;
    int best_rank = kNone;
    for (Army* army : stack) {
        int rank = map.FightOrder()[static_cast<size_t>(OwnerOrNeutral(army->OwnerIndex()))]
                                   [static_cast<size_t>(army->TypeIndex())];
        if (best_rank == kNone || rank > best_rank) {
            best = army;
            best_rank = rank;
        }
    }
    return best;
}

int MapScreenController::StackFigure(const ArmyList& stack) const {
    for (Army* army : stack) {
        if (army->IsAtSea()) return kNavyTypeIndex;
    }
    Army* top = TopArmy(stack);
    return top ? top->TypeIndex() : kNone;
}

void MapScreenController::DrawFog() {
    UiContext& ctx = Ctx();
    Game& game = ctx.ActiveGame();
    int viewer = game::ViewerSide(game, ctx.Player());
    if (game.GetMap().GameOptions().hidden_map == 0 || viewer == kNone) return;
    const auto& view = ctx.GetViewport().View();
    gfx::Renderer& ren = ctx.Renderer();
    for (int tile_y = view[1]; tile_y <= view[3]; tile_y++) {
        for (int tile_x = view[0]; tile_x <= view[2]; tile_x++) {
            int cell = game::FogCell(game, viewer, tile_x, tile_y);
            if (cell == kNone) continue;
            int pixel_x = tile_x * kTile;
            int pixel_y = tile_y * kTile;
            if (cell == game::kFogBlack) {
                ren.SetColor(0, 0, 0);
                ren.Rectangle(gfx::kFill, pixel_x, pixel_y, kTile, kTile);
            } else {
                ren.SetColor(1, 1, 1);
                ren.Draw(ctx.Assets().FogImg(),
                         gfx::NewQuad((cell % 7) * 48, (cell / 7) * 41, 40, 40), pixel_x, pixel_y);
            }
        }
    }
    ren.SetColor(1, 1, 1);
}

void MapScreenController::DrawStack(int owner, int army_type, int count, int pixel_x, int pixel_y) {
    if (owner < 0 || owner > 8) owner = 8;
    UiContext& ctx = Ctx();
    gfx::Renderer& ren = ctx.Renderer();
    ren.SetColor(1, 1, 1);
    ren.Draw(ctx.Assets().ArmyImg(owner), kit::ArmyQuad(army_type), pixel_x + 8, pixel_y + 7);
    kit::SetPal(ctx, 14);
    ren.Rectangle(gfx::kFill, pixel_x + 2, pixel_y, 1, kTile);
    ren.Rectangle(gfx::kFill, pixel_x + 4, pixel_y, 1, kTile);
    kit::SetPal(ctx, 13);
    ren.Rectangle(gfx::kFill, pixel_x + 3, pixel_y, 1, kTile);
    ren.SetColor(1, 1, 1);
    auto draw_flag = [&](int flag_size, int y) {
        ren.Draw(ctx.Assets().ArmyImg(owner),
                 gfx::NewQuad(464, kFlagY[static_cast<size_t>(flag_size - 1)], 48, 8), pixel_x, y);
    };
    if (count > 4) {
        draw_flag(1, pixel_y + 8);
        count -= 4;
    }
    draw_flag(std::max(1, std::min(count, 4)), pixel_y);
}

void MapScreenController::DrawRoute() {
    if (!route_) return;
    UiContext& ctx = Ctx();
    const Viewport& viewport = ctx.GetViewport();
    gfx::Renderer& ren = ctx.Renderer();
    const Selection* sel = ctx.ActiveSelection();
    Army* ghost = (sel && !sel->stack.empty()) ? sel->stack[0] : nullptr;
    size_t first = state_->IsWalking() ? state_->WalkingStepIndex() + 1 : 1;
    for (size_t i = first; i <= route_->path.size(); i++) {
        const auto& step = route_->path[i - 1];
        if (!viewport.Shown(step.x, step.y)) continue;
        int pixel_x = step.x * kTile;
        int pixel_y = step.y * kTile;
        ren.SetColor(1, 1, 1);
        if (i == route_->path.size()) {
            if (ghost) {
                ren.Draw(ctx.Assets().ShadowImg(), kit::ArmyQuad(ghost->TypeIndex()),
                         pixel_x + kWalkGhostAt[0], pixel_y + kWalkGhostAt[1]);
            }
        } else {
            const auto& src = static_cast<int>(i) <= route_->reach ? kWalkRing : kWalkCrossed;
            ren.Draw(ctx.Assets().ShadowImg(), gfx::NewQuad(src[0], src[1], kWalkRingW, kWalkRingH),
                     pixel_x + kWalkRingAt[0], pixel_y + kWalkRingAt[1]);
        }
    }
}

void MapScreenController::DrawMap() {
    UiContext& ctx = Ctx();
    Game& game = ctx.ActiveGame();
    Viewport& viewport = ctx.GetViewport();
    const auto& view = viewport.UpdateViewTiles(game, ctx.Display().GetState().scale);
    Map& map = game.GetMap();
    gfx::Renderer& ren = ctx.Renderer();

    std::map<int, ArmyList> armies_on;
    for (Army* army : game.Armies()) {
        if (!army->InTransit() && army->X() != kNone && army->X() >= view[0] &&
            army->X() <= view[2] && army->Y() >= view[1] && army->Y() <= view[3]) {
            armies_on[army->X() + army->Y() * 1000].push_back(army);
        }
    }

    std::map<int, Item*> items_on_tile;
    auto map_items = map.Items();
    for (int i = static_cast<int>(map_items.size()) - 1; i >= 0; i--) {
        Item& item = map_items[static_cast<size_t>(i)];
        if (item.status == 1 && item.x != kNone && !items_on_tile.count(item.x + item.y * 1000)) {
            items_on_tile[item.x + item.y * 1000] = &item;
        }
    }

    struct Standing {
        ArmyList stack;
        int x = 0;
        int y = 0;
        int sx = 0;
        int sy = 0;
        bool hidden = false;
    };
    std::vector<Standing> standing;

    for (int tile_y = view[1]; tile_y <= view[3]; tile_y++) {
        for (int tile_x = view[0]; tile_x <= view[2]; tile_x++) {
            int pixel_x = tile_x * kTile;
            int pixel_y = tile_y * kTile;
            int tile = scn::TileAt(map, tile_x, tile_y);
            int sheet = std::min(1, tile / 96);
            ren.SetColor(1, 1, 1);
            ren.Draw(ctx.Assets().Sheet(sheet), TileQuad(tile), pixel_x, pixel_y);
            int road = scn::RoadAt(map, tile_x, tile_y);
            if (road != 0) ren.Draw(ctx.Assets().RoadImg(), RoadQuad(road - 1), pixel_x, pixel_y);
            if (scn::IsCrossing(map, tile_x, tile_y)) {
                ren.Draw(ctx.Assets().AtransShields(), gfx::NewQuad(0, 0, 32, 29), pixel_x,
                         pixel_y);
            }
            auto item_it = items_on_tile.find(tile_x + tile_y * 1000);
            if (item_it != items_on_tile.end()) {
                Item* item = item_it->second;
                if (item->planted && item->standard_of != kNone) {
                    ren.Draw(ctx.Assets().ArmyImg(item->standard_of), kit::ArmyQuad(29), pixel_x,
                             pixel_y);
                } else {
                    ren.Draw(ctx.Assets().AtransShields(), gfx::NewQuad(64, 0, 32, 29), pixel_x,
                             pixel_y);
                }
            }
            auto armies_it = armies_on.find(tile_x + tile_y * 1000);
            if (armies_it != armies_on.end()) {
                ArmyList stack;
                for (Army* army : armies_it->second) {
                    if (!state_->IsArmyWalking(army)) stack.push_back(army);
                }
                if (!stack.empty()) {
                    standing.push_back(
                            Standing{std::move(stack), tile_x, tile_y, pixel_x, pixel_y, false});
                }
            }
        }
    }

    const Selection* sel = ctx.ActiveSelection();
    for (auto& stack_here : standing) {
        if (game::TowerAt(game, stack_here.x, stack_here.y)) {
            int owner = OwnerOrNeutral(stack_here.stack[0]->OwnerIndex());
            if (owner == 15) owner = 8;
            ren.SetColor(1, 1, 1);
            ren.Draw(ctx.Assets().RoadImg(), gfx::NewQuad(owner * 48 + 192, 40, 48, 40),
                     stack_here.sx, stack_here.sy);
            stack_here.hidden = !(sel && sel->x == stack_here.x && sel->y == stack_here.y);
        }
    }

    Army* lead = (sel && !sel->stack.empty()) ? TopArmy(sel->stack) : nullptr;
    if (lead && state_->IsArmyWalking(lead)) lead = nullptr;
    for (const auto& stack_here : standing) {
        if (stack_here.hidden) continue;
        Army* top = TopArmy(stack_here.stack);
        int figure = StackFigure(stack_here.stack);
        if (lead && sel && sel->x == stack_here.x && sel->y == stack_here.y) {
            top = lead;
            figure = lead->IsAtSea() ? kNavyTypeIndex : lead->TypeIndex();
        }
        DrawStack(OwnerOrNeutral(top->OwnerIndex()), figure,
                  static_cast<int>(stack_here.stack.size()), stack_here.sx, stack_here.sy);
    }

    DrawFog();
    DrawRoute();

    const auto* walk_pos = state_->WalkingTile();
    if (walk_pos) {
        Army* walker = state_->WalkingTopArmy();
        if (walker && viewport.Shown(walk_pos->first, walk_pos->second)) {
            ArmyList walking;
            for (Army* army : game.Armies()) {
                if (state_->IsArmyWalking(army)) walking.push_back(army);
            }
            Army* top = TopArmy(walking);
            int figure =
                    state_->WalkingSeaBefore() ? kNavyTypeIndex : (top ? top->TypeIndex() : kNone);
            DrawStack(OwnerOrNeutral(walker->OwnerIndex()), figure,
                      static_cast<int>(walking.size()), walk_pos->first * kTile,
                      walk_pos->second * kTile);
        }
    }

    if (sel) {
        int x = walk_pos ? walk_pos->first : sel->x;
        int y = walk_pos ? walk_pos->second : sel->y;
        if (viewport.Shown(x, y)) {
            ren.SetColor(1, 1, 1);
            int base = sel->stack.size() > 1 ? 4 : 0;
            int frame = base + static_cast<int>(std::floor(Now() * kCursRate)) % 4;
            ren.Draw(ctx.Assets().CursImg(),
                     gfx::NewQuad((frame % 4) * 64, (frame / 4) * 40, 40, 40), x * kTile,
                     y * kTile);
        }
    }
}

void MapScreenController::Update() {
    UiContext& ctx = Ctx();
    ctx.Audio().Update();
    if (auto top = ctx.Modals().TopShared()) top->Update();
}

void MapScreenController::Draw() {
    UiContext& ctx = Ctx();
    gfx::Renderer& ren = ctx.Renderer();
    Viewport& viewport = ctx.GetViewport();

    ren.Begin();
    ren.Clear(0, 0, 0);
    SyncLayout();
    state_->StepFrame(*this);
    ctx.Display().PushUi(ren);

    auto draw_modals = [&ctx]() {
        auto items = ctx.Modals().Items();
        for (auto& modal : items) modal->Draw();
    };

    if (ctx.Starting() || !ctx.GamePtr()) {
        draw_modals();
        DrawMenuBar();
        DrawPointer();
        ren.Pop();
        return;
    }

    state_->UpdateMapAnimation(*this);
    if (ctx.Player() && ctx.ActiveGame().TurnNews(ctx.Player()->Id()).quest_news &&
        !(state_->IsGameOver() || state_->IsAssaulting() || state_->IsWalking() ||
          state_->IsComputerTurn() || move_all_.has_value() || open_menu_ >= 0 ||
          !ctx.Modals().Empty())) {
        questnews::Poll(ctx);
    }

    RefreshControls();
    const auto& layout = viewport.GetLayout();
    screen::DrawBackground(ren, ctx.Assets().MainScreen(), layout);

    const auto& map_r = viewport.MapRect();
    if (map_after_banner_) {
        const auto& strat_r = viewport.StratRect();
        ren.SetColor(0, 0, 0);
        ren.Rectangle(gfx::kFill, map_r.x, map_r.y, map_r.w, map_r.h);
        ren.Rectangle(gfx::kFill, strat_r.x, strat_r.y, strat_r.w, strat_r.h);
        ren.SetColor(1, 1, 1);
    } else {
        ren.SetScissor(map_r.x, map_r.y, map_r.w, map_r.h);
        auto [cam_x, cam_y] = viewport.CamDev();
        ctx.Display().PushMap(ren, map_r.x, map_r.y, cam_x, cam_y, viewport.Zoom());
        DrawMap();
        state_->DrawMapOverlay(*this);
        ren.Pop();
        ren.SetScissor();
        DrawStrategic();
    }

    ren.Push();
    ren.Translate(layout.panel.x, layout.panel.y);
    ren.SetColor(1, 1, 1);
    ren.Draw(ctx.Assets().Marble(), gfx::NewQuad(0, 0, 224, 114), 400, 355);
    ren.Pop();

    screen::DrawControls(ren, ctx.Assets().MainScreen());
    DrawShortcutIcons();

    ren.Push();
    ren.Translate(layout.bar.x, layout.bar.y);
    DrawBottomBar();
    ren.Pop();

    ren.Push();
    ren.Translate(layout.dialog.x, layout.dialog.y);
    draw_modals();
    ren.Pop();

    DrawMenuBar();

    ren.Push();
    ren.Translate(layout.dialog.x, layout.dialog.y);
    state_->DrawDialogOverlay(*this);
    ren.Pop();

    DrawPointer();
    ren.Pop();
}

}  // namespace w2
