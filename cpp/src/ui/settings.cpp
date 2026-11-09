/**
 * @file
 * @brief Game > Settings -- the same screen that sets the sides up for a new
 * game.
 *
 * Popup 4, (120, 50) 400x360, dialog 9: a row a side -- its name in its
 * colours, "Deceased!" for a side out of play, else the Human box and "Human"
 * or its level, Enhanced, and for a computer Observe -- and the boxes for
 * Music, Effects and Speech. 252-259 turn a side human or computer, 260-267
 * Enhanced, 268-275 Observe, 276-278 the sounds (a toggle writes OPTIONS.SND
 * straight back); OK (251).
 */
#include <array>
#include <memory>
#include <string>
#include <utility>

#include "platform/sound.hpp"
#include "ui/dialogs.hpp"

namespace settings {

namespace {

constexpr kit::Rect kPopupRect{120, 50, 400, 360};  ///< Popup 4, the whole dialog.
constexpr int kDialog = 9;                          ///< BUTTON.DAT dialog number.
constexpr int kOk = 251;                            ///< The OK control.
constexpr int kHuman = 252;                         ///< First of the eight Human boxes, one a side.
constexpr int kEnhanced = 260;                      ///< First of the eight Enhanced boxes.
constexpr int kObserve = 268;                       ///< First of the eight Observe boxes.
constexpr int kSound = 276;                         ///< First of the three sound boxes.
/// The computer's levels, as the Human column words them.
constexpr std::array<const char*, 3> kLevels = {"Knight", "Lord", "Warlord"};

/** @brief One of the sound boxes: where it is, its label and its channel. */
struct SoundBox {
    int x;                        ///< Left edge of the box.
    int y;                        ///< Top edge of the box.
    const char* label;            ///< The words beside it.
    sound::AudioChannel channel;  ///< The audio channel it toggles.
};

/// Music, Effects and Speech, in control order.
constexpr std::array<SoundBox, 3> kSounds = {{
    {152, 340, "Music", sound::AudioChannel::kMusic},
    {152, 370, "Effects", sound::AudioChannel::kEffects},
    {256, 340, "Speech", sound::AudioChannel::kSpeech},
}};

/** @brief The settings dialog: who plays each side, and the sounds. */
class SettingsModal final : public kit::Modal {
  public:
    SettingsModal(w2::UiContext& ctx, kit::Done after)
            : kit::Modal(ctx), dialog_view_(kit::View(ctx, kDialog)), after_(std::move(after)) {
        SetDialogView(&dialog_view_);
        Refresh();
    }

    /** @brief Draws the headings, a row a side and the sound boxes. */
    void Draw() override {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        kit::Popup(ctx, kPopupRect);
        const Font& font = kit::GetFont(ctx, 2);
        w2::Side* player = ctx.Player();
        const Font& head = font.Colours(player->Colour(), player->Edge());
        ren.SetColor(1, 1, 1);
        kit::Right(head, "Name", 248, 60);
        kit::Centred(head, "Human", 280, 60);
        kit::Centred(head, "Enhanced", 396, 60);
        kit::Centred(head, "Observe", 468, 60);
        for (int i = 0; i < 8; i++) {
            const w2::Side& side = ctx.ActiveGame().GetMap().Sides()[i];
            int y = 90 + 30 * i;
            ren.SetColor(1, 1, 1);
            kit::Right(font.Colours(side.Colour(), side.Edge()), side.Name(), 248, y);
            bool playing = side.IsActive();
            if (!playing) {
                if (side.Name() != "Not used") font.Draw("Deceased!", 280, y);
            } else {
                DrawBox(ctx, !side.IsComputerControlled(), 256, y);
                ren.SetColor(1, 1, 1);
                font.Draw(side.IsComputerControlled()
                                  ? (side.Level() >= 0 && side.Level() < 3 ? kLevels[side.Level()]
                                                                           : "")
                                  : "Human",
                          280, y);
                DrawBox(ctx, side.IsEnhanced(), 400, y);
                if (side.IsComputerControlled()) DrawBox(ctx, side.IsObserved(), 448, y);
            }
        }
        const auto& sound_options = ctx.Audio().GetOptions();
        bool sound_on[3] = {sound_options.music, sound_options.effects, sound_options.speech};
        for (int i = 0; i < 3; i++) {
            DrawBox(ctx, sound_on[i], kSounds[i].x, kSounds[i].y);
            ren.SetColor(1, 1, 1);
            font.Draw(kSounds[i].label, kSounds[i].x + 24, kSounds[i].y);
        }
        kit::DrawControls(ctx, dialog_view_, HiddenControls());
    }

