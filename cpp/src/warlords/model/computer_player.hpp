/**
 * @file
 * @brief Polymorphic computer player strategy interface (`IComputerPlayer`), attitude breakdowns,
 * and default strategy factory hooks.
 */
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/types/span.h"

#include "nlohmann/json.hpp"
#include "warlords/model/ids.hpp"
#include "warlords/model/tile_pos.hpp"

namespace w2 {

struct AIThought;
class Army;
struct Battle;
class City;
class Game;
class Side;
struct SideBelief;

/**
 * @brief Optional observer notified during computer-turn execution for UI pacing,
 * movement/combat animation handoff, and cooperative cancellation.
 *
 * Returning `false` from any callback signals cooperative cancellation (`IsAborted()`),
 * causing the executing `IComputerPlayer` strategy to unwind cleanly via normal early
 * returns without exceptions.
 */
class IAiTurnEvents {
  public:
    /** @brief Ordered list of army pointers forming a stack. */
    using ArmyStack = std::vector<Army*>;
    /** @brief Ordered list of `(x, y)` tile coordinates visited along a movement path. */
    using TileCoords = std::vector<std::pair<int, int>>;

    virtual ~IAiTurnEvents() = default;

    /**
     * @brief Invoked after a computer turn phase completes.
     * @param phase Phase identifier (e.g. `"diplomacy"`, `"production"`).
     * @return True to continue turn execution, or false to abort early.
     */
    virtual bool OnPhase(std::string_view phase) {
        (void)phase;
        return true;
    }

    /**
     * @brief Invoked after `stack` walks one or more steps along `walked`.
     * @param game Active game aggregate.
     * @param stack Moving stack in fighting order.
     * @param walked Tile coordinates traversed in step order.
     * @param sea_before True when the stack began this step embarked at sea.
     * @return True to continue turn execution, or false to abort early.
     */
    virtual bool OnWalk(Game& game, const ArmyStack& stack, const TileCoords& walked,
                        bool sea_before) {
        (void)game;
        (void)stack;
        (void)walked;
        (void)sea_before;
        return true;
    }

    /**
     * @brief Invoked after combat `result` is decided at `(x, y)` before applying casualties.
     * @param game Active game aggregate.
     * @param attackers Attacking stack.
     * @param x Attacked map tile x coordinate.
     * @param y Attacked map tile y coordinate.
     * @param result Decided battle outcome.
     * @return True to continue turn execution, or false to abort early.
     */
    virtual bool OnFight(Game& game, const ArmyStack& attackers, int x, int y, Battle& result) {
        (void)game;
        (void)attackers;
        (void)x;
        (void)y;
        (void)result;
        return true;
    }

    /**
     * @brief Invoked when a computer side pillages, sacks, or razes `city`.
     * @param game Active game aggregate.
     * @param side Acting computer faction.
     * @param city Captured city acted upon.
     * @param action Action label (`"pillaged"`, `"sacked"`, or `"razed"`).
     * @return True to continue turn execution, or false to abort early.
     */
    virtual bool OnSpoils(Game& game, Side& side, City& city, std::string_view action) {
        (void)game;
        (void)side;
        (void)city;
        (void)action;
        return true;
    }

    /** @brief True when cooperative cancellation has been requested. */
    virtual bool IsAborted() const { return false; }
};

/** @brief One labelled contribution to an AI side's diplomatic attitude score. */
struct AttitudeFactor {
    std::string label;  ///< Human-readable factor description shown in parley dialogs.
    int delta = 0;      ///< Signed contribution added to the attitude score.
};

/**
 * @brief Transparent breakdown explaining why an AI side holds its current diplomatic disposition
 * toward another side and what deal terms it will accept.
 */
struct AttitudeBreakdown {
    int from_side = kNone;                ///< Side holding the attitude.
    int to_side = kNone;                  ///< Target side being evaluated.
    std::string personality_name;         ///< Ruler personality name.
    std::string personality_archetype;    ///< Ruler personality archetype label.
    std::string motto;                    ///< Ruler motto string.
    int total_score = 0;                  ///< Summed attitude score clamped to `[-100, 100]`.
    int trust_score = 0;                  ///< Relational trust score in `[-100, 100]`.
    int truce_remaining = 0;              ///< Remaining turns on an active truce.
    std::vector<AttitudeFactor> factors;  ///< Ordered contributions to `total_score`.
    std::string stance_summary;           ///< Short stance headline for UI display.
    std::string what_it_takes;            ///< Guidance describing acceptable peace/alliance terms.
    int peace_gold_needed = 0;  ///< `0` = free, `> 0` = tribute required, `kNone` = refuses peace.
    int ally_gold_needed = 0;   ///< `0` = free, `> 0` = tribute required, `kNone` = refuses ally.
};

/**
 * @brief Polymorphic strategy interface governing a faction's autonomous decision-making,
 * diplomatic evaluation, battle memory, telemetry, and save-state serialization.
 *
 * Concrete implementations (`ClassicAiPlayer`, `ModernMlPlayer`, and training opponent
 * strategies) sit in `//cpp:ai` above `//cpp:rules`, allowing core game rules, combat,
 * hero offers, and diplomacy to invoke computer-player hooks without depending on AI
 * headers or storing AI scratchpads on `Army` or `Side`.
 *
 * Thread safety: instances mutate `Game` and internal strategy state during turn execution
 * and battle resolution on the active game thread; external synchronization is required if
 * inspected concurrently from a UI thread.
 *
 * Ownership: held on `Side::computer_player` via `std::shared_ptr<IComputerPlayer>`.
 */
class IComputerPlayer {
  public:
    /** @brief Read-only span of scored macro-action thoughts for the Brain Inspector UI. */
    using ThoughtSpan = absl::Span<const AIThought>;
    /** @brief Ordered list of army pointers forming a stack. */
    using ArmyStack = std::vector<Army*>;

