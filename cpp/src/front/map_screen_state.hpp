/**
 * @file
 * @brief Explicit State Pattern (`w2::IMapScreenState`) and concrete map-interaction states for
 * `w2::MapScreenController`.
 */
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "platform/ai_turn_runner.hpp"
#include "warlords/combat.hpp"
#include "warlords/types.hpp"

namespace w2 {

class MapScreenController;

/**
 * @brief The twelve mouse pointer cursors of `STAND.PCK`, in sprite-sheet order.
 */
enum class PointerCursor : int {
    kArrow = 0,   ///< Menus, dialogs, or computer turn.
    kView = 1,    ///< Over the strategic map: moves the camera view.
    kBoat = 2,    ///< Walks the selected stack onto water/shore.
    kCity = 3,    ///< Opens the city inspection dialog.
    kHand = 4,    ///< Drags the map or indicates an unactionable tile.
    kSelect = 5,  ///< Selects the player's stack on the tile.
    kWalk = 6,    ///< Walks the selected stack to the tile.
    kSite = 7,    ///< Inspects a razed city or ruin site.
    kAttack = 8,  ///< Attacks an adjacent enemy stack or city.
    kAdvise = 9,  ///< Opens the Military Advisor odds dialog.
    kPeace = 10,  ///< Attacks a non-hostile faction after war confirmation.
    kAlt = 11,    ///< Plans a route without walking it (`Alt` held).
};

/**
 * @brief Active computer faction turn status displayed in the bottom bar.
 */
struct AiStatus {
    Side* side = nullptr;  ///< Computer faction currently taking its turn.
    std::string text;      ///< Status headline displayed above the progress bar.
    int progress = 0;      ///< Completion percentage (`0..100`).
};

/**
 * @brief Progress tracker for an active `Order > Move All` sweep.
 */
struct MoveAllProgress {
    std::set<Army*> seen;  ///< Armies already visited during this sweep.
    int moved = 0;         ///< Number of stacks that took at least one step.
};

/**
 * @brief Parameters describing a tile-by-tile stack walk animation.
 */
struct WalkSpec {
    using ArmyList = std::vector<Army*>;
    using TileCoordList = std::vector<std::pair<int, int>>;

    ArmyList armies;          ///< Armies walking together.
    TileCoordList tiles;      ///< Visited `(x, y)` tiles in step order.
    bool sea_before = false;  ///< True when the stack began embarked at sea.
    bool computer = false;    ///< True when initiated by a computer turn.
    ArmyList put_down;        ///< Armies to mark done once the walk finishes.
};

/**
 * @brief Parameters describing a battle cloud and casualty-line animation.
 */
struct AssaultSpec {
    int x = 0;                        ///< Attacked tile x coordinate.
    int y = 0;                        ///< Attacked tile y coordinate.
    Battle result;                    ///< Pre-resolved combat result.
    Battle* shared_battle = nullptr;  ///< Optional pointer to worker `Battle` during AI turns.
    bool computer = false;            ///< True when initiated by a computer turn.
    bool window = true;               ///< True when the battle window opens after the cloud.
    std::string outcome;              ///< One-line computer battle outcome summary.
    std::function<void()> after;      ///< Continuation invoked once human battle closes.
};

/**
 * @brief Polymorphic interaction and animation state owned by `MapScreenController`.
 *
 * Models each active map-screen mode (`HumanTurnState`, `MapDragState`, `StackWalkingState`,
 * `AssaultAnimationState`, `ComputerTurnState`, and `GameOverState`) behind a uniform per-frame
 * rendering, animation, and input contract. Transient substates (`MapDragState`,
 * `StackWalkingState`, `AssaultAnimationState`) take ownership of the suspended `resume_state_`
 * (such as an active `HumanTurnState` or `ComputerTurnState`) and restore it upon completion.
 */
class IMapScreenState {
  public:
    using CoordPair = std::pair<int, int>;

    virtual ~IMapScreenState() = default;

    virtual bool IsGameOver() const { return false; }
    virtual bool IsComputerTurn() const { return false; }
    virtual bool IsDragging() const { return false; }
    virtual bool IsWalking() const { return false; }
    virtual bool IsHumanBusyWalking() const { return false; }
    virtual bool IsAssaulting() const { return false; }
    virtual bool BlocksWheelAndLeftStick() const { return false; }

