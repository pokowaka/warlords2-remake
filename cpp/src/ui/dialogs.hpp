// Every dialog's way in. Each lives in a file of its own (love2d/ui/*.lua).
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ui/kit.hpp"
#include "warlords/game.hpp"
#include "warlords/quest.hpp"
#include "warlords/types.hpp"

namespace input {
struct Options {
    std::string title;
    std::vector<std::string> lines;
    std::string text;
    int max_chars = 15, max_width = 232;
    bool cancel = true;    // false greys Cancel
    bool confirm = false;  // a yes-or-no question: no field
    std::function<void(const std::string&)> ok;
    std::function<void()> cancelled;
};
/** Ask for a line of text, or a yes-or-no question (7b4c:0000). */
void Open(const Options& opts);
/** A line being typed into a field (7b4c:03a6). */
struct Editor {
    std::string text;
    int max_chars = 15, max_width = 232;
    /** "keep" for Enter, "undo" for Escape, "" otherwise. */
    std::string Key(const std::string& k);
    void Input(const std::string& t);
    void DrawCursor(double x, double y) const;
};
}  // namespace input

namespace search {
/** A message box (8065:1160); `after` runs when it is closed. */
void ShowMessage(const std::string& line1, const std::string& line2, kit::Done after = nullptr);
/** A one-line message (8065:10fb). */
void Say(const std::string& line, kit::Done after = nullptr);
/** Hero > Search with the selected stack. */
void Open(const std::vector<w2::Army*>& stack);
}  // namespace search

namespace honours {
/** Show each hero promoted -- hero::checkPromotions's -- in its own popup,
 *  then `after`. */
void Promotions(const std::vector<w2::Promotion>& list, kit::Done after);
/** An army's new medal (game::applyAttack's Battle::medal), then `after`. */
void Medal(w2::Army* a, kit::Done after);
}  // namespace honours

namespace choose {
/** The game's list chooser (796c:0000): pick one of `names`, entry `start`
 *  chosen; `after(index or NONE)` runs when it closes. */
void Open(const std::string& title, const std::vector<std::string>& names, int start,
          std::function<void(int)> after);
}  // namespace choose

namespace help {
extern const kit::Rect kPopuP4;
/** Show a help file -- "HELP\HITEM.GFX" -- in popup `R` (or the one its #D
 *  picks); false when there is none. */
bool Open(const std::string& name, kit::Done after = nullptr, const kit::Rect* r = nullptr);
}  // namespace help

namespace infobox {
struct Board {
    int bitmap;
    const char* file;
    int w, h, key;
};
extern const Board kLines, kArmy, kPopuP2;
/** A board, loaded once and keyed on its mask colour. */
gfx::ImageP BoardImage(const Board& which);
/** Where a board goes for a point on the screen (740d:131a), in the dialogs'
 *  frame. */
std::pair<int, int> Place(const Board& which, int sx, int sy);
/** Two lines (740d:1201 and 740d:1158). */
void ShowLines(int sx, int sy, const std::string& title, const std::string& text);
/** An army (ui_army_info, 740d:0626). */
void Army(int sx, int sy, const w2::Army* a);
/** An army type (ui_army_type_info, 740d:032a). */
void ArmyType(int sx, int sy, int type_id, const w2::Slot& s, int side = w2::kNone);
/** The help for control `id` at a point on the screen, asking `d` about
 *  the controls that stand for something. True when a box went up. */
bool ExplainControl(int sx, int sy, int id, kit::Modal* d = nullptr);
/** The control on a dialog's view under a point, disabled ones included. */
const w2::uidata::Control* ControlAt(const screen::View& v, int x, int y,
                                     const std::set<int>* hidden = nullptr);
}  // namespace infobox

namespace advisor {
/** Have him say a FILE.DAT group, and run `after` once he has finished. */
void Say(int group, kit::Done after = nullptr);
}  // namespace advisor

namespace tutorial {
/** Show the page for `moment` once, if the tutorial is on. */
void Show(const std::string& moment, kit::Done after = nullptr);
void Chain(const std::vector<std::string>& moments, kit::Done after = nullptr);
}  // namespace tutorial

namespace about {
void Open();
}
namespace spoils {
void Open(bool sacked, w2::City* city, int gold, const std::vector<std::pair<int, int>>& lost,
          int left, kit::Done after);
}
namespace resign {
void Open();
}
namespace items {
void Open();
}
namespace levels {
void Open();
}
namespace miladvisor {
void Open(const std::vector<w2::Army*>* stack, int tx, int ty);
}
namespace signpost {
void Open(const std::vector<w2::Army*>& stack);
}
namespace questnews {
void Show(std::shared_ptr<w2::QuestResult> news, kit::Done after = nullptr);
/** A quest that ended is told once the screen is the player's again. */
void Poll();
}  // namespace questnews
namespace questui {
void Open();
}
namespace stackui {
void Open();
}
namespace armybonus {
void Open();
/** 89e0:1a07: the first of the type's bonuses, as group 163 words it. */
std::string BonusText(const w2::ArmyType& t, const w2::Army* army = nullptr);
}  // namespace armybonus
namespace city {
constexpr int kInfo = 0, kCity = 1, kProduction = 2, kVector = 3;
/** Production for one of the side's own by default, Info for any other. */
void Open(w2::City* c, int mode = w2::kNone);
}  // namespace city
namespace buyprod {
void Open(w2::City* c, kit::Done after = nullptr);
}
namespace fightorder {
void Open();
}
namespace ending {
/** A side put out at the round's end: a line of group 11 (8065:18ab). */
std::string FallenText(const w2::Fallen& f);
/** The game's end, or nearly: told, then `after`. */
void Show(w2::Ending& e, kit::Done after);
void Over(w2::Ending& e);
}  // namespace ending
namespace diplomacyui {
void Open();
/** Diplomatic Action, the shield button. */
void Action();
/** Which face the shield button shows: 183, 184 or 185. */
int ButtonFor(w2::Game& g, int side);
/** Open Parley & Attitude Ledger for a specific side. */
void OpenParley(int other_side, bool return_to_action = false);
/** Show any queued Turn-Start Envoy Missives for the current human player. */
void PollMissives(kit::Done after = nullptr);
/** Open the F10 AI Brain & Neural Policy Inspector modal. */
void OpenBrainInspector(int initial_side = w2::kNone);
}  // namespace diplomacyui
namespace heroinfo {
void Open();
}
namespace historyui {
void Open(int which);
void Triumphs();
}  // namespace historyui
namespace reports {
void Open(int which);
}
namespace ruin {
w2::Site* Nearest(int x, int y);
void Open(w2::Site* s);
}  // namespace ruin
namespace savegame {
void Save();
void Load(std::function<void(std::unique_ptr<w2::Game>)> after);
int Used();
bool WriteSlot(const std::string& key, const w2::Game& g);
std::unique_ptr<w2::Game> ReadSlot(const std::string& key);
}  // namespace savegame
namespace settings {
/** `after` runs when OK is pressed. */
void Open(kit::Done after = nullptr);
}  // namespace settings
namespace shortcuts {
void Open();
}
namespace tileinfo {
void Open(int tx, int ty);
}
namespace start {
/** The start screens (7f77:0000): a new game set up there, or a saved one. */
void Open(std::function<void(const std::string& dir, const w2::game::NewGameOptions& opts)> begin,
          std::function<void(std::unique_ptr<w2::Game>)> loaded);
}  // namespace start
