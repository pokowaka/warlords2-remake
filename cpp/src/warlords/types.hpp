/**
 * @file
 * @brief The game's state, as structs.
 *
 * The Lua keeps it in tables that grow fields as they go; here every field is
 * declared, and Lua's nil is kNone (-1) for an index or a coordinate, or
 * std::optional where -1 is a real value.
 *
 * A Game owns every army it ever made (`pool`), so a pointer held by a quest,
 * an AI group or the selection stays valid after the army has left
 * `armies`. Being in the game is being in `armies`.
 */
#pragma once

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "util/json.hpp"
#include "warlords/rng.hpp"

namespace w2 {

/// "Nothing": the Lua's nil for an index or a coordinate.
constexpr int kNone = -1;

struct Side;
struct City;
struct Army;
struct Item;
struct Game;
struct AIData;
struct SideBelief;

// ------------------------------------------------------------- army types

/** @brief An army type as ARMYTYPE.DAT gives it (docs/formats/armytype.md). */
struct ArmyType {
    int id = 0;        ///< The type id; also the sprite index.
    std::string name;  ///< The name shown in the game.
    int strength = 0;  ///< Fighting strength, 1..9.
    int time = 0;      ///< Turns to build one.
    int cost = 0;      ///< Per-turn upkeep basis.
    int move = 0;      ///< Movement points per turn.
    int price = 0;     ///< What it costs to buy for a city; negative: can never be bought.
    /// The record's ability and bonus words, keyed by their offset in the
    /// record (32..60); see docs/formats/armytype.md.
    std::array<int, 64> bonus{};
    bool flies = false;       ///< It flies: crosses any terrain, water included.
    bool siege = false;       ///< A siege type.
    bool boat = false;        ///< It is a boat.
    bool woods_move = false;  ///< It moves through forest at little cost.
    bool hills_move = false;  ///< It moves through hills at little cost.
};

/** @brief Every army type of the game, in file order and by id. */
struct Types {
    std::vector<ArmyType> all;        ///< In file (display) order.
    std::array<ArmyType*, 64> ids{};  ///< By type id; null where there is none.
    /** @brief The type with this id, or null for an unknown one. */
    const ArmyType* ById(int type_id) const {
        return type_id >= 0 && type_id < 64 ? ids[type_id] : nullptr;
    }
    /** @brief Every type, in file order. */
    std::vector<const ArmyType*> List() const {
        std::vector<const ArmyType*> out;
        for (auto& army_type : all) out.push_back(&army_type);
        return out;
    }
};

// --------------------------------------------------------------- the map

/** @brief A production slot: an army type with the city's own numbers for it. */
struct Slot {
    int type = 0;      ///< The army type id.
    std::string name;  ///< The type's name.
    int strength = 0;  ///< Strength of the armies the city builds.
    int time = 0;      ///< Turns the city takes to build one.
    int cost = 0;      ///< Upkeep basis of the armies built.
    int move = 0;      ///< Movement points of the armies built.
    int price = 0;     ///< What the slot cost to buy.
    /** @brief Equal when every number matches. */
    bool operator==(const Slot& other) const {
        return type == other.type && name == other.name && strength == other.strength &&
               time == other.time && cost == other.cost && move == other.move &&
               price == other.price;
    }
};

/** @brief A city: a 2x2 block of tiles that earns gold and builds armies. */
struct City {
    int index = 0;              ///< Its place in Map::cities.
    int x = 0;                  ///< Column of its top-left tile.
    int y = 0;                  ///< Row of its top-left tile.
    std::string name;           ///< The city's name.
    int income = 0;             ///< Gold it earns its owner each turn.
    std::vector<int> produces;  ///< Army type ids, as the scenario gives them.
    int defence = 0;            ///< Its defence level, which strengthens its defenders.
    Side* owner = nullptr;      ///< Only while the scenario loads.
    int owner_index = kNone;    ///< The side holding it; kNone for neutral.
    std::vector<Slot> slots;    ///< What it can build.
    int producing = kNone;      ///< The slot index being built.
    int countdown = 0;          ///< Turns left until the army in production is done.
    int vector_to = kNone;      ///< Where it sends what it builds: a city index, or kStandard.
    bool razed = false;         ///< Burnt to the ground: out of play.
    int razed_by = kNone;       ///< The side that razed it.
    /// The computer whose ground it is; after a capture, the side it was
    /// taken from (docs/rules.md > Capture).
    int claim = kNone;
    int attacked = 0;  ///< A bit per side that has attacked it.
};

/** @brief A magic item, a standard included. */
struct Item {
    int index = 0;            ///< Its place in Map::items.
    std::string name;         ///< The item's name.
    int type = 0;             ///< Its effect (rules::kItemBattle and so on).
    int value = 0;            ///< How strong the effect is.
    int status = 0;           ///< 0 out of play, 1 on the ground, 2 hidden, 3 carried.
    int x = kNone;            ///< Column where it lies, if on the ground or hidden.
    int y = kNone;            ///< Row where it lies, if on the ground or hidden.
    bool planted = false;     ///< A standard planted in the ground.
    int standard_of = kNone;  ///< Items 0-7: the eight sides' standards.
};

/** @brief An entry of the scenario's item pool (.ITM, docs/formats/itm.md). */
struct PoolItem {
    std::string name;  ///< The item's name.
    int type = 0;      ///< Its effect.
    int value = 0;     ///< How strong the effect is.
};

/** @brief A temple or a ruin (docs/rules.md > Ruins, temples and sages). */
struct Site {
    int index = 0;             ///< Its place in Map::sites.
    int x = 0;                 ///< Column.
    int y = 0;                 ///< Row.
    std::string name;          ///< The site's name.
    int type = 0;              ///< 1 = temple, 2 = ruin.
    int content = 0;           ///< What a search finds (temple, empty, sage, gold, allies...).
    int item = kNone;          ///< The item hidden in it, if any.
    int guardian = kNone;      ///< The monster guarding it, if any.
    int ally_type = kNone;     ///< The army type of the allies it holds, if any.
    bool rich = false;         ///< A rich ruin: the reserved items are hidden only in these.
    int revealed = 0;          ///< A bit per side the site is shown to.
    bool searched = false;     ///< Searched already: nothing more to find.
    std::string band;          ///< "rich", "near" or "far": the table its content is drawn from.
    int temple_index = kNone;  ///< For a temple: its number among the temples.
};

/** @brief A ruin's guardian. */
struct Monster {
    int index = 0;     ///< Its place in Map::monsters.
    std::string name;  ///< The monster's name.
    int strength = 0;  ///< Its fighting strength.
};

/** @brief A signpost on the map. */
struct Sign {
    int index = 0;                     ///< Its place in Map::signs.
    int x = 0;                         ///< Column.
    int y = 0;                         ///< Row.
    std::array<std::string, 2> lines;  ///< Its two lines of text.
};

/** @brief The game options, as the setup screen and the scenario set them. */
struct Options {
    int neutral_cities = 0;    ///< Neutral cities' garrisons: 0 none .. 3 strong.
    int diplomacy = 0;         ///< Diplomacy on.
    int quests = 0;            ///< Quests on.
    int random_turns = 0;      ///< The turn order is shuffled each round.
    int hidden_map = 0;        ///< Each side sees only what it has explored.
    int intense_combat = 0;    ///< Intense combat.
    int quick_start = 0;       ///< Quick start: the cities are dealt out at the start.
    int view_enemies = 0;      ///< Enemy stacks can be looked into.
    int military_advisor = 1;  ///< The military advisor is available.
    int tutorial = 0;          ///< The tutorial's moments are shown.
    int view_production = 0;   ///< Enemy production can be looked at.
    /** @brief The option by the Lua's name for it; null for an unknown name. */
    int* Field(const std::string& name);
    /** @brief Every option's Lua name. */
    static const std::vector<std::string>& Names();
};

// ------------------------------------------------------------------ sides

/** @brief What a side's turn start produced, for the report (side.produced). */
struct Produced {
    std::string kind;       ///< "built", "sent" or "arrived".
    int type = 0;           ///< The army type.
    int city = kNone;       ///< The city it concerns.
    bool standard = false;  ///< It went to the side's standard.
};

/** @brief A quest (quest.lua). The target is one of the pointers, as target_kind says. */
struct Quest {
    int type = 0;                         ///< The kind of quest.
    Army* hero = nullptr;                 ///< The hero on the quest.
    std::string target_kind;              ///< "city", "side", "item", "armytype", "army", "none".
    City* city = nullptr;                 ///< The target city.
    Side* side = nullptr;                 ///< The target side.
    Item* item = nullptr;                 ///< The target item.
    const ArmyType* army_type = nullptr;  ///< The target army type.
    Army* army = nullptr;                 ///< The target army.
    int required = kNone;                 ///< How many are needed, for a counting quest.
    int done = 0;                         ///< How many are done so far.
    /** @brief Does the quest still have its target? */
    bool HasTarget() const {
        return target_kind == "none" || city || side || item || army_type || army;
    }
};

struct Reward;
struct QuestResult;

/** @brief The advisor's marks (cues.lua). */
struct Advisor {
    int dir = 0;   ///< 1 after a rise in cities, 2 after a fall.
    int mark = 0;  ///< The city count last remarked on, a multiple of 5.
};

struct HeroOffer;

/** @brief A hero promoted as the turn opened, and the strength it gained. */
struct Promotion {
    Army* hero = nullptr;  ///< The hero.
    int gain = 0;          ///< The strength it gained.
};

/** @brief A scored macro-action recorded by the modern ML AI for the in-game F10 Brain Inspector.
 */
struct AIThought {
    std::string category;     ///< What kind of action.
    std::string actor_name;   ///< Who would act.
    std::string target_name;  ///< On what.
    int sx = kNone;           ///< Column the actor stands on.
    int sy = kNone;           ///< Row the actor stands on.
    int tx = kNone;           ///< Column of the target.
    int ty = kNone;           ///< Row of the target.
    float score = 0.0F;       ///< The action's score.
    float win_prob = 0.0F;    ///< The predicted chance of winning the fight.
    bool stage_near = false;  ///< Stage near the target rather than strike it.
    std::string detail;       ///< Free text for the inspector.
};

/** @brief A turn-start diplomatic parley missive sent from an AI side to a player side. */
struct DiplomaticMissive {
    int from_side = kNone;          ///< The sender.
    int to_side = kNone;            ///< The receiver.
    int proposed_state = kNone;     ///< The diplomatic state proposed.
    bool propose_alliance = false;  ///< An alliance is proposed.
    int gold_demand = 0;            ///< > 0: from_side demands gold; < 0: from_side offers gold.
    int joint_war_target = kNone;   ///< The side proposed for a joint war.
    std::string headline;           ///< The missive's title.
    std::string body;               ///< Its text.
    std::string reason_summary;     ///< Why it was sent.
};

/** @brief A player: human or computer, with its gold, its capital and its turn's news. */
struct Side {
    int index = 0;                            ///< 0..7.
    std::string name;                         ///< The side's name.
    int colour = 0;                           ///< Its colour.
    int edge = 0;                             ///< The colour of its edging.
    int gold = 0;                             ///< Gold in the treasury.
    int cap_x = 0;                            ///< Column of its capital, as the scenario gives it.
    int cap_y = 0;                            ///< Row of its capital, as the scenario gives it.
    bool computer = false;                    ///< Played by the computer.
    bool classic_ai = true;                   ///< A computer playing the original's AI.
    int level = 0;                            ///< The computer's level.
    bool enhanced = false;                    ///< Enhanced: its armies are built stronger.
    bool observe = false;                     ///< The human watches this computer's turns.
    int diplo_score = 0;                      ///< Its diplomatic score.
    int card = 0;                             ///< The computer's personality card (aicard).
    bool in_use = false;                      ///< In this game at all.
    City* capital = nullptr;                  ///< Its capital city.
    bool alive = false;                       ///< Still in the game.
    int owner_index = 0;                      ///< The side's own index, as an owner.
    int income = 0;                           ///< The turn's income.
    int upkeep_total = 0;                     ///< The turn's upkeep.
    std::vector<Produced> produced;           ///< What the turn start produced.
    std::vector<std::string> diplo_news;      ///< Diplomatic news for the turn.
    std::shared_ptr<HeroOffer> hero_offer;    ///< A hero offering to join, if any.
    std::shared_ptr<Quest> quest;             ///< Its quest, if any.
    std::shared_ptr<QuestResult> quest_news;  ///< How its quest went, to tell.
    std::shared_ptr<AIData> ai;               ///< The computer player's memory.
    std::shared_ptr<SideBelief> belief;       ///< What the modern AI believes of the others.
    std::vector<AIThought> ai_thoughts;       ///< For the Brain Inspector.
    std::vector<DiplomaticMissive> missives;  ///< Missives waiting to be read.
    std::array<int, 8> last_missive_turn{};   ///< The turn it last sent each side a missive.
    float last_board_value = 0.0F;            ///< The modern AI's last view of its position.
    int ai_solidarity = 0;                    ///< The computer's solidarity with its allies.
    Advisor advisor;                          ///< The advisor's marks.
    int cap_now_x = kNone;  ///< Column of where the capital now stands; kNone until the first turn.
    int cap_now_y = kNone;  ///< Row of where the capital now stands.
    bool production_due = false;      ///< A human's cities build once the offer is settled.
    std::vector<Promotion> promoted;  ///< For the interface to show.
};

// ------------------------------------------------------------------ armies

/** @brief An army on its way to a city or a standard, off the map. */
struct Transit {
    int turns = 0;     ///< Turns until it arrives.
    int dest = kNone;  ///< A city index, or kStandard.
};

/** @brief One army: a unit, a hero, or a ruin's allies. */
struct Army {
    int slot = 0;           ///< Its place in the army table.
    int x = kNone;          ///< Column; kNone while in transit.
    int y = kNone;          ///< Row; kNone while in transit.
    int owner = kNone;      ///< The side owning it; kNone for neutral.
    int type = 0;           ///< The army type id.
    std::string name;       ///< Its name.
    bool female = false;    ///< A heroine.
    int strength = 0;       ///< Fighting strength.
    int moves = 0;          ///< Movement points left this turn.
    int max_moves = 0;      ///< Movement points per turn.
    int upkeep = 0;         ///< Gold it costs each turn.
    int home_city = kNone;  ///< The city that built it.
    int medals = 0;         ///< Medals won in battle.
    bool at_sea = false;    ///< Aboard a ship.
    int group = 0;          ///< The bottom bar's group it belongs to; 0 ungrouped.
    std::optional<std::pair<int, int>> target;  ///< Where it is walking to.
    bool fortified = false;                     ///< Fortified: left out of the cycle.
    // heroes
    int level = 0;                   ///< A hero's level.
    int experience = 0;              ///< A hero's experience.
    std::string title;               ///< A hero's title.
    std::vector<Item*> items;        ///< Items it carries.
    std::set<int> blessings;         ///< Temples that have blessed it.
    std::optional<Transit> transit;  ///< On its way somewhere, off the map.
    bool returning = false;          ///< Going back to its home city.
    // the turn's marks
    bool done = false;       ///< Done for the turn.
    bool offered = false;    ///< Offered by the army cycle already this pass.
    bool disbanded = false;  ///< Disbanded this turn.
    // the computer player's
    int ai_order = 0;         ///< The computer's order for it.
    int ai_dest = kNone;      ///< The order's destination.
    int ai_group = 0;         ///< The computer's group it moves with.
    bool ai_explore = false;  ///< Sent exploring.
    bool ai_neutral = false;  ///< Sent after a neutral city.
    bool ai_party = false;    ///< Part of a party.
    bool ai_moved = false;    ///< Moved this turn.

