/**
 * @file
 * @brief Main map screen controller (`w2::MapScreenController`) implementing `w2::IScreenActions`
 * and coordinating map rendering, input routing, turn flow, and `IMapScreenState` transitions.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"

#include "front/map_screen_state.hpp"
#include "ui/kit.hpp"
#include "ui/screen_actions.hpp"
#include "ui/ui_context.hpp"
#include "warlords/move.hpp"

struct SDL_Window;

namespace w2 {

class ScriptPlayer;

/**
 * @brief Owns main-screen interaction state, active `Game` lifetime, explicit `IMapScreenState`,
 * menu/control dispatch tables, and map/chrome rendering.
 *
 * Implements `IScreenActions` so modal dialogs in `//cpp:ui_dialogs` invoke main-screen actions
 * and shared strategic-map renderers through `UiContext::Actions()` with zero circular dependency.
 *
 * Invariants:
 * - `state_` is always non-null (`HumanTurnState` by default).
 * - `ctx_->GamePtr() == owned_game_.get()` whenever `ctx_` is bound.
 * - `ShutdownSession()` stops any background `AiTurnRunner` and clears all open modals before
 *   destroying `owned_game_`.
 *
 * Thread safety: Main-thread only.
 */
class MapScreenController final : public IScreenActions {
  public:
    using StatePtr = std::unique_ptr<IMapScreenState>;
    using CoordPair = std::pair<int, int>;
    using ArmyList = std::vector<Army*>;
    using ActionFn = std::function<void()>;
    using ControlActionMap = absl::flat_hash_map<int, ActionFn>;
    using MenuActionMap = absl::flat_hash_map<std::string, ActionFn>;

    static constexpr int kTile = screen::kTile;
    static constexpr double kCursRate = 18.2 / 4.0;
    static constexpr int kSlotFirst = 224;
    static constexpr int kBarFirst = 232;
    static constexpr int kSlotCount = 8;
    static constexpr int kGrpAll = 240;
    static constexpr int kGrpNone = 241;

    MapScreenController();
    ~MapScreenController() override;

    MapScreenController(const MapScreenController&) = delete;
    MapScreenController& operator=(const MapScreenController&) = delete;

    /** @brief Binds `ctx` and `script_player` and populates control/menu dispatch tables. */
    void BindContext(UiContext& ctx, ScriptPlayer& script_player);

    /** @brief Stops any active AI worker and clears open modals before teardown. */
    void ShutdownSession();

    UiContext& Ctx() const;
    bool HasScript() const;

    IMapScreenState& State() { return *state_; }
    const IMapScreenState& State() const { return *state_; }
    void SetState(StatePtr next_state);
    StatePtr TakeState();

    // --- IScreenActions implementation ---
    void OpenCity(City* city) override;
    void OpenQuest() override;
    void OpenStart() override;
    void TakeLoaded(std::unique_ptr<Game> game) override;
    void StartAssault(int x, int y, const Battle& result) override;
    void PresentVictory(City* city, const std::string& victor) override;
    void SelectAt(int x, int y) override;
    void AfterSlotChange() override;
    void PlayComputer() override;
    void Say(const std::string& message) override;
    void Quit() override;
    int Roll(int n) override;
    bool MenuEnabled(const std::string& menu_key) override;
    void DrawStrategicMap(int x, int y, const City* mark = nullptr, bool no_cities = false,
                          const OwnerOverrideList* owners = nullptr, int scroll = 0,
                          int rows = 312) override;
    void DrawSiteMarkers(int x, int y, const Site* highlight = nullptr) override;
    void DrawVectorMap(int x, int y, City* city, int filter, bool see_all) override;
    void DrawHeroFigure(int x, int y, int tile_x, int tile_y, bool dark = false) override;
    void DrawStrategicPanel(int x, int y, const City* city) override;

    // --- Startup & Turn Flow ---
    absl::Status StartCliScenario(const std::string& scenario, double seed, bool force_diplomacy,
                                  bool force_hidden_map, bool spectate_all_ai);
    void BeginGame();
    void AdoptGame(std::unique_ptr<Game> game);
    void ShowBanner(Side* side);
    void DismissBanner();
    void AfterOffer();
    bool HotseatHidden() const;
    bool HumansLeft() const;
    void PlayComputers(Side* first_side);
    void EndTurn();
    void FinishRound(Side* next_side);
    void AutoSave();

    // --- State Transitions for Walk / Assault / Drag ---
    void EnterWalk(WalkSpec spec);
    void EnterAssault(AssaultSpec spec);
    void EnterDrag();

    // --- Selection, Cycle, Movement & Combat ---
    ArmyList SelectableAt(int x, int y) const;
    void SyncSelection();
    void Reslot(const ArmyList& armies);
    bool DrownStranded(int x = kNone, int y = kNone, kit::Done after = nullptr);
    void Select(int x, int y, Army* pick = nullptr);
    CoordPair CapOf(const Side* side) const;
    void CycleReset();
    void SelectNext();
    void QuitArmy();
    void Fortify();
    void SelectClick(int tile_x, int tile_y);
    void Deselect();
    void RefreshRoute();
    void OrderTo(const ArmyList& stack, int x, int y);
    void PlanRoute(int x, int y);
    void PutDown(const ArmyList& stack);
    void AttackTile(int x, int y);
    void MoveSelection(int x, int y);
    void MoveAll();
    void MoveAllStep();
    bool HasMoveAll() const { return move_all_.has_value(); }

