/**
 * @file
 * @brief Implementation of `QuestSystem` and `w2::quest` wrappers.
 */
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

namespace w2 {

namespace {

int StraightLineDistance(int from_x, int from_y, int to_x, int to_y) {
    double dx = from_x - to_x;
    double dy = from_y - to_y;
    return static_cast<int>(std::floor(std::sqrt(dx * dx + dy * dy)));
}

std::optional<Quest::Target> PickTarget(Game& game, const Side& side, int type, const Army* hero,
                                        bool computer, bool flying) {
    if (type == quest::kSlayHero) {
        std::vector<Army*> heroes;
        for (int i = static_cast<int>(game.armies.size()) - 1; i >= 0; i--) {
            Army* army = game.armies[i];
            if (army->Hero() && !army->OwnedBy(side.Id())) heroes.push_back(army);
        }
        if (heroes.empty()) return std::nullopt;
        Army* chosen = heroes[game.rng.Dice(1, static_cast<int>(heroes.size()), -1)];
        return Quest::Target{chosen->Id()};
    }
    if (type == quest::kRetrieveItem) {
        int range = computer ? quest::kAiItemRange : quest::kItemRange;
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
                if (ruin && std::abs(hero->X() - ruin->x) < range &&
                    std::abs(hero->Y() - ruin->y) < range)
                    choices.emplace_back(&item, ruin);
            }
        }
        if (choices.empty()) return std::nullopt;
        auto& pick = choices[game.rng.Dice(1, static_cast<int>(choices.size()), -1)];
        pick.second->revealed |= 1 << side.Index();
        game::Reveal(game, side.Index(), pick.second->x, pick.second->y, flying);
        return Quest::Target{ItemId(pick.first->index)};
    }
    if (type == quest::kSlayType) {
        for (int attempt = 0; attempt < 5; attempt++) {
            const ArmyType* want = hero::MagicalType(game);
            for (int i = static_cast<int>(game.armies.size()) - 1; i >= 0; i--) {
                Army* army = game.armies[i];
                if (army->TypeIndex() == want->id && !army->OwnedBy(side.Id()) && army->OnMap()) {
                    return Quest::Target{ArmyTypeId(want->id)};
                }
            }
        }
        return std::nullopt;
    }
    if (type == quest::kSlaughter) {
        if (game.won) return std::nullopt;
        for (int attempt = 0; attempt < 200; attempt++) {
            Side* other = game.map->Side(game.rng.Dice(1, 8, -1));
            if (other && other->Id() != side.Id() && other->IsActive()) {
                return Quest::Target{other->Id()};
            }
        }
        return std::nullopt;
    }
    if (type == quest::kOccupy || type == quest::kRaze) {
        if (game.won) return std::nullopt;
        int range = type == quest::kRaze ? quest::kRazeRange
                                         : (computer ? quest::kAiCityRange : quest::kCityRange);
        for (int attempt = 0; attempt < 200; attempt++) {
            City* city =
                    game.map->City(game.rng.Dice(1, static_cast<int>(game.map->cities.size()), -1));
            if (city && !city->OwnedBy(side.Id()) && !city->IsRazed() &&
                (StraightLineDistance(hero->X(), hero->Y(), city->X(), city->Y()) <= range ||
                 attempt > 99)) {
                return Quest::Target{city->Id()};
            }
        }
        return std::nullopt;
    }
    if (type == quest::kPillageGold) {
        return Quest::Target{std::monostate{}};
    }
    return std::nullopt;
}

int QuestType(Game& game, int attempt, bool computer) {
    if (!computer) return quest::kTypeTable[game.rng.Dice(1, 10, -1)];
    if (attempt == 1) return game.rng.Dice(1, 5, -1) == 0 ? quest::kRaze : quest::kOccupy;
    if (attempt == 2) return quest::kSlaughter;
    return quest::kPillageGold;
}

bool HeroInStack(const Quest& active_quest, const std::vector<Army*>& stack) {
    if (!active_quest.HeroId().IsValid()) return false;
    return std::any_of(stack.begin(), stack.end(), [&active_quest](const Army* member) {
        return member && member->Id() == active_quest.HeroId();
    });
}

}  // namespace

namespace quest {

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

std::shared_ptr<Quest> Assign(Game& game, Side& side, Army* hero, bool computer, bool flying) {
    return QuestSystem(game).Assign(side, hero, computer, flying);
}

std::string Describe(const Game& game, const Quest& active_quest) {
    return QuestSystem::Describe(game, active_quest);
}

std::shared_ptr<QuestResult> Notify(Game& game, Side& side, const std::string& event,
                                    const Event& data) {
    return QuestSystem(game).Notify(side, event, data);
}

std::shared_ptr<Reward> ChooseReward(Game& game, Side& side, const Quest* active_quest) {
    return QuestSystem(game).ChooseReward(side, active_quest);
}

}  // namespace quest

