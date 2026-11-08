// The game's state, as structs. The Lua keeps it in tables that grow fields
// as they go; here every field is declared, and Lua's nil is NONE (-1) for an
// index or a coordinate, or std::optional where -1 is a real value.
//
// A Game owns every army it ever made (`pool`), so a pointer held by a quest,
// an AI group or the selection stays valid after the army has left
// `armies`. Being in the game is being in `armies`.
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

constexpr int kNone = -1;

struct Side;
struct City;
struct Army;
struct Item;
struct Game;
struct AIData;
struct SideBelief;

// ------------------------------------------------------------- army types

struct ArmyType {
    int id = 0;  // also the sprite index
    std::string name;
    int strength = 0, time = 0;
    int cost = 0;  // per-turn upkeep basis
    int move = 0;
    int price = 0;                // negative: can never be bought
    std::array<int, 64> bonus{};  // keyed by field offset, 32..60
    bool flies = false, siege = false, boat = false, woods_move = false, hills_move = false;
};

struct Types {
    std::vector<ArmyType> all;        // in file (display) order
    std::array<ArmyType*, 64> ids{};  // by type id
    const ArmyType* ById(int id) const { return id >= 0 && id < 64 ? ids[id] : nullptr; }
    std::vector<const ArmyType*> List() const {
        std::vector<const ArmyType*> out;
        for (auto& a : all) out.push_back(&a);
        return out;
    }
};

// --------------------------------------------------------------- the map

/** A production slot: an army type with the city's own numbers for it. */
struct Slot {
    int type = 0;
    std::string name;
    int strength = 0, time = 0, cost = 0, move = 0, price = 0;
    bool operator==(const Slot& o) const {
        return type == o.type && name == o.name && strength == o.strength && time == o.time &&
               cost == o.cost && move == o.move && price == o.price;
    }
};

struct City {
    int index = 0;
    int x = 0, y = 0;
    std::string name;
    int income = 0;
    std::vector<int> produces;  // army type ids, as the scenario gives them
    int defence = 0;
    Side* owner = nullptr;    // only while the scenario loads
    int owner_index = kNone;  // the side holding it; NONE for neutral
    std::vector<Slot> slots;
    int producing = kNone;  // slot index being built
    int countdown = 0;
    int vector_to = kNone;  // a city index, or STANDARD
    bool razed = false;
    int razed_by = kNone;
    int claim = kNone;  // the computer whose ground it is; after a
                        // capture, the side it was taken from (+0x2f)
    int attacked = 0;   // a bit per side that has attacked it (+0x30)
};

struct Item {
    int index = 0;
    std::string name;
    int type = 0, value = 0;
    int status = 0;  // 0 out of play, 1 on the ground, 2 hidden, 3 carried
    int x = kNone, y = kNone;
    bool planted = false;
    int standard_of = kNone;  // items 0-7: the eight sides' standards
};

struct PoolItem {
    std::string name;
    int type = 0, value = 0;
};

struct Site {
    int index = 0;
    int x = 0, y = 0;
    std::string name;
    int type = 0;  // 1 = temple, 2 = ruin
    int content = 0;
    int item = kNone;
    int guardian = kNone;
    int ally_type = kNone;
    bool rich = false;
    int revealed = 0;  // a bit per side the site is shown to
    bool searched = false;
    std::string band;  // "rich", "near" or "far"
    int temple_index = kNone;
};

struct Monster {
    int index = 0;
    std::string name;
    int strength = 0;
};

struct Sign {
    int index = 0;
    int x = 0, y = 0;
    std::array<std::string, 2> lines;
};

struct Options {
    int neutral_cities = 0, diplomacy = 0, quests = 0, random_turns = 0, hidden_map = 0;
    int intense_combat = 0, quick_start = 0, view_enemies = 0, military_advisor = 1;
    int tutorial = 0, view_production = 0;
    /** By the Lua's name for it; unknown names are ignored. */
    int* Field(const std::string& name);
    static const std::vector<std::string>& Names();
};

// ------------------------------------------------------------------ sides

/** What a side's turn start produced, for the report (side.produced). */
struct Produced {
    std::string kind;  // "built", "sent", "arrived"
    int type = 0;
    int city = kNone;
    bool standard = false;
};

/** A quest (quest.lua). The target is one of the pointers, as targetKind says. */
struct Quest {
    int type = 0;
    Army* hero = nullptr;
    std::string target_kind;  // "city", "side", "item", "armytype", "army", "none"
    City* city = nullptr;
    Side* side = nullptr;
    Item* item = nullptr;
    const ArmyType* army_type = nullptr;
    Army* army = nullptr;
    int required = kNone;
    int done = 0;
    bool HasTarget() const {
        return target_kind == "none" || city || side || item || army_type || army;
    }
};

struct Reward;
struct QuestResult;

