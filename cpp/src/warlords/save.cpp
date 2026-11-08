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
// A random world has no files to reload, so its save carries them, as the
// original's saves carry the map: in hex, as the Lua's does.
std::string Hex(const std::string& b) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(b.size() * 2);
    for (unsigned char c : b) {
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}
std::string Unhex(const std::string& h) {
    std::string out;
    for (size_t i = 0; i + 1 < h.size(); i += 2)
        out += static_cast<char>(std::stoi(h.substr(i, 2), nullptr, 16));
    return out;
}

Json IntMap(const std::map<int, int>& m) {
    Json o = Json::Object();
    for (auto& [k, v] : m) o.Set(std::to_string(k), v);
    return o;
}
std::map<int, int> ReadIntMap(const Json& j) {
    std::map<int, int> m;
    for (auto& [k, v] : j.Entries()) m[std::stoi(k)] = static_cast<int>(v.Integer());
    return m;
}
template <typename T, size_t N>
Json IntArray(const std::array<T, N>& a) {
    Json o = Json::Array();
    for (T v : a) o.Push(static_cast<int>(v));
    return o;
}
template <typename T, size_t N>
void ReadIntArray(const Json& j, std::array<T, N>& a) {
    for (size_t i = 0; i < N && i < j.size(); i++) a[i] = static_cast<T>(j[i].Integer());
}
Json OptInt(int v) {
    return v == kNone ? Json() : Json(v);
}
int ReadOpt(const Json& j) {
    return j.IsNull() ? kNone : static_cast<int>(j.Integer());
}

// the hidden map, a side's mask as runs of seen and unseen tiles
Json PackMask(const std::vector<uint8_t>& mask) {
    Json runs = Json::Array();
    int cur = 0, n = 0;
    for (uint8_t m : mask) {
        int v = m ? 1 : 0;
        if (v == cur)
            n++;
        else {
            runs.Push(n);
            cur = v;
            n = 1;
        }
    }
    runs.Push(n);
    return runs;
}
std::vector<uint8_t> UnpackMask(const Json& runs, size_t size) {
    std::vector<uint8_t> mask(size, 0);
    size_t i = 0;
    int v = 0;
    for (auto& n : runs.Items()) {
        for (long k = 0; k < n.Integer() && i < size; k++) mask[i++] = static_cast<uint8_t>(v);
        v = 1 - v;
    }
    return mask;
}

Json SaveAi(const AIData& d, const std::map<const Army*, int>& ids) {
    Json o = Json::Object();
    o.Set("turns", d.turns);
    o.Set("own", d.own);
    o.Set("enemy", d.enemy);
    o.Set("neutral", d.neutral);
    o.Set("unseen", d.unseen);
    o.Set("rebuildLimit", d.rebuild_limit);
    o.Set("bold", d.bold);
    o.Set("heroes", d.heroes);
    o.Set("rebuildType", d.rebuild_type);
    o.Set("rebuildTypeRich", d.rebuild_type_rich);
    o.Set("bought", d.bought);
    o.Set("dieHuman", d.die_human);
    o.Set("dieLord", d.die_lord);
    o.Set("dieKnight", d.die_knight);
    o.Set("dieWarlord", d.die_warlord);
    o.Set("sims", d.sims);
    o.Set("searchers", d.searchers);
    o.Set("explorers", d.explorers);
    o.Set("raze", d.raze);
    o.Set("sack", d.sack);
    o.Set("pillage", d.pillage);
    o.Set("perCity", d.per_city);
    o.Set("bonusHuman", d.bonus_human);
    o.Set("bonusWarlord", d.bonus_warlord);
    o.Set("bonusLord", d.bonus_lord);
    o.Set("bonusKnight", d.bonus_knight);
    o.Set("poor", d.poor);
    o.Set("solidarity", d.solidarity);
    o.Set("minStrength", d.min_strength);
    o.Set("flyCities", d.fly_cities);
    o.Set("strongCities", d.strong_cities);
    o.Set("fastCities", d.fast_cities);
    o.Set("early", d.early);
    o.Set("humanShare", d.human_share);
    o.Set("questCity", OptInt(d.quest_city));
    o.Set("cautious", d.cautious);
    o.Set("questsDone", d.quests_done);
    o.Set("itemsPassed", d.items_passed);
    o.Set("maxGroups", d.max_groups);
    o.Set("roles", IntMap(d.roles));
    o.Set("held", IntMap(d.held));
    o.Set("flags", IntMap(d.flags));
    o.Set("garrison", IntMap(d.garrison));
    o.Set("keep", IntMap(d.keep));
    o.Set("heroesKilled", IntArray(d.heroes_killed));
    o.Set("armiesKilled", IntArray(d.armies_killed));
    o.Set("battles", IntArray(d.battles));
    o.Set("lost", IntArray(d.lost));
    o.Set("cityBattles", IntArray(d.city_battles));
    o.Set("citiesLost", IntArray(d.cities_lost));
    if (d.cursor) {
        Json c = Json::Array();
        c.Push(d.cursor->first);
        c.Push(d.cursor->second);
        o.Set("cursor", c);
    }
    Json groups = Json::Array();
    for (int gi = 1; gi <= ai::core::kMaxGroups; gi++) {
        const Group& grp = *d.groups[gi];
        Json gr = Json::Object();
        gr.Set("active", grp.active);
        gr.Set("target", OptInt(grp.target));
        gr.Set("rally", OptInt(grp.rally));
        gr.Set("members", IntMap(grp.members));
        gr.Set("cities", IntMap(grp.cities));
        gr.Set("dist", IntMap(grp.dist));
        gr.Set("bonus", IntMap(grp.bonus));
        gr.Set("taken", IntMap(grp.taken));
        Json staged = Json::Object();
        for (auto& [k, a] : grp.staged) {
            auto it = ids.find(a);
            if (a && it != ids.end()) staged.Set(std::to_string(k), it->second);
        }
        gr.Set("staged", staged);
        gr.Set("size", grp.size);
        gr.Set("flags", grp.flags);
        groups.Push(gr);
    }
    o.Set("groups", groups);
    return o;
}

