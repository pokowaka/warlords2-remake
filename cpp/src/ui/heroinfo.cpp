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
#include <algorithm>
#include <array>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/str_format.h"

#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/armytype.hpp"
#include "warlords/combat.hpp"
#include "warlords/hero.hpp"
#include "warlords/quest.hpp"
#include "warlords/rules.hpp"

namespace heroinfo {

namespace rules = w2::rules;

namespace {
constexpr kit::Rect kPopupRect{80, 60, 480, 312};  ///< Popup 2.
constexpr int kMapX = 80, kMapY = 60;              ///< Where the strategic map is drawn.
constexpr int kDialog = 8;                         ///< The dialog's BUTTON.DAT controls.
/// Done, next/previous hero, item up/down, Drop It, Take It, Ground, Carried.
constexpr int kDone = 242, kNext = 243, kPrev = 244, kUp = 245, kDown = 246, kDrop = 247,
              kTake = 248, kGround = 249, kCarried = 250;
constexpr int kStringGroup = 0x83;  ///< STRING.DAT group 131: the dialog's words.
/// The checked and the clear tick box: their (x, y) in ABITS.
constexpr std::array<int, 2> kChecked = {320, 0};
constexpr std::array<int, 2> kClear = {320, 20};

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
class HeroInfo final : public kit::Modal {
  public:
    using HeroList = std::vector<w2::Army*>;

    HeroInfo(w2::UiContext& ctx, HeroList heroes, int initial_hero)
            : kit::Modal(ctx), dialog_view_(kit::View(ctx, kDialog)), heroes_(std::move(heroes)) {
        SetDialogView(&dialog_view_);
        ShowHero(initial_hero);
    }

