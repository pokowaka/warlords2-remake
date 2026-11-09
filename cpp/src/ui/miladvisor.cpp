/**
 * @file
 * @brief The Military Advisor (`military_advisor`): Shift and a click on an
 * enemy beside the selected stack asks how the fight would go.
 *
 * Popup 12, `(176, 60)` `288x300`, with dialog 25's one button, `424`. `"Advisor!"`
 * in font 1, `ADVISOR.PCK` at `(256, 117)`, and in font 2 a random line of group
 * 124, one of group 125, and the verdict -- group 126's line `wins / 2` out of
 * 19 battles fought in secret, on the game's own dice as the original's are.
 */
#include <memory>
#include <string>
#include <utility>

#include "ui/dialogs.hpp"
#include "warlords/combat.hpp"

namespace miladvisor {

namespace {

constexpr kit::Rect kPopupRect{176, 60, 288, 300};  ///< Popup 12.
constexpr int kDialog = 25;                         ///< The dialog with the button.
constexpr int kOk = 424;                            ///< Its one button.
constexpr int kTitle = 0x7b;                        ///< STRING.DAT group: "Advisor!".
constexpr int kHail = 0x7c;                         ///< Group: his greeting, picked at random.
constexpr int kBattle = 0x7d;                       ///< Group: his second line, picked at random.
constexpr int kVerdict = 0x7e;                      ///< Group: the verdict, by wins / 2.
constexpr int kPicture = 48;                        ///< ADVISOR.PCK.

/**
 * @brief Modal presenting the Military Advisor portrait and 19-trial battle verdict.
 */
class Advice final : public kit::Modal {
  public:
    Advice(w2::UiContext& ctx, std::string hail, std::string battle, std::string verdict)
            : kit::Modal(ctx)
            , view_(kit::View(ctx, kDialog))
            , hail_(std::move(hail))
            , battle_(std::move(battle))
            , verdict_(std::move(verdict)) {
        view_.state[kOk] = w2::uidata::kNormal;
        SetDialogView(&view_);
    }

    void Draw() override {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        kit::Popup(ctx, kPopupRect);
        ren.SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(ctx, 1), kit::Text(ctx, kTitle, 0), 320, 67);
        if (const auto* art = ctx.ControlArt(kPicture)) {
            ren.SetColor(1, 1, 1);
            ren.Draw(art->image, gfx::NewQuad(0, 0, 128, 130), 256, 117);
        }
        const Font& font = kit::GetFont(ctx, 2);
        ren.SetColor(1, 1, 1);
        kit::Centred(font, hail_, 320, 258);
        kit::Centred(font, battle_, 320, 278);
        kit::Centred(font, verdict_, 320, 298);
        kit::DrawControls(ctx, view_);
    }

    void Mousepressed(int x, int y, int) override {
        const auto* control = kit::ControlAt(view_, x, y);
        if (control && control->id == kOk) PopSelf();
    }

    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") PopSelf();
    }

  private:
    screen::View view_;    ///< Dialog 25's button.
    std::string hail_;     ///< The first line.
    std::string battle_;   ///< The second line.
    std::string verdict_;  ///< How the fight would go.
};

}  // namespace

void Open(w2::UiContext& ctx, const ArmyStack* stack, int tile_x, int tile_y) {
    w2::Game& game = ctx.ActiveGame();
    if (game.GetMap().GameOptions().military_advisor == 0 || !stack || stack->empty()) return;
    auto lines = w2::combat::BattleLines(game, *stack, tile_x, tile_y);
    int wins = w2::combat::Advise(game, lines.attackers, lines.defenders, tile_x, tile_y).second;
    std::string hail = kit::Text(ctx, kHail, game.GetRng().Dice(1, 5, -1));
    std::string battle = kit::Text(ctx, kBattle, game.GetRng().Dice(1, 5, -1));
    std::string verdict = kit::Text(ctx, kVerdict, wins / 2);
    kit::Push(ctx, std::make_shared<Advice>(ctx, std::move(hail), std::move(battle),
                                            std::move(verdict)));
}

}  // namespace miladvisor
