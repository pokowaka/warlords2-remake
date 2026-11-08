/**
 * @file
 * @brief The Military Advisor (military_advisor): Shift and a click on an
 * enemy beside the selected stack asks how the fight would go.
 *
 * Popup 12, (176, 60) 288x300, with dialog 25's one button, 424. "Advisor!"
 * in font 1, ADVISOR.PCK at (256, 117), and in font 2 a random line of group
 * 124, one of group 125, and the verdict -- group 126's line wins / 2 out of
 * 19 battles fought in secret, on the game's own dice as the original's are.
 */
#include "ui/dialogs.hpp"
#include "warlords/combat.hpp"

namespace miladvisor {

namespace {
const kit::Rect kPopupRect{176, 60, 288, 300};  ///< Popup 12.
const int kDialog = 25,                         ///< The dialog with the button.
        kOk = 424;                              ///< Its one button.
const int kTitle = 0x7b,                        ///< STRING.DAT group: "Advisor!".
        kHail = 0x7c,                           ///< Group: his greeting, one picked at random.
        kBattle = 0x7d,                         ///< Group: his second line, one picked at random.
        kVerdict = 0x7e;                        ///< Group: the verdict, by wins / 2.
const int kPicture = 48;                        ///< ADVISOR.PCK.

/** @brief The advisor's popup: his picture and three lines. */
struct Advice : kit::Modal {
    screen::View dialog_view;  ///< Dialog 25's button.
    std::string hail,          ///< The first line.
            battle,            ///< The second line.
            verdict;           ///< How the fight would go.
    void Draw() override {
        kit::Popup(kPopupRect);
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), kit::Text(kTitle, 0), 320, 67);
        if (auto art = g_app.screen->ArtFor(kPicture)) {
            gfx::SetColor(1, 1, 1);
            gfx::Draw(art->image, gfx::NewQuad(0, 0, 128, 130), 256, 117);
        }
        const Font& font = kit::GetFont(2);
        gfx::SetColor(1, 1, 1);
        kit::Centred(font, hail, 320, 258);
        kit::Centred(font, battle, 320, 278);
        kit::Centred(font, verdict, 320, 298);
        kit::DrawControls(dialog_view);
    }
    void Mousepressed(int x, int y, int) override {
        auto control = kit::ControlAt(dialog_view, x, y);
        if (control && control->id == kOk) kit::Pop(this);
    }
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") kit::Pop(this);
    }
};
}  // namespace

/**
 * @brief Asks the Military Advisor how `stack`'s attack on the tile (`x`,
 * `y`) would go, and shows his answer.
 *
 * Does nothing when the scenario has the advisor off or there is no stack.
 * The trial battles roll the game's own dice, as the original's do.
 */
void Open(const std::vector<w2::Army*>* stack, int x, int y) {
    w2::Game& game = *g_app.g;
    if (game.map->options.military_advisor == 0 || !stack || stack->empty()) return;
    auto lines = w2::combat::BattleLines(game, *stack, x, y);
    int wins = w2::combat::Advise(game, lines.attackers, lines.defenders, x, y).second;
    auto dialog = std::make_shared<Advice>();
    dialog->dialog_view = kit::View(kDialog);
    dialog->view = &dialog->dialog_view;
    dialog->hail = kit::Text(kHail, game.rng.Dice(1, 5, -1));
    dialog->battle = kit::Text(kBattle, game.rng.Dice(1, 5, -1));
    dialog->verdict = kit::Text(kVerdict, wins / 2);
    dialog->dialog_view.state[kOk] = w2::uidata::kNormal;
    kit::Push(dialog);
}

}  // namespace miladvisor
