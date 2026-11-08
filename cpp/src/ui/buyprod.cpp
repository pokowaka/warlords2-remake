// Build Production: buying a new army type for a city.
//
// The city dialog's Build Prod button (control 204, 7087:0978) pushes popup
// 11 -- (80, 60) 480x350, marble -- and dialog 23 over it, drawn by
// auto_ui_build_production (7087:09da): the title between the side's
// shields, "The %s city of %s", every type with a price four to a row --
// ghosted when the city already has it or the side cannot pay -- "Currently
// Producing" with the city's four slots, and the gold. The slot being bought
// into is framed in colours 0 and 9, the others in 3. Control 396 is Done,
// 397-400 pick the slot (live only once all four are full), 401 on are the
// types. Done puts the list back in price order (7087:0eca).
#include "ui/dialogs.hpp"
#include "util/util.hpp"

namespace buyprod {

namespace {
const kit::Rect kR{80, 60, 480, 350};  // popup 11
const int kDialog = 23;
const int kDone = 396, kSlotFirst = 397, kTypeFirst = 401;
const int kStr = 0x70;  // STRING.DAT group 112

struct BuyDialog : kit::Modal {
    w2::City* city = nullptr;
    kit::Done done;
    screen::View v;
    std::vector<const w2::ArmyType*> types;
    int slot = 0;

    void Refresh() {
        auto& st = v.state;
        st[kDone] = w2::uidata::NORMAL;
        bool full = city->slots.size() >= 4;
        for (int i = 0; i < 4; i++)
            st[kSlotFirst + i] = full ? w2::uidata::NORMAL : w2::uidata::DISABLED;
        for (size_t n = 0; n < types.size(); n++)
            st[kTypeFirst + static_cast<int>(n)] =
                    w2::game::cannotBuy(*G.g, *G.player, *city, *types[n]).empty()
                            ? w2::uidata::NORMAL
                            : w2::uidata::DISABLED;
    }
    void Close() {
        w2::game::sortProduction(*city);
        auto d = done;
        kit::pop(this);
        if (d) d();
    }
    void Buy(int n) {
        if (n < 0 || n >= static_cast<int>(types.size())) return;
        const w2::ArmyType* a = types[n];
        if (!w2::game::cannotBuy(*G.g, *G.player, *city, *a).empty()) return;
        w2::game::buyProduction(*G.g, *G.player, *city, slot, a->id);
        // on to the next empty slot, if there is one (7087:0ee3)
        if (slot < 3 && slot + 1 >= static_cast<int>(city->slots.size())) slot++;
        Refresh();
    }
    void draw() override {
        int side = G.player->index;
        kit::popup(kR);
        gfx::setColor(1, 1, 1);
        kit::centred(kit::font(1), kit::text(kStr, 0), 320, 64);
        kit::shield(side, 88, 64);
        kit::shield(side, 512, 64);
        const Font& body = kit::font(2);
        gfx::setColor(1, 1, 1);
        kit::centred(body, w2::format(kit::text(kStr, 1), G.player->name, city->name), 320, 104);
        kit::centred(body, w2::format(kit::text(kStr, 2), G.player->gold), 352, 365);
        kit::centred(body, kit::text(kStr, 3), 176, 348);
        for (int i = 0; i < 4; i++) {
            bool have = i < static_cast<int>(city->slots.size());
            int x = 104 + i * 40;
            kit::army(have ? city->slots[i].type : w2::NONE, side, x, 370, have ? side + 2 : 1);
            int fx = x - 2, fy = 368;
            if (slot == i) {
                kit::setPal(0);
                kit::outline(fx, fy, 37, 35);
                kit::setPal(9);
                kit::outline(fx - 1, fy - 1, 37, 35);
            } else {
                kit::setPal(3);
                kit::outline(fx, fy, 37, 35);
                kit::outline(fx - 1, fy - 1, 37, 35);
            }
        }
        for (size_t n = 0; n < types.size(); n++) {
            int col = static_cast<int>(n) % 4, row = static_cast<int>(n) / 4;
            int x = 88 + col * 120, y = 136 + row * 31;
            kit::army(types[n]->id, side, x, y, 1,
                      !w2::game::cannotBuy(*G.g, *G.player, *city, *types[n]).empty());
            gfx::setColor(1, 1, 1);
            body.draw(w2::fmt("%d gp", types[n]->price), x + 40, y + 6);
        }
        kit::drawControls(v);
    }
    void mousepressed(int x, int y, int) override {
        auto c = kit::controlAt(v, x, y);
        if (!c) return;
        if (c->id == kDone)
            Close();
        else if (c->id >= kSlotFirst && c->id < kSlotFirst + 4)
            slot = c->id - kSlotFirst;
        else if (c->id >= kTypeFirst)
            Buy(c->id - kTypeFirst);
    }
    // the right button on a type for sale (sub-ids 5-24, 7087:11f4)
    bool info(int sub, int sx, int sy) override {
        int n = sub - 5;
        if (n < 0 || n >= static_cast<int>(types.size())) return false;
        const w2::ArmyType* a = types[n];
        infobox::armyType(
                sx, sy, a->id,
                w2::Slot{a->id, a->name, a->strength, a->time, a->cost, a->move, a->price});
        return true;
    }
    void keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") Close();
    }
};
}  // namespace

void open(w2::City* c, kit::Done after) {
    auto d = std::make_shared<BuyDialog>();
    d->city = c;
    d->done = after;
    d->v = kit::view(kDialog);
    d->view = &d->v;
    d->types = w2::game::buyableTypes(*G.g);
    d->slot = w2::game::buySlot(*c);
    d->Refresh();
    kit::push(d);
}

}  // namespace buyprod
