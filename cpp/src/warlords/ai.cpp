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
bool g_default_classic_ai = true;
// The level's built-in settings (59bf:0d7b).
void LevelDefaults(Game& g, AIData& d, int level) {
    Rng& r = g.rng;
    if (level == 0) {
        d.rebuild_limit = 30;
        d.cautious = 1;
        d.die_human = r.Dice(1, 4, 0);
        d.die_warlord = r.Dice(1, 4, 0);
        d.die_lord = r.Dice(1, 4, 0);
        d.max_groups = 1;
        d.solidarity = 0;
        d.rebuild_type = 3;
        d.rebuild_type_rich = 3;
        d.raze = 0;
        d.sack = 0;
        d.pillage = 0;
        d.per_city = 0;
        d.bonus_human = 0;
        d.bonus_warlord = 0;
        d.bonus_lord = 0;
        d.bonus_knight = 0;
        d.poor = 0;
        d.early = 0;
        d.human_share = 80;
    } else if (level == 1) {
        d.rebuild_limit = 20;
        d.cautious = 0;
        d.die_human = r.Dice(1, 4, 0);
        d.die_warlord = r.Dice(1, 4, 0);
        d.die_knight = r.Dice(1, 8, 0);
        d.bold = true;
        d.solidarity = 0;
        d.max_groups = 2;
        d.rebuild_type = 6;
        d.rebuild_type_rich = 18;
        d.raze = 5;
        d.sack = 10;
        d.pillage = 20;
        d.per_city = 1;
        d.bonus_human = 5;
        d.bonus_knight = 0;
        d.bonus_lord = 0;
        d.bonus_warlord = 0;
        d.poor = 0;
        d.early = 0;
        d.human_share = 35;
    } else {
        d.rebuild_limit = 10;
        d.cautious = 0;
        d.die_human = r.Dice(1, 10, 0);
        d.die_lord = r.Dice(1, 8, 0);
        d.die_knight = r.Dice(1, 6, 0);
        d.max_groups = 4;
        d.solidarity = 1;
        d.bold = true;
        d.rebuild_type = 7;
        d.rebuild_type_rich = 0;
        d.raze = 5;
        d.sack = 10;
        d.pillage = 20;
        d.per_city = 5;
        d.bonus_human = 50;
        d.bonus_knight = 0;
        d.bonus_lord = 0;
        d.bonus_warlord = 0;
        d.poor = 50;
        d.early = 1;
        d.human_share = 35;
    }
}

// Fill a computer side's AI data from its card (59bf:0d7b), dice and all.
void FromCard(Game& g, Side& side, AIData& d, const aicard::Card& card) {
    Rng& r = g.rng;
    d.die_human = r.Dice(1, card.die_human, 0);
    d.die_lord = r.Dice(1, card.die_lord, 0);
    d.die_knight = r.Dice(1, card.die_knight, 0);
    d.die_warlord = r.Dice(1, card.die_warlord, 0);
    d.max_groups = std::max(0, std::min(core::kMaxGroups, card.groups));
    d.solidarity = card.solidarity;
    if (card.bold != 0) d.bold = true;
    d.cautious = card.cautious;
    d.rebuild_type = card.rebuild_type;
    d.rebuild_type_rich = card.rebuild_type_rich;
    d.rebuild_limit = card.rebuild_limit;
    d.raze = card.raze;
    d.sack = card.sack;
    d.pillage = card.pillage;
    d.per_city = card.per_city;
    d.bonus_human = card.bonus_human;
    d.bonus_warlord = card.bonus_warlord;
    d.bonus_lord = card.bonus_lord;
    d.bonus_knight = card.bonus_knight;
    d.poor = card.poor;
    d.early = card.early;
    d.human_share = card.human_share * 10 + r.Dice(1, 10, 0);
    // the card's fight order replaces the side's
    g.map->fight_order[side.index] = card.fight_order;
}

