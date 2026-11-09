/**
 * @file
 * @brief Dijkstra monitor (`w2::AiTurnRunner`) and typed observer contract (`w2::ITurnObserver`)
 * for serialized AI turn execution and UI animation handoff.
 */
#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/synchronization/mutex.h"

#include "warlords/model/ids.hpp"
#include "warlords/model/tile_pos.hpp"

struct SDL_Thread;

namespace w2 {

/** @brief Flow-control directive returned by `ITurnObserver::OnTurnEvent`. */
enum class TurnControl {
    kContinue,  ///< Continue executing the computer turn after handling the event.
    kAbort,     ///< Abort the computer turn cleanly and unwind the worker thread.
};

/// Ordered sequence of stable army handles participating in a stack movement.
using ArmyIdList = std::vector<ArmyId>;

/// Ordered sequence of map tile coordinates visited along a movement path.
using TilePath = std::vector<TilePos>;

/** @brief Status-bar delay emitted during an AI turn (`ticks` at 18.2 Hz BIOS rate). */
struct PauseEvent {
    int ticks = 0;     ///< Number of 18.2 Hz ticks to pause before resuming.
    std::string text;  ///< Optional status bar line displayed during the pause.
};

/** @brief Visible stack movement emitted when an AI stack crosses explored tiles. */
struct WalkEvent {
    ArmyIdList armies;        ///< Moving stack's stable army identifiers, in fighting order.
    TilePath tiles;           ///< Tile coordinates traversed in step order.
    bool sea_before = false;  ///< True when the stack began this step embarked at sea.
};

/** @brief Combat animation trigger emitted when an AI stack initiates a visible battle. */
struct AssaultEvent {
    TilePos target;                    ///< Attacked map tile coordinate.
    SideId attacker = SideId::None();  ///< Attacking computer faction identifier.
    SideId defender = SideId::None();  ///< Defending faction identifier, or `SideId::None()`.
    bool show_window = false;          ///< True when the full battle dialog window should open.
    std::string outcome;               ///< Formatted battle outcome summary string.
};

/** @brief Progress-bar update emitted as an AI faction advances through its turn phases. */
struct PhaseProgressEvent {
    SideId side = SideId::None();  ///< Computer faction currently taking its turn.
    int percent = 0;               ///< Completion percentage (`0..100`).
    std::string phase;             ///< Phase identifier (e.g. `"diplomacy"`, `"production"`).
};

/** @brief Modal announcement emitted when a faction is eliminated at round end. */
struct FallenEvent {
    SideId side = SideId::None();  ///< Eliminated faction identifier, if applicable.
    std::string text;              ///< Formatted elimination message text.
};

/** @brief Notification emitted when the final human faction falls while AI factions continue. */
struct NoHumansEvent {};

/** @brief Boundary event emitted at the end of a single computer faction's turn. */
struct TurnDoneEvent {
    SideId finished_side = SideId::None();  ///< Computer faction that finished its turn.
    SideId next_side = SideId::None();      ///< Next active faction, or `SideId::None()` at end.
};

/// Discriminated union of all observable events emitted during AI turn playback.
using TurnEvent = std::variant<PauseEvent, WalkEvent, AssaultEvent, PhaseProgressEvent, FallenEvent,
                               NoHumansEvent, TurnDoneEvent>;

/// Optional `TurnEvent` returned when stepping an `AiTurnRunner`.
using OptionalTurnEvent = std::optional<TurnEvent>;

/**
 * @brief Sink receiving typed `TurnEvent`s from an executing AI turn.
 *
 * Implemented both by the worker-side channel passed into `AiTurnRunner`'s
 * `TurnBody` (which suspends the worker until the main thread resumes or
 * aborts) and by UI/test consumers inspecting events on the main thread.
 */
class ITurnObserver {
  public:
    virtual ~ITurnObserver() = default;

    /**
     * @brief Handles `event` and returns whether AI turn execution should continue or abort.
     * @param event Typed turn playback event.
     * @return `TurnControl::kContinue` to proceed, or `TurnControl::kAbort` to unwind early.
     */
    virtual TurnControl OnTurnEvent(const TurnEvent& event) = 0;

    /** @brief True when cancellation has been requested and the caller should exit early. */
    virtual bool IsAborted() const { return false; }
};

/**
 * @brief Dijkstra monitor serializing background AI turn execution with main-thread UI playback.
 *
 * Runs a `TurnBody` on a dedicated 16 MB worker thread while transferring a
 * single execution token (`TurnOwner`) back and forth across `turn_mutex_`.
 * Only one thread executes domain or UI logic at any instant:
 * - On `Resume()` / `Step()`, the main thread transfers ownership to
 *   `TurnOwner::kWorker` and awaits `IsMainTurn()`.
 * - When `body_` invokes `ITurnObserver::OnTurnEvent`, the worker stores the
 *   event in `pending_event_`, transfers ownership back to `TurnOwner::kMain`,
 *   and awaits `IsWorkerTurn()`.
 * - On `Abort()` or destruction (`~AiTurnRunner()`), `abort_requested_` is
 *   latched under `turn_mutex_` and subsequent worker `OnTurnEvent` calls
 *   return `TurnControl::kAbort` immediately so `body_` unwinds without
 *   exceptions.
 *
 * Invariants (holding whenever `turn_mutex_` is unlocked):
 * - Whenever the main thread is outside `Resume()` and `Abort()`,
 *   `owner_ == TurnOwner::kMain` (the worker thread is either unspawned,
 *   parked inside `turn_mutex_.Await`, or exited).
 * - Once `finished_` is true, `owner_ == TurnOwner::kMain` permanently and
 *   `pending_event_` is empty.
 * - Neither `body_` nor any external `ITurnObserver` callback is ever invoked
 *   while `turn_mutex_` is held.
 *
 * Thread safety:
 * - `Resume`, `Step`, `RunToCompletion`, `Abort`, `Done`, and `~AiTurnRunner`
 *   must be called from the owning main thread; worker synchronization is
 *   guarded internally by `turn_mutex_`.
 */
class AiTurnRunner {
  public:
    /// Worker callable receiving the worker-side `ITurnObserver` handoff channel.
    using TurnBody = std::function<void(ITurnObserver&)>;

