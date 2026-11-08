// The end of the game (8065:1aed and the screens it leads to).
//
// The three pictures are popups 20-22, (136, 40) 368x390, each its own
// bitmap with the words painted in:
//   20 RESIGN.PCK   "An offer of Peace" -- dialog 30: No (486), Yes (485)
//   21 RESIGNNO.PCK "Peace is not an option!" -- dialog 31: Done (484)
//   22 RESIGNYE.PCK "Congratulations!" -- dialog 32: Done (483)
// Each comes with its own music.
//
// The round's end puts out the sides left without a city first, each told
// with a line of group 11 -- in a box, or in the status bar when no human
// plays (8065:18ab) -- and says once when the last human has fallen (group
// 13). When the last computer side has triumphed (group 15) it is handed to
// the player, its turn opening as a human's, to look the world over.
#include "platform/sound.hpp"
#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/cues.hpp"

namespace ending {

namespace {
const kit::Rect kR{136, 40, 368, 390};  // popups 20-22
int Picture(int popup) {
    return popup == 20 ? 12 : popup == 21 ? 14 : 13;
}  // FILE.DAT group 3
std::string T(int g, int i) {
    return kit::Text(g, i);
}

struct PictureDialog : kit::Modal {
    int popup = 20;
    screen::View v;
    int dflt = 0, cancel = 0;
    std::function<void(int)> pick;
    void Draw() override {
        kit::PopupFrame(kR);
        if (auto art = G.screen->ArtFor(Picture(popup))) {
            gfx::SetColor(1, 1, 1);
            gfx::Draw(art->image, gfx::NewQuad(0, 0, kR.w, kR.h), kR.x, kR.y);
        }
        kit::DrawControls(v);
    }
    void Press(int id) {
        auto p = pick;
        kit::Pop(this);
        p(id);
    }
    void Mousepressed(int x, int y, int) override {
        if (auto c = kit::ControlAt(v, x, y)) Press(c->id);
    }
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter")
            Press(dflt);
        else if (key == "escape")
            Press(cancel);
    }
};

void ShowPicture(int popup, int dialog, std::vector<int> ids, int dflt, int cancel,
                 std::function<void(int)> pick) {
    auto d = std::make_shared<PictureDialog>();
    d->popup = popup;
    d->v = kit::View(dialog);
    d->view = &d->v;
    for (int id : ids) d->v.state[id] = w2::uidata::kNormal;
    d->dflt = dflt;
    d->cancel = cancel;
    d->pick = pick;
    kit::Push(d);
}

// Congratulations (8065:1fbd), then the map shown whole (8065:2004).
void Victory(kit::Done after) {
    sound::Music(w2::cues::kTriumph);
    ShowPicture(22, 32, {483}, 483, 483, [after](int) {
        G.g->map->options.hidden_map = 0;
        front::StratDirty();
        search::ShowMessage(T(0x10, 0), T(0x10, 1), after);
    });
}
}  // namespace

std::string FallenText(const w2::Fallen& f) {
    return w2::Format(T(0xb, f.line), f.side->name);
}

namespace {
// What the round's end put out and whether the last human fell, in boxes,
// unless the computer's turns have told it already; then `after`.
void Tell(w2::Ending& e, kit::Done after) {
    auto done = [after]() {
        if (after) after();
    };
    if (e.told) return done();
    e.told = true;
    std::vector<std::string> lines;
    for (auto& f : e.fallen) lines.push_back(FallenText(f));
    bool no_humans = e.no_humans;
    auto next = std::make_shared<std::function<void(size_t)>>();
    std::weak_ptr<std::function<void(size_t)>> self = next;
    *next = [lines, no_humans, done, self](size_t i) {
        if (i < lines.size()) {
            auto again = self.lock();
            search::Say(lines[i], [again, i]() { (*again)(i + 1); });
            return;
        }
        if (no_humans) {
            search::ShowMessage(T(0xd, 0), T(0xd, 1),
                                [done]() { search::ShowMessage(T(0xd, 2), T(0xd, 3), done); });
            return;
        }
        done();
    };
    (*next)(0);
}
}  // namespace

void Show(w2::Ending& e, kit::Done after) {
    auto done = [after]() {
        if (after) after();
    };
    if (!e.told) {
        Tell(e, [&e, after]() { Show(e, after); });
        return;
    }
    // the last computer side has triumphed, and is the player's now
    if (e.triumph && e.winner == G.player && !e.shown) {
        e.shown = true;
        search::Say(w2::Format(T(0xf, 0), G.player->name), done);
        return;
    }
    // the offer waits on the game, not on the round's finding: computer
    // turns may have come between (8cc6:04bd)
    if (w2::game::SurrenderPending(*G.g)) {
        G.g->surrender_shown = true;         // 2c04:015f
        sound::Music(w2::cues::kSurrender);  // 8065:1f68
        ShowPicture(20, 30, {485, 486}, 485, 486, [after, done](int id) {
            if (id == 485) {
                w2::game::AcceptSurrender(*G.g, *G.player);
                Victory(after);
            } else {
                sound::Music(w2::cues::kDefiance);  // 8065:1ecd
                ShowPicture(21, 31, {484}, 484, 484, [done](int) { done(); });
            }
        });
        return;
    }
    if (e.won && e.winner == G.player) {
        search::ShowMessage(w2::Format(T(0xf, 0), G.player->name), T(0xf, 1),
                            [after]() { Victory(after); });
        return;
    }
    done();
}

void Over(w2::Ending& e) {
    if (!e.told && !e.fallen.empty()) {
        Tell(e, [&e]() { Over(e); });
        return;
    }
    if (e.winner) {
        search::Say(w2::Format(T(0xf, 0), e.winner->name));
        return;
    }
    bool humans = false;
    for (w2::Side* s : G.g->sides)
        if (s->alive && !s->computer && !w2::game::SideCities(*G.g, *s).empty()) humans = true;
    if (!humans && w2::Contains(e.message, "No more players")) {
        search::ShowMessage(T(0xc, 0), T(0xc, 1), []() {
            // "return to DOS" -- here, the start screens
            search::ShowMessage(T(0xc, 2), T(0xc, 3), []() { front::Quit(); });
        });
        return;
    }
    search::ShowMessage(T(0xd, 0), T(0xd, 1), []() { search::ShowMessage(T(0xd, 2), T(0xd, 3)); });
}

}  // namespace ending
