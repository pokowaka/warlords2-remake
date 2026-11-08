#include "warlords/save.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>

#include "util/json.hpp"
#include "warlords/ai/belief.hpp"
#include "warlords/ai/core.hpp"
#include "warlords/armytype.hpp"
#include "warlords/game.hpp"
#include "warlords/move.hpp"
#include "warlords/randommap.hpp"
#include "warlords/scn.hpp"

namespace w2::save {

namespace {
/**
 * @brief Encodes raw binary `bytes` as lower-case hexadecimal (used to embed
 * generated random-map files inside a save).
 */
std::string Hex(const std::string& bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char byte : bytes) {
        out += digits[byte >> 4];
        out += digits[byte & 15];
    }
    return out;
}
/** @brief Decodes a hexadecimal string back into raw binary bytes. */
std::string Unhex(const std::string& hex_str) {
    std::string out;
    for (size_t i = 0; i + 1 < hex_str.size(); i += 2)
        out += static_cast<char>(std::stoi(hex_str.substr(i, 2), nullptr, 16));
    return out;
}

/** @brief Serializes an integer-to-integer map as a JSON object keyed by stringified ints. */
Json IntMap(const std::map<int, int>& map_in) {
    Json obj = Json::Object();
    for (const auto& [key, val] : map_in) obj.Set(std::to_string(key), val);
    return obj;
}
/** @brief Deserializes a JSON object with integer string keys into `std::map<int, int>`. */
std::map<int, int> ReadIntMap(const Json& json_obj) {
    std::map<int, int> out;
    for (const auto& [key, val] : json_obj.Entries())
        out[std::stoi(key)] = static_cast<int>(val.Integer());
    return out;
}
/** @brief Serializes a fixed-size array of integral/boolean values into a JSON array. */
template <typename T, size_t N>
Json IntArray(const std::array<T, N>& arr) {
    Json out = Json::Array();
    for (T val : arr) out.Push(static_cast<int>(val));
    return out;
}
/** @brief Reads up to `N` integer elements from `json_arr` into `arr`. */
template <typename T, size_t N>
void ReadIntArray(const Json& json_arr, std::array<T, N>& arr) {
    for (size_t i = 0; i < N && i < json_arr.size(); i++)
        arr[i] = static_cast<T>(json_arr[i].Integer());
}
/** @brief Converts an optional index (`kNone` for absent) to a nullable `Json` value. */
Json OptInt(int val) {
    return val == kNone ? Json() : Json(val);
}
/** @brief Converts a nullable `Json` value back to an integer or `kNone`. */
int ReadOpt(const Json& json_val) {
    return json_val.IsNull() ? kNone : static_cast<int>(json_val.Integer());
}

/**
 * @brief Compresses a side's fog-of-war exploration mask into run-length
 * counts of alternating unseen (starting at 0) and seen tiles.
 */
Json PackMask(const std::vector<uint8_t>& mask) {
    Json runs = Json::Array();
    int cur = 0, run_len = 0;
    for (uint8_t seen_byte : mask) {
        int val = seen_byte ? 1 : 0;
        if (val == cur) {
            run_len++;
        } else {
            runs.Push(run_len);
            cur = val;
            run_len = 1;
        }
    }
    runs.Push(run_len);
    return runs;
}
/** @brief Expands a run-length encoded fog-of-war array back into a tile mask of `size`. */
std::vector<uint8_t> UnpackMask(const Json& runs, size_t size) {
    std::vector<uint8_t> mask(size, 0);
    size_t idx = 0;
    int val = 0;
    for (const auto& run : runs.Items()) {
        for (long step = 0; step < run.Integer() && idx < size; step++)
            mask[idx++] = static_cast<uint8_t>(val);
        val = 1 - val;
    }
    return mask;
}

/** @brief Serializes classic AI per-side state (`AIData`) to a JSON object. */
Json SaveAi(const AIData& data, const std::map<const Army*, int>& ids) {
    Json obj = Json::Object();
    obj.Set("turns", data.turns);
    obj.Set("own", data.own);
    obj.Set("enemy", data.enemy);
    obj.Set("neutral", data.neutral);
    obj.Set("unseen", data.unseen);
    obj.Set("rebuildLimit", data.rebuild_limit);
    obj.Set("bold", data.bold);
    obj.Set("heroes", data.heroes);
    obj.Set("rebuildType", data.rebuild_type);
    obj.Set("rebuildTypeRich", data.rebuild_type_rich);
    obj.Set("bought", data.bought);
    obj.Set("dieHuman", data.die_human);
    obj.Set("dieLord", data.die_lord);
    obj.Set("dieKnight", data.die_knight);
    obj.Set("dieWarlord", data.die_warlord);
    obj.Set("sims", data.sims);
    obj.Set("searchers", data.searchers);
    obj.Set("explorers", data.explorers);
    obj.Set("raze", data.raze);
    obj.Set("sack", data.sack);
    obj.Set("pillage", data.pillage);
    obj.Set("perCity", data.per_city);
    obj.Set("bonusHuman", data.bonus_human);
    obj.Set("bonusWarlord", data.bonus_warlord);
    obj.Set("bonusLord", data.bonus_lord);
    obj.Set("bonusKnight", data.bonus_knight);
    obj.Set("poor", data.poor);
    obj.Set("solidarity", data.solidarity);
    obj.Set("minStrength", data.min_strength);
    obj.Set("flyCities", data.fly_cities);
    obj.Set("strongCities", data.strong_cities);
    obj.Set("fastCities", data.fast_cities);
    obj.Set("early", data.early);
    obj.Set("humanShare", data.human_share);
    obj.Set("questCity", OptInt(data.quest_city));
    obj.Set("cautious", data.cautious);
    obj.Set("questsDone", data.quests_done);
    obj.Set("itemsPassed", data.items_passed);
    obj.Set("maxGroups", data.max_groups);
    obj.Set("roles", IntMap(data.roles));
    obj.Set("held", IntMap(data.held));
    obj.Set("flags", IntMap(data.flags));
    obj.Set("garrison", IntMap(data.garrison));
    obj.Set("keep", IntMap(data.keep));
    obj.Set("heroesKilled", IntArray(data.heroes_killed));
    obj.Set("armiesKilled", IntArray(data.armies_killed));
    obj.Set("battles", IntArray(data.battles));
    obj.Set("lost", IntArray(data.lost));
    obj.Set("cityBattles", IntArray(data.city_battles));
    obj.Set("citiesLost", IntArray(data.cities_lost));
    if (data.cursor) {
        Json cur = Json::Array();
        cur.Push(data.cursor->first);
        cur.Push(data.cursor->second);
        obj.Set("cursor", cur);
    }
    Json groups = Json::Array();
    for (int group_idx = 1; group_idx <= ai::core::kMaxGroups; group_idx++) {
        const Group& grp = *data.groups[group_idx];
        Json grp_json = Json::Object();
        grp_json.Set("active", grp.active);
        grp_json.Set("target", OptInt(grp.target));
        grp_json.Set("rally", OptInt(grp.rally));
        grp_json.Set("members", IntMap(grp.members));
        grp_json.Set("cities", IntMap(grp.cities));
        grp_json.Set("dist", IntMap(grp.dist));
        grp_json.Set("bonus", IntMap(grp.bonus));
        grp_json.Set("taken", IntMap(grp.taken));
        Json staged = Json::Object();
        for (const auto& [slot_key, army] : grp.staged) {
            auto found_it = ids.find(army);
            if (army && found_it != ids.end())
                staged.Set(std::to_string(slot_key), found_it->second);
        }
        grp_json.Set("staged", staged);
        grp_json.Set("size", grp.size);
        grp_json.Set("flags", grp.flags);
        groups.Push(grp_json);
    }
    obj.Set("groups", groups);
    return obj;
}

/** @brief Deserializes classic AI per-side state (`AIData`) from `obj`. */
void LoadAi(AIData& data, const Json& obj, const std::vector<Army*>& armies) {
    auto read_int = [&](const char* key, int& target) {
        if (obj.Has(key)) target = static_cast<int>(obj[key].Integer());
    };
    read_int("turns", data.turns);
    read_int("own", data.own);
    read_int("enemy", data.enemy);
    read_int("neutral", data.neutral);
    read_int("unseen", data.unseen);
    read_int("rebuildLimit", data.rebuild_limit);
    data.bold = obj["bold"].Boolean();
    read_int("heroes", data.heroes);
    read_int("rebuildType", data.rebuild_type);
    read_int("rebuildTypeRich", data.rebuild_type_rich);
    read_int("bought", data.bought);
    read_int("dieHuman", data.die_human);
    read_int("dieLord", data.die_lord);
    read_int("dieKnight", data.die_knight);
    read_int("dieWarlord", data.die_warlord);
    read_int("sims", data.sims);
    read_int("searchers", data.searchers);
    read_int("explorers", data.explorers);
    read_int("raze", data.raze);
    read_int("sack", data.sack);
    read_int("pillage", data.pillage);
    read_int("perCity", data.per_city);
    read_int("bonusHuman", data.bonus_human);
    read_int("bonusWarlord", data.bonus_warlord);
    read_int("bonusLord", data.bonus_lord);
    read_int("bonusKnight", data.bonus_knight);
    read_int("poor", data.poor);
    read_int("solidarity", data.solidarity);
    read_int("minStrength", data.min_strength);
    read_int("flyCities", data.fly_cities);
    read_int("strongCities", data.strong_cities);
    read_int("fastCities", data.fast_cities);
    read_int("early", data.early);
    read_int("humanShare", data.human_share);
    data.quest_city = ReadOpt(obj["questCity"]);
    read_int("cautious", data.cautious);
    read_int("questsDone", data.quests_done);
    read_int("itemsPassed", data.items_passed);
    read_int("maxGroups", data.max_groups);
    data.roles = ReadIntMap(obj["roles"]);
    data.held = ReadIntMap(obj["held"]);
    data.flags = ReadIntMap(obj["flags"]);
    data.garrison = ReadIntMap(obj["garrison"]);
    data.keep = ReadIntMap(obj["keep"]);
    ReadIntArray(obj["heroesKilled"], data.heroes_killed);
    ReadIntArray(obj["armiesKilled"], data.armies_killed);
    ReadIntArray(obj["battles"], data.battles);
    ReadIntArray(obj["lost"], data.lost);
    ReadIntArray(obj["cityBattles"], data.city_battles);
    ReadIntArray(obj["citiesLost"], data.cities_lost);
    if (obj["cursor"].IsArray())
        data.cursor = std::make_pair(static_cast<int>(obj["cursor"][0].Integer()),
                                     static_cast<int>(obj["cursor"][1].Integer()));
    else
        data.cursor.reset();
    const Json& groups = obj["groups"];
    for (int group_idx = 1; group_idx <= ai::core::kMaxGroups; group_idx++) {
        auto grp = ai::core::EmptyGroup();
        const Json& grp_json = groups[group_idx - 1];
        if (grp_json.IsObject()) {
            grp->active = static_cast<int>(grp_json["active"].Integer());
            grp->target = ReadOpt(grp_json["target"]);
            grp->rally = ReadOpt(grp_json["rally"]);
            grp->members = ReadIntMap(grp_json["members"]);
            grp->cities = ReadIntMap(grp_json["cities"]);
            grp->dist = ReadIntMap(grp_json["dist"]);
            grp->bonus = ReadIntMap(grp_json["bonus"]);
            grp->taken = ReadIntMap(grp_json["taken"]);
            for (const auto& [slot_key, val] : grp_json["staged"].Entries()) {
                long army_id = val.Integer();
                if (army_id >= 0 && army_id < static_cast<long>(armies.size()))
                    grp->staged[std::stoi(slot_key)] = armies[army_id];
            }
            grp->size = static_cast<int>(grp_json["size"].Integer());
            grp->flags = static_cast<int>(grp_json["flags"].Integer());
        }
        data.groups[group_idx] = grp;
    }
}

/** @brief Serializes a city production `Slot` to JSON. */
Json SlotJson(const Slot& slot) {
    Json obj = Json::Object();
    obj.Set("type", slot.type);
    obj.Set("name", slot.name);
    obj.Set("strength", slot.strength);
    obj.Set("time", slot.time);
    obj.Set("cost", slot.cost);
    obj.Set("move", slot.move);
    obj.Set("price", slot.price);
    return obj;
}
/** @brief Deserializes a city production `Slot` from JSON. */
Slot ReadSlot(const Json& obj) {
    return Slot{
        static_cast<int>(obj["type"].Integer()),     obj["name"].Str(),
        static_cast<int>(obj["strength"].Integer()), static_cast<int>(obj["time"].Integer()),
        static_cast<int>(obj["cost"].Integer()),     static_cast<int>(obj["move"].Integer()),
        static_cast<int>(obj["price"].Integer())};
}
}  // namespace

