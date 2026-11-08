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
    Json o = Json::object();
    for (auto& [k, v] : m) o.set(std::to_string(k), v);
    return o;
}
std::map<int, int> ReadIntMap(const Json& j) {
    std::map<int, int> m;
    for (auto& [k, v] : j.entries()) m[std::stoi(k)] = static_cast<int>(v.integer());
    return m;
}
template <typename T, size_t N>
Json IntArray(const std::array<T, N>& a) {
    Json o = Json::array();
    for (T v : a) o.push(static_cast<int>(v));
    return o;
}
template <typename T, size_t N>
void ReadIntArray(const Json& j, std::array<T, N>& a) {
    for (size_t i = 0; i < N && i < j.size(); i++) a[i] = static_cast<T>(j[i].integer());
}
Json OptInt(int v) {
    return v == NONE ? Json() : Json(v);
}
int ReadOpt(const Json& j) {
    return j.isNull() ? NONE : static_cast<int>(j.integer());
}

// the hidden map, a side's mask as runs of seen and unseen tiles
Json PackMask(const std::vector<uint8_t>& mask) {
    Json runs = Json::array();
    int cur = 0, n = 0;
    for (uint8_t m : mask) {
        int v = m ? 1 : 0;
        if (v == cur)
            n++;
        else {
            runs.push(n);
            cur = v;
            n = 1;
        }
    }
    runs.push(n);
    return runs;
}
std::vector<uint8_t> UnpackMask(const Json& runs, size_t size) {
    std::vector<uint8_t> mask(size, 0);
    size_t i = 0;
    int v = 0;
    for (auto& n : runs.items()) {
        for (long k = 0; k < n.integer() && i < size; k++) mask[i++] = static_cast<uint8_t>(v);
        v = 1 - v;
    }
    return mask;
}

Json SaveAi(const AIData& d, const std::map<const Army*, int>& ids) {
    Json o = Json::object();
    o.set("turns", d.turns);
    o.set("own", d.own);
    o.set("enemy", d.enemy);
    o.set("neutral", d.neutral);
    o.set("unseen", d.unseen);
    o.set("rebuildLimit", d.rebuildLimit);
    o.set("bold", d.bold);
    o.set("heroes", d.heroes);
    o.set("rebuildType", d.rebuildType);
    o.set("rebuildTypeRich", d.rebuildTypeRich);
    o.set("bought", d.bought);
    o.set("dieHuman", d.dieHuman);
    o.set("dieLord", d.dieLord);
    o.set("dieKnight", d.dieKnight);
    o.set("dieWarlord", d.dieWarlord);
    o.set("sims", d.sims);
    o.set("searchers", d.searchers);
    o.set("explorers", d.explorers);
    o.set("raze", d.raze);
    o.set("sack", d.sack);
    o.set("pillage", d.pillage);
    o.set("perCity", d.perCity);
    o.set("bonusHuman", d.bonusHuman);
    o.set("bonusWarlord", d.bonusWarlord);
    o.set("bonusLord", d.bonusLord);
    o.set("bonusKnight", d.bonusKnight);
    o.set("poor", d.poor);
    o.set("solidarity", d.solidarity);
    o.set("minStrength", d.minStrength);
    o.set("flyCities", d.flyCities);
    o.set("strongCities", d.strongCities);
    o.set("fastCities", d.fastCities);
    o.set("early", d.early);
    o.set("humanShare", d.humanShare);
    o.set("questCity", OptInt(d.questCity));
    o.set("cautious", d.cautious);
    o.set("questsDone", d.questsDone);
    o.set("itemsPassed", d.itemsPassed);
    o.set("maxGroups", d.maxGroups);
    o.set("roles", IntMap(d.roles));
    o.set("held", IntMap(d.held));
    o.set("flags", IntMap(d.flags));
    o.set("garrison", IntMap(d.garrison));
    o.set("keep", IntMap(d.keep));
    o.set("heroesKilled", IntArray(d.heroesKilled));
    o.set("armiesKilled", IntArray(d.armiesKilled));
    o.set("battles", IntArray(d.battles));
    o.set("lost", IntArray(d.lost));
    o.set("cityBattles", IntArray(d.cityBattles));
    o.set("citiesLost", IntArray(d.citiesLost));
    if (d.cursor) {
        Json c = Json::array();
        c.push(d.cursor->first);
        c.push(d.cursor->second);
        o.set("cursor", c);
    }
    Json groups = Json::array();
    for (int gi = 1; gi <= ai::core::MAX_GROUPS; gi++) {
        const Group& grp = *d.groups[gi];
        Json gr = Json::object();
        gr.set("active", grp.active);
        gr.set("target", OptInt(grp.target));
        gr.set("rally", OptInt(grp.rally));
        gr.set("members", IntMap(grp.members));
        gr.set("cities", IntMap(grp.cities));
        gr.set("dist", IntMap(grp.dist));
        gr.set("bonus", IntMap(grp.bonus));
        gr.set("taken", IntMap(grp.taken));
        Json staged = Json::object();
        for (auto& [k, a] : grp.staged) {
            auto it = ids.find(a);
            if (a && it != ids.end()) staged.set(std::to_string(k), it->second);
        }
        gr.set("staged", staged);
        gr.set("size", grp.size);
        gr.set("flags", grp.flags);
        groups.push(gr);
    }
    o.set("groups", groups);
    return o;
}