std::shared_ptr<QuestResult> QuestSystem::Finish(Side& side, const std::string& reason, int why) {
    auto finished = side.QuestPtr();
    side.ClearQuest();
    if (!finished) return nullptr;
    auto out = std::make_shared<QuestResult>();
    out->quest = finished;
    if (reason == "done") {
        Army* hero = finished->ResolveHero(game_);
        history::RecordDeed(game_, &side, DeedType::kQuestCompleted, 0, 0,
                            hero ? hero->Name() : "");
        if (hero) hero::AddExperience(game_, hero, quest::kExperience);
        out->reward = ChooseReward(side, finished.get());
    } else {
        out->failed = reason;
        out->why = why;
    }
    if (!side.IsComputerControlled()) game_.TurnNews(side.Id()).quest_news = out;
    return out;
}

std::shared_ptr<Quest> QuestSystem::Assign(Side& side, Army* hero, bool computer, bool flying) {
    if (side.HasQuest()) return nullptr;
    if (game_.map->options.quests == 0) return nullptr;
    history::RecordDeed(game_, &side, DeedType::kQuestReceived, 0, 0, hero ? hero->Name() : "");
    for (int attempt = 1;; attempt++) {
        int type = QuestType(game_, attempt, computer);
        if (auto target = PickTarget(game_, side, type, hero, computer, flying)) {
            int required = kNone;
            if (type == quest::kSlaughter)
                required = game_.rng.Dice(1, 12, 10);
            else if (type == quest::kPillageGold)
                required = game_.rng.Dice(3, 300, 500);
            ArmyId hero_id = hero ? hero->Id() : ArmyId::None();
            auto assigned = std::make_shared<Quest>(type, hero_id, *target, required, 0);
            side.SetQuest(assigned);
            return assigned;
        }
    }
}

std::string QuestSystem::Describe(const Game& game, const Quest& active_quest) {
    const char* text = quest::kDescriptions[active_quest.Type()];
    if (active_quest.Required() != kNone) return Format(text, active_quest.Required());
    if (active_quest.Type() == quest::kOccupy || active_quest.Type() == quest::kRaze) {
        const City* city = active_quest.ResolveCity(game);
        return std::string(text) + ": " + (city ? city->Name() : "");
    }
    if (active_quest.Type() == quest::kSlayType) {
        const ArmyType* army_type = active_quest.ResolveArmyType(game);
        return std::string(text) + ": " + (army_type ? army_type->name : "");
    }
    return text;
}

std::shared_ptr<QuestResult> QuestSystem::Notify(Side& side, const std::string& event,
                                                 const quest::Event& data) {
    auto active = side.QuestPtr();
    if (!active) return nullptr;
    Quest& current_quest = *active;
    int type = current_quest.Type();

    if (event == "turn") {
        Army* hero = game_.armies.FindAlive(current_quest.HeroId());
        bool alive = hero != nullptr && hero->OwnedBy(side.Id());
        if (!alive) return Finish(side, "the hero is lost", 0x20);
        if (type == quest::kOccupy || type == quest::kRaze) {
            const City* city = current_quest.ResolveCity(game_);
            if (!city || city->IsRazed()) return Finish(side, "the city is ruins", 0x21);
            if (city->OwnedBy(side.Id())) return Finish(side, "another took the city", 0x2a);
        } else if (type == quest::kSlaughter) {
            const Side* target_side = current_quest.ResolveSide(game_);
            if (!target_side || !target_side->IsAlive())
                return Finish(side, "that side is gone", 0x27);
        } else if (type == quest::kSlayHero) {
            if (!game_.armies.Alive(current_quest.TargetArmyId()))
                return Finish(side, "the quarry is gone", 0x29);
        } else if (type == quest::kRetrieveItem) {
            const Item* item = current_quest.ResolveItem(game_);
            if (!item || item->status == 0) return Finish(side, "the item is lost", 0x28);
        }
        return nullptr;
    }

    if (event == "battle") {
        if (!HeroInStack(current_quest, data.stack)) return nullptr;
        if (type == quest::kSlayHero) {
            ArmyId target_army = current_quest.TargetArmyId();
            for (Army* victim : data.killed) {
                if (victim && target_army.IsValid() && victim->Id() == target_army)
                    return Finish(side, "done");
            }
        } else if (type == quest::kSlayType) {
            ArmyTypeId want_type = current_quest.TargetArmyTypeId();
            for (Army* victim : data.killed) {
                if (victim && victim->Type() == want_type) return Finish(side, "done");
            }
        } else if (type == quest::kSlaughter) {
            SideId target_owner = current_quest.TargetSideId();
            for (Army* victim : data.killed) {
                if (victim && victim->OwnedBy(target_owner)) current_quest.AddProgress(1);
            }
            if (current_quest.Done() >= current_quest.Required()) return Finish(side, "done");
        }
    } else if (event == "item") {
        Army* hero = current_quest.ResolveHero(game_);
        ItemId want_item = current_quest.TargetItemId();
        if (type == quest::kRetrieveItem && hero && want_item.IsValid()) {
            Item* matched = nullptr;
            for (Item* carried : hero->Items()) {
                if (carried && carried->index == want_item.Value()) {
                    matched = carried;
                    break;
                }
            }
            if (matched != nullptr) {
                hero->RemoveItem(matched);
                matched->status = 0;
                return Finish(side, "done");
            }
        }
    } else if (event == "pillage") {
        CityId target_city = current_quest.TargetCityId();
        if (type == quest::kPillageGold && HeroInStack(current_quest, data.stack)) {
            current_quest.AddProgress(data.gold);
            if (current_quest.Done() >= current_quest.Required()) return Finish(side, "done");
        } else if ((type == quest::kOccupy || type == quest::kRaze) && data.city &&
                   data.city->Id() == target_city) {
            return Finish(side, "that was not to pillage", 0x24);
        }
    } else if (event == "occupy") {
        CityId target_city = current_quest.TargetCityId();
        if (type == quest::kOccupy && data.city && data.city->Id() == target_city) {
            if (HeroInStack(current_quest, data.stack)) return Finish(side, "done");
            return Finish(side, "the hero was not there", 0x22);
        }
        if (type == quest::kRaze && data.city && data.city->Id() == target_city) {
            return Finish(side, "the quest was to raze it", 0x23);
        }
    } else if (event == "raze") {
        CityId target_city = current_quest.TargetCityId();
        if (type == quest::kRaze && data.city && data.city->Id() == target_city) {
            if (HeroInStack(current_quest, data.stack)) return Finish(side, "done");
            return Finish(side, "the hero was not there", 0x25);
        }
        if (type == quest::kOccupy && data.city && data.city->Id() == target_city) {
            return Finish(side, "the quest was to keep it", 0x26);
        }
    }
    return nullptr;
}

