#include "warlords/save.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/base/no_destructor.h"
#include "absl/status/status.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"

#include "nlohmann/json.hpp"
#include "util/util.hpp"
#include "warlords/armytype.hpp"
#include "warlords/game.hpp"
#include "warlords/move.hpp"
#include "warlords/randommap.hpp"
#include "warlords/scn.hpp"

namespace w2::save {

namespace {

using Json = nlohmann::ordered_json;

const Json& NullJson() {
    static const absl::NoDestructor<Json> kNull(nullptr);
    return *kNull;
}

/** @brief Non-throwing lookup on a const JSON object. */
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

std::string StrOf(const Json& val, std::string_view fallback = "") {
    return val.is_string() ? Utf8ToLatin1(val.get_ref<const std::string&>())
                           : std::string(fallback);
}

std::string StrField(const Json& obj, std::string_view key, std::string_view fallback = "") {
    return StrOf(Field(obj, key), fallback);
}

/** @brief Encodes raw binary `bytes` as lower-case hexadecimal. */
std::string BytesToHex(absl::Span<const uint8_t> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t byte : bytes) {
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0f]);
    }
    return out;
}

int HexNibble(char chr) {
    if (chr >= '0' && chr <= '9') return chr - '0';
    if (chr >= 'a' && chr <= 'f') return chr - 'a' + 10;
    if (chr >= 'A' && chr <= 'F') return chr - 'A' + 10;
    return -1;
}

/** @brief Decodes a hexadecimal string back into raw binary bytes. */
absl::StatusOr<std::vector<uint8_t>> HexToBytes(std::string_view hex_str) {
    if (hex_str.size() % 2 != 0) {
        return absl::InvalidArgumentError(absl::StrFormat(
                "Failed to decode save randomWorld hex payload: odd length %zu. Provide a valid "
                "save file.",
                hex_str.size()));
    }
    std::vector<uint8_t> out;
    out.reserve(hex_str.size() / 2);
    for (size_t i = 0; i < hex_str.size(); i += 2) {
        int high = HexNibble(hex_str[i]);
        int low = HexNibble(hex_str[i + 1]);
        if (high < 0 || low < 0) {
            return absl::InvalidArgumentError(absl::StrFormat(
                    "Failed to decode save randomWorld hex payload: invalid hex digits at offset "
                    "%zu. Provide a valid save file.",
                    i));
        }
        out.push_back(static_cast<uint8_t>((high << 4) | low));
    }
    return out;
}

/** @brief Serializes a fixed-size array of integral/boolean values into a JSON array. */
template <typename T, size_t N>
Json IntArray(const std::array<T, N>& arr) {
    Json out = Json::array();
    for (T val : arr) out.push_back(static_cast<int>(val));
    return out;
}

/** @brief Reads up to `N` integer elements from `json_arr` into `arr`. */
template <typename T, size_t N>
void ReadIntArray(const Json& json_arr, std::array<T, N>& arr) {
    if (!json_arr.is_array()) return;
    for (size_t i = 0; i < N && i < json_arr.size(); i++) {
        arr[i] = static_cast<T>(IntOf(json_arr[i]));
    }
}

/** @brief Converts an optional index (`kNone` for absent) to a nullable `Json` value. */
Json OptInt(int val) {
    return val == kNone ? Json(nullptr) : Json(val);
}

/** @brief Converts a nullable `Json` value back to an integer or `kNone`. */
int ReadOpt(const Json& json_val) {
    return json_val.is_null() || !json_val.is_number() ? kNone : json_val.get<int>();
}

int OptField(const Json& obj, std::string_view key) {
    return ReadOpt(Field(obj, key));
}

/**
 * @brief Compresses a side's fog-of-war exploration mask into run-length
 * counts of alternating unseen (starting at 0) and seen tiles.
 */
Json PackMask(absl::Span<const uint8_t> mask) {
    Json runs = Json::array();
    int cur = 0;
    int run_len = 0;
    for (uint8_t seen_byte : mask) {
        int val = seen_byte ? 1 : 0;
        if (val == cur) {
            run_len++;
        } else {
            runs.push_back(run_len);
            cur = val;
            run_len = 1;
        }
    }
    runs.push_back(run_len);
    return runs;
}

/** @brief Expands a run-length encoded fog-of-war array back into a tile mask of `size`. */
std::vector<uint8_t> UnpackMask(const Json& runs, size_t size) {
    std::vector<uint8_t> mask(size, 0);
    if (!runs.is_array()) return mask;
    size_t idx = 0;
    int val = 0;
    for (const auto& run : runs) {
        int count = IntOf(run);
        for (int step = 0; step < count && idx < size; step++) {
            mask[idx++] = static_cast<uint8_t>(val);
        }
        val = 1 - val;
    }
    return mask;
}