    /** @brief Draws the map with the heroes, the hero's stack, his bonuses and his items. */
    void Draw() override {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        w2::Game& game = ctx.ActiveGame();
        w2::Side* player = ctx.Player();
        w2::Army* hero = heroes_[current_hero_];
        const Font& font = kit::GetFont(ctx, 2);
        kit::Popup(ctx, kPopupRect);
        ctx.Actions().DrawStrategicMap(kMapX, kMapY, nullptr, true);
        for (size_t i = 0; i < heroes_.size(); i++) {
            if (static_cast<int>(i) != current_hero_) {
                ctx.Actions().DrawHeroFigure(kMapX, kMapY, heroes_[i]->X(), heroes_[i]->Y(), true);
            }
        }
        ctx.Actions().DrawHeroFigure(kMapX, kMapY, hero->X(), hero->Y());
        kit::SetPal(ctx, 0);
        kit::Outline(ctx, 308, 206, 248, 129);
        kit::Bevel(ctx, 307, 205, 250, 131, 4, 2);
        ren.SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(ctx, 1), hero->Name(), 432, 62);
        kit::Army(ctx, w2::armytype::kHero, player->Index(), 304, 110, 1);
        // The rest of his stack, then empty rings up to eight.
        int stack_slot = 1;
        for (w2::Army* army : w2::game::ArmiesAt(game, hero->X(), hero->Y())) {
            if (army != hero && stack_slot < 8) {
                kit::Army(ctx, army->TypeIndex(), army->OwnerIndex(), 304 + stack_slot * 32, 110,
                          1);
                stack_slot++;
            }
        }
        for (int j = stack_slot; j <= 7; j++) {
            kit::Army(ctx, w2::kNone, player->Index(), 304 + j * 32, 110, 1);
        }
        w2::City* in_city = w2::game::CityAt(game, hero->X(), hero->Y());
        // The nearest city the viewer has seen.
        w2::City* near_city = nullptr;
        int near_distance = w2::kNone;
        int viewer = w2::game::ViewerSide(game, player);
        for (auto& city : game.GetMap().Cities()) {
            if (w2::game::Seen(game, viewer, city.X(), city.Y())) {
                int distance =
                        std::max(std::abs(city.X() - hero->X()), std::abs(city.Y() - hero->Y()));
                if (near_distance == w2::kNone || distance < near_distance) {
                    near_city = &city;
                    near_distance = distance;
                }
            }
        }
        int battle = ItemSum(hero, rules::kItemBattle);
        int command = w2::combat::kHeroTable[std::max(0, std::min(9, hero->Strength() + battle))] +
                      ItemSum(hero, rules::kItemCommand);
        ren.SetColor(1, 1, 1);
        kit::Right(font, kit::Text(ctx, kStringGroup, in_city ? 1 : 0), 384, 145);
        kit::Right(font, kit::Text(ctx, kStringGroup, 2), 384, 165);
        kit::Right(font, kit::Text(ctx, kStringGroup, 3), 384, 185);
        kit::Right(font, kit::Text(ctx, kStringGroup, 4), 528, 165);
        kit::Right(font, kit::Text(ctx, kStringGroup, 5), 528, 185);
        font.Draw(near_city ? near_city->Name() : "", 392, 145);
        font.Draw(absl::StrFormat("+%d", battle), 392, 165);
        font.Draw(absl::StrFormat("+%d", command), 392, 185);
        font.Draw(absl::StrFormat("%d", hero->Level() ? hero->Level() : 1), 536, 165);
        font.Draw(absl::StrFormat("%d", hero->Experience()), 536, 185);
        font.Draw(w2::Format(kit::Text(ctx, kStringGroup, 6), current_hero_ + 1,
                             static_cast<int>(heroes_.size())),
                  312, 347);
        ItemList items_here = List();
        kit::SetPal(ctx, 3);
        ren.Rectangle(gfx::kFill, 367, 236, 184, 66);
        kit::Bevel(ctx, 367, 236, 184, 66, 4, 2);
        kit::Bevel(ctx, 369, 258, 180, 21, 2, 4);
        ren.SetColor(1, 1, 1);
        kit::Centred(font, kit::Text(ctx, kStringGroup, ground_ ? 7 : 8), 432, 211);
        if (item_ != w2::kNone) {
            // The chosen item in the middle row, its neighbours above and below.
            for (int row = 0; row <= 2; row++) {
                int index = item_ - 1 + row;
                if (index >= 0 && index < static_cast<int>(items_here.size())) {
                    font.Colours(ItemColour(items_here[index]), 0)
                            .Draw(items_here[index]->name, 376, 238 + 22 * row);
                }
            }
            w2::Item* chosen_item = items_here[item_];
            kit::Centred(font.Colours(ItemColour(chosen_item), 0), Effect(chosen_item), 340, 260);
        }
        auto box = [&ctx, &ren](const std::array<int, 2>& cell, int x) {
            ren.SetColor(1, 1, 1);
            ren.Draw(ctx.Assets().Abits(), gfx::NewQuad(cell[0], cell[1], 24, 20), x, 311);
        };
        box(ground_ ? kClear : kChecked, 312);
        box(ground_ ? kChecked : kClear, 400);
        font.Colours(7, 0).Draw("Carried", 336, 313);
        font.Colours(10, 0).Draw("Ground", 424, 313);
        kit::DrawControls(ctx, dialog_view_, HiddenControls());
    }

