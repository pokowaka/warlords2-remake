// Combat: building the two lines, the modifiers, and the fight itself.
//
// docs/rules.md > Combat. combat_setup (6a89:008b) builds the lines,
// combat_terrain_class (6a89:0000) classifies the tile and combat_resolve
// (67cc:08a6) fights.
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

/** A battle, decided and then applied (combat.resolve, game.applyAttack). */
struct Battle {
    bool won = false;
    std::set<const Army*> dead_by_army;
    std::vector<int> log;  // 0 = a defender died, 1 = an attacker
    int attack_mod = 0, defend_mod = 0, cls = 0;
    std::vector<Army*> attackers, defenders;  // the survivors
    std::vector<Army*> dead_attackers, dead_defenders;
    // set by game::decideAttack
    struct Lines {
        std::vector<Army*> attackers, defenders;
        City* city = nullptr;
    } lines;
    int loot = 0;
    struct Fought {
        std::vector<Army*> stack;
        int x = 0, y = 0, def_owner = kNone;
    };
    std::optional<Fought> fought;  // until applied
    std::shared_ptr<QuestResult> quest;
    City* captured = nullptr;
    std::set<const Army*> boats;  // those that fought as boats, at sea on water or shore
    int def_strength0 = 0;        // the first defender's battle strength
    Army* hero = nullptr;         // the stack's hero, set by game::decideAttack
    Army* medal = nullptr;        // the army that won a medal, set by game::applyAttack
};

namespace combat {

// terrain classes, in the order the ARMYTYPE bonus fields are stored
constexpr int kCity = 0, kOpen = 1, kWoods = 2, kHills = 3;
extern const char* const kClassNames[4];

constexpr int kSiege = 1, kNegateHero = 2, kNegateOther = 3;

// hero strength -> command bonus, indexed 0..9
constexpr int kHeroTable[10] = {0, 0, 0, 0, 1, 1, 1, 2, 2, 3};

constexpr int kHitPoints = 2;
constexpr int kMaxStrength = 15;
constexpr int kDie = 20, kDieIntense = 24;
constexpr int kStalemate = 10000;  // throws in one duel before the attacker loses it

/** The battle tile's terrain class (combat_terrain_class, 6a89:0000). */
int TerrainClass(const Game& g, int x, int y);

struct Lines {
    std::vector<Army*> attackers, defenders;
    int def_owner = kNone;
    City* city = nullptr;
};
/** Build both battle lines for an attack on (x, y). */
Lines BattleLines(const Game& g, const std::vector<Army*>& stack, int x, int y);

/** Battle items (+n in battle) carried by one army. */
int BattleItems(const Army& army);
/** Command items carried by one army, a standard counting 1 (6a89:0d71). */
int CommandItems(const Army& army);
/** The side's hero bonus: the table lookup on its strongest hero, plus every
 *  command item it carries. */
int HeroBonus(const Game& g, const std::vector<Army*>& line);
/** The city's fortification bonus for the defender, or 0. */
int Fortify(const Game& g, const std::vector<Army*>& attackers, int x, int y, int cls);
/** Both sides' modifiers: {attackMod, defendMod}. */
std::pair<int, int> Modifiers(const Game& g, const std::vector<Army*>& attackers,
                              const std::vector<Army*>& defenders, int x, int y, int cls);
/** One army's fighting strength. A boat at sea on water or shore is 4. */
int Strength(const Game& g, const Army& army, int mod, int cls, int terrain);
/** What View > Stack shows beside each army of the group that moves: its
 *  strength in a fight on (x, y) (89e0:1b9c). */
std::map<const Army*, int> StackStrengths(const Game& g, const std::vector<Army*>& armies, int x,
                                          int y);
/** Fight a battle. The armies are not removed; the caller applies the
 *  outcome (game::applyAttack). */
Battle Resolve(Game& g, const std::vector<Army*>& attackers, const std::vector<Army*>& defenders,
               int x, int y);

/** The Military Advisor's verdict: 19 simulated battles, wins / 2 into
 *  STRING.DAT group 126 (military_advisor, 67cc:1f19). */
extern const char* const kAdvice[10];
constexpr int kAdviceBattles = 19;
std::pair<std::string, int> Advise(Game& g, const std::vector<Army*>& attackers,
                                   const std::vector<Army*>& defenders, int x, int y);

}  // namespace combat
}  // namespace w2
