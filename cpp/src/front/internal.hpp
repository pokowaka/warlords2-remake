/**
 * @file
 * @brief Internal helpers, constants, and cross-module functions shared by `cpp/src/front/`.
 */
#pragma once

#include <array>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <SDL.h>

#include "front/app.hpp"
#include "platform/coroutine.hpp"
#include "platform/gfx.hpp"
#include "platform/keys.hpp"
#include "platform/sound.hpp"
#include "ui/kit.hpp"
#include "util/util.hpp"
#include "warlords/game.hpp"
#include "warlords/uidata.hpp"

namespace front {

constexpr int kTile = screen::kTile;    ///< Tile size in UI pixels (40x40).
constexpr double kCursRate = 18.2 / 4;  ///< CURS.PCK animation rate in frames per second.
constexpr int kSlotFirst = 224;         ///< First army slot control ID in the bottom bar.
constexpr int kBarFirst = 232;          ///< First moves-left bar control ID.
constexpr int kSlotCount = 8;           ///< Number of army slots in the bottom bar.
constexpr int kGrpAll = 240;            ///< Control ID for the Group All button.
constexpr int kGrpNone = 241;           ///< Control ID for the Group None button.
constexpr int kHeroOk = 287;            ///< Control ID for the Hero Offer Accept button.
constexpr int kHeroCancel = 288;        ///< Control ID for the Hero Offer Reject button.
constexpr int kHeroField = 289;         ///< Control ID for the Hero Offer name field.
constexpr int kHeroMale = 290;          ///< Control ID for the Hero Offer Male checkbox.
constexpr int kHeroFemale = 291;        ///< Control ID for the Hero Offer Female checkbox.
constexpr size_t kHeroNameMax = 19;     ///< Maximum hero name length in the offer dialog.

/**
 * @brief The twelve mouse pointers of STAND.PCK, in sheet order.
 *
 * The original decides the mouse pointer from what is under it and what is
 * selected, and a click on the map does whatever that pointer promises.
 */
enum Ptr {
    kArrow,   ///< Nothing to do here: menus, dialogs, the computer's turn.
    kView,    ///< Over the strategic map: moves the view.
    kBoat,    ///< Walks the selection onto the water.
    kCityP,   ///< Looks at a city.
    kHand,    ///< Nothing to do on this tile.
    kSelect,  ///< Selects the player's stack here.
    kWalk,    ///< Walks the selection here.
    kSiteP,   ///< Looks at a site (or a razed city).
    kAttack,  ///< Attacks the enemy here.
    kAdvise,  ///< Asks the military advisor about the enemy here.
    kPeace,   ///< The enemy here is not at war: attacking means declaring it.
    kAlt      ///< Alt held: plans a route without walking it.
};

/** @brief The quad of terrain tile `tile` in its scenery sheet (96 a sheet, 16 a row). */
inline gfx::Quad TileQuad(int tile) {
    int cell = tile % 96;
    return gfx::NewQuad((cell % 16) * kTile, (cell / 16) * kTile, kTile, kTile);
}

/** @brief The quad of road piece `road` in ROAD.PCK (13 a row). */
inline gfx::Quad RoadQuad(int road) {
    return gfx::NewQuad((road % 13) * 48, (road / 13) * kTile, kTile, kTile);
}

/** @brief Sets the drawing colour to palette entry `colour`. */
inline void PalColour(int colour) {
    kit::SetPal(colour);
}

/**
 * @brief Whether any of the named keys is held.
 *
 * Uses only keys tracked by `keys::IsDown` (cleared on focus loss) rather than
 * `SDL_GetModState` so phantom modifiers from window switches cannot stick.
 */
inline bool Held(std::initializer_list<const char*> key_names) {
    return keys::IsDown(key_names);
}

/** @brief A side index for the art: no owner (`kNone`) is the neutral's 8. */
inline int OwnerOr8(int owner) {
    return owner == w2::kNone ? 8 : owner;
}

extern int opt_military_advisor;  ///< CLI override for militaryAdvisor (-1 for scenario default).
extern int strat_scroll;          ///< Vertical scroll offset of the strategic map on short screens.
extern bool offer_typing;         ///< Whether the hero offer name field was clicked for typing.
extern Coroutine* ai_co;          ///< Active AI turn coroutine while executing inside a step.

// camera.cpp
/** @brief Returns how many tiles fit across and down the map viewport at the current zoom. */
std::pair<double, double> ViewSize();
/** @brief Returns the camera's sub-tile offset in device pixels for smooth map scrolling. */
std::pair<int, int> CamDev();
/** @brief Converts UI pixel coordinates `(x, y)` to fractional map tile coordinates. */
std::pair<double, double> UiToMap(double x, double y);
/** @brief Returns the inclusive tile bounds `(x0, y0, x1, y1)` visible in the map viewport. */
std::array<int, 4> ViewTiles();
/** @brief Returns whether tile `(x, y)` is currently within the visible map viewport. */
bool Shown(int x, int y);
/** @brief Returns whether tile `(x, y)` lies safely inside the viewport's inner margin. */
bool InView(int x, int y);
/** @brief Clamps the camera position `(g_app.cx, g_app.cy)` to the map bounds. */
void ClampCamera();
/** @brief Zooms the map to `zoom`, keeping the map point under UI `(x, y)` (or centre) fixed. */
void SetZoomAt(int zoom, double x, double y, bool centre = false);
/** @brief Sets the UI scale factor (`0` auto, `display::kSmooth` smooth, or integer scale). */
void SetUiScale(int scale);
/** @brief Enables or disables the original 640x480 4:3 aspect lock. */
void SetOriginalSize(bool enabled);
/** @brief Switches between windowed (`window == true`) and fullscreen display modes. */
void SetScreen(bool window);
/** @brief Toggles the CRT shader effect and persists the preference. */
void ToggleCrt();
/** @brief Switches between the classic 1993 AI and the modern AI. */
void SetAiMode(bool classic);
/** @brief Returns whether the menu item with key `key` should show a checkmark. */
bool MenuTicked(const std::string& key);

// walk.cpp
/** @brief Recomputes the route preview for the selected stack's current target. */
void RefreshRoute();
/** @brief Assigns movement orders towards `(x, y)` for every army in `stack`. */
void OrderTo(const std::vector<w2::Army*>& stack, int x, int y);
/** @brief Plans and previews a route to `(x, y)` for the selected stack without moving. */
void PlanRoute(int x, int y);
/** @brief Starts the step-by-step walk animation for `armies` along `tiles`. */
void StartWalk(const std::vector<w2::Army*>& armies, const std::vector<std::pair<int, int>>& tiles,
               bool sea_before = false);
/** @brief Marks every army in `stack` as done for the turn and deselects them. */
void PutDown(const std::vector<w2::Army*>& stack);
/** @brief Advances an active walk animation by wall-clock time. */
void AdvanceWalk();
/** @brief Returns the current tile of the walking stack, or `nullptr` if no walk is active. */
const std::pair<int, int>* WalkingAt();
/** @brief Draws the planned route dots and target X on the map. */
void DrawRoute();
/** @brief Converts a UI pixel point `(point_x, point_y)` in the map viewport to a tile. */
std::optional<std::pair<int, int>> TileAtPoint(int point_x, int point_y);

// selection.cpp
/** @brief Returns the current player's selectable armies on tile `(x, y)`. */
std::vector<w2::Army*> SelectableAt(int x, int y);
/** @brief Synchronizes `g_app.selection->stack` with the active slots in the bottom bar. */
void SyncSelection();
/** @brief Rebuilds the bottom bar slots for `armies` while keeping previously chosen members. */
void Reslot(const std::vector<w2::Army*>& armies);
/** @brief Drowns any armies stranded on water without a boat and shows the drowning popup. */
bool DrownStranded(int x = w2::kNone, int y = w2::kNone, kit::Done after = nullptr);
/** @brief Selects the player's stack at tile `(x, y)`, optionally picking only `pick`. */
void Select(int x, int y, w2::Army* pick = nullptr);
/** @brief Returns the capital coordinates of `side` (or `(0, 0)` if none). */
std::pair<int, int> CapOf(const w2::Side* side);
/** @brief Resets the `offered` cycle flags on all of the current player's armies. */
void CycleReset();
/** @brief Selects the next movable army stack in the cycle order. */
void SelectNext();
/** @brief Marks the selected stack as done for the turn (`q` / Quit command). */
void QuitArmy();
/** @brief Fortifies the selected stack (`s` / Defend command) and moves to the next stack. */
void Fortify();
/** @brief Handles a click with the select pointer on tile `(tile_x, tile_y)`. */
void SelectClick(int tile_x, int tile_y);
/** @brief Clears the current stack selection and route preview. */
void Deselect();

// pointer.cpp
/** @brief Returns the `Ptr` cursor kind for UI point `(x, y)`. */
int PointerKind(int x, int y);
/** @brief Draws the custom STAND.PCK mouse pointer at the current cursor position. */
void DrawPointer();

// turn.cpp
/** @brief Attacks the enemy stack or city on adjacent tile `(x, y)` with the selected stack. */
void AttackTile(int x, int y);
/** @brief Walks the selected stack along its planned route toward `(x, y)`. */
void MoveSelection(int x, int y);
/** @brief Shows the start-of-turn banner for `side` and plays the turn fanfare. */
void ShowBanner(w2::Side* side);
/** @brief Returns whether hotseat hidden-map mode should blank the map until the banner closes. */
bool HotseatHidden();
/** @brief Dismisses the start-of-turn banner and proceeds to drowning checks and advisor cues. */
void DismissBanner();
/** @brief Accepts the active hero offer using the name and gender in the dialog. */
void AcceptOffer();
/** @brief Refuses the active hero offer and continues the turn opening sequence. */
void RefuseOffer();
/** @brief Draws the start-of-turn banner popup. */
void DrawBanner();
/** @brief Draws the hero-offer popup dialog. */
void DrawHeroOffer();
/** @brief Starts turn 1 of a newly created game and centres the camera on the capital. */
void BeginGame();
/** @brief Starts a scenario specified on the command line (`--scenario NAME`). */
void StartCliScenario(const std::string& scenario, double seed, bool force_diplomacy,
                      bool force_hidden_map, bool spectate_all_ai);

// computer.cpp
/** @brief Returns whether any human-controlled side is still alive in the game. */
bool HumansLeft();
/** @brief Installs the AI movement, battle, status, and diplomacy hooks for visual playback. */
void InstallHooks();
/** @brief Resumes the paused AI turn coroutine by one step. */
void ResumeComputer();
/** @brief Checks timers and advances the AI turn coroutine once per frame when ready. */
void StepComputer();
/** @brief Starts the AI turn coroutine beginning at computer side `side`. */
void PlayComputers(w2::Side* side);
/** @brief Ends the current human player's turn and transitions to the next side. */
void EndTurn();

// draw.cpp
/** @brief Loads all .PCK spritesheets, fonts, and the main screen layout from `g_app.data_dir`. */
void LoadArt();
/** @brief Displays status-bar information for a click at `(x, y)` in the bottom bar. */
void BarInfo(int x, int y);
/** @brief Updates sound playback and the top modal dialog once per frame. */
void Update();
/** @brief Renders one complete frame of the main screen, map, modals, and pointer. */
void Draw();

// assault.cpp
/** @brief Hurries or closes the active assault animation when clicked or a key is pressed. */
void PressAssault();
/** @brief Draws the initial WAR.PCK fire cloud over the assaulted tile on the map. */
void DrawCloud();
/** @brief Draws the battle window with attacker and defender lines and casualties. */
void DrawAssault();
/** @brief Draws the city-captured victory popup for `vic`. */
void DrawVictory(const Victory& vic);
/** @brief Executes the chosen city-capture action (`what`: Occupy, Pillage, Sack, or Raze). */
void TakeCity(int what);

// input.cpp
/** @brief Replays any keys buffered while a stack walk animation was in progress. */
void FlushKeys();
/** @brief Handles a mouse button press at UI pixel `(x, y)`. */
void Mousepressed(int x, int y, int button);
/** @brief Advances the Move All sequence to the next stack with a planned route. */
void MoveAllStep();
/** @brief Updates the enabled/disabled/active state of the main screen's buttons. */
void RefreshControls();
/** @brief Handles mouse pointer movement by `(dx, dy)` UI pixels (including map dragging). */
void Mousemoved(double dx, double dy);
/** @brief Handles mouse wheel scrolling (`wheel_y > 0` zoom in, `wheel_y < 0` zoom out). */
void Wheelmoved(int wheel_y);
/** @brief Handles a mouse button release at UI pixel `(x, y)`. */
void Mousereleased(int x, int y, int button);
/** @brief Handles a keyboard press identified by LÖVE key name `key`. */
void Keypressed(const std::string& key);
/** @brief Appends typed Latin-1 characters `text` to the active text input field. */
void Textinput(const std::string& text);
/** @brief Translates a gamepad button event (`button`, `down`) into mouse or key input. */
void PadButton(int button, bool down);
/** @brief Releases any held virtual mouse buttons when the gamepad disconnects or focus is lost. */
void PadLetGo();
/** @brief Polls gamepad triggers and analog sticks once per frame. */
void PadUpdate();
/** @brief Synchronizes SDL text input and on-screen keyboard state with active text fields. */
void SyncTyping();
/** @brief Initializes mouse and gamepad pointer coordinates from the SDL window state. */
void InitPointerState(SDL_Window* win);
/** @brief Processes an `SDL_MOUSEMOTION` event at window coordinates `(win_x, win_y)`. */
void HandleMouseMotion(int win_x, int win_y);
/** @brief Polls global mouse position while `win` has keyboard focus (workaround for SDL3/macOS).
 */
void PollFocusedMouse(SDL_Window* win);

// script.cpp
/** @brief Loads a UI test script file from `path` for frame-by-frame execution. */
void LoadScript(const std::string& path);
/** @brief Returns whether a UI test script is currently loaded. */
bool HasScript();
/** @brief Executes commands from the active UI test script until a `wait` or `quit`. */
void StepScript();

}  // namespace front