    /** @brief Done, next/previous hero, item up/down, Drop It, Take It, Ground, Carried. */
    void Mousepressed(int x, int y, int) override {
        w2::UiContext& ctx = Ctx();
        const auto* control = kit::ControlAt(dialog_view_, x, y, HiddenControls());
        if (!control) return;
        ItemList items_here = List();
        if (control->id == kDone) {
            Close();
        } else if (control->id == kNext) {
            ShowHero(current_hero_ + 1);
        } else if (control->id == kPrev) {
            ShowHero(current_hero_ - 1);
        } else if (control->id == kUp) {
            Choose(item_ - 1);
        } else if (control->id == kDown) {
            Choose(item_ + 1);
        } else if (control->id == kDrop && item_ != w2::kNone) {
            w2::Item* dropped = items_here[item_];
            w2::hero::DropItem(ctx.ActiveGame(), heroes_[current_hero_], dropped);
            // Dropped at sea it is gone, with a splash.
            if (dropped->status == 0) ctx.Audio().Effect("splash");
            Choose(std::min(item_, static_cast<int>(List().size()) - 1));
        } else if (control->id == kTake && item_ != w2::kNone) {
            w2::hero::TakeItem(ctx.ActiveGame(), heroes_[current_hero_], items_here[item_]);
            Choose(std::min(item_, static_cast<int>(List().size()) - 1));
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

  private:
    using ItemList = std::vector<w2::Item*>;

    /** @brief The items carried by the shown hero and lying where he stands. */
    ItemList List() { return w2::hero::ItemsHere(Ctx().ActiveGame(), heroes_[current_hero_]); }

    /**
     * @brief Chooses item `index` of the list (from 0; anything else chooses none).
     *
     * Sets the buttons to match: Drop It for a carried item, Take It for one
     * on the ground, the arrows where there is an item to go to.
     */
    void Choose(int index) {
        ItemList items_here = List();
        item_ = (index != w2::kNone && index >= 0 && index < static_cast<int>(items_here.size()))
                        ? index
                        : w2::kNone;
        ground_ = item_ != w2::kNone && items_here[item_]->status == 1;
        auto& states = dialog_view_.state;
        states[kDone] = w2::uidata::kNormal;
        states[kNext] = heroes_.size() > 1 ? w2::uidata::kNormal : w2::uidata::kDisabled;
        states[kPrev] = states[kNext];
        states[kUp] =
                (item_ != w2::kNone && item_ > 0) ? w2::uidata::kNormal : w2::uidata::kDisabled;
        states[kDown] = (item_ != w2::kNone && item_ < static_cast<int>(items_here.size()) - 1)
                                ? w2::uidata::kNormal
                                : w2::uidata::kDisabled;
        states[kDrop] = item_ != w2::kNone ? w2::uidata::kNormal : w2::uidata::kDisabled;
        states[kTake] = states[kDrop];
        HiddenControls() = {ground_ ? kDrop : kTake};
    }

    /** @brief Shows hero `index`, wrapping round the list, with his first item chosen. */
    void ShowHero(int index) {
        int count = static_cast<int>(heroes_.size());
        current_hero_ = ((index % count) + count) % count;
        Choose(!List().empty() ? 0 : w2::kNone);
    }

    /** @brief Chooses the first item of a kind: status 3 carried, 1 on the ground. */
    void FirstOf(int status) {
        ItemList items_here = List();
        for (size_t i = 0; i < items_here.size(); i++) {
            if (items_here[i]->status == status) {
                Choose(static_cast<int>(i));
                return;
            }
        }
    }

    /** @brief Closes, and lets the quests know items may have changed hands. */
    void Close() {
        w2::UiContext& ctx = Ctx();
        PopSelf();
        w2::quest::Notify(ctx.ActiveGame(), *ctx.Player(), "item");
    }

    screen::View dialog_view_;  ///< Dialog 8's controls and their states.
    HeroList heroes_;           ///< The side's heroes, last army first.
    int current_hero_ = 0;      ///< Index into `heroes_` of the hero shown.
    int item_ = w2::kNone;      ///< Index into List() of the chosen item, or w2::kNone.
    bool ground_ = false;       ///< The chosen item lies on the ground (not carried).
};
}  // namespace

/** @brief Opens hero info on the side's hero nearest the view's centre; nothing without one. */
void Open(w2::UiContext& ctx) {
    HeroInfo::HeroList heroes;
    const auto& armies = ctx.ActiveGame().Armies();
    for (int i = static_cast<int>(armies.size()) - 1; i >= 0; i--) {
        w2::Army* army = armies[i];
        if (army->Hero() && army->OwnerIndex() == ctx.Player()->Index() && !army->InTransit() &&
            army->X() != w2::kNone) {
            heroes.push_back(army);
        }
    }
    if (heroes.empty()) return;
    auto [centre_x, centre_y] = ctx.ViewCentre();
    int nearest = 0, best = w2::kNone;
    for (size_t i = 0; i < heroes.size(); i++) {
        int distance =
                std::max(std::abs(heroes[i]->X() - centre_x), std::abs(heroes[i]->Y() - centre_y));
        if (best == w2::kNone || distance < best) {
            nearest = static_cast<int>(i);
            best = distance;
        }
    }
    kit::Push(ctx, std::make_shared<HeroInfo>(ctx, std::move(heroes), nearest));
}

}  // namespace heroinfo
