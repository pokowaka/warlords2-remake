#include "ui/kit.hpp"

#include <algorithm>
#include <cmath>

#include "util/util.hpp"
#include "warlords/game.hpp"

namespace kit {

ModalP Push(ModalP d) {
    G.modals.push_back(d);
    return d;
}

void Pop(const Modal* d) {
    for (int i = static_cast<int>(G.modals.size()) - 1; i >= 0; i--) {
        if (G.modals[i].get() == d) {
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

void SetPal(int i) {
    const auto& c = i >= 0 && i < static_cast<int>(G.palette.size()) ? G.palette[i] : G.palette[0];
    gfx::SetColor(static_cast<float>(c[0]), static_cast<float>(c[1]), static_cast<float>(c[2]));
}

void Outline(double x, double y, double w, double h) {
    gfx::Rectangle(gfx::kFill, x, y, w, 1);
    gfx::Rectangle(gfx::kFill, x, y + h - 1, w, 1);
    gfx::Rectangle(gfx::kFill, x, y, 1, h);
    gfx::Rectangle(gfx::kFill, x + w - 1, y, 1, h);
}

void PopupFrame(const Rect& r) {
    gfx::SetColor(0, 0, 0);
    Outline(r.x - 1, r.y - 1, r.w + 2, r.h + 2);
    gfx::Rectangle(gfx::kFill, r.x + 1, r.y + r.h + 1, r.w + 2, 2);
    gfx::Rectangle(gfx::kFill, r.x + r.w + 1, r.y + 1, 2, r.h + 2);
}

void Popup(const Rect& r) {
    PopupFrame(r);
    gfx::SetColor(1, 1, 1);
    gfx::SetScissor(r.x, r.y, r.w, r.h);
    gfx::Draw(G.marble, r.x, r.y);
    // past the marble's 480 columns the blit reads on into the next row
    int mw = G.marble->w, mh = G.marble->h;
    if (r.w > mw)
        gfx::Draw(G.marble, gfx::NewQuad(0, 1, r.w - mw, std::min(r.h, mh - 1)), r.x + mw, r.y);
    gfx::SetScissor();
}

const Font& GetFont(int n) {
    if (n == 1) return *G.title_font;
    if (n == 2) return *G.big_font;
    return *G.font;
}

void Centred(const Font& f, const std::string& text, double x, double y) {
    f.Draw(text, x - std::floor(f.Width(text) / 2.0), y);
}

void Right(const Font& f, const std::string& text, double x, double y) {
    f.Draw(text, x - f.Width(text), y);
}

void Bevel(double x, double y, double w, double h, int a, int b) {
    auto top_left = [&]() {
        SetPal(a);
        gfx::Rectangle(gfx::kFill, x, y, w, 1);
        gfx::Rectangle(gfx::kFill, x, y, 1, h);
    };
    auto bottom_right = [&]() {
        SetPal(b);
        gfx::Rectangle(gfx::kFill, x + w - 1, y, 1, h);
        gfx::Rectangle(gfx::kFill, x, y + h - 1, w, 1);
    };
    if (b < a) {
        bottom_right();
        top_left();
    } else {
        top_left();
        bottom_right();
    }
}

void Field(double x, double y, double w, double h, const std::string* text, const Font* f) {
    SetPal(3);
    gfx::Rectangle(gfx::kFill, x, y, w, h);
    Bevel(x, y, w, h, 4, 2);
    gfx::SetColor(1, 1, 1);
    if (text) (f ? *f : *G.big_font).Draw(*text, x + 3, y + 2);
}

gfx::Quad ArmyQuad(int type_id) {
    int i = ((type_id % 32) + 32) % 32;
    // An army sheet is 16 cells across on a 32-pixel stride; its rows are 30
    // apart and 29 tall, which is what 8611:08be reads out of it.
    return gfx::NewQuad((i % 16) * 32, (i / 16) * 30, 32, 29);
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

void Line(int c, int x0, int y0, int x1, int y1) {
    SetPal(c);
    int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        gfx::Rectangle(gfx::kFill, x0, y0, 1, 1);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void MapTarget(int mx, int my, int hx, int hy, int tx, int ty) {
    // with the hidden map nothing is drawn over ground the player has not
    // seen: the line needs both its ends seen (828e:0a3f), the box and its
    // frame the target's tile (828e:08cd, 828e:099e)
    auto seen = [](int x, int y) {
        return w2::game::Seen(*G.g, w2::game::ViewerSide(*G.g, G.player), x, y);
    };
    if (!seen(tx, ty)) return;
    gfx::SetScissor(mx, my, 224, 312);
    if (seen(hx, hy)) Line(8, mx + hx * 2 - 2, my + hy * 2 - 2, mx + tx * 2 - 2, my + ty * 2 - 2);
    SetPal(8);
    int bx = mx + tx * 2, by = my + ty * 2 - 1;
    gfx::Rectangle(gfx::kFill, bx, by, 4, 5);
    SetPal(0);
    gfx::Rectangle(gfx::kFill, bx - 1, by, 6, 1);
    gfx::Rectangle(gfx::kFill, bx, by + 5, 4, 1);
    gfx::Rectangle(gfx::kFill, bx - 1, by, 1, 5);
    gfx::Rectangle(gfx::kFill, bx + 4, by, 1, 5);
    SetPal(8);
    Outline(mx + tx * 2 - 2, my + ty * 2 - 2, 8, 8);
    gfx::SetScissor();
}

screen::View View(int dialog_id) {
    return screen::MakeView(*G.screen, dialog_id);
}

void DrawControls(const screen::View& v, const std::set<int>* hidden) {
    screen::DrawDialogControls(*G.screen, v, hidden);
}

const w2::uidata::Control* ControlAt(const screen::View& v, int x, int y,
                                     const std::set<int>* hidden) {
    for (const auto& c : v.dialog.controls) {
        if (c.w > 0 && c.h > 0 && !(hidden && hidden->count(c.id)) && x >= c.x && x < c.x + c.w &&
            y >= c.y && y < c.y + c.h) {
            if (v.StateOf(c.id) == w2::uidata::kDisabled) return nullptr;
            return &c;
        }
    }
    return nullptr;
}

const w2::uidata::Control* FindControl(const screen::View& v, int id) {
    return screen::DialogControl(v, id);
}

std::string Text(int group, int i) {
    return w2::uidata::Text(G.screen->ui, group, i);
}

double Now() {
    return w2::Now();
}

}  // namespace kit
