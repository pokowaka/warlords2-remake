#include "ui/kit.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "util/util.hpp"
#include "warlords/game.hpp"

namespace kit {

void Modal::PopSelf() {
    ctx_.Modals().Pop(this);
}

ModalP Push(w2::UiContext& ctx, ModalP modal) {
    return ctx.Modals().Push(std::move(modal));
}

void Pop(w2::UiContext& ctx, const Modal* modal) {
    ctx.Modals().Pop(modal);
}

Modal* Top(const w2::UiContext& ctx) {
    return ctx.Modals().Top();
}

ModalP TopShared(const w2::UiContext& ctx) {
    return ctx.Modals().TopShared();
}

void SetPal(w2::UiContext& ctx, int index) {
    const auto& palette = ctx.Assets().Palette();
    const auto& colour =
            index >= 0 && index < static_cast<int>(palette.size()) ? palette[index] : palette[0];
    ctx.Renderer().SetColor(static_cast<float>(colour[0]), static_cast<float>(colour[1]),
                            static_cast<float>(colour[2]));
}

void Outline(w2::UiContext& ctx, double x, double y, double width, double height) {
    gfx::Renderer& ren = ctx.Renderer();
    ren.Rectangle(gfx::kFill, x, y, width, 1);
    ren.Rectangle(gfx::kFill, x, y + height - 1, width, 1);
    ren.Rectangle(gfx::kFill, x, y, 1, height);
    ren.Rectangle(gfx::kFill, x + width - 1, y, 1, height);
}

void PopupFrame(w2::UiContext& ctx, const Rect& rect) {
    gfx::Renderer& ren = ctx.Renderer();
    ren.SetColor(0, 0, 0);
    Outline(ctx, rect.x - 1, rect.y - 1, rect.w + 2, rect.h + 2);
    ren.Rectangle(gfx::kFill, rect.x + 1, rect.y + rect.h + 1, rect.w + 2, 2);
    ren.Rectangle(gfx::kFill, rect.x + rect.w + 1, rect.y + 1, 2, rect.h + 2);
}

void Popup(w2::UiContext& ctx, const Rect& rect) {
    PopupFrame(ctx, rect);
    gfx::Renderer& ren = ctx.Renderer();
    const gfx::ImageP& marble = ctx.Assets().Marble();
    ren.SetColor(1, 1, 1);
    ren.SetScissor(rect.x, rect.y, rect.w, rect.h);
    ren.Draw(marble, rect.x, rect.y);
    // Past the marble's 480 columns the blit reads on into the next row.
    int marble_w = marble->w;
    int marble_h = marble->h;
    if (rect.w > marble_w) {
        ren.Draw(marble, gfx::NewQuad(0, 1, rect.w - marble_w, std::min(rect.h, marble_h - 1)),
                 rect.x + marble_w, rect.y);
    }
    ren.SetScissor();
}

const Font& GetFont(const w2::UiContext& ctx, int n) {
    return ctx.Assets().GetFont(n);
}

void Centred(const Font& font, const std::string& text, double x, double y) {
    font.Draw(text, x - std::floor(font.Width(text) / 2.0), y);
}

void Right(const Font& font, const std::string& text, double x, double y) {
    font.Draw(text, x - font.Width(text), y);
}

void Bevel(w2::UiContext& ctx, double x, double y, double width, double height, int top_col,
           int bot_col) {
    gfx::Renderer& ren = ctx.Renderer();
    auto top_left = [&]() {
        SetPal(ctx, top_col);
        ren.Rectangle(gfx::kFill, x, y, width, 1);
        ren.Rectangle(gfx::kFill, x, y, 1, height);
    };
    auto bottom_right = [&]() {
        SetPal(ctx, bot_col);
        ren.Rectangle(gfx::kFill, x + width - 1, y, 1, height);
        ren.Rectangle(gfx::kFill, x, y + height - 1, width, 1);
    };
    if (bot_col < top_col) {
        bottom_right();
        top_left();
    } else {
        top_left();
        bottom_right();
    }
}

void Field(w2::UiContext& ctx, double x, double y, double width, double height,
           const std::string* text, const Font* font) {
    SetPal(ctx, 3);
    ctx.Renderer().Rectangle(gfx::kFill, x, y, width, height);
    Bevel(ctx, x, y, width, height, 4, 2);
    ctx.Renderer().SetColor(1, 1, 1);
    if (text) (font ? *font : ctx.Assets().BigFont()).Draw(*text, x + 3, y + 2);
}

gfx::Quad ArmyQuad(int type_id) {
    int cell = ((type_id % 32) + 32) % 32;
    // An army sheet is 16 cells across on a 32-pixel stride; its rows are 30
    // apart and 29 tall.
    return gfx::NewQuad((cell % 16) * 32, (cell / 16) * 30, 32, 29);
}

void Army(w2::UiContext& ctx, int type_id, int side, double x, double y, int ring, bool shadow) {
    gfx::Renderer& ren = ctx.Renderer();
    ren.SetColor(1, 1, 1);
    if (ring > 0) ren.Draw(ctx.Assets().Abits(), gfx::NewQuad((ring - 1) * 32, 0, 32, 30), x, y);
    if (type_id != w2::kNone) {
        ren.Draw(shadow ? ctx.Assets().ShadowImg() : ctx.Assets().ArmyImg(side), ArmyQuad(type_id),
                 x, y);
    }
}

void Shield(w2::UiContext& ctx, int side, double x, double y) {
    if (side < 0 || side == 15 || side > 8) side = 8;
    const gfx::ImageP& shields = ctx.Assets().ShieldsImg();
    if (!shields) return;
    ctx.Renderer().SetColor(1, 1, 1);
    ctx.Renderer().Draw(shields, gfx::NewQuad(side * 40, 0, 40, 40), x, y);
}

void Line(w2::UiContext& ctx, int colour_index, int from_x, int from_y, int to_x, int to_y) {
    SetPal(ctx, colour_index);
    gfx::Renderer& ren = ctx.Renderer();
    int dx = std::abs(to_x - from_x);
    int dy = -std::abs(to_y - from_y);
    int step_x = from_x < to_x ? 1 : -1;
    int step_y = from_y < to_y ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        ren.Rectangle(gfx::kFill, from_x, from_y, 1, 1);
        if (from_x == to_x && from_y == to_y) break;
        int err2 = 2 * err;
        if (err2 >= dy) {
            err += dy;
            from_x += step_x;
        }
        if (err2 <= dx) {
            err += dx;
            from_y += step_y;
        }
    }
}

