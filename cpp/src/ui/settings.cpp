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
#include "platform/sound.hpp"
#include "ui/dialogs.hpp"

namespace settings {

namespace {
const kit::Rect kPopupRect{120, 50, 400, 360};  ///< Popup 4, the whole dialog.
const int kDialog = 9;                          ///< BUTTON.DAT dialog number.
const int kOk = 251;                            ///< The OK control.
const int kHuman = 252;                         ///< First of the eight Human boxes, one a side.
const int kEnhanced = 260;                      ///< First of the eight Enhanced boxes.
const int kObserve = 268;                       ///< First of the eight Observe boxes.
const int kSound = 276;                         ///< First of the three sound boxes.
/// The computer's levels, as the Human column words them.
const char* const kLevels[3] = {"Knight", "Lord", "Warlord"};
/** @brief One of the sound boxes: where it is, its label and its option. */
struct SoundBox {
    int x;              ///< Left edge of the box.
    int y;              ///< Top edge of the box.
    const char* label;  ///< The words beside it.
    const char* key;    ///< The sound option it toggles.
};
/// Music, Effects and Speech, in control order.
const SoundBox kSounds[3] = {
    {152, 340, "Music", "music"}, {152, 370, "Effects", "effects"}, {256, 340, "Speech", "speech"}};

/** @brief The settings dialog: who plays each side, and the sounds. */
struct Settings : kit::Modal {
    screen::View dialog_view;  ///< Dialog 9's controls.
    kit::Done after;           ///< Runs when OK closes the dialog.
    /** @brief Enables the controls of the sides in play; hides the others'. */
    void Refresh() {
        auto& states = dialog_view.state;
        states[kOk] = w2::uidata::kNormal;
        for (int i = 0; i < 3; i++) states[kSound + i] = w2::uidata::kNormal;
        hidden.clear();
        for (int i = 0; i < 8; i++) {
            if (G.g->map->sides[i].in_use) {
                states[kHuman + i] = states[kEnhanced + i] = states[kObserve + i] =
                        w2::uidata::kNormal;
            } else {
                hidden.insert(kHuman + i);
                hidden.insert(kEnhanced + i);
                hidden.insert(kObserve + i);
            }
        }
    }
    /** @brief Draws a check box at (x, y), ticked when `checked`. */
    static void Box(bool checked, int x, int y) {
        gfx::SetColor(1, 1, 1);
        gfx::Draw(G.abits, gfx::NewQuad(320, checked ? 0 : 20, 24, 20), x, y);
    }
    /** @brief Draws the headings, a row a side and the sound boxes. */
    void Draw() override {
        kit::Popup(kPopupRect);
        const Font& font = kit::GetFont(2);
        w2::Side* player = G.player;
        const Font& head = font.Colours(player->colour, player->edge);
        gfx::SetColor(1, 1, 1);
        kit::Right(head, "Name", 248, 60);
        kit::Centred(head, "Human", 280, 60);
        kit::Centred(head, "Enhanced", 396, 60);
        kit::Centred(head, "Observe", 468, 60);
        for (int i = 0; i < 8; i++) {
            const w2::Side& side = G.g->map->sides[i];
            int y = 90 + 30 * i;
            gfx::SetColor(1, 1, 1);
            kit::Right(font.Colours(side.colour, side.edge), side.name, 248, y);
            bool playing = side.in_use && side.alive;
            if (!playing) {
                if (side.name != "Not used") font.Draw("Deceased!", 280, y);
            } else {
                Box(!side.computer, 256, y);
                gfx::SetColor(1, 1, 1);
                font.Draw(side.computer
                                  ? (side.level >= 0 && side.level < 3 ? kLevels[side.level] : "")
                                  : "Human",
                          280, y);
                Box(side.enhanced, 400, y);
                if (side.computer) Box(side.observe, 448, y);
            }
        }
        const auto& sound_options = sound::MutableOptions();
        bool sound_on[3] = {sound_options.music, sound_options.effects, sound_options.speech};
        for (int i = 0; i < 3; i++) {
            Box(sound_on[i], kSounds[i].x, kSounds[i].y);
            gfx::SetColor(1, 1, 1);
            font.Draw(kSounds[i].label, kSounds[i].x + 24, kSounds[i].y);
        }
        kit::DrawControls(dialog_view, hidden);
    }
    /** @brief Closes the dialog, then runs `after`. */
    void Finish() {
        auto after_copy = after;
        kit::Pop(this);
        if (after_copy) after_copy();
    }
    /** @brief OK closes; a box toggles its side's setting or its sound. */
    void Mousepressed(int x, int y, int) override {
        auto control = kit::ControlAt(dialog_view, x, y, hidden);
        if (!control || dialog_view.StateOf(control->id) == w2::uidata::kDisabled) return;
        int control_id = control->id;
        if (control_id == kOk) return Finish();
        if (control_id >= kHuman && control_id < kHuman + 8) {
            w2::Side& side = G.g->map->sides[control_id - kHuman];
            side.computer = !side.computer;
        } else if (control_id >= kEnhanced && control_id < kEnhanced + 8) {
            w2::Side& side = G.g->map->sides[control_id - kEnhanced];
            side.enhanced = !side.enhanced;
        } else if (control_id >= kObserve && control_id < kObserve + 8) {
            w2::Side& side = G.g->map->sides[control_id - kObserve];
            if (side.computer) side.observe = !side.observe;
        } else if (control_id >= kSound && control_id < kSound + 3) {
            sound::Toggle(kSounds[control_id - kSound].key);
        }
        Refresh();
    }
    /** @brief Enter or Escape closes, as OK does. */
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") Finish();
    }
};
}  // namespace

/** @brief Opens the settings dialog; `after` runs when it closes. */
void Open(kit::Done after) {
    auto dialog = std::make_shared<Settings>();
    dialog->dialog_view = kit::View(kDialog);
    dialog->view = &dialog->dialog_view;
    dialog->after = after;
    dialog->Refresh();
    kit::Push(dialog);
}

}  // namespace settings