/** The advisor's marks (cues.lua). */
struct Advisor {
    int dir = 0, mark = 0;
};

struct HeroOffer;

/** A hero promoted as the turn opened, and the strength it gained. */
struct Promotion {
    Army* hero = nullptr;
    int gain = 0;
};

/** A scored macro-action recorded by the modern ML AI for the in-game F10 Brain Inspector. */
struct AIThought {
    std::string category;
    std::string actor_name;
    std::string target_name;
    int sx = kNone, sy = kNone;
    int tx = kNone, ty = kNone;
    float score = 0.0F;
    float win_prob = 0.0F;
    bool stage_near = false;
    std::string detail;
};

/** A turn-start diplomatic parley missive sent from an AI side to a player side. */
struct DiplomaticMissive {
    int from_side = kNone;
    int to_side = kNone;
    int proposed_state = kNone;
    bool propose_alliance = false;
    int gold_demand = 0;  // > 0: fromSide demands gold; < 0: fromSide offers gold
    int joint_war_target = kNone;
    std::string headline;
    std::string body;
    std::string reason_summary;
};

struct Side {
    int index = 0;
    std::string name;
    int colour = 0, edge = 0;
    int gold = 0;
    int cap_x = 0, cap_y = 0;
    bool computer = false;
    bool classic_ai = true;
    int level = 0;
    bool enhanced = false;
    bool observe = false;
    int diplo_score = 0;
    int card = 0;
    bool in_use = false;
    City* capital = nullptr;
    bool alive = false;
    int owner_index = 0;
    int income = 0, upkeep_total = 0;
    std::vector<Produced> produced;
    std::vector<std::string> diplo_news;
    std::shared_ptr<HeroOffer> hero_offer;
    std::shared_ptr<Quest> quest;
    std::shared_ptr<QuestResult> quest_news;
    std::shared_ptr<AIData> ai;
    std::shared_ptr<SideBelief> belief;
    std::vector<AIThought> ai_thoughts;
    std::vector<DiplomaticMissive> missives;
    std::array<int, 8> last_missive_turn{};
    float last_board_value = 0.0F;
    int ai_solidarity = 0;
    Advisor advisor;
    // where the capital now stands (67cc:10d7); NONE until the first turn
    int cap_now_x = kNone, cap_now_y = kNone;
    bool production_due = false;      // a human's cities build once the offer is settled
    std::vector<Promotion> promoted;  // for the interface to show
};

// ------------------------------------------------------------------ armies

struct Transit {
    int turns = 0;
    int dest = kNone;  // a city index, or STANDARD
};

struct Army {
    int slot = 0;              // its place in the army table (1b62:0309)
    int x = kNone, y = kNone;  // NONE while in transit
    int owner = kNone;         // NONE for neutral
    int type = 0;
    std::string name;
    bool female = false;
    int strength = 0;
    int moves = 0, max_moves = 0;
    int upkeep = 0;
    int home_city = kNone;
    int medals = 0;
    bool at_sea = false;
    int group = 0;
    std::optional<std::pair<int, int>> target;  // where it is walking to
    bool fortified = false;
    // heroes
    int level = 0, experience = 0;
    std::string title;
    std::vector<Item*> items;
    std::set<int> blessings;  // temples that have blessed it
    std::optional<Transit> transit;
    bool returning = false;
    // the turn's marks
    bool done = false, offered = false, disbanded = false;
    // the computer player's
    int ai_order = 0;
    int ai_dest = kNone;
    int ai_group = 0;
    bool ai_explore = false, ai_neutral = false, ai_party = false, ai_moved = false;

    bool Hero() const;
};

// ----------------------------------------------------------------- the map

constexpr int kMapW = 112, kMapH = 156;

struct Map {
    std::string name;
    std::array<Side, 8> sides;
    std::vector<City> cities;
    std::vector<City*> city_at;    // a city's top-left tile
    std::vector<City*> city_tile;  // every tile of its 2x2 footprint
    std::vector<Site> sites;
    std::vector<Site*> site_at;
    std::vector<Item> items;
    std::array<std::optional<Monster>, 10> monsters;
    std::vector<bool> crossing;
    std::optional<std::vector<PoolItem>> item_pool;
    std::map<int, std::array<std::string, 3>> city_text, site_text;
    std::vector<Sign> signs;
    Options options;
    std::array<int, 256> terrain_type{};
    std::vector<std::vector<int>> fight_order;  // 9 rows of 29, row 8 neutral
    int combat_cap = 0;
    std::vector<int> tiles;
    std::string roads;
    int width = kMapW, height = kMapH;
    int active_x0 = 0, active_y0 = 0, active_x1 = kMapW - 1, active_y1 = kMapH - 1;

    Side* Side(int i) { return i >= 0 && i < 8 ? &sides[i] : nullptr; }
    City* City(int i) {
        return i >= 0 && i < static_cast<int>(cities.size()) ? &cities[i] : nullptr;
    }
};