    /// Worker thread stack size (16 MB) accommodating deep AI search trees on macOS.
    static constexpr size_t kWorkerStackBytes = 16UZ * 1024 * 1024;

    /**
     * @brief Constructs an unstarted runner that will execute `body` on first `Resume()`/`Step()`.
     * @param body AI turn callable; exits cleanly when `observer.OnTurnEvent` returns `kAbort`.
     */
    explicit AiTurnRunner(TurnBody body);

    /** @brief Requests abort if still in flight and joins the worker thread cleanly. */
    ~AiTurnRunner();

    AiTurnRunner(const AiTurnRunner&) = delete;
    AiTurnRunner& operator=(const AiTurnRunner&) = delete;

    /**
     * @brief Advances the worker until its next emitted `TurnEvent` or turn completion.
     * @return The emitted `TurnEvent`, or `std::nullopt` once `body_` has finished or aborted.
     */
    OptionalTurnEvent Resume() ABSL_LOCKS_EXCLUDED(turn_mutex_);

    /**
     * @brief Advances one worker event and dispatches it to `observer` outside `turn_mutex_`.
     *
     * If `observer.OnTurnEvent` returns `TurnControl::kAbort`, immediately
     * unwinds and joins the worker thread before returning `false`.
     *
     * @param observer Main-thread observer notified with the yielded event.
     * @return True if an event was delivered with `kContinue`; false once finished or aborted.
     */
    bool Step(ITurnObserver& observer) ABSL_LOCKS_EXCLUDED(turn_mutex_);

    /**
     * @brief Drives `Step(observer)` until `body_` completes or `observer` requests `kAbort`.
     * @param observer Main-thread observer notified for every emitted event.
     */
    void RunToCompletion(ITurnObserver& observer) ABSL_LOCKS_EXCLUDED(turn_mutex_);

    /** @brief Latches cancellation, wakes any parked worker to unwind, and joins the thread. */
    void Abort() ABSL_LOCKS_EXCLUDED(turn_mutex_);

    /** @brief Returns true once `body_` has finished or been aborted. */
    bool Done() const ABSL_LOCKS_EXCLUDED(turn_mutex_);

  private:
    enum class TurnOwner {
        kMain,    ///< Main thread holds the execution token.
        kWorker,  ///< Worker thread holds the execution token.
    };

    class WorkerObserver final : public ITurnObserver {
      public:
        explicit WorkerObserver(AiTurnRunner& runner) : runner_(runner) {}
        TurnControl OnTurnEvent(const TurnEvent& event) override;
        bool IsAborted() const override;

      private:
        AiTurnRunner& runner_;  ///< Enclosing monitor coordinating handoff.
    };

    static int WorkerThreadMain(void* self_ptr);

    bool IsWorkerTurn() const ABSL_EXCLUSIVE_LOCKS_REQUIRED(turn_mutex_) {
        return owner_ == TurnOwner::kWorker;
    }
    bool IsMainTurn() const ABSL_EXCLUSIVE_LOCKS_REQUIRED(turn_mutex_) {
        return owner_ == TurnOwner::kMain;
    }

    TurnControl EmitFromWorker(const TurnEvent& event) ABSL_LOCKS_EXCLUDED(turn_mutex_);
    bool CheckAborted() const ABSL_LOCKS_EXCLUDED(turn_mutex_);
    void RunWorkerBody() ABSL_LOCKS_EXCLUDED(turn_mutex_);
    void JoinWorkerThread();

    TurnBody body_;                        ///< AI turn callable executed on `worker_thread_`.
    SDL_Thread* worker_thread_ = nullptr;  ///< Underlying OS worker thread (main-thread owned).
    bool started_ = false;                 ///< True once `worker_thread_` spawn was attempted.
    mutable absl::Mutex turn_mutex_;       ///< Guards turn token ownership, events, and lifecycle.
    TurnOwner owner_ ABSL_GUARDED_BY(turn_mutex_) = TurnOwner::kMain;  ///< Active execution owner.
    bool finished_ ABSL_GUARDED_BY(turn_mutex_) = false;            ///< Latched when `body_` exits.
    bool abort_requested_ ABSL_GUARDED_BY(turn_mutex_) = false;     ///< Latched on cancellation.
    OptionalTurnEvent pending_event_ ABSL_GUARDED_BY(turn_mutex_);  ///< Event awaiting main thread.
};

}  // namespace w2