std::string Encode(const Game& game) {
    std::map<const Army*, int> ids;
    for (size_t i = 0; i < game.armies.size(); i++) ids[game.armies[i]] = static_cast<int>(i);
    auto id_of = [&](const Army* army) -> Json {
        auto found_it = ids.find(army);
        return found_it == ids.end() ? Json() : Json(found_it->second);
    };

    Json armies = Json::Array();
    for (Army* army : game.armies) {
        Json obj = Json::Object();
        obj.Set("slot", army->slot);
        obj.Set("x", OptInt(army->x));
        obj.Set("y", OptInt(army->y));
        obj.Set("owner", OptInt(army->owner));
        obj.Set("type", army->type);
        obj.Set("name", army->name);
        if (army->female) obj.Set("female", true);
        if (army->medals) obj.Set("medals", army->medals);
        obj.Set("strength", army->strength);
        obj.Set("moves", army->moves);
        obj.Set("maxMoves", army->max_moves);
        obj.Set("upkeep", army->upkeep);
        obj.Set("homeCity", OptInt(army->home_city));
        if (army->at_sea) obj.Set("atSea", true);
        if (army->group) obj.Set("group", army->group);
        if (army->target) {
            Json tgt = Json::Array();
            tgt.Push(army->target->first);
            tgt.Push(army->target->second);
            obj.Set("target", tgt);
        }
        if (army->fortified) obj.Set("fortified", true);
        if (army->level) obj.Set("level", army->level);
        if (army->experience) obj.Set("experience", army->experience);
        if (!army->title.empty()) obj.Set("title", army->title);
        if (!army->items.empty()) {
            Json its = Json::Array();
            for (Item* item : army->items) its.Push(item->index);
            obj.Set("items", its);
        }
        if (!army->blessings.empty()) {
            Json blessed = Json::Array();
            for (int temple_id : army->blessings) blessed.Push(temple_id);
            obj.Set("blessings", blessed);
        }
        if (army->transit) {
            Json trn = Json::Object();
            trn.Set("turns", army->transit->turns);
            trn.Set("dest", army->transit->dest);
            obj.Set("transit", trn);
        }
        if (army->returning) obj.Set("returning", true);
        if (army->ai_order) obj.Set("aiOrder", army->ai_order);
        if (army->ai_dest != kNone) obj.Set("aiDest", army->ai_dest);
        if (army->ai_group) obj.Set("aiGroup", army->ai_group);
        if (army->ai_explore) obj.Set("aiExplore", true);
        if (army->ai_neutral) obj.Set("aiNeutral", true);
        if (army->ai_party) obj.Set("aiParty", true);
        armies.Push(obj);
    }

    Json sides = Json::Array();
    for (Side* side : game.sides) {
        Json obj = Json::Object();
        obj.Set("index", side->index);
        obj.Set("name", side->name);
        obj.Set("gold", side->gold);
        obj.Set("alive", side->alive);
        obj.Set("computer", side->computer);
        obj.Set("level", side->level);
        obj.Set("enhanced", side->enhanced);
        obj.Set("observe", side->observe);
        if (side->cap_now_x != kNone) {
            Json cap_now = Json::Object();
            cap_now.Set("x", side->cap_now_x);
            cap_now.Set("y", side->cap_now_y);
            obj.Set("capNow", cap_now);
        }
        if (side->production_due) obj.Set("productionDue", true);
        obj.Set("diploScore", side->diplo_score);
        obj.Set("income", side->income);
        obj.Set("upkeepTotal", side->upkeep_total);
        obj.Set("aiSolidarity", side->ai_solidarity);
        obj.Set("card", side->card);
        if (!side->classic_ai) obj.Set("classicAi", false);
        Json adv = Json::Object();
        adv.Set("dir", side->advisor.dir);
        adv.Set("mark", side->advisor.mark);
        obj.Set("advisor", adv);
        if (side->ai) obj.Set("ai", SaveAi(*side->ai, ids));
        if (side->quest) {
            const Quest& quest = *side->quest;
            Json quest_json = Json::Object();
            quest_json.Set("type", quest.type);
            quest_json.Set("hero", id_of(quest.hero));
            quest_json.Set("done", quest.done);
            quest_json.Set("required", OptInt(quest.required));
            quest_json.Set("targetKind", quest.target_kind);
            if (quest.target_kind == "army")
                quest_json.Set("target", id_of(quest.army));
            else if (quest.target_kind == "armytype" && quest.army_type)
                quest_json.Set("target", quest.army_type->id);
            else if (quest.target_kind == "city" && quest.city)
                quest_json.Set("target", quest.city->index);
            else if (quest.target_kind == "side" && quest.side)
                quest_json.Set("target", quest.side->index);
            else if (quest.target_kind == "item" && quest.item)
                quest_json.Set("target", quest.item->index);
            obj.Set("quest", quest_json);
        }
        sides.Push(obj);
    }

    Json cities = Json::Array();
    for (const auto& city : game.map->cities) {
        Json obj = Json::Object();
        obj.Set("index", city.index);
        obj.Set("name", city.name);
        obj.Set("ownerIndex", OptInt(city.owner_index));
        obj.Set("producing", OptInt(city.producing));
        obj.Set("countdown", city.countdown);
        if (city.vector_to != kNone) obj.Set("vectorTo", city.vector_to);
        if (city.razed) obj.Set("razed", true);
        obj.Set("defence", city.defence);
        obj.Set("income", city.income);
        obj.Set("claim", OptInt(city.claim));
        obj.Set("razedBy", OptInt(city.razed_by));
        if (city.attacked) obj.Set("attacked", city.attacked);
        Json slots = Json::Array();
        for (const auto& slot : city.slots) slots.Push(SlotJson(slot));
        obj.Set("slots", slots);
        cities.Push(obj);
    }

    Json sites = Json::Array();
    for (const auto& site : game.map->sites) {
        Json obj = Json::Object();
        obj.Set("index", site.index);
        obj.Set("content", site.content);
        obj.Set("item", OptInt(site.item));
        obj.Set("guardian", OptInt(site.guardian));
        obj.Set("allyType", OptInt(site.ally_type));
        if (site.rich) obj.Set("rich", true);
        obj.Set("revealed", site.revealed);
        if (site.searched) obj.Set("searched", true);
        obj.Set("band", site.band);
        obj.Set("templeIndex", OptInt(site.temple_index));
        sites.Push(obj);
    }

    Json items = Json::Array();
    for (const auto& item : game.map->items) {
        Json obj = Json::Object();
        obj.Set("index", item.index);
        obj.Set("name", item.name);
        obj.Set("type", item.type);
        obj.Set("value", item.value);
        obj.Set("status", item.status);
        obj.Set("x", OptInt(item.x));
        obj.Set("y", OptInt(item.y));
        if (item.planted) obj.Set("planted", true);
        obj.Set("standardOf", OptInt(item.standard_of));
        items.Push(obj);
    }

    Json signs = Json::Array();
    for (const auto& sign : game.map->signs) {
        Json lines = Json::Array();
        lines.Push(sign.lines[0]);
        lines.Push(sign.lines[1]);
        signs.Push(lines);
    }
    Json towers = Json::Array();
    for (int tile_key : game.towers) towers.Push(tile_key);
    Json explored = Json::Object();
    for (const auto& [side_idx, mask] : game.explored)
        explored.Set(std::to_string(side_idx), PackMask(mask));

    Json opts = Json::Object();
    for (const auto& opt_name : Options::Names())
        opts.Set(opt_name, *const_cast<Options&>(game.map->options).Field(opt_name));

    Json dip = Json::Object();
    dip.Set("state", IntArray(game.diplomacy.state));
    dip.Set("proposal", IntArray(game.diplomacy.proposal));
    auto has_non_default = [](const auto& arr, int dflt) {
        for (int val : arr)
            if (val != dflt) return true;
        return false;
    };
    if (has_non_default(game.diplomacy.allied, 0))
        dip.Set("allied", IntArray(game.diplomacy.allied));
    if (has_non_default(game.diplomacy.propose_allied, 0))
        dip.Set("proposeAllied", IntArray(game.diplomacy.propose_allied));
    if (has_non_default(game.diplomacy.truce_turns, 0))
        dip.Set("truceTurns", IntArray(game.diplomacy.truce_turns));
    if (has_non_default(game.diplomacy.gold_offer, 0))
        dip.Set("goldOffer", IntArray(game.diplomacy.gold_offer));
    if (has_non_default(game.diplomacy.war_target, kNone))
        dip.Set("warTarget", IntArray(game.diplomacy.war_target));
    if (has_non_default(game.diplomacy.trust, 0)) dip.Set("trust", IntArray(game.diplomacy.trust));
    if (has_non_default(game.diplomacy.allied_target_city, kNone))
        dip.Set("alliedTargetCity", IntArray(game.diplomacy.allied_target_city));

    Json fight = Json::Array();
    for (const auto& row : game.map->fight_order) {
        Json row_json = Json::Array();
        for (int val : row) row_json.Push(val);
        fight.Push(row_json);
    }

    Json history = Json::Array();
    auto deed_json = [](const Deed& deed) {
        Json obj = Json::Object();
        obj.Set("side", deed.side);
        obj.Set("type", static_cast<int>(deed.type));
        obj.Set("v1", deed.v1);
        obj.Set("v2", deed.v2);
        obj.Set("name", deed.name);
        return obj;
    };
    for (const auto& rec : game.history) {
        Json obj = Json::Object();
        obj.Set("gold", IntArray(rec.gold));
        obj.Set("score", IntArray(rec.score));
        obj.Set("cities", IntArray(rec.cities));
        Json owners = Json::Array();
        for (int val : rec.owners) owners.Push(val);
        obj.Set("owners", owners);
        Json events = Json::Array();
        for (const auto& deed : rec.events) events.Push(deed_json(deed));
        obj.Set("events", events);
        history.Push(obj);
    }
    Json deeds = Json::Object();
    for (const auto& [side_idx, list] : game.deeds) {
        Json list_json = Json::Array();
        for (const auto& deed : list) list_json.Push(deed_json(deed));
        deeds.Set(std::to_string(side_idx), list_json);
    }
    Json triumphs = Json::Object();
    for (const auto& [my_side, row] : game.triumphs) {
        Json row_json = Json::Object();
        for (const auto& [opp, counts] : row) row_json.Set(std::to_string(opp), IntArray(counts));
        triumphs.Set(std::to_string(my_side), row_json);
    }
    Json seen = Json::Array();
    for (const auto& moment : game.tutorial_seen) seen.Push(moment);
    Json log = Json::Array();
    for (const auto& line : game.log) log.Push(line);

    Json out = Json::Object();
    out.Set("version", kVersion);
    out.Set("scenario", game.map->name);
    out.Set("turn", game.turn);
    out.Set("current", game.current);
    out.Set("seed", static_cast<double>(game.rng.state));
    {
        Json order = Json::Array();
        for (int val : game.turn_order) order.Push(val);
        out.Set("turnOrder", order);
        out.Set("orderAt", game.order_at);
        if (game.ai_neighbours) {
            Json nbrs = Json::Object();
            for (size_t i = 0; i < game.ai_neighbours->by_city.size(); i++) {
                const auto& entry = game.ai_neighbours->by_city[i];
                Json obj = Json::Object();
                Json city_arr = Json::Array(), dist_arr = Json::Array();
                for (int city_idx : entry.cities) city_arr.Push(city_idx);
                for (int dist : entry.dist) dist_arr.Push(dist);
                obj.Set("cities", city_arr);
                obj.Set("dist", dist_arr);
                nbrs.Set(std::to_string(i), obj);
            }
            out.Set("aiNeighbours", nbrs);
        }
    }
    out.Set("won", game.won);
    out.Set("over", game.over);
    out.Set("noHumansSaid", game.no_humans_said);
    out.Set("greatest", game.greatest);
    out.Set("surrenderOffered", game.surrender_offered);
    out.Set("surrenderShown", game.surrender_shown);
    out.Set("options", opts);
    out.Set("diplomacy", dip);
    out.Set("sides", sides);
    out.Set("cities", cities);
    out.Set("sites", sites);
    out.Set("items", items);
    out.Set("armies", armies);
    out.Set("signs", signs);
    out.Set("fightOrder", fight);
    out.Set("towers", towers);
    out.Set("explored", explored);
    out.Set("history", history);
    out.Set("deeds", deeds);
    out.Set("triumphs", triumphs);
    out.Set("tutorialSeen", seen);
    out.Set("log", log);
    if (game.map->name == randommap::kDir) {
        Json world = Json::Object();
        for (const auto& [ext, bytes] : randommap::Installed(game.data_dir))
            world.Set(ext, Hex(bytes));
        out.Set("randomWorld", world);
    }
    return out.Dump();
}