// ----------------------------------------------------------------- the game

/** What a deed was: one of the events History plays back (STRING.DAT group
 *  94, docs/formats/history.md). The order is also each deed's rank: a side
 *  keeps only its two most important deeds of a round, and a lower value
 *  counts for more. The comments say what a deed's two values hold. */
enum class DeedType : int {
    kHeroEmerges = 0,  // v1: the city the hero appeared in
    kHeroKilled = 1,   // v1: the city, or history::KILLED_IN_BATTLE / KILLED_SEARCHING
    kQuestCompleted = 2,
    kQuestReceived = 3,
    kSideVanquished = 4,  // v1: the side
    kCityWon = 5,         // v1: the side, or history::WON_BY_NAME; v2: the city
    kFound = 6,           // v1: the item, or history::FOUND_ALLIES / FOUND_SAGE / FOUND_GOLD
    kSideVictorious = 7,  // v1: the side
    kTreachery = 8,       // v1: the side that attacked at peace, v2: its victim
    kAtWar = 9,           // v1, v2: the two sides
    kAtPeace = 10,        // v1, v2: the two sides
};

/** A deed of the round, for the history: who did it, what, two values whose
 *  meaning depends on the type, and the hero's or the side's name. */
struct Deed {
    int side = 0;
    DeedType type = DeedType::kHeroEmerges;
    int v1 = 0, v2 = 0;
    std::string name;
};
/** A round's record: what History plays back. */
struct HistoryRecord {
    std::array<int, 8> gold{}, score{}, cities{};
    std::vector<int> owners;
    std::vector<Deed> events;
};
/** A side put out at the round's end (8065:18ab), and how it is told: a
 *  line of STRING.DAT group 11, in a box or in the status bar. */
struct Fallen {
    Side* side = nullptr;
    int line = 0;
    bool boxed = false;
};
struct Ending {
    bool over = false, won = false, triumph = false, surrender = false, no_humans = false;
    bool shown = false;  // the front end has told it
    bool told = false;   // ... and the fallen and the last human's fall
    Side* winner = nullptr;
    std::string message;
    std::vector<Fallen> fallen;
};

struct Grids;      // move.cpp's cached cost grids
struct PathCache;  // move.cpp's route cache (1555:1508)
struct Neighbours;

struct Diplomacy {
    std::array<int, 64> state{};
    std::array<int, 64> proposal{};  // NONE where none was made
    std::array<bool, 64> allied{};
    std::array<bool, 64> propose_allied{};
    std::array<int, 64> truce_turns{};
    std::array<int, 64> gold_offer{};  // > 0 offered by a to b; < 0 demanded by a from b
    std::array<int, 64> war_target{};  // NONE or target side index 0..7 for joint war pact
    std::array<int, 64> trust{};       // relational trust from a toward b (-100..+100)
    std::array<int, 8> allied_target_city{kNone, kNone, kNone, kNone, kNone, kNone, kNone, kNone};
};

struct Game {
    Game();
    ~Game();
    Rng rng;
    std::string data_dir;
    Types types;
    std::unique_ptr<Map> map;
    std::vector<Army*> armies;
    std::vector<std::unique_ptr<Army>> pool;
    int turn = 1;
    std::vector<std::string> log;
    bool greatest = false;
    std::vector<Side*> sides;
    int current = 0;
    Side* side = nullptr;
    Diplomacy diplomacy;
    std::map<int, std::vector<uint8_t>> explored;
    std::set<int> towers;
    bool won = false, over = false, no_humans_said = false, surrender_offered = false;
    // the offer put to the player (2c04:015f): it waits for the human's turn,
    // however many computer turns come between
    bool surrender_shown = false;
    Ending ending;
    std::vector<HistoryRecord> history;
    std::map<int, std::vector<Deed>> deeds;
    std::map<int, std::map<int, std::array<int, 5>>> triumphs;
    std::set<std::string> tutorial_seen;  // the tutorial's moments, as shown
    std::map<int, std::vector<std::pair<std::string, bool>>> hero_names;
    std::shared_ptr<Grids> grids;
    std::shared_ptr<Neighbours> ai_neighbours;
    std::shared_ptr<PathCache> path_cache;
    // the turn order (.SCN 0x163), shuffled each round with Random Turns, and
    // the place in it of the side to play
    std::array<int, 8> turn_order{0, 1, 2, 3, 4, 5, 6, 7};
    int order_at = -1;

    /** A new army, owned by the game but not yet in it. */
    Army* Make(const Army& a);
    /** A new army, in the game, in the army table's lowest free slot
     *  (1b62:0309); `armies` stays in slot order, so a scan from its end is the
     *  original's from the top down. */
    Army* Add(const Army& a);
    bool Alive(const Army* a) const;
    void Remove(const Army* a);
};

}  // namespace w2