    /** @brief Is it a hero? */
    bool Hero() const;
};

// ----------------------------------------------------------------- the map

/// The map's full size in tiles.
constexpr int kMapW = 112, kMapH = 156;

/** @brief Everything the scenario holds: the sides, the cities, the sites, the tiles. */
struct Map {
    std::string name;                                 ///< The scenario's name.
    std::array<Side, 8> sides;                        ///< The eight sides.
    std::vector<City> cities;                         ///< Every city.
    std::vector<City*> city_at;                       ///< By tile: a city's top-left tile.
    std::vector<City*> city_tile;                     ///< By tile: every tile of its 2x2 footprint.
    std::vector<Site> sites;                          ///< Every temple and ruin.
    std::vector<Site*> site_at;                       ///< By tile.
    std::vector<Item> items;                          ///< Every item.
    std::array<std::optional<Monster>, 10> monsters;  ///< The ruins' guardians.
    std::vector<bool> crossing;                       ///< By tile: land and sea may meet here.
    std::optional<std::vector<PoolItem>> item_pool;   ///< The scenario's item pool, if any.
    std::map<int, std::array<std::string, 3>> city_text;  ///< The cities' descriptions.
    std::map<int, std::array<std::string, 3>> site_text;  ///< The sites' descriptions.
    std::vector<Sign> signs;                              ///< The signposts.
    Options options;                                      ///< The game options.
    std::array<int, 256> terrain_type{};                  ///< Terrain type by tile graphic.
    std::vector<std::vector<int>> fight_order;            ///< 9 rows of 29, row 8 neutral.
    int combat_cap = 0;                                   ///< The most a stack's bonus may add.
    std::vector<int> tiles;                               ///< The map words, row by row.
    std::string roads;                                    ///< The road layer.
    int width = kMapW;                                    ///< Width in tiles.
    int height = kMapH;                                   ///< Height in tiles.
    int active_x0 = 0;          ///< Left column of the part of the map in play.
    int active_y0 = 0;          ///< Top row of the part in play.
    int active_x1 = kMapW - 1;  ///< Right column of the part in play.
    int active_y1 = kMapH - 1;  ///< Bottom row of the part in play.