void MapTarget(w2::UiContext& ctx, int map_x, int map_y, int hero_x, int hero_y, int target_x,
               int target_y) {
    const w2::Game& game = ctx.ActiveGame();
    int viewer = w2::game::ViewerSide(game, ctx.Player());
    auto seen = [&](int x, int y) { return w2::game::Seen(game, viewer, x, y); };
    if (!seen(target_x, target_y)) return;
    gfx::Renderer& ren = ctx.Renderer();
    ren.SetScissor(map_x, map_y, 224, 312);
    if (seen(hero_x, hero_y)) {
        Line(ctx, 8, map_x + hero_x * 2 - 2, map_y + hero_y * 2 - 2, map_x + target_x * 2 - 2,
             map_y + target_y * 2 - 2);
    }
    SetPal(ctx, 8);
    int box_x = map_x + target_x * 2;
    int box_y = map_y + target_y * 2 - 1;
    ren.Rectangle(gfx::kFill, box_x, box_y, 4, 5);
    SetPal(ctx, 0);
    ren.Rectangle(gfx::kFill, box_x - 1, box_y, 6, 1);
    ren.Rectangle(gfx::kFill, box_x, box_y + 5, 4, 1);
    ren.Rectangle(gfx::kFill, box_x - 1, box_y, 1, 5);
    ren.Rectangle(gfx::kFill, box_x + 4, box_y, 1, 5);
    SetPal(ctx, 8);
    Outline(ctx, map_x + target_x * 2 - 2, map_y + target_y * 2 - 2, 8, 8);
    ren.SetScissor();
}

screen::View View(w2::UiContext& ctx, int dialog_id) {
    return screen::MakeView(ctx.Assets().MainScreen(), dialog_id);
}

void DrawControls(w2::UiContext& ctx, const screen::View& view, const ControlIdSet* hidden) {
    screen::DrawDialogControls(ctx.Renderer(), ctx.Assets().MainScreen(), view, hidden);
}

const w2::uidata::Control* ControlAt(const screen::View& view, int x, int y,
                                     const ControlIdSet* hidden) {
    for (const auto& control : view.dialog.controls) {
        if (control.w > 0 && control.h > 0 && !(hidden && hidden->count(control.id)) &&
            x >= control.x && x < control.x + control.w && y >= control.y &&
            y < control.y + control.h) {
            if (view.StateOf(control.id) == w2::uidata::kDisabled) return nullptr;
            return &control;
        }
    }
    return nullptr;
}

const w2::uidata::Control* FindControl(const screen::View& view, int control_id) {
    return screen::DialogControl(view, control_id);
}

std::string Text(const w2::UiContext& ctx, int group, int index) {
    return w2::uidata::Text(ctx.Assets().MainScreen().ui, group, index);
}

double Now() {
    return w2::Now();
}

}  // namespace kit
