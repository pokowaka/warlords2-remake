// Every dialog's entry point. Each lives in a file of its own (love2d/ui/*.lua).
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
/**
 * @brief Configuration for the text-entry and confirmation dialog.
 */
struct Options {
    std::string title;                           ///< Dialog heading text.
    std::vector<std::string> lines;              ///< Prompt lines shown above the field.
    std::string text;                            ///< Initial text in the input field.
    int max_chars = 15;                          ///< Maximum character count allowed.
    int max_width = 232;                         ///< Maximum pixel width of typed text.
    bool cancel = true;                          ///< False disables the Cancel button.
    bool confirm = false;                        ///< True shows a yes/no prompt with no text field.
    std::function<void(const std::string&)> ok;  ///< Callback invoked on confirmation.
    std::function<void()> cancelled;             ///< Callback invoked on cancellation.
};
/** @brief Opens the text-entry or yes/no confirmation dialog. */
void Open(const Options& opts);
/**
 * @brief Single-line text editor state used by input fields.
 */
struct Editor {
    std::string text;     ///< Current edited string.
    int max_chars = 15;   ///< Maximum character count allowed.
    int max_width = 232;  ///< Maximum pixel width allowed.
    /** @brief Handles a key press; returns "keep" for Enter, "undo" for Escape, "" otherwise. */
    std::string Key(const std::string& key);
    /** @brief Appends printable characters from `typed` while within length and width limits. */
    void Input(const std::string& typed);
    /** @brief Draws the blinking backtick cursor at the end of the current text. */
    void DrawCursor(double x, double y) const;
};
}  // namespace input

namespace search {
/** @brief Shows a two-line message popup; `after` runs when closed. */
void ShowMessage(const std::string& line1, const std::string& line2, kit::Done after = nullptr);
/** @brief Shows a one-line message popup; `after` runs when closed. */
void Say(const std::string& line, kit::Done after = nullptr);
/** @brief Opens Hero > Search with the selected stack. */
void Open(const std::vector<w2::Army*>& stack);
}  // namespace search

namespace honours {
/** @brief Shows each promoted hero in its own popup, then runs `after`. */
void Promotions(const std::vector<w2::Promotion>& list, kit::Done after);
/** @brief Shows an army's new medal popup, then runs `after`. */
void Medal(w2::Army* army, kit::Done after);
}  // namespace honours

namespace choose {
/**
 * @brief Opens the list chooser dialog with `start` initially selected;
 * `after(index or kNone)` runs when it closes.
 */
void Open(const std::string& title, const std::vector<std::string>& names, int start,
          std::function<void(int)> after);
}  // namespace choose

namespace help {
extern const kit::Rect kPopuP4;
/**
 * @brief Shows a help file (such as "HELP\\HITEM.GFX") in `popup_rect` (or the
 * popup specified by its `#D` header); returns false when missing.
 */
bool Open(const std::string& name, kit::Done after = nullptr,
          const kit::Rect* popup_rect = nullptr);
}  // namespace help

namespace infobox {
/**
 * @brief Metadata for an info-box background bitmap in `MARBLE.PCK` / `POPUP*.PCK`.
 */
struct Board {
    int bitmap;        ///< Popup bitmap index.
    const char* file;  ///< Asset file path.
    int w;             ///< Width in pixels.
    int h;             ///< Height in pixels.
    int key;           ///< Transparent colour key index.
};
extern const Board kLines, kArmy, kPopuP2;
/** @brief Loads and caches an info-box board keyed on its mask colour. */
gfx::ImageP BoardImage(const Board& which);
/** @brief Computes the dialog-frame position of `which` for a screen point. */
std::pair<int, int> Place(const Board& which, int screen_x, int screen_y);
/** @brief Shows a two-line info popup while the mouse button is held. */
void ShowLines(int screen_x, int screen_y, const std::string& title, const std::string& text);
/** @brief Shows an army inspection popup while the mouse button is held. */
void Army(int screen_x, int screen_y, const w2::Army* army);
/** @brief Shows an army-type inspection popup while the mouse button is held. */
void ArmyType(int screen_x, int screen_y, int type_id, const w2::Slot& slot, int side = w2::kNone);
/**
 * @brief Shows the right-click help box for control `control_id`, delegating
 * dynamic sub-controls to `modal` when present. Returns true if a box opened.
 */
bool ExplainControl(int screen_x, int screen_y, int control_id, kit::Modal* modal = nullptr);
/** @brief Finds the control in `view` under `(x, y)`, including disabled controls. */
const w2::uidata::Control* ControlAt(const screen::View& view, int x, int y,
                                     const std::set<int>* hidden = nullptr);
}  // namespace infobox

namespace advisor {
/** @brief Speaks a `FILE.DAT` advisor group and runs `after` when finished. */
void Say(int group, kit::Done after = nullptr);
}  // namespace advisor

namespace tutorial {
/** @brief Shows the tutorial page for `moment` once, when the tutorial is enabled. */
void Show(const std::string& moment, kit::Done after = nullptr);
/** @brief Shows a sequence of tutorial moments in order, then runs `after`. */
void Chain(const std::vector<std::string>& moments, kit::Done after = nullptr);
}  // namespace tutorial

