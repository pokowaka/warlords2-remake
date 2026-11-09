/**
 * @file
 * @brief Implementation of `TurnEngine`.
 */
#include "warlords/turn_engine.hpp"

#include <algorithm>
#include <map>
#include <set>

#include "warlords/armytype.hpp"
#include "warlords/city_economy.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/game.hpp"
#include "warlords/hero.hpp"
#include "warlords/history.hpp"
#include "warlords/move.hpp"
#include "warlords/quest.hpp"
#include "warlords/rules.hpp"
#include "warlords/scn.hpp"

namespace w2 {

namespace {

bool CanFlyOff(const Game& game, const std::vector<Army*>& armies) {
    bool hero_flight = false, all_fly = true, non_heroes_fly = true, free_flier = false;
    for (Army* army : armies) {
        bool flies = game.types.ById(army->type)->flies;
        if (army->type == armytype::kHero) {
            for (Item* item : army->items) {
                if (item->type == rules::kItemFlight) hero_flight = true;
            }
        }
        if (!flies || army->at_sea) all_fly = false;
        if (army->type != armytype::kHero && !flies) non_heroes_fly = false;
        if (flies && !army->at_sea) free_flier = true;
    }
    return hero_flight || all_fly || (non_heroes_fly && free_flier);
}

}  // namespace

Fallen TurnEngine::EliminateSide(Side& side, bool boxed) {
    ArmyStack mine;
    for (int i = static_cast<int>(game_.armies.size()) - 1; i >= 0; i--) {
        if (game_.armies[i]->owner == side.index) mine.push_back(game_.armies[i]);
    }
    Disband(side, mine);
    history::RecordDeed(game_, &side, DeedType::kSideVanquished, side.index, 0, "");
    int line = game_.rng.Dice(1, game::kFallenLines, -1);
    game_.log.push_back(side.name + " has been eliminated.");
    side.alive = false;
    side.gold = 0;
    game_.diplomacy.ResetFallenSide(SideId(side.index), diplomacy::kIntermediate);
    return Fallen{&side, line, boxed};
}

std::vector<Fallen> TurnEngine::EliminateFallen() {
    bool human = false;
    for (Side* side : game_.sides) {
        if (side->alive && !side->computer) human = true;
    }
    std::vector<Fallen> fallen;
    for (Side* side : game_.sides) {
        if (side->alive && game::SideCities(game_, *side).empty())
            fallen.push_back(EliminateSide(*side, human || !side->computer));
    }
    return fallen;
}

void TurnEngine::ResetMovement(Side& side) {
    for (Army* army : game_.armies) {
        if (army->owner == side.index) {
            army->done = false;
            army->offered = false;
        }
    }
    for (int i = static_cast<int>(game_.armies.size()) - 1; i >= 0; i--) {
        Army* army = game_.armies[i];
        if (army->owner == side.index && !army->transit) {
            int carry = std::min(army->moves, rules::kMoveCarry);
            army->moves = (army->at_sea ? rules::kSeaMoves : army->max_moves) + carry;
            if (army->target && army->target->first == army->x && army->target->second == army->y)
                army->target.reset();
        }
    }
    for (int i = static_cast<int>(game_.armies.size()) - 1; i >= 0; i--) {
        Army* hero = game_.armies[i];
        if (hero->owner != side.index || hero->type != armytype::kHero || hero->transit) continue;
        bool double_move = false;
        for (Item* item : hero->items) {
            if (item->type == rules::kItemDoubleMove) double_move = true;
        }
        if (!double_move) continue;
        auto here = game::ArmiesAt(game_, hero->x, hero->y);
        if (static_cast<int>(here.size()) > rules::kMaxStack) here.resize(rules::kMaxStack);
        for (Army* army : here) army->moves += army->max_moves;
    }
    for (Army* army : game_.armies) {
        if (army->owner == side.index && army->moves > rules::kMaxMove)
            army->moves = rules::kMaxMove;
    }
}

void TurnEngine::Encamp(Side& side) {
    static const std::set<int> kEncampOn = {move::kPlain,  move::kForest, move::kHills,
                                            move::kBridge, move::kMarsh,  move::kTower};
    for (Army* army : game_.armies) {
        if (army->owner == side.index && army->fortified && !army->transit && army->x != kNone &&
            army->moves >= army->max_moves) {
            bool road = scn::RoadAt(*game_.map, army->x, army->y) % 32 != 0;
            if (road || kEncampOn.count(scn::TerrainAt(*game_.map, army->x, army->y))) {
                game_.towers.insert(army->y * game_.map->width + army->x);
            }
        }
    }
}

void TurnEngine::ComeAshore(Side& side) {
    for (Army* army : game_.armies) {
        if (army->owner == side.index && army->at_sea && !army->transit && army->x != kNone &&
            game::CityAt(game_, army->x, army->y)) {
            army->at_sea = false;
        }
    }
}

void TurnEngine::FollowCapital(Side& side) {
    if (side.cap_now_x == kNone) {
        side.cap_now_x = side.cap_x;
        side.cap_now_y = side.cap_y;
    }
    int playing = 0;
    for (Side* other : game_.sides) {
        if (other->alive) playing++;
    }
    if (playing != 1) {
        City* cap_city = game::CityAt(game_, side.cap_x, side.cap_y);
        if (cap_city && cap_city->razed) cap_city = nullptr;
        if (!game::SideCities(game_, side).empty() &&
            (!cap_city || cap_city->owner_index != side.index)) {
            City* nearest = CityEconomy(game_).NearestCity(side.cap_now_x, side.cap_now_y, &side);
            if (nearest) {
                side.cap_now_x = nearest->x;
                side.cap_now_y = nearest->y;
            }
            return;
        }
    }
    side.cap_now_x = side.cap_x;
    side.cap_now_y = side.cap_y;
}

void TurnEngine::NotifyTurnQuest(Side& side) {
    auto quest_result = quest::Notify(game_, side, "turn");
    if (quest_result && !quest_result->failed.empty())
        game_.log.push_back("Quest abandoned: " + quest_result->failed + ".");
}

bool TurnEngine::StartTurn(Side& side) {
    CityEconomy economy(game_);
    TurnReport& news = game_.TurnNews(SideId(side.index));
    ComeAshore(side);
    FollowCapital(side);
    game::TidyExplored(game_, side.index);
    news.diplo_news = diplomacy::Apply(game_, side);
    for (const auto& msg : news.diplo_news) game_.log.push_back(msg);
    news.hero_offer.reset();
    side.production_due = false;
    if (side.computer) {
        if (game::SideCities(game_, side).empty()) return false;
        auto offer = hero::Offer(game_, side);
        if (offer) hero::Recruit(game_, side, *offer);
        hero::CheckPromotions(game_, side);
        economy.ApplyIncome(side);
        economy.RunProduction(side);
        ResetMovement(side);
        game::TidyTowers(game_);
        Encamp(side);
        if (side.computer_player) side.computer_player->OnTurnStarted(side);
        NotifyTurnQuest(side);
        return true;
    }
    ResetMovement(side);
    game::TidyTowers(game_);
    Encamp(side);
    economy.ApplyIncome(side);
    NotifyTurnQuest(side);
    news.promoted = hero::CheckPromotions(game_, side);
    news.hero_offer = hero::Offer(game_, side);
    side.production_due = true;
    return true;
}

void TurnEngine::HumanProduction(Side& side) {
    if (!side.production_due) return;
    side.production_due = false;
    if (game_.turn == 1) return;
    CityEconomy economy(game_);
    for (const auto& entry : economy.RunProduction(side)) {
        City* city = game_.map->City(entry.city);
        if (city && city->producing != kNone &&
            city->producing < static_cast<int>(city->slots.size())) {
            city->countdown = city->slots[city->producing].time;
        }
    }
}

Side* TurnEngine::AdvanceToNextSide(Side* leaving) {
    for (int step = 0; step < 3 * 8 + 1; step++) {
        if (game_.turn_schedule.AtRoundEnd()) {
            game_.turn++;
            game_.turn_schedule.StartNewRound(game_.map->options.random_turns != 0, game_.rng);
            EndRound(leaving);
            if (game_.ending.over) {
                game_.side = nullptr;
                return nullptr;
            }
            history::Record(game_);
        }
        SideId scheduled_side = game_.turn_schedule.Advance();
        Side* side = game_.map->Side(scheduled_side);
        if (side && side->in_use && side->alive) {
            game_.side = side;
            game_.current = static_cast<int>(
                    std::find(game_.sides.begin(), game_.sides.end(), side) - game_.sides.begin());
            if (StartTurn(*side)) return side;
        }
    }
    game_.side = nullptr;
    return nullptr;
}

Side* TurnEngine::EndTurn() {
    Side* leaving = game_.side;
    if (leaving && leaving->alive && (leaving->computer || !game_.won))
        diplomacy::ScoreUpdate(game_, *leaving);
    game_.ending = Ending{};
    return AdvanceToNextSide(leaving);
}

Side* TurnEngine::Begin() {
    game_.turn_schedule.RewindCursor();
    game_.ending = Ending{};
    return AdvanceToNextSide(nullptr);
}

Side* TurnEngine::NextHuman() const {
    for (int i = std::max(game_.turn_schedule.Cursor(), 0); i <= 7; i++) {
        Side* side = game_.map->Side(game_.turn_schedule.SideAt(i));
        if (side && side->in_use && side->alive && !side->computer) return side;
    }
    for (Side* side : game_.sides) {
        if (side->alive && !side->computer) return side;
    }
    return nullptr;
}

int TurnEngine::SinkStranded(Side& side, int x, int y) {
    auto wet = [&](int tile_x, int tile_y) {
        return scn::TerrainAt(*game_.map, tile_x, tile_y) == move::kWater ||
               scn::IsCrossing(*game_.map, tile_x, tile_y);
    };
    std::map<int, ArmyStack> tiles;
    for (Army* army : game_.armies) {
        if (army->owner != side.index || army->transit || army->x == kNone || army->x < 0 ||
            army->y < 0)
            continue;
        if (x != kNone && (army->x != x || army->y != y)) continue;
        if (wet(army->x, army->y)) tiles[army->y * game_.map->width + army->x].push_back(army);
    }
    ArmyStack dead;
    for (auto& [tile_key, here] : tiles) {
        if (CanFlyOff(game_, here)) continue;
        for (Army* army : here) {
            if (!army->at_sea && army->type != armytype::kHero &&
                !game_.types.ById(army->type)->flies)
                dead.push_back(army);
        }
    }
    game_.armies.RemoveAll(dead);
    return static_cast<int>(dead.size());
}

void TurnEngine::Disband(Side& side, const ArmyStack& armies) {
    for (Army* army : armies) {
        if (army->type == armytype::kHero) {
            hero::DropItems(game_, army, army->x, army->y, side.index);
            if (side.quest && army->id.IsValid() && side.quest->HeroId() == army->id) {
                side.quest.reset();
            }
        }
    }
    game_.armies.RemoveAll(armies);
}

void TurnEngine::Resign(Side& side) {
    CityEconomy economy(game_);
    for (auto& city : game_.map->cities) {
        if (!city.razed && city.owner_index == side.index) economy.MakeRuins(city);
    }
    Disband(side, game::SideArmies(game_, side));
    if (game_.map->options.hidden_map != 0) {
        game_.explored.ResetSide(SideId(side.index), game_.map->width, game_.map->height, true);
    }
}

void TurnEngine::AcceptSurrender(Side& side) {
    for (Side* other : game_.sides) {
        if (other->computer) other->alive = false;
    }
    game_.won = true;
    history::RecordDeed(game_, &side, DeedType::kSideVictorious, side.index, 0, "");
}

bool TurnEngine::SurrenderPending() const {
    return game_.surrender_offered && !game_.surrender_shown && !game_.won;
}

Ending TurnEngine::EndRound(Side* last) {
    auto fallen = EliminateFallen();
    Ending ending = CheckEnd();
    ending.fallen = fallen;
    if (!ending.message.empty()) game_.log.push_back(ending.message);
    game_.ending = ending;
    CityEconomy(game_).NeutralProduction(last ? last : game_.map->Side(game::kSetupPlayer));
    return ending;
}

Ending TurnEngine::CheckEnd() {
    std::vector<Side*> humans, computers;
    bool fallen_human = false;
    for (Side* side : game_.sides) {
        if (side->alive) {
            (side->computer ? computers : humans).push_back(side);
        } else if (!side->computer) {
            side->computer = true;
            fallen_human = true;
        }
    }
    int standing = 0;
    for (const auto& city : game_.map->cities) {
        if (!city.razed) standing++;
    }

    Ending ending;
    if (humans.empty() && computers.empty()) {
        game_.over = true;
        ending.over = true;
        ending.message = "Alas! No more players are left!";
        return ending;
    }
    if (!game_.won && humans.empty() && fallen_human && !game_.no_humans_said) {
        game_.no_humans_said = true;
        ending.no_humans = true;
    }
    if (!game_.won && humans.size() == 1 && computers.empty()) {
        int mine = static_cast<int>(game::SideCities(game_, *humans[0]).size());
        if (mine * 2 > standing) {
            game_.won = true;
            history::RecordDeed(game_, humans[0], DeedType::kSideVictorious, humans[0]->index, 0,
                                "");
            ending.won = true;
            ending.winner = humans[0];
            ending.message = humans[0]->name + " rules the world!";
        }
    }
    if (!game_.won && humans.empty() && computers.size() == 1) {
        Side* winner = computers[0];
        game_.won = true;
        winner->computer = false;
        history::RecordDeed(game_, winner, DeedType::kSideVictorious, winner->index, 0, "");
        ending.triumph = true;
        ending.winner = winner;
        ending.message = winner->name + " has triumphed!";
    }
    if (!game_.won && humans.size() == 1 && !computers.empty() && !game_.surrender_offered) {
        int mine = static_cast<int>(game::SideCities(game_, *humans[0]).size());
        int biggest = 0;
        for (Side* side : computers)
            biggest = std::max(biggest, static_cast<int>(game::SideCities(game_, *side).size()));
        int all = static_cast<int>(game_.map->cities.size());
        if (mine > all / 2 && mine > biggest + all / 8) {
            game_.surrender_offered = true;
            ending.surrender = true;
            ending.message = "Your enemies offer their surrender!";
        }
    }
    return ending;
}

}  // namespace w2
