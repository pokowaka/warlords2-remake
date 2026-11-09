/**
 * @file
 * @brief Hero > Levels (`auto_ui_hero_levels`).
 *
 * Popup 0, `(80, 60)` `480x320`, and dialog 28: Done (`469`). `"Hero Levels"` in
 * font 1, the column heads in the side's colours at `y = 105`, and a row per
 * hero, 30 apart from `y = 128`, highest level first: the hero on a ring --
 * its side's colour at the top level, grey below -- its name, its title, its
 * experience, what the next level needs, its strength and its moves.
 */
#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/str_format.h"

#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/armytype.hpp"

namespace levels {

namespace {

using HeroList = std::vector<w2::Army*>;

constexpr kit::Rect kPopupRect{80, 60, 480, 320};  ///< Popup 0.
constexpr int kDialog = 28;                        ///< The dialog with the button.
constexpr int kDone = 469;                         ///< The Done button.
constexpr int kTextGroup = 0x68;                   ///< STRING.DAT group: title and column heads.
constexpr int kTitlesMale = 0x63;                  ///< Group: a hero's title at each level.
constexpr int kTitlesFemale = 0x64;                ///< Group: a heroine's title at each level.
constexpr std::array<int, 4> kColumnX = {392, 440, 488, 536};

/** @brief The experience the next level needs, or `kNone` at the top level. */
int Needs(int level) {
    return level == 1 ? 15 : level == 2 ? 30 : level == 3 ? 60 : w2::kNone;
}

/**
 * @brief Modal displaying the player's heroes ordered highest level first.
 */
class Levels final : public kit::Modal {
  public:
    Levels(w2::UiContext& ctx, HeroList rows)
            : kit::Modal(ctx), view_(kit::View(ctx, kDialog)), rows_(std::move(rows)) {
        view_.state[kDone] = w2::uidata::kNormal;
        SetDialogView(&view_);
    }

    void Draw() override {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        w2::Side* side = ctx.Player();
        const Font& font = kit::GetFont(ctx, 2);
        kit::Popup(ctx, kPopupRect);
        ren.SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(ctx, 1), kit::Text(ctx, kTextGroup, 0), 320, 63);
        const Font& head = font.Colours(side->Colour(), side->Edge());
        head.Draw(kit::Text(ctx, kTextGroup, 1), 128, 105);
        head.Draw(kit::Text(ctx, kTextGroup, 2), 272, 105);
        for (int column = 0; column < 4; column++) {
            kit::Centred(head, kit::Text(ctx, kTextGroup, column + 3), kColumnX[column], 105);
        }
        for (size_t i = 0; i < rows_.size(); i++) {
            w2::Army* hero = rows_[i];
            int y = 128 + 30 * static_cast<int>(i);
            int level = hero->Level() ? hero->Level() : 1;
            kit::Army(ctx, w2::armytype::kHero, side->Index(), 88, y,
                      level == 4 ? side->Index() + 2 : 1);
            ren.SetColor(1, 1, 1);
            font.Draw(hero->Name(), 128, y + 6);
            font.Draw(kit::Text(ctx, hero->IsFemale() ? kTitlesFemale : kTitlesMale, level - 1),
                      272, y + 6);
            kit::Centred(font, absl::StrFormat("%d", hero->Experience()), 392, y + 6);
            kit::Centred(font,
                         Needs(level) != w2::kNone ? absl::StrFormat("%d", Needs(level)) : "-", 440,
                         y + 6);
            kit::Centred(font, absl::StrFormat("%d", hero->Strength()), 488, y + 6);
            kit::Centred(font, absl::StrFormat("%d", hero->MaxMoves()), 536, y + 6);
        }
        for (int i = static_cast<int>(rows_.size()); i < 6; i++) {
            kit::Army(ctx, w2::kNone, side->Index(), 88, 128 + 30 * i, 1);
        }
        kit::DrawControls(ctx, view_);
    }

    void Mousepressed(int x, int y, int) override {
        const auto* control = kit::ControlAt(view_, x, y);
        if (control && control->id == kDone) PopSelf();
    }

    void Keypressed(const std::string& key) override {
        if (key == "escape" || key == "return" || key == "kpenter") PopSelf();
    }

  private:
    screen::View view_;  ///< Dialog 28's button.
    HeroList rows_;      ///< The heroes, highest level first.
};

}  // namespace

void Open(w2::UiContext& ctx) {
    w2::Game& game = ctx.ActiveGame();
    int player_index = ctx.Player()->Index();
    HeroList rows;
    for (int level = 4; level >= 1; level--) {
        for (int i = static_cast<int>(game.Armies().size()) - 1; i >= 0; i--) {
            w2::Army* army = game.Armies()[i];
            if (army->Hero() && army->OwnerIndex() == player_index &&
                (army->Level() ? army->Level() : 1) == level) {
                rows.push_back(army);
            }
        }
    }
    kit::Push(ctx, std::make_shared<Levels>(ctx, std::move(rows)));
}

}  // namespace levels
