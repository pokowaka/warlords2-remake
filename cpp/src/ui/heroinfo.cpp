// Hero > Inspect: the hero info dialog.
//
// 6c1b:0000 lists the side's heroes -- by army, last first -- and opens
// dialog 8 over popup 2 on the one nearest the cursor. 6c1b:00f9 draws the
// strategic map with every hero's figure -- the others dark, the one shown
// white and on top (6c1b:10ac) -- the hero's name, its stack along
// y = 110, where it is, its battle and command bonuses, level and
// experience; 6c1b:0724 the items it carries and those on the ground.
// Controls: 242 Done, 243/244 next and previous hero, 245/246 the item
// above and below, 247 Drop It, 248 Take It, 250/249 Carried and Ground.
#include <cstdlib>

#include "platform/sound.hpp"
#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/armytype.hpp"
#include "warlords/combat.hpp"
#include "warlords/hero.hpp"
#include "warlords/rules.hpp"

namespace heroinfo {

namespace rules = w2::rules;

namespace {
const kit::Rect kR{80, 60, 480, 312};  // popup 2
const int kMapX = 80, kMapY = 60;
const int kDialog = 8;
const int kDone = 242, kNext = 243, kPrev = 244, kUp = 245, kDown = 246, kDrop = 247, kTake = 248,
          kGround = 249, kCarried = 250;
const int kS = 0x83;                                      // STRING.DAT group 131
const int kChecked[2] = {320, 0}, kClear[2] = {320, 20};  // 4125:0ad4

int ItemColour(const w2::Item* it) {
    if (it->status == 1) return 10;
    if (it->status == 0) return 5;
    return 7;
}

// What an item does, as the dialog abbreviates it (4125:0d7c-0d9c).
std::string Effect(const w2::Item* it) {
    int t = it->type, v = it->value;
    if (t == rules::kItemStandard) return "com +1";
    if (t == rules::kItemCommand) return w2::Fmt("com +%d", v);
    if (t == rules::kItemBattle) return w2::Fmt("bat +%d", v);
    if (t == rules::kItemFlight) return "fly";
    if (t == rules::kItemDoubleMove) return "move";
    if (t == rules::kItemGoldPerCity) return w2::Fmt("gld +%d", v);
    return "";
}

int ItemSum(const w2::Army* h, int t) {
    int n = 0;
    for (w2::Item* it : h->items) {
        if (it->type == t) n += it->value;
        if (t == rules::kItemCommand && it->type == rules::kItemStandard) n++;
    }
    return n;
}

struct HeroInfo : kit::Modal {
    screen::View v;
    std::vector<w2::Army*> heroes;
    int cur = 0;
    int item = w2::kNone;
    bool ground = false;

    std::vector<w2::Item*> List() { return w2::hero::ItemsHere(*G.g, heroes[cur]); }

    // choose item i of the list, from 0 (or none)
    void Choose(int i) {
        auto items = List();
        item = (i != w2::kNone && i >= 0 && i < static_cast<int>(items.size())) ? i : w2::kNone;
        ground = item != w2::kNone && items[item]->status == 1;
        auto& st = v.state;
        st[kDone] = w2::uidata::kNormal;
        st[kNext] = heroes.size() > 1 ? w2::uidata::kNormal : w2::uidata::kDisabled;
        st[kPrev] = st[kNext];
        st[kUp] = (item != w2::kNone && item > 0) ? w2::uidata::kNormal : w2::uidata::kDisabled;
        st[kDown] = (item != w2::kNone && item < static_cast<int>(items.size()) - 1)
                            ? w2::uidata::kNormal
                            : w2::uidata::kDisabled;
        st[kDrop] = item != w2::kNone ? w2::uidata::kNormal : w2::uidata::kDisabled;
        st[kTake] = st[kDrop];
        hidden = {ground ? kDrop : kTake};
    }

    void ShowHero(int i) {
        int n = static_cast<int>(heroes.size());
        cur = ((i % n) + n) % n;
        Choose(!List().empty() ? 0 : w2::kNone);
    }

    // the first item of a kind: 3 carried, 1 on the ground (6c1b:0fc2, 1037)
    void FirstOf(int status) {
        auto items = List();
        for (size_t i = 0; i < items.size(); i++)
            if (items[i]->status == status) {
                Choose(static_cast<int>(i));
                return;
            }
    }

    void Close() {
        kit::Pop(this);
        w2::quest::Notify(*G.g, *G.player, "item");
    }

