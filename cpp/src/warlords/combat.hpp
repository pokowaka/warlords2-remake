/**
 * @file
 * @brief `Battle` outcome model and `CombatResolver` domain service (`w2::combat`).
 */
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "warlords/types.hpp"

namespace w2 {

struct QuestResult;

/**
 * @brief Outcome and pending aftermath of a single tactical battle.
 *
 * `CombatResolver::Resolve` runs the round-by-round dice duel; `DecideAttack` attaches
 * battle lines, city loot, and pending target state; `ApplyAttack` commits casualties,
 * hero item drops, experience, quest progress, city capture, and medals.
 */
struct Battle {
    bool won = false;                    ///< True when every defender died.
    std::set<const Army*> dead_by_army;  ///< Every army that died, on either side.
    std::vector<int> log;          ///< Each death in order: 0 = a defender died, 1 = an attacker.
    int attack_mod = 0;            ///< The attackers' combined modifier.
    int defend_mod = 0;            ///< The defenders' combined modifier.
    int cls = 0;                   ///< The battle tile's terrain class (combat::kCity ...).
    std::vector<Army*> attackers;  ///< The attackers that survived.
    std::vector<Army*> defenders;  ///< The defenders that survived.
    std::vector<Army*> dead_attackers;  ///< The attackers that died, in fighting order.
    std::vector<Army*> dead_defenders;  ///< The defenders that died, in fighting order.
    /** @brief Both battle lines as they were drawn up. */
    struct Lines {
        std::vector<Army*> attackers;  ///< The attacking line, in fighting order.
        std::vector<Army*> defenders;  ///< The defending line, in fighting order.
        City* city = nullptr;          ///< The standing city fought for, or null.
    } lines;                           ///< The lines as they were drawn up.
    int loot = 0;                      ///< Gold taken from the losing side when a city falls.
    /** @brief What was attacked and where, kept until the battle is applied. */
    struct Fought {
        std::vector<Army*> stack;  ///< The attacking stack.
        int x = 0;                 ///< The attacked tile's column.
        int y = 0;                 ///< The attacked tile's row.
        int def_owner = kNone;     ///< The defending side's index, or kNone.
    };
    std::optional<Fought> fought;        ///< What was fought; empty once applied.
    std::shared_ptr<QuestResult> quest;  ///< The quest outcome the battle brought about, if any.
    City* captured = nullptr;            ///< The city the attacker took, or null.
    std::set<const Army*> boats;         ///< Those that fought as boats, at sea on water or shore.
    int def_strength0 = 0;               ///< The first defender's battle strength.
    Army* hero = nullptr;                ///< The stack's hero.
    Army* medal = nullptr;               ///< The army that won a medal.
};

namespace combat {

constexpr int kCity = 0, kOpen = 1, kWoods = 2, kHills = 3;
extern const char* const kClassNames[4];

constexpr int kSiege = 1, kNegateHero = 2, kNegateOther = 3;

/// Hero strength -> command bonus, indexed 0..9.
constexpr int kHeroTable[10] = {0, 0, 0, 0, 1, 1, 1, 2, 2, 3};

constexpr int kHitPoints = 2;      ///< Hits an army takes to die.
constexpr int kMaxStrength = 15;   ///< The cap on an army's battle strength.
constexpr int kDie = 20;           ///< The die each side throws per round.
constexpr int kDieIntense = 24;    ///< The die with the Intense Combat option.
constexpr int kStalemate = 10000;  ///< Throws in one duel before the attacker loses it.

/** @brief The two battle lines for an attack, each in fighting order. */
struct Lines {
    std::vector<Army*> attackers;  ///< The attacking line, in fighting order.
    std::vector<Army*> defenders;  ///< The defending line, in fighting order.
    int def_owner = kNone;         ///< The defending side's index, or kNone.
    City* city = nullptr;          ///< The standing city fought for, or null.
};

extern const char* const kAdvice[10];
constexpr int kAdviceBattles = 19;

/** @brief Fast simulation summary with minimal allocations for Monte Carlo rollouts. */
struct FastResult {
    bool won = false;             ///< True if every defender was eliminated.
    int surviving_attackers = 0;  ///< Number of surviving attacking units.
    int dead_defenders = 0;       ///< Number of defending units eliminated.
};

/** @brief Battle items (+n in battle) carried by one army. */
int BattleItems(const Army& army);
/** @brief Command items carried by one army, a standard counting 1. */
int CommandItems(const Army& army);

int TerrainClass(const Game& game, int x, int y);
Lines BattleLines(const Game& game, const std::vector<Army*>& stack, int x, int y);
int HeroBonus(const Game& game, const std::vector<Army*>& line);
int Fortify(const Game& game, const std::vector<Army*>& attackers, int x, int y, int cls);
std::pair<int, int> Modifiers(const Game& game, const std::vector<Army*>& attackers,
                              const std::vector<Army*>& defenders, int x, int y, int cls);
int Strength(const Game& game, const Army& army, int mod, int cls, int terrain);
std::map<const Army*, int> StackStrengths(const Game& game, const std::vector<Army*>& armies, int x,
                                          int y);
Battle Resolve(Game& game, const std::vector<Army*>& attackers, const std::vector<Army*>& defenders,
               int x, int y);
FastResult FastSimulate(Game& game, const std::vector<Army*>& attackers,
                        const std::vector<Army*>& defenders, int x, int y);
void FastSimulateRepeated(Game& game, const std::vector<Army*>& attackers,
                          const std::vector<Army*>& defenders, int x, int y, int count,
                          int& out_wins, int& out_survivors, int& out_dead_defenders);
std::pair<std::string, int> Advise(Game& game, const std::vector<Army*>& attackers,
                                   const std::vector<Army*>& defenders, int x, int y);

}  // namespace combat

/**
 * @brief Domain service resolving tactical combat, modifier calculation, military advice,
 * and post-battle world-state updates (casualties, city capture, hero XP, and medals).
 *
 * Operates over a borrowed `Game&` aggregate root, invoking `IComputerPlayer` hooks on
 * participating sides without depending on AI implementation headers.
 */
class CombatResolver {
  public:
    /** @brief Ordered list of army pointers forming a battle line or stack. */
    using ArmyStack = std::vector<Army*>;
    /** @brief Per-army effective battle strength lookup map. */
    using StrengthMap = std::map<const Army*, int>;
    /** @brief Attack and defence modifier pair `{attack_mod, defend_mod}`. */
    using ModifierPair = std::pair<int, int>;
    /** @brief Military advisor verdict string and simulated win count. */
    using AdviceVerdict = std::pair<std::string, int>;

