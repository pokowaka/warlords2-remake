#include "ui/kit.hpp"

#include <algorithm>
#include <cmath>

#include "util/util.hpp"
#include "warlords/game.hpp"

namespace kit {

ModalP Push(ModalP modal) {
    G.modals.push_back(modal);
    return modal;
}

void Pop(const Modal* modal) {
    for (int i = static_cast<int>(G.modals.size()) - 1; i >= 0; i--) {
        if (G.modals[i].get() == modal) {
            G.modals.erase(G.modals.begin() + i);
            return;
        }
    }
}

Modal* Top() {
    return G.modals.empty() ? nullptr : G.modals.back().get();
}

ModalP TopShared() {
    return G.modals.empty() ? nullptr : G.modals.back();
}

void SetPal(int index) {
    const auto& colour = index >= 0 && index < static_cast<int>(G.palette.size()) ? G.palette[index]
                                                                                  : G.palette[0];
    gfx::SetColor(static_cast<float>(colour[0]), static_cast<float>(colour[1]),
                  static_cast<float>(colour[2]));
}

void Outline(double x, double y, double width, double height) {
    gfx::Rectangle(gfx::kFill, x, y, width, 1);
    gfx::Rectangle(gfx::kFill, x, y + height - 1, width, 1);
    gfx::Rectangle(gfx::kFill, x, y, 1, height);
    gfx::Rectangle(gfx::kFill, x + width - 1, y, 1, height);
}

void PopupFrame(const Rect& rect) {
    gfx::SetColor(0, 0, 0);
    Outline(rect.x - 1, rect.y - 1, rect.w + 2, rect.h + 2);
    gfx::Rectangle(gfx::kFill, rect.x + 1, rect.y + rect.h + 1, rect.w + 2, 2);
    gfx::Rectangle(gfx::kFill, rect.x + rect.w + 1, rect.y + 1, 2, rect.h + 2);
}

void Popup(const Rect& rect) {
    PopupFrame(rect);
    gfx::SetColor(1, 1, 1);
    gfx::SetScissor(rect.x, rect.y, rect.w, rect.h);
    gfx::Draw(G.marble, rect.x, rect.y);
    // Past the marble's 480 columns the blit reads on into the next row.
    int marble_w = G.marble->w, marble_h = G.marble->h;
    if (rect.w > marble_w)
        gfx::Draw(G.marble, gfx::NewQuad(0, 1, rect.w - marble_w, std::min(rect.h, marble_h - 1)),
                  rect.x + marble_w, rect.y);
    gfx::SetScissor();
}

const Font& GetFont(int n) {
    if (n == 1) return *G.title_font;
    if (n == 2) return *G.big_font;
    return *G.font;
}

void Centred(const Font& font, const std::string& text, double x, double y) {
    font.Draw(text, x - std::floor(font.Width(text) / 2.0), y);
}

void Right(const Font& font, const std::string& text, double x, double y) {
    font.Draw(text, x - font.Width(text), y);
}

void Bevel(double x, double y, double width, double height, int top_col, int bot_col) {
    auto top_left = [&]() {
        SetPal(top_col);
        gfx::Rectangle(gfx::kFill, x, y, width, 1);
        gfx::Rectangle(gfx::kFill, x, y, 1, height);
    };
    auto bottom_right = [&]() {
        SetPal(bot_col);
        gfx::Rectangle(gfx::kFill, x + width - 1, y, 1, height);
        gfx::Rectangle(gfx::kFill, x, y + height - 1, width, 1);
    };
    if (bot_col < top_col) {
        bottom_right();
        top_left();
    } else {
        top_left();
        bottom_right();
    }
}

void Field(double x, double y, double width, double height, const std::string* text,
           const Font* font) {
    SetPal(3);
    gfx::Rectangle(gfx::kFill, x, y, width, height);
    Bevel(x, y, width, height, 4, 2);
    gfx::SetColor(1, 1, 1);
    if (text) (font ? *font : *G.big_font).Draw(*text, x + 3, y + 2);
}

gfx::Quad ArmyQuad(int type_id) {
    int cell = ((type_id % 32) + 32) % 32;
    // An army sheet is 16 cells across on a 32-pixel stride; its rows are 30
    // apart and 29 tall.
    return gfx::NewQuad((cell % 16) * 32, (cell / 16) * 30, 32, 29);
}

void Army(int type_id, int side, double x, double y, int ring, bool shadow) {
    if (side < 0 || side == 15 || side > 8) side = 8;
    gfx::SetColor(1, 1, 1);
    if (ring > 0) gfx::Draw(G.abits, gfx::NewQuad((ring - 1) * 32, 0, 32, 30), x, y);
    if (type_id != w2::kNone)
        gfx::Draw(shadow ? G.shadow_img : G.army_img[side], ArmyQuad(type_id), x, y);
}

void Shield(int side, double x, double y) {
    if (side < 0 || side == 15 || side > 8) side = 8;
    if (!G.shields_img) return;
    gfx::SetColor(1, 1, 1);
    gfx::Draw(G.shields_img, gfx::NewQuad(side * 40, 0, 40, 40), x, y);
}

void Line(int colour_index, int from_x, int from_y, int to_x, int to_y) {
    SetPal(colour_index);
    int dx = std::abs(to_x - from_x), dy = -std::abs(to_y - from_y);
    int step_x = from_x < to_x ? 1 : -1, step_y = from_y < to_y ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        gfx::Rectangle(gfx::kFill, from_x, from_y, 1, 1);
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

void MapTarget(int map_x, int map_y, int hero_x, int hero_y, int target_x, int target_y) {
    // With the hidden map nothing is drawn over ground the player has not
    // seen: the line needs both its ends seen, and the box and its frame need
    // the target's tile.
    auto seen = [](int x, int y) {
        return w2::game::Seen(*G.g, w2::game::ViewerSide(*G.g, G.player), x, y);
    };
    if (!seen(target_x, target_y)) return;
    gfx::SetScissor(map_x, map_y, 224, 312);
    if (seen(hero_x, hero_y))
        Line(8, map_x + hero_x * 2 - 2, map_y + hero_y * 2 - 2, map_x + target_x * 2 - 2,
             map_y + target_y * 2 - 2);
    SetPal(8);
    int box_x = map_x + target_x * 2, box_y = map_y + target_y * 2 - 1;
    gfx::Rectangle(gfx::kFill, box_x, box_y, 4, 5);
    SetPal(0);
    gfx::Rectangle(gfx::kFill, box_x - 1, box_y, 6, 1);
    gfx::Rectangle(gfx::kFill, box_x, box_y + 5, 4, 1);
    gfx::Rectangle(gfx::kFill, box_x - 1, box_y, 1, 5);
    gfx::Rectangle(gfx::kFill, box_x + 4, box_y, 1, 5);
    SetPal(8);
    Outline(map_x + target_x * 2 - 2, map_y + target_y * 2 - 2, 8, 8);
    gfx::SetScissor();
}

screen::View View(int dialog_id) {
    return screen::MakeView(*G.screen, dialog_id);
}

void DrawControls(const screen::View& view, const std::set<int>* hidden) {
    screen::DrawDialogControls(*G.screen, view, hidden);
}

const w2::uidata::Control* ControlAt(const screen::View& view, int x, int y,
                                     const std::set<int>* hidden) {
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

std::string Text(int group, int index) {
    return w2::uidata::Text(G.screen->ui, group, index);
}

double Now() {
    return w2::Now();
}

}  // namespace kit
