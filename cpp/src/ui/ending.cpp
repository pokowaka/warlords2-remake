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
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/cues.hpp"

namespace ending {

namespace {
constexpr kit::Rect kPopupRect{136, 40, 368, 390};  ///< Popups 20-22.

/** @brief The picture for popup 20, 21 or 22: its entry in FILE.DAT group 3. */
int ArtForPopup(int popup) {
    return popup == 20 ? 12 : popup == 21 ? 14 : 13;
}

/** @brief Line `line` of STRING.DAT group `group`. */
std::string GroupLine(const w2::UiContext& ctx, int group, int line) {
    return kit::Text(ctx, group, line);
}

/** @brief One of the three end pictures, with its buttons painted in. */
class PictureDialog final : public kit::Modal {
  public:
    using ControlIdList = std::vector<int>;
    using PickControlFn = std::function<void(int)>;

    PictureDialog(w2::UiContext& ctx, int popup, int dialog_id, const ControlIdList& live_ids,
                  int default_id, int cancel_id, PickControlFn pick)
            : kit::Modal(ctx)
            , popup_(popup)
            , dialog_view_(kit::View(ctx, dialog_id))
            , default_id_(default_id)
            , cancel_id_(cancel_id)
            , pick_(std::move(pick)) {
        SetDialogView(&dialog_view_);
        for (int control_id : live_ids) dialog_view_.state[control_id] = w2::uidata::kNormal;
    }

    /** @brief Draws the frame, the picture and its controls. */
    void Draw() override {
        w2::UiContext& ctx = Ctx();
        kit::PopupFrame(ctx, kPopupRect);
        if (const screen::Art* art = ctx.ControlArt(ArtForPopup(popup_))) {
            ctx.Renderer().SetColor(1, 1, 1);
            ctx.Renderer().Draw(art->image, gfx::NewQuad(0, 0, kPopupRect.w, kPopupRect.h),
                                kPopupRect.x, kPopupRect.y);
        }
        kit::DrawControls(ctx, dialog_view_);
    }

    void Mousepressed(int x, int y, int) override {
        if (const auto* control = kit::ControlAt(dialog_view_, x, y)) Press(control->id);
    }

    /** @brief Enter presses the default control, Escape the cancelling one. */
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter") {
            Press(default_id_);
        } else if (key == "escape") {
            Press(cancel_id_);
        }
    }

  private:
    /** @brief Closes, then hands `control_id` to `pick_`. */
    void Press(int control_id) {
        PickControlFn pick_copy = std::move(pick_);
        PopSelf();
        pick_copy(control_id);
    }

    int popup_ = 20;            ///< Which picture: popup 20, 21 or 22.
    screen::View dialog_view_;  ///< The picture's dialog controls and their states.
    int default_id_ = 0;        ///< The control Enter presses.
    int cancel_id_ = 0;         ///< The control Escape presses.
    PickControlFn pick_;        ///< Gets the pressed control's id, once closed.
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
void ShowPicture(w2::UiContext& ctx, int popup, int dialog_id,
                 const PictureDialog::ControlIdList& live_ids, int default_id, int cancel_id,
                 PictureDialog::PickControlFn pick) {
    kit::Push(ctx, std::make_shared<PictureDialog>(ctx, popup, dialog_id, live_ids, default_id,
                                                   cancel_id, std::move(pick)));
}

/** @brief Congratulations, then the map shown whole, then `after`. */
void Victory(w2::UiContext& ctx, kit::Done after) {
    ctx.Audio().Music(w2::cues::kTriumph);
    ShowPicture(ctx, 22, 32, {483}, 483, 483, [&ctx, after](int) {
        ctx.ActiveGame().GetMap().MutableOptions().hidden_map = 0;
        ctx.GetViewport().StratDirty();
        search::ShowMessage(ctx, GroupLine(ctx, 0x10, 0), GroupLine(ctx, 0x10, 1), after);
    });
}
}  // namespace

/** @brief A side put out at the round's end: its line of group 11. */
std::string FallenText(const w2::UiContext& ctx, const w2::Fallen& fallen) {
    return w2::Format(GroupLine(ctx, 0xb, fallen.line), fallen.side->Name());
}