    /** @brief OK closes; a box toggles its side's setting or its sound. */
    void Mousepressed(int x, int y, int) override {
        w2::UiContext& ctx = Ctx();
        const auto* control = kit::ControlAt(dialog_view_, x, y, HiddenControls());
        if (!control || dialog_view_.StateOf(control->id) == w2::uidata::kDisabled) return;
        int control_id = control->id;
        if (control_id == kOk) {
            Finish();
            return;
        }
        auto& sides = ctx.ActiveGame().GetMap().Sides();
        if (control_id >= kHuman && control_id < kHuman + 8) {
            w2::Side& side = sides[control_id - kHuman];
            side.SetComputerControlled(!side.IsComputerControlled());
        } else if (control_id >= kEnhanced && control_id < kEnhanced + 8) {
            w2::Side& side = sides[control_id - kEnhanced];
            side.SetEnhanced(!side.IsEnhanced());
        } else if (control_id >= kObserve && control_id < kObserve + 8) {
            w2::Side& side = sides[control_id - kObserve];
            if (side.IsComputerControlled()) side.SetObserved(!side.IsObserved());
        } else if (control_id >= kSound && control_id < kSound + 3) {
            ctx.Audio().Toggle(kSounds[control_id - kSound].channel);
        }
        Refresh();
    }

    /** @brief Enter or Escape closes, as OK does. */
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") Finish();
    }

  private:
    /** @brief Draws a check box at (x, y), ticked when `checked`. */
    static void DrawBox(w2::UiContext& ctx, bool checked, int x, int y) {
        ctx.Renderer().SetColor(1, 1, 1);
        ctx.Renderer().Draw(ctx.Assets().Abits(), gfx::NewQuad(320, checked ? 0 : 20, 24, 20), x,
                            y);
    }

    /** @brief Enables the controls of the sides in play; hides the others'. */
    void Refresh() {
        auto& states = dialog_view_.state;
        states[kOk] = w2::uidata::kNormal;
        for (int i = 0; i < 3; i++) states[kSound + i] = w2::uidata::kNormal;
        HiddenControls().clear();
        const auto& sides = Ctx().ActiveGame().GetMap().Sides();
        for (int i = 0; i < 8; i++) {
            if (sides[i].InUse()) {
                states[kHuman + i] = states[kEnhanced + i] = states[kObserve + i] =
                        w2::uidata::kNormal;
            } else {
                HiddenControls().insert(kHuman + i);
                HiddenControls().insert(kEnhanced + i);
                HiddenControls().insert(kObserve + i);
            }
        }
    }

    /** @brief Closes the dialog, then runs `after_`. */
    void Finish() {
        kit::Done after_copy = std::move(after_);
        PopSelf();
        if (after_copy) after_copy();
    }

    screen::View dialog_view_;  ///< Dialog 9's controls.
    kit::Done after_;           ///< Runs when OK closes the dialog.
};

}  // namespace

/** @brief Opens the settings dialog; `after` runs when it closes. */
void Open(w2::UiContext& ctx, kit::Done after) {
    kit::Push(ctx, std::make_shared<SettingsModal>(ctx, std::move(after)));
}

}  // namespace settings
