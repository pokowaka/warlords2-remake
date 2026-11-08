#include "warlords/quest.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "util/util.hpp"
#include "warlords/armytype.hpp"
#include "warlords/game.hpp"
#include "warlords/hero.hpp"
#include "warlords/history.hpp"
#include "warlords/site.hpp"

namespace w2::quest {

const std::vector<int> kTypeTable = {0, 1, 2, 3, 4, 5, 6, 4, 5, 6};
const char* const kDescriptions[7] = {
    "slay the enemy hero",
    "retrieve a magic item",
    "slay a unit of an enemy army type",
    "slaughter %d armies of one side",
    "force a city into submission and occupy it",
    "conquer a city and raze it",
    "sack and pillage %d gold",
};
const char* const kTargetKind[7] = {"army", "item", "armytype", "side", "city", "city", "none"};

namespace {
int Dist(int x1, int y1, int x2, int y2) {
    double dx = x1 - x2, dy = y1 - y2;
    return static_cast<int>(std::floor(std::sqrt(dx * dx + dy * dy)));
}

// Pick a target for a quest of this type into q (quest_assign, 4976:0d7a);
// false if there is none. `computer` is quest_assign(1): shorter ranges.
bool PickTarget(Game& g, const Side& side, int type, Army* h, Quest& q, bool computer,
                bool flying) {
    if (type == kSlayHero) {
        // another side's hero, counted from the top of the army table down
        std::vector<Army*> heroes;
        for (int i = static_cast<int>(g.armies.size()) - 1; i >= 0; i--) {
            Army* a = g.armies[i];
            if (a->type == armytype::kHero && a->owner != side.index) heroes.push_back(a);
        }
        if (heroes.empty()) return false;
        q.army = heroes[g.rng.Dice(1, static_cast<int>(heroes.size()), -1)];
        return true;
    } else if (type == kRetrieveItem) {
        // an ordinary item still hidden in a ruin, less than 50 (30) away each way
        int range = computer ? kAiItemRange : kItemRange;
        std::vector<std::pair<Item*, Site*>> choices;
        for (auto& it : g.map->items) {
            if (it.index <= 21 && it.status == 2 && !site::ItemReserved(it)) {
                Site* s = nullptr;
                for (auto& x : g.map->sites) {
                    if (x.content == site::kItem && x.item == it.index) {
                        s = &x;
                        break;
                    }
                }
                if (s && std::abs(h->x - s->x) < range && std::abs(h->y - s->y) < range)
                    choices.emplace_back(&it, s);
            }
        }
        if (choices.empty()) return false;
        auto& pick = choices[g.rng.Dice(1, static_cast<int>(choices.size()), -1)];
        // the priests show the side where it lies, uncovering round it as
        // 8611:1298 does: two tiles out when the searching stack flies (DS:1e9c)
        pick.second->revealed |= 1 << side.index;
        game::Reveal(g, side.index, pick.second->x, pick.second->y, flying);
        q.item = pick.first;
        return true;
    } else if (type == kSlayType) {
        // up to five magical types, one of which some other army on the map is
        for (int t = 0; t < 5; t++) {
            const ArmyType* want = hero::MagicalType(g);
            for (int i = static_cast<int>(g.armies.size()) - 1; i >= 0; i--) {
                Army* a = g.armies[i];
                if (a->type == want->id && a->owner != side.index && !a->transit && a->x != kNone) {
                    q.army_type = want;
                    return true;
                }
            }
        }
        return false;
    } else if (type == kSlaughter) {
        if (g.won) return false;
        for (int t = 0; t < 200; t++) {
            Side* o = g.map->Side(g.rng.Dice(1, 8, -1));
            if (o && o->index != side.index && o->in_use && o->alive) {
                q.side = o;
                return true;
            }
        }
        return false;
    } else if (type == kOccupy || type == kRaze) {
        // any standing city not the side's, near the hero -- after a hundred
        // tries, anywhere
        if (g.won) return false;
        int range = type == kRaze ? kRazeRange : (computer ? kAiCityRange : kCityRange);
        for (int t = 0; t < 200; t++) {
            City* c = g.map->City(g.rng.Dice(1, static_cast<int>(g.map->cities.size()), -1));
            if (c && c->owner_index != side.index && !c->razed &&
                (Dist(h->x, h->y, c->x, c->y) <= range || t > 99)) {
                q.city = c;
                return true;
            }
        }
        return false;
    } else if (type == kPillageGold) {
        return true;  // no target, just a count
    }
    return false;
}

// The type of a quest's `n`th try (from 1). A human's is rolled from the
// table; a computer's (quest_assign(1)) is raze or occupy first, then
// slaughter, then pillage gold.
int QuestType(Game& g, int n, bool computer) {
    if (!computer) return kTypeTable[g.rng.Dice(1, 10, -1)];
    if (n == 1) return g.rng.Dice(1, 5, -1) == 0 ? kRaze : kOccupy;
    if (n == 2) return kSlaughter;
    return kPillageGold;
}

bool HeroInStack(const Quest& q, const std::vector<Army*>& stack) {
    return std::find(stack.begin(), stack.end(), q.hero) != stack.end();
}

// The quest is over. A failure carries the STRING.DAT group quest_check tells
// a human it with; a human side keeps the outcome as `questNews`.
std::shared_ptr<QuestResult> Finish(Game& g, Side& side, const std::string& reason, int why = 0) {
    auto q = side.quest;
    side.quest.reset();
    if (!q) return nullptr;
    auto out = std::make_shared<QuestResult>();
    out->quest = q;
    if (reason == "done") {
        history::RecordDeed(g, &side, DeedType::kQuestCompleted, 0, 0,
                            q->hero ? q->hero->name : "");  // 4976:1da8
        hero::AddExperience(g, q->hero, kExperience);
        out->reward = ChooseReward(g, side, q.get());
    } else {
        out->failed = reason;
        out->why = why;
    }
    if (!side.computer) side.quest_news = out;
    return out;
}
}  // namespace

std::shared_ptr<Quest> Assign(Game& g, Side& side, Army* h, bool computer, bool flying) {
    if (side.quest) return nullptr;  // one at a time
    if (g.map->options.quests == 0) return nullptr;
    history::RecordDeed(g, &side, DeedType::kQuestReceived, 0, 0, h ? h->name : "");  // 4976:0dae
    for (int n = 1;; n++) {
        int type = QuestType(g, n, computer);
        Quest q;
        if (PickTarget(g, side, type, h, q, computer, flying)) {
            q.type = type;
            q.hero = h;
            q.done = 0;
            q.target_kind = kTargetKind[type];
            if (type == kSlaughter)
                q.required = g.rng.Dice(1, 12, 10);
            else if (type == kPillageGold)
                q.required = g.rng.Dice(3, 300, 500);
            side.quest = std::make_shared<Quest>(q);
            return side.quest;
        }
    }
}

std::string Describe(const Quest& q) {
    const char* text = kDescriptions[q.type];
    if (q.required != kNone) return Fmt(text, q.required);
    if (q.type == kOccupy || q.type == kRaze)
        return std::string(text) + ": " + (q.city ? q.city->name : "");
    if (q.type == kSlayType)
        return std::string(text) + ": " + (q.army_type ? q.army_type->name : "");
    return text;
}

std::shared_ptr<QuestResult> Notify(Game& g, Side& side, const std::string& ev, const Event& data) {
    auto qp = side.quest;
    if (!qp) return nullptr;
    Quest& q = *qp;

    if (ev == "turn") {
        bool alive = false;
        for (Army* a : g.armies)
            if (a == q.hero && a->owner == side.index) alive = true;
        if (!alive) return Finish(g, side, "the hero is lost", 0x20);
        if (q.type == kOccupy || q.type == kRaze) {
            if (q.city->razed) return Finish(g, side, "the city is ruins", 0x21);
            if (q.city->owner_index == side.index)
                return Finish(g, side, "another took the city", 0x2a);
        } else if (q.type == kSlaughter) {
            if (!q.side->alive) return Finish(g, side, "that side is gone", 0x27);
        } else if (q.type == kSlayHero) {
            if (!g.Alive(q.army)) return Finish(g, side, "the quarry is gone", 0x29);
        } else if (q.type == kRetrieveItem) {
            if (q.item->status == 0) return Finish(g, side, "the item is lost", 0x28);
        }
        return nullptr;
    }

    if (ev == "battle") {
        if (!HeroInStack(q, data.stack)) return nullptr;
        if (q.type == kSlayHero) {
            for (Army* d : data.killed)
                if (d == q.army) return Finish(g, side, "done");
        } else if (q.type == kSlayType) {
            for (Army* d : data.killed)
                if (d->type == q.army_type->id) return Finish(g, side, "done");
        } else if (q.type == kSlaughter) {
            for (Army* d : data.killed)
                if (d->owner == q.side->index) q.done++;
            if (q.done >= q.required) return Finish(g, side, "done");
        }
    } else if (ev == "item") {
        if (q.type == kRetrieveItem && q.hero) {
            auto& items = q.hero->items;
            auto i = std::find(items.begin(), items.end(), q.item);
            if (i != items.end()) {
                Item* it = *i;
                items.erase(i);  // the priests take it away
                it->status = 0;
                return Finish(g, side, "done");
            }
        }
    } else if (ev == "pillage") {
        if (q.type == kPillageGold && HeroInStack(q, data.stack)) {
            q.done += data.gold;
            if (q.done >= q.required) return Finish(g, side, "done");
        } else if ((q.type == kOccupy || q.type == kRaze) && data.city == q.city) {
            return Finish(g, side, "that was not to pillage", 0x24);
        }
    } else if (ev == "occupy") {
        if (q.type == kOccupy && data.city == q.city) {
            if (HeroInStack(q, data.stack)) return Finish(g, side, "done");
            return Finish(g, side, "the hero was not there", 0x22);
        } else if (q.type == kRaze && data.city == q.city) {
            return Finish(g, side, "the quest was to raze it", 0x23);
        }
    } else if (ev == "raze") {
        if (q.type == kRaze && data.city == q.city) {
            if (HeroInStack(q, data.stack)) return Finish(g, side, "done");
            return Finish(g, side, "the hero was not there", 0x25);
        } else if (q.type == kOccupy && data.city == q.city) {
            return Finish(g, side, "the quest was to keep it", 0x26);
        }
    }
    return nullptr;
}

std::shared_ptr<Reward> ChooseReward(Game& g, Side& side, const Quest* q) {
    int cities = static_cast<int>(game::SideCities(g, side).size());

    Army* h = q ? q->hero : nullptr;

    // Archons, on the quest hero's tile (create_ally_army)
    auto allies = [&](int n) {
        const ArmyType* type = g.types.ById(kRewardAlly);
        auto r = std::make_shared<Reward>();
        r->kind = "allies";
        r->type = type;
        r->count = n;
        for (int i = 0; i < n; i++) {
            Army* a = h ? hero::PlaceAlly(g, type, h->x, h->y, side) : nullptr;
            if (a) r->armies.push_back(a);
        }
        return r;
    };

    auto gold = [&]() {
        int n = g.rng.Dice(2, 1000, 1000);
        side.gold += n;
        auto r = std::make_shared<Reward>();
        r->kind = "gold";
        r->gold = n;
        return r;
    };

    if (cities < 10 && g.turn > 15) return allies(g.rng.Dice(1, 3, 5));
    if (side.gold < 100) return gold();

    // an unused reserved item -- of the standards only the side's own
    std::vector<Item*> unused;
    for (auto& it : g.map->items) {
        if (it.index <= 21 && it.status == 0 && site::ItemReserved(it) &&
            (it.index > 7 || it.index == side.index)) {
            unused.push_back(&it);
        }
    }
    // a rich site the side does not know of yet, not emptied
    std::vector<Site*> unknown;
    for (auto& s : g.map->sites) {
        if (s.rich && !site::ShownTo(s, side) && s.content != site::kEmpty) unknown.push_back(&s);
    }
    if (!unused.empty() && g.rng.Dice(1, 3, 0) == 1) {
        Item* it = unused[g.rng.Dice(1, static_cast<int>(unused.size()), -1)];
        if (h) {
            h->items.push_back(it);
            it->status = 3;
        }
        auto r = std::make_shared<Reward>();
        r->kind = "item";
        r->item = it;
        return r;
    }
    if (unused.empty() && !unknown.empty() && g.rng.Dice(1, 3, 0) < 3) {
        // the nearest to the hero, the first of equals
        Site* best = nullptr;
        int best_d = 10000;
        for (Site* s : unknown) {
            int d = h ? std::max(std::abs(s->x - h->x), std::abs(s->y - h->y)) : 0;
            if (d < best_d) {
                best = s;
                best_d = d;
            }
        }
        best->revealed |= 1 << side.index;
        if (!side.computer || side.observe) game::Reveal(g, side.index, best->x, best->y, false);
        auto r = std::make_shared<Reward>();
        r->kind = "revealed";
        r->site = best;
        return r;
    }
    if (g.rng.Dice(1, 2, 0) == 1) return allies(g.rng.Dice(1, 3, 2));
    return gold();
}

}  // namespace w2::quest