void LoadAi(AIData& d, const Json& o, const std::vector<Army*>& armies) {
    auto read_int = [&](const char* k, int& v) {
        if (o.has(k)) v = static_cast<int>(o[k].integer());
    };
    read_int("turns", d.turns);
    read_int("own", d.own);
    read_int("enemy", d.enemy);
    read_int("neutral", d.neutral);
    read_int("unseen", d.unseen);
    read_int("rebuildLimit", d.rebuildLimit);
    d.bold = o["bold"].boolean();
    read_int("heroes", d.heroes);
    read_int("rebuildType", d.rebuildType);
    read_int("rebuildTypeRich", d.rebuildTypeRich);
    read_int("bought", d.bought);
    read_int("dieHuman", d.dieHuman);
    read_int("dieLord", d.dieLord);
    read_int("dieKnight", d.dieKnight);
    read_int("dieWarlord", d.dieWarlord);
    read_int("sims", d.sims);
    read_int("searchers", d.searchers);
    read_int("explorers", d.explorers);
    read_int("raze", d.raze);
    read_int("sack", d.sack);
    read_int("pillage", d.pillage);
    read_int("perCity", d.perCity);
    read_int("bonusHuman", d.bonusHuman);
    read_int("bonusWarlord", d.bonusWarlord);
    read_int("bonusLord", d.bonusLord);
    read_int("bonusKnight", d.bonusKnight);
    read_int("poor", d.poor);
    read_int("solidarity", d.solidarity);
    read_int("minStrength", d.minStrength);
    read_int("flyCities", d.flyCities);
    read_int("strongCities", d.strongCities);
    read_int("fastCities", d.fastCities);
    read_int("early", d.early);
    read_int("humanShare", d.humanShare);
    d.questCity = ReadOpt(o["questCity"]);
    read_int("cautious", d.cautious);
    read_int("questsDone", d.questsDone);
    read_int("itemsPassed", d.itemsPassed);
    read_int("maxGroups", d.maxGroups);
    d.roles = ReadIntMap(o["roles"]);
    d.held = ReadIntMap(o["held"]);
    d.flags = ReadIntMap(o["flags"]);
    d.garrison = ReadIntMap(o["garrison"]);
    d.keep = ReadIntMap(o["keep"]);
    ReadIntArray(o["heroesKilled"], d.heroesKilled);
    ReadIntArray(o["armiesKilled"], d.armiesKilled);
    ReadIntArray(o["battles"], d.battles);
    ReadIntArray(o["lost"], d.lost);
    ReadIntArray(o["cityBattles"], d.cityBattles);
    ReadIntArray(o["citiesLost"], d.citiesLost);
    if (o["cursor"].isArray())
        d.cursor = std::make_pair(static_cast<int>(o["cursor"][0].integer()),
                                  static_cast<int>(o["cursor"][1].integer()));
    else
        d.cursor.reset();
    const Json& groups = o["groups"];
    for (int gi = 1; gi <= ai::core::MAX_GROUPS; gi++) {
        auto grp = ai::core::emptyGroup();
        const Json& gr = groups[gi - 1];
        if (gr.isObject()) {
            grp->active = static_cast<int>(gr["active"].integer());
            grp->target = ReadOpt(gr["target"]);
            grp->rally = ReadOpt(gr["rally"]);
            grp->members = ReadIntMap(gr["members"]);
            grp->cities = ReadIntMap(gr["cities"]);
            grp->dist = ReadIntMap(gr["dist"]);
            grp->bonus = ReadIntMap(gr["bonus"]);
            grp->taken = ReadIntMap(gr["taken"]);
            for (auto& [k, v] : gr["staged"].entries()) {
                long id = v.integer();
                if (id >= 0 && id < static_cast<long>(armies.size()))
                    grp->staged[std::stoi(k)] = armies[id];
            }
            grp->size = static_cast<int>(gr["size"].integer());
            grp->flags = static_cast<int>(gr["flags"].integer());
        }
        d.groups[gi] = grp;
    }
}

