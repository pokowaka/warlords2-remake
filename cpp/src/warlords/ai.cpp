#include "warlords/ai.hpp"

#include <algorithm>

#include "warlords/ai/belief.hpp"
#include "warlords/ai/cities.hpp"
#include "warlords/ai/core.hpp"
#include "warlords/ai/diplomacy.hpp"
#include "warlords/ai/groups.hpp"
#include "warlords/ai/heroes.hpp"
#include "warlords/ai/modern_ai.hpp"
#include "warlords/ai/moves.hpp"
#include "warlords/aicard.hpp"
#include "warlords/hero.hpp"
#include "warlords/quest.hpp"

namespace w2::ai {

Hooks hooks;

namespace {
bool g_default_classic_ai = true;  ///< What DefaultClassicAi() returns.

/**
 * @brief Fills a side's AI data with its level's built-in settings.
 *
 * Only human sides keep these: a computer side's card overwrites them all
 * (docs/formats/crd.md).
 */
void LevelDefaults(Game& game, AIData& data, int level) {
    Rng& rng = game.rng;
    if (level == 0) {
        data.rebuild_limit = 30;
        data.cautious = 1;
        data.die_human = rng.Dice(1, 4, 0);
        data.die_warlord = rng.Dice(1, 4, 0);
        data.die_lord = rng.Dice(1, 4, 0);
        data.max_groups = 1;
        data.solidarity = 0;
        data.rebuild_type = 3;
        data.rebuild_type_rich = 3;
        data.raze = 0;
        data.sack = 0;
        data.pillage = 0;
        data.per_city = 0;
        data.bonus_human = 0;
        data.bonus_warlord = 0;
        data.bonus_lord = 0;
        data.bonus_knight = 0;
        data.poor = 0;
        data.early = 0;
        data.human_share = 80;
    } else if (level == 1) {
        data.rebuild_limit = 20;
        data.cautious = 0;
        data.die_human = rng.Dice(1, 4, 0);
        data.die_warlord = rng.Dice(1, 4, 0);
        data.die_knight = rng.Dice(1, 8, 0);
        data.bold = true;
        data.solidarity = 0;
        data.max_groups = 2;
        data.rebuild_type = 6;
        data.rebuild_type_rich = 18;
        data.raze = 5;
        data.sack = 10;
        data.pillage = 20;
        data.per_city = 1;
        data.bonus_human = 5;
        data.bonus_knight = 0;
        data.bonus_lord = 0;
        data.bonus_warlord = 0;
        data.poor = 0;
        data.early = 0;
        data.human_share = 35;
    } else {
        data.rebuild_limit = 10;
        data.cautious = 0;
        data.die_human = rng.Dice(1, 10, 0);
        data.die_lord = rng.Dice(1, 8, 0);
        data.die_knight = rng.Dice(1, 6, 0);
        data.max_groups = 4;
        data.solidarity = 1;
        data.bold = true;
        data.rebuild_type = 7;
        data.rebuild_type_rich = 0;
        data.raze = 5;
        data.sack = 10;
        data.pillage = 20;
        data.per_city = 5;
        data.bonus_human = 50;
        data.bonus_knight = 0;
        data.bonus_lord = 0;
        data.bonus_warlord = 0;
        data.poor = 50;
        data.early = 1;
        data.human_share = 35;
    }
}

/** @brief Fills a computer side's AI data from its character card, dice and all. */
void FromCard(Game& game, Side& side, AIData& data, const aicard::Card& card) {
    Rng& rng = game.rng;
    data.die_human = rng.Dice(1, card.die_human, 0);
    data.die_lord = rng.Dice(1, card.die_lord, 0);
    data.die_knight = rng.Dice(1, card.die_knight, 0);
    data.die_warlord = rng.Dice(1, card.die_warlord, 0);
    data.max_groups = std::max(0, std::min(core::kMaxGroups, card.groups));
    data.solidarity = card.solidarity;
    if (card.bold != 0) data.bold = true;
    data.cautious = card.cautious;
    data.rebuild_type = card.rebuild_type;
    data.rebuild_type_rich = card.rebuild_type_rich;
    data.rebuild_limit = card.rebuild_limit;
    data.raze = card.raze;
    data.sack = card.sack;
    data.pillage = card.pillage;
    data.per_city = card.per_city;
    data.bonus_human = card.bonus_human;
    data.bonus_warlord = card.bonus_warlord;
    data.bonus_lord = card.bonus_lord;
    data.bonus_knight = card.bonus_knight;
    data.poor = card.poor;
    data.early = card.early;
    data.human_share = card.human_share * 10 + rng.Dice(1, 10, 0);
    // the card's fight order replaces the side's
    game.map->fight_order[side.index] = card.fight_order;
}

/**
 * @brief Splits the cities no side starts with among the computer players,
 * for the AI to think of as its own ground.
 *
 * The sides take turns claiming the nearest unclaimed city, measured from
 * their capital or, at random, from the city they claimed last.
 */
void ShareOutCities(Game& game) {
    for (auto& city : game.map->cities)
        city.claim = city.owner_index != kNone ? city.owner_index : core::kNeutral;
    bool shares[8] = {false};
    int humans = 0, computers = 0;
    std::pair<int, int> from[8];
    for (int i = 0; i < 8; i++) {
        Side& side = game.map->sides[i];
        if (side.in_use) {
            if (side.computer) {
                shares[i] = true;
                computers++;
            } else
                humans++;
            from[i] = {side.cap_x, side.cap_y};
            City* capital = core::CityAt(game, side.cap_x, side.cap_y);
            if (capital) capital->claim = i;
        }
    }
    if (computers == 0 && humans != 0) {
        int best = kNone, best_roll = 0;
        for (int i = 7; i >= 0; i--) {
            if (game.map->sides[i].in_use) {
                shares[i] = true;
                int roll = game.rng.Dice(1, 100, 0);
                if (best == kNone || best_roll < roll) {
                    best = i;
                    best_roll = roll;
                }
            }
        }
        if (best != kNone) shares[best] = false;
    }
    int turn = kNone;
    for (int i = 7; i >= 0; i--)
        if (shares[i]) {
            turn = i;
            break;
        }
    if (turn == kNone) return;
    for (;;) {
        auto [from_x, from_y] = from[turn];
        City* pick = nullptr;
        int best_distance = kNone;
        for (int i = static_cast<int>(game.map->cities.size()) - 1; i >= 0; i--) {
            City& city = game.map->cities[i];
            if (core::Standing(city) && city.claim == core::kNeutral) {
                int distance = core::Dist(from_x, from_y, city.x, city.y);
                if (best_distance == kNone || distance < best_distance) {
                    pick = &city;
                    best_distance = distance;
                }
            }
        }
        if (!pick) break;
        pick->claim = turn;
        Side& side = game.map->sides[turn];
        if (game.rng.Dice(1, 10, -1) < 5)
            from[turn] = {side.cap_x, side.cap_y};
        else
            from[turn] = {pick->x, pick->y};
        do {
            turn = (turn + 1) % 8;
        } while (!shares[turn]);
    }
}

/**
 * @brief Prepares a side's AI data for its turn.
 *
 * Makes the side at peace with itself, copies its solidarity to the side,
 * retires the explorer roles of the opening and clears each city's
 * cleaned flag.
 */
void TurnSetup(Game& game, Side& side) {
    AIData& data = core::Data(game, side);
    // the side is at peace with itself, state and proposal
    SideId self_id(side.index);
    game.diplomacy.SetState(self_id, self_id, 0);
    game.diplomacy.SetProposal(self_id, self_id, 0);
    side.ai_solidarity = data.solidarity;
    bool quick = game.map->options.quick_start != 0 && game.map->options.hidden_map != 0;
    for (int i = static_cast<int>(game.map->cities.size()) - 1; i >= 0; i--) {
        City& city = game.map->cities[i];
        if (city.owner_index == side.index) {
            int role = core::Role(data, city);
            if (role == core::kExplorer || role == core::kExploreR2)
                core::SetRole(data, city, game.turn < 5 ? core::kWeak : core::kStop);
            if (quick) {
                if (game.turn == 1)
                    core::SetRole(data, city, core::kExplorer);
                else if (game.turn < 3)
                    core::SetRole(data, city, core::kExploreR2);
            }
            core::ClearCflag(data, city, core::kCfCleaned);
        }
    }
}
}  // namespace

bool DefaultClassicAi() {
    return g_default_classic_ai;
}
void SetDefaultClassicAi(bool classic) {
    g_default_classic_ai = classic;
}

AIData& InitSide(Game& game, Side& side) {
    side.classic_ai = g_default_classic_ai;
    belief::Ensure(side);
    side.ai = std::make_shared<AIData>(core::NewData());
    AIData& data = *side.ai;
    bool quick = game.map->options.quick_start != 0;
    for (auto& city : game.map->cities) {
        data.roles[city.index] = quick ? core::kWeak : 0;
        data.held[city.index] = 0;
        data.flags[city.index] = game.map->options.hidden_map != 0 ? core::kCfUnseen : 0;
    }
    int level = 2;
    if (side.computer && (side.level == 0 || side.level == 1)) level = side.level;
    LevelDefaults(game, data, level);
    if (side.in_use && side.computer) {
        auto card = aicard::Load(game.data_dir, level, side.card);
        if (card) FromCard(game, side, data, *card);
    }
    return data;
}

void StartGame(Game& game) {
    // the neighbour table, from the cities as the game begins
    game.ai_neighbours = core::BuildNeighbours(game);
    for (auto& side : game.map->sides) InitSide(game, side);
    ShareOutCities(game);
    for (auto& side : game.map->sides) {
        side.diplo_score = game.rng.Dice(1, 8, 0);
        if (game.greatest && !side.computer) side.diplo_score += 400;
    }
}

void PlayTurn(Game& game, Side& side, const std::function<void(const char*)>& pause) {
    if (!side.classic_ai) {
        modern::PlayTurn(game, side, pause);
        return;
    }
    auto pause_after = [&](const char* name) {
        if (pause) pause(name);
    };
    bool hidden = game.map->options.hidden_map != 0;

    for (Army* army : game.armies)
        if (army->owner == side.index) army->ai_moved = false;

    TurnSetup(game, side);
    diplo::Phase(game, side);
    pause_after("diplomacy");
    heroes::Phase(game, side);
    pause_after("move hero");
    if (hidden) moves::Search(game, side);
    pause_after("move search");
    moves::HeroParties(game, side);
    pause_after("move explore");
    groups::Assault(game, side);
    pause_after("assault");
    moves::MoveAll(game, side);
    pause_after("move #1");
    moves::Rescue(game, side);
    pause_after("rescue");
    cities::Evaluate(game, side, side.index);
    pause_after("evaluate");
    cities::Clean(game, side);
    pause_after("clean city");
    cities::Neutral(game, side);
    pause_after("neutral");
    moves::MoveAll(game, side);
    pause_after("move #2");
    cities::QuickAttack(game, side);
    if (hidden) cities::UpdateHide(game, side);
    groups::AssaultXx(game, side);
    pause_after("assault XX");
    moves::Specials(game, side);
    pause_after("specials");
    cities::Rebuild(game, side);
    pause_after("rebuilding");
    moves::LastRescue(game, side);
    pause_after("last rescue");
    cities::Production(game, side);
    pause_after("production");
    cities::Vectoring(game, side);
    pause_after("vectoring");
}

void Walked(Game& game, const std::vector<Army*>& stack, const move::WalkResult& walk) {
    if (hooks.on_walk && walk.steps > 0) hooks.on_walk(game, stack, walk);
}

City* HeroCity(Game& game, Side& side, City* fallback) {
    if (!side.classic_ai) {
        return modern::SelectHeroCity(game, side, fallback);
    }
    AIData& data = core::Data(game, side);
    City* best = fallback;
    int best_score = 0;
    for (int i = static_cast<int>(game.map->cities.size()) - 1; i >= 0; i--) {
        City& city = game.map->cities[i];
        if (city.owner_index == side.index) {
            int role = core::Role(data, city);
            int score = 0;
            if (role == core::kRally)
                score = game.rng.Dice(1, 100, 100);
            else if (role == core::kTakingNeutral)
                score = game.rng.Dice(1, 100, 50);
            else if (role == core::kNearNeutral)
                score = game.rng.Dice(1, 100, 0);
            if (best_score < score) {
                best = &city;
                best_score = score;
            }
        }
    }
    return best;
}

bool QuestCapture(Game& game, Side& side, City& city, const std::vector<Army*>& stack) {
    auto side_quest = side.quest;
    if (game.map->options.quests == 0 || !side_quest ||
        side_quest->TargetCityId() != CityId(city.index)) {
        return false;
    }
    if (side_quest->Type() != quest::kOccupy && side_quest->Type() != quest::kRaze) return false;
    bool hero_in_stack = side_quest->HeroId().IsValid() &&
                         std::any_of(stack.begin(), stack.end(), [&side_quest](const Army* member) {
                             return member && member->id == side_quest->HeroId();
                         });
    if (!hero_in_stack) return false;
    AIData& data = core::Data(game, side);
    data.quests_done++;
    data.quest_city = kNone;
    if (side_quest->Type() == quest::kRaze) {
        core::Sel sel = core::Select(game, stack);
        groups::Raze(game, side, city, true, &sel);
        return true;
    }
    return false;
}

void RecordBattle(Game& game, int defender, int attacker, int x, int y, int heroes_lost,
                  int armies_lost, bool all_lost, bool city_tile) {
    if (defender == kNone || attacker == kNone || defender == core::kNeutral) return;
    if (defender < 0 || defender > 7 || attacker < 0 || attacker > 7) return;
    AIData& data = core::Data(game, defender);
    data.heroes_killed[attacker] += heroes_lost;
    data.armies_killed[attacker] += armies_lost;
    data.battles[attacker]++;
    if (all_lost) data.lost[attacker]++;
    if (city_tile) {
        data.city_battles[attacker]++;
        if (all_lost) data.cities_lost[attacker]++;
    }
    // Update fair-information belief state for both participants
    belief::RecordBattleObservation(game, defender, x, y);
    belief::RecordBattleObservation(game, attacker, x, y);
}

}  // namespace w2::ai