    void Draw() override {
        w2::Army* h = heroes[cur];
        const Font& f = kit::GetFont(2);
        kit::Popup(kR);
        front::DrawStrategicMap(kMapX, kMapY, nullptr, true);
        for (size_t i = 0; i < heroes.size(); i++)
            if (static_cast<int>(i) != cur)
                front::DrawHeroFigure(kMapX, kMapY, heroes[i]->x, heroes[i]->y, true);
        front::DrawHeroFigure(kMapX, kMapY, h->x, h->y);
        kit::SetPal(0);
        kit::Outline(308, 206, 248, 129);
        kit::Bevel(307, 205, 250, 131, 4, 2);
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), h->name, 432, 62);
        kit::Army(w2::armytype::kHero, G.player->index, 304, 110, 1);
        int k = 1;
        for (w2::Army* a : w2::game::ArmiesAt(*G.g, h->x, h->y)) {
            if (a != h && k < 8) {
                kit::Army(a->type, a->owner, 304 + k * 32, 110, 1);
                k++;
            }
        }
        for (int j = k; j <= 7; j++) kit::Army(w2::kNone, G.player->index, 304 + j * 32, 110, 1);
        w2::City* in_city = w2::game::CityAt(*G.g, h->x, h->y);
        w2::City* near = nullptr;
        int near_d = w2::kNone;
        int viewer = w2::game::ViewerSide(*G.g, G.player);
        for (auto& c : G.g->map->cities) {
            if (w2::game::Seen(*G.g, viewer, c.x, c.y)) {
                int dd = std::max(std::abs(c.x - h->x), std::abs(c.y - h->y));
                if (near_d == w2::kNone || dd < near_d) {
                    near = &c;
                    near_d = dd;
                }
            }
        }
        int battle = ItemSum(h, rules::kItemBattle);
        int command = w2::combat::kHeroTable[std::max(0, std::min(9, h->strength + battle))] +
                      ItemSum(h, rules::kItemCommand);
        gfx::SetColor(1, 1, 1);
        kit::Right(f, kit::Text(kS, in_city ? 1 : 0), 384, 145);
        kit::Right(f, kit::Text(kS, 2), 384, 165);
        kit::Right(f, kit::Text(kS, 3), 384, 185);
        kit::Right(f, kit::Text(kS, 4), 528, 165);
        kit::Right(f, kit::Text(kS, 5), 528, 185);
        f.Draw(near ? near->name : "", 392, 145);
        f.Draw(w2::Fmt("+%d", battle), 392, 165);
        f.Draw(w2::Fmt("+%d", command), 392, 185);
        f.Draw(w2::Fmt("%d", h->level ? h->level : 1), 536, 165);
        f.Draw(w2::Fmt("%d", h->experience), 536, 185);
        f.Draw(w2::Format(kit::Text(kS, 6), cur + 1, static_cast<int>(heroes.size())), 312, 347);
        auto items = List();
        kit::SetPal(3);
        gfx::Rectangle(gfx::kFill, 367, 236, 184, 66);
        kit::Bevel(367, 236, 184, 66, 4, 2);
        kit::Bevel(369, 258, 180, 21, 2, 4);
        gfx::SetColor(1, 1, 1);
        kit::Centred(f, kit::Text(kS, ground ? 7 : 8), 432, 211);
        if (item != w2::kNone) {
            for (int row = 0; row <= 2; row++) {
                int i = item - 1 + row;
                if (i >= 0 && i < static_cast<int>(items.size()))
                    f.Colours(ItemColour(items[i]), 0).Draw(items[i]->name, 376, 238 + 22 * row);
            }
            w2::Item* it = items[item];
            kit::Centred(f.Colours(ItemColour(it), 0), Effect(it), 340, 260);
        }
        auto box = [](const int* src, int x) {
            gfx::SetColor(1, 1, 1);
            gfx::Draw(G.abits, gfx::NewQuad(src[0], src[1], 24, 20), x, 311);
        };
        box(ground ? kClear : kChecked, 312);
        box(ground ? kChecked : kClear, 400);
        f.Colours(7, 0).Draw("Carried", 336, 313);
        f.Colours(10, 0).Draw("Ground", 424, 313);
        kit::DrawControls(v, hidden);
    }

    void Mousepressed(int x, int y, int) override {
        auto c = kit::ControlAt(v, x, y, hidden);
        if (!c) return;
        auto items = List();
        if (c->id == kDone)
            Close();
        else if (c->id == kNext)
            ShowHero(cur + 1);
        else if (c->id == kPrev)
            ShowHero(cur - 1);
        else if (c->id == kUp)
            Choose(item - 1);
        else if (c->id == kDown)
            Choose(item + 1);
        else if (c->id == kDrop && item != w2::kNone) {
            w2::Item* it = items[item];
            w2::hero::DropItem(*G.g, heroes[cur], it);
            // 7563:0943: dropped at sea it is gone, with a splash
            if (it->status == 0) sound::Effect("splash");
            Choose(std::min(item, static_cast<int>(List().size()) - 1));
        } else if (c->id == kTake && item != w2::kNone) {
            w2::hero::TakeItem(*G.g, heroes[cur], items[item]);
            Choose(std::min(item, static_cast<int>(List().size()) - 1));
        } else if (c->id == kGround) {
            FirstOf(1);
        } else if (c->id == kCarried) {
            FirstOf(3);
        }
    }

    void Keypressed(const std::string& key) override {
        if (key == "escape" || key == "return" || key == "kpenter") Close();
    }
};
}  // namespace

void Open() {
    auto d = std::make_shared<HeroInfo>();
    for (int i = static_cast<int>(G.g->armies.size()) - 1; i >= 0; i--) {
        w2::Army* a = G.g->armies[i];
        if (a->Hero() && a->owner == G.player->index && !a->transit && a->x != w2::kNone)
            d->heroes.push_back(a);
    }
    if (d->heroes.empty()) return;
    auto [cx, cy] = front::ViewCentre();
    int cur = 0, best = w2::kNone;
    for (size_t i = 0; i < d->heroes.size(); i++) {
        int dd = std::max(std::abs(d->heroes[i]->x - cx), std::abs(d->heroes[i]->y - cy));
        if (best == w2::kNone || dd < best) {
            cur = static_cast<int>(i);
            best = dd;
        }
    }
    d->v = kit::View(kDialog);
    d->view = &d->v;
    d->ShowHero(cur);
    kit::Push(d);
}

}  // namespace heroinfo