void LoadAi(AIData& d, const Json& o, const std::vector<Army*>& armies) {
    auto read_int = [&](const char* k, int& v) {
        if (o.Has(k)) v = static_cast<int>(o[k].Integer());
    };
    read_int("turns", d.turns);
    read_int("own", d.own);
    read_int("enemy", d.enemy);
    read_int("neutral", d.neutral);
    read_int("unseen", d.unseen);
    read_int("rebuildLimit", d.rebuild_limit);
    d.bold = o["bold"].Boolean();
    read_int("heroes", d.heroes);
    read_int("rebuildType", d.rebuild_type);
    read_int("rebuildTypeRich", d.rebuild_type_rich);
    read_int("bought", d.bought);
    read_int("dieHuman", d.die_human);
    read_int("dieLord", d.die_lord);
    read_int("dieKnight", d.die_knight);
    read_int("dieWarlord", d.die_warlord);
    read_int("sims", d.sims);
    read_int("searchers", d.searchers);
    read_int("explorers", d.explorers);
    read_int("raze", d.raze);
    read_int("sack", d.sack);
    read_int("pillage", d.pillage);
    read_int("perCity", d.per_city);
    read_int("bonusHuman", d.bonus_human);
    read_int("bonusWarlord", d.bonus_warlord);
    read_int("bonusLord", d.bonus_lord);
    read_int("bonusKnight", d.bonus_knight);
    read_int("poor", d.poor);
    read_int("solidarity", d.solidarity);
    read_int("minStrength", d.min_strength);
    read_int("flyCities", d.fly_cities);
    read_int("strongCities", d.strong_cities);
    read_int("fastCities", d.fast_cities);
    read_int("early", d.early);
    read_int("humanShare", d.human_share);
    d.quest_city = ReadOpt(o["questCity"]);
    read_int("cautious", d.cautious);
    read_int("questsDone", d.quests_done);
    read_int("itemsPassed", d.items_passed);
    read_int("maxGroups", d.max_groups);
    d.roles = ReadIntMap(o["roles"]);
    d.held = ReadIntMap(o["held"]);
    d.flags = ReadIntMap(o["flags"]);
    d.garrison = ReadIntMap(o["garrison"]);
    d.keep = ReadIntMap(o["keep"]);
    ReadIntArray(o["heroesKilled"], d.heroes_killed);
    ReadIntArray(o["armiesKilled"], d.armies_killed);
    ReadIntArray(o["battles"], d.battles);
    ReadIntArray(o["lost"], d.lost);
    ReadIntArray(o["cityBattles"], d.city_battles);
    ReadIntArray(o["citiesLost"], d.cities_lost);
    if (o["cursor"].IsArray())
        d.cursor = std::make_pair(static_cast<int>(o["cursor"][0].Integer()),
                                  static_cast<int>(o["cursor"][1].Integer()));
    else
        d.cursor.reset();
    const Json& groups = o["groups"];
    for (int gi = 1; gi <= ai::core::kMaxGroups; gi++) {
        auto grp = ai::core::EmptyGroup();
        const Json& gr = groups[gi - 1];
        if (gr.IsObject()) {
            grp->active = static_cast<int>(gr["active"].Integer());
            grp->target = ReadOpt(gr["target"]);
            grp->rally = ReadOpt(gr["rally"]);
            grp->members = ReadIntMap(gr["members"]);
            grp->cities = ReadIntMap(gr["cities"]);
            grp->dist = ReadIntMap(gr["dist"]);
            grp->bonus = ReadIntMap(gr["bonus"]);
            grp->taken = ReadIntMap(gr["taken"]);
            for (auto& [k, v] : gr["staged"].Entries()) {
                long id = v.Integer();
                if (id >= 0 && id < static_cast<long>(armies.size()))
                    grp->staged[std::stoi(k)] = armies[id];
            }
            grp->size = static_cast<int>(gr["size"].Integer());
            grp->flags = static_cast<int>(gr["flags"].Integer());
        }
        d.groups[gi] = grp;
    }
}

