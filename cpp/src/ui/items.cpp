// View > Items (66d4:0c21): the scenario's fourteen magic items.
//
// Popup 4, (120, 50) 400x360: "Items" (group 166) in font 1, "Items in this
// scenario" (group 167) at the foot, and a row for each item from record 8
// on, sorted by kind and then by value, 20 apart from y = 90: its name in
// colour 7 ending at x = 304, and what it does from x = 336. Dialog 35: Done
// (495) and Help (496), which shows HELP\HITEM.GFX.
#include <map>

#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/rules.hpp"

namespace items {

namespace {
const kit::Rect kR{120, 50, 400, 360};  // popup 4
const int kDialog = 35, kDone = 495, kHelp = 496;
const int kFirst = 8, kLast = 21;  // records 8-21
const std::map<int, int> kWhat = {{w2::rules::kItemBattle, 1},
                                  {w2::rules::kItemCommand, 2},
                                  {w2::rules::kItemFlight, 3},
                                  {w2::rules::kItemDoubleMove, 4},
                                  {w2::rules::kItemGoldPerCity, 5}};

struct Items : kit::Modal {
    screen::View v;
    std::vector<w2::Item*> list;
    void Draw() override {
        kit::Popup(kR);
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), kit::Text(0xa6, 0), 320, 52);
        kit::Centred(kit::GetFont(2), kit::Text(0xa7, 0), 320, 382);
        const Font& f = kit::GetFont(2).Colours(7, 0);
        // the kind's line carries over when an item is of no kind it knows
        int what = 0;
        for (size_t i = 0; i < list.size(); i++) {
            int y = 90 + 20 * static_cast<int>(i);
            auto w = kWhat.find(list[i]->type);
            if (w != kWhat.end()) what = w->second;
            kit::Right(f, list[i]->name, 304, y);
            f.Draw(w2::Format(kit::Text(0xa7, what), list[i]->value), 336, y);
        }
        kit::DrawControls(v);
    }
    void Mousepressed(int x, int y, int) override {
        auto c = kit::ControlAt(v, x, y);
        if (!c) return;
        if (c->id == kDone)
            kit::Pop(this);
        else if (c->id == kHelp)
            help::Open("HELP\\HITEM.GFX");
    }
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") kit::Pop(this);
    }
};
}  // namespace

void Open() {
    auto d = std::make_shared<Items>();
    d->v = kit::View(kDialog);
    d->view = &d->v;
    d->v.state[kDone] = w2::uidata::kNormal;
    d->v.state[kHelp] = w2::uidata::kNormal;
    for (auto& it : G.g->map->items)
        if (it.index >= kFirst && it.index <= kLast) d->list.push_back(&it);
    // an insertion sort, by kind then value: equal ones keep their order
    auto& l = d->list;
    for (size_t i = 1; i < l.size(); i++) {
        size_t j = i;
        while (j > 0 && (l[j]->type < l[j - 1]->type ||
                         (l[j]->type == l[j - 1]->type && l[j]->value < l[j - 1]->value))) {
            std::swap(l[j], l[j - 1]);
            j--;
        }
    }
    kit::Push(d);
}

}  // namespace items
