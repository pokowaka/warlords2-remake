// History > City, Events, Gold, Winners (6d51:0000 with 0-3), and Triumphs.
//
// Plays back what warlords/history recorded, a round at a time. With
// nothing on record yet it says so (group 93). Popup 10, (32, 60) 576x312,
// dialog 19 (6d51:0096): the strategic map with the city shields as they
// stood that turn, the title, the four tabs (352-355); for City, Gold and
// Winners a graph of every side, one step a turn, scaled to the largest on
// record (6d51:034a); for Events that round's deeds and a timeline
// (6d51:16ba, 18da). The arrows (357/358) step a turn; a click on the graph
// or the timeline picks the turn under it (6d51:0908). Done (356).
#include <cmath>

#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/history.hpp"

namespace historyui {

namespace history = w2::history;
using w2::DeedType;
using w2::format;

namespace {
const kit::Rect kR{32, 60, 576, 312};  // popup 10
const int kDialog = 19, kTab = 352, kDone = 356, kPrev = 357, kNext = 358;
const int kGx = 300, kGy = 150, kGw = 292, kGh = 140;  // 4125:0dc6
const kit::Rect kTl{275, 322, 314, 11};                // 4125:0dd6
const int kCity = 0, kEvents = 1, kGold = 2, kWinners = 3;

int FloorOf(int mode) {
    return mode == kCity ? 10 : mode == kGold ? 500 : 100;
}
const std::array<int, 8>& Values(const w2::HistoryRecord& r, int mode) {
    if (mode == kCity) return r.cities;
    if (mode == kGold) return r.gold;
    return r.score;
}
void Hline(int x, int y, int n) {
    gfx::rectangle(gfx::FILL, x, y, n, 1);
}
void Vline(int x, int y, int n) {
    gfx::rectangle(gfx::FILL, x, y, 1, n);
}

void Shield(int s, int x, int y) {
    if (s < 0 || s > 7) return;
    gfx::setColor(1, 1, 1);
    gfx::draw(G.atransShields, gfx::newQuad(112 + 16 * (s / 4), 94 + 14 * (s % 4), 16, 14), x, y);
}

std::string EventText(const w2::Deed& e) {
    auto t = [](int i) { return kit::text(0x5e, i); };
    auto side_name = [](int i) {
        return i >= 0 && i < 8 ? G.g->map->sides[i].name : std::string();
    };
    auto city_name = [](int i) {
        w2::City* c = G.g->map->city(i);
        return c ? c->name : std::string();
    };
    int v1 = e.v1, v2 = e.v2;
    // the lines are group 94 of STRING.DAT, in the order below
    switch (e.type) {
    case DeedType::HERO_EMERGES:
        return format(t(0), e.name, city_name(v1));
    case DeedType::HERO_KILLED:
        if (v1 == history::KILLED_IN_BATTLE) return format(t(1), e.name);
        if (v1 == history::KILLED_SEARCHING) return format(t(2), e.name);
        return format(t(3), e.name, city_name(v1));
    case DeedType::QUEST_COMPLETED:
        return format(t(4), e.name);
    case DeedType::QUEST_RECEIVED:
        return format(t(5), e.name);
    case DeedType::SIDE_VANQUISHED:
        return format(t(6), side_name(v1));
    case DeedType::CITY_WON:
        if (v1 == history::WON_BY_NAME) return format(t(7), e.name, city_name(v2));
        return format(t(8), side_name(v1), city_name(v2));
    case DeedType::FOUND: {
        std::string what;
        if (v1 == history::FOUND_ALLIES)
            what = t(13);
        else if (v1 == history::FOUND_SAGE)
            what = t(14);
        else if (v1 == history::FOUND_GOLD)
            what = t(15);
        else
            for (auto& it : G.g->map->items)
                if (it.index == v1) what = it.name;
        return format(t(9), e.name, what);
    }
    case DeedType::SIDE_VICTORIOUS:
        return format(t(10), side_name(v1));
    case DeedType::AT_WAR:
        return format(t(11), side_name(v1));
    case DeedType::AT_PEACE:
        return format(t(12), side_name(v1));
    case DeedType::TREACHERY:
        return format(t(16), side_name(v1));
    }
    return "";
}

struct History : kit::Modal {
    screen::View v;
    int mode = 0, turn = 1, n = 1;
    const w2::HistoryRecord& Rec(int t) { return G.g->history[t - 1]; }
    void Refresh() {
        auto& st = v.state;
        for (int i = 0; i <= 3; i++)
            st[kTab + i] = i == mode ? w2::uidata::ACTIVE : w2::uidata::NORMAL;
        st[kPrev] = turn == 1 ? w2::uidata::DISABLED : w2::uidata::NORMAL;
        st[kNext] = turn == n ? w2::uidata::DISABLED : w2::uidata::NORMAL;
        st[kDone] = w2::uidata::NORMAL;
    }
    int Tx(int t) const { return kGx + t * kGw / n; }
    void DrawGraph() {
        int top = FloorOf(mode);
        for (int t = 1; t <= n; t++)
            for (int s = 0; s < 8; s++) top = std::max(top, Values(Rec(t), mode)[s]);
        auto axes = [](int c, int o) {
            kit::setPal(c);
            Vline(kGx - o, kGy - o, kGh);
            Hline(kGx - o, kGy + kGh - 1 - o, kGw);
            for (int y : {kGy, kGy + kGh / 2, kGy + kGh - 1}) Hline(kGx - 4 - o, y - o, 4);
            for (int x : {kGx, kGx + kGw / 2, kGx + kGw - 1}) Vline(x - o, kGy + kGh - 1 - o, 4);
        };
        axes(0, 1);
        axes(15, 0);
        const Font& f = kit::font(2);
        gfx::setColor(1, 1, 1);
        kit::right(f, w2::fmt("%d", top), kGx - 8, kGy - 6);
        kit::right(f, "0", kGx - 8, kGy + kGh - 8);
        kit::centred(f, "0", kGx, kGy + kGh + 8);
        kit::right(f, w2::fmt("%d", n), kGx + kGw + 8, kGy + kGh + 8);
        kit::centred(f, "Turns", kGx + kGw / 2, kGy + kGh + 8);
        int px[8], py[8];
        for (int s = 0; s < 8; s++) {
            px[s] = kGx + 1;
            py[s] = kGy + kGh - 2;
        }
        for (int t = 1; t <= n; t++) {
            int x = Tx(t);
            const auto& val = Values(Rec(t), mode);
            for (int s = 0; s < 8; s++) {
                int y = kGy + kGh - 1 -
                        static_cast<int>(std::floor(static_cast<double>(val[s]) / top * kGh));
                kit::line(G.g->map->sides[s].colour, px[s], py[s], x, y);
                px[s] = x;
                py[s] = y;
            }
        }
        int x = Tx(turn);
        kit::line(13, x, kGy - 2, x, kGy + kGh + 6);
        const auto& r = Rec(turn);
        std::string text;
        if (mode == kWinners) {
            int best = 0, lead = 0;
            for (int s = 0; s < 8; s++)
                if (r.score[s] > best) {
                    best = r.score[s];
                    lead = s;
                }
            text = format(kit::text(0x5c, 3), turn, G.g->map->sides[lead].name);
        } else {
            text = format(kit::text(0x5c, mode), turn, Values(r, mode)[G.player->index]);
        }
        gfx::setColor(1, 1, 1);
        kit::centred(f, text, 432, 318);
    }
    void DrawEvents() {
        const Font& f = kit::font(2);
        const auto& events = Rec(turn).events;
        for (size_t i = 0; i < events.size(); i++) {
            const w2::Deed& e = events[i];
            int y = 149 + 17 * static_cast<int>(i);
            Shield(e.side, 264, y + 1);
            std::string text = EventText(e);
            gfx::setColor(1, 1, 1);
            f.draw(text, 280, y);
            if (e.type == DeedType::AT_WAR || e.type == DeedType::AT_PEACE ||
                e.type == DeedType::TREACHERY)
                Shield(e.v2, 280 + f.width(text) / 8 * 8 + 16, y + 1);
        }
        kit::bevel(272, 319, 320, 17, 4, 2);
        kit::bevel(273, 320, 318, 15, 4, 2);
        kit::setPal(0);
        kit::outline(274, 321, 316, 13);
        int w = std::min(kTl.w, turn * kTl.w / n);
        kit::setPal(G.player->colour);
        gfx::rectangle(gfx::FILL, kTl.x, kTl.y, w, kTl.h);
        if (w < kTl.w - 8) {
            kit::setPal(G.player->edge);
            gfx::rectangle(gfx::FILL, kTl.x + w, kTl.y, kTl.w - w, kTl.h);
        }
    }
    void draw() override {
        kit::popup(kR);
        front::drawStrategicMap(kR.x, kR.y, nullptr, false, &Rec(turn).owners);
        gfx::setColor(1, 1, 1);
        kit::centred(kit::font(1), kit::text(0x5b, mode), 432, 64);
        if (mode == kEvents)
            DrawEvents();
        else
            DrawGraph();
        gfx::setColor(1, 1, 1);
        kit::centred(kit::font(2), w2::fmt("Turn %d", turn), 312, 350);
        kit::drawControls(v);
    }
    void Pick(int x, int x0, int w) {
        int t = static_cast<int>(std::floor(static_cast<double>(x - x0) / w * n)) + 1;
        turn = std::max(1, std::min(n, t));
    }
    void mousepressed(int x, int y, int) override {
        if (mode != kEvents && x >= kGx && x < kGx + kGw && y >= kGy && y < kGy + 160) {
            Pick(x, kGx, kGw);
            Refresh();
            return;
        }
        if (mode == kEvents && kTl.contains(x, y)) {
            Pick(x, kTl.x, kTl.w);
            Refresh();
            return;
        }
        auto c = kit::controlAt(v, x, y);
        if (!c || v.stateOf(c->id) == w2::uidata::DISABLED) return;
        if (c->id == kDone) {
            kit::pop(this);
            return;
        }
        if (c->id == kPrev)
            turn--;
        else if (c->id == kNext)
            turn++;
        else if (c->id >= kTab && c->id < kTab + 4)
            mode = c->id - kTab;
        Refresh();
    }
    void keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") kit::pop(this);
    }
};

// History > Triumphs (6d51:09eb): what the side has done to each other side,
// or lost itself. Popup 0, dialog 20 (6d51:0a21): eight tabs of BUTTON.PCK
// with the sides' shields on them (360-367), then five kinds of army with
// their counts (groups 81-85 for your own losses, 86-90 for the others').
const kit::Rect kTri{80, 60, 480, 320};  // popup 0
const int kTriDialog = 20, kTriDone = 359, kTriTab = 360;
const int kKinds[5] = {4, 25, 28, 5, 29};

struct Triumphs : kit::Modal {
    screen::View v;
    int opp = 0;
    void Refresh() {
        for (int i = 0; i < 8; i++)
            v.state[kTriTab + i] = i == opp ? w2::uidata::ACTIVE : w2::uidata::NORMAL;
        v.state[kTriDone] = w2::uidata::NORMAL;
    }
    void draw() override {
        int me = G.player->index;
        kit::popup(kTri);
        gfx::setColor(1, 1, 1);
        kit::centred(kit::font(1), kit::text(0x50, 0), 320, 64);
        auto button = G.screen->artFor(4);
        auto bshield = G.screen->artFor(38);
        for (int i = 0; i < 8; i++) {
            int x = 128 + 48 * i;
            gfx::setColor(1, 1, 1);
            if (button)
                gfx::draw(button->image, gfx::newQuad(400, i == opp ? 40 : 0, 48, 40), x, 109);
            if (bshield) gfx::draw(bshield->image, gfx::newQuad(i * 32, 0, 32, 36), x + 8, 111);
        }
        const Font& f = kit::font(2);
        int base = opp == me ? 0x51 : 0x56;
        for (int k = 0; k <= 4; k++) {
            int y = 168 + 35 * k;
            kit::army(kKinds[k], opp, 104, y, 1);
            int cnt = history::triumph(*G.g, me, opp, static_cast<history::Loss>(k));
            if (cnt > 0) {
                gfx::setColor(1, 1, 1);
                f.draw(format(kit::text(base + k, cnt == 1 ? 0 : 1), cnt), 144, y + 9);
            }
        }
        std::set<int> h;
        for (int i = 0; i < 8; i++) h.insert(kTriTab + i);
        kit::drawControls(v, h);
    }
    void mousepressed(int x, int y, int) override {
        auto c = kit::controlAt(v, x, y);
        if (!c) return;
        if (c->id == kTriDone) {
            kit::pop(this);
            return;
        }
        if (c->id >= kTriTab && c->id < kTriTab + 8) {
            opp = c->id - kTriTab;
            Refresh();
        }
    }
    void keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") kit::pop(this);
    }
};
}  // namespace

void open(int which) {
    int n = std::min(static_cast<int>(G.g->history.size()), history::LAST_RECORDED_TURN);
    if (n < 1) {
        search::message(kit::text(0x5d, 0), kit::text(0x5d, 1));
        return;
    }
    // the turn shown counts from 1, as the original's does
    auto d = std::make_shared<History>();
    d->v = kit::view(kDialog);
    d->view = &d->v;
    d->mode = which;
    d->turn = n;
    d->n = n;
    d->Refresh();
    kit::push(d);
}

void triumphs() {
    auto d = std::make_shared<Triumphs>();
    d->v = kit::view(kTriDialog);
    d->view = &d->v;
    d->opp = G.player->index;
    d->Refresh();
    kit::push(d);
}

}  // namespace historyui