    virtual Side* ActiveComputerSide() const { return nullptr; }
    virtual const AiStatus* ActiveAiStatus() const { return nullptr; }
    virtual void SetAiStatusText(const std::string& text) { (void)text; }

    virtual const CoordPair* WalkingTile() const { return nullptr; }
    virtual size_t WalkingStepIndex() const { return 0; }
    virtual bool IsArmyWalking(const Army* army) const {
        (void)army;
        return false;
    }
    virtual Army* WalkingTopArmy() const { return nullptr; }
    virtual bool WalkingSeaBefore() const { return false; }

    /** @brief Invoked when a transient walk or assault substate finishes and restores this state.
     */
    virtual void OnSubstateFinished(MapScreenController& ctrl) { (void)ctrl; }

    /** @brief Per-frame pre-UI tick (advances `ComputerTurnState` when unblocked). */
    virtual void StepFrame(MapScreenController& ctrl) { (void)ctrl; }

    /** @brief Per-frame map animation tick (advances `StackWalkingState`). */
    virtual void UpdateMapAnimation(MapScreenController& ctrl) { (void)ctrl; }

    /** @brief Renders state-specific overlays in map coordinates (e.g. `WAR.PCK` cloud). */
    virtual void DrawMapOverlay(MapScreenController& ctrl) { (void)ctrl; }

    /** @brief Renders state-specific overlays in the 640x480 dialog coordinate frame. */
    virtual void DrawDialogOverlay(MapScreenController& ctrl) { (void)ctrl; }

    /** @brief State-specific key handler; returns true when consumed before default map keys. */
    virtual bool HandleKeyPressed(MapScreenController& ctrl, const std::string& key) {
        (void)ctrl;
        (void)key;
        return false;
    }

    /** @brief State-specific mouse press handler; returns true when consumed. */
    virtual bool HandleMousePressed(MapScreenController& ctrl, int x, int y, int button) {
        (void)ctrl;
        (void)x;
        (void)y;
        (void)button;
        return false;
    }

    /** @brief State-specific mouse motion handler. */
    virtual void HandleMouseMoved(MapScreenController& ctrl, double dx, double dy) {
        (void)ctrl;
        (void)dx;
        (void)dy;
    }

    /** @brief State-specific mouse release handler; returns true when consumed. */
    virtual bool HandleMouseReleased(MapScreenController& ctrl, int x, int y, int button) {
        (void)ctrl;
        (void)x;
        (void)y;
        (void)button;
        return false;
    }
};

/**
 * @brief Default interactive state during a human faction's turn.
 */
class HumanTurnState final : public IMapScreenState {
  public:
    HumanTurnState() = default;
    ~HumanTurnState() override = default;
};

/**
 * @brief Interactive map-panning drag state entered on left-button drag with the hand cursor.
 */
class MapDragState final : public IMapScreenState {
  public:
    using StatePtr = std::unique_ptr<IMapScreenState>;

    MapDragState(double start_cx, double start_cy, StatePtr resume_state);
    ~MapDragState() override = default;

    bool IsDragging() const override { return true; }
    bool BlocksWheelAndLeftStick() const override { return true; }
    bool IsComputerTurn() const override;
    Side* ActiveComputerSide() const override;
    const AiStatus* ActiveAiStatus() const override;
    void SetAiStatusText(const std::string& text) override;

    void StepFrame(MapScreenController& ctrl) override;
    void HandleMouseMoved(MapScreenController& ctrl, double dx, double dy) override;
    bool HandleMouseReleased(MapScreenController& ctrl, int x, int y, int button) override;

  private:
    double start_cx_ = 0.0;  ///< Camera tile x when the drag began.
    double start_cy_ = 0.0;  ///< Camera tile y when the drag began.
    double drag_dx_ = 0.0;   ///< Accumulated mouse x delta in UI pixels.
    double drag_dy_ = 0.0;   ///< Accumulated mouse y delta in UI pixels.
    StatePtr resume_state_;  ///< Suspended state restored when the mouse button releases.
};

/**
 * @brief Step-by-step stack movement animation state on the main map.
 */
class StackWalkingState final : public IMapScreenState {
  public:
    using StatePtr = std::unique_ptr<IMapScreenState>;
    using ArmySet = std::set<Army*>;
    using TileList = std::vector<CoordPair>;
    using ArmyList = std::vector<Army*>;

    StackWalkingState(WalkSpec spec, StatePtr resume_state);
    ~StackWalkingState() override = default;

