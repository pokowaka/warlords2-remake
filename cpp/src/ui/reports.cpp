// The Report menu: Army, City, Gold, Production and Winning.
//
// Dialog 7 over popup 2, (80, 60) 480x312, the five reports as tabs (218-222)
// and Done (223); auto_ui_reports_menu (6ef3:0030) draws the strategic map
// -- a banner on every stack outside a city for Army (834b:158d), See All's
// vectors for Production -- the title, what it measures, a bar per side or
// the list of what was built this turn, and the summary in the side's
// colours. The bars (6ef3:05ff) are tiled 8 pixels at a time from
// SHIELDS.PCK; the Production list (6f8c:086e) shows five at a time, and
// 206-209 scroll it.
#include <set>

#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/report.hpp"

namespace reports {

namespace report = w2::report;
using w2::Format;

namespace {
const kit::Rect kR{80, 60, 480, 312};  // popup 2
const int kMapX = 80, kMapY = 60;
const int kDialog = 7;
const int kTabFirst = 218, kDone = 223;
const int kUp = 206, kDown = 207, kPageUp = 208, kPageDown = 209;
const int kSTitle = 0x49, kSWhat = 0x4a;
const int kSSummary[5] = {0x4b, 0x4c, 0x4d, 0x4e, 0x4f};
const int kRows = 5;

std::string Summary(int n, const report::Figures& f) {
    int r = f.result;
    if (n == report::kArmy || n == report::kCity)
        return r == 1 ? kit::Text(kSSummary[n], 0) : Format(kit::Text(kSSummary[n], 1), r);
    if (n == report::kGold) return Format(kit::Text(kSSummary[n], 0), r);
    if (n == report::kProduction) return Format(kit::Text(kSSummary[n], r == 1 ? 1 : 0), r);
    return kit::Text(kSSummary[n], r);
}

void DrawArmyBanners() {
    std::set<int> done;
    gfx::SetScissor(kMapX, kMapY, 224, 312);
    gfx::SetColor(1, 1, 1);
    int viewer = w2::game::ViewerSide(*G.g, G.player);
    for (w2::Army* a : G.g->armies) {
        if (!a->transit && a->x != w2::kNone) {
            int k = a->y * 1000 + a->x;
            if (!done.count(k) && w2::game::Seen(*G.g, viewer, a->x, a->y) &&
                !w2::game::CityAt(*G.g, a->x, a->y)) {
                done.insert(k);
                int side = a->owner == w2::kNone ? 8 : a->owner;
                gfx::Draw(G.atrans_shields, gfx::NewQuad(side * 16, 164, 16, 10),
                          kMapX + std::max(0, a->x * 2 - 1), kMapY + std::max(0, a->y * 2 - 1));
            }
        }
    }
    gfx::SetScissor();
}

void DrawBars(const report::Figures& f) {
    for (int i = 0; i < 8; i++) {
        if (!f.out[i]) {
            int y = 200 + 14 * i;
            int w = 1;
            if (f.max > 0) w = std::max(0, f.value[i]) * 240 / f.max;
            w = std::max(1, w);
            gfx::SetColor(1, 1, 1);
            int sx = 320 + 8 * (i % 4), sy = 40 + 8 * (i / 4);
            for (int x = 0; x < w; x += 8) {
                int piece = std::min(8, w - x);
                gfx::Draw(G.shields_img, gfx::NewQuad(sx, sy, piece, 8), 312 + x, y);
            }
            kit::Bevel(311, y - 1, w + 2, 10, 0, 1);
        }
    }
    auto axis = [](int colour, int d) {
        kit::SetPal(colour);
        gfx::Rectangle(gfx::kFill, 312 - d, 196 - d, 240, 1);
        const int tx[5] = {312, 372, 432, 492, 552};
        for (int k = 0; k < 5; k++) {
            bool lng = k % 2 == 0;
            gfx::Rectangle(gfx::kFill, tx[k] - d, (lng ? 190 : 192) - d, 1, lng ? 6 : 4);
        }
    };
    axis(1, 0);
    axis(0, 1);
    const Font& font = kit::GetFont(2);
    gfx::SetColor(1, 1, 1);
    kit::Right(font, w2::Fmt("%d", f.max), 556, 172);
    kit::Centred(font, w2::Fmt("%d", f.max / 2), 432, 172);
    kit::Centred(font, "0", 308, 172);
}

struct Reports : kit::Modal {
    screen::View v;
    int n = 0, top_row = 0;
    void Refresh() {
        auto& st = v.state;
        for (int i = 0; i <= 4; i++)
            st[kTabFirst + i] = i == n ? w2::uidata::kActive : w2::uidata::kNormal;
        st[kDone] = w2::uidata::kNormal;
        hidden.clear();
        if (n == report::kProduction) {
            int count = static_cast<int>(G.player->produced.size());
            st[kUp] = top_row > 0 ? w2::uidata::kNormal : w2::uidata::kDisabled;
            st[kPageUp] = st[kUp];
            st[kDown] = top_row + kRows < count ? w2::uidata::kNormal : w2::uidata::kDisabled;
            st[kPageDown] = st[kDown];
        } else {
            for (int id : {kUp, kDown, kPageUp, kPageDown}) hidden.insert(id);
        }
    }
    void Scroll(int by) {
        int count = static_cast<int>(G.player->produced.size());
        top_row = std::max(0, std::min(std::max(0, count - kRows), top_row + by));
        Refresh();
    }
    void DrawProduction() {
        const auto& list = G.player->produced;
        const Font& font = kit::GetFont(2);
        for (int i = 0; i < kRows; i++) {
            if (top_row + i >= static_cast<int>(list.size())) break;
            const w2::Produced& e = list[top_row + i];
            int y = 176 + 30 * i;
            gfx::SetColor(1, 1, 1);
            font.Draw(w2::Fmt("%d", top_row + i + 1), 312, y);
            kit::Army(e.type, G.player->index, 328, 170 + 30 * i, 1);
            w2::City* city = e.city != w2::kNone ? G.g->map->City(e.city) : nullptr;
            std::string name = e.standard ? "Standard" : (city ? city->name : "");
            std::string text;
            if (e.kind == "sent")
                text = name + " ...";
            else if (e.kind == "arrived")
                text = "... " + name;
            else
                text = name;
            gfx::SetColor(1, 1, 1);
            font.Draw(text, 368, y);
        }
    }
    void Draw() override {
        auto f = report::ComputeFigures(*G.g, *G.player, n);
        kit::Popup(kR);
        if (n == report::kProduction) {
            front::DrawVectorMap(kMapX, kMapY, nullptr, w2::kNone, true);
        } else {
            front::DrawStrategicMap(kMapX, kMapY);
            if (n == report::kArmy) DrawArmyBanners();
        }
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), kit::Text(kSTitle, n), 432, 62);
        kit::Centred(kit::GetFont(2), kit::Text(kSWhat, n), 432, 149);
        if (n == report::kProduction)
            DrawProduction();
        else
            DrawBars(f);
        kit::Centred(kit::GetFont(2).Colours(G.player->colour, G.player->edge), Summary(n, f), 432,
                     322);
        kit::DrawControls(v, hidden);
    }
    void Mousepressed(int x, int y, int) override {
        auto c = kit::ControlAt(v, x, y, hidden);
        if (!c) return;
        if (c->id == kDone)
            kit::Pop(this);
        else if (c->id >= kTabFirst && c->id < kTabFirst + 5) {
            n = c->id - kTabFirst;
            top_row = 0;
            Refresh();
        } else if (c->id == kUp)
            Scroll(-1);
        else if (c->id == kDown)
            Scroll(1);
        else if (c->id == kPageUp)
            Scroll(-kRows);
        else if (c->id == kPageDown)
            Scroll(kRows);
    }
    void Keypressed(const std::string& key) override {
        if (key == "escape" || key == "return" || key == "kpenter") kit::Pop(this);
    }
};
}  // namespace

void Open(int which) {
    auto d = std::make_shared<Reports>();
    d->v = kit::View(kDialog);
    d->view = &d->v;
    d->n = which;
    d->Refresh();
    kit::Push(d);
}

}  // namespace reports
