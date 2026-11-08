/**
 * @file
 * @brief The end of the game, and the screens it leads to.
 *
 * The three pictures are popups 20-22, (136, 40) 368x390, each its own
 * bitmap with the words painted in:
 *   20 RESIGN.PCK   "An offer of Peace" -- dialog 30: No (486), Yes (485)
 *   21 RESIGNNO.PCK "Peace is not an option!" -- dialog 31: Done (484)
 *   22 RESIGNYE.PCK "Congratulations!" -- dialog 32: Done (483)
 * Each comes with its own music.
 *
 * The round's end puts out the sides left without a city first, each told
 * with a line of group 11 -- in a box, or in the status bar when no human
 * plays -- and says once when the last human has fallen (group 13). When the
 * last computer side has triumphed (group 15) it is handed to the player,
 * its turn opening as a human's, to look the world over.
 */
#include "platform/sound.hpp"
#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/cues.hpp"

namespace ending {

namespace {
const kit::Rect kPopupRect{136, 40, 368, 390};  ///< Popups 20-22.

/** @brief The picture for popup 20, 21 or 22: its entry in FILE.DAT group 3. */
int ArtForPopup(int popup) {
    return popup == 20 ? 12 : popup == 21 ? 14 : 13;
}

/** @brief Line `line` of STRING.DAT group `group`. */
std::string GroupLine(int group, int line) {
    return kit::Text(group, line);
}

/** @brief One of the three end pictures, with its buttons painted in. */
struct PictureDialog : kit::Modal {
    int popup = 20;                 ///< Which picture: popup 20, 21 or 22.
    screen::View dialog_view;       ///< The picture's dialog controls and their states.
    int default_id = 0;             ///< The control Enter presses.
    int cancel_id = 0;              ///< The control Escape presses.
    std::function<void(int)> pick;  ///< Gets the pressed control's id, once closed.

    /** @brief Draws the frame, the picture and its controls. */
    void Draw() override {
        kit::PopupFrame(kPopupRect);
        if (auto art = g_app.screen->ArtFor(ArtForPopup(popup))) {
            gfx::SetColor(1, 1, 1);
            gfx::Draw(art->image, gfx::NewQuad(0, 0, kPopupRect.w, kPopupRect.h), kPopupRect.x,
                      kPopupRect.y);
        }
        kit::DrawControls(dialog_view);
    }

    /** @brief Closes, then hands `control_id` to `pick`. */
    void Press(int control_id) {
        auto pick_copy = pick;
        kit::Pop(this);
        pick_copy(control_id);
    }

    void Mousepressed(int x, int y, int) override {
        if (auto control = kit::ControlAt(dialog_view, x, y)) Press(control->id);
    }

    /** @brief Enter presses the default control, Escape the cancelling one. */
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter")
            Press(default_id);
        else if (key == "escape")
            Press(cancel_id);
    }
};

/**
 * @brief Shows an end picture.
 * @param popup Which picture: popup 20, 21 or 22.
 * @param dialog_id The dialog its buttons come from.
 * @param live_ids The controls to show live.
 * @param default_id The control Enter presses.
 * @param cancel_id The control Escape presses.
 * @param pick Gets the pressed control's id once the picture has closed.
 */
void ShowPicture(int popup, int dialog_id, std::vector<int> live_ids, int default_id, int cancel_id,
                 std::function<void(int)> pick) {
    auto dialog = std::make_shared<PictureDialog>();
    dialog->popup = popup;
    dialog->dialog_view = kit::View(dialog_id);
    dialog->view = &dialog->dialog_view;
    for (int control_id : live_ids) dialog->dialog_view.state[control_id] = w2::uidata::kNormal;
    dialog->default_id = default_id;
    dialog->cancel_id = cancel_id;
    dialog->pick = pick;
    kit::Push(dialog);
}

/** @brief Congratulations, then the map shown whole, then `after`. */
void Victory(kit::Done after) {
    sound::Music(w2::cues::kTriumph);
    ShowPicture(22, 32, {483}, 483, 483, [after](int) {
        g_app.g->map->options.hidden_map = 0;
        front::StratDirty();
        search::ShowMessage(GroupLine(0x10, 0), GroupLine(0x10, 1), after);
    });
}
}  // namespace

