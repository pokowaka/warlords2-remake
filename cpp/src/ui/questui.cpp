// Report > Quest (auto_ui_no_quest, 4976:0167; the text, 4976:0320).
//
// Popup 2 with dialog 16 -- Done (330). "Quest" in font 1, and the strategic
// map with no city shields. With no quest, one of four lines of group 20.
// With one, SCROLL.PCK and on it, black on yellow, "%s's Quest", a black rule
// and the quest's own lines (groups 21-27). The map shows where to go: a
// line from the hero to the target with a little shield in a box -- or, for
// a quest to slay a kind of army or a side's armies, a banner on every such
// stack (834b:158d). The hero's figure is drawn over it all.
#include <set>

#include "ui/dialogs.hpp"
#include "util/util.hpp"

namespace questui {

using w2::Format;

namespace {
const kit::Rect kR{80, 60, 480, 312};  // popup 2
const int kMapX = 80, kMapY = 60;
const int kDialog = 16, kDone = 330;
const char* const kCompass[8] = {"north", "northeast", "east", "southeast",
                                 "south", "southwest", "west", "northwest"};

// 828e:0b51: the compass point from (x1, y1) to (x2, y2).
int Direction(int x1, int y1, int x2, int y2) {
    if (x1 == x2) return y1 < y2 ? 4 : 0;
    if (y1 == y2) return x1 < x2 ? 2 : 6;
    if (x2 < x1 && y2 < y1) return 7;
    if (x2 < x1 && y1 < y2) return 5;
    if (x1 < x2 && y2 < y1) return 1;
    if (x1 < x2 && y1 < y2) return 3;
    return 0;
}

// Where an item is: its carrier, the ruin it lies in, or the ground.
std::pair<int, int> ItemPlace(const w2::Game& g, const w2::Item* it) {
    if (it->status == 3) {
        for (w2::Army* a : g.armies)
            for (w2::Item* c : a->items)
                if (c == it && a->x != w2::kNone) return {a->x, a->y};
    } else if (it->status == 2) {
        for (auto& s : g.map->sites)
            if (s.item == it->index) return {s.x, s.y};
    }
    return {it->x, it->y};
}

struct QuestText {
    std::vector<std::pair<int, std::string>> lines;
    int tx = w2::kNone, ty = w2::kNone;
};

QuestText DescribeQuest(const w2::Game& g, const w2::Side& side, const w2::Quest& q) {
    auto t = [](int grp, int i) { return kit::Text(grp, i); };
    QuestText out;
    auto add = [&](int y, const std::string& s) { out.lines.emplace_back(y, s); };
    w2::Army* h = q.hero;
    // a city the side has not seen is told of as one only heard of: 4976:0850
    // tests (x,y) twice -- (x+1,y) never -- then (x+1,y+1) and (x,y+1)
    auto fabled = [&](const w2::City* c) {
        auto seen = [&](int x, int y) { return w2::game::Seen(g, side.index, x, y); };
        return g.map->options.hidden_map != 0 && !seen(c->x, c->y) && !seen(c->x + 1, c->y + 1) &&
               !seen(c->x, c->y + 1);
    };
    using namespace w2::quest;
    if (q.type == kSlayHero && q.army) {
        w2::Army* foe = q.army;
        add(175, t(0x15, 0));
        add(195, t(0x15, 1));
        const w2::Side* fs = g.map->Side(foe->owner);
        add(215, Format(t(0x15, 2), fs ? fs->name : ""));
        add(235, foe->name);
        add(265, t(0x15, 3));
        out.tx = foe->x;
        out.ty = foe->y;
        add(285, kCompass[Direction(h->x, h->y, out.tx, out.ty)]);
    } else if (q.type == kRetrieveItem && q.item) {
        add(175, t(0x16, 0));
        add(195, t(0x16, 1));
        add(215, q.item->name);
        add(245, t(0x16, 2));
        auto [x, y] = ItemPlace(g, q.item);
        out.tx = x;
        out.ty = y;
        if (x != w2::kNone) add(265, kCompass[Direction(h->x, h->y, x, y)]);
    } else if (q.type == kSlayType && q.army_type) {
        add(175, t(0x17, 0));
        add(195, t(0x17, 1));
        add(215, t(0x17, 2));
        add(235, q.army_type->name);
    } else if (q.type == kSlaughter && q.side) {
        add(175, t(0x18, 0));
        add(195, Format(t(0x18, 1), q.required));
        add(215, q.side->name);
        add(245, t(0x18, 2));
        add(265, Format(t(0x18, 3), q.done));
    } else if ((q.type == kOccupy || q.type == kRaze) && q.city) {
        int grp = q.type == kOccupy ? 0x19 : 0x1a;
        const w2::City* c = q.city;
        add(175, t(grp, 0));
        add(195, Format(t(grp, fabled(c) ? 2 : 1), c->name));
        add(215, t(grp, 3));
        add(235, t(grp, 4));
        add(265, t(grp, 5));
        out.tx = c->x;
        out.ty = c->y;
        add(285, kCompass[Direction(h->x, h->y, out.tx, out.ty)]);
    } else if (q.type == kPillageGold) {
        add(175, t(0x1b, 0));
        add(195, Format(t(0x1b, 1), q.required));
        add(215, t(0x1b, 2));
        add(235, t(0x1b, 3));
        add(265, t(0x1b, 4));
        add(285, Format(t(0x1b, 5), q.done));
    }
    return out;
}

struct QuestDialog : kit::Modal {
    screen::View v;
    std::shared_ptr<w2::Quest> q;
    std::string no_quest;
    void MapMarks(int tx, int ty) {
        using namespace w2::quest;
        gfx::SetScissor(kMapX, kMapY, 224, 312);
        if (q->type == kSlayType || q->type == kSlaughter) {
            std::set<int> done;
            int viewer = w2::game::ViewerSide(*G.g, G.player);
            for (w2::Army* a : G.g->armies) {
                bool match =
                        (q->type == kSlayType && q->army_type && a->type == q->army_type->id) ||
                        (q->type == kSlaughter && q->side && a->owner == q->side->index);
                if (match && a->x != w2::kNone && a->owner != G.player->index &&
                    !done.count(a->x + a->y * 1000) && w2::game::Seen(*G.g, viewer, a->x, a->y)) {
                    done.insert(a->x + a->y * 1000);
                    gfx::SetColor(1, 1, 1);
                    int owner = a->owner == w2::kNone ? 8 : a->owner;
                    gfx::Draw(G.atrans_shields, gfx::NewQuad(owner * 16, 164, 16, 10),
                              kMapX + std::max(0, a->x * 2 - 1), kMapY + std::max(0, a->y * 2 - 1));
                }
            }
        }
        gfx::SetScissor();
        if (tx != w2::kNone && q->type != kSlayType && q->type != kSlaughter)
            kit::MapTarget(kMapX, kMapY, q->hero->x, q->hero->y, tx, ty);
    }
    void Draw() override {
        kit::Popup(kR);
        front::DrawStrategicMap(kMapX, kMapY, nullptr, true);
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), "Quest", 432, 62);  // 4125:00b4
        if (!q) {
            kit::Centred(kit::GetFont(2), no_quest, 432, 134);
        } else {
            gfx::Draw(G.scroll_pic, 304, 95);
            const Font& f = kit::GetFont(2).Colours(0, 7);
            kit::Centred(f, q->hero->name + "'s Quest", 432, 145);
            kit::SetPal(0);
            gfx::Rectangle(gfx::kFill, 360, 165, 160, 1);
            QuestText t = DescribeQuest(*G.g, *G.player, *q);
            for (auto& l : t.lines) kit::Centred(f, l.second, 432, l.first);
            MapMarks(t.tx, t.ty);
            front::DrawHeroFigure(kMapX, kMapY, q->hero->x, q->hero->y);
        }
        kit::DrawControls(v);
    }
    void Mousepressed(int x, int y, int) override {
        auto c = kit::ControlAt(v, x, y);
        if (c && c->id == kDone) kit::Pop(this);
    }
    void Keypressed(const std::string& key) override {
        if (key == "escape" || key == "return" || key == "kpenter") kit::Pop(this);
    }
};
}  // namespace

void Open() {
    auto d = std::make_shared<QuestDialog>();
    d->v = kit::View(kDialog);
    d->view = &d->v;
    d->v.state[kDone] = w2::uidata::kNormal;
    auto q = G.player->quest;
    if (q && q->hero && G.g->Alive(q->hero) && q->hero->x != w2::kNone) d->q = q;
    d->no_quest = kit::Text(0x14, G.g->rng.Dice(1, 4, -1));
    kit::Push(d);
}

}  // namespace questui
