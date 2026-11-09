#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/str_format.h"

#include "front/map_screen_controller.hpp"
#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/armytype.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/game.hpp"
#include "warlords/rules.hpp"
#include "warlords/scn.hpp"
#include "warlords/uidata.hpp"

namespace w2 {

namespace {

constexpr int kSlotX = 24;
constexpr int kSlotY = 405;
constexpr int kSlotStep = 40;
constexpr int kSlotMovesX = 8;
constexpr int kSlotMovesY = 31;
constexpr int kMarkY = 449;
constexpr int kMarkW = 32;
constexpr int kMarkH = 16;
constexpr int kStatusY = 425;
constexpr std::array<int, 12> kPtrHot = {0, 6, 8, 6, 8, 8, 8, 8, 0, 8, 8, 0};

struct StatusCell {
    int sx;
    int sy;
    int x;
    int tx;
    const char* fmt;
};
constexpr std::array<StatusCell, 4> kStatusCells = {{{344, 0, 32, 72, "%d"},
                                                     {344, 20, 120, 144, "%dgp"},
                                                     {384, 0, 200, 232, "%dgp"},
                                                     {384, 20, 280, 320, "%dgp"}}};

}  // namespace

void MapScreenController::DrawShortcutIcons() {
    UiContext& ctx = Ctx();
    gfx::Renderer& ren = ctx.Renderer();
    screen::Screen& main_screen = ctx.Assets().MainScreen();
    const auto& main_ui = main_screen.ui;
    const auto* art = main_screen.ArtFor(ren, uidata::kShortcutBitmap);
    const Font& font = ctx.Assets().TextFont();
    for (int i = 0; i < uidata::kShortcutCount; i++) {
        const auto* control = screen::FindControl(main_screen, uidata::kShortcutFirst + i);
        auto shortcut = main_ui.shortcuts.find(i);
        if (!control || shortcut == main_ui.shortcuts.end()) continue;
        auto item = main_ui.shortcut_items.find(shortcut->second);
        if (item == main_ui.shortcut_items.end()) continue;
        if (art) {
            auto src = item->second.src[main_screen.StateOf(control->id)];
            ren.SetColor(1, 1, 1);
            ren.Draw(art->image, gfx::NewQuad(src.x, src.y, item->second.w, item->second.h),
                     control->x, control->y);
        } else {
            std::string short_name = item->second.name.substr(0, 6);
            ren.SetColor(1, 1, 1);
            font.Draw(short_name,
                      control->x + std::max(1, (control->w - font.Width(short_name)) / 2),
                      control->y + (control->h - font.LineHeight()) / 2);
        }
    }
}

void MapScreenController::DrawTurnStrip() {
    UiContext& ctx = Ctx();
    gfx::Renderer& ren = ctx.Renderer();
    Game& game = ctx.ActiveGame();
    kit::SetPal(ctx, 15);
    ren.Rectangle(gfx::kFill, 437, 0, 202, 17);
    std::vector<Side*> alive;
    for (Side* side : game.Sides()) {
        if (side->IsAlive()) alive.push_back(side);
    }
    int count = static_cast<int>(alive.size());
    std::string turn = absl::StrFormat("Turn %d", game.Turn());
    const Font& font = ctx.Assets().BigFont().Colours(0, 15);
    font.Draw(turn, (8 - count) * 16 + 508 - font.Width(turn), 0);
    Side* comp_side = state_->ActiveComputerSide();
    const Side* active_side = (comp_side && !HumansLeft()) ? comp_side : ctx.Player();
    for (int i = 0; i < count; i++) {
        int x = (8 - count + i) * 16 + 512;
        if (alive[static_cast<size_t>(i)] == active_side) {
            kit::SetPal(ctx, 0);
            ren.Rectangle(gfx::kFill, x - 3, 1, 17, 15);
        }
        int side_index = alive[static_cast<size_t>(i)]->Index();
        ren.SetColor(1, 1, 1);
        ren.Draw(ctx.Assets().AtransShields(),
                 gfx::NewQuad(112 + (side_index / 4) * 16, 94 + (side_index % 4) * 14, 16, 14), x,
                 2);
    }
}

void MapScreenController::DrawMenuBar() {
    UiContext& ctx = Ctx();
    gfx::Renderer& ren = ctx.Renderer();
    const Viewport& viewport = ctx.GetViewport();
    const auto& layout = viewport.GetLayout();
    const auto& menu_layout = viewport.MenuLayout();
    const Font& text_font = ctx.Assets().TextFont();

    kit::SetPal(ctx, 15);
    ren.Rectangle(gfx::kFill, 0, 0, layout.w, menu::kBarH);
    for (size_t i = 0; i < menu_layout.size(); i++) {
        const auto& menu_entry = menu_layout[i];
        bool lit = static_cast<int>(i) == open_menu_;
        if (lit) {
            kit::SetPal(ctx, 8);
            ren.Rectangle(gfx::kFill, menu_entry.x, 0, menu_entry.w, menu::kBarH);
        }
        ren.SetColor(1, 1, 1);
        text_font.Colours(lit ? 7 : 0, lit ? 8 : 15)
                .Draw(menu_entry.title, menu_entry.x + 2, menu::kBarY);
    }
    if (!ctx.Starting() && ctx.GamePtr()) {
        ren.Push();
        ren.Translate(layout.ex, 0);
        DrawTurnStrip();
        ren.Pop();
    }
    if (open_menu_ < 0 || open_menu_ >= static_cast<int>(menu_layout.size())) return;

    const auto& drop = menu_layout[static_cast<size_t>(open_menu_)].drop;
    kit::SetPal(ctx, 15);
    ren.Rectangle(gfx::kFill, drop.x, drop.y, drop.w - 1, drop.h - 1);
    kit::SetPal(ctx, 0);
    ren.Rectangle(gfx::kFill, drop.x, drop.y + drop.h - 2, drop.w - 2, 1);
    ren.Rectangle(gfx::kFill, drop.x + 2, drop.y + drop.h - 1, drop.w - 2, 1);
    ren.Rectangle(gfx::kFill, drop.x, drop.y, 1, drop.h - 2);
    ren.Rectangle(gfx::kFill, drop.x + drop.w - 2, drop.y, 1, drop.h - 1);
    ren.Rectangle(gfx::kFill, drop.x + drop.w - 1, drop.y + 1, 1, drop.h - 1);

    const menu::Row* hover = menu::RowAt(menu_layout, open_menu_, mouse_x_, mouse_y_);
    for (const auto& row : drop.rows) {
        if (row.label == "-") {
            kit::SetPal(ctx, 0);
            ren.Rectangle(gfx::kFill, row.x + 1, row.y - 1, drop.w - 2, 1);
            continue;
        }
        std::string key = row.key.empty() ? row.act : row.key;
        bool grey = !MenuEnabled(key);
        bool lit = &row == hover && !grey;
        if (lit) {
            kit::SetPal(ctx, 8);
            ren.Rectangle(gfx::kFill, row.x + 1, row.y, drop.w - 3, row.h);
        }
        int glyph = grey ? 3 : (lit ? 7 : 0);
        const Font& font = text_font.Colours(glyph, lit ? 8 : 15);
        ren.SetColor(1, 1, 1);
        font.Draw(row.label, row.x + 3, row.y);
        if (!row.key.empty()) font.Draw(row.key, row.x + drop.key_col, row.y);
        if (MenuTicked(key)) {
            kit::SetPal(ctx, glyph);
            int centre_x = row.x + drop.key_col + 2;
            int centre_y = row.y + row.h / 2;
            for (int j = -2; j <= 2; j++) {
                int half = 2 - std::abs(j);
                ren.Rectangle(gfx::kFill, centre_x - half, centre_y + j, half * 2 + 1, 1);
            }
        }
    }
}

void MapScreenController::DrawAbits(int src_x, int src_y, int width, int height, int x, int y) {
    UiContext& ctx = Ctx();
    ctx.Renderer().SetColor(1, 1, 1);
    ctx.Renderer().Draw(ctx.Assets().Abits(), gfx::NewQuad(src_x, src_y, width, height), x, y);
}

void MapScreenController::DrawDigits(int number, int x, int y) {
    std::string text = absl::StrFormat("%02d", std::max(0, std::min(99, number)));
    for (size_t i = 0; i < text.size(); i++) {
        DrawAbits(64 + (text[i] - '0') * 8, 30, 8, 8, x + static_cast<int>(i) * 8, y);
    }
}

int MapScreenController::StatusValue(int cell) const {
    const UiContext& ctx = Ctx();
    switch (cell) {
    case 0:
        return static_cast<int>(game::SideCities(ctx.ActiveGame(), *ctx.Player()).size());
    case 1:
        return ctx.Player()->Gold();
    case 2:
        return game::Income(ctx.ActiveGame(), *ctx.Player());
    default:
        return game::Upkeep(ctx.ActiveGame(), *ctx.Player());
    }
}

void MapScreenController::DrawArmySlots() {
    UiContext& ctx = Ctx();
    gfx::Renderer& ren = ctx.Renderer();
    const Selection* sel = ctx.ActiveSelection();
    const slots::Slots* slot_state = sel ? &sel->slots : nullptr;
    int player_idx = ctx.Player()->Index();
    for (int i = 0; i < kSlotCount; i++) {
        int x = kSlotX + i * kSlotStep;
        Army* army = (slot_state && i < slot_state->n) ? slot_state->army[i] : nullptr;
        ren.SetColor(1, 1, 1);
        int ring = army ? ((player_idx + slot_state->group[i]) % 8 + 1) : 0;
        ren.Draw(ctx.Assets().Abits(), gfx::NewQuad(ring * 32, 0, 32, 30), x, kSlotY);
        if (army) {
            ren.Draw(slot_state->in_group[i] ? ctx.Assets().ArmyImg(player_idx)
                                             : ctx.Assets().ShadowImg(),
                     kit::ArmyQuad(army->TypeIndex()), x, kSlotY);
            DrawDigits(army->Moves(), x + kSlotMovesX, kSlotY + kSlotMovesY);
            if (slot_state->mark[i] != slots::kNomark) {
                DrawAbits(448, slot_state->mark[i] == slots::kTick ? 16 : 0, kMarkW, kMarkH, x,
                          kMarkY);
            }
        }
    }
    DrawAbits(0, 30, 32, 8, 344, 420);
    DrawAbits(32, 30, 32, 8, 344, 428);
    DrawDigits(slot_state ? slots::Moves(*slot_state) : 0, 352, 436);
    bool green = slot_state && slots::Grouped(*slot_state);
    DrawAbits(288, green ? 19 : 0, 32, 19, 344, 447);

    ArmyList moving;
    if (slot_state) {
        for (int i = 0; i < slot_state->n; i++) {
            if (slot_state->in_group[i]) moving.push_back(slot_state->army[i]);
        }
    }
    int icon = kNone;
    if (!moving.empty()) {
        bool sea = true;
        bool woods = false;
        bool hills = false;
        for (Army* army : moving) {
            const ArmyType* army_type = ctx.ActiveGame().ArmyTypes().ById(army->Type());
            if (army_type->woods_move) woods = true;
            if (army_type->hills_move) hills = true;
            if (!army->IsAtSea()) sea = false;
        }
        if (move::ModeOf(ctx.ActiveGame(), moving) == move::kFlying) {
            icon = 184;
        } else if (sea) {
            icon = 424;
        } else if (woods && hills) {
            icon = 216;
        } else if (woods) {
            icon = 248;
        } else if (hills) {
            icon = 152;
        }
    }
    if (icon != kNone) {
        DrawAbits(icon, 30, 32, 10, 344, 407);
    } else {
        kit::SetPal(ctx, 3);
        ren.Rectangle(gfx::kFill, 344, 407, 32, 10);
    }
}

void MapScreenController::DrawStatus() {
    const Font& big_font = Ctx().Assets().BigFont();
    for (int cell = 0; cell < 4; cell++) {
        const auto& status_cell = kStatusCells[static_cast<size_t>(cell)];
        DrawAbits(status_cell.sx, status_cell.sy, 40, 20, status_cell.x, kStatusY);
        big_font.Draw(Format(status_cell.fmt, StatusValue(cell)), status_cell.tx, kStatusY);
    }
}

void MapScreenController::DrawComputerStatus(const AiStatus& status) {
    UiContext& ctx = Ctx();
    gfx::Renderer& ren = ctx.Renderer();
    Side* side = status.side;
    ren.SetColor(1, 1, 1);
    kit::Centred(ctx.Assets().BigFont().Colours(side->Colour(), side->Edge()), status.text, 196,
                 415);
    kit::SetPal(ctx, 0);
    kit::Outline(ctx, 36, 442, 320, 22);
    int bar_width = status.progress / 5 * 16;
    if (bar_width > 0) {
        gfx::ImageP bar = ctx.Assets().MoveBar(ren, side->Index());
        ren.SetColor(1, 1, 1);
        if (bar) {
            ren.Draw(bar, gfx::NewQuad(0, 0, std::min(bar_width, 320), 18), 32, 444);
        }
    }
}

void MapScreenController::DrawBottomBar() {
    UiContext& ctx = Ctx();
    ctx.Renderer().SetColor(1, 1, 1);
    ctx.Renderer().Draw(ctx.Assets().Marble(), gfx::NewQuad(0, 60, 360, 66), 16, 403);
    if (const AiStatus* ai_status = state_->ActiveAiStatus()) {
        DrawComputerStatus(*ai_status);
    } else if (ctx.ActiveSelection()) {
        DrawArmySlots();
    } else {
        DrawStatus();
    }
}

void MapScreenController::BarInfo(int x, int y) {
    UiContext& ctx = Ctx();
    if (state_->ActiveAiStatus()) return;
    int bar_x = x - ctx.GetViewport().GetLayout().bar.x;
    if (const Selection* sel = ctx.ActiveSelection()) {
        if (bar_x < 336) {
            int slot = (bar_x - 16) / 40;
            if (bar_x >= 16 && slot < sel->slots.n) {
                infobox::Army(ctx, x, y, sel->slots.army[slot]);
            } else {
                infobox::ShowLines(ctx, x, y, "Select Army", "Select armies when present");
            }
        } else {
            infobox::ShowLines(ctx, x, y, "Group/Ungroup", "Manipulate all armies");
        }
        return;
    }
    constexpr std::array<std::array<const char*, 2>, 4> kHelp = {{
        {"Number of Cities", "You have %d cities!"},
        {"Your Treasury", "You have %d gold!"},
        {"Your Income", "You earn %d gold!"},
        {"Your Upkeep", "You pay %d gold!"},
    }};
    int cell = (bar_x - 16) / 90;
    if (bar_x >= 16 && cell >= 0 && cell < 4) {
        const auto& entry = kHelp[static_cast<size_t>(cell)];
        infobox::ShowLines(ctx, x, y, entry[0], Format(entry[1], StatusValue(cell)));
    }
}

int MapScreenController::PointerKind(int x, int y) const {
    const UiContext& ctx = Ctx();
    if (ctx.Starting() || state_->IsGameOver() || state_->IsAssaulting() ||
        state_->IsComputerTurn() || open_menu_ >= 0 || !ctx.Modals().Empty() || !ctx.Player() ||
        ctx.Player()->IsComputerControlled()) {
        return static_cast<int>(PointerCursor::kArrow);
    }
    if (state_->IsDragging()) return static_cast<int>(PointerCursor::kHand);
    bool alt = Held({"lalt", "ralt"});
    bool ctrl = Held({"lctrl", "rctrl"});
    const Viewport& viewport = ctx.GetViewport();
    if (viewport.StratRect().Contains(x, y)) {
        return static_cast<int>(alt ? PointerCursor::kAlt : PointerCursor::kView);
    }
    int ui_scale = ctx.Display().GetState().scale;
    auto tile = viewport.TileAtPoint(x, y, ui_scale);
    if (!tile) {
        return static_cast<int>(viewport.PanelRect().Contains(x, y) ? PointerCursor::kHand
                                                                    : PointerCursor::kArrow);
    }
    auto [tile_x, tile_y] = *tile;
    Game& game_state = ctx.ActiveGame();
    const Map& map = game_state.GetMap();
    int player_index = ctx.Player()->Index();
    if (!map.InBounds(tile_x, tile_y)) return static_cast<int>(PointerCursor::kHand);

    auto here = game::ArmiesAt(game_state, tile_x, tile_y);
    City* city = game::CityAt(game_state, tile_x, tile_y);
    bool armies = !here.empty();
    int owner = armies ? here[0]->OwnerIndex() : kNone;
    if (owner == kNone) owner = city ? city->OwnerIndex() : kNone;
    if (owner == kNone) owner = rules::kNeutral;
    int terrain = city ? (city->IsRazed() ? move::kSite : move::kCity)
                       : scn::TerrainAt(map, tile_x, tile_y);
    const Selection* active_sel = ctx.ActiveSelection();
    const ArmyList* sel = (active_sel && !active_sel->stack.empty()) ? &active_sel->stack : nullptr;
    int dist = 0;
    int mode = kNone;
    int under = move::kPlain;
    bool at_sea = false;
    if (sel) {
        int stack_x = (*sel)[0]->X();
        int stack_y = (*sel)[0]->Y();
        if (city && !city->IsRazed()) {
            int dx = std::max(0, std::max(city->X() - stack_x, stack_x - (city->X() + 1)));
            int dy = std::max(0, std::max(city->Y() - stack_y, stack_y - (city->Y() + 1)));
            dist = std::max(dx, dy);
        } else {
            dist = std::max(std::abs(tile_x - stack_x), std::abs(tile_y - stack_y));
        }
        mode = move::ModeOf(game_state, *sel);
        under = scn::TerrainAt(map, stack_x, stack_y);
        at_sea = (*sel)[0]->IsAtSea();
    }
    bool road = scn::RoadAt(map, tile_x, tile_y) % 0x20 != 0;
    int cost = (sel && mode != move::kFlying && !road) ? move::kCost[terrain] : 1;
    if (!game::Seen(game_state, player_index, tile_x, tile_y) || cost <= 0) {
        return static_cast<int>(PointerCursor::kHand);
    }
    auto walk_cursor = [&]() {
        return static_cast<int>(
                (mode != move::kFlying && (terrain == move::kWater || terrain == move::kShore))
                        ? PointerCursor::kBoat
                        : PointerCursor::kWalk);
    };
    auto look_cursor = [&]() {
        if (terrain == move::kCity) return static_cast<int>(PointerCursor::kCity);
        if (terrain == move::kSite) return static_cast<int>(PointerCursor::kSite);
        return static_cast<int>(PointerCursor::kHand);
    };
    auto is_ptr_ok = [](int ter) {
        return ter == move::kRoad || ter == move::kBridge || ter == move::kWater ||
               ter == move::kShore || ter == move::kCity;
    };
    bool enemy_near =
            sel && dist <= 1 && owner != player_index && (terrain == move::kCity || armies);
    if (Held({"lshift", "rshift"})) {
        if (enemy_near && map.GameOptions().military_advisor != 0) {
            return static_cast<int>(PointerCursor::kAdvise);
        }
        return look_cursor();
    }
    if (armies && owner == player_index && !ctrl) {
        if (alt) return static_cast<int>(PointerCursor::kAlt);
        if (!sel || dist < 1) return static_cast<int>(PointerCursor::kSelect);
        return walk_cursor();
    }
    if (enemy_near &&
        !(at_sea && mode == move::kLand && under == move::kShore && !is_ptr_ok(terrain)) &&
        !(!at_sea && mode == move::kLand && terrain == move::kShore && !is_ptr_ok(under))) {
        if (map.GameOptions().diplomacy == 0 || owner == rules::kNeutral) {
            return static_cast<int>(PointerCursor::kAttack);
        }
        int relation = diplomacy::State(game_state, player_index, owner);
        if (terrain == move::kCity) {
            return static_cast<int>(relation == diplomacy::kWar ? PointerCursor::kAttack
                                                                : PointerCursor::kPeace);
        }
        return static_cast<int>(relation != diplomacy::kPeace ? PointerCursor::kAttack
                                                              : PointerCursor::kPeace);
    }
    if (sel && (owner == player_index || (owner == rules::kNeutral && terrain != move::kCity))) {
        if (alt) return static_cast<int>(PointerCursor::kAlt);
        if (dist >= 1 && ctrl && armies) return static_cast<int>(PointerCursor::kSelect);
        return walk_cursor();
    }
    return look_cursor();
}

void MapScreenController::DrawPointer() {
    UiContext& ctx = Ctx();
    const display::DisplayWindow& disp = ctx.Display();
    if (disp.PointerAway()) return;
    const auto& disp_state = disp.GetState();
    int kind = disp_state.off ? static_cast<int>(PointerCursor::kArrow)
                              : PointerKind(mouse_x_, mouse_y_);
    auto [x, y] = disp_state.off ? *disp_state.off : std::make_pair(mouse_x_, mouse_y_);
    ctx.Renderer().SetColor(1, 1, 1);
    ctx.Renderer().Draw(ctx.Assets().PointerImg(), gfx::NewQuad(kind * 16, 0, 16, 16),
                        x - kPtrHot[static_cast<size_t>(kind)],
                        y - kPtrHot[static_cast<size_t>(kind)]);
}

}  // namespace w2
