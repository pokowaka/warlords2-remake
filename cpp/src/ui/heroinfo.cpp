/**
 * @file
 * @brief Hero > Inspect: the hero info dialog.
 *
 * Lists the side's heroes -- by army, last first -- and opens dialog 8 over
 * popup 2 on the one nearest the cursor. It draws the strategic map with
 * every hero's figure -- the others dark, the one shown white and on top --
 * the hero's name, its stack along y = 110, where it is, its battle and
 * command bonuses, level and experience; then the items it carries and
 * those on the ground. Controls: 242 Done, 243/244 next and previous hero,
 * 245/246 the item above and below, 247 Drop It, 248 Take It, 250/249
 * Carried and Ground.
 */
#include <cstdlib>

#include "absl/strings/str_format.h"

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
const kit::Rect kPopupRect{80, 60, 480, 312};  ///< Popup 2.
const int kMapX = 80, kMapY = 60;              ///< Where the strategic map is drawn.
const int kDialog = 8;                         ///< The dialog's BUTTON.DAT controls.
/// Done, next/previous hero, item up/down, Drop It, Take It, Ground, Carried.
const int kDone = 242, kNext = 243, kPrev = 244, kUp = 245, kDown = 246, kDrop = 247, kTake = 248,
          kGround = 249, kCarried = 250;
const int kStringGroup = 0x83;  ///< STRING.DAT group 131: the dialog's words.
/// The checked and the clear tick box: their (x, y) in ABITS.
const int kChecked[2] = {320, 0}, kClear[2] = {320, 20};

/** @brief The colour an item's name is shown in: 10 on the ground, 5 gone, 7 carried. */
int ItemColour(const w2::Item* item) {
    if (item->status == 1) return 10;
    if (item->status == 0) return 5;
    return 7;
}

/** @brief What an item does, as the dialog abbreviates it ("com +2", "fly"). */
std::string Effect(const w2::Item* item) {
    int item_type = item->type, value = item->value;
    if (item_type == rules::kItemStandard) return "com +1";
    if (item_type == rules::kItemCommand) return absl::StrFormat("com +%d", value);
    if (item_type == rules::kItemBattle) return absl::StrFormat("bat +%d", value);
    if (item_type == rules::kItemFlight) return "fly";
    if (item_type == rules::kItemDoubleMove) return "move";
    if (item_type == rules::kItemGoldPerCity) return absl::StrFormat("gld +%d", value);
    return "";
}

/**
 * @brief The total the hero's items of one kind add.
 *
 * A standard counts one towards command.
 */
int ItemSum(const w2::Army* hero, int item_type) {
    int total = 0;
    for (w2::Item* item : hero->Items()) {
        if (item->type == item_type) total += item->value;
        if (item_type == rules::kItemCommand && item->type == rules::kItemStandard) total++;
    }
    return total;
}

/** @brief The hero info dialog, stepping through the side's heroes. */
struct HeroInfo : kit::Modal {
    screen::View dialog_view;       ///< Dialog 8's controls and their states.
    std::vector<w2::Army*> heroes;  ///< The side's heroes, last army first.
    int current_hero = 0;           ///< Index into `heroes` of the hero shown.
    int item = w2::kNone;           ///< Index into List() of the chosen item, or w2::kNone.
    bool ground = false;            ///< The chosen item lies on the ground (not carried).

    /** @brief The items carried by the shown hero and lying where he stands. */
    std::vector<w2::Item*> List() { return w2::hero::ItemsHere(*g_app.g, heroes[current_hero]); }

    /**
     * @brief Chooses item `index` of the list (from 0; anything else chooses none).
     *
     * Sets the buttons to match: Drop It for a carried item, Take It for one
     * on the ground, the arrows where there is an item to go to.
     */
    void Choose(int index) {
        auto items = List();
        item = (index != w2::kNone && index >= 0 && index < static_cast<int>(items.size()))
                       ? index
                       : w2::kNone;
        ground = item != w2::kNone && items[item]->status == 1;
        auto& states = dialog_view.state;
        states[kDone] = w2::uidata::kNormal;
        states[kNext] = heroes.size() > 1 ? w2::uidata::kNormal : w2::uidata::kDisabled;
        states[kPrev] = states[kNext];
        states[kUp] = (item != w2::kNone && item > 0) ? w2::uidata::kNormal : w2::uidata::kDisabled;
        states[kDown] = (item != w2::kNone && item < static_cast<int>(items.size()) - 1)
                                ? w2::uidata::kNormal
                                : w2::uidata::kDisabled;
        states[kDrop] = item != w2::kNone ? w2::uidata::kNormal : w2::uidata::kDisabled;
        states[kTake] = states[kDrop];
        hidden = {ground ? kDrop : kTake};
    }