    /** @brief Binds a combat resolver to `game` (must outlive this service instance). */
    explicit CombatResolver(Game& game) : game_(game) {}

    int TerrainClass(int x, int y) const;
    combat::Lines BattleLines(const ArmyStack& stack, int x, int y) const;
    int HeroBonus(const ArmyStack& line) const;
    int Fortify(const ArmyStack& attackers, int x, int y, int cls) const;
    ModifierPair Modifiers(const ArmyStack& attackers, const ArmyStack& defenders, int x, int y,
                           int cls) const;
    int Strength(const Army& army, int mod, int cls, int terrain) const;
    StrengthMap StackStrengths(const ArmyStack& armies, int x, int y) const;

    /** @brief Fights a duel between `attackers` and `defenders` at `(x, y)` without mutating
     * armies. */
    Battle Resolve(const ArmyStack& attackers, const ArmyStack& defenders, int x, int y);

    /** @brief Lightweight simulation without allocating Battle vectors or sets. */
    combat::FastResult FastSimulate(const ArmyStack& attackers, const ArmyStack& defenders,
                                    int x, int y);

    /** @brief Simulates `count` battles with cached modifiers and unit strengths. */
    void FastSimulateRepeated(const ArmyStack& attackers, const ArmyStack& defenders, int x, int y,
                              int count, int& out_wins, int& out_survivors,
                              int& out_dead_defenders);

    /** @brief Simulates `combat::kAdviceBattles` engagements to produce the advisor verdict. */
    AdviceVerdict Advise(const ArmyStack& attackers, const ArmyStack& defenders, int x, int y);

    /** @brief Computes gold plundered when a city belonging to `loser` falls. */
    int Loot(const Side* loser) const;

    /** @brief Draws battle lines and resolves an attack on `(x, y)` without applying casualties
     * yet. */
    Battle DecideAttack(const ArmyStack& stack, int x, int y);

    /** @brief Deducts movement cost and embarks non-flying attackers onto water/shore if needed. */
    void ChargeAttack(const ArmyStack& stack, int x, int y);

    /** @brief Commits a previously decided `Battle` to `game_` (once only). */
    Battle& ApplyAttack(Battle& result);

    /** @brief Decides and immediately applies an attack by `stack` on `(x, y)`. */
    Battle ResolveAttack(const ArmyStack& stack, int x, int y);

  private:
    Army* AwardMedal(const Battle& result, int side_index);

    Game& game_;  ///< Active game aggregate root.
};

}  // namespace w2