Json SlotJson(const Slot& s) {
    Json o = Json::object();
    o.set("type", s.type);
    o.set("name", s.name);
    o.set("strength", s.strength);
    o.set("time", s.time);
    o.set("cost", s.cost);
    o.set("move", s.move);
    o.set("price", s.price);
    return o;
}
Slot ReadSlot(const Json& o) {
    return Slot{static_cast<int>(o["type"].integer()),     o["name"].str(),
                static_cast<int>(o["strength"].integer()), static_cast<int>(o["time"].integer()),
                static_cast<int>(o["cost"].integer()),     static_cast<int>(o["move"].integer()),
                static_cast<int>(o["price"].integer())};
}
}  // namespace

std::string encode(const Game& g) {
    std::map<const Army*, int> ids;
    for (size_t i = 0; i < g.armies.size(); i++) ids[g.armies[i]] = static_cast<int>(i);
    auto id_of = [&](const Army* a) -> Json {
        auto it = ids.find(a);
        return it == ids.end() ? Json() : Json(it->second);
    };

    Json armies = Json::array();
    for (Army* a : g.armies) {
        Json o = Json::object();
        o.set("slot", a->slot);
        o.set("x", OptInt(a->x));
        o.set("y", OptInt(a->y));
        o.set("owner", OptInt(a->owner));
        o.set("type", a->type);
        o.set("name", a->name);
        if (a->female) o.set("female", true);
        if (a->medals) o.set("medals", a->medals);
        o.set("strength", a->strength);
        o.set("moves", a->moves);
        o.set("maxMoves", a->maxMoves);
        o.set("upkeep", a->upkeep);
        o.set("homeCity", OptInt(a->homeCity));
        if (a->atSea) o.set("atSea", true);
        if (a->group) o.set("group", a->group);
        if (a->target) {
            Json t = Json::array();
            t.push(a->target->first);
            t.push(a->target->second);
            o.set("target", t);
        }
        if (a->fortified) o.set("fortified", true);
        if (a->level) o.set("level", a->level);
        if (a->experience) o.set("experience", a->experience);
        if (!a->title.empty()) o.set("title", a->title);
        if (!a->items.empty()) {
            Json its = Json::array();
            for (Item* it : a->items) its.push(it->index);
            o.set("items", its);
        }
        if (!a->blessings.empty()) {
            Json b = Json::array();
            for (int t : a->blessings) b.push(t);
            o.set("blessings", b);
        }
        if (a->transit) {
            Json t = Json::object();
            t.set("turns", a->transit->turns);
            t.set("dest", a->transit->dest);
            o.set("transit", t);
        }
        if (a->returning) o.set("returning", true);
        if (a->aiOrder) o.set("aiOrder", a->aiOrder);
        if (a->aiDest != NONE) o.set("aiDest", a->aiDest);
        if (a->aiGroup) o.set("aiGroup", a->aiGroup);
        if (a->aiExplore) o.set("aiExplore", true);
        if (a->aiNeutral) o.set("aiNeutral", true);
        if (a->aiParty) o.set("aiParty", true);
        armies.push(o);
    }

    Json sides = Json::array();
    for (Side* s : g.sides) {
        Json o = Json::object();
        o.set("index", s->index);
        o.set("name", s->name);
        o.set("gold", s->gold);
        o.set("alive", s->alive);
        o.set("computer", s->computer);
        o.set("level", s->level);
        o.set("enhanced", s->enhanced);
        o.set("observe", s->observe);
        if (s->capNowX != NONE) {
            Json cn = Json::object();
            cn.set("x", s->capNowX);
            cn.set("y", s->capNowY);
            o.set("capNow", cn);
        }
        if (s->productionDue) o.set("productionDue", true);
        o.set("diploScore", s->diploScore);
        o.set("income", s->income);
        o.set("upkeepTotal", s->upkeepTotal);
        o.set("aiSolidarity", s->aiSolidarity);
        o.set("card", s->card);
        if (!s->classicAi) o.set("classicAi", false);
        Json adv = Json::object();
        adv.set("dir", s->advisor.dir);
        adv.set("mark", s->advisor.mark);
        o.set("advisor", adv);
        if (s->ai) o.set("ai", SaveAi(*s->ai, ids));
        if (s->quest) {
            const Quest& q = *s->quest;
            Json qq = Json::object();
            qq.set("type", q.type);
            qq.set("hero", id_of(q.hero));
            qq.set("done", q.done);
            qq.set("required", OptInt(q.required));
            qq.set("targetKind", q.targetKind);
            if (q.targetKind == "army")
                qq.set("target", id_of(q.army));
            else if (q.targetKind == "armytype" && q.armyType)
                qq.set("target", q.armyType->id);
            else if (q.targetKind == "city" && q.city)
                qq.set("target", q.city->index);
            else if (q.targetKind == "side" && q.side)
                qq.set("target", q.side->index);
            else if (q.targetKind == "item" && q.item)
                qq.set("target", q.item->index);
            o.set("quest", qq);
        }
        sides.push(o);
    }

    Json cities = Json::array();
    for (auto& c : g.map->cities) {
        Json o = Json::object();
        o.set("index", c.index);
        o.set("name", c.name);
        o.set("ownerIndex", OptInt(c.ownerIndex));
        o.set("producing", OptInt(c.producing));
        o.set("countdown", c.countdown);
        if (c.vectorTo != NONE) o.set("vectorTo", c.vectorTo);
        if (c.razed) o.set("razed", true);
        o.set("defence", c.defence);
        o.set("income", c.income);
        o.set("claim", OptInt(c.claim));
        o.set("razedBy", OptInt(c.razedBy));
        if (c.attacked) o.set("attacked", c.attacked);
        Json slots = Json::array();
        for (auto& s : c.slots) slots.push(SlotJson(s));
        o.set("slots", slots);
        cities.push(o);
    }

    Json sites = Json::array();
    for (auto& s : g.map->sites) {
        Json o = Json::object();
        o.set("index", s.index);
        o.set("content", s.content);
        o.set("item", OptInt(s.item));
        o.set("guardian", OptInt(s.guardian));
        o.set("allyType", OptInt(s.allyType));
        if (s.rich) o.set("rich", true);
        o.set("revealed", s.revealed);
        if (s.searched) o.set("searched", true);
        o.set("band", s.band);
        o.set("templeIndex", OptInt(s.templeIndex));
        sites.push(o);
    }

    Json items = Json::array();
    for (auto& it : g.map->items) {
        Json o = Json::object();
        o.set("index", it.index);
        o.set("name", it.name);
        o.set("type", it.type);
        o.set("value", it.value);
        o.set("status", it.status);
        o.set("x", OptInt(it.x));
        o.set("y", OptInt(it.y));
        if (it.planted) o.set("planted", true);
        o.set("standardOf", OptInt(it.standardOf));
        items.push(o);
    }

    Json signs = Json::array();
    for (auto& sg : g.map->signs) {
        Json l = Json::array();
        l.push(sg.lines[0]);
        l.push(sg.lines[1]);
        signs.push(l);
    }
    Json towers = Json::array();
    for (int k : g.towers) towers.push(k);
    Json explored = Json::object();
    for (auto& [k, m] : g.explored) explored.set(std::to_string(k), PackMask(m));

    Json opts = Json::object();
    for (auto& n : Options::names()) opts.set(n, *const_cast<Options&>(g.map->options).field(n));

    Json dip = Json::object();
    dip.set("state", IntArray(g.diplomacy.state));
    dip.set("proposal", IntArray(g.diplomacy.proposal));
    auto has_non_default = [](const auto& arr, int dflt) {
        for (int v : arr)
            if (v != dflt) return true;
        return false;
    };
    if (has_non_default(g.diplomacy.allied, 0)) dip.set("allied", IntArray(g.diplomacy.allied));
    if (has_non_default(g.diplomacy.proposeAllied, 0))
        dip.set("proposeAllied", IntArray(g.diplomacy.proposeAllied));
    if (has_non_default(g.diplomacy.truceTurns, 0))
        dip.set("truceTurns", IntArray(g.diplomacy.truceTurns));
    if (has_non_default(g.diplomacy.goldOffer, 0))
        dip.set("goldOffer", IntArray(g.diplomacy.goldOffer));
    if (has_non_default(g.diplomacy.warTarget, NONE))
        dip.set("warTarget", IntArray(g.diplomacy.warTarget));
    if (has_non_default(g.diplomacy.trust, 0)) dip.set("trust", IntArray(g.diplomacy.trust));
    if (has_non_default(g.diplomacy.alliedTargetCity, NONE))
        dip.set("alliedTargetCity", IntArray(g.diplomacy.alliedTargetCity));

    Json fight = Json::array();
    for (auto& row : g.map->fightOrder) {
        Json r = Json::array();
        for (int v : row) r.push(v);
        fight.push(r);
    }

    Json history = Json::array();
    auto deed_json = [](const Deed& e) {
        Json o = Json::object();
        o.set("side", e.side);
        o.set("type", static_cast<int>(e.type));
        o.set("v1", e.v1);
        o.set("v2", e.v2);
        o.set("name", e.name);
        return o;
    };
    for (auto& r : g.history) {
        Json o = Json::object();
        o.set("gold", IntArray(r.gold));
        o.set("score", IntArray(r.score));
        o.set("cities", IntArray(r.cities));
        Json owners = Json::array();
        for (int v : r.owners) owners.push(v);
        o.set("owners", owners);
        Json ev = Json::array();
        for (auto& e : r.events) ev.push(deed_json(e));
        o.set("events", ev);
        history.push(o);
    }
    Json deeds = Json::object();
    for (auto& [k, list] : g.deeds) {
        Json l = Json::array();
        for (auto& e : list) l.push(deed_json(e));
        deeds.set(std::to_string(k), l);
    }
    Json triumphs = Json::object();
    for (auto& [me, row] : g.triumphs) {
        Json r = Json::object();
        for (auto& [opp, counts] : row) r.set(std::to_string(opp), IntArray(counts));
        triumphs.set(std::to_string(me), r);
    }
    Json seen = Json::array();
    for (auto& m : g.tutorialSeen) seen.push(m);
    Json log = Json::array();
    for (auto& l : g.log) log.push(l);

    Json out = Json::object();
    out.set("version", VERSION);
    out.set("scenario", g.map->name);
    out.set("turn", g.turn);
    out.set("current", g.current);
    out.set("seed", static_cast<double>(g.rng.state));
    {
        Json order = Json::array();
        for (int v : g.turnOrder) order.push(v);
        out.set("turnOrder", order);
        out.set("orderAt", g.orderAt);
        if (g.aiNeighbours) {
            Json nb = Json::object();
            for (size_t i = 0; i < g.aiNeighbours->byCity.size(); i++) {
                const auto& e = g.aiNeighbours->byCity[i];
                Json o = Json::object();
                Json cs = Json::array(), ds = Json::array();
                for (int c : e.cities) cs.push(c);
                for (int d : e.dist) ds.push(d);
                o.set("cities", cs);
                o.set("dist", ds);
                nb.set(std::to_string(i), o);
            }
            out.set("aiNeighbours", nb);
        }
    }
    out.set("won", g.won);
    out.set("over", g.over);
    out.set("noHumansSaid", g.noHumansSaid);
    out.set("greatest", g.greatest);
    out.set("surrenderOffered", g.surrenderOffered);
    out.set("surrenderShown", g.surrenderShown);
    out.set("options", opts);
    out.set("diplomacy", dip);
    out.set("sides", sides);
    out.set("cities", cities);
    out.set("sites", sites);
    out.set("items", items);
    out.set("armies", armies);
    out.set("signs", signs);
    out.set("fightOrder", fight);
    out.set("towers", towers);
    out.set("explored", explored);
    out.set("history", history);
    out.set("deeds", deeds);
    out.set("triumphs", triumphs);
    out.set("tutorialSeen", seen);
    out.set("log", log);
    if (g.map->name == randommap::DIR) {
        Json world = Json::object();
        for (auto& [ext, b] : randommap::installed(g.dataDir)) world.set(ext, Hex(b));
        out.set("randomWorld", world);
    }
    return out.dump();
}