// Split the cities no side starts with among the computer players, for the
// AI to think of as its own ground (623c:010b).
void ShareOutCities(Game& g) {
    for (auto& c : g.map->cities) c.claim = c.owner_index != kNone ? c.owner_index : core::kNeutral;
    bool shares[8] = {false};
    int humans = 0, computers = 0;
    std::pair<int, int> from[8];
    for (int i = 0; i < 8; i++) {
        Side& s = g.map->sides[i];
        if (s.in_use) {
            if (s.computer) {
                shares[i] = true;
                computers++;
            } else
                humans++;
            from[i] = {s.cap_x, s.cap_y};
            City* c = core::CityAt(g, s.cap_x, s.cap_y);
            if (c) c->claim = i;
        }
    }
    if (computers == 0 && humans != 0) {
        int best = kNone, best_roll = 0;
        for (int i = 7; i >= 0; i--) {
            if (g.map->sides[i].in_use) {
                shares[i] = true;
                int roll = g.rng.Dice(1, 100, 0);
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
        auto [x, y] = from[turn];
        City* pick = nullptr;
        int best_d = kNone;
        for (int i = static_cast<int>(g.map->cities.size()) - 1; i >= 0; i--) {
            City& c = g.map->cities[i];
            if (core::Standing(c) && c.claim == core::kNeutral) {
                int d = core::Dist(x, y, c.x, c.y);
                if (best_d == kNone || d < best_d) {
                    pick = &c;
                    best_d = d;
                }
            }
        }
        if (!pick) break;
        pick->claim = turn;
        Side& s = g.map->sides[turn];
        if (g.rng.Dice(1, 10, -1) < 5)
            from[turn] = {s.cap_x, s.cap_y};
        else
            from[turn] = {pick->x, pick->y};
        do {
            turn = (turn + 1) % 8;
        } while (!shares[turn]);
    }
}

// ai_turn_setup (5db9:0386).
void TurnSetup(Game& g, Side& side) {
    AIData& d = core::Data(g, side);
    // the side is at peace with itself, state and proposal (5db9:0000)
    g.diplomacy.state[static_cast<size_t>(side.index) * 9] = 0;
    g.diplomacy.proposal[static_cast<size_t>(side.index) * 9] = 0;
    side.ai_solidarity = d.solidarity;
    bool quick = g.map->options.quick_start != 0 && g.map->options.hidden_map != 0;
    for (int i = static_cast<int>(g.map->cities.size()) - 1; i >= 0; i--) {
        City& c = g.map->cities[i];
        if (c.owner_index == side.index) {
            int r = core::Role(d, c);
            if (r == core::kExplorer || r == core::kExploreR2)
                core::SetRole(d, c, g.turn < 5 ? core::kWeak : core::kStop);
            if (quick) {
                if (g.turn == 1)
                    core::SetRole(d, c, core::kExplorer);
                else if (g.turn < 3)
                    core::SetRole(d, c, core::kExploreR2);
            }
            core::ClearCflag(d, c, core::kCfCleaned);
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

AIData& InitSide(Game& g, Side& side) {
    side.classic_ai = g_default_classic_ai;
    belief::Ensure(side);
    side.ai = std::make_shared<AIData>(core::NewData());
    AIData& d = *side.ai;
    bool quick = g.map->options.quick_start != 0;
    for (auto& c : g.map->cities) {
        d.roles[c.index] = quick ? core::kWeak : 0;
        d.held[c.index] = 0;
        d.flags[c.index] = g.map->options.hidden_map != 0 ? core::kCfUnseen : 0;
    }
    int level = 2;
    if (side.computer && (side.level == 0 || side.level == 1)) level = side.level;
    LevelDefaults(g, d, level);
    if (side.in_use && side.computer) {
        auto card = aicard::Load(g.data_dir, level, side.card);
        if (card) FromCard(g, side, d, *card);
    }
    return d;
}

void StartGame(Game& g) {
    // the neighbour table, from the cities as the game begins (623c:0398)
    g.ai_neighbours = core::BuildNeighbours(g);
    for (auto& s : g.map->sides) InitSide(g, s);
    ShareOutCities(g);
    for (auto& s : g.map->sides) {
        s.diplo_score = g.rng.Dice(1, 8, 0);
        if (g.greatest && !s.computer) s.diplo_score += 400;
    }
}

void PlayTurn(Game& g, Side& side, const std::function<void(const char*)>& pause) {
    if (!side.classic_ai) {
        modern::PlayTurn(g, side, pause);
        return;
    }
    auto p = [&](const char* name) {
        if (pause) pause(name);
    };
    bool hidden = g.map->options.hidden_map != 0;

    for (Army* a : g.armies)
        if (a->owner == side.index) a->ai_moved = false;

    TurnSetup(g, side);
    diplo::Phase(g, side);
    p("diplomacy");
    heroes::Phase(g, side);
    p("move hero");
    if (hidden) moves::Search(g, side);
    p("move search");
    moves::HeroParties(g, side);
    p("move explore");
    groups::Assault(g, side);
    p("assault");
    moves::MoveAll(g, side);
    p("move #1");
    moves::Rescue(g, side);
    p("rescue");
    cities::Evaluate(g, side, side.index);
    p("evaluate");
    cities::Clean(g, side);
    p("clean city");
    cities::Neutral(g, side);
    p("neutral");
    moves::MoveAll(g, side);
    p("move #2");
    cities::QuickAttack(g, side);
    if (hidden) cities::UpdateHide(g, side);
    groups::AssaultXx(g, side);
    p("assault XX");
    moves::Specials(g, side);
    p("specials");
    cities::Rebuild(g, side);
    p("rebuilding");
    moves::LastRescue(g, side);
    p("last rescue");
    cities::Production(g, side);
    p("production");
    cities::Vectoring(g, side);
    p("vectoring");
}

void Walked(Game& g, const std::vector<Army*>& stack, const move::WalkResult& r) {
    if (hooks.on_walk && r.steps > 0) hooks.on_walk(g, stack, r);
}

City* HeroCity(Game& g, Side& side, City* dflt) {
    if (!side.classic_ai) {
        return modern::SelectHeroCity(g, side, dflt);
    }
    AIData& d = core::Data(g, side);
    City* best = dflt;
    int best_score = 0;
    for (int i = static_cast<int>(g.map->cities.size()) - 1; i >= 0; i--) {
        City& c = g.map->cities[i];
        if (c.owner_index == side.index) {
            int r = core::Role(d, c);
            int score = 0;
            if (r == core::kRally)
                score = g.rng.Dice(1, 100, 100);
            else if (r == core::kTakingNeutral)
                score = g.rng.Dice(1, 100, 50);
            else if (r == core::kNearNeutral)
                score = g.rng.Dice(1, 100, 0);
            if (best_score < score) {
                best = &c;
                best_score = score;
            }
        }
    }
    return best;
}

bool QuestCapture(Game& g, Side& side, City& c, const std::vector<Army*>& stack) {
    auto q = side.quest;
    if (g.map->options.quests == 0 || !q || q->city != &c) return false;
    if (q->type != quest::kOccupy && q->type != quest::kRaze) return false;
    if (std::find(stack.begin(), stack.end(), q->hero) == stack.end()) return false;
    AIData& d = core::Data(g, side);
    d.quests_done++;
    d.quest_city = kNone;
    if (q->type == quest::kRaze) {
        core::Sel sel = core::Select(g, stack);
        groups::Raze(g, side, c, true, &sel);
        return true;
    }
    return false;
}

void RecordBattle(Game& g, int defender, int attacker, int x, int y, int heroes_lost,
                  int armies_lost, bool all_lost, bool city_tile) {
    if (defender == kNone || attacker == kNone || defender == core::kNeutral) return;
    if (defender < 0 || defender > 7 || attacker < 0 || attacker > 7) return;
    AIData& d = core::Data(g, defender);
    d.heroes_killed[attacker] += heroes_lost;
    d.armies_killed[attacker] += armies_lost;
    d.battles[attacker]++;
    if (all_lost) d.lost[attacker]++;
    if (city_tile) {
        d.city_battles[attacker]++;
        if (all_lost) d.cities_lost[attacker]++;
    }
    // Update fair-information belief state for both participants
    belief::RecordBattleObservation(g, defender, x, y);
    belief::RecordBattleObservation(g, attacker, x, y);
}

}  // namespace w2::ai