    // --- Viewport & Preference Actions ---
    void SyncLayout();
    void SetZoomAt(int zoom, double x, double y, bool centre = false);
    void SetZoom(int zoom);
    void SetUiScale(int scale);
    void SetOriginalSize(bool enabled);
    void SetScreen(bool window);
    void ToggleCrt();
    void SetAiMode(bool classic);
    bool MenuTicked(const std::string& key) const;
    void ExecuteMenuAction(const std::string& menu_key);

    // --- Pointer, Input & Gamepad ---
    int MouseX() const { return mouse_x_; }
    int MouseY() const { return mouse_y_; }
    void SetMousePos(int x, int y) {
        mouse_x_ = x;
        mouse_y_ = y;
    }
    int OpenMenu() const { return open_menu_; }
    void SetOpenMenu(int menu_index) { open_menu_ = menu_index; }
    int PressedControl() const { return pressed_control_; }

    bool Held(std::initializer_list<const char*> key_names) const;
    bool BusyWalking() const;
    void BufferKey(const std::string& key);
    void FlushKeys();
    void RefreshControls();
    int PointerKind(int x, int y) const;

    void Mousepressed(int x, int y, int button);
    void Mousemoved(double dx, double dy);
    void Wheelmoved(int wheel_y);
    void Mousereleased(int x, int y, int button);
    void Keypressed(const std::string& key);
    void Textinput(const std::string& text);

    void PadButton(int button, bool down);
    void PadLetGo();
    void PadUpdate();
    void SyncTyping();
    void InitPointerState(SDL_Window* win);
    void HandleMouseMotion(int win_x, int win_y);
    void PollFocusedMouse(SDL_Window* win);

    // --- Frame Update & Rendering ---
    void Update();
    void Draw();

  private:
    void InitActions();
    void AfterBattle(const Battle& result);
    void DoSearch();
    void ViewCity(int mode);
    std::string ShortcutKey(int slot_index) const;
    bool ActiveClassicAi() const;
    std::string UiScaleKey() const;
    void PointTo(double x, double y);
    void PadMouse(int& held, int button, bool down);
    void PadKey(const std::string& key, bool down);

    // Rendering helpers (`map_screen_render.cpp`)
    Army* TopArmy(const ArmyList& stack) const;
    int StackFigure(const ArmyList& stack) const;
    void DrawFog();
    void DrawStack(int owner, int army_type, int count, int pixel_x, int pixel_y);
    void DrawMap();
    void DrawRoute();
    void DrawStrategic();
    void DrawShortcutIcons();
    void DrawTurnStrip();
    void DrawMenuBar();
    void DrawAbits(int src_x, int src_y, int width, int height, int x, int y);
    void DrawDigits(int number, int x, int y);
    int StatusValue(int cell) const;
    void DrawArmySlots();
    void DrawStatus();
    void DrawComputerStatus(const AiStatus& status);
    void DrawBottomBar();
    void BarInfo(int x, int y);
    void DrawPointer();

    UiContext* ctx_ = nullptr;               ///< Bound UI dependency bundle.
    ScriptPlayer* script_player_ = nullptr;  ///< Bound `--script` runner.
    std::unique_ptr<Game> owned_game_;       ///< Active game session owned by controller.
    StatePtr state_;                         ///< Active `IMapScreenState` (never null).
    int mouse_x_ = -1;                       ///< Current UI pointer x coordinate.
    int mouse_y_ = -1;                       ///< Current UI pointer y coordinate.
    int open_menu_ = -1;                     ///< Open menu bar dropdown index, or `-1`.
    int pressed_control_ = kNone;            ///< Held main-screen button ID, or `kNone`.
    double seed_ = 0.0;                      ///< Most recent new-game seed.
    std::optional<CoordPair> cycle_cursor_;  ///< Last tile visited by the army cycle.
    std::optional<move::Preview> route_;     ///< Planned route preview for active selection.
    bool map_after_banner_ = false;          ///< True when hotseat blanks maps until banner closes.
    std::optional<MoveAllProgress> move_all_;  ///< Active `Order > Move All` state, if any.
    double last_select_click_ = -1.0;          ///< Timestamp of previous select-pointer click.
    std::vector<std::string> key_buffer_;      ///< Keys buffered during a human stack walk.
    std::string status_;                       ///< Last status-bar message string.
    int pad_left_held_ = 0;                    ///< Count of gamepad controls holding left click.
    int pad_right_held_ = 0;                   ///< Count of gamepad controls holding right click.
    int typing_now_ = 0;                       ///< Active SDL typing mode (`0`, `1`, or `2`).
    uint64_t last_pad_counter_ = 0;            ///< Previous `SDL_GetPerformanceCounter` tick.
    std::mt19937 loose_dice_;                  ///< Non-gameplay UI random number generator.
    ControlActionMap control_actions_;         ///< Main-screen control ID click handlers.
    MenuActionMap menu_actions_;               ///< Menu-bar accelerator and item handlers.
};

}  // namespace w2