Json SlotJson(const Slot& s) {
    Json o = Json::Object();
    o.Set("type", s.type);
    o.Set("name", s.name);
    o.Set("strength", s.strength);
    o.Set("time", s.time);
    o.Set("cost", s.cost);
    o.Set("move", s.move);
    o.Set("price", s.price);
    return o;
}
Slot ReadSlot(const Json& o) {
    return Slot{static_cast<int>(o["type"].Integer()),     o["name"].Str(),
                static_cast<int>(o["strength"].Integer()), static_cast<int>(o["time"].Integer()),
                static_cast<int>(o["cost"].Integer()),     static_cast<int>(o["move"].Integer()),
                static_cast<int>(o["price"].Integer())};
}
}  // namespace

std::string Encode(const Game& g) {
    std::map<const Army*, int> ids;
    for (size_t i = 0; i < g.armies.size(); i++) ids[g.armies[i]] = static_cast<int>(i);
    auto id_of = [&](const Army* a) -> Json {
        auto it = ids.find(a);
        return it == ids.end() ? Json() : Json(it->second);
    };

    Json armies = Json::Array();
    for (Army* a : g.armies) {
        Json o = Json::Object();
        o.Set("slot", a->slot);
        o.Set("x", OptInt(a->x));
        o.Set("y", OptInt(a->y));
        o.Set("owner", OptInt(a->owner));
        o.Set("type", a->type);
        o.Set("name", a->name);
        if (a->female) o.Set("female", true);
        if (a->medals) o.Set("medals", a->medals);
        o.Set("strength", a->strength);
        o.Set("moves", a->moves);
        o.Set("maxMoves", a->max_moves);
        o.Set("upkeep", a->upkeep);
        o.Set("homeCity", OptInt(a->home_city));
        if (a->at_sea) o.Set("atSea", true);
        if (a->group) o.Set("group", a->group);
        if (a->target) {
            Json t = Json::Array();
            t.Push(a->target->first);
            t.Push(a->target->second);
            o.Set("target", t);
        }
        if (a->fortified) o.Set("fortified", true);
        if (a->level) o.Set("level", a->level);
        if (a->experience) o.Set("experience", a->experience);
        if (!a->title.empty()) o.Set("title", a->title);
        if (!a->items.empty()) {
            Json its = Json::Array();
            for (Item* it : a->items) its.Push(it->index);
            o.Set("items", its);
        }
        if (!a->blessings.empty()) {
            Json b = Json::Array();
            for (int t : a->blessings) b.Push(t);
            o.Set("blessings", b);
        }
        if (a->transit) {
            Json t = Json::Object();
            t.Set("turns", a->transit->turns);
            t.Set("dest", a->transit->dest);
            o.Set("transit", t);
        }
        if (a->returning) o.Set("returning", true);
        if (a->ai_order) o.Set("aiOrder", a->ai_order);
        if (a->ai_dest != kNone) o.Set("aiDest", a->ai_dest);
        if (a->ai_group) o.Set("aiGroup", a->ai_group);
        if (a->ai_explore) o.Set("aiExplore", true);
        if (a->ai_neutral) o.Set("aiNeutral", true);
        if (a->ai_party) o.Set("aiParty", true);
        armies.Push(o);
    }

    Json sides = Json::Array();
    for (Side* s : g.sides) {
        Json o = Json::Object();
        o.Set("index", s->index);
        o.Set("name", s->name);
        o.Set("gold", s->gold);
        o.Set("alive", s->alive);
        o.Set("computer", s->computer);
        o.Set("level", s->level);
        o.Set("enhanced", s->enhanced);
        o.Set("observe", s->observe);
        if (s->cap_now_x != kNone) {
            Json cn = Json::Object();
            cn.Set("x", s->cap_now_x);
            cn.Set("y", s->cap_now_y);
            o.Set("capNow", cn);
        }
        if (s->production_due) o.Set("productionDue", true);
        o.Set("diploScore", s->diplo_score);
        o.Set("income", s->income);
        o.Set("upkeepTotal", s->upkeep_total);
        o.Set("aiSolidarity", s->ai_solidarity);
        o.Set("card", s->card);
        if (!s->classic_ai) o.Set("classicAi", false);
        Json adv = Json::Object();
        adv.Set("dir", s->advisor.dir);
        adv.Set("mark", s->advisor.mark);
        o.Set("advisor", adv);
        if (s->ai) o.Set("ai", SaveAi(*s->ai, ids));
        if (s->quest) {
            const Quest& q = *s->quest;
            Json qq = Json::Object();
            qq.Set("type", q.type);
            qq.Set("hero", id_of(q.hero));
            qq.Set("done", q.done);
            qq.Set("required", OptInt(q.required));
            qq.Set("targetKind", q.target_kind);
            if (q.target_kind == "army")
                qq.Set("target", id_of(q.army));
            else if (q.target_kind == "armytype" && q.army_type)
                qq.Set("target", q.army_type->id);
            else if (q.target_kind == "city" && q.city)
                qq.Set("target", q.city->index);
            else if (q.target_kind == "side" && q.side)
                qq.Set("target", q.side->index);
            else if (q.target_kind == "item" && q.item)
                qq.Set("target", q.item->index);
            o.Set("quest", qq);
        }
        sides.Push(o);
    }

    Json cities = Json::Array();
    for (auto& c : g.map->cities) {
        Json o = Json::Object();
        o.Set("index", c.index);
        o.Set("name", c.name);
        o.Set("ownerIndex", OptInt(c.owner_index));
        o.Set("producing", OptInt(c.producing));
        o.Set("countdown", c.countdown);
        if (c.vector_to != kNone) o.Set("vectorTo", c.vector_to);
        if (c.razed) o.Set("razed", true);
        o.Set("defence", c.defence);
        o.Set("income", c.income);
        o.Set("claim", OptInt(c.claim));
        o.Set("razedBy", OptInt(c.razed_by));
        if (c.attacked) o.Set("attacked", c.attacked);
        Json slots = Json::Array();
        for (auto& s : c.slots) slots.Push(SlotJson(s));
        o.Set("slots", slots);
        cities.Push(o);
    }

    Json sites = Json::Array();
    for (auto& s : g.map->sites) {
        Json o = Json::Object();
        o.Set("index", s.index);
        o.Set("content", s.content);
        o.Set("item", OptInt(s.item));
        o.Set("guardian", OptInt(s.guardian));
        o.Set("allyType", OptInt(s.ally_type));
        if (s.rich) o.Set("rich", true);
        o.Set("revealed", s.revealed);
        if (s.searched) o.Set("searched", true);
        o.Set("band", s.band);
        o.Set("templeIndex", OptInt(s.temple_index));
        sites.Push(o);
    }

    Json items = Json::Array();
    for (auto& it : g.map->items) {
        Json o = Json::Object();
        o.Set("index", it.index);
        o.Set("name", it.name);
        o.Set("type", it.type);
        o.Set("value", it.value);
        o.Set("status", it.status);
        o.Set("x", OptInt(it.x));
        o.Set("y", OptInt(it.y));
        if (it.planted) o.Set("planted", true);
        o.Set("standardOf", OptInt(it.standard_of));
        items.Push(o);
    }

    Json signs = Json::Array();
    for (auto& sg : g.map->signs) {
        Json l = Json::Array();
        l.Push(sg.lines[0]);
        l.Push(sg.lines[1]);
        signs.Push(l);
    }
    Json towers = Json::Array();
    for (int k : g.towers) towers.Push(k);
    Json explored = Json::Object();
    for (auto& [k, m] : g.explored) explored.Set(std::to_string(k), PackMask(m));

    Json opts = Json::Object();
    for (auto& n : Options::Names()) opts.Set(n, *const_cast<Options&>(g.map->options).Field(n));

    Json dip = Json::Object();
    dip.Set("state", IntArray(g.diplomacy.state));
    dip.Set("proposal", IntArray(g.diplomacy.proposal));
    auto has_non_default = [](const auto& arr, int dflt) {
        for (int v : arr)
            if (v != dflt) return true;
        return false;
    };
    if (has_non_default(g.diplomacy.allied, 0)) dip.Set("allied", IntArray(g.diplomacy.allied));
    if (has_non_default(g.diplomacy.propose_allied, 0))
        dip.Set("proposeAllied", IntArray(g.diplomacy.propose_allied));
    if (has_non_default(g.diplomacy.truce_turns, 0))
        dip.Set("truceTurns", IntArray(g.diplomacy.truce_turns));
    if (has_non_default(g.diplomacy.gold_offer, 0))
        dip.Set("goldOffer", IntArray(g.diplomacy.gold_offer));
    if (has_non_default(g.diplomacy.war_target, kNone))
        dip.Set("warTarget", IntArray(g.diplomacy.war_target));
    if (has_non_default(g.diplomacy.trust, 0)) dip.Set("trust", IntArray(g.diplomacy.trust));
    if (has_non_default(g.diplomacy.allied_target_city, kNone))
        dip.Set("alliedTargetCity", IntArray(g.diplomacy.allied_target_city));

    Json fight = Json::Array();
    for (auto& row : g.map->fight_order) {
        Json r = Json::Array();
        for (int v : row) r.Push(v);
        fight.Push(r);
    }

    Json history = Json::Array();
    auto deed_json = [](const Deed& e) {
        Json o = Json::Object();
        o.Set("side", e.side);
        o.Set("type", static_cast<int>(e.type));
        o.Set("v1", e.v1);
        o.Set("v2", e.v2);
        o.Set("name", e.name);
        return o;
    };
    for (auto& r : g.history) {
        Json o = Json::Object();
        o.Set("gold", IntArray(r.gold));
        o.Set("score", IntArray(r.score));
        o.Set("cities", IntArray(r.cities));
        Json owners = Json::Array();
        for (int v : r.owners) owners.Push(v);
        o.Set("owners", owners);
        Json ev = Json::Array();
        for (auto& e : r.events) ev.Push(deed_json(e));
        o.Set("events", ev);
        history.Push(o);
    }
    Json deeds = Json::Object();
    for (auto& [k, list] : g.deeds) {
        Json l = Json::Array();
        for (auto& e : list) l.Push(deed_json(e));
        deeds.Set(std::to_string(k), l);
    }
    Json triumphs = Json::Object();
    for (auto& [me, row] : g.triumphs) {
        Json r = Json::Object();
        for (auto& [opp, counts] : row) r.Set(std::to_string(opp), IntArray(counts));
        triumphs.Set(std::to_string(me), r);
    }
    Json seen = Json::Array();
    for (auto& m : g.tutorial_seen) seen.Push(m);
    Json log = Json::Array();
    for (auto& l : g.log) log.Push(l);

    Json out = Json::Object();
    out.Set("version", kVersion);
    out.Set("scenario", g.map->name);
    out.Set("turn", g.turn);
    out.Set("current", g.current);
    out.Set("seed", static_cast<double>(g.rng.state));
    {
        Json order = Json::Array();
        for (int v : g.turn_order) order.Push(v);
        out.Set("turnOrder", order);
        out.Set("orderAt", g.order_at);
        if (g.ai_neighbours) {
            Json nb = Json::Object();
            for (size_t i = 0; i < g.ai_neighbours->by_city.size(); i++) {
                const auto& e = g.ai_neighbours->by_city[i];
                Json o = Json::Object();
                Json cs = Json::Array(), ds = Json::Array();
                for (int c : e.cities) cs.Push(c);
                for (int d : e.dist) ds.Push(d);
                o.Set("cities", cs);
                o.Set("dist", ds);
                nb.Set(std::to_string(i), o);
            }
            out.Set("aiNeighbours", nb);
        }
    }
    out.Set("won", g.won);
    out.Set("over", g.over);
    out.Set("noHumansSaid", g.no_humans_said);
    out.Set("greatest", g.greatest);
    out.Set("surrenderOffered", g.surrender_offered);
    out.Set("surrenderShown", g.surrender_shown);
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
    if (g.map->name == randommap::kDir) {
        Json world = Json::Object();
        for (auto& [ext, b] : randommap::Installed(g.data_dir)) world.Set(ext, Hex(b));
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
        for (auto& [ext, h] : state["randomWorld"].Entries()) files[ext] = Unhex(h.Str());
        randommap::Install(data_dir, files);
    }

    game::NewGameOptions opts;
    for (auto& [k, v] : state["options"].Entries())
        opts.options.emplace_back(k, static_cast<int>(v.Integer()));
    // the setup the game began with: who plays each side, its name, and the
    // sides turned Off, which are the ones the save leaves out -- as the
    // original's save keeps the .SCN image it was set up in (0x137, in play)
    for (int i = 0; i < 8; i++) opts.sides[i].off = true;
    for (auto& saved : state["sides"].Items()) {
        game::SideSetup& o = opts.sides[static_cast<int>(saved["index"].Integer())];
        o.off = false;
        o.name = saved["name"].Str("");
        o.computer = saved["computer"].Boolean();
        o.level = static_cast<int>(saved["level"].Integer());
        o.card = static_cast<int>(saved["card"].Integer());
    }
    auto gp = game::NewGame(data_dir, state["scenario"].Str(), opts);
    Game& g = *gp;
    g.armies.clear();
    g.turn = static_cast<int>(state["turn"].Integer());
    g.current = static_cast<int>(state["current"].Integer());
    if (state["turnOrder"].IsArray()) {
        for (size_t i = 0; i < 8 && i < state["turnOrder"].size(); i++)
            g.turn_order[i] = static_cast<int>(state["turnOrder"][i].Integer());
    }
    if (state["aiNeighbours"].IsObject()) {
        auto nb = std::make_shared<Neighbours>();
        nb->by_city.resize(g.map->cities.size());
        for (auto& [k, e] : state["aiNeighbours"].Entries()) {
            size_t i = static_cast<size_t>(std::stoi(k));
            if (i >= nb->by_city.size()) continue;
            for (auto& v : e["cities"].Items())
                nb->by_city[i].cities.push_back(static_cast<int>(v.Integer()));
            for (auto& v : e["dist"].Items())
                nb->by_city[i].dist.push_back(static_cast<int>(v.Integer()));
        }
        g.ai_neighbours = nb;
    }
    g.rng.state = static_cast<uint32_t>(state["seed"].Number());
    g.won = state["won"].Boolean();
    g.over = state["over"].Boolean();
    g.surrender_offered = state["surrenderOffered"].Boolean();
    g.surrender_shown = state["surrenderShown"].Boolean();
    g.no_humans_said = state["noHumansSaid"].Boolean();
    g.greatest = state["greatest"].Boolean();
    ReadIntArray(state["diplomacy"]["state"], g.diplomacy.state);
    ReadIntArray(state["diplomacy"]["proposal"], g.diplomacy.proposal);
    if (state["diplomacy"].Has("allied"))
        ReadIntArray(state["diplomacy"]["allied"], g.diplomacy.allied);
    if (state["diplomacy"].Has("proposeAllied"))
        ReadIntArray(state["diplomacy"]["proposeAllied"], g.diplomacy.propose_allied);
    if (state["diplomacy"].Has("truceTurns"))
        ReadIntArray(state["diplomacy"]["truceTurns"], g.diplomacy.truce_turns);
    if (state["diplomacy"].Has("goldOffer"))
        ReadIntArray(state["diplomacy"]["goldOffer"], g.diplomacy.gold_offer);
    if (state["diplomacy"].Has("warTarget"))
        ReadIntArray(state["diplomacy"]["warTarget"], g.diplomacy.war_target);
    if (state["diplomacy"].Has("trust"))
        ReadIntArray(state["diplomacy"]["trust"], g.diplomacy.trust);
    if (state["diplomacy"].Has("alliedTargetCity"))
        ReadIntArray(state["diplomacy"]["alliedTargetCity"], g.diplomacy.allied_target_city);
    g.log.clear();
    for (auto& l : state["log"].Items()) g.log.push_back(l.Str());

    std::map<int, Item*> item_by_index;
    for (auto& saved : state["items"].Items()) {
        for (auto& it : g.map->items) {
            if (it.index == saved["index"].Integer()) {
                it.name = saved["name"].Str();
                it.type = static_cast<int>(saved["type"].Integer());
                it.value = static_cast<int>(saved["value"].Integer());
                it.status = static_cast<int>(saved["status"].Integer());
                it.x = ReadOpt(saved["x"]);
                it.y = ReadOpt(saved["y"]);
                it.planted = saved["planted"].Boolean();
                it.standard_of = ReadOpt(saved["standardOf"]);
                item_by_index[it.index] = &it;
            }
        }
    }

    for (auto& saved : state["cities"].Items()) {
        City* c = g.map->City(static_cast<int>(saved["index"].Integer()));
        if (!c) continue;
        c->owner_index = ReadOpt(saved["ownerIndex"]);
        c->producing = ReadOpt(saved["producing"]);
        c->countdown = static_cast<int>(saved["countdown"].Integer());
        c->vector_to =
                saved.Has("vectorTo") ? static_cast<int>(saved["vectorTo"].Integer()) : kNone;
        c->razed = saved["razed"].Boolean();
        c->defence = static_cast<int>(saved["defence"].Integer());
        c->income = static_cast<int>(saved["income"].Integer());
        c->slots.clear();
        for (auto& s : saved["slots"].Items()) c->slots.push_back(ReadSlot(s));
        if (!saved["claim"].IsNull()) c->claim = static_cast<int>(saved["claim"].Integer());
        c->razed_by = ReadOpt(saved["razedBy"]);
        c->attacked = static_cast<int>(saved["attacked"].Integer());
        if (!saved["name"].Str().empty()) c->name = saved["name"].Str();
        // ruins belong to nobody; older saves can have them won in a fight
        if (c->razed) c->owner_index = kNone;
    }
    scn::RefreshCityTiles(*g.map);

    for (auto& saved : state["sites"].Items()) {
        int i = static_cast<int>(saved["index"].Integer());
        if (i < 0 || i >= static_cast<int>(g.map->sites.size())) continue;
        Site& s = g.map->sites[i];
        s.content = static_cast<int>(saved["content"].Integer());
        s.item = ReadOpt(saved["item"]);
        s.guardian = ReadOpt(saved["guardian"]);
        s.ally_type = ReadOpt(saved["allyType"]);
        s.rich = saved["rich"].Boolean();
        s.revealed = static_cast<int>(saved["revealed"].Integer());
        s.searched = saved["searched"].Boolean();
        s.band = saved["band"].Str();
        s.temple_index = ReadOpt(saved["templeIndex"]);
    }
    if (state["fightOrder"].IsArray()) {
        g.map->fight_order.clear();
        for (auto& row : state["fightOrder"].Items()) {
            std::vector<int> r;
            for (auto& v : row.Items()) r.push_back(static_cast<int>(v.Integer()));
            g.map->fight_order.push_back(r);
        }
    }
    g.history.clear();
    auto read_deed = [](const Json& o) {
        return Deed{static_cast<int>(o["side"].Integer()),
                    static_cast<DeedType>(o["type"].Integer()), static_cast<int>(o["v1"].Integer()),
                    static_cast<int>(o["v2"].Integer()), o["name"].Str()};
    };
    for (auto& r : state["history"].Items()) {
        HistoryRecord rec;
        ReadIntArray(r["gold"], rec.gold);
        ReadIntArray(r["score"], rec.score);
        ReadIntArray(r["cities"], rec.cities);
        for (auto& v : r["owners"].Items()) rec.owners.push_back(static_cast<int>(v.Integer()));
        for (auto& e : r["events"].Items()) rec.events.push_back(read_deed(e));
        g.history.push_back(rec);
    }
    g.deeds.clear();
    for (auto& [k, list] : state["deeds"].Entries())
        for (auto& e : list.Items()) g.deeds[std::stoi(k)].push_back(read_deed(e));
    g.triumphs.clear();
    for (auto& [me, row] : state["triumphs"].Entries())
        for (auto& [opp, counts] : row.Entries())
            ReadIntArray(counts, g.triumphs[std::stoi(me)][std::stoi(opp)]);
    g.tutorial_seen.clear();
    for (auto& m : state["tutorialSeen"].Items()) g.tutorial_seen.insert(m.Str());
    g.towers.clear();
    for (auto& k : state["towers"].Items()) g.towers.insert(static_cast<int>(k.Integer()));
    const Json& signs = state["signs"];
    for (size_t i = 0; i < g.map->signs.size() && i < signs.size(); i++) {
        g.map->signs[i].lines = {signs[i][0].Str(), signs[i][1].Str()};
    }
    g.explored.clear();
    for (auto& [k, runs] : state["explored"].Entries())
        g.explored[std::stoi(k)] =
                UnpackMask(runs, static_cast<size_t>(g.map->width) * g.map->height);

    std::vector<Army*> armies;
    for (auto& saved : state["armies"].Items()) {
        Army a;
        a.x = ReadOpt(saved["x"]);
        a.y = ReadOpt(saved["y"]);
        a.owner = ReadOpt(saved["owner"]);
        a.type = static_cast<int>(saved["type"].Integer());
        a.name = saved["name"].Str();
        a.female = saved["female"].Boolean();
        a.medals = static_cast<int>(saved["medals"].Integer());
        a.strength = static_cast<int>(saved["strength"].Integer());
        a.moves = static_cast<int>(saved["moves"].Integer());
        a.max_moves = static_cast<int>(saved["maxMoves"].Integer());
        a.upkeep = static_cast<int>(saved["upkeep"].Integer());
        a.home_city = ReadOpt(saved["homeCity"]);
        a.at_sea = saved["atSea"].Boolean();
        a.group = static_cast<int>(saved["group"].Integer());
        if (saved["target"].IsArray())
            a.target = std::make_pair(static_cast<int>(saved["target"][0].Integer()),
                                      static_cast<int>(saved["target"][1].Integer()));
        a.fortified = saved["fortified"].Boolean();
        a.level = static_cast<int>(saved["level"].Integer());
        a.experience = static_cast<int>(saved["experience"].Integer());
        a.title = saved["title"].Str("");
        for (auto& idx : saved["items"].Items()) {
            auto it = item_by_index.find(static_cast<int>(idx.Integer()));
            if (it != item_by_index.end()) a.items.push_back(it->second);
        }
        for (auto& t : saved["blessings"].Items())
            a.blessings.insert(static_cast<int>(t.Integer()));
        if (saved["transit"].IsObject())
            a.transit = Transit{static_cast<int>(saved["transit"]["turns"].Integer()),
                                static_cast<int>(saved["transit"]["dest"].Integer())};
        a.returning = saved["returning"].Boolean();
        a.ai_order = static_cast<int>(saved["aiOrder"].Integer());
        a.ai_dest = saved.Has("aiDest") ? static_cast<int>(saved["aiDest"].Integer()) : kNone;
        a.ai_group = static_cast<int>(saved["aiGroup"].Integer());
        a.ai_explore = saved["aiExplore"].Boolean();
        a.ai_neutral = saved["aiNeutral"].Boolean();
        a.ai_party = saved["aiParty"].Boolean();
        // the army table in slot order; older saves had none, and their order
        a.slot = saved.Has("slot") && !saved["slot"].IsNull()
                         ? static_cast<int>(saved["slot"].Integer())
                         : static_cast<int>(armies.size());
        armies.push_back(g.Make(a));
    }
    g.armies = armies;
    std::stable_sort(g.armies.begin(), g.armies.end(),
                     [](Army* p, Army* q) { return p->slot < q->slot; });

    for (auto& saved : state["sides"].Items()) {
        Side* s = g.map->Side(static_cast<int>(saved["index"].Integer()));
        if (!s) continue;
        s->gold = static_cast<int>(saved["gold"].Integer());
        s->alive = saved["alive"].Boolean();
        s->computer = saved["computer"].Boolean();
        s->level = static_cast<int>(saved["level"].Integer());
        s->enhanced = saved["enhanced"].Boolean();
        s->diplo_score = static_cast<int>(saved["diploScore"].Integer());
        s->observe = saved["observe"].Boolean();
        if (saved["capNow"].IsObject()) {
            s->cap_now_x = static_cast<int>(saved["capNow"]["x"].Integer());
            s->cap_now_y = static_cast<int>(saved["capNow"]["y"].Integer());
        }
        s->production_due = saved["productionDue"].Boolean();
        s->income = static_cast<int>(saved["income"].Integer());
        s->upkeep_total = static_cast<int>(saved["upkeepTotal"].Integer());
        s->ai_solidarity = static_cast<int>(saved["aiSolidarity"].Integer());
        s->card = static_cast<int>(saved["card"].Integer());
        s->classic_ai = saved.Has("classicAi") ? saved["classicAi"].Boolean(true) : true;
        ai::belief::Ensure(*s);
        s->advisor.dir = static_cast<int>(saved["advisor"]["dir"].Integer());
        s->advisor.mark = static_cast<int>(saved["advisor"]["mark"].Integer());
        if (saved["ai"].IsObject()) {
            s->ai = std::make_shared<AIData>(ai::core::NewData());
            LoadAi(*s->ai, saved["ai"], armies);
        }
        s->quest.reset();
        if (saved["quest"].IsObject()) {
            const Json& qs = saved["quest"];
            auto q = std::make_shared<Quest>();
            q->type = static_cast<int>(qs["type"].Integer());
            q->done = static_cast<int>(qs["done"].Integer());
            q->required = ReadOpt(qs["required"]);
            long hero = qs["hero"].Integer(-1);
            q->hero = hero >= 0 && hero < static_cast<long>(armies.size()) ? armies[hero] : nullptr;
            q->target_kind = qs["targetKind"].Str();
            int ref = static_cast<int>(qs["target"].Integer(-1));
            if (q->target_kind == "city")
                q->city = g.map->City(ref);
            else if (q->target_kind == "side")
                q->side = g.map->Side(ref);
            else if (q->target_kind == "item")
                q->item = item_by_index.count(ref) ? item_by_index[ref] : nullptr;
            else if (q->target_kind == "armytype")
                q->army_type = g.types.ById(ref);
            else if (q->target_kind == "army")
                q->army = ref >= 0 && ref < static_cast<int>(armies.size()) ? armies[ref] : nullptr;
            if (q->hero && q->HasTarget()) s->quest = q;
        }
    }

    // a flier is never at sea, nor a hero flying with one (1a8b:04c8): the
    // Lua puts right any save that says otherwise, and so does this
    std::set<int> fliers, walkers;
    for (Army* a : g.armies) {
        if (a->x != kNone) {
            int k = a->y * g.map->width + a->x;
            if (g.types.ById(a->type)->flies)
                fliers.insert(k);
            else if (a->type != armytype::kHero)
                walkers.insert(k);
        }
    }
    for (Army* a : g.armies) {
        if (a->at_sea && a->x != kNone) {
            int k = a->y * g.map->width + a->x;
            if (g.types.ById(a->type)->flies ||
                (a->type == armytype::kHero && fliers.count(k) && !walkers.count(k))) {
                a->at_sea = false;
            }
        }
    }

    g.side = g.current < static_cast<int>(g.sides.size()) ? g.sides[g.current] : nullptr;
    // where the side to play stands in the turn order; older saves: the order
    // as it is
    if (state.Has("orderAt") && !state["orderAt"].IsNull()) {
        g.order_at = static_cast<int>(state["orderAt"].Integer());
    } else {
        int me = g.side ? g.side->index : 0;
        g.order_at = -1;
        for (int i = 0; i < 8; i++)
            if (g.turn_order[i] == me) {
                g.order_at = i;
                break;
            }
    }
    move::Invalidate(g);
    return gp;
}

}  // namespace w2::save