/** @brief Serializes a city production `Slot` to JSON. */
Json SlotJson(const Slot& slot) {
    Json obj = Json::object();
    obj["type"] = slot.type;
    obj["name"] = Latin1ToUtf8(slot.name);
    obj["strength"] = slot.strength;
    obj["time"] = slot.time;
    obj["cost"] = slot.cost;
    obj["move"] = slot.move;
    obj["price"] = slot.price;
    return obj;
}

/** @brief Deserializes a city production `Slot` from JSON. */
Slot ReadSlot(const Json& obj) {
    return Slot{IntField(obj, "type"), StrField(obj, "name"), IntField(obj, "strength"),
                IntField(obj, "time"), IntField(obj, "cost"), IntField(obj, "move"),
                IntField(obj, "price")};
}

}  // namespace

std::string Encode(const Game& game) {
    std::map<const Army*, int> ids;
    for (size_t i = 0; i < game.armies.size(); i++) ids[game.armies[i]] = static_cast<int>(i);
    auto id_of = [&](const Army* army) -> Json {
        auto found_it = ids.find(army);
        return found_it == ids.end() ? Json(nullptr) : Json(found_it->second);
    };

    Json armies = Json::array();
    for (Army* army : game.armies) {
        Json obj = Json::object();
        obj["slot"] = army->slot;
        obj["x"] = OptInt(army->x);
        obj["y"] = OptInt(army->y);
        obj["owner"] = OptInt(army->owner);
        obj["type"] = army->type;
        obj["name"] = Latin1ToUtf8(army->name);
        if (army->female) obj["female"] = true;
        if (army->medals) obj["medals"] = army->medals;
        obj["strength"] = army->strength;
        obj["moves"] = army->moves;
        obj["maxMoves"] = army->max_moves;
        obj["upkeep"] = army->upkeep;
        obj["homeCity"] = OptInt(army->home_city);
        if (army->at_sea) obj["atSea"] = true;
        if (army->group) obj["group"] = army->group;
        if (army->target) {
            obj["target"] = Json::array({army->target->first, army->target->second});
        }
        if (army->fortified) obj["fortified"] = true;
        if (army->level) obj["level"] = army->level;
        if (army->experience) obj["experience"] = army->experience;
        if (!army->title.empty()) obj["title"] = Latin1ToUtf8(army->title);
        if (!army->items.empty()) {
            Json its = Json::array();
            for (Item* item : army->items) its.push_back(item->index);
            obj["items"] = std::move(its);
        }
        if (!army->blessings.empty()) {
            Json blessed = Json::array();
            for (int temple_id : army->blessings) blessed.push_back(temple_id);
            obj["blessings"] = std::move(blessed);
        }
        if (army->transit) {
            Json trn = Json::object();
            trn["turns"] = army->transit->turns;
            trn["dest"] = army->transit->dest;
            obj["transit"] = std::move(trn);
        }
        if (army->returning) obj["returning"] = true;
        armies.push_back(std::move(obj));
    }

    Json sides = Json::array();
    for (Side* side : game.sides) {
        Json obj = Json::object();
        obj["index"] = side->index;
        obj["name"] = Latin1ToUtf8(side->name);
        obj["gold"] = side->gold;
        obj["alive"] = side->alive;
        obj["computer"] = side->computer;
        obj["level"] = side->level;
        obj["enhanced"] = side->enhanced;
        obj["observe"] = side->observe;
        if (side->cap_now_x != kNone) {
            Json cap_now = Json::object();
            cap_now["x"] = side->cap_now_x;
            cap_now["y"] = side->cap_now_y;
            obj["capNow"] = std::move(cap_now);
        }
        if (side->production_due) obj["productionDue"] = true;
        obj["diploScore"] = side->diplo_score;
        obj["income"] = side->income;
        obj["upkeepTotal"] = side->upkeep_total;
        obj["aiSolidarity"] = side->ai_solidarity;
        obj["card"] = side->card;
        if (!side->IsClassicAi()) obj["classicAi"] = false;
        Json adv = Json::object();
        adv["dir"] = side->advisor.dir;
        adv["mark"] = side->advisor.mark;
        obj["advisor"] = std::move(adv);
        if (side->computer_player) {
            obj["computerPlayer"] = side->computer_player->SaveState();
        }
        if (side->quest) {
            const Quest& quest = *side->quest;
            Json quest_json = Json::object();
            quest_json["type"] = quest.Type();
            quest_json["hero"] = id_of(quest.ResolveHero(game));
            quest_json["done"] = quest.Done();
            quest_json["required"] = OptInt(quest.Required());
            std::visit(
                    [&](const auto& target_val) {
                        using TargetType = std::decay_t<decltype(target_val)>;
                        if constexpr (std::is_same_v<TargetType, std::monostate>) {
                            quest_json["targetKind"] = "none";
                        } else if constexpr (std::is_same_v<TargetType, CityId>) {
                            quest_json["targetKind"] = "city";
                            if (target_val.IsValid()) quest_json["target"] = target_val.Value();
                        } else if constexpr (std::is_same_v<TargetType, SideId>) {
                            quest_json["targetKind"] = "side";
                            if (target_val.IsValid()) quest_json["target"] = target_val.Value();
                        } else if constexpr (std::is_same_v<TargetType, ItemId>) {
                            quest_json["targetKind"] = "item";
                            if (target_val.IsValid()) quest_json["target"] = target_val.Value();
                        } else if constexpr (std::is_same_v<TargetType, ArmyTypeId>) {
                            quest_json["targetKind"] = "armytype";
                            if (target_val.IsValid()) quest_json["target"] = target_val.Value();
                        } else if constexpr (std::is_same_v<TargetType, ArmyId>) {
                            quest_json["targetKind"] = "army";
                            quest_json["target"] = id_of(quest.ResolveTargetArmy(game));
                        }
                    },
                    quest.GetTarget());
            obj["quest"] = std::move(quest_json);
        }
        sides.push_back(std::move(obj));
    }

    Json cities = Json::array();
    for (const auto& city : game.map->cities) {
        Json obj = Json::object();
        obj["index"] = city.index;
        obj["name"] = Latin1ToUtf8(city.name);
        obj["ownerIndex"] = OptInt(city.owner_index);
        obj["producing"] = OptInt(city.producing);
        obj["countdown"] = city.countdown;
        if (city.vector_to != kNone) obj["vectorTo"] = city.vector_to;
        if (city.razed) obj["razed"] = true;
        obj["defence"] = city.defence;
        obj["income"] = city.income;
        obj["claim"] = OptInt(city.claim);
        obj["razedBy"] = OptInt(city.razed_by);
        if (city.attacked) obj["attacked"] = city.attacked;
        Json slots = Json::array();
        for (const auto& slot : city.slots) slots.push_back(SlotJson(slot));
        obj["slots"] = std::move(slots);
        cities.push_back(std::move(obj));
    }

    Json sites = Json::array();
    for (const auto& site : game.map->sites) {
        Json obj = Json::object();
        obj["index"] = site.index;
        obj["content"] = site.content;
        obj["item"] = OptInt(site.item);
        obj["guardian"] = OptInt(site.guardian);
        obj["allyType"] = OptInt(site.ally_type);
        if (site.rich) obj["rich"] = true;
        obj["revealed"] = site.revealed;
        if (site.searched) obj["searched"] = true;
        obj["band"] = Latin1ToUtf8(site.band);
        obj["templeIndex"] = OptInt(site.temple_index);
        sites.push_back(std::move(obj));
    }

    Json items = Json::array();
    for (const auto& item : game.map->items) {
        Json obj = Json::object();
        obj["index"] = item.index;
        obj["name"] = Latin1ToUtf8(item.name);
        obj["type"] = item.type;
        obj["value"] = item.value;
        obj["status"] = item.status;
        obj["x"] = OptInt(item.x);
        obj["y"] = OptInt(item.y);
        if (item.planted) obj["planted"] = true;
        obj["standardOf"] = OptInt(item.standard_of);
        items.push_back(std::move(obj));
    }

    Json signs = Json::array();
    for (const auto& sign : game.map->signs) {
        signs.push_back(Json::array({Latin1ToUtf8(sign.lines[0]), Latin1ToUtf8(sign.lines[1])}));
    }
    Json towers = Json::array();
    for (int tile_key : game.towers) towers.push_back(tile_key);
    Json explored = Json::object();
    for (SideId side_id : game.explored.SidesWithMasks()) {
        explored[std::to_string(side_id.Value())] = PackMask(game.explored.MaskFor(side_id));
    }

    Json opts = Json::object();
    for (const auto& opt_name : Options::Names()) {
        opts[opt_name] = *const_cast<Options&>(game.map->options).Field(opt_name);
    }

    Json dip = Json::object();
    dip["state"] = IntArray(game.diplomacy.StateTable());
    dip["proposal"] = IntArray(game.diplomacy.ProposalTable());
    auto has_non_default = [](const auto& arr, int dflt) {
        for (int val : arr) {
            if (val != dflt) return true;
        }
        return false;
    };
    if (has_non_default(game.diplomacy.AlliedTable(), 0))
        dip["allied"] = IntArray(game.diplomacy.AlliedTable());
    if (has_non_default(game.diplomacy.ProposeAlliedTable(), 0))
        dip["proposeAllied"] = IntArray(game.diplomacy.ProposeAlliedTable());
    if (has_non_default(game.diplomacy.TruceTurnsTable(), 0))
        dip["truceTurns"] = IntArray(game.diplomacy.TruceTurnsTable());
    if (has_non_default(game.diplomacy.GoldOfferTable(), 0))
        dip["goldOffer"] = IntArray(game.diplomacy.GoldOfferTable());
    if (has_non_default(game.diplomacy.WarTargetTable(), kNone))
        dip["warTarget"] = IntArray(game.diplomacy.WarTargetTable());
    if (has_non_default(game.diplomacy.TrustTable(), 0))
        dip["trust"] = IntArray(game.diplomacy.TrustTable());
    if (has_non_default(game.diplomacy.AlliedTargetCityTable(), kNone))
        dip["alliedTargetCity"] = IntArray(game.diplomacy.AlliedTargetCityTable());

    Json fight = Json::array();
    for (const auto& row : game.map->fight_order) {
        Json row_json = Json::array();
        for (int val : row) row_json.push_back(val);
        fight.push_back(std::move(row_json));
    }

    Json history = Json::array();
    auto deed_json = [](const Deed& deed) {
        Json obj = Json::object();
        obj["side"] = deed.side;
        obj["type"] = static_cast<int>(deed.type);
        obj["v1"] = deed.v1;
        obj["v2"] = deed.v2;
        obj["name"] = Latin1ToUtf8(deed.name);
        return obj;
    };
    for (const auto& rec : game.history) {
        Json obj = Json::object();
        obj["gold"] = IntArray(rec.gold);
        obj["score"] = IntArray(rec.score);
        obj["cities"] = IntArray(rec.cities);
        Json owners = Json::array();
        for (int val : rec.owners) owners.push_back(val);
        obj["owners"] = std::move(owners);
        Json events = Json::array();
        for (const auto& deed : rec.events) events.push_back(deed_json(deed));
        obj["events"] = std::move(events);
        history.push_back(std::move(obj));
    }
    Json deeds = Json::object();
    for (const auto& [side_idx, list] : game.deeds) {
        Json list_json = Json::array();
        for (const auto& deed : list) list_json.push_back(deed_json(deed));
        deeds[std::to_string(side_idx)] = std::move(list_json);
    }
    Json triumphs = Json::object();
    for (const auto& [my_side, row] : game.triumphs) {
        Json row_json = Json::object();
        for (const auto& [opp, counts] : row) row_json[std::to_string(opp)] = IntArray(counts);
        triumphs[std::to_string(my_side)] = std::move(row_json);
    }
    Json seen = Json::array();
    for (const auto& moment : game.tutorial_seen) seen.push_back(Latin1ToUtf8(moment));
    Json log = Json::array();
    for (const auto& line : game.log) log.push_back(Latin1ToUtf8(line));

    Json out = Json::object();
    out["version"] = kVersion;
    out["scenario"] = Latin1ToUtf8(game.map->name);
    out["turn"] = game.turn;
    out["current"] = game.current;
    out["seed"] = static_cast<double>(game.rng.State());
    {
        Json order = Json::array();
        for (int val : game.turn_schedule.Order()) order.push_back(val);
        out["turnOrder"] = std::move(order);
        out["orderAt"] = game.turn_schedule.Cursor();
    }
    out["won"] = game.won;
    out["over"] = game.over;
    out["noHumansSaid"] = game.no_humans_said;
    out["greatest"] = game.greatest;
    out["surrenderOffered"] = game.surrender_offered;
    out["surrenderShown"] = game.surrender_shown;
    out["options"] = std::move(opts);
    out["diplomacy"] = std::move(dip);
    out["sides"] = std::move(sides);
    out["cities"] = std::move(cities);
    out["sites"] = std::move(sites);
    out["items"] = std::move(items);
    out["armies"] = std::move(armies);
    out["signs"] = std::move(signs);
    out["fightOrder"] = std::move(fight);
    out["towers"] = std::move(towers);
    out["explored"] = std::move(explored);
    out["history"] = std::move(history);
    out["deeds"] = std::move(deeds);
    out["triumphs"] = std::move(triumphs);
    out["tutorialSeen"] = std::move(seen);
    out["log"] = std::move(log);
    if (!game.map->source_files.empty()) {
        std::vector<std::string> sorted_exts;
        sorted_exts.reserve(game.map->source_files.size());
        for (const auto& [ext, bytes] : game.map->source_files) {
            (void)bytes;
            sorted_exts.push_back(ext);
        }
        std::sort(sorted_exts.begin(), sorted_exts.end());
        Json world = Json::object();
        for (const std::string& ext : sorted_exts) {
            world[ext] = BytesToHex(game.map->source_files.at(ext));
        }
        out["randomWorld"] = std::move(world);
    } else if (game.map->name == randommap::kDir) {
        Json world = Json::object();
        for (const char* ext : randommap::kFiles) {
            if (auto bytes = ReadBytes(game.data_dir / randommap::kDir /
                                       (std::string(randommap::kDir) + "." + ext));
                bytes.ok()) {
                world[ext] = BytesToHex(*bytes);
            }
        }
        out["randomWorld"] = std::move(world);
    }
    return out.dump();
}

