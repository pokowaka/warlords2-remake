#include "warlords/quest.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <variant>

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

namespace {
/** @brief Straight-line distance between two tiles, rounded down. */
int StraightLineDistance(int from_x, int from_y, int to_x, int to_y) {
    double dx = from_x - to_x;
    double dy = from_y - to_y;
    return static_cast<int>(std::floor(std::sqrt(dx * dx + dy * dy)));
}

/**
 * @brief Picks a target for a quest of `type` (part of the original's quest_assign).
 * @param computer For a computer player: shorter ranges.
 * @param flying Whether the searching stack flies (uncovers more round a retrieve-item site).
 * @return The chosen `Quest::Target`, or `std::nullopt` when no eligible target exists.
 */
std::optional<Quest::Target> PickTarget(Game& game, const Side& side, int type, const Army* hero,
                                        bool computer, bool flying) {
    if (type == kSlayHero) {
        // another side's hero, counted from the top of the army table down
        std::vector<Army*> heroes;
        for (int i = static_cast<int>(game.armies.size()) - 1; i >= 0; i--) {
            Army* army = game.armies[i];
            if (army->type == armytype::kHero && army->owner != side.index) heroes.push_back(army);
        }
        if (heroes.empty()) return std::nullopt;
        Army* chosen = heroes[game.rng.Dice(1, static_cast<int>(heroes.size()), -1)];
        return Quest::Target{chosen->id};
    } else if (type == kRetrieveItem) {
        // an ordinary item still hidden in a ruin, less than 50 (30) away each way
        int range = computer ? kAiItemRange : kItemRange;
        std::vector<std::pair<Item*, Site*>> choices;
        for (auto& item : game.map->items) {
            if (item.index <= 21 && item.status == 2 && !site::ItemReserved(item)) {
                Site* ruin = nullptr;
                for (auto& candidate : game.map->sites) {
                    if (candidate.content == site::kItem && candidate.item == item.index) {
                        ruin = &candidate;
                        break;
                    }
                }
                if (ruin && std::abs(hero->x - ruin->x) < range &&
                    std::abs(hero->y - ruin->y) < range)
                    choices.emplace_back(&item, ruin);
            }
        }
        if (choices.empty()) return std::nullopt;
        auto& pick = choices[game.rng.Dice(1, static_cast<int>(choices.size()), -1)];
        // the priests show the side where it lies, uncovering round it as the
        // original does: two tiles out when the searching stack flies
        pick.second->revealed |= 1 << side.index;
        game::Reveal(game, side.index, pick.second->x, pick.second->y, flying);
        return Quest::Target{ItemId(pick.first->index)};
    } else if (type == kSlayType) {
        // up to five magical types, one of which some other army on the map is
        for (int attempt = 0; attempt < 5; attempt++) {
            const ArmyType* want = hero::MagicalType(game);
            for (int i = static_cast<int>(game.armies.size()) - 1; i >= 0; i--) {
                Army* army = game.armies[i];
                if (army->type == want->id && army->owner != side.index && !army->transit &&
                    army->x != kNone) {
                    return Quest::Target{ArmyTypeId(want->id)};
                }
            }
        }
        return std::nullopt;
    } else if (type == kSlaughter) {
        if (game.won) return std::nullopt;
        for (int attempt = 0; attempt < 200; attempt++) {
            Side* other = game.map->Side(game.rng.Dice(1, 8, -1));
            if (other && other->index != side.index && other->in_use && other->alive) {
                return Quest::Target{SideId(other->index)};
            }
        }
        return std::nullopt;
    } else if (type == kOccupy || type == kRaze) {
        // any standing city not the side's, near the hero -- after a hundred
        // tries, anywhere
        if (game.won) return std::nullopt;
        int range = type == kRaze ? kRazeRange : (computer ? kAiCityRange : kCityRange);
        for (int attempt = 0; attempt < 200; attempt++) {
            City* city =
                    game.map->City(game.rng.Dice(1, static_cast<int>(game.map->cities.size()), -1));
            if (city && city->owner_index != side.index && !city->razed &&
                (StraightLineDistance(hero->x, hero->y, city->x, city->y) <= range ||
                 attempt > 99)) {
                return Quest::Target{CityId(city->index)};
            }
        }
        return std::nullopt;
    } else if (type == kPillageGold) {
        return Quest::Target{std::monostate{}};  // no target entity, just a count
    }
    return std::nullopt;
}

/**
 * @brief The type of a quest's `attempt`th try (from 1).
 *
 * A human's is rolled from the table; a computer's is raze or occupy first,
 * then slaughter, then pillage gold.
 */
int QuestType(Game& game, int attempt, bool computer) {
    if (!computer) return kTypeTable[game.rng.Dice(1, 10, -1)];
    if (attempt == 1) return game.rng.Dice(1, 5, -1) == 0 ? kRaze : kOccupy;
    if (attempt == 2) return kSlaughter;
    return kPillageGold;
}

/** @brief Whether the quest's hero is present in `stack`. */
bool HeroInStack(const Quest& quest, const std::vector<Army*>& stack) {
    if (!quest.HeroId().IsValid()) return false;
    return std::any_of(stack.begin(), stack.end(), [&quest](const Army* member) {
        return member && member->id == quest.HeroId();
    });
}

/**
 * @brief Ends the side's quest.
 *
 * A failure carries the STRING.DAT group quest_check tells a human it with;
 * a human side keeps the outcome as `quest_news`.
 * @param reason "done", or why it failed.
 * @return The outcome, or nullptr when the side had no quest.
 */
std::shared_ptr<QuestResult> Finish(Game& game, Side& side, const std::string& reason,
                                    int why = 0) {
    auto quest = side.quest;
    side.quest.reset();
    if (!quest) return nullptr;
    auto out = std::make_shared<QuestResult>();
    out->quest = quest;
    if (reason == "done") {
        Army* hero = quest->ResolveHero(game);
        history::RecordDeed(game, &side, DeedType::kQuestCompleted, 0, 0, hero ? hero->name : "");
        if (hero) hero::AddExperience(game, hero, kExperience);
        out->reward = ChooseReward(game, side, quest.get());
    } else {
        out->failed = reason;
        out->why = why;
    }
    if (!side.computer) side.quest_news = out;
    return out;
}
}  // namespace

