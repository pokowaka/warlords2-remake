// Combat: building the two lines, the modifiers, and the fight itself.
//
// See docs/rules.md > Combat. BattleLines() builds the lines, TerrainClass()
// classifies the battle tile and Resolve() fights.
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "warlords/types.hpp"

namespace w2 {

struct QuestResult;

/**
 * @brief A battle, first decided and then applied.
 *
 * combat::Resolve() fights it and fills in the outcome; game::DecideAttack()
 * adds the lines, the loot and what was fought, and game::ApplyAttack()
 * removes the dead and records the consequences.
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
    /**
     * @brief Both battle lines as they were drawn up, set by game::DecideAttack().
     */
    struct Lines {
        std::vector<Army*> attackers;  ///< The attacking line, in fighting order.
        std::vector<Army*> defenders;  ///< The defending line, in fighting order.
        City* city = nullptr;          ///< The standing city fought for, or null.
    } lines;                           ///< The lines as they were drawn up.
    int loot = 0;                      ///< Gold taken from the losing side when a city falls.
    /**
     * @brief What was attacked and where, kept until the battle is applied.
     */
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
    Army* hero = nullptr;                ///< The stack's hero, set by game::DecideAttack().
    Army* medal = nullptr;               ///< The army that won a medal, set by game::ApplyAttack().
};

namespace combat {

// Terrain classes, in the order the ARMYTYPE bonus fields are stored
// (see docs/formats/armytype.md).
constexpr int kCity = 0, kOpen = 1, kWoods = 2, kHills = 3;
extern const char* const kClassNames[4];  ///< The terrain classes' names, by class.

// The ARMYTYPE ability enum (see docs/formats/armytype.md).
constexpr int kSiege = 1, kNegateHero = 2, kNegateOther = 3;

/// Hero strength -> command bonus, indexed 0..9.
constexpr int kHeroTable[10] = {0, 0, 0, 0, 1, 1, 1, 2, 2, 3};

constexpr int kHitPoints = 2;      ///< Hits an army takes to die.
constexpr int kMaxStrength = 15;   ///< The cap on an army's battle strength.
constexpr int kDie = 20;           ///< The die each side throws per round.
constexpr int kDieIntense = 24;    ///< The die with the Intense Combat option.
constexpr int kStalemate = 10000;  ///< Throws in one duel before the attacker loses it.

/**
 * @brief Classifies the battle tile: city (towers, cities and sites), woods,
 *        hills (hills and mountains) or open.
 */
int TerrainClass(const Game& game, int x, int y);

/**
 * @brief The two battle lines for an attack, each in fighting order.
 */
struct Lines {
    std::vector<Army*> attackers;  ///< The attacking line, in fighting order.
    std::vector<Army*> defenders;  ///< The defending line, in fighting order.
    int def_owner = kNone;         ///< The defending side's index, or kNone.
    City* city = nullptr;          ///< The standing city fought for, or null.
};
/**
 * @brief Builds both battle lines for an attack on (x, y).
 *
 * Every army of another side on the tile -- or anywhere in the city, when a
 * standing city is attacked -- defends. Each line is sorted by its owner's
 * fight order.
 */
Lines BattleLines(const Game& game, const std::vector<Army*>& stack, int x, int y);

/** @brief Battle items (+n in battle) carried by one army. */
int BattleItems(const Army& army);
/** @brief Command items carried by one army, a standard counting 1. */
int CommandItems(const Army& army);
/**
 * @brief The side's hero bonus: the table lookup on its strongest hero, plus
 *        every command item it carries.
 */
int HeroBonus(const Game& game, const std::vector<Army*>& line);
/** @brief The city's fortification bonus for the defender, or 0. */
int Fortify(const Game& game, const std::vector<Army*>& attackers, int x, int y, int cls);
/** @brief Both sides' modifiers: {attack_mod, defend_mod}. */
std::pair<int, int> Modifiers(const Game& game, const std::vector<Army*>& attackers,
                              const std::vector<Army*>& defenders, int x, int y, int cls);
/** @brief One army's fighting strength. A boat at sea on water or shore is 4. */
int Strength(const Game& game, const Army& army, int mod, int cls, int terrain);
/**
 * @brief What View > Stack shows beside each army of the group that moves:
 *        its strength in a fight on (x, y).
 */
std::map<const Army*, int> StackStrengths(const Game& game, const std::vector<Army*>& armies, int x,
                                          int y);
/**
 * @brief Fights a battle.
 *
 * The armies are not removed; the caller applies the outcome
 * (game::ApplyAttack()).
 */
Battle Resolve(Game& game, const std::vector<Army*>& attackers, const std::vector<Army*>& defenders,
               int x, int y);

/// The Military Advisor's verdicts, by wins / 2 (STRING.DAT group 126).
extern const char* const kAdvice[10];
constexpr int kAdviceBattles = 19;  ///< Battles the Military Advisor simulates.
/**
 * @brief The Military Advisor's verdict on an attack.
 *
 * Simulates kAdviceBattles battles and picks the verdict by wins / 2.
 * @return The verdict and the number of simulated battles won.
 */
std::pair<std::string, int> Advise(Game& game, const std::vector<Army*>& attackers,
                                   const std::vector<Army*>& defenders, int x, int y);

}  // namespace combat
}  // namespace w2
