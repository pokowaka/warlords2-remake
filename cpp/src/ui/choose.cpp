// The game's list chooser (796c:0000): pick one of a list of names.
//
// Popup 3, (96, 50) 200x200, with dialog 2: the title in font 2 centred on
// (196, 52), a colour-3 box sunk with a (4, 2) bevel, five names 20 apart,
// the chosen one in colour 15 and the rest in colour 2. Controls 114-118
// choose a row, 121/122 scroll a row and 119/120 five, hidden when the list
// fits; OK (123) hands the chosen entry over, Cancel (124) none.
#include <algorithm>

#include "ui/dialogs.hpp"

namespace choose {

namespace {
const kit::Rect kR{96, 50, 200, 200};     // popup 3
const kit::Rect kBox{106, 70, 180, 110};  // 4125:15a6
const int kDialog = 2;
const int kRow = 114, kPageUp = 119, kPageDown = 120, kUp = 121, kDown = 122, kOk = 123,
          kCancel = 124;
const int kRows = 5;

struct Dialog : kit::Modal {
    std::string title;
    std::vector<std::string> names;
    std::function<void(int)> after;
    screen::View v;
    int top_row = 0, cur = 0;

    int N() const { return static_cast<int>(names.size()); }

    void Refresh() {
        auto& st = v.state;
        for (int i = 0; i < kRows; i++) st[kRow + i] = w2::uidata::kNormal;
        st[kOk] = w2::uidata::kNormal;
        st[kCancel] = w2::uidata::kNormal;
        hidden.clear();
        if (N() > kRows) {
            int up = top_row > 0 ? w2::uidata::kNormal : w2::uidata::kDisabled;
            int down = top_row + kRows - 1 < N() - 1 ? w2::uidata::kNormal : w2::uidata::kDisabled;
            st[kPageUp] = up;
            st[kUp] = up;
            st[kPageDown] = down;
            st[kDown] = down;
        } else {
            for (int id : {kPageUp, kPageDown, kUp, kDown}) hidden.insert(id);
        }
    }

    int Chosen() const { return top_row + cur < N() ? top_row + cur : w2::kNone; }

    void Close(int entry) {
        auto a = after;
        kit::Pop(this);
        if (a) a(entry);
    }

    void Draw() override {
        kit::Popup(kR);
        const Font& f = kit::GetFont(2);
        gfx::SetColor(1, 1, 1);
        kit::Centred(f, title, 196, 52);
        kit::SetPal(3);
        gfx::Rectangle(gfx::kFill, kBox.x, kBox.y, kBox.w, kBox.h);
        kit::Bevel(kBox.x, kBox.y, kBox.w, kBox.h, 4, 2);
        gfx::SetColor(1, 1, 1);
        for (int i = 0; i < kRows; i++) {
            if (top_row + i < N())
                f.Colours(i == cur ? 15 : 2, 0).Draw(names[top_row + i], 112, 74 + 20 * i);
        }
        kit::DrawControls(v, hidden);
    }

    void Mousepressed(int x, int y, int) override {
        auto c = kit::ControlAt(v, x, y, hidden);
        if (!c) return;
        int id = c->id;
        if (id >= kRow && id < kRow + kRows) {
            if (top_row + id - kRow < N()) cur = id - kRow;
        } else if (id == kUp) {
            top_row--;
            cur = std::min(cur + 1, kRows - 1);
        } else if (id == kDown) {
            top_row++;
            cur = std::max(cur - 1, 0);
        } else if (id == kPageUp) {
            top_row -= std::min(kRows, top_row);
        } else if (id == kPageDown) {
            top_row += std::min(kRows, N() - (top_row + kRows - 1) - 1);
        } else if (id == kOk) {
            return Close(Chosen());
        } else if (id == kCancel) {
            return Close(w2::kNone);
        }
        Refresh();
    }

    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter")
            Close(Chosen());
        else if (key == "escape")
            Close(w2::kNone);
    }
};
}  // namespace

void Open(const std::string& title, const std::vector<std::string>& names, int start,
          std::function<void(int)> after) {
    auto d = std::make_shared<Dialog>();
    d->title = title;
    d->names = names;
    d->after = after;
    d->v = kit::View(kDialog);
    d->view = &d->v;
    d->cur = std::max(0, start);
    while (d->cur >= kRows) {
        d->cur--;
        d->top_row++;
    }
    d->Refresh();
    kit::Push(d);
}

}  // namespace choose