absl::StatusOr<std::unique_ptr<Game>> Decode(std::string_view text,
                                             const std::filesystem::path& data_dir,
                                             const ComputerPlayerFactory& player_factory) {
    Json state = Json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (state.is_discarded() || !state.is_object()) {
        return absl::InvalidArgumentError(
                "Failed to decode save file: input is not valid JSON. Provide a valid Warlords II "
                "save file.");
    }
    int version = IntField(state, "version", -1);
    if (version != kVersion) {
        return absl::InvalidArgumentError(absl::StrFormat(
                "Failed to decode save file: unsupported schema version %d (expected %d). Start a "
                "new game or use a compatible save.",
                version, kVersion));
    }

    ScenarioFiles random_files;
    game::NewGameOptions opts;
    const Json& random_world = Field(state, "randomWorld");
    if (random_world.is_object()) {
        for (const auto& [ext, hex_val] : random_world.items()) {
            if (!hex_val.is_string()) continue;
            auto decoded_bytes = HexToBytes(hex_val.get_ref<const std::string&>());
            if (!decoded_bytes.ok()) return decoded_bytes.status();
            random_files[ext] = std::move(*decoded_bytes);
        }
        opts.memory_files = &random_files;
    }

    const Json& options_json = Field(state, "options");
    if (options_json.is_object()) {
        for (const auto& [key, val] : options_json.items()) {
            opts.options.emplace_back(key, IntOf(val));
        }
    }
    // Reconstruct the setup the game began with: who plays each side, its name,
    // and which sides were turned Off (those omitted from the save's sides list).
    for (int i = 0; i < 8; i++) opts.sides[i].off = true;
    const Json& saved_sides = Field(state, "sides");
    if (saved_sides.is_array()) {
        for (const auto& saved : saved_sides) {
            game::SideSetup& setup = opts.sides[IntField(saved, "index")];
            setup.off = false;
            setup.name = StrField(saved, "name");
            setup.computer = BoolField(saved, "computer");
            setup.level = IntField(saved, "level");
            setup.card = IntField(saved, "card");
        }
    }
    std::string scenario = StrField(state, "scenario");
    if (scenario.empty()) {
        return absl::InvalidArgumentError(
                "Failed to decode save file: missing \"scenario\" field. Provide a valid Warlords "
                "II save file.");
    }
    auto game_ptr = game::NewGame(data_dir, scenario, opts);
    if (!game_ptr.ok()) return game_ptr.status();
    Game& game = **game_ptr;
    game.armies.ClearActive();
    game.turn = IntField(state, "turn", 1);
    game.current = IntField(state, "current", 0);
    const Json& turn_order = Field(state, "turnOrder");
    if (turn_order.is_array()) {
        std::array<int, 8> loaded_order = game.turn_schedule.Order();
        for (size_t i = 0; i < 8 && i < turn_order.size(); i++) {
            loaded_order[i] = IntOf(turn_order[i]);
        }
        game.turn_schedule.SetOrder(loaded_order);
    }
    const Json& seed_val = Field(state, "seed");
    if (seed_val.is_number()) {
        game.rng.SetState(static_cast<uint32_t>(seed_val.get<double>()));
    }
    game.won = BoolField(state, "won");
    game.over = BoolField(state, "over");
    game.surrender_offered = BoolField(state, "surrenderOffered");
    game.surrender_shown = BoolField(state, "surrenderShown");
    game.no_humans_said = BoolField(state, "noHumansSaid");
    game.greatest = BoolField(state, "greatest");
    const Json& dip = Field(state, "diplomacy");
    {
        auto state_table = game.diplomacy.StateTable();
        ReadIntArray(Field(dip, "state"), state_table);
        game.diplomacy.SetStateTable(state_table);

        auto proposal_table = game.diplomacy.ProposalTable();
        ReadIntArray(Field(dip, "proposal"), proposal_table);
        game.diplomacy.SetProposalTable(proposal_table);

        if (HasField(dip, "allied")) {
            auto allied_table = game.diplomacy.AlliedTable();
            ReadIntArray(Field(dip, "allied"), allied_table);
            game.diplomacy.SetAlliedTable(allied_table);
        }
        if (HasField(dip, "proposeAllied")) {
            auto propose_allied_table = game.diplomacy.ProposeAlliedTable();
            ReadIntArray(Field(dip, "proposeAllied"), propose_allied_table);
            game.diplomacy.SetProposeAlliedTable(propose_allied_table);
        }
        if (HasField(dip, "truceTurns")) {
            auto truce_table = game.diplomacy.TruceTurnsTable();
            ReadIntArray(Field(dip, "truceTurns"), truce_table);
            game.diplomacy.SetTruceTurnsTable(truce_table);
        }
        if (HasField(dip, "goldOffer")) {
            auto gold_table = game.diplomacy.GoldOfferTable();
            ReadIntArray(Field(dip, "goldOffer"), gold_table);
            game.diplomacy.SetGoldOfferTable(gold_table);
        }
        if (HasField(dip, "warTarget")) {
            auto war_table = game.diplomacy.WarTargetTable();
            ReadIntArray(Field(dip, "warTarget"), war_table);
            game.diplomacy.SetWarTargetTable(war_table);
        }
        if (HasField(dip, "trust")) {
            auto trust_table = game.diplomacy.TrustTable();
            ReadIntArray(Field(dip, "trust"), trust_table);
            game.diplomacy.SetTrustTable(trust_table);
        }
        if (HasField(dip, "alliedTargetCity")) {
            auto target_city_table = game.diplomacy.AlliedTargetCityTable();
            ReadIntArray(Field(dip, "alliedTargetCity"), target_city_table);
            game.diplomacy.SetAlliedTargetCityTable(target_city_table);
        }
    }
    game.log.clear();
    const Json& log_json = Field(state, "log");
    if (log_json.is_array()) {
        for (const auto& line : log_json) game.log.push_back(StrOf(line));
    }

    std::map<int, Item*> item_by_index;
    const Json& items_json = Field(state, "items");
    if (items_json.is_array()) {
        for (const auto& saved : items_json) {
            int saved_index = IntField(saved, "index", -1);
            for (auto& item : game.map->items) {
                if (item.index == saved_index) {
                    item.name = StrField(saved, "name");
                    item.type = IntField(saved, "type");
                    item.value = IntField(saved, "value");
                    item.status = IntField(saved, "status");
                    item.x = OptField(saved, "x");
                    item.y = OptField(saved, "y");
                    item.planted = BoolField(saved, "planted");
                    item.standard_of = OptField(saved, "standardOf");
                    item_by_index[item.index] = &item;
                }
            }
        }
    }

    const Json& cities_json = Field(state, "cities");
    if (cities_json.is_array()) {
        for (const auto& saved : cities_json) {
            City* city = game.map->City(IntField(saved, "index", -1));
            if (!city) continue;
            city->owner_index = OptField(saved, "ownerIndex");
            city->producing = OptField(saved, "producing");
            city->countdown = IntField(saved, "countdown");
            city->vector_to = HasField(saved, "vectorTo") ? IntField(saved, "vectorTo") : kNone;
            city->razed = BoolField(saved, "razed");
            city->defence = IntField(saved, "defence");
            city->income = IntField(saved, "income");
            city->slots.clear();
            const Json& slots_json = Field(saved, "slots");
            if (slots_json.is_array()) {
                for (const auto& slot : slots_json) city->slots.push_back(ReadSlot(slot));
            }
            const Json& claim_val = Field(saved, "claim");
            if (!claim_val.is_null() && claim_val.is_number()) {
                city->claim = claim_val.get<int>();
            }
            city->razed_by = OptField(saved, "razedBy");
            city->attacked = IntField(saved, "attacked");
            std::string saved_name = StrField(saved, "name");
            if (!saved_name.empty()) city->name = std::move(saved_name);
            // Razed cities belong to nobody; older saves could leave an owner set.
            if (city->razed) city->owner_index = kNone;
        }
    }
    scn::RefreshCityTiles(*game.map);

    const Json& sites_json = Field(state, "sites");
    if (sites_json.is_array()) {
        for (const auto& saved : sites_json) {
            int idx = IntField(saved, "index", -1);
            if (idx < 0 || idx >= static_cast<int>(game.map->sites.size())) continue;
            Site& site = game.map->sites[static_cast<size_t>(idx)];
            site.content = IntField(saved, "content");
            site.item = OptField(saved, "item");
            site.guardian = OptField(saved, "guardian");
            site.ally_type = OptField(saved, "allyType");
            site.rich = BoolField(saved, "rich");
            site.revealed = IntField(saved, "revealed");
            site.searched = BoolField(saved, "searched");
            site.band = StrField(saved, "band");
            site.temple_index = OptField(saved, "templeIndex");
        }
    }
    const Json& fight_order = Field(state, "fightOrder");
    if (fight_order.is_array()) {
        game.map->fight_order.clear();
        for (const auto& row : fight_order) {
            std::vector<int> row_vals;
            if (row.is_array()) {
                for (const auto& val : row) row_vals.push_back(IntOf(val));
            }
            game.map->fight_order.push_back(std::move(row_vals));
        }
    }
    game.history.clear();
    auto read_deed = [](const Json& obj) {
        return Deed{IntField(obj, "side"), static_cast<DeedType>(IntField(obj, "type")),
                    IntField(obj, "v1"), IntField(obj, "v2"), StrField(obj, "name")};
    };
    const Json& history_json = Field(state, "history");
    if (history_json.is_array()) {
        for (const auto& rec_json : history_json) {
            HistoryRecord rec;
            ReadIntArray(Field(rec_json, "gold"), rec.gold);
            ReadIntArray(Field(rec_json, "score"), rec.score);
            ReadIntArray(Field(rec_json, "cities"), rec.cities);
            const Json& owners_json = Field(rec_json, "owners");
            if (owners_json.is_array()) {
                for (const auto& val : owners_json) rec.owners.push_back(IntOf(val));
            }
            const Json& events_json = Field(rec_json, "events");
            if (events_json.is_array()) {
                for (const auto& deed : events_json) rec.events.push_back(read_deed(deed));
            }
            game.history.push_back(std::move(rec));
        }
    }
    game.deeds.clear();
    const Json& deeds_json = Field(state, "deeds");
    if (deeds_json.is_object()) {
        for (const auto& [side_key, list] : deeds_json.items()) {
            auto side_idx = ToInt(side_key);
            if (!side_idx || !list.is_array()) continue;
            for (const auto& deed : list) {
                game.deeds[static_cast<int>(*side_idx)].push_back(read_deed(deed));
            }
        }
    }
    game.triumphs.clear();
    const Json& triumphs_json = Field(state, "triumphs");
    if (triumphs_json.is_object()) {
        for (const auto& [my_side, row] : triumphs_json.items()) {
            auto my_idx = ToInt(my_side);
            if (!my_idx || !row.is_object()) continue;
            for (const auto& [opp, counts] : row.items()) {
                auto opp_idx = ToInt(opp);
                if (!opp_idx) continue;
                ReadIntArray(counts,
                             game.triumphs[static_cast<int>(*my_idx)][static_cast<int>(*opp_idx)]);
            }
        }
    }
    game.tutorial_seen.clear();
    const Json& seen_json = Field(state, "tutorialSeen");
    if (seen_json.is_array()) {
        for (const auto& moment : seen_json) game.tutorial_seen.insert(StrOf(moment));
    }
    game.towers.clear();
    const Json& towers_json = Field(state, "towers");
    if (towers_json.is_array()) {
        for (const auto& tile_key : towers_json) game.towers.insert(IntOf(tile_key));
    }
    const Json& signs = Field(state, "signs");
    if (signs.is_array()) {
        for (size_t i = 0; i < game.map->signs.size() && i < signs.size(); i++) {
            if (signs[i].is_array() && signs[i].size() >= 2) {
                game.map->signs[i].lines = {StrOf(signs[i][0]), StrOf(signs[i][1])};
            }
        }
    }
    game.explored.Clear();
    const Json& explored_json = Field(state, "explored");
    if (explored_json.is_object()) {
        for (const auto& [side_key, runs] : explored_json.items()) {
            if (auto side_idx = ToInt(side_key)) {
                game.explored.SetMask(
                        SideId(static_cast<int>(*side_idx)),
                        UnpackMask(runs, static_cast<size_t>(game.map->width) * game.map->height));
            }
        }
    }

    std::vector<Army*> armies;
    const Json& armies_json = Field(state, "armies");
    if (armies_json.is_array()) {
        for (const auto& saved : armies_json) {
            Army army;
            army.x = OptField(saved, "x");
            army.y = OptField(saved, "y");
            army.owner = OptField(saved, "owner");
            army.type = IntField(saved, "type");
            army.name = StrField(saved, "name");
            army.female = BoolField(saved, "female");
            army.medals = IntField(saved, "medals");
            army.strength = IntField(saved, "strength");
            army.moves = IntField(saved, "moves");
            army.max_moves = IntField(saved, "maxMoves");
            army.upkeep = IntField(saved, "upkeep");
            army.home_city = OptField(saved, "homeCity");
            army.at_sea = BoolField(saved, "atSea");
            army.group = IntField(saved, "group");
            const Json& target_json = Field(saved, "target");
            if (target_json.is_array() && target_json.size() >= 2) {
                army.target = std::make_pair(IntOf(target_json[0]), IntOf(target_json[1]));
            }
            army.fortified = BoolField(saved, "fortified");
            army.level = IntField(saved, "level");
            army.experience = IntField(saved, "experience");
            army.title = StrField(saved, "title");
            const Json& carried = Field(saved, "items");
            if (carried.is_array()) {
                for (const auto& idx : carried) {
                    auto found_it = item_by_index.find(IntOf(idx, -1));
                    if (found_it != item_by_index.end()) army.items.push_back(found_it->second);
                }
            }
            const Json& blessings = Field(saved, "blessings");
            if (blessings.is_array()) {
                for (const auto& temple_id : blessings) army.blessings.insert(IntOf(temple_id));
            }
            const Json& transit_json = Field(saved, "transit");
            if (transit_json.is_object()) {
                army.transit =
                        Transit{IntField(transit_json, "turns"), IntField(transit_json, "dest")};
            }
            army.returning = BoolField(saved, "returning");
            // Keep the army table in slot order; older saves without slot numbers
            // preserve their array order.
            const Json& slot_val = Field(saved, "slot");
            army.slot =
                    slot_val.is_number() ? slot_val.get<int>() : static_cast<int>(armies.size());
            armies.push_back(game.Make(army));
        }
    }
    game.armies.RestoreActive(armies);

    if (saved_sides.is_array()) {
        for (const auto& saved : saved_sides) {
            Side* side = game.map->Side(IntField(saved, "index", -1));
            if (!side) continue;
            side->gold = IntField(saved, "gold");
            side->alive = BoolField(saved, "alive");
            side->computer = BoolField(saved, "computer");
            side->level = IntField(saved, "level");
            side->enhanced = BoolField(saved, "enhanced");
            side->diplo_score = IntField(saved, "diploScore");
            side->observe = BoolField(saved, "observe");
            const Json& cap_now = Field(saved, "capNow");
            if (cap_now.is_object()) {
                side->cap_now_x = IntField(cap_now, "x");
                side->cap_now_y = IntField(cap_now, "y");
            }
            side->production_due = BoolField(saved, "productionDue");
            side->income = IntField(saved, "income");
            side->upkeep_total = IntField(saved, "upkeepTotal");
            side->ai_solidarity = IntField(saved, "aiSolidarity");
            side->card = IntField(saved, "card");
            bool is_classic =
                    HasField(saved, "classicAi") ? BoolField(saved, "classicAi", true) : true;
            side->computer_player = player_factory
                                            ? player_factory(is_classic, game, *side)
                                            : CreateDefaultComputerPlayer(is_classic, game, *side);
            const Json& advisor_json = Field(saved, "advisor");
            side->advisor.dir = IntField(advisor_json, "dir");
            side->advisor.mark = IntField(advisor_json, "mark");
            const Json& cp_json = HasField(saved, "computerPlayer") ? Field(saved, "computerPlayer")
                                                                    : Field(saved, "ai");
            if (side->computer_player && cp_json.is_object()) {
                absl::Status load_status = side->computer_player->LoadState(cp_json, game);
                if (!load_status.ok()) return load_status;
            }
            side->quest.reset();
            const Json& quest_json = Field(saved, "quest");
            if (quest_json.is_object()) {
                int quest_type = IntField(quest_json, "type");
                int quest_done = IntField(quest_json, "done");
                int quest_req = OptField(quest_json, "required");
                int hero_idx = IntField(quest_json, "hero", -1);
                Army* hero_ptr = hero_idx >= 0 && hero_idx < static_cast<int>(armies.size())
                                         ? armies[static_cast<size_t>(hero_idx)]
                                         : nullptr;
                ArmyId hero_id = hero_ptr ? hero_ptr->id : ArmyId::None();
                std::string target_kind = StrField(quest_json, "targetKind");
                int ref = IntField(quest_json, "target", -1);
                Quest::Target target = std::monostate{};
                if (target_kind == "city" && game.map->City(ref)) {
                    target = CityId(ref);
                } else if (target_kind == "side" && game.map->Side(ref)) {
                    target = SideId(ref);
                } else if (target_kind == "item" && item_by_index.count(ref)) {
                    target = ItemId(ref);
                } else if (target_kind == "armytype" && game.types.ById(ref)) {
                    target = ArmyTypeId(ref);
                } else if (target_kind == "army" && ref >= 0 &&
                           ref < static_cast<int>(armies.size())) {
                    target = armies[static_cast<size_t>(ref)]->id;
                }
                auto quest =
                        std::make_shared<Quest>(quest_type, hero_id, target, quest_req, quest_done);
                if (hero_id.IsValid() && quest->HasTarget()) side->quest = quest;
            }
        }
    }

    // A flying unit is never at sea, nor is a hero flying in a stack with one:
    // clear any stale at_sea flag in older saves, matching the Lua reference.
    std::set<int> fliers;
    std::set<int> walkers;
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
    const Json& order_at = Field(state, "orderAt");
    if (order_at.is_number()) {
        game.turn_schedule.SetCursor(order_at.get<int>());
    } else {
        int cur_side = game.side ? game.side->index : 0;
        game.turn_schedule.SyncCursorToSide(SideId(cur_side));
    }
    move::Invalidate(game);
    return game_ptr;
}

}  // namespace w2::save
