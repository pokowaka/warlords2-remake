// View > Stack (89e0:0c9c): the selected stack laid out at length, to be
// grouped the way the bar under the map groups it.
//
// It works on a copy of the bar's slots and its clicks are the bar's: an army
// (336-343) joins the group that moves or leaves it, a mark (344-351) makes
// its group the one, Group (334) puts the lot together and Ungroup (335)
// breaks it up. OK (332) keeps it, Cancel (333) throws it away.
//
// Popup 0, (80, 60) 480x320 (89e0:0e85): STACK.PCK's column heads with the
// combat cap, and eight rows 30 apart from (112, 90): the mark, the army on a
// ring of its group's colour (its shadow when not moving), name, strength --
// and for the group that moves its strength in a fight here (89e0:1b9c) --
// moves, how it moves and its bonus.
#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/combat.hpp"
#include "warlords/rules.hpp"

namespace stackui {

namespace {
const kit::Rect kR{80, 60, 480, 320};  // popup 0
const int kDialog = 18, kOk = 332, kCancel = 333, kGroup = 334, kUngroup = 335, kArmy = 336,
          kMark = 344;
const int kRows = 8;

bool Flies(const w2::Army* a, const w2::ArmyType* t) {
    if (t->flies) return true;
    if (!a->Hero()) return false;
    for (w2::Item* it : a->items)
        if (it->type == w2::rules::kItemFlight) return true;
    return false;
}

struct Stack : kit::Modal {
    screen::View v;
    w2::slots::Slots s;
    void Refresh() {
        auto& st = v.state;
        st[kOk] = st[kCancel] = w2::uidata::kNormal;
        st[kGroup] = s.n > 1 ? w2::uidata::kNormal : w2::uidata::kDisabled;
        st[kUngroup] = st[kGroup];
        for (int i = 0; i < kRows; i++) {
            st[kArmy + i] = i < s.n ? w2::uidata::kNormal : w2::uidata::kDisabled;
            st[kMark + i] = st[kArmy + i];
        }
    }
    void Draw() override {
        w2::Side* side = G.player;
        kit::Popup(kR);
        gfx::SetColor(1, 1, 1);
        if (auto head = G.screen->ArtFor(46))
            gfx::Draw(head->image, gfx::NewQuad(0, 0, 400, 23), 80, 62);
        const Font& f = kit::GetFont(2);
        f.Draw(w2::Fmt("(max +%d)", G.g->map->combat_cap), 480, 66);  // 4125:320d
        auto abits = [](int sx, int sy, int w, int h, int x, int y) {
            gfx::SetColor(1, 1, 1);
            gfx::Draw(G.abits, gfx::NewQuad(sx, sy, w, h), x, y);
        };
        std::vector<w2::Army*> moving;
        for (int i = 0; i < s.n; i++)
            if (s.in_group[i]) moving.push_back(s.army[i]);
        w2::Army* a1 = s.n > 0 ? s.army[0] : nullptr;
        auto fight = a1 ? w2::combat::StackStrengths(*G.g, moving, a1->x, a1->y)
                        : std::map<const w2::Army*, int>{};
        for (int i = 0; i < kRows; i++) {
            int x = 112, y = 90 + 30 * i;
            if (i < s.n && s.army[i]) {
                w2::Army* a = s.army[i];
                const w2::ArmyType* t = G.g->types.ById(a->type);
                if (s.mark[i] != w2::slots::kNomark)
                    abits(448, s.mark[i] == w2::slots::kTick ? 16 : 0, 32, 16, x - 32, y + 5);
                kit::Army(a->type, side->index, x, y, (side->index + s.group[i]) % 8 + 2,
                          !s.in_group[i]);
                gfx::SetColor(1, 1, 1);
                f.Draw(a->Hero() ? a->name : t->name, x + 48, y + 5);
                int str = a->strength;
                if (a->Hero()) str += w2::combat::BattleItems(*a);
                f.Draw(w2::Fmt("%d", std::min(9, str)), x + 168, y + 5);
                auto fi = fight.find(a);
                if (s.in_group[i] && fi != fight.end())
                    f.Draw(w2::Fmt("(%d)", fi->second), x + 184, y + 5);
                f.Draw(w2::Fmt("%d", a->moves), x + 232, y + 5);
                int src = -1;
                if (Flies(a, t))
                    src = 184;
                else if (a->at_sea)
                    src = 424;
                else if (t->woods_move && t->hills_move)
                    src = 216;
                else if (t->woods_move)
                    src = 248;
                else if (t->hills_move)
                    src = 152;
                if (src >= 0) abits(src, 30, 32, 10, x + 256, y + 8);
                gfx::SetColor(1, 1, 1);
                f.Draw(armybonus::BonusText(*t, a), x + 304, y + 5);
            } else {
                kit::Army(w2::kNone, side->index, x, y, 1);
            }
        }
        std::set<int> h;
        for (int i = 0; i < kRows; i++) {
            h.insert(kArmy + i);
            h.insert(kMark + i);
        }
        kit::DrawControls(v, h);
    }
    void Keep() {
        w2::slots::Slots kept = s;
        kit::Pop(this);
        if (G.selection) G.selection->slots = kept;
        front::AfterSlotChange();
    }
    void Mousepressed(int x, int y, int) override {
        auto c = kit::ControlAt(v, x, y);
        if (!c || v.StateOf(c->id) == w2::uidata::kDisabled) return;
        int id = c->id;
        if (id == kOk) return Keep();
        if (id == kCancel) {
            kit::Pop(this);
            return;
        }
        if (id == kGroup)
            w2::slots::All(s, *G.g);
        else if (id == kUngroup)
            w2::slots::Single(s, *G.g);
        else if (id >= kArmy && id < kArmy + kRows)
            w2::slots::Toggle(s, *G.g, id - kArmy);
        else if (id >= kMark && id < kMark + kRows)
            w2::slots::PickGroup(s, *G.g, id - kMark);
        Refresh();
    }
    // the right button on a row (sub-ids 37-44, 89e0:1747)
    bool Info(int sub, int sx, int sy) override {
        int n = sub - 37;
        if (n >= 0 && n < s.n)
            infobox::Army(sx, sy, s.army[n]);
        else
            infobox::ShowLines(sx, sy, "Select Army", "Select armies when present");
        return true;
    }
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter")
            Keep();
        else if (key == "escape")
            kit::Pop(this);
    }
};
}  // namespace

void Open() {
    if (!G.selection) return;
    auto d = std::make_shared<Stack>();
    d->s = G.selection->slots;
    d->v = kit::View(kDialog);
    d->view = &d->v;
    d->Refresh();
    kit::Push(d);
}

}  // namespace stackui
