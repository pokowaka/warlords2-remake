#include "warlords/site.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>

#include "warlords/armytype.hpp"
#include "warlords/combat.hpp"
#include "warlords/game.hpp"
#include "warlords/hero.hpp"
#include "warlords/history.hpp"
#include "warlords/move.hpp"
#include "warlords/rules.hpp"
#include "warlords/scn.hpp"

namespace w2::site {

const char* const kContentNames[6] = {"empty",  "temple", "a magic item",
                                      "a sage", "gold",   "allies"};

// The content tables, by band. 3 = sage, 4 = gold, 5 = allies.
static const std::vector<int>& Content(const std::string& band) {
    static const std::vector<int> kRich = {5, 5, 4}, kFar = {3, 4, 5, 3, 4}, kNear = {3, 4, 5};
    return band == "rich" ? kRich : band == "near" ? kNear : kFar;
}
// Ally army types, by band (army type ids).
static const std::vector<int>& AllyTypes(const std::string& band) {
    static const std::vector<int> kRich = {25, 23, 27, 19},  // Dragons, Wizards, Devils, Archons
            kFar = {24, 20, 26},                             // Ghosts, Giant Worms, Demons
            kNear = {22, 20};                                // Elementals, Giant Worms
    return band == "rich" ? kRich : band == "near" ? kNear : kFar;
}

bool ItemReserved(int type, int value) {
    return type == rules::kItemFlight || type == rules::kItemDoubleMove ||
           type == rules::kItemStandard || (type == rules::kItemCommand && value >= 2);
}

void FillItemPool(Game& g, int reserved) {
    if (!g.map->item_pool) return;
    const auto& pool = *g.map->item_pool;
    std::map<int, Item*> by_index;
    std::map<int, bool> used;
    for (auto& it : g.map->items) by_index[it.index] = &it;
    for (int idx = 8; idx <= 21; idx++) {
        auto f = by_index.find(idx);
        if (f == by_index.end()) continue;
        Item* item = f->second;
        bool want = idx < 8 + reserved;
        std::vector<int> choices;
        for (size_t i = 0; i < pool.size(); i++) {
            if (!used[static_cast<int>(i)] && ItemReserved(pool[i]) == want)
                choices.push_back(static_cast<int>(i));
        }
        int k = g.rng.PickIndex(choices.size());
        if (k >= 0) {
            int pick = choices[k];
            used[pick] = true;
            item->name = pool[pick].name;
            item->type = pool[pick].type;
            item->value = pool[pick].value;
        }
    }
}

void MarkRich(Game& g) {
    std::vector<Site*> ruins;
    for (auto& s : g.map->sites) {
        s.rich = false;
        s.revealed = 0xff;
        if (s.type != kTemple) ruins.push_back(&s);
    }
    int want = static_cast<int>(g.map->sites.size()) * kRichShare / 10;
    g.rng.Shuffle(ruins);
    for (int i = 0; i < std::min(want, static_cast<int>(ruins.size())); i++) {
        ruins[i]->rich = true;
        if (g.map->options.quests != 0) ruins[i]->revealed = 0;
    }
}

// rich, near (an in-play side's capital less than 15 away by map_distance,
// 1a8b:0acc) or far.
static std::string Band(const Site& s, const std::vector<std::pair<int, int>>& capitals) {
    if (s.rich) return "rich";
    for (auto& c : capitals) {
        double dx = s.x - c.first, dy = s.y - c.second;
        if (static_cast<int>(std::floor(std::sqrt(dx * dx + dy * dy))) < kCapitalRange)
            return "near";
    }
    return "far";
}

void Setup(Game& g) {
    // the in-play sides' capitals where they stand (+0x189), city or not
    std::vector<std::pair<int, int>> capitals;
    for (Side* s : g.sides) capitals.emplace_back(s->cap_x, s->cap_y);

    MarkRich(g);
    int temples = 0;
    for (auto& s : g.map->sites) {
        s.content = s.type == kTemple ? kTemple : kEmpty;
        if (s.content == kTemple) {
            s.temple_index = temples;
            temples++;
        }
        s.item = kNone;
        s.guardian = kNone;
        s.ally_type = kNone;
        s.searched = false;
        s.band = Band(s, capitals);
    }

    int n_sites = static_cast<int>(g.map->sites.size());
    int reserved = n_sites * 2 / 10;
    FillItemPool(g, reserved);

    int band_hi = reserved;
    int band_lo = std::min(g.rng.Dice(2, 3, 1), band_hi);
    int last = std::min(22, n_sites / 3 + g.rng.Dice(1, 5, -3) + 8);

    std::vector<Site*> free;
    for (auto& s : g.map->sites)
        if (s.content == kEmpty) free.push_back(&s);
    g.rng.Shuffle(free);

    std::map<int, Item*> by_index;
    for (auto& it : g.map->items) {
        by_index[it.index] = &it;
        it.status = 0;
    }

    for (int idx = 8; idx <= last - 1; idx++) {
        auto f = by_index.find(idx);
        // the band between bandLo and bandHi is held back, probably for quests
        if (f != by_index.end() && !(idx >= 8 + band_lo && idx < 8 + band_hi)) {
            Item* item = f->second;
            bool want = ItemReserved(*item);
            for (size_t i = 0; i < free.size(); i++) {
                Site* s = free[i];
                if (s->rich == want) {
                    s->content = kItem;
                    s->item = idx;
                    item->status = 2;  // hidden in a ruin
                    free.erase(free.begin() + i);
                    break;
                }
            }
        }
    }

    for (auto& s : g.map->sites) {
        if (s.content == kEmpty) s.content = g.rng.Pick(Content(s.band), 0);
        if (s.content == kTemple) {
            s.guardian = kNone;
        } else if (s.content == kAllies) {
            // the allies' type takes the guardian's place (66d4:0000)
            s.ally_type = g.rng.Pick(AllyTypes(s.band), 0);
            s.guardian = kNone;
        } else {
            s.guardian = g.rng.Dice(1, 9, 0);
        }
    }
    g.map->site_at.assign(static_cast<size_t>(g.map->width) * g.map->height, nullptr);
    for (auto& s : g.map->sites) g.map->site_at[s.y * g.map->width + s.x] = &s;
}

static Army* HeroIn(const std::vector<Army*>& stack) {
    for (Army* a : stack)
        if (a->type == armytype::kHero) return a;
    return nullptr;
}

bool SurvivesGuardian(Game& g, const Army& h, int monster_strength) {
    int margin = 90 + 5 * (h.strength + combat::BattleItems(h) - monster_strength) +
                 3 * static_cast<int>(game::ArmiesAt(g, h.x, h.y).size());
    return g.rng.Dice(1, 100, 0) <= margin;
}

int GuardianStrength(const Game& g, const Site& s) {
    if (s.guardian < 0 || s.guardian > 9) return 0;
    const auto& m = g.map->monsters[s.guardian];
    return m ? m->strength : 0;
}

int Bless(Game& g, const Site& s, const std::vector<Army*>& stack) {
    if ((s.temple_index == kNone ? 0 : s.temple_index) >= kBlessingTemples) return 0;
    int bit = s.temple_index;
    int blessed = 0;
    for (Army* a : stack) {
        if (!a->blessings.count(bit)) {
            a->blessings.insert(bit);
            a->strength = std::min(9, a->strength + 1);
            blessed++;
            if (a->type == armytype::kHero) hero::AddExperience(g, a, 1);
        }
    }
    return blessed;
}

static const Monster* MonsterOf(const Game& g, int i) {
    if (i < 0 || i > 9 || !g.map->monsters[i]) return nullptr;
    return &*g.map->monsters[i];
}

std::shared_ptr<SearchResult> Search(Game& g, const std::vector<Army*>& stack, int x, int y,
                                     bool human) {
    if (g.map->site_at.empty() || x < 0 || y < 0) return nullptr;
    Site* s = g.map->site_at[y * g.map->width + x];
    if (!s || s->searched) return nullptr;
    if (stack.empty() || stack[0]->moves < 1) return nullptr;
    Army* h = HeroIn(stack);
    auto out = std::make_shared<SearchResult>();
    out->site = s;

    if (s->content == kTemple) {
        out->kind = "temple";
        out->hero = h;
        if (human && h) return out;
        out->blessed = Bless(g, *s, stack);
        return out;
    }

    if (!h) {
        out->kind = "no hero";
        return out;
    }
    Side* side = g.map->Side(h->owner);
    out->hero = h;

    if (s->content == kSage) {
        hero::AddExperience(g, h, 3);
        history::RecordDeed(g, side, DeedType::kFound, history::kFoundSage, 0,
                            h->name);  // 6536:013c
        s->searched = true;
        out->kind = "sage";
        return out;
    }

    // ruin_search (6536:01ab)
    hero::AddExperience(g, h, 3);

    // an allies site holds its allies' type where the guardian would be, and
    // nothing guards it
    const Monster* beaten = nullptr;
    if (s->content != kAllies && s->guardian != kNone && s->guardian > 0) {
        int str = GuardianStrength(g, *s);
        const Monster* monster = MonsterOf(g, s->guardian);
        if (!SurvivesGuardian(g, *h, str)) {
            history::RecordDeed(g, side, DeedType::kHeroKilled, history::kKilledSearching, 0,
                                h->name);        // 6536:02ef
            history::Tally(g, h, kNone, false);  // the side's own losses
            hero::DropItems(g, h, h->x, h->y, h->owner);
            g.Remove(h);
            out->kind = "killed";
            out->monster = monster;
            return out;
        }
        beaten = monster;
        s->guardian = 0;
    }
    out->guardian = beaten;

    if (s->content == kItem) {
        Item* found = nullptr;
        for (auto& it : g.map->items)
            if (it.index == s->item) found = &it;
        if (found && human) {
            found->status = 1;
            found->x = x;
            found->y = y;  // on the ground, to take
        } else if (found) {
            found->status = 3;  // carried
            h->items.push_back(found);
        }
        if (found)
            history::RecordDeed(g, side, DeedType::kFound, found->index, 0, h->name);  // 6536:0571
        s->searched = true;
        s->content = kEmpty;
        out->kind = "item";
        out->item = found;
        return out;
    } else if (s->content == kGold) {
        int gold = s->rich ? g.rng.Dice(3, 1000, 1000) : g.rng.Dice(3, 500, 500);
        if (side) side->gold += gold;
        s->searched = true;
        history::RecordDeed(g, side, DeedType::kFound, history::kFoundGold, 0, h->name);
        out->kind = "gold";
        out->gold = gold;
        return out;
    } else if (s->content == kAllies) {
        s->searched = true;
        const ArmyType* type = g.types.ById(s->ally_type);
        if (!type) type = g.types.ById(armytype::kScouts);
        int n = g.rng.Dice(1, 2, 0);
        if (s->rich) n += 2;
        for (int k = 0; k < n; k++) {
            Army* a = hero::PlaceAlly(g, type, x, y, *side);
            if (a) out->armies.push_back(a);
        }
        history::RecordDeed(g, side, DeedType::kFound, history::kFoundAllies, 0,
                            h->name);  // 6536:07a1
        out->kind = "allies";
        out->type = type;
        out->count = n;
        return out;
    }
    out->kind = "empty";
    return out;
}

static int SageDistance(const Site& s, int hx, int hy) {
    double dx = s.x - hx, dy = s.y - hy;
    return static_cast<int>(std::floor(std::sqrt(dx * dx + dy * dy)));  // 2012:1199
}

bool ShownTo(const Site& s, const Side& side) {
    return ((s.revealed >> side.index) & 1) == 1;
}

static bool Unshown(const Side& side, const Site& s, int hx, int hy) {
    return s.rich && !s.searched && !ShownTo(s, side) && SageDistance(s, hx, hy) < kSageRange;
}

std::vector<SageEntry> SageList(Game& g, const Side& side, int hx, int hy) {
    std::vector<SageEntry> list;
    bool gold = false, allies = false;
    for (auto& s : g.map->sites) {
        if (Unshown(side, s, hx, hy)) {
            if (s.content == kGold && !gold) {
                gold = true;
                list.push_back(SageEntry{"gold", "Gold", nullptr});
            } else if (s.content == kAllies && !allies) {
                allies = true;
                list.push_back(SageEntry{"allies", "Allies", nullptr});
            } else if (s.content == kItem) {
                for (auto& it : g.map->items) {
                    if (it.index == s.item) list.push_back(SageEntry{"item", it.name, &it});
                }
            }
        }
    }
    return list;
}

Site* SageShow(Game& g, const Side& side, const SageEntry& entry, int hx, int hy) {
    Site* found = nullptr;
    int best = kNone;
    for (auto& s : g.map->sites) {
        if (entry.kind == "item") {
            if (s.content == kItem && entry.item && s.item == entry.item->index) {
                found = &s;
                break;
            }
        } else if (Unshown(side, s, hx, hy) &&
                   s.content == (entry.kind == "gold" ? kGold : kAllies)) {
            int d = SageDistance(s, hx, hy);
            if (best == kNone || d < best) {
                found = &s;
                best = d;
            }
        }
    }
    if (!found) return nullptr;
    if (!ShownTo(*found, side)) found->revealed = found->revealed + (1 << side.index);
    game::Reveal(g, side.index, found->x, found->y, false);
    return found;
}

int SageGem(Game& g, Side& side) {
    int n = g.rng.Dice(3, 500, 500);
    side.gold += n;
    return n;
}

std::array<int, 4> SageMap(Game& g, const Side& side, int cx, int cy) {
    int x0 = std::max(0, cx - g.rng.Dice(1, 5, 8));
    int y0 = std::max(0, cy - g.rng.Dice(1, 5, 8));
    int w = g.rng.Dice(1, 10, 15), h = g.rng.Dice(1, 10, 15);
    if (x0 + w >= g.map->width) w = g.map->width - x0 - 1;
    if (y0 + h >= g.map->height) h = g.map->height - y0 - 1;
    for (int x = x0; x < x0 + w; x++)
        for (int y = y0; y < y0 + h; y++) game::Reveal(g, side.index, x, y, false);
    return {x0, y0, w, h};
}

}  // namespace w2::site