std::shared_ptr<Quest> Assign(Game& game, Side& side, Army* hero, bool computer, bool flying) {
    if (side.quest) return nullptr;  // one at a time
    if (game.map->options.quests == 0) return nullptr;
    history::RecordDeed(game, &side, DeedType::kQuestReceived, 0, 0, hero ? hero->name : "");
    for (int attempt = 1;; attempt++) {
        int type = QuestType(game, attempt, computer);
        if (auto target = PickTarget(game, side, type, hero, computer, flying)) {
            int required = kNone;
            if (type == kSlaughter)
                required = game.rng.Dice(1, 12, 10);
            else if (type == kPillageGold)
                required = game.rng.Dice(3, 300, 500);
            ArmyId hero_id = hero ? hero->id : ArmyId::None();
            side.quest = std::make_shared<Quest>(type, hero_id, *target, required, 0);
            return side.quest;
        }
    }
}

std::string Describe(const Game& game, const Quest& quest) {
    const char* text = kDescriptions[quest.Type()];
    if (quest.Required() != kNone) return Format(text, quest.Required());
    if (quest.Type() == kOccupy || quest.Type() == kRaze) {
        const City* city = quest.ResolveCity(game);
        return std::string(text) + ": " + (city ? city->name : "");
    }
    if (quest.Type() == kSlayType) {
        const ArmyType* army_type = quest.ResolveArmyType(game);
        return std::string(text) + ": " + (army_type ? army_type->name : "");
    }
    return text;
}

std::shared_ptr<QuestResult> Notify(Game& game, Side& side, const std::string& event,
                                    const Event& data) {
    auto active = side.quest;
    if (!active) return nullptr;
    Quest& quest = *active;
    int type = quest.Type();

    if (event == "turn") {
        Army* hero = game.armies.FindAlive(quest.HeroId());
        bool alive = hero != nullptr && hero->owner == side.index;
        if (!alive) return Finish(game, side, "the hero is lost", 0x20);
        if (type == kOccupy || type == kRaze) {
            const City* city = quest.ResolveCity(game);
            if (!city || city->razed) return Finish(game, side, "the city is ruins", 0x21);
            if (city->owner_index == side.index)
                return Finish(game, side, "another took the city", 0x2a);
        } else if (type == kSlaughter) {
            const Side* target_side = quest.ResolveSide(game);
            if (!target_side || !target_side->alive)
                return Finish(game, side, "that side is gone", 0x27);
        } else if (type == kSlayHero) {
            if (!game.armies.Alive(quest.TargetArmyId()))
                return Finish(game, side, "the quarry is gone", 0x29);
        } else if (type == kRetrieveItem) {
            const Item* item = quest.ResolveItem(game);
            if (!item || item->status == 0) return Finish(game, side, "the item is lost", 0x28);
        }
        return nullptr;
    }

    if (event == "battle") {
        if (!HeroInStack(quest, data.stack)) return nullptr;
        if (type == kSlayHero) {
            ArmyId target_army = quest.TargetArmyId();
            for (Army* victim : data.killed)
                if (victim && target_army.IsValid() && victim->id == target_army)
                    return Finish(game, side, "done");
        } else if (type == kSlayType) {
            int want_type = quest.TargetArmyTypeId().Value();
            for (Army* victim : data.killed)
                if (victim && victim->type == want_type) return Finish(game, side, "done");
        } else if (type == kSlaughter) {
            int target_owner = quest.TargetSideId().Value();
            for (Army* victim : data.killed)
                if (victim && victim->owner == target_owner) quest.AddProgress(1);
            if (quest.Done() >= quest.Required()) return Finish(game, side, "done");
        }
    } else if (event == "item") {
        Army* hero = quest.ResolveHero(game);
        ItemId want_item = quest.TargetItemId();
        if (type == kRetrieveItem && hero && want_item.IsValid()) {
            auto& items = hero->items;
            auto carried = std::find_if(items.begin(), items.end(), [want_item](const Item* item) {
                return item && item->index == want_item.Value();
            });
            if (carried != items.end()) {
                Item* item = *carried;
                items.erase(carried);  // the priests take it away
                item->status = 0;
                return Finish(game, side, "done");
            }
        }
    } else if (event == "pillage") {
        CityId target_city = quest.TargetCityId();
        if (type == kPillageGold && HeroInStack(quest, data.stack)) {
            quest.AddProgress(data.gold);
            if (quest.Done() >= quest.Required()) return Finish(game, side, "done");
        } else if ((type == kOccupy || type == kRaze) && data.city &&
                   CityId(data.city->index) == target_city) {
            return Finish(game, side, "that was not to pillage", 0x24);
        }
    } else if (event == "occupy") {
        CityId target_city = quest.TargetCityId();
        if (type == kOccupy && data.city && CityId(data.city->index) == target_city) {
            if (HeroInStack(quest, data.stack)) return Finish(game, side, "done");
            return Finish(game, side, "the hero was not there", 0x22);
        } else if (type == kRaze && data.city && CityId(data.city->index) == target_city) {
            return Finish(game, side, "the quest was to raze it", 0x23);
        }
    } else if (event == "raze") {
        CityId target_city = quest.TargetCityId();
        if (type == kRaze && data.city && CityId(data.city->index) == target_city) {
            if (HeroInStack(quest, data.stack)) return Finish(game, side, "done");
            return Finish(game, side, "the hero was not there", 0x25);
        } else if (type == kOccupy && data.city && CityId(data.city->index) == target_city) {
            return Finish(game, side, "the quest was to keep it", 0x26);
        }
    }
    return nullptr;
}