namespace {
using FallenLineList = std::vector<std::string>;

/** @brief Shows the two group-13 boxes when the last human side has fallen, then runs `done`. */
void ShowNoHumansMessages(w2::UiContext& ctx, kit::Done done) {
    search::ShowMessage(ctx, GroupLine(ctx, 0xd, 0), GroupLine(ctx, 0xd, 1), [&ctx, done]() {
        search::ShowMessage(ctx, GroupLine(ctx, 0xd, 2), GroupLine(ctx, 0xd, 3), done);
    });
}

/** @brief Shows `lines[line_index]` and chains to the remaining fallen announcements. */
void TellNextFallen(w2::UiContext& ctx, FallenLineList lines, size_t line_index, bool no_humans,
                    kit::Done done) {
    if (line_index < lines.size()) {
        std::string current_line = lines[line_index];
        search::Say(ctx, current_line,
                    [&ctx, remaining = std::move(lines), line_index, no_humans, done]() {
                        TellNextFallen(ctx, remaining, line_index + 1, no_humans, done);
                    });
        return;
    }
    if (no_humans) {
        ShowNoHumansMessages(ctx, std::move(done));
        return;
    }
    if (done) done();
}

/**
 * @brief Tells what the round's end put out and whether the last human fell, then `after`.
 *
 * Each in a box of its own, one after the other; nothing when the computer's
 * turns have told it already.
 */
void Tell(w2::UiContext& ctx, w2::Ending& end_state, kit::Done after) {
    if (end_state.told) {
        if (after) after();
        return;
    }
    end_state.told = true;
    FallenLineList lines;
    lines.reserve(end_state.fallen.size());
    for (const auto& fallen : end_state.fallen) lines.push_back(FallenText(ctx, fallen));
    TellNextFallen(ctx, std::move(lines), 0, end_state.no_humans, std::move(after));
}

/** @brief Resolves the Yes/No choice on the surrender offer picture. */
void HandleSurrenderChoice(w2::UiContext& ctx, int control_id, kit::Done after) {
    if (control_id == 485) {
        w2::game::AcceptSurrender(ctx.ActiveGame(), *ctx.Player());
        Victory(ctx, std::move(after));
    } else {
        ctx.Audio().Music(w2::cues::kDefiance);
        ShowPicture(ctx, 21, 31, {484}, 484, 484, [after](int) {
            if (after) after();
        });
    }
}

/** @brief Shows the second "no more players" message box and returns to the start screens. */
void ShowReturnToStart(w2::UiContext& ctx) {
    search::ShowMessage(ctx, GroupLine(ctx, 0xc, 2), GroupLine(ctx, 0xc, 3),
                        [&ctx]() { ctx.Actions().Quit(); });
}
}  // namespace

/**
 * @brief The game's end, or nearly: told, then `after`.
 *
 * In order: what the round's end put out; a computer side's triumph handed
 * to the player; a pending offer of surrender; the player's own victory.
 */
void Show(w2::UiContext& ctx, w2::Ending& end_state, kit::Done after) {
    auto done = [after]() {
        if (after) after();
    };
    if (!end_state.told) {
        Tell(ctx, end_state, [&ctx, &end_state, after]() { Show(ctx, end_state, after); });
        return;
    }
    // The last computer side has triumphed, and is the player's now.
    if (end_state.triumph && end_state.winner == ctx.Player() && !end_state.shown) {
        end_state.shown = true;
        search::Say(ctx, w2::Format(GroupLine(ctx, 0xf, 0), ctx.Player()->Name()), done);
        return;
    }
    // The offer waits on the game, not on the round's finding: computer
    // turns may have come between.
    if (w2::game::SurrenderPending(ctx.ActiveGame())) {
        ctx.ActiveGame().SetSurrenderShown(true);
        ctx.Audio().Music(w2::cues::kSurrender);
        ShowPicture(ctx, 20, 30, {485, 486}, 485, 486, [&ctx, after](int control_id) {
            HandleSurrenderChoice(ctx, control_id, after);
        });
        return;
    }
    if (end_state.won && end_state.winner == ctx.Player()) {
        search::ShowMessage(ctx, w2::Format(GroupLine(ctx, 0xf, 0), ctx.Player()->Name()),
                            GroupLine(ctx, 0xf, 1), [&ctx, after]() { Victory(ctx, after); });
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
void Over(w2::UiContext& ctx, w2::Ending& end_state) {
    if (!end_state.told && !end_state.fallen.empty()) {
        Tell(ctx, end_state, [&ctx, &end_state]() { Over(ctx, end_state); });
        return;
    }
    if (end_state.winner) {
        search::Say(ctx, w2::Format(GroupLine(ctx, 0xf, 0), end_state.winner->Name()));
        return;
    }
    bool humans = false;
    for (w2::Side* side : ctx.ActiveGame().Sides()) {
        if (side->IsAlive() && !side->IsComputerControlled() &&
            !w2::game::SideCities(ctx.ActiveGame(), *side).empty()) {
            humans = true;
        }
    }
    if (!humans && end_state.message.contains("No more players")) {
        // "return to DOS" -- here, the start screens
        search::ShowMessage(ctx, GroupLine(ctx, 0xc, 0), GroupLine(ctx, 0xc, 1),
                            [&ctx]() { ShowReturnToStart(ctx); });
        return;
    }
    ShowNoHumansMessages(ctx, nullptr);
}

}  // namespace ending