    virtual ~IComputerPlayer() = default;

    /**
     * @brief Executes one full computer turn for `side` on `game`.
     * @param game Active game aggregate root.
     * @param side Side whose turn is being played.
     * @param events Optional observer notified of phase, movement, combat, and spoils events.
     */
    virtual void PlayTurn(Game& game, Side& side, IAiTurnEvents* events = nullptr) = 0;

    /**
     * @brief Notifies the strategy that `side`'s turn has opened (e.g. resetting army-cycle
     * cursors).
     * @param side Side whose turn just started.
     */
    virtual void OnTurnStarted(Side& side) {}

    /**
     * @brief Evaluates `ai_side`'s diplomatic disposition and deal thresholds toward
     * `proposer_side` using only information visible to `ai_side`.
     * @param game Read-only game state.
     * @param ai_side Side whose attitude is computed.
     * @param proposer_side Other side being evaluated.
     * @return Populated `AttitudeBreakdown` value object.
     */
    virtual AttitudeBreakdown EvaluateProposal(const Game& game, SideId ai_side,
                                               SideId proposer_side) const = 0;

    /**
     * @brief Chooses which owned city a newly recruited hero should appear in.
     * @param game Active game state (may advance `game.rng` for classic AI scoring).
     * @param side Recruiting computer side.
     * @param fallback Default randomly chosen owned city.
     * @return Selected `City*` owned by `side`.
     */
    virtual City* ChooseHeroCity(Game& game, Side& side, City* fallback) = 0;

    /**
     * @brief Handles quest-target city capture rules when `stack` conquers `city`.
     * @param game Active game state.
     * @param side Capturing computer side.
     * @param city Newly captured city.
     * @param stack Surviving attacking stack.
     * @return True if the strategy razed or sacked `city` (suppressing default occupation).
     */
    virtual bool OnQuestCapture(Game& game, Side& side, City& city, const ArmyStack& stack) = 0;

    /**
     * @brief Records battle casualties suffered on `defender`'s tile against `attacker`.
     * @param game Active game state.
     * @param defender Defending side identifier (`0..7`).
     * @param attacker Attacking side identifier (`0..7`).
     * @param pos Tile coordinates where the battle occurred.
     * @param heroes_lost Number of defending heroes slain.
     * @param armies_lost Total number of defending armies slain.
     * @param all_lost True when the entire defending force was wiped out.
     * @param city_tile True when the battle fought for a standing city.
     */
    virtual void RecordBattle(Game& game, SideId defender, SideId attacker, TilePos pos,
                              int heroes_lost, int armies_lost, bool all_lost, bool city_tile) = 0;

    /**
     * @brief Updates fair-information enemy stack observations for `observer` after a battle at
     * `pos`.
     * @param game Active game state.
     * @param observer Observing participant side (`0..7`).
     * @param pos Battle tile coordinates.
     */
    virtual void RecordBattleObservation(Game& game, SideId observer, TilePos pos) = 0;

    /**
     * @brief Serializes strategy-specific mutable state to ordered JSON.
     * @return JSON object representing this computer player's state.
     */
    virtual nlohmann::ordered_json SaveState() const = 0;

    /**
     * @brief Restores strategy-specific mutable state from `state` against `game`.
     * @param state Previously serialized JSON object.
     * @param game Reconstructed game aggregate with active armies and cities populated.
     * @return `absl::OkStatus()` on success, or an error status on malformed input.
     */
    virtual absl::Status LoadState(const nlohmann::ordered_json& state, const Game& game) = 0;

    /** @brief Returns scored macro-action thoughts from the latest turn (empty for classic AI). */
    virtual ThoughtSpan Thoughts() const = 0;

    /** @brief Returns the critic's latest board value estimate (`0.0F` for classic AI). */
    virtual float LastBoardValue() const = 0;

    /** @brief Returns true when this strategy follows the 1993 Classic AI ruleset. */
    virtual bool IsClassic() const = 0;

    /** @brief Returns the side's fair-information `SideBelief` if maintained, or `nullptr`. */
    virtual const SideBelief* Belief() const { return nullptr; }
};

/** @brief Factory callable creating an `IComputerPlayer` for `side` on `game`. */
using ComputerPlayerFactory =
        std::function<std::shared_ptr<IComputerPlayer>(bool classic_mode, Game& game, Side& side)>;

/** @brief Hook callable initializing computer strategies and initial claims/scores for `game`. */
using GameComputerSetupFn = std::function<void(Game& game)>;

/** @brief Registers the process-wide default `ComputerPlayerFactory` (installed by `//cpp:ai`). */
void RegisterDefaultComputerPlayerFactory(ComputerPlayerFactory factory);

/** @brief Creates a computer player using the registered default factory, or `nullptr` if unset. */
std::shared_ptr<IComputerPlayer> CreateDefaultComputerPlayer(bool classic_mode, Game& game,
                                                             Side& side);

/** @brief Registers the process-wide default `GameComputerSetupFn` (installed by `//cpp:ai`). */
void RegisterDefaultGameComputerSetup(GameComputerSetupFn setup_fn);

/** @brief Runs the registered default game computer setup on `game` if present. */
void RunDefaultGameComputerSetup(Game& game);

}  // namespace w2