    bool IsWalking() const override { return true; }
    bool IsHumanBusyWalking() const override { return !computer_; }
    bool IsComputerTurn() const override;
    Side* ActiveComputerSide() const override;
    const AiStatus* ActiveAiStatus() const override;
    void SetAiStatusText(const std::string& text) override;

    const CoordPair* WalkingTile() const override;
    size_t WalkingStepIndex() const override { return step_index_; }
    bool IsArmyWalking(const Army* army) const override;
    Army* WalkingTopArmy() const override { return top_; }
    bool WalkingSeaBefore() const override { return sea_before_; }

    void UpdateMapAnimation(MapScreenController& ctrl) override;
    bool HandleKeyPressed(MapScreenController& ctrl, const std::string& key) override;
    bool HandleMousePressed(MapScreenController& ctrl, int x, int y, int button) override;

    /** @brief Immediately cancels an AI walk when fast-forward (`ai_skip`) is triggered. */
    void CancelComputerWalk(MapScreenController& ctrl);

  private:
    ArmySet armies_;           ///< Set of armies participating in the walk.
    TileList tiles_;           ///< Sequence of tiles visited along the walk.
    size_t step_index_ = 0;    ///< Current index in `tiles_`.
    double step_at_ = 0.0;     ///< Timestamp when the current tile step began.
    Army* top_ = nullptr;      ///< Representative army whose sprite is drawn.
    bool computer_ = false;    ///< True when driven by a computer turn.
    bool sea_before_ = false;  ///< True when embarked at sea before starting the walk.
    ArmyList put_down_;        ///< Armies to mark done once the walk completes.
    StatePtr resume_state_;  ///< Suspended parent state (`HumanTurnState` or `ComputerTurnState`).
};

/**
 * @brief Visual playback state for tactical combat (map fire cloud + casualty battle window).
 */
class AssaultAnimationState final : public IMapScreenState {
  public:
    using StatePtr = std::unique_ptr<IMapScreenState>;
    using SlotList = std::vector<CoordPair>;
    using ArmyList = std::vector<Army*>;
    using LineList = std::vector<std::string>;

    AssaultAnimationState(MapScreenController& ctrl, AssaultSpec spec, StatePtr resume_state);
    ~AssaultAnimationState() override = default;

    bool IsAssaulting() const override { return true; }
    bool IsComputerTurn() const override;
    Side* ActiveComputerSide() const override;
    const AiStatus* ActiveAiStatus() const override;
    void SetAiStatusText(const std::string& text) override;

    void DrawMapOverlay(MapScreenController& ctrl) override;
    void DrawDialogOverlay(MapScreenController& ctrl) override;
    bool HandleKeyPressed(MapScreenController& ctrl, const std::string& key) override;
    bool HandleMousePressed(MapScreenController& ctrl, int x, int y, int button) override;

  private:
    enum class Phase {
        kCloud,   ///< Initial `WAR.PCK` fire cloud over the assaulted tile.
        kBattle,  ///< Casualty-by-casualty combat playback in the battle window.
        kOver,    ///< Outcome summary displayed while awaiting dismissal or auto-close.
    };

    struct Fell {
        double at = 0.0;    ///< Timestamp when casualty occurred.
        double burn = 0.0;  ///< Duration of blast sprite in seconds.
    };
    using FellList = std::vector<Fell>;

    void Advance(MapScreenController& ctrl);
    void Press(MapScreenController& ctrl);
    void Close(MapScreenController& ctrl, Army*& out_medal);
    void DrawBattleWindow(MapScreenController& ctrl) const;
    void DrawBattleLine(MapScreenController& ctrl, const ArmyList& armies, const SlotList& slots,
                        int side, int down, const FellList& fell) const;

