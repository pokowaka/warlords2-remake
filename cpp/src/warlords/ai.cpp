/**
 * @file
 * @brief Computer player setup, default factory registration, and strategy delegation.
 */
#include "warlords/ai.hpp"

#include <algorithm>
#include <memory>
#include <utility>

#include "warlords/ai/classic_ai_player.hpp"
#include "warlords/ai/core.hpp"
#include "warlords/ai/modern_ml_player.hpp"
#include "warlords/aicard.hpp"

namespace w2::ai {

Hooks hooks;

namespace {

bool& DefaultClassicAiFlag() {
    static bool classic = true;
    return classic;
}

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
    game.map->fight_order[side.Index()] = card.fight_order;
}

AIData RollInitialData(Game& game, Side& side) {
    AIData data = core::NewData();
    bool quick = game.map->options.quick_start != 0;
    for (auto& city : game.map->cities) {
        data.roles[city.Index()] = quick ? core::kWeak : 0;
        data.held[city.Index()] = 0;
        data.flags[city.Index()] = game.map->options.hidden_map != 0 ? core::kCfUnseen : 0;
    }
    int level = 2;
    if (side.IsComputerControlled() && (side.Level() == 0 || side.Level() == 1)) {
        level = side.Level();
    }
    LevelDefaults(game, data, level);
    if (side.InUse() && side.IsComputerControlled()) {
        auto card = aicard::Load(game.data_dir, level, side.Card());
        if (card) FromCard(game, side, data, *card);
    }
    return data;
}

AIData MakeDeterministicData(const Game& game, const Side& side) {
    AIData data = core::NewData();
    if (!game.map) return data;
    bool quick = game.map->options.quick_start != 0;
    for (const auto& city : game.map->cities) {
        data.roles[city.Index()] = quick ? core::kWeak : 0;
        data.held[city.Index()] = 0;
        data.flags[city.Index()] = game.map->options.hidden_map != 0 ? core::kCfUnseen : 0;
    }
    int level = 2;
    if (side.IsComputerControlled() && (side.Level() == 0 || side.Level() == 1)) {
        level = side.Level();
    }
    if (level == 0) {
        data.rebuild_limit = 30;
        data.cautious = 1;
        data.max_groups = 1;
        data.rebuild_type = 3;
        data.rebuild_type_rich = 3;
        data.human_share = 80;
    } else if (level == 1) {
        data.rebuild_limit = 20;
        data.bold = true;
        data.max_groups = 2;
        data.rebuild_type = 6;
        data.rebuild_type_rich = 18;
        data.raze = 5;
        data.sack = 10;
        data.pillage = 20;
        data.per_city = 1;
        data.bonus_human = 5;
        data.human_share = 35;
    } else {
        data.rebuild_limit = 10;
        data.max_groups = 4;
        data.solidarity = 1;
        data.bold = true;
        data.rebuild_type = 7;
        data.raze = 5;
        data.sack = 10;
        data.pillage = 20;
        data.per_city = 5;
        data.bonus_human = 50;
        data.poor = 50;
        data.early = 1;
        data.human_share = 35;
    }
    if (side.InUse() && side.IsComputerControlled() && !game.data_dir.empty()) {
        if (auto card = aicard::Load(game.data_dir, level, side.Card())) {
            data.max_groups = std::max(0, std::min(core::kMaxGroups, card->groups));
            data.solidarity = card->solidarity;
            if (card->bold != 0) data.bold = true;
            data.cautious = card->cautious;
            data.rebuild_type = card->rebuild_type;
            data.rebuild_type_rich = card->rebuild_type_rich;
            data.rebuild_limit = card->rebuild_limit;
            data.raze = card->raze;
            data.sack = card->sack;
            data.pillage = card->pillage;
            data.per_city = card->per_city;
            data.bonus_human = card->bonus_human;
            data.bonus_warlord = card->bonus_warlord;
            data.bonus_lord = card->bonus_lord;
            data.bonus_knight = card->bonus_knight;
            data.poor = card->poor;
            data.early = card->early;
            data.human_share = card->human_share * 10 + 5;
        }
    }
    return data;
}

std::shared_ptr<Neighbours> SharedNeighboursFor(Game& game) {
    if (!game.map) return nullptr;
    for (const auto& candidate : game.map->sides) {
        if (const ClassicAiPlayer* classic = ClassicAiPlayer::FindOn(candidate)) {
            if (classic->NeighbourTable()) return classic->NeighbourTable();
        }
    }
    return core::BuildNeighbours(game);
}

void ShareOutCities(Game& game) {
    for (auto& city : game.map->cities) {
        city.SetClaim(city.Owner());
    }
    bool shares[8] = {false};
    int humans = 0;
    int computers = 0;
    std::pair<int, int> from[8];
    for (int i = 0; i < 8; i++) {
        Side& side = game.map->sides[i];
        if (side.InUse()) {
            if (side.IsComputerControlled()) {
                shares[i] = true;
                computers++;
            } else {
                humans++;
            }
            TilePos cap_pos = side.OriginCapitalPos();
            from[i] = {cap_pos.x, cap_pos.y};
            City* capital = core::CityAt(game, cap_pos.x, cap_pos.y);
            if (capital) capital->SetClaim(SideId(i));
        }
    }
    if (computers == 0 && humans != 0) {
        int best = kNone;
        int best_roll = 0;
        for (int i = 7; i >= 0; i--) {
            if (game.map->sides[i].InUse()) {
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
    for (int i = 7; i >= 0; i--) {
        if (shares[i]) {
            turn = i;
            break;
        }
    }
    if (turn == kNone) return;
    for (;;) {
        auto [from_x, from_y] = from[turn];
        City* pick = nullptr;
        int best_distance = kNone;
        for (int i = static_cast<int>(game.map->cities.size()) - 1; i >= 0; i--) {
            City& city = game.map->cities[i];
            if (core::Standing(city) && !city.Claim().IsValid()) {
                int distance = core::Dist(from_x, from_y, city.X(), city.Y());
                if (best_distance == kNone || distance < best_distance) {
                    pick = &city;
                    best_distance = distance;
                }
            }
        }
        if (!pick) break;
        pick->SetClaim(SideId(turn));
        Side& side = game.map->sides[turn];
        if (game.rng.Dice(1, 10, -1) < 5) {
            TilePos cap_pos = side.OriginCapitalPos();
            from[turn] = {cap_pos.x, cap_pos.y};
        } else {
            from[turn] = {pick->X(), pick->Y()};
        }
        do {
            turn = (turn + 1) % 8;
        } while (!shares[turn]);
    }
}

const bool kRegisteredDefaults = []() {
    RegisterDefaultGameComputerSetup(&StartGame);
    RegisterDefaultComputerPlayerFactory(&MakeComputerPlayer);
    return true;
}();

}  // namespace

bool DefaultClassicAi() {
    (void)kRegisteredDefaults;
    return DefaultClassicAiFlag();
}

void SetDefaultClassicAi(bool classic) {
    DefaultClassicAiFlag() = classic;
}

std::shared_ptr<IComputerPlayer> MakeComputerPlayer(bool classic, Game& game, Side& side) {
    if (classic) {
        return std::make_shared<ClassicAiPlayer>(MakeDeterministicData(game, side),
                                                 SharedNeighboursFor(game), &game.armies);
    }
    return std::make_shared<ModernMlPlayer>();
}

void SetSideClassicAi(Game& game, Side& side, bool classic) {
    if (side.ComputerPlayer() && side.ComputerPlayer()->IsClassic() == classic) return;
    side.SetComputerPlayer(MakeComputerPlayer(classic, game, side));
}

AIData& InitSide(Game& game, Side& side) {
    AIData data = RollInitialData(game, side);
    ClassicAiPlayer& classic = ClassicAiPlayer::EnsureOn(game, side);
    classic.MutableData() = std::move(data);
    return classic.MutableData();
}

void StartGame(Game& game) {
    auto neighbours = core::BuildNeighbours(game);
    for (auto& side : game.map->sides) {
        AIData data = RollInitialData(game, side);
        if (DefaultClassicAiFlag()) {
            side.SetComputerPlayer(
                    std::make_shared<ClassicAiPlayer>(std::move(data), neighbours, &game.armies));
        } else {
            side.SetComputerPlayer(std::make_shared<ModernMlPlayer>());
        }
    }
    ShareOutCities(game);
    for (auto& side : game.map->sides) {
        side.SetDiploScore(game.rng.Dice(1, 8, 0));
        if (game.greatest && !side.IsComputerControlled()) side.AddDiploScore(400);
    }
}

void PlayTurn(Game& game, Side& side, const std::function<void(const char*)>& pause) {
    if (!side.ComputerPlayer()) {
        side.SetComputerPlayer(MakeComputerPlayer(DefaultClassicAiFlag(), game, side));
    }
    side.ComputerPlayer()->PlayTurn(game, side, pause);
}

void Walked(Game& game, const std::vector<Army*>& stack, const move::WalkResult& walk) {
    if (hooks.on_walk && walk.steps > 0) hooks.on_walk(game, stack, walk);
}

City* HeroCity(Game& game, Side& side, City* fallback) {
    if (!side.ComputerPlayer()) {
        side.SetComputerPlayer(MakeComputerPlayer(DefaultClassicAiFlag(), game, side));
    }
    return side.ComputerPlayer()->ChooseHeroCity(game, side, fallback);
}

bool QuestCapture(Game& game, Side& side, City& city, const std::vector<Army*>& stack) {
    if (!side.ComputerPlayer()) {
        side.SetComputerPlayer(MakeComputerPlayer(DefaultClassicAiFlag(), game, side));
    }
    return side.ComputerPlayer()->OnQuestCapture(game, side, city, stack);
}

void RecordBattle(Game& game, int defender, int attacker, int x, int y, int heroes_lost,
                  int armies_lost, bool all_lost, bool city_tile) {
    if (defender == kNone || attacker == kNone || defender == core::kNeutral) return;
    if (defender < 0 || defender > 7 || attacker < 0 || attacker > 7) return;
    TilePos pos{x, y};
    Side* def_side = game.map->Side(defender);
    Side* atk_side = game.map->Side(attacker);
    if (def_side && def_side->ComputerPlayer()) {
        def_side->ComputerPlayer()->RecordBattle(game, SideId(defender), SideId(attacker), pos,
                                                 heroes_lost, armies_lost, all_lost, city_tile);
        def_side->ComputerPlayer()->RecordBattleObservation(game, SideId(defender), pos);
    }
    if (atk_side && atk_side->ComputerPlayer()) {
        atk_side->ComputerPlayer()->RecordBattleObservation(game, SideId(attacker), pos);
    }
}

}  // namespace w2::ai
