/**
 * @file
 * @brief Hero > Levels (auto_ui_hero_levels).
 *
 * Popup 0, (80, 60) 480x320, and dialog 28: Done (469). "Hero Levels" in
 * font 1, the column heads in the side's colours at y = 105, and a row per
 * hero, 30 apart from y = 128, highest level first: the hero on a ring --
 * its side's colour at the top level, grey below -- its name, its title, its
 * experience, what the next level needs, its strength and its moves.
 */
#include "absl/strings/str_format.h"

#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/armytype.hpp"

namespace levels {

namespace {
const kit::Rect kPopupRect{80, 60, 480, 320};  ///< Popup 0.
const int kDialog = 28,                        ///< The dialog with the button.
        kDone = 469;                           ///< The Done button.
const int kTextGroup = 0x68,   ///< STRING.DAT group: the title and the column heads.
        kTitlesMale = 0x63,    ///< Group: a hero's title at each level.
        kTitlesFemale = 0x64;  ///< Group: a heroine's title at each level.

/** @brief The experience the next level needs, or kNone at the top level. */
int Needs(int level) {
    return level == 1 ? 15 : level == 2 ? 30 : level == 3 ? 60 : w2::kNone;
}

/** @brief The Hero Levels dialog: a row per hero of the player's. */
struct Levels : kit::Modal {
    screen::View dialog_view;     ///< Dialog 28's button.
    std::vector<w2::Army*> rows;  ///< The heroes, highest level first.
    void Draw() override {
        w2::Side* side = g_app.player;
        const Font& font = kit::GetFont(2);
        kit::Popup(kPopupRect);
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), kit::Text(kTextGroup, 0), 320, 63);
        const Font& head = font.Colours(side->Colour(), side->Edge());
        head.Draw(kit::Text(kTextGroup, 1), 128, 105);
        head.Draw(kit::Text(kTextGroup, 2), 272, 105);
        const int column_x[4] = {392, 440, 488, 536};
        for (int column = 0; column < 4; column++)
            kit::Centred(head, kit::Text(kTextGroup, column + 3), column_x[column], 105);
        for (size_t i = 0; i < rows.size(); i++) {
            w2::Army* hero = rows[i];
            int y = 128 + 30 * static_cast<int>(i);
            int level = hero->Level() ? hero->Level() : 1;
            kit::Army(w2::armytype::kHero, side->Index(), 88, y,
                      level == 4 ? side->Index() + 2 : 1);
            gfx::SetColor(1, 1, 1);
            font.Draw(hero->Name(), 128, y + 6);
            font.Draw(kit::Text(hero->IsFemale() ? kTitlesFemale : kTitlesMale, level - 1), 272,
                      y + 6);
            kit::Centred(font, absl::StrFormat("%d", hero->Experience()), 392, y + 6);
            kit::Centred(font,
                         Needs(level) != w2::kNone ? absl::StrFormat("%d", Needs(level)) : "-", 440,
                         y + 6);
            kit::Centred(font, absl::StrFormat("%d", hero->Strength()), 488, y + 6);
            kit::Centred(font, absl::StrFormat("%d", hero->MaxMoves()), 536, y + 6);
        }
        for (int i = static_cast<int>(rows.size()); i < 6; i++)
            kit::Army(w2::kNone, side->Index(), 88, 128 + 30 * i, 1);
        kit::DrawControls(dialog_view);
    }
    void Mousepressed(int x, int y, int) override {
        auto control = kit::ControlAt(dialog_view, x, y);
        if (control && control->id == kDone) kit::Pop(this);
    }
    void Keypressed(const std::string& key) override {
        if (key == "escape" || key == "return" || key == "kpenter") kit::Pop(this);
    }
};
}  // namespace

/**
 * @brief Opens Hero > Levels: the player's heroes, highest level first and,
 * within a level, the newest first.
 */
void Open() {
    auto dialog = std::make_shared<Levels>();
    dialog->dialog_view = kit::View(kDialog);
    dialog->view = &dialog->dialog_view;
    dialog->dialog_view.state[kDone] = w2::uidata::kNormal;
    for (int level = 4; level >= 1; level--) {
        for (int i = static_cast<int>(g_app.g->armies.size()) - 1; i >= 0; i--) {
            w2::Army* army = g_app.g->armies[i];
            if (army->Hero() && army->OwnerIndex() == g_app.player->Index() &&
                (army->Level() ? army->Level() : 1) == level)
                dialog->rows.push_back(army);
        }
    }
    kit::Push(dialog);
}

}  // namespace levels