    int tile_x_ = 0;                   ///< Assaulted map tile x coordinate.
    int tile_y_ = 0;                   ///< Assaulted map tile y coordinate.
    Battle result_;                    ///< Pre-resolved combat record.
    Battle* shared_battle_ = nullptr;  ///< Optional pointer to worker `Battle` during AI turns.
    ArmyList defenders_;               ///< Defending battle line.
    ArmyList attackers_;               ///< Attacking battle line.
    SlotList def_slots_;               ///< Defender sprite coordinates in the battle window.
    SlotList atk_slots_;               ///< Attacker sprite coordinates in the battle window.
    int def_side_ = 8;                 ///< Defender sprite sheet index (`8` = neutral).
    int atk_side_ = 8;                 ///< Attacker sprite sheet index (`8` = neutral).
    size_t step_ = 0;                  ///< Number of casualty rounds revealed so far.
    int def_down_ = 0;                 ///< Count of fallen defenders revealed so far.
    int atk_down_ = 0;                 ///< Count of fallen attackers revealed so far.
    FellList def_fell_;                ///< Blast timings for fallen defenders.
    FellList atk_fell_;                ///< Blast timings for fallen attackers.
    Phase phase_ = Phase::kCloud;      ///< Active animation stage.
    double at_ = 0.0;                  ///< Timestamp of current phase/round transition.
    double cloud_time_ = 0.0;          ///< Duration of `WAR.8SN` war-cry sound effect.
    double close_at_ = 0.0;            ///< Auto-close timestamp for computer battles.
    bool fast_ = false;                ///< True once hurried by player input.
    bool computer_ = false;            ///< True when initiated by a computer turn.
    bool window_ = true;               ///< True when the full battle window is shown.
    std::string victor_;               ///< Formatted winning hero or army-type name.
    std::string outcome_;              ///< Computer status-bar battle summary.
    LineList message_;                 ///< Outcome lines displayed at `Phase::kOver`.
    std::function<void()> after_;      ///< Post-close callback for human battles.
    StatePtr resume_state_;  ///< Suspended parent state (`HumanTurnState` or `ComputerTurnState`).
};

/**
 * @brief Computer-turn playback state driving `w2::AiTurnRunner` frame by frame without coroutines
 * or exceptions.
 */
class ComputerTurnState final : public IMapScreenState {
  public:
    ComputerTurnState(MapScreenController& ctrl, Side* first_side);
    ~ComputerTurnState() override;

    bool IsComputerTurn() const override { return true; }
    Side* ActiveComputerSide() const override { return ai_side_; }
    const AiStatus* ActiveAiStatus() const override { return ai_status_ ? &*ai_status_ : nullptr; }
    void SetAiStatusText(const std::string& text) override;

    void OnSubstateFinished(MapScreenController& ctrl) override;
    void StepFrame(MapScreenController& ctrl) override;
    bool HandleKeyPressed(MapScreenController& ctrl, const std::string& key) override;
    bool HandleMousePressed(MapScreenController& ctrl, int x, int y, int button) override;

    /**
     * @brief Advances `runner_` to its next `TurnEvent` or completes the round.
     * @return True when this `ComputerTurnState` remains the active top-level `ctrl.State()`.
     */
    bool Resume(MapScreenController& ctrl);

    /** @brief Handles spectator or skip input while a computer substate (walk) is active. */
    bool HandleComputerKey(MapScreenController& ctrl, const std::string& key,
                           StackWalkingState* active_walk);

  private:
    void RunWorkerTurns(MapScreenController& ctrl, Side* start_side, ITurnObserver& channel);
    TurnControl HandleMainEvent(MapScreenController& ctrl, const TurnEvent& event);

    std::unique_ptr<AiTurnRunner> runner_;  ///< Dijkstra monitor executing AI turns on worker.
    bool ai_wait_ = false;                  ///< True when ready to step once UI is clear.
    bool ai_skip_ = false;                  ///< True when fast-forwarding unwatched AI turns.
    bool ai_mods_held_ = false;          ///< True while initial Shift/Alt from `Alt+E` stays held.
    Side* ai_side_ = nullptr;            ///< Computer faction currently taking its turn.
    std::optional<AiStatus> ai_status_;  ///< Bottom-bar progress and status line.
    double ai_resume_at_ = 0.0;          ///< Monotonic timestamp when a `PauseEvent` expires.
    Side* next_human_side_ = nullptr;    ///< Next human faction after AI turns end, or `nullptr`.
    std::optional<TilePos> pending_centre_;  ///< Deferred main-thread camera centre request.
    Battle* pending_battle_ = nullptr;  ///< Worker `Battle` awaiting main-thread `AssaultEvent`.
    AssaultSpec pending_assault_{};     ///< Prepared `AssaultSpec` staged before `AssaultEvent`.
    bool has_pending_assault_ = false;  ///< True when `pending_assault_` holds a staged battle.
};

/**
 * @brief Terminal map state entered once no human faction remains at round end.
 */
class GameOverState final : public IMapScreenState {
  public:
    GameOverState() = default;
    ~GameOverState() override = default;

    bool IsGameOver() const override { return true; }
    bool BlocksWheelAndLeftStick() const override { return true; }
};

}  // namespace w2