namespace about {
/** @brief Opens the About dialog. */
void Open();
}  // namespace about
namespace spoils {
/** @brief Opens the city capture spoils / pillage / sack summary dialog. */
void Open(bool sacked, w2::City* city, int gold, const std::vector<std::pair<int, int>>& lost,
          int left, kit::Done after);
}  // namespace spoils
namespace resign {
/** @brief Opens the resignation confirmation dialog. */
void Open();
}  // namespace resign
namespace items {
/** @brief Opens the carried items dialog for heroes on the selected tile. */
void Open();
}  // namespace items
namespace levels {
/** @brief Opens the AI difficulty levels dialog. */
void Open();
}  // namespace levels
namespace miladvisor {
/** @brief Opens the Military Advisor odds assessment for attacking `(tile_x, tile_y)`. */
void Open(const std::vector<w2::Army*>* stack, int tile_x, int tile_y);
}  // namespace miladvisor
namespace signpost {
/** @brief Opens the signpost / ruin / temple inspection dialog for `stack`. */
void Open(const std::vector<w2::Army*>& stack);
}  // namespace signpost
namespace questnews {
/** @brief Shows a quest completion or failure announcement, then runs `after`. */
void Show(std::shared_ptr<w2::QuestResult> news, kit::Done after = nullptr);
/** @brief Shows any pending quest news once control returns to the player. */
void Poll();
}  // namespace questnews
namespace questui {
/** @brief Opens the active quest status dialog. */
void Open();
}  // namespace questui
namespace stackui {
/** @brief Opens the stack inspection dialog. */
void Open();
}  // namespace stackui
namespace armybonus {
/** @brief Opens the Army Bonus report dialog. */
void Open();
/** @brief Formats the primary combat bonus description for `army_type` (or `army`). */
std::string BonusText(const w2::ArmyType& army_type, const w2::Army* army = nullptr);
}  // namespace armybonus
namespace city {
constexpr int kInfo = 0, kCity = 1, kProduction = 2, kVector = 3;
/** @brief Opens the city dialog (Production by default for own cities, Info otherwise). */
void Open(w2::City* target_city, int mode = w2::kNone);
}  // namespace city
namespace buyprod {
/** @brief Opens the Buy Production dialog for `target_city`. */
void Open(w2::City* target_city, kit::Done after = nullptr);
}  // namespace buyprod
namespace fightorder {
/** @brief Opens the Fighting Order dialog. */
void Open();
}  // namespace fightorder
namespace ending {
/** @brief Formats the elimination announcement line for `fallen`. */
std::string FallenText(const w2::Fallen& fallen);
/** @brief Shows end-of-round eliminations and victory/defeat dialogs, then runs `after`. */
void Show(w2::Ending& end_state, kit::Done after);
/** @brief Opens the final victory or defeat screen for `end_state`. */
void Over(w2::Ending& end_state);
}  // namespace ending
namespace diplomacyui {
/** @brief Opens the Diplomacy state matrix dialog. */
void Open();
/** @brief Opens the Diplomatic Action dialog from the shield button. */
void Action();
/** @brief Returns the shield control ID (183, 184, or 185) for `side` in `game`. */
int ButtonFor(w2::Game& game, int side);
/** @brief Opens the Parley & Attitude Ledger for `other_side`. */
void OpenParley(int other_side, bool return_to_action = false);
/** @brief Shows any queued Turn-Start Envoy Missives for the current human player. */
void PollMissives(kit::Done after = nullptr);
/** @brief Opens the F10 AI Brain & Neural Policy Inspector modal. */
void OpenBrainInspector(int initial_side = w2::kNone);
}  // namespace diplomacyui
namespace heroinfo {
/** @brief Opens the Hero inspection and item management dialog. */
void Open();
}  // namespace heroinfo
namespace historyui {
/** @brief Opens the History dialog on tab `which` (0..3). */
void Open(int which);
/** @brief Opens the History > Triumphs tally dialog. */
void Triumphs();
}  // namespace historyui
namespace reports {
/** @brief Opens the Reports dialog on tab `which` (0..4). */
void Open(int which);
}  // namespace reports
namespace ruin {
/** @brief Finds the nearest unsearched ruin to `(x, y)`. */
w2::Site* Nearest(int x, int y);
/** @brief Opens the Ruin Map dialog centred on `site`. */
void Open(w2::Site* site);
}  // namespace ruin
namespace savegame {
/** @brief Opens the Save Game dialog. */
void Save();
/** @brief Opens the Load Game dialog and passes the loaded game to `after`. */
void Load(std::function<void(std::unique_ptr<w2::Game>)> after);
/** @brief Returns the last save slot used (0..7). */
int Used();
/** @brief Writes `game` to save slot `key`. */
bool WriteSlot(const std::string& key, const w2::Game& game);
/** @brief Reads a saved game from slot `key`, or returns nullptr on failure. */
std::unique_ptr<w2::Game> ReadSlot(const std::string& key);
}  // namespace savegame
namespace settings {
/** @brief Opens the Game Settings dialog; `after` runs when OK is pressed. */
void Open(kit::Done after = nullptr);
}  // namespace settings
namespace shortcuts {
/** @brief Opens the Customize Shortcuts dialog. */
void Open();
}  // namespace shortcuts
namespace tileinfo {
/** @brief Opens the Hat / Tile Info inspection popup for `(tile_x, tile_y)`. */
void Open(int tile_x, int tile_y);
}  // namespace tileinfo
namespace start {
/** @brief Opens the start screens to begin a new scenario or load a saved game. */
void Open(std::function<void(const std::string& dir, const w2::game::NewGameOptions& opts)> begin,
          std::function<void(std::unique_ptr<w2::Game>)> loaded);
}  // namespace start