std::shared_ptr<Reward> ChooseReward(Game& game, Side& side, const Quest* quest) {
    int cities = static_cast<int>(game::SideCities(game, side).size());

    Army* hero = quest ? quest->ResolveHero(game) : nullptr;

    // Archons, on the quest hero's tile (the original's create_ally_army)
    auto give_allies = [&](int count) {
        const ArmyType* type = game.types.ById(kRewardAlly);
        auto reward = std::make_shared<Reward>();
        reward->kind = "allies";
        reward->type = type;
        reward->count = count;
        for (int i = 0; i < count; i++) {
            Army* ally = hero ? hero::PlaceAlly(game, type, hero->x, hero->y, side) : nullptr;
            if (ally) reward->armies.push_back(ally);
        }
        return reward;
    };

    auto give_gold = [&]() {
        int amount = game.rng.Dice(2, 1000, 1000);
        side.gold += amount;
        auto reward = std::make_shared<Reward>();
        reward->kind = "gold";
        reward->gold = amount;
        return reward;
    };

    if (cities < 10 && game.turn > 15) return give_allies(game.rng.Dice(1, 3, 5));
    if (side.gold < 100) return give_gold();

    // an unused reserved item -- of the standards only the side's own
    std::vector<Item*> unused;
    for (auto& item : game.map->items) {
        if (item.index <= 21 && item.status == 0 && site::ItemReserved(item) &&
            (item.index > 7 || item.index == side.index)) {
            unused.push_back(&item);
        }
    }
    // a rich site the side does not know of yet, not emptied
    std::vector<Site*> unknown;
    for (auto& candidate : game.map->sites) {
        if (candidate.rich && !site::ShownTo(candidate, side) && candidate.content != site::kEmpty)
            unknown.push_back(&candidate);
    }
    if (!unused.empty() && game.rng.Dice(1, 3, 0) == 1) {
        Item* item = unused[game.rng.Dice(1, static_cast<int>(unused.size()), -1)];
        if (hero) {
            hero->items.push_back(item);
            item->status = 3;
        }
        auto reward = std::make_shared<Reward>();
        reward->kind = "item";
        reward->item = item;
        return reward;
    }
    if (unused.empty() && !unknown.empty() && game.rng.Dice(1, 3, 0) < 3) {
        // the nearest to the hero, the first of equals
        Site* best = nullptr;
        int best_distance = 10000;
        for (Site* candidate : unknown) {
            int distance = hero ? std::max(std::abs(candidate->x - hero->x),
                                           std::abs(candidate->y - hero->y))
                                : 0;
            if (distance < best_distance) {
                best = candidate;
                best_distance = distance;
            }
        }
        best->revealed |= 1 << side.index;
        if (!side.computer || side.observe) game::Reveal(game, side.index, best->x, best->y, false);
        auto reward = std::make_shared<Reward>();
        reward->kind = "revealed";
        reward->site = best;
        return reward;
    }
    if (game.rng.Dice(1, 2, 0) == 1) return give_allies(game.rng.Dice(1, 3, 2));
    return give_gold();
}

}  // namespace w2::quest
