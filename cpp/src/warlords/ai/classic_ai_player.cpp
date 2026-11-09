/**
 * @file
 * @brief Implementation of `ClassicAiPlayer` turn orchestration and JSON state persistence.
 */
#include "warlords/ai/classic_ai_player.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/base/no_destructor.h"

#include "util/util.hpp"
#include "warlords/ai/attitude.hpp"
#include "warlords/ai/cities.hpp"
#include "warlords/ai/diplomacy.hpp"
#include "warlords/ai/groups.hpp"
#include "warlords/ai/heroes.hpp"
#include "warlords/ai/moves.hpp"
#include "warlords/quest.hpp"

namespace w2 {

namespace {

using Json = nlohmann::ordered_json;

const Json& NullJson() {
    static const absl::NoDestructor<Json> kNull(nullptr);
    return *kNull;
}

const Json& Field(const Json& obj, std::string_view key) {
    if (!obj.is_object()) return NullJson();
    auto found = obj.find(key);
    return found == obj.end() ? NullJson() : *found;
}

bool HasField(const Json& obj, std::string_view key) {
    return obj.is_object() && obj.find(key) != obj.end();
}

int IntOf(const Json& val, int fallback = 0) {
    return val.is_number() ? val.get<int>() : fallback;
}

int IntField(const Json& obj, std::string_view key, int fallback = 0) {
    return IntOf(Field(obj, key), fallback);
}

bool BoolField(const Json& obj, std::string_view key, bool fallback = false) {
    const Json& val = Field(obj, key);
    return val.is_boolean() ? val.get<bool>() : fallback;
}

Json OptInt(int val) {
    return val == kNone ? Json(nullptr) : Json(val);
}

int OptField(const Json& obj, std::string_view key) {
    const Json& val = Field(obj, key);
    return val.is_null() || !val.is_number() ? kNone : val.get<int>();
}

Json IntMap(const std::map<int, int>& map_in) {
    Json obj = Json::object();
    for (const auto& [key, val] : map_in) obj[std::to_string(key)] = val;
    return obj;
}

std::map<int, int> ReadIntMap(const Json& json_obj) {
    std::map<int, int> out;
    if (!json_obj.is_object()) return out;
    for (const auto& [key, val] : json_obj.items()) {
        if (auto parsed_key = ToInt(key)) out[static_cast<int>(*parsed_key)] = IntOf(val);
    }
    return out;
}

template <typename T, size_t N>
Json IntArray(const std::array<T, N>& arr) {
    Json out = Json::array();
    for (T val : arr) out.push_back(static_cast<int>(val));
    return out;
}

template <typename T, size_t N>
void ReadIntArray(const Json& json_arr, std::array<T, N>& arr) {
    if (!json_arr.is_array()) return;
    for (size_t i = 0; i < N && i < json_arr.size(); i++) {
        arr[i] = static_cast<T>(IntOf(json_arr[i]));
    }
}

void TurnSetup(Game& game, Side& side, AIData& data) {
    SideId self_id(side.index);
    game.diplomacy.SetState(self_id, self_id, 0);
    game.diplomacy.SetProposal(self_id, self_id, 0);
    side.ai_solidarity = data.solidarity;
    bool quick = game.map->options.quick_start != 0 && game.map->options.hidden_map != 0;
    for (int i = static_cast<int>(game.map->cities.size()) - 1; i >= 0; i--) {
        City& city = game.map->cities[i];
        if (city.owner_index == side.index) {
            int role = ai::core::Role(data, city);
            if (role == ai::core::kExplorer || role == ai::core::kExploreR2) {
                ai::core::SetRole(data, city, game.turn < 5 ? ai::core::kWeak : ai::core::kStop);
            }
            if (quick) {
                if (game.turn == 1)
                    ai::core::SetRole(data, city, ai::core::kExplorer);
                else if (game.turn < 3)
                    ai::core::SetRole(data, city, ai::core::kExploreR2);
            }
            ai::core::ClearCflag(data, city, ai::core::kCfCleaned);
        }
    }
}

std::shared_ptr<Neighbours> FindExistingNeighbours(const Game& game) {
    if (!game.map) return nullptr;
    for (const auto& side : game.map->sides) {
        if (const ClassicAiPlayer* classic = ClassicAiPlayer::FindOn(side)) {
            if (classic->NeighbourTable()) return classic->NeighbourTable();
        }
    }
    return nullptr;
}

}  // namespace

ClassicAiPlayer::ClassicAiPlayer() : data_(ai::core::NewData()) {}

ClassicAiPlayer::ClassicAiPlayer(AIData initial_data, SharedNeighbours neighbours,
                                 const ArmyRegistry* registry)
        : data_(std::move(initial_data)), neighbours_(std::move(neighbours)), registry_(registry) {}

void ClassicAiPlayer::PlayTurn(Game& game, Side& side, const PauseCallback& pause) {
    registry_ = &game.armies;
    if (!neighbours_) {
        neighbours_ = FindExistingNeighbours(game);
        if (!neighbours_) neighbours_ = ai::core::BuildNeighbours(game);
    }
    auto pause_after = [&](const char* name) {
        if (pause) pause(name);
    };
    bool hidden = game.map->options.hidden_map != 0;

    ResetMovedFlags();
    TurnSetup(game, side, data_);
    ai::diplo::Phase(game, side);
    pause_after("diplomacy");
    ai::heroes::Phase(game, side);
    pause_after("move hero");
    if (hidden) ai::moves::Search(game, side);
    pause_after("move search");
    ai::moves::HeroParties(game, side);
    pause_after("move explore");
    ai::groups::Assault(game, side);
    pause_after("assault");
    ai::moves::MoveAll(game, side);
    pause_after("move #1");
    ai::moves::Rescue(game, side);
    pause_after("rescue");
    ai::cities::Evaluate(game, side, side.index);
    pause_after("evaluate");
    ai::cities::Clean(game, side);
    pause_after("clean city");
    ai::cities::Neutral(game, side);
    pause_after("neutral");
    ai::moves::MoveAll(game, side);
    pause_after("move #2");
    ai::cities::QuickAttack(game, side);
    if (hidden) ai::cities::UpdateHide(game, side);
    ai::groups::AssaultXx(game, side);
    pause_after("assault XX");
    ai::moves::Specials(game, side);
    pause_after("specials");
    ai::cities::Rebuild(game, side);
    pause_after("rebuilding");
    ai::moves::LastRescue(game, side);
    pause_after("last rescue");
    ai::cities::Production(game, side);
    pause_after("production");
    ai::cities::Vectoring(game, side);
    pause_after("vectoring");
}

void ClassicAiPlayer::OnTurnStarted(Side& side) {
    data_.cursor = std::make_pair(side.cap_now_x, side.cap_now_y);
}

AttitudeBreakdown ClassicAiPlayer::EvaluateProposal(const Game& game, SideId ai_side,
                                                    SideId proposer_side) const {
    ai::BattleGrudge grudge{data_.cities_lost, data_.heroes_killed, data_.lost};
    return ai::EvaluateSideProposal(game, ai_side, proposer_side, nullptr, &grudge);
}

City* ClassicAiPlayer::ChooseHeroCity(Game& game, Side& side, City* fallback) {
    City* best = fallback;
    int best_score = 0;
    for (int i = static_cast<int>(game.map->cities.size()) - 1; i >= 0; i--) {
        City& city = game.map->cities[i];
        if (city.owner_index == side.index) {
            int role = ai::core::Role(data_, city);
            int score = 0;
            if (role == ai::core::kRally)
                score = game.rng.Dice(1, 100, 100);
            else if (role == ai::core::kTakingNeutral)
                score = game.rng.Dice(1, 100, 50);
            else if (role == ai::core::kNearNeutral)
                score = game.rng.Dice(1, 100, 0);
            if (best_score < score) {
                best = &city;
                best_score = score;
            }
        }
    }
    return best;
}

bool ClassicAiPlayer::OnQuestCapture(Game& game, Side& side, City& city, const ArmyStack& stack) {
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
    data_.quests_done++;
    data_.quest_city = kNone;
    if (side_quest->Type() == quest::kRaze) {
        ai::core::Sel sel = ai::core::Select(game, stack);
        ai::groups::Raze(game, side, city, true, &sel);
        return true;
    }
    return false;
}

void ClassicAiPlayer::RecordBattle(Game&, SideId, SideId attacker, TilePos, int heroes_lost,
                                   int armies_lost, bool all_lost, bool city_tile) {
    int atk = attacker.Value();
    if (atk < 0 || atk >= 8) return;
    data_.heroes_killed[atk] += heroes_lost;
    data_.armies_killed[atk] += armies_lost;
    data_.battles[atk]++;
    if (all_lost) data_.lost[atk]++;
    if (city_tile) {
        data_.city_battles[atk]++;
        if (all_lost) data_.cities_lost[atk]++;
    }
}

void ClassicAiPlayer::RecordBattleObservation(Game&, SideId, TilePos) {}

const ClassicArmyOrder& ClassicAiPlayer::OrderFor(ArmyId army_id) const {
    static const absl::NoDestructor<ClassicArmyOrder> kDefault;
    if (!army_id.IsValid()) return *kDefault;
    auto found = orders_.find(army_id);
    return found == orders_.end() ? *kDefault : found->second;
}

ClassicArmyOrder& ClassicAiPlayer::MutableOrderFor(ArmyId army_id) {
    static ClassicArmyOrder fallback_scratch;
    if (!army_id.IsValid()) {
        fallback_scratch = ClassicArmyOrder{};
        return fallback_scratch;
    }
    return orders_[army_id];
}

void ClassicAiPlayer::ClearOrder(ArmyId army_id) {
    if (!army_id.IsValid()) return;
    auto found = orders_.find(army_id);
    if (found != orders_.end()) {
        found->second.order = ai::core::kOrderNone;
        found->second.dest = kNone;
    }
}

void ClassicAiPlayer::ResetMovedFlags() {
    for (auto& [army_id, ord] : orders_) {
        (void)army_id;
        ord.moved = false;
    }
}

nlohmann::ordered_json ClassicAiPlayer::SaveState() const {
    Json obj = Json::object();
    obj["kind"] = "classic";
    obj["turns"] = data_.turns;
    obj["own"] = data_.own;
    obj["enemy"] = data_.enemy;
    obj["neutral"] = data_.neutral;
    obj["unseen"] = data_.unseen;
    obj["rebuildLimit"] = data_.rebuild_limit;
    obj["bold"] = data_.bold;
    obj["heroes"] = data_.heroes;
    obj["rebuildType"] = data_.rebuild_type;
    obj["rebuildTypeRich"] = data_.rebuild_type_rich;
    obj["bought"] = data_.bought;
    obj["dieHuman"] = data_.die_human;
    obj["dieLord"] = data_.die_lord;
    obj["dieKnight"] = data_.die_knight;
    obj["dieWarlord"] = data_.die_warlord;
    obj["sims"] = data_.sims;
    obj["searchers"] = data_.searchers;
    obj["explorers"] = data_.explorers;
    obj["raze"] = data_.raze;
    obj["sack"] = data_.sack;
    obj["pillage"] = data_.pillage;
    obj["perCity"] = data_.per_city;
    obj["bonusHuman"] = data_.bonus_human;
    obj["bonusWarlord"] = data_.bonus_warlord;
    obj["bonusLord"] = data_.bonus_lord;
    obj["bonusKnight"] = data_.bonus_knight;
    obj["poor"] = data_.poor;
    obj["solidarity"] = data_.solidarity;
    obj["minStrength"] = data_.min_strength;
    obj["flyCities"] = data_.fly_cities;
    obj["strongCities"] = data_.strong_cities;
    obj["fastCities"] = data_.fast_cities;
    obj["early"] = data_.early;
    obj["humanShare"] = data_.human_share;
    obj["questCity"] = OptInt(data_.quest_city);
    obj["cautious"] = data_.cautious;
    obj["questsDone"] = data_.quests_done;
    obj["itemsPassed"] = data_.items_passed;
    obj["maxGroups"] = data_.max_groups;
    obj["roles"] = IntMap(data_.roles);
    obj["held"] = IntMap(data_.held);
    obj["flags"] = IntMap(data_.flags);
    obj["garrison"] = IntMap(data_.garrison);
    obj["keep"] = IntMap(data_.keep);
    obj["heroesKilled"] = IntArray(data_.heroes_killed);
    obj["armiesKilled"] = IntArray(data_.armies_killed);
    obj["battles"] = IntArray(data_.battles);
    obj["lost"] = IntArray(data_.lost);
    obj["cityBattles"] = IntArray(data_.city_battles);
    obj["citiesLost"] = IntArray(data_.cities_lost);
    if (data_.cursor) {
        obj["cursor"] = Json::array({data_.cursor->first, data_.cursor->second});
    }

    Json groups = Json::array();
    for (int group_idx = 1; group_idx <= ai::core::kMaxGroups; group_idx++) {
        const Group& grp = *data_.groups[group_idx];
        Json grp_json = Json::object();
        grp_json["active"] = grp.active;
        grp_json["target"] = OptInt(grp.target);
        grp_json["rally"] = OptInt(grp.rally);
        grp_json["members"] = IntMap(grp.members);
        grp_json["cities"] = IntMap(grp.cities);
        grp_json["dist"] = IntMap(grp.dist);
        grp_json["bonus"] = IntMap(grp.bonus);
        grp_json["taken"] = IntMap(grp.taken);
        Json staged = Json::object();
        for (const auto& [slot_key, army] : grp.staged) {
            if (army != nullptr && (!registry_ || registry_->Alive(army))) {
                staged[std::to_string(slot_key)] = army->slot;
            }
        }
        grp_json["staged"] = std::move(staged);
        grp_json["size"] = grp.size;
        grp_json["flags"] = grp.flags;
        groups.push_back(std::move(grp_json));
    }
    obj["groups"] = std::move(groups);

    if (registry_ != nullptr && !orders_.empty()) {
        std::map<int, const ClassicArmyOrder*> by_slot;
        for (const auto& [army_id, ord] : orders_) {
            if (ord.IsDefault()) continue;
            if (const Army* army = registry_->FindAlive(army_id)) {
                by_slot[army->slot] = &ord;
            }
        }
        if (!by_slot.empty()) {
            Json orders_json = Json::object();
            for (const auto& [slot_num, ord] : by_slot) {
                Json ord_json = Json::object();
                if (ord->order != 0) ord_json["order"] = ord->order;
                if (ord->dest != kNone) ord_json["dest"] = ord->dest;
                if (ord->group != 0) ord_json["group"] = ord->group;
                if (ord->explore) ord_json["explore"] = true;
                if (ord->neutral) ord_json["neutral"] = true;
                if (ord->party) ord_json["party"] = true;
                if (!ord_json.empty()) orders_json[std::to_string(slot_num)] = std::move(ord_json);
            }
            if (!orders_json.empty()) obj["orders"] = std::move(orders_json);
        }
    }

    if (neighbours_) {
        Json nbrs = Json::object();
        for (size_t i = 0; i < neighbours_->by_city.size(); i++) {
            const auto& entry = neighbours_->by_city[i];
            Json entry_obj = Json::object();
            Json city_arr = Json::array();
            Json dist_arr = Json::array();
            for (int city_idx : entry.cities) city_arr.push_back(city_idx);
            for (int dist : entry.dist) dist_arr.push_back(dist);
            entry_obj["cities"] = std::move(city_arr);
            entry_obj["dist"] = std::move(dist_arr);
            nbrs[std::to_string(i)] = std::move(entry_obj);
        }
        obj["neighbours"] = std::move(nbrs);
    }

    return obj;
}

absl::Status ClassicAiPlayer::LoadState(const nlohmann::ordered_json& obj, const Game& game) {
    registry_ = &game.armies;
    if (!obj.is_object()) return absl::OkStatus();

    absl::flat_hash_map<int, Army*> army_by_slot;
    army_by_slot.reserve(game.armies.size());
    for (Army* army : game.armies) army_by_slot[army->slot] = army;

    auto read_int = [&](std::string_view key, int& target) {
        if (HasField(obj, key)) target = IntField(obj, key, target);
    };
    read_int("turns", data_.turns);
    read_int("own", data_.own);
    read_int("enemy", data_.enemy);
    read_int("neutral", data_.neutral);
    read_int("unseen", data_.unseen);
    read_int("rebuildLimit", data_.rebuild_limit);
    data_.bold = BoolField(obj, "bold");
    read_int("heroes", data_.heroes);
    read_int("rebuildType", data_.rebuild_type);
    read_int("rebuildTypeRich", data_.rebuild_type_rich);
    read_int("bought", data_.bought);
    read_int("dieHuman", data_.die_human);
    read_int("dieLord", data_.die_lord);
    read_int("dieKnight", data_.die_knight);
    read_int("dieWarlord", data_.die_warlord);
    read_int("sims", data_.sims);
    read_int("searchers", data_.searchers);
    read_int("explorers", data_.explorers);
    read_int("raze", data_.raze);
    read_int("sack", data_.sack);
    read_int("pillage", data_.pillage);
    read_int("perCity", data_.per_city);
    read_int("bonusHuman", data_.bonus_human);
    read_int("bonusWarlord", data_.bonus_warlord);
    read_int("bonusLord", data_.bonus_lord);
    read_int("bonusKnight", data_.bonus_knight);
    read_int("poor", data_.poor);
    read_int("solidarity", data_.solidarity);
    read_int("minStrength", data_.min_strength);
    read_int("flyCities", data_.fly_cities);
    read_int("strongCities", data_.strong_cities);
    read_int("fastCities", data_.fast_cities);
    read_int("early", data_.early);
    read_int("humanShare", data_.human_share);
    data_.quest_city = OptField(obj, "questCity");
    read_int("cautious", data_.cautious);
    read_int("questsDone", data_.quests_done);
    read_int("itemsPassed", data_.items_passed);
    read_int("maxGroups", data_.max_groups);
    data_.roles = ReadIntMap(Field(obj, "roles"));
    data_.held = ReadIntMap(Field(obj, "held"));
    data_.flags = ReadIntMap(Field(obj, "flags"));
    data_.garrison = ReadIntMap(Field(obj, "garrison"));
    data_.keep = ReadIntMap(Field(obj, "keep"));
    ReadIntArray(Field(obj, "heroesKilled"), data_.heroes_killed);
    ReadIntArray(Field(obj, "armiesKilled"), data_.armies_killed);
    ReadIntArray(Field(obj, "battles"), data_.battles);
    ReadIntArray(Field(obj, "lost"), data_.lost);
    ReadIntArray(Field(obj, "cityBattles"), data_.city_battles);
    ReadIntArray(Field(obj, "citiesLost"), data_.cities_lost);

    const Json& cursor = Field(obj, "cursor");
    if (cursor.is_array() && cursor.size() >= 2) {
        data_.cursor = std::make_pair(IntOf(cursor[0]), IntOf(cursor[1]));
    } else {
        data_.cursor.reset();
    }

    const Json& groups = Field(obj, "groups");
    for (int group_idx = 1; group_idx <= ai::core::kMaxGroups; group_idx++) {
        auto grp = ai::core::EmptyGroup();
        size_t idx = static_cast<size_t>(group_idx - 1);
        if (groups.is_array() && idx < groups.size() && groups[idx].is_object()) {
            const Json& grp_json = groups[idx];
            grp->active = IntField(grp_json, "active");
            grp->target = OptField(grp_json, "target");
            grp->rally = OptField(grp_json, "rally");
            grp->members = ReadIntMap(Field(grp_json, "members"));
            grp->cities = ReadIntMap(Field(grp_json, "cities"));
            grp->dist = ReadIntMap(Field(grp_json, "dist"));
            grp->bonus = ReadIntMap(Field(grp_json, "bonus"));
            grp->taken = ReadIntMap(Field(grp_json, "taken"));
            const Json& staged = Field(grp_json, "staged");
            if (staged.is_object()) {
                for (const auto& [slot_key, val] : staged.items()) {
                    int saved_slot = IntOf(val, -1);
                    auto group_slot = ToInt(slot_key);
                    auto army_it = army_by_slot.find(saved_slot);
                    if (group_slot && army_it != army_by_slot.end()) {
                        grp->staged[static_cast<int>(*group_slot)] = army_it->second;
                    }
                }
            }
            grp->size = IntField(grp_json, "size");
            grp->flags = IntField(grp_json, "flags");
        }
        data_.groups[group_idx] = grp;
    }

    orders_.clear();
    const Json& orders_json = Field(obj, "orders");
    if (orders_json.is_object()) {
        for (const auto& [slot_str, ord_json] : orders_json.items()) {
            auto slot_num = ToInt(slot_str);
            if (!slot_num || !ord_json.is_object()) continue;
            auto army_it = army_by_slot.find(static_cast<int>(*slot_num));
            if (army_it == army_by_slot.end()) continue;
            ClassicArmyOrder ord;
            ord.order = IntField(ord_json, "order");
            ord.dest = HasField(ord_json, "dest") ? IntField(ord_json, "dest") : kNone;
            ord.group = IntField(ord_json, "group");
            ord.explore = BoolField(ord_json, "explore");
            ord.neutral = BoolField(ord_json, "neutral");
            ord.party = BoolField(ord_json, "party");
            orders_[army_it->second->id] = ord;
        }
    }

    const Json& ai_nbrs = Field(obj, "neighbours");
    if (ai_nbrs.is_object() && game.map) {
        auto nbrs = std::make_shared<Neighbours>();
        nbrs->by_city.resize(game.map->cities.size());
        for (const auto& [key, entry] : ai_nbrs.items()) {
            auto parsed_idx = ToInt(key);
            if (!parsed_idx || *parsed_idx < 0) continue;
            auto city_idx = static_cast<size_t>(*parsed_idx);
            if (city_idx >= nbrs->by_city.size()) continue;
            const Json& entry_cities = Field(entry, "cities");
            if (entry_cities.is_array()) {
                for (const auto& val : entry_cities)
                    nbrs->by_city[city_idx].cities.push_back(IntOf(val));
            }
            const Json& entry_dist = Field(entry, "dist");
            if (entry_dist.is_array()) {
                for (const auto& val : entry_dist)
                    nbrs->by_city[city_idx].dist.push_back(IntOf(val));
            }
        }
        neighbours_ = std::move(nbrs);
    }

    return absl::OkStatus();
}

const ClassicAiPlayer* ClassicAiPlayer::FindOn(const Side& side) {
    const auto* backed = dynamic_cast<const IClassicBackedPlayer*>(side.computer_player.get());
    return backed ? &backed->ClassicImpl() : nullptr;
}

ClassicAiPlayer* ClassicAiPlayer::FindMutableOn(Side& side) {
    auto* backed = dynamic_cast<IClassicBackedPlayer*>(side.computer_player.get());
    return backed ? &backed->ClassicImpl() : nullptr;
}

ClassicAiPlayer& ClassicAiPlayer::EnsureOn(Game& game, Side& side) {
    if (ClassicAiPlayer* existing = FindMutableOn(side)) {
        existing->BindRegistry(&game.armies);
        if (!existing->NeighbourTable()) {
            auto shared = FindExistingNeighbours(game);
            existing->SetNeighbourTable(shared ? shared : ai::core::BuildNeighbours(game));
        }
        return *existing;
    }
    auto shared = FindExistingNeighbours(game);
    if (!shared && game.map) shared = ai::core::BuildNeighbours(game);
    auto created = std::make_shared<ClassicAiPlayer>(ai::core::NewData(), shared, &game.armies);
    ClassicAiPlayer& ref = *created;
    side.computer_player = std::move(created);
    return ref;
}

}  // namespace w2