    /** @brief Shows hero `index`, wrapping round the list, with his first item chosen. */
    void ShowHero(int index) {
        int count = static_cast<int>(heroes.size());
        current_hero = ((index % count) + count) % count;
        Choose(!List().empty() ? 0 : w2::kNone);
    }

    /** @brief Chooses the first item of a kind: status 3 carried, 1 on the ground. */
    void FirstOf(int status) {
        auto items = List();
        for (size_t i = 0; i < items.size(); i++)
            if (items[i]->status == status) {
                Choose(static_cast<int>(i));
                return;
            }
    }

    /** @brief Closes, and lets the quests know items may have changed hands. */
    void Close() {
        kit::Pop(this);
        w2::quest::Notify(*g_app.g, *g_app.player, "item");
    }

    /** @brief Draws the map with the heroes, the hero's stack, his bonuses and his items. */
    void Draw() override {
        w2::Army* hero = heroes[current_hero];
        const Font& font = kit::GetFont(2);
        kit::Popup(kPopupRect);
        front::DrawStrategicMap(kMapX, kMapY, nullptr, true);
        for (size_t i = 0; i < heroes.size(); i++)
            if (static_cast<int>(i) != current_hero)
                front::DrawHeroFigure(kMapX, kMapY, heroes[i]->X(), heroes[i]->Y(), true);
        front::DrawHeroFigure(kMapX, kMapY, hero->X(), hero->Y());
        kit::SetPal(0);
        kit::Outline(308, 206, 248, 129);
        kit::Bevel(307, 205, 250, 131, 4, 2);
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), hero->Name(), 432, 62);
        kit::Army(w2::armytype::kHero, g_app.player->Index(), 304, 110, 1);
        // The rest of his stack, then empty rings up to eight.
        int stack_slot = 1;
        for (w2::Army* army : w2::game::ArmiesAt(*g_app.g, hero->X(), hero->Y())) {
            if (army != hero && stack_slot < 8) {
                kit::Army(army->TypeIndex(), army->OwnerIndex(), 304 + stack_slot * 32, 110, 1);
                stack_slot++;
            }
        }
        for (int j = stack_slot; j <= 7; j++)
            kit::Army(w2::kNone, g_app.player->Index(), 304 + j * 32, 110, 1);
        w2::City* in_city = w2::game::CityAt(*g_app.g, hero->X(), hero->Y());
        // The nearest city the viewer has seen.
        w2::City* near = nullptr;
        int near_distance = w2::kNone;
        int viewer = w2::game::ViewerSide(*g_app.g, g_app.player);
        for (auto& city : g_app.g->GetMap().Cities()) {
            if (w2::game::Seen(*g_app.g, viewer, city.X(), city.Y())) {
                int distance =
                        std::max(std::abs(city.X() - hero->X()), std::abs(city.Y() - hero->Y()));
                if (near_distance == w2::kNone || distance < near_distance) {
                    near = &city;
                    near_distance = distance;
                }
            }
        }
        int battle = ItemSum(hero, rules::kItemBattle);
        int command = w2::combat::kHeroTable[std::max(0, std::min(9, hero->Strength() + battle))] +
                      ItemSum(hero, rules::kItemCommand);
        gfx::SetColor(1, 1, 1);
        kit::Right(font, kit::Text(kStringGroup, in_city ? 1 : 0), 384, 145);
        kit::Right(font, kit::Text(kStringGroup, 2), 384, 165);
        kit::Right(font, kit::Text(kStringGroup, 3), 384, 185);
        kit::Right(font, kit::Text(kStringGroup, 4), 528, 165);
        kit::Right(font, kit::Text(kStringGroup, 5), 528, 185);
        font.Draw(near ? near->Name() : "", 392, 145);
        font.Draw(absl::StrFormat("+%d", battle), 392, 165);
        font.Draw(absl::StrFormat("+%d", command), 392, 185);
        font.Draw(absl::StrFormat("%d", hero->Level() ? hero->Level() : 1), 536, 165);
        font.Draw(absl::StrFormat("%d", hero->Experience()), 536, 185);
        font.Draw(w2::Format(kit::Text(kStringGroup, 6), current_hero + 1,
                             static_cast<int>(heroes.size())),
                  312, 347);
        auto items = List();
        kit::SetPal(3);
        gfx::Rectangle(gfx::kFill, 367, 236, 184, 66);
        kit::Bevel(367, 236, 184, 66, 4, 2);
        kit::Bevel(369, 258, 180, 21, 2, 4);
        gfx::SetColor(1, 1, 1);
        kit::Centred(font, kit::Text(kStringGroup, ground ? 7 : 8), 432, 211);
        if (item != w2::kNone) {
            // The chosen item in the middle row, its neighbours above and below.
            for (int row = 0; row <= 2; row++) {
                int index = item - 1 + row;
                if (index >= 0 && index < static_cast<int>(items.size()))
                    font.Colours(ItemColour(items[index]), 0)
                            .Draw(items[index]->name, 376, 238 + 22 * row);
            }
            w2::Item* chosen_item = items[item];
            kit::Centred(font.Colours(ItemColour(chosen_item), 0), Effect(chosen_item), 340, 260);
        }
        auto box = [](const int* cell, int x) {
            gfx::SetColor(1, 1, 1);
            gfx::Draw(g_app.abits, gfx::NewQuad(cell[0], cell[1], 24, 20), x, 311);
        };
        box(ground ? kClear : kChecked, 312);
        box(ground ? kChecked : kClear, 400);
        font.Colours(7, 0).Draw("Carried", 336, 313);
        font.Colours(10, 0).Draw("Ground", 424, 313);
        kit::DrawControls(dialog_view, hidden);
    }

    /** @brief Done, next/previous hero, item up/down, Drop It, Take It, Ground, Carried. */
    void Mousepressed(int x, int y, int) override {
        auto control = kit::ControlAt(dialog_view, x, y, hidden);
        if (!control) return;
        auto items = List();
        if (control->id == kDone)
            Close();
        else if (control->id == kNext)
            ShowHero(current_hero + 1);
        else if (control->id == kPrev)
            ShowHero(current_hero - 1);
        else if (control->id == kUp)
            Choose(item - 1);
        else if (control->id == kDown)
            Choose(item + 1);
        else if (control->id == kDrop && item != w2::kNone) {
            w2::Item* dropped = items[item];
            w2::hero::DropItem(*g_app.g, heroes[current_hero], dropped);
            // Dropped at sea it is gone, with a splash.
            if (dropped->status == 0) sound::Effect("splash");
            Choose(std::min(item, static_cast<int>(List().size()) - 1));
        } else if (control->id == kTake && item != w2::kNone) {
            w2::hero::TakeItem(*g_app.g, heroes[current_hero], items[item]);
            Choose(std::min(item, static_cast<int>(List().size()) - 1));
        } else if (control->id == kGround) {
            FirstOf(1);
        } else if (control->id == kCarried) {
            FirstOf(3);
        }
    }

    /** @brief Escape or Enter closes. */
    void Keypressed(const std::string& key) override {
        if (key == "escape" || key == "return" || key == "kpenter") Close();
    }
};
}  // namespace

/** @brief Opens hero info on the side's hero nearest the view's centre; nothing without one. */
void Open() {
    auto dialog = std::make_shared<HeroInfo>();
    for (int i = static_cast<int>(g_app.g->Armies().size()) - 1; i >= 0; i--) {
        w2::Army* army = g_app.g->Armies()[i];
        if (army->Hero() && army->OwnerIndex() == g_app.player->Index() && !army->InTransit() &&
            army->X() != w2::kNone)
            dialog->heroes.push_back(army);
    }
    if (dialog->heroes.empty()) return;
    auto [centre_x, centre_y] = front::ViewCentre();
    int nearest = 0, best = w2::kNone;
    for (size_t i = 0; i < dialog->heroes.size(); i++) {
        int distance = std::max(std::abs(dialog->heroes[i]->X() - centre_x),
                                std::abs(dialog->heroes[i]->Y() - centre_y));
        if (best == w2::kNone || distance < best) {
            nearest = static_cast<int>(i);
            best = distance;
        }
    }
    dialog->dialog_view = kit::View(kDialog);
    dialog->view = &dialog->dialog_view;
    dialog->ShowHero(nearest);
    kit::Push(dialog);
}

}  // namespace heroinfo