std::unique_ptr<Game> decode(const std::string& text, const std::string& data_dir) {
    Json state = Json::parse(text);
    if (state["version"].integer() != VERSION) throw std::runtime_error("unsupported save version");

    if (state.has("randomWorld")) {
        randommap::Files files;
        for (auto& [ext, h] : state["randomWorld"].entries()) files[ext] = Unhex(h.str());
        randommap::install(data_dir, files);
    }

    game::NewGameOptions opts;
    for (auto& [k, v] : state["options"].entries())
        opts.options.emplace_back(k, static_cast<int>(v.integer()));
    // the setup the game began with: who plays each side, its name, and the
    // sides turned Off, which are the ones the save leaves out -- as the
    // original's save keeps the .SCN image it was set up in (0x137, in play)
    for (int i = 0; i < 8; i++) opts.sides[i].off = true;
    for (auto& saved : state["sides"].items()) {
        game::SideSetup& o = opts.sides[static_cast<int>(saved["index"].integer())];
        o.off = false;
        o.name = saved["name"].str("");
        o.computer = saved["computer"].boolean();
        o.level = static_cast<int>(saved["level"].integer());
        o.card = static_cast<int>(saved["card"].integer());
    }
    auto gp = game::newGame(data_dir, state["scenario"].str(), opts);
    Game& g = *gp;
    g.armies.clear();
    g.turn = static_cast<int>(state["turn"].integer());
    g.current = static_cast<int>(state["current"].integer());
    if (state["turnOrder"].isArray()) {
        for (size_t i = 0; i < 8 && i < state["turnOrder"].size(); i++)
            g.turnOrder[i] = static_cast<int>(state["turnOrder"][i].integer());
    }
    if (state["aiNeighbours"].isObject()) {
        auto nb = std::make_shared<Neighbours>();
        nb->byCity.resize(g.map->cities.size());
        for (auto& [k, e] : state["aiNeighbours"].entries()) {
            size_t i = static_cast<size_t>(std::stoi(k));
            if (i >= nb->byCity.size()) continue;
            for (auto& v : e["cities"].items())
                nb->byCity[i].cities.push_back(static_cast<int>(v.integer()));
            for (auto& v : e["dist"].items())
                nb->byCity[i].dist.push_back(static_cast<int>(v.integer()));
        }
        g.aiNeighbours = nb;
    }
    g.rng.state = static_cast<uint32_t>(state["seed"].number());
    g.won = state["won"].boolean();
    g.over = state["over"].boolean();
    g.surrenderOffered = state["surrenderOffered"].boolean();
    g.surrenderShown = state["surrenderShown"].boolean();
    g.noHumansSaid = state["noHumansSaid"].boolean();
    g.greatest = state["greatest"].boolean();
    ReadIntArray(state["diplomacy"]["state"], g.diplomacy.state);
    ReadIntArray(state["diplomacy"]["proposal"], g.diplomacy.proposal);
    if (state["diplomacy"].has("allied"))
        ReadIntArray(state["diplomacy"]["allied"], g.diplomacy.allied);
    if (state["diplomacy"].has("proposeAllied"))
        ReadIntArray(state["diplomacy"]["proposeAllied"], g.diplomacy.proposeAllied);
    if (state["diplomacy"].has("truceTurns"))
        ReadIntArray(state["diplomacy"]["truceTurns"], g.diplomacy.truceTurns);
    if (state["diplomacy"].has("goldOffer"))
        ReadIntArray(state["diplomacy"]["goldOffer"], g.diplomacy.goldOffer);
    if (state["diplomacy"].has("warTarget"))
        ReadIntArray(state["diplomacy"]["warTarget"], g.diplomacy.warTarget);
    if (state["diplomacy"].has("trust"))
        ReadIntArray(state["diplomacy"]["trust"], g.diplomacy.trust);
    if (state["diplomacy"].has("alliedTargetCity"))
        ReadIntArray(state["diplomacy"]["alliedTargetCity"], g.diplomacy.alliedTargetCity);
    g.log.clear();
    for (auto& l : state["log"].items()) g.log.push_back(l.str());

    std::map<int, Item*> item_by_index;
    for (auto& saved : state["items"].items()) {
        for (auto& it : g.map->items) {
            if (it.index == saved["index"].integer()) {
                it.name = saved["name"].str();
                it.type = static_cast<int>(saved["type"].integer());
                it.value = static_cast<int>(saved["value"].integer());
                it.status = static_cast<int>(saved["status"].integer());
                it.x = ReadOpt(saved["x"]);
                it.y = ReadOpt(saved["y"]);
                it.planted = saved["planted"].boolean();
                it.standardOf = ReadOpt(saved["standardOf"]);
                item_by_index[it.index] = &it;
            }
        }
    }

    for (auto& saved : state["cities"].items()) {
        City* c = g.map->city(static_cast<int>(saved["index"].integer()));
        if (!c) continue;
        c->ownerIndex = ReadOpt(saved["ownerIndex"]);
        c->producing = ReadOpt(saved["producing"]);
        c->countdown = static_cast<int>(saved["countdown"].integer());
        c->vectorTo = saved.has("vectorTo") ? static_cast<int>(saved["vectorTo"].integer()) : NONE;
        c->razed = saved["razed"].boolean();
        c->defence = static_cast<int>(saved["defence"].integer());
        c->income = static_cast<int>(saved["income"].integer());
        c->slots.clear();
        for (auto& s : saved["slots"].items()) c->slots.push_back(ReadSlot(s));
        if (!saved["claim"].isNull()) c->claim = static_cast<int>(saved["claim"].integer());
        c->razedBy = ReadOpt(saved["razedBy"]);
        c->attacked = static_cast<int>(saved["attacked"].integer());
        if (!saved["name"].str().empty()) c->name = saved["name"].str();
        // ruins belong to nobody; older saves can have them won in a fight
        if (c->razed) c->ownerIndex = NONE;
    }
    scn::refreshCityTiles(*g.map);

    for (auto& saved : state["sites"].items()) {
        int i = static_cast<int>(saved["index"].integer());
        if (i < 0 || i >= static_cast<int>(g.map->sites.size())) continue;
        Site& s = g.map->sites[i];
        s.content = static_cast<int>(saved["content"].integer());
        s.item = ReadOpt(saved["item"]);
        s.guardian = ReadOpt(saved["guardian"]);
        s.allyType = ReadOpt(saved["allyType"]);
        s.rich = saved["rich"].boolean();
        s.revealed = static_cast<int>(saved["revealed"].integer());
        s.searched = saved["searched"].boolean();
        s.band = saved["band"].str();
        s.templeIndex = ReadOpt(saved["templeIndex"]);
    }
    if (state["fightOrder"].isArray()) {
        g.map->fightOrder.clear();
        for (auto& row : state["fightOrder"].items()) {
            std::vector<int> r;
            for (auto& v : row.items()) r.push_back(static_cast<int>(v.integer()));
            g.map->fightOrder.push_back(r);
        }
    }
    g.history.clear();
    auto read_deed = [](const Json& o) {
        return Deed{static_cast<int>(o["side"].integer()),
                    static_cast<DeedType>(o["type"].integer()), static_cast<int>(o["v1"].integer()),
                    static_cast<int>(o["v2"].integer()), o["name"].str()};
    };
    for (auto& r : state["history"].items()) {
        HistoryRecord rec;
        ReadIntArray(r["gold"], rec.gold);
        ReadIntArray(r["score"], rec.score);
        ReadIntArray(r["cities"], rec.cities);
        for (auto& v : r["owners"].items()) rec.owners.push_back(static_cast<int>(v.integer()));
        for (auto& e : r["events"].items()) rec.events.push_back(read_deed(e));
        g.history.push_back(rec);
    }
    g.deeds.clear();
    for (auto& [k, list] : state["deeds"].entries())
        for (auto& e : list.items()) g.deeds[std::stoi(k)].push_back(read_deed(e));
    g.triumphs.clear();
    for (auto& [me, row] : state["triumphs"].entries())
        for (auto& [opp, counts] : row.entries())
            ReadIntArray(counts, g.triumphs[std::stoi(me)][std::stoi(opp)]);
    g.tutorialSeen.clear();
    for (auto& m : state["tutorialSeen"].items()) g.tutorialSeen.insert(m.str());
    g.towers.clear();
    for (auto& k : state["towers"].items()) g.towers.insert(static_cast<int>(k.integer()));
    const Json& signs = state["signs"];
    for (size_t i = 0; i < g.map->signs.size() && i < signs.size(); i++) {
        g.map->signs[i].lines = {signs[i][0].str(), signs[i][1].str()};
    }
    g.explored.clear();
    for (auto& [k, runs] : state["explored"].entries())
        g.explored[std::stoi(k)] =
                UnpackMask(runs, static_cast<size_t>(g.map->width) * g.map->height);

    std::vector<Army*> armies;
    for (auto& saved : state["armies"].items()) {
        Army a;
        a.x = ReadOpt(saved["x"]);
        a.y = ReadOpt(saved["y"]);
        a.owner = ReadOpt(saved["owner"]);
        a.type = static_cast<int>(saved["type"].integer());
        a.name = saved["name"].str();
        a.female = saved["female"].boolean();
        a.medals = static_cast<int>(saved["medals"].integer());
        a.strength = static_cast<int>(saved["strength"].integer());
        a.moves = static_cast<int>(saved["moves"].integer());
        a.maxMoves = static_cast<int>(saved["maxMoves"].integer());
        a.upkeep = static_cast<int>(saved["upkeep"].integer());
        a.homeCity = ReadOpt(saved["homeCity"]);
        a.atSea = saved["atSea"].boolean();
        a.group = static_cast<int>(saved["group"].integer());
        if (saved["target"].isArray())
            a.target = std::make_pair(static_cast<int>(saved["target"][0].integer()),
                                      static_cast<int>(saved["target"][1].integer()));
        a.fortified = saved["fortified"].boolean();
        a.level = static_cast<int>(saved["level"].integer());
        a.experience = static_cast<int>(saved["experience"].integer());
        a.title = saved["title"].str("");
        for (auto& idx : saved["items"].items()) {
            auto it = item_by_index.find(static_cast<int>(idx.integer()));
            if (it != item_by_index.end()) a.items.push_back(it->second);
        }
        for (auto& t : saved["blessings"].items())
            a.blessings.insert(static_cast<int>(t.integer()));
        if (saved["transit"].isObject())
            a.transit = Transit{static_cast<int>(saved["transit"]["turns"].integer()),
                                static_cast<int>(saved["transit"]["dest"].integer())};
        a.returning = saved["returning"].boolean();
        a.aiOrder = static_cast<int>(saved["aiOrder"].integer());
        a.aiDest = saved.has("aiDest") ? static_cast<int>(saved["aiDest"].integer()) : NONE;
        a.aiGroup = static_cast<int>(saved["aiGroup"].integer());
        a.aiExplore = saved["aiExplore"].boolean();
        a.aiNeutral = saved["aiNeutral"].boolean();
        a.aiParty = saved["aiParty"].boolean();
        // the army table in slot order; older saves had none, and their order
        a.slot = saved.has("slot") && !saved["slot"].isNull()
                         ? static_cast<int>(saved["slot"].integer())
                         : static_cast<int>(armies.size());
        armies.push_back(g.make(a));
    }
    g.armies = armies;
    std::stable_sort(g.armies.begin(), g.armies.end(),
                     [](Army* p, Army* q) { return p->slot < q->slot; });

    for (auto& saved : state["sides"].items()) {
        Side* s = g.map->side(static_cast<int>(saved["index"].integer()));
        if (!s) continue;
        s->gold = static_cast<int>(saved["gold"].integer());
        s->alive = saved["alive"].boolean();
        s->computer = saved["computer"].boolean();
        s->level = static_cast<int>(saved["level"].integer());
        s->enhanced = saved["enhanced"].boolean();
        s->diploScore = static_cast<int>(saved["diploScore"].integer());
        s->observe = saved["observe"].boolean();
        if (saved["capNow"].isObject()) {
            s->capNowX = static_cast<int>(saved["capNow"]["x"].integer());
            s->capNowY = static_cast<int>(saved["capNow"]["y"].integer());
        }
        s->productionDue = saved["productionDue"].boolean();
        s->income = static_cast<int>(saved["income"].integer());
        s->upkeepTotal = static_cast<int>(saved["upkeepTotal"].integer());
        s->aiSolidarity = static_cast<int>(saved["aiSolidarity"].integer());
        s->card = static_cast<int>(saved["card"].integer());
        s->classicAi = saved.has("classicAi") ? saved["classicAi"].boolean(true) : true;
        ai::belief::ensure(*s);
        s->advisor.dir = static_cast<int>(saved["advisor"]["dir"].integer());
        s->advisor.mark = static_cast<int>(saved["advisor"]["mark"].integer());
        if (saved["ai"].isObject()) {
            s->ai = std::make_shared<AIData>(ai::core::newData());
            LoadAi(*s->ai, saved["ai"], armies);
        }
        s->quest.reset();
        if (saved["quest"].isObject()) {
            const Json& qs = saved["quest"];
            auto q = std::make_shared<Quest>();
            q->type = static_cast<int>(qs["type"].integer());
            q->done = static_cast<int>(qs["done"].integer());
            q->required = ReadOpt(qs["required"]);
            long hero = qs["hero"].integer(-1);
            q->hero = hero >= 0 && hero < static_cast<long>(armies.size()) ? armies[hero] : nullptr;
            q->targetKind = qs["targetKind"].str();
            int ref = static_cast<int>(qs["target"].integer(-1));
            if (q->targetKind == "city")
                q->city = g.map->city(ref);
            else if (q->targetKind == "side")
                q->side = g.map->side(ref);
            else if (q->targetKind == "item")
                q->item = item_by_index.count(ref) ? item_by_index[ref] : nullptr;
            else if (q->targetKind == "armytype")
                q->armyType = g.types.byId(ref);
            else if (q->targetKind == "army")
                q->army = ref >= 0 && ref < static_cast<int>(armies.size()) ? armies[ref] : nullptr;
            if (q->hero && q->hasTarget()) s->quest = q;
        }
    }

    // a flier is never at sea, nor a hero flying with one (1a8b:04c8): the
    // Lua puts right any save that says otherwise, and so does this
    std::set<int> fliers, walkers;
    for (Army* a : g.armies) {
        if (a->x != NONE) {
            int k = a->y * g.map->width + a->x;
            if (g.types.byId(a->type)->flies)
                fliers.insert(k);
            else if (a->type != armytype::HERO)
                walkers.insert(k);
        }
    }
    for (Army* a : g.armies) {
        if (a->atSea && a->x != NONE) {
            int k = a->y * g.map->width + a->x;
            if (g.types.byId(a->type)->flies ||
                (a->type == armytype::HERO && fliers.count(k) && !walkers.count(k))) {
                a->atSea = false;
            }
        }
    }

    g.side = g.current < static_cast<int>(g.sides.size()) ? g.sides[g.current] : nullptr;
    // where the side to play stands in the turn order; older saves: the order
    // as it is
    if (state.has("orderAt") && !state["orderAt"].isNull()) {
        g.orderAt = static_cast<int>(state["orderAt"].integer());
    } else {
        int me = g.side ? g.side->index : 0;
        g.orderAt = -1;
        for (int i = 0; i < 8; i++)
            if (g.turnOrder[i] == me) {
                g.orderAt = i;
                break;
            }
    }
    move::invalidate(g);
    return gp;
}

}  // namespace w2::save