std::unique_ptr<Game> Decode(const std::string& text, const std::string& data_dir) {
    Json state = Json::Parse(text);
    if (state["version"].Integer() != kVersion)
        throw std::runtime_error("unsupported save version");

    if (state.Has("randomWorld")) {
        randommap::Files files;
        for (const auto& [ext, hex_str] : state["randomWorld"].Entries())
            files[ext] = Unhex(hex_str.Str());
        randommap::Install(data_dir, files);
    }

    game::NewGameOptions opts;
    for (const auto& [key, val] : state["options"].Entries())
        opts.options.emplace_back(key, static_cast<int>(val.Integer()));
    // Reconstruct the setup the game began with: who plays each side, its name,
    // and which sides were turned Off (those omitted from the save's sides list).
    for (int i = 0; i < 8; i++) opts.sides[i].off = true;
    for (const auto& saved : state["sides"].Items()) {
        game::SideSetup& setup = opts.sides[static_cast<int>(saved["index"].Integer())];
        setup.off = false;
        setup.name = saved["name"].Str("");
        setup.computer = saved["computer"].Boolean();
        setup.level = static_cast<int>(saved["level"].Integer());
        setup.card = static_cast<int>(saved["card"].Integer());
    }
    auto game_ptr = game::NewGame(data_dir, state["scenario"].Str(), opts);
    Game& game = *game_ptr;
    game.armies.clear();
    game.turn = static_cast<int>(state["turn"].Integer());
    game.current = static_cast<int>(state["current"].Integer());
    if (state["turnOrder"].IsArray()) {
        for (size_t i = 0; i < 8 && i < state["turnOrder"].size(); i++)
            game.turn_order[i] = static_cast<int>(state["turnOrder"][i].Integer());
    }
    if (state["aiNeighbours"].IsObject()) {
        auto nbrs = std::make_shared<Neighbours>();
        nbrs->by_city.resize(game.map->cities.size());
        for (const auto& [key, entry] : state["aiNeighbours"].Entries()) {
            size_t city_idx = static_cast<size_t>(std::stoi(key));
            if (city_idx >= nbrs->by_city.size()) continue;
            for (const auto& val : entry["cities"].Items())
                nbrs->by_city[city_idx].cities.push_back(static_cast<int>(val.Integer()));
            for (const auto& val : entry["dist"].Items())
                nbrs->by_city[city_idx].dist.push_back(static_cast<int>(val.Integer()));
        }
        game.ai_neighbours = nbrs;
    }
    game.rng.state = static_cast<uint32_t>(state["seed"].Number());
    game.won = state["won"].Boolean();
    game.over = state["over"].Boolean();
    game.surrender_offered = state["surrenderOffered"].Boolean();
    game.surrender_shown = state["surrenderShown"].Boolean();
    game.no_humans_said = state["noHumansSaid"].Boolean();
    game.greatest = state["greatest"].Boolean();
    ReadIntArray(state["diplomacy"]["state"], game.diplomacy.state);
    ReadIntArray(state["diplomacy"]["proposal"], game.diplomacy.proposal);
    if (state["diplomacy"].Has("allied"))
        ReadIntArray(state["diplomacy"]["allied"], game.diplomacy.allied);
    if (state["diplomacy"].Has("proposeAllied"))
        ReadIntArray(state["diplomacy"]["proposeAllied"], game.diplomacy.propose_allied);
    if (state["diplomacy"].Has("truceTurns"))
        ReadIntArray(state["diplomacy"]["truceTurns"], game.diplomacy.truce_turns);
    if (state["diplomacy"].Has("goldOffer"))
        ReadIntArray(state["diplomacy"]["goldOffer"], game.diplomacy.gold_offer);
    if (state["diplomacy"].Has("warTarget"))
        ReadIntArray(state["diplomacy"]["warTarget"], game.diplomacy.war_target);
    if (state["diplomacy"].Has("trust"))
        ReadIntArray(state["diplomacy"]["trust"], game.diplomacy.trust);
    if (state["diplomacy"].Has("alliedTargetCity"))
        ReadIntArray(state["diplomacy"]["alliedTargetCity"], game.diplomacy.allied_target_city);
    game.log.clear();
    for (const auto& line : state["log"].Items()) game.log.push_back(line.Str());

    std::map<int, Item*> item_by_index;
    for (const auto& saved : state["items"].Items()) {
        for (auto& item : game.map->items) {
            if (item.index == saved["index"].Integer()) {
                item.name = saved["name"].Str();
                item.type = static_cast<int>(saved["type"].Integer());
                item.value = static_cast<int>(saved["value"].Integer());
                item.status = static_cast<int>(saved["status"].Integer());
                item.x = ReadOpt(saved["x"]);
                item.y = ReadOpt(saved["y"]);
                item.planted = saved["planted"].Boolean();
                item.standard_of = ReadOpt(saved["standardOf"]);
                item_by_index[item.index] = &item;
            }
        }
    }

    for (const auto& saved : state["cities"].Items()) {
        City* city = game.map->City(static_cast<int>(saved["index"].Integer()));
        if (!city) continue;
        city->owner_index = ReadOpt(saved["ownerIndex"]);
        city->producing = ReadOpt(saved["producing"]);
        city->countdown = static_cast<int>(saved["countdown"].Integer());
        city->vector_to =
                saved.Has("vectorTo") ? static_cast<int>(saved["vectorTo"].Integer()) : kNone;
        city->razed = saved["razed"].Boolean();
        city->defence = static_cast<int>(saved["defence"].Integer());
        city->income = static_cast<int>(saved["income"].Integer());
        city->slots.clear();
        for (const auto& slot : saved["slots"].Items()) city->slots.push_back(ReadSlot(slot));
        if (!saved["claim"].IsNull()) city->claim = static_cast<int>(saved["claim"].Integer());
        city->razed_by = ReadOpt(saved["razedBy"]);
        city->attacked = static_cast<int>(saved["attacked"].Integer());
        if (!saved["name"].Str().empty()) city->name = saved["name"].Str();
        // Razed cities belong to nobody; older saves could leave an owner set.
        if (city->razed) city->owner_index = kNone;
    }
    scn::RefreshCityTiles(*game.map);

    for (const auto& saved : state["sites"].Items()) {
        int idx = static_cast<int>(saved["index"].Integer());
        if (idx < 0 || idx >= static_cast<int>(game.map->sites.size())) continue;
        Site& site = game.map->sites[idx];
        site.content = static_cast<int>(saved["content"].Integer());
        site.item = ReadOpt(saved["item"]);
        site.guardian = ReadOpt(saved["guardian"]);
        site.ally_type = ReadOpt(saved["allyType"]);
        site.rich = saved["rich"].Boolean();
        site.revealed = static_cast<int>(saved["revealed"].Integer());
        site.searched = saved["searched"].Boolean();
        site.band = saved["band"].Str();
        site.temple_index = ReadOpt(saved["templeIndex"]);
    }
    if (state["fightOrder"].IsArray()) {
        game.map->fight_order.clear();
        for (const auto& row : state["fightOrder"].Items()) {
            std::vector<int> row_vals;
            for (const auto& val : row.Items()) row_vals.push_back(static_cast<int>(val.Integer()));
            game.map->fight_order.push_back(row_vals);
        }
    }
    game.history.clear();
    auto read_deed = [](const Json& obj) {
        return Deed{static_cast<int>(obj["side"].Integer()),
                    static_cast<DeedType>(obj["type"].Integer()),
                    static_cast<int>(obj["v1"].Integer()), static_cast<int>(obj["v2"].Integer()),
                    obj["name"].Str()};
    };
    for (const auto& rec_json : state["history"].Items()) {
        HistoryRecord rec;
        ReadIntArray(rec_json["gold"], rec.gold);
        ReadIntArray(rec_json["score"], rec.score);
        ReadIntArray(rec_json["cities"], rec.cities);
        for (const auto& val : rec_json["owners"].Items())
            rec.owners.push_back(static_cast<int>(val.Integer()));
        for (const auto& deed : rec_json["events"].Items()) rec.events.push_back(read_deed(deed));
        game.history.push_back(rec);
    }
    game.deeds.clear();
    for (const auto& [side_key, list] : state["deeds"].Entries())
        for (const auto& deed : list.Items())
            game.deeds[std::stoi(side_key)].push_back(read_deed(deed));
    game.triumphs.clear();
    for (const auto& [my_side, row] : state["triumphs"].Entries())
        for (const auto& [opp, counts] : row.Entries())
            ReadIntArray(counts, game.triumphs[std::stoi(my_side)][std::stoi(opp)]);
    game.tutorial_seen.clear();
    for (const auto& moment : state["tutorialSeen"].Items())
        game.tutorial_seen.insert(moment.Str());
    game.towers.clear();
    for (const auto& tile_key : state["towers"].Items())
        game.towers.insert(static_cast<int>(tile_key.Integer()));
    const Json& signs = state["signs"];
    for (size_t i = 0; i < game.map->signs.size() && i < signs.size(); i++) {
        game.map->signs[i].lines = {signs[i][0].Str(), signs[i][1].Str()};
    }
    game.explored.clear();
    for (const auto& [side_key, runs] : state["explored"].Entries())
        game.explored[std::stoi(side_key)] =
                UnpackMask(runs, static_cast<size_t>(game.map->width) * game.map->height);

    std::vector<Army*> armies;
    for (const auto& saved : state["armies"].Items()) {
        Army army;
        army.x = ReadOpt(saved["x"]);
        army.y = ReadOpt(saved["y"]);
        army.owner = ReadOpt(saved["owner"]);
        army.type = static_cast<int>(saved["type"].Integer());
        army.name = saved["name"].Str();
        army.female = saved["female"].Boolean();
        army.medals = static_cast<int>(saved["medals"].Integer());
        army.strength = static_cast<int>(saved["strength"].Integer());
        army.moves = static_cast<int>(saved["moves"].Integer());
        army.max_moves = static_cast<int>(saved["maxMoves"].Integer());
        army.upkeep = static_cast<int>(saved["upkeep"].Integer());
        army.home_city = ReadOpt(saved["homeCity"]);
        army.at_sea = saved["atSea"].Boolean();
        army.group = static_cast<int>(saved["group"].Integer());
        if (saved["target"].IsArray())
            army.target = std::make_pair(static_cast<int>(saved["target"][0].Integer()),
                                         static_cast<int>(saved["target"][1].Integer()));
        army.fortified = saved["fortified"].Boolean();
        army.level = static_cast<int>(saved["level"].Integer());
        army.experience = static_cast<int>(saved["experience"].Integer());
        army.title = saved["title"].Str("");
        for (const auto& idx : saved["items"].Items()) {
            auto found_it = item_by_index.find(static_cast<int>(idx.Integer()));
            if (found_it != item_by_index.end()) army.items.push_back(found_it->second);
        }
        for (const auto& temple_id : saved["blessings"].Items())
            army.blessings.insert(static_cast<int>(temple_id.Integer()));
        if (saved["transit"].IsObject())
            army.transit = Transit{static_cast<int>(saved["transit"]["turns"].Integer()),
                                   static_cast<int>(saved["transit"]["dest"].Integer())};
        army.returning = saved["returning"].Boolean();
        army.ai_order = static_cast<int>(saved["aiOrder"].Integer());
        army.ai_dest = saved.Has("aiDest") ? static_cast<int>(saved["aiDest"].Integer()) : kNone;
        army.ai_group = static_cast<int>(saved["aiGroup"].Integer());
        army.ai_explore = saved["aiExplore"].Boolean();
        army.ai_neutral = saved["aiNeutral"].Boolean();
        army.ai_party = saved["aiParty"].Boolean();
        // Keep the army table in slot order; older saves without slot numbers
        // preserve their array order.
        army.slot = saved.Has("slot") && !saved["slot"].IsNull()
                            ? static_cast<int>(saved["slot"].Integer())
                            : static_cast<int>(armies.size());
        armies.push_back(game.Make(army));
    }
    game.armies = armies;
    std::stable_sort(game.armies.begin(), game.armies.end(),
                     [](Army* lhs, Army* rhs) { return lhs->slot < rhs->slot; });

    for (const auto& saved : state["sides"].Items()) {
        Side* side = game.map->Side(static_cast<int>(saved["index"].Integer()));
        if (!side) continue;
        side->gold = static_cast<int>(saved["gold"].Integer());
        side->alive = saved["alive"].Boolean();
        side->computer = saved["computer"].Boolean();
        side->level = static_cast<int>(saved["level"].Integer());
        side->enhanced = saved["enhanced"].Boolean();
        side->diplo_score = static_cast<int>(saved["diploScore"].Integer());
        side->observe = saved["observe"].Boolean();
        if (saved["capNow"].IsObject()) {
            side->cap_now_x = static_cast<int>(saved["capNow"]["x"].Integer());
            side->cap_now_y = static_cast<int>(saved["capNow"]["y"].Integer());
        }
        side->production_due = saved["productionDue"].Boolean();
        side->income = static_cast<int>(saved["income"].Integer());
        side->upkeep_total = static_cast<int>(saved["upkeepTotal"].Integer());
        side->ai_solidarity = static_cast<int>(saved["aiSolidarity"].Integer());
        side->card = static_cast<int>(saved["card"].Integer());
        side->classic_ai = saved.Has("classicAi") ? saved["classicAi"].Boolean(true) : true;
        ai::belief::Ensure(*side);
        side->advisor.dir = static_cast<int>(saved["advisor"]["dir"].Integer());
        side->advisor.mark = static_cast<int>(saved["advisor"]["mark"].Integer());
        if (saved["ai"].IsObject()) {
            side->ai = std::make_shared<AIData>(ai::core::NewData());
            LoadAi(*side->ai, saved["ai"], armies);
        }
        side->quest.reset();
        if (saved["quest"].IsObject()) {
            const Json& quest_json = saved["quest"];
            auto quest = std::make_shared<Quest>();
            quest->type = static_cast<int>(quest_json["type"].Integer());
            quest->done = static_cast<int>(quest_json["done"].Integer());
            quest->required = ReadOpt(quest_json["required"]);
            long hero = quest_json["hero"].Integer(-1);
            quest->hero =
                    hero >= 0 && hero < static_cast<long>(armies.size()) ? armies[hero] : nullptr;
            quest->target_kind = quest_json["targetKind"].Str();
            int ref = static_cast<int>(quest_json["target"].Integer(-1));
            if (quest->target_kind == "city")
                quest->city = game.map->City(ref);
            else if (quest->target_kind == "side")
                quest->side = game.map->Side(ref);
            else if (quest->target_kind == "item")
                quest->item = item_by_index.count(ref) ? item_by_index[ref] : nullptr;
            else if (quest->target_kind == "armytype")
                quest->army_type = game.types.ById(ref);
            else if (quest->target_kind == "army")
                quest->army =
                        ref >= 0 && ref < static_cast<int>(armies.size()) ? armies[ref] : nullptr;
            if (quest->hero && quest->HasTarget()) side->quest = quest;
        }
    }

    // A flying unit is never at sea, nor is a hero flying in a stack with one:
    // clear any stale at_sea flag in older saves, matching the Lua reference.
    std::set<int> fliers, walkers;
    for (Army* army : game.armies) {
        if (army->x != kNone) {
            int tile_key = army->y * game.map->width + army->x;
            if (game.types.ById(army->type)->flies)
                fliers.insert(tile_key);
            else if (army->type != armytype::kHero)
                walkers.insert(tile_key);
        }
    }
    for (Army* army : game.armies) {
        if (army->at_sea && army->x != kNone) {
            int tile_key = army->y * game.map->width + army->x;
            if (game.types.ById(army->type)->flies ||
                (army->type == armytype::kHero && fliers.count(tile_key) &&
                 !walkers.count(tile_key))) {
                army->at_sea = false;
            }
        }
    }

    game.side =
            game.current < static_cast<int>(game.sides.size()) ? game.sides[game.current] : nullptr;
    // Restore the current side's position in the turn order, or derive it for
    // older saves that did not record `orderAt`.
    if (state.Has("orderAt") && !state["orderAt"].IsNull()) {
        game.order_at = static_cast<int>(state["orderAt"].Integer());
    } else {
        int cur_side = game.side ? game.side->index : 0;
        game.order_at = -1;
        for (int i = 0; i < 8; i++)
            if (game.turn_order[i] == cur_side) {
                game.order_at = i;
                break;
            }
    }
    move::Invalidate(game);
    return game_ptr;
}

}  // namespace w2::save