/** @brief A side put out at the round's end: its line of group 11. */
std::string FallenText(const w2::Fallen& fallen) {
    return w2::Format(GroupLine(0xb, fallen.line), fallen.side->name);
}

namespace {
/**
 * @brief Tells what the round's end put out and whether the last human fell, then `after`.
 *
 * Each in a box of its own, one after the other; nothing when the computer's
 * turns have told it already.
 */
void Tell(w2::Ending& outcome, kit::Done after) {
    auto done = [after]() {
        if (after) after();
    };
    if (outcome.told) return done();
    outcome.told = true;
    std::vector<std::string> lines;
    for (auto& fallen : outcome.fallen) lines.push_back(FallenText(fallen));
    bool no_humans = outcome.no_humans;
    // Says one line, and on closing calls itself for the next.
    auto next = std::make_shared<std::function<void(size_t)>>();
    std::weak_ptr<std::function<void(size_t)>> self = next;
    *next = [lines, no_humans, done, self](size_t line_index) {
        if (line_index < lines.size()) {
            auto again = self.lock();
            search::Say(lines[line_index], [again, line_index]() { (*again)(line_index + 1); });
            return;
        }
        if (no_humans) {
            search::ShowMessage(GroupLine(0xd, 0), GroupLine(0xd, 1), [done]() {
                search::ShowMessage(GroupLine(0xd, 2), GroupLine(0xd, 3), done);
            });
            return;
        }
        done();
    };
    (*next)(0);
}
}  // namespace

/**
 * @brief The game's end, or nearly: told, then `after`.
 *
 * In order: what the round's end put out; a computer side's triumph handed
 * to the player; a pending offer of surrender; the player's own victory.
 */
void Show(w2::Ending& outcome, kit::Done after) {
    auto done = [after]() {
        if (after) after();
    };
    if (!outcome.told) {
        Tell(outcome, [&outcome, after]() { Show(outcome, after); });
        return;
    }
    // The last computer side has triumphed, and is the player's now.
    if (outcome.triumph && outcome.winner == g_app.player && !outcome.shown) {
        outcome.shown = true;
        search::Say(w2::Format(GroupLine(0xf, 0), g_app.player->name), done);
        return;
    }
    // The offer waits on the game, not on the round's finding: computer
    // turns may have come between.
    if (w2::game::SurrenderPending(*g_app.g)) {
        g_app.g->surrender_shown = true;
        sound::Music(w2::cues::kSurrender);
        ShowPicture(20, 30, {485, 486}, 485, 486, [after, done](int control_id) {
            if (control_id == 485) {
                w2::game::AcceptSurrender(*g_app.g, *g_app.player);
                Victory(after);
            } else {
                sound::Music(w2::cues::kDefiance);
                ShowPicture(21, 31, {484}, 484, 484, [done](int) { done(); });
            }
        });
        return;
    }
    if (outcome.won && outcome.winner == g_app.player) {
        search::ShowMessage(w2::Format(GroupLine(0xf, 0), g_app.player->name), GroupLine(0xf, 1),
                            [after]() { Victory(after); });
        return;
    }
    done();
}

/**
 * @brief The game is over: says who won, or that no player is left.
 *
 * With no human left and the game saying "No more players", the second box
 * quits to the start screens.
 */
void Over(w2::Ending& outcome) {
    if (!outcome.told && !outcome.fallen.empty()) {
        Tell(outcome, [&outcome]() { Over(outcome); });
        return;
    }
    if (outcome.winner) {
        search::Say(w2::Format(GroupLine(0xf, 0), outcome.winner->name));
        return;
    }
    bool humans = false;
    for (w2::Side* side : g_app.g->sides)
        if (side->alive && !side->computer && !w2::game::SideCities(*g_app.g, *side).empty())
            humans = true;
    if (!humans && w2::Contains(outcome.message, "No more players")) {
        search::ShowMessage(GroupLine(0xc, 0), GroupLine(0xc, 1), []() {
            // "return to DOS" -- here, the start screens
            search::ShowMessage(GroupLine(0xc, 2), GroupLine(0xc, 3), []() { front::Quit(); });
        });
        return;
    }
    search::ShowMessage(GroupLine(0xd, 0), GroupLine(0xd, 1),
                        []() { search::ShowMessage(GroupLine(0xd, 2), GroupLine(0xd, 3)); });
}

}  // namespace ending