std::shared_ptr<Reward> QuestSystem::ChooseReward(Side& side, const Quest* active_quest) {
    int cities = static_cast<int>(game::SideCities(game_, side).size());
    Army* hero = active_quest ? active_quest->ResolveHero(game_) : nullptr;

    auto give_allies = [&](int count) {
        const ArmyType* type = game_.types.ById(quest::kRewardAlly);
        auto reward = std::make_shared<Reward>();
        reward->kind = "allies";
        reward->type = type;
        reward->count = count;
        for (int i = 0; i < count; i++) {
            Army* ally = hero ? hero::PlaceAlly(game_, type, hero->X(), hero->Y(), side) : nullptr;
            if (ally) reward->armies.push_back(ally);
        }
        return reward;
    };

    auto give_gold = [&]() {
        int amount = game_.rng.Dice(2, 1000, 1000);
        side.AddGold(amount);
        auto reward = std::make_shared<Reward>();
        reward->kind = "gold";
        reward->gold = amount;
        return reward;
    };

    if (cities < 10 && game_.turn > 15) return give_allies(game_.rng.Dice(1, 3, 5));
    if (side.Gold() < 100) return give_gold();

    std::vector<Item*> unused;
    for (auto& item : game_.map->items) {
        if (item.index <= 21 && item.status == 0 && site::ItemReserved(item) &&
            (item.index > 7 || item.index == side.Index())) {
            unused.push_back(&item);
        }
    }
    std::vector<Site*> unknown;
    for (auto& candidate : game_.map->sites) {
        if (candidate.rich && !site::ShownTo(candidate, side) && candidate.content != site::kEmpty)
            unknown.push_back(&candidate);
    }
    if (!unused.empty() && game_.rng.Dice(1, 3, 0) == 1) {
        Item* item = unused[game_.rng.Dice(1, static_cast<int>(unused.size()), -1)];
        if (hero) {
            hero->AddItem(item);
            item->status = 3;
        }
        auto reward = std::make_shared<Reward>();
        reward->kind = "item";
        reward->item = item;
        return reward;
    }
    if (unused.empty() && !unknown.empty() && game_.rng.Dice(1, 3, 0) < 3) {
        Site* best = nullptr;
        int best_distance = 10000;
        for (Site* candidate : unknown) {
            int distance = hero ? std::max(std::abs(candidate->x - hero->X()),
                                           std::abs(candidate->y - hero->Y()))
                                : 0;
            if (distance < best_distance) {
                best = candidate;
                best_distance = distance;
            }
        }
        best->revealed |= 1 << side.Index();
        if (!side.IsComputerControlled() || side.IsObserved())
            game::Reveal(game_, side.Index(), best->x, best->y, false);
        auto reward = std::make_shared<Reward>();
        reward->kind = "revealed";
        reward->site = best;
        return reward;
    }
    if (game_.rng.Dice(1, 2, 0) == 1) return give_allies(game_.rng.Dice(1, 3, 2));
    return give_gold();
}

}  // namespace w2