    /** @brief The side with this index, or null. */
    Side* Side(int index) { return index >= 0 && index < 8 ? &sides[index] : nullptr; }
    /** @brief The city with this index, or null. */
    City* City(int index) {
        return index >= 0 && index < static_cast<int>(cities.size()) ? &cities[index] : nullptr;
    }
};

// ----------------------------------------------------------------- the game

/**
 * @brief What a deed was: one of the events History plays back.
 *
 * STRING.DAT group 94, docs/formats/history.md. The order is also each
 * deed's rank: a side keeps only its two most important deeds of a round,
 * and a lower value counts for more. The comments say what a deed's two
 * values hold.
 */
enum class DeedType : int {
    kHeroEmerges = 0,     ///< v1: the city the hero appeared in.
    kHeroKilled = 1,      ///< v1: the city, or history::kKilledInBattle / kKilledSearching.
    kQuestCompleted = 2,  ///< A hero completed its quest.
    kQuestReceived = 3,   ///< A hero was given a quest.
    kSideVanquished = 4,  ///< v1: the side.
    kCityWon = 5,         ///< v1: the side, or history::kWonByName; v2: the city.
    kFound = 6,           ///< v1: the item, or history::kFoundAllies / kFoundSage / kFoundGold.
    kSideVictorious = 7,  ///< v1: the side.
    kTreachery = 8,       ///< v1: the side that attacked at peace, v2: its victim.
    kAtWar = 9,           ///< v1, v2: the two sides.
    kAtPeace = 10,        ///< v1, v2: the two sides.
};

/**
 * @brief A deed of the round, for the history.
 *
 * Who did it, what, two values whose meaning depends on the type, and the
 * hero's or the side's name.
 */
struct Deed {
    int side = 0;                            ///< The side whose deed it is.
    DeedType type = DeedType::kHeroEmerges;  ///< What it was.
    int v1 = 0;                              ///< The first value (see DeedType).
    int v2 = 0;                              ///< The second value (see DeedType).
    std::string name;                        ///< The hero's or the side's name.
};
/** @brief A round's record: what History plays back. */
struct HistoryRecord {
    std::array<int, 8> gold{};    ///< Each side's gold.
    std::array<int, 8> score{};   ///< Each side's score.
    std::array<int, 8> cities{};  ///< Each side's city count.
    std::vector<int> owners;      ///< Each city's owner: 8 neutral, 0xff razed.
    std::vector<Deed> events;     ///< The round's deeds.
};
/**
 * @brief A side put out at the round's end, and how it is told.
 *
 * A line of STRING.DAT group 11, in a box or in the status bar.
 */
struct Fallen {
    Side* side = nullptr;  ///< The side put out.
    int line = 0;          ///< The line of STRING.DAT group 11 that tells it.
    bool boxed = false;    ///< Told in a box rather than in the status bar.
};
/** @brief How the game ended, or is ending, as the round's end finds it. */
struct Ending {
    bool over = false;           ///< The game is over.
    bool won = false;            ///< A side has won.
    bool triumph = false;        ///< A triumph: the winner holds every city.
    bool surrender = false;      ///< The others offer to surrender.
    bool no_humans = false;      ///< No human is left.
    bool shown = false;          ///< The front end has told it.
    bool told = false;           ///< ... and the fallen and the last human's fall.
    Side* winner = nullptr;      ///< The side that won.
    std::string message;         ///< What to say.
    std::vector<Fallen> fallen;  ///< The sides put out this round.
};

struct Grids;      // move.cpp's cached cost grids
struct PathCache;  // move.cpp's route cache
struct Neighbours;

/**
 * @brief The diplomatic relations between every pair of sides.
 *
 * The 64-entry arrays are indexed by a * 8 + b, for side a's view of side b.
 */
struct Diplomacy {
    std::array<int, 64> state{};            ///< The diplomatic state.
    std::array<int, 64> proposal{};         ///< The state proposed; kNone where none was made.
    std::array<bool, 64> allied{};          ///< Allied.
    std::array<bool, 64> propose_allied{};  ///< An alliance proposed.
    std::array<int, 64> truce_turns{};      ///< Turns of truce left.
    std::array<int, 64> gold_offer{};       ///< > 0 offered by a to b; < 0 demanded by a from b.
    std::array<int, 64> war_target{};       ///< kNone or target side index 0..7 for joint war pact.
    std::array<int, 64> trust{};            ///< Relational trust from a toward b (-100..+100).
    /// Each side's allied target city.
    std::array<int, 8> allied_target_city{kNone, kNone, kNone, kNone, kNone, kNone, kNone, kNone};
};

/** @brief A game in play: the map, the armies, the sides and whose turn it is. */
struct Game {
    Game();
    ~Game();
    Rng rng;                                       ///< The game's dice.
    std::string data_dir;                          ///< Where the original's data files are.
    Types types;                                   ///< The army types.
    std::unique_ptr<Map> map;                      ///< The map and what is on it.
    std::vector<Army*> armies;                     ///< The armies in the game, in slot order.
    std::vector<std::unique_ptr<Army>> pool;       ///< Every army ever made.
    int turn = 1;                                  ///< The round being played.
    std::vector<std::string> log;                  ///< The game's log.
    bool greatest = false;                         ///< A game of the Greatest Warlord campaign.
    std::vector<Side*> sides;                      ///< The sides in play.
    int current = 0;                               ///< The index of the side to play.
    Side* side = nullptr;                          ///< The side to play.
    Diplomacy diplomacy;                           ///< The relations between the sides.
    std::map<int, std::vector<uint8_t>> explored;  ///< By side: the tiles it has seen.
    std::set<int> towers;                          ///< Tiles with an encampment.
    bool won = false;                              ///< The game is won.
    bool over = false;                             ///< The game is over.
    bool no_humans_said = false;                   ///< "No humans left" has been told.
    bool surrender_offered = false;                ///< The surrender offer was made.
    /// The offer is put to the player: it waits for the human's turn,
    /// however many computer turns come between.
    bool surrender_shown = false;
    Ending ending;                           ///< How the game is ending.
    std::vector<HistoryRecord> history;      ///< Every round's record.
    std::map<int, std::vector<Deed>> deeds;  ///< By side: this round's deeds.
    /// By side, then by opponent: what History > Triumphs counts (history::Loss).
    std::map<int, std::map<int, std::array<int, 5>>> triumphs;
    std::set<std::string> tutorial_seen;  ///< The tutorial's moments, as shown.
    /// By side: the hero names, and whether each is a heroine's.
    std::map<int, std::vector<std::pair<std::string, bool>>> hero_names;
    std::shared_ptr<Grids> grids;               ///< Cached movement cost grids.
    std::shared_ptr<Neighbours> ai_neighbours;  ///< The computer's view of who borders whom.
    std::shared_ptr<PathCache> path_cache;      ///< Cached routes.
    /// The turn order (see docs/formats/scenario.md), shuffled each round
    /// with Random Turns.
    std::array<int, 8> turn_order{0, 1, 2, 3, 4, 5, 6, 7};
    int order_at = -1;  ///< The place in turn_order of the side to play.

    /** @brief A new army, owned by the game but not yet in it. */
    Army* Make(const Army& army);
    /**
     * @brief A new army, in the game, in the army table's lowest free slot.
     *
     * `armies` stays in slot order, so a scan from its end is the original's
     * from the top down.
     */
    Army* Add(const Army& army);
    /** @brief Is the army in the game? */
    bool Alive(const Army* army) const;
    /** @brief Takes the army out of the game (it stays in the pool). */
    void Remove(const Army* army);
};

}  // namespace w2
