#include "warlords/ai/core.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <map>
#include <mutex>
#include <queue>

#include "absl/base/no_destructor.h"

#include "util/util.hpp"
#include "warlords/ai.hpp"
#include "warlords/ai/classic_ai_player.hpp"
#include "warlords/ai/groups.hpp"
#include "warlords/ai/moves.hpp"
#include "warlords/armytype.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/game.hpp"
#include "warlords/scn.hpp"
#include "warlords/site.hpp"

namespace w2::ai::core {

int Dist(int from_x, int from_y, int to_x, int to_y) {
    double dx = from_x - to_x, dy = from_y - to_y;
    return static_cast<int>(std::floor(std::sqrt(dx * dx + dy * dy)));
}

bool InPlay(const Game& game, int index) {
    if (index < 0 || index > 7) return false;
    const Side& side = game.map->sides[index];
    return side.in_use && side.alive;
}

bool IsComputer(const Game& game, int index) {
    if (index < 0 || index > 7) return false;
    return game.map->sides[index].computer;
}

int State(const Game& game, int side_index, int other_index) {
    if (side_index == kNeutral || other_index == kNeutral || side_index == kNone ||
        other_index == kNone)
        return 2;
    if (side_index == other_index) return 0;
    return diplomacy::State(game, side_index, other_index);
}

int Proposal(const Game& game, int side_index, int other_index) {
    if (side_index == other_index) return 0;
    return diplomacy::Proposal(game, side_index, other_index);
}

void Propose(Game& game, int side_index, int other_index, int proposal) {
    if (side_index == other_index) return;
    diplomacy::Propose(game, side_index, other_index, proposal);
}

City* CityAt(const Game& game, int x, int y) {
    return game::CityAt(game, x, y);
}

Site* SiteAt(const Game& game, int x, int y) {
    if (game.map->site_at.empty() || x < 0 || y < 0 || x >= game.map->width ||
        y >= game.map->height)
        return nullptr;
    return game.map->site_at[y * game.map->width + x];
}

int Terrain(const Game& game, int x, int y) {
    return scn::TerrainAt(*game.map, x, y);
}

bool SiteOpen(const Site& site) {
    return site.content == site::kTemple || !site.searched;
}

bool Explored(const Game& game, int side_index, int x, int y) {
    if (game.map->options.hidden_map == 0) return true;
    for (int tile_x = x - 1; tile_x <= x + 1; tile_x++) {
        for (int tile_y = y - 1; tile_y <= y + 1; tile_y++) {
            if (tile_x >= 0 && tile_y >= 0 && tile_x < game.map->width &&
                tile_y < game.map->height && game::Seen(game, side_index, tile_x, tile_y))
                return true;
        }
    }
    return false;
}

bool Flies(const Game& game, const Army* army) {
    return game.types.ById(army->type)->flies;
}
bool Magical(const Game& game, const Army* army) {
    return game.types.ById(army->type)->IsMagical();
}
int Ability(const Game& game, const Army* army) {
    return game.types.ById(army->type)->Ability();
}
bool Woods(const Game& game, const Army* army) {
    return game.types.ById(army->type)->woods_move;
}
bool Hills(const Game& game, const Army* army) {
    return game.types.ById(army->type)->hills_move;
}

std::vector<Army*> Armies(const Game& game, int side_index) {
    std::vector<Army*> out;
    for (int i = static_cast<int>(game.armies.size()) - 1; i >= 0; i--) {
        Army* army = game.armies[i];
        if (army->owner == side_index) out.push_back(army);
    }
    return out;
}

std::vector<Army*> OnTile(const Game& game, int side_index, int x, int y) {
    std::vector<Army*> out;
    for (int i = static_cast<int>(game.armies.size()) - 1; i >= 0; i--) {
        Army* army = game.armies[i];
        if (army->owner == side_index && !army->transit && army->x == x && army->y == y)
            out.push_back(army);
    }
    return out;
}

AIData NewData() {
    AIData data;
    data.groups[0] = EmptyGroup();  // never used: groups count from 1
    for (int i = 1; i <= kMaxGroups; i++) data.groups[i] = EmptyGroup();
    return data;
}

AIData& Data(Game& game, Side& side) {
    return ClassicAiPlayer::EnsureOn(game, side).MutableData();
}

AIData& Data(Game& game, int side_index) {
    return Data(game, *game.map->Side(side_index));
}

const ClassicArmyOrder& OrderOf(const Game& game, const Army* army) {
    static const absl::NoDestructor<ClassicArmyOrder> kDefault;
    if (!army || !game.map || army->owner < 0 || army->owner > 7) return *kDefault;
    const ClassicAiPlayer* player = ClassicAiPlayer::FindOn(game.map->sides[army->owner]);
    return player ? player->OrderFor(army->id) : *kDefault;
}

ClassicArmyOrder& MutableOrderOf(Game& game, Army* army) {
    static ClassicArmyOrder fallback_scratch;
    if (!army || !game.map || army->owner < 0 || army->owner > 7) {
        fallback_scratch = ClassicArmyOrder{};
        return fallback_scratch;
    }
    return ClassicAiPlayer::EnsureOn(game, game.map->sides[army->owner]).MutableOrderFor(army->id);
}

void ClearOrder(Game& game, Army* army) {
    if (!army || !game.map || army->owner < 0 || army->owner > 7) return;
    if (ClassicAiPlayer* player = ClassicAiPlayer::FindMutableOn(game.map->sides[army->owner])) {
        player->ClearOrder(army->id);
    }
}

std::pair<std::vector<City*>, std::vector<int>> FindNeighbours(Game& game, const City& city) {
    std::shared_ptr<Neighbours> table;
    for (const auto& side : game.map->sides) {
        if (const ClassicAiPlayer* player = ClassicAiPlayer::FindOn(side)) {
            if (player->NeighbourTable()) {
                table = player->NeighbourTable();
                break;
            }
        }
    }
    if (!table) {
        table = BuildNeighbours(game);
        for (auto& side : game.map->sides) {
            if (ClassicAiPlayer* player = ClassicAiPlayer::FindMutableOn(side)) {
                player->SetNeighbourTable(table);
            }
        }
    }
    std::pair<std::vector<City*>, std::vector<int>> out;
    if (city.index >= 0 && city.index < static_cast<int>(table->by_city.size())) {
        const auto& entry = table->by_city[city.index];
        for (int city_index : entry.cities) out.first.push_back(&game.map->cities[city_index]);
        out.second = entry.dist;
    }
    return out;
}

namespace {
/**
 * @brief Picks a city's six neighbours from a flood out of it.
 *
 * Every other city under 100 away goes into a list of 50 -- the first free
 * place, or over the farthest one when this one is nearer -- walking the
 * cities from the last; then six are taken, nearest first and 50 further
 * off for a direction that has three already, the list read from its end so
 * a tie goes to the later place.
 *
 * @return How many cities the flood found.
 */
int PickNeighbours(Game& game, const City& city, const Flood& flood, std::vector<int>& picked,
                   std::vector<int>& dists) {
    int slot_city[50], slot_dist[50];
    for (int slot = 0; slot < 50; slot++) {
        slot_city[slot] = -1;
        slot_dist[slot] = 0;
    }
    int found = 0;
    for (int i = static_cast<int>(game.map->cities.size()) - 1; i >= 0; i--) {
        if (i == city.index) continue;
        auto [distance, near_x, near_y] = CityDistance(game, flood, game.map->cities[i]);
        (void)near_y;
        if (near_x == kNone) continue;
        found++;
        int chosen = -1, worst = distance;
        int slot = 0;
        for (; slot < 50 && slot_city[slot] != -1; slot++) {
            if (worst < slot_dist[slot]) {
                worst = slot_dist[slot];
                chosen = slot;
            }
        }
        if (slot < 50) chosen = slot;
        if (chosen != -1 && (slot_city[chosen] == -1 || distance < slot_dist[chosen])) {
            slot_city[chosen] = i;
            slot_dist[chosen] = distance & 0xff;
        }
    }
    int left = 0, right = 0, above = 0, down = 0;
    picked.clear();
    dists.clear();
    while (picked.size() < 6) {
        int best = -1, best_score = 10000;
        for (int slot = 49; slot >= 0; slot--) {
            if (slot_city[slot] != -1) {
                const City& other = game.map->cities[slot_city[slot]];
                int score = slot_dist[slot];
                if ((other.x < city.x && left > 2) || (city.x < other.x && right > 2) ||
                    (other.y < city.y && above > 2) || (city.y < other.y && down > 2)) {
                    score += 50;
                }
                if (score < best_score) {
                    best_score = score;
                    best = slot;
                }
            }
        }
        if (best == -1) break;
        const City& other = game.map->cities[slot_city[best]];
        picked.push_back(other.index);
        dists.push_back(slot_dist[best]);
        if (other.x < city.x) left++;
        if (city.x < other.x) right++;
        if (other.y < city.y) above++;
        if (city.y < other.y) down++;
        slot_city[best] = -1;
    }
    return found;
}
}  // namespace

std::shared_ptr<Neighbours> BuildNeighbours(Game& game) {
    auto out = std::make_shared<Neighbours>();
    out->by_city.resize(game.map->cities.size());
    for (auto& city : game.map->cities) {
        std::vector<int> picked, dists;
        int found = PickNeighbours(game, city,
                                   FloodFrom(game, 0, city.x, city.y, 45, move::kLand, kNone),
                                   picked, dists);
        if (found == 0)
            PickNeighbours(game, city, FloodFrom(game, 0, city.x, city.y, 60, move::kLand, kNone),
                           picked, dists);
        out->by_city[city.index] = Neighbours::Entry{picked, dists};
    }
    return out;
}

Sel& Refresh(Game& game, Sel& sel) {
    int least = 100;
    for (Army* army : sel.armies) least = std::min(least, army->moves);
    sel.min_moves = sel.armies.empty() ? 0 : least;
    sel.mode = sel.armies.empty() ? move::kLand : move::ModeOf(game, sel.armies);
    sel.hero = nullptr;
    for (Army* army : sel.armies)
        if (IsHero(army) && !sel.hero) sel.hero = army;
    return sel;
}

Sel Select(Game& game, const std::vector<Army*>& list) {
    Sel sel;
    for (Army* army : list) {
        if (army && Alive(game, army) && !army->transit) sel.armies.push_back(army);
    }
    sel.leader = sel.armies.empty() ? nullptr : sel.armies[0];
    Refresh(game, sel);
    return sel;
}

/**
 * @brief The army's place in its side's fight order (neutrals use row 8);
 *        the highest-ranked army leads a stack.
 */
static int FightRank(const Game& game, const Army* army) {
    const auto& row = game.map->fight_order[army->owner == kNone ? 8 : army->owner];
    return army->type < static_cast<int>(row.size()) ? row[army->type] : 0;
}

int NewGroup(const Game& game, int side_index) {
    std::set<int> used;
    for (Army* army : game.armies) {
        if (army->owner == side_index && !army->transit && army->group) used.insert(army->group);
    }
    for (int number = 2; number <= 254; number++)
        if (!used.count(number)) return number;
    return 0;
}

std::optional<Sel> Order(Game& game, const std::vector<Army*>& list, int order, int dest,
                         int flags) {
    Sel sel = Select(game, list);
    if (sel.armies.empty()) return std::nullopt;
    Army* best = nullptr;
    for (Army* army : sel.armies)
        if (!best || FightRank(game, army) > FightRank(game, best)) best = army;
    sel.leader = best;
    int group_number = sel.armies.size() > 1 ? NewGroup(game, best->owner) : 0;
    for (Army* army : sel.armies) {
        ClassicArmyOrder& ord = MutableOrderOf(game, army);
        ord.order = order;
        ord.dest = dest;
        army->group = group_number;
        SetTarget(game, army, order, dest);
        ord.neutral = false;
        ord.explore = false;
        if (flags) {
            if (Has(flags, 0x80)) ord.neutral = true;
            if (Has(flags, 0x20)) ord.explore = true;
            if (Has(flags, 0x100)) ord.party = true;
        }
    }
    Refresh(game, sel);
    return sel;
}

void SetTarget(Game& game, Army* army, int order, int dest) {
    if (order == kOrderRoam) return;
    int x = kNone, y = kNone;
    if (order == kOrderCity && dest != kNone) {
        City* city = game.map->City(dest);
        if (city) {
            // An own city's bottom-right tile; an enemy's nearest corner.
            x = city->x;
            y = city->y;
            if (city->owner_index == army->owner) {
                x++;
                y++;
            } else {
                if (x < army->x) x++;
                if (y < army->y) y++;
            }
        }
    } else if (order == kOrderItem && dest != kNone) {
        if (dest >= 0 && dest < static_cast<int>(game.map->items.size())) {
            const Item& item = game.map->items[dest];
            if (item.x != kNone) {
                x = item.x;
                y = item.y;
            }
        }
    } else if (order == kOrderSite && dest != kNone) {
        if (dest >= 0 && dest < static_cast<int>(game.map->sites.size())) {
            const Site& site = game.map->sites[dest];
            x = site.x;
            y = site.y;
        }
    }
    army->target = std::make_pair(x != kNone ? x : army->x, y != kNone ? y : army->y);
}

Sel SelectStackOf(Game& game, Army* army) {
    std::vector<Army*> list;
    for (Army* other : OnTile(game, army->owner, army->x, army->y)) {
        if (other == army || (army->group != 0 && other->group == army->group)) {
            list.push_back(other);
            MutableOrderOf(game, other).moved = true;
        }
    }
    Sel sel = Select(game, list);
    Army* best = nullptr;
    for (Army* member : sel.armies)
        if (!best || FightRank(game, member) > FightRank(game, best)) best = member;
    sel.leader = best ? best : army;
    return sel;
}

std::vector<Army*> Collect(const Game& game, int side_index, int x, int y, int min_moves) {
    std::vector<Army*> out;
    for (Army* army : OnTile(game, side_index, x, y)) {
        if (static_cast<int>(out.size()) >= rules::kMaxStack) break;
        if (!min_moves || army->moves >= min_moves) out.push_back(army);
    }
    return out;
}

std::vector<Army*> CollectOrdered(const Game& game, int side_index, int x, int y, int group,
                                  int order, int min_max) {
    std::vector<Army*> out;
    for (Army* army : OnTile(game, side_index, x, y)) {
        if (static_cast<int>(out.size()) >= rules::kMaxStack) break;
        const ClassicArmyOrder& ord = OrderOf(game, army);
        if (ord.group == group && ord.order == order && (!min_max || army->max_moves >= min_max))
            out.push_back(army);
    }
    return out;
}

Flood FloodFrom(Game& game, int side_index, int x, int y, int range, const Sel* sel, int fog_side) {
    int mode = sel ? sel->mode : move::kLand;
    bool woods = false, hills = false;
    if (sel) {
        move::Mode stack_mode = move::StackMode(game, sel->armies);
        woods = stack_mode.woods;
        hills = stack_mode.hills;
    }
    Flood flood;
    flood.w = game.map->width;
    flood.h = game.map->height;
    flood.dist = move::Flood(game, side_index, x, y, range, mode, woods, hills,
                             fog_side == kSameSide ? side_index : fog_side);
    return flood;
}

Flood FloodFrom(Game& game, int side_index, int x, int y, int range, int mode, int fog_side) {
    Flood flood;
    flood.w = game.map->width;
    flood.h = game.map->height;
    flood.dist = move::Flood(game, side_index, x, y, range, mode, false, false,
                             fog_side == kSameSide ? side_index : fog_side);
    return flood;
}

int FloodAt(const Flood& flood, int x, int y) {
    if (x < 0 || y < 0 || x >= flood.w || y >= flood.h) return kUnreached;
    int cost = flood.dist[y * flood.w + x];
    if (cost == 30000 || cost == move::kShut) return kUnreached;
    return std::abs(cost);
}

// The ring round a city's 2x2 footprint, as the original walks it: twelve
// steps, each a direction into kRingDx/kRingDy, starting from the city's
// top-left tile.
static const int kRing[12] = {0, 2, 2, 4, 4, 4, 6, 6, 6, 0, 0, 0};
static const int kRingDx[8] = {0, 1, 1, 1, 0, -1, -1, -1};
static const int kRingDy[8] = {-1, -1, 0, 1, 1, 1, 0, -1};

std::tuple<int, int, int> CityDistance(const Game& game, const Flood& flood, const City& city,
                                       bool far) {
    int best = far ? 1000 : 100;
    int best_x = kNone, best_y = kNone;
    int x = city.x, y = city.y;
    for (int dir : kRing) {
        int next_x = x + kRingDx[dir], next_y = y + kRingDy[dir];
        // The walk ends at the edge of the full-sized map (112 x 156 tiles).
        if (next_x < 0 || next_y < 0 || next_x >= 112 || next_y >= 156)
            return {best, best_x, best_y};
        x = next_x;
        y = next_y;
        int distance = FloodAt(flood, x, y);
        if (distance < best) {
            best = distance;
            best_x = x;
            best_y = y;
        }
    }
    return {best, best_x, best_y};
}

int Odds(Game& game, const Sel& sel, int x, int y) {
    if (sel.armies.empty()) return 0;
    int owner = sel.armies[0]->owner;
    int rounds = owner != kNone ? Data(game, owner).sims : 10;
    if (rounds < 1) rounds = 1;
    combat::Lines lines = combat::BattleLines(game, sel.armies, x, y);
    if (lines.defenders.empty()) return 100;
    int wins = 0;
    for (int i = 0; i < rounds; i++)
        if (combat::Resolve(game, lines.attackers, lines.defenders, x, y).won) wins++;
    return wins * 100 / rounds;
}

void PickUp(Game& game, const Sel& sel) {
    if (!sel.hero || !Alive(game, sel.hero)) return;
    Army* hero = sel.hero;
    City* city = CityAt(game, hero->x, hero->y);
    for (auto& item : game.map->items) {
        if (item.status == 1 && item.x != kNone && !item.planted) {
            // In a city, anything inside its footprint counts.
            bool here;
            if (city)
                here = item.x >= city->x && item.x <= city->x + 1 && item.y >= city->y &&
                       item.y <= city->y + 1;
            else
                here = item.x == hero->x && item.y == hero->y;
            if (here) {
                item.status = 3;
                item.x = kNone;
                item.y = kNone;
                hero->items.push_back(&item);
            }
        }
    }
}

namespace {
/** @brief How one walk of a selection ended. */
struct StepResult {
    int code = 0;              ///< See Step().
    int x = kNone, y = kNone;  ///< The tile ahead, for codes 3 and 5.
};

/**
 * @brief Walks the selection towards (target_x, target_y).
 * @return 1 no path, 2 stopped short, 3 an enemy army is next, 4 the path is
 *         walked, 5 an enemy city is next; with the tile ahead.
 */
StepResult Step(Game& game, Sel& sel, int target_x, int target_y) {
    Army* lead = sel.leader;
    lead->target = std::make_pair(target_x, target_y);
    auto path = move::FindPath(game, sel.armies, lead->x, lead->y, target_x, target_y);
    if (!path) return {1};
    if (path->empty()) return {4};
    move::WalkResult walk = move::Walk(game, sel.armies, *path);
    if (walk.steps > 0) Walked(game, sel.armies, walk);
    Refresh(game, sel);
    PickUp(game, sel);
    if (walk.stopped == move::WalkStop::kAttack) {
        return {walk.attack->city ? 5 : 3, walk.attack->x, walk.attack->y};
    } else if (walk.stopped == move::WalkStop::kAtPeace) {
        // Stopped by a side it is at peace with: a city there is still a city
        // to attack.
        if (walk.steps < static_cast<int>(path->size())) {
            const move::Step& ahead = (*path)[walk.steps];
            if (CityAt(game, ahead.x, ahead.y)) return {5, ahead.x, ahead.y};
        }
        return {2};
    } else if (walk.stopped == move::WalkStop::kArrived) {
        return {4};
    }
    return {2};
}
}  // namespace

Battle Fight(Game& game, Sel& sel, int x, int y) {
    // The computer's attack: the lines are drawn up first; a city of a side
    // it is not at war with is treachery -- war, and the score's penalty;
    // then the attack is paid for and fought, shown, and only then takes
    // effect.
    Battle result = game::DecideAttack(game, sel.armies, x, y);
    int side_index = sel.armies.empty() ? kNone : sel.armies[0]->owner;
    City* city = result.lines.city;
    if (city && city->owner_index != kNone && side_index != kNone &&
        game.map->options.diplomacy != 0 &&
        diplomacy::State(game, side_index, city->owner_index) != diplomacy::kWar) {
        diplomacy::AttackAnyway(game, side_index, city->owner_index);
    }
    game::ChargeAttack(game, sel.armies, x, y);
    if (hooks.on_fight) hooks.on_fight(game, sel.armies, x, y, result);
    game::ApplyAttack(game, result);
    std::vector<Army*> keep;
    for (Army* army : sel.armies)
        if (Alive(game, army)) keep.push_back(army);
    sel.armies = keep;
    if (!Alive(game, sel.leader)) sel.leader = keep.empty() ? nullptr : keep[0];
    Refresh(game, sel);
    return result;
}

void Done(const Sel& sel) {
    for (Army* army : sel.armies) army->done = true;
}

int MoveTo(Game& game, Sel& sel, int target_x, int target_y, bool keep) {
    if (sel.armies.empty() || target_x < 0 || target_y < 0 || target_x >= game.map->width ||
        target_y >= game.map->height)
        return 0;
    int code = 0;
    bool again = true;
    while (again) {
        again = false;
        if (sel.armies.empty()) break;
        StepResult step = Step(game, sel, target_x, target_y);
        code = step.code;
        int attack_x = step.x, attack_y = step.y;
        Army* lead = sel.leader;
        if (lead && lead->target && lead->x == lead->target->first &&
            lead->y == lead->target->second) {
            code = 4;
            lead->target.reset();
        }
        if (code == 2) {
            Done(sel);
        } else if (code == 5) {
            City* city = CityAt(game, target_x, target_y);
            if (city && Standing(*city)) {
                int lead_dest = lead ? OrderOf(game, lead).dest : kNone;
                City* dest = lead_dest != kNone ? game.map->City(lead_dest) : nullptr;
                if (!dest) dest = city;
                auto [walk_on, next_x, next_y] = AttackCity(game, sel, dest, attack_x, attack_y);
                again = walk_on;
                if (again) {
                    target_x = next_x;
                    target_y = next_y;
                }
            }
        } else if (code == 4) {
            if (lead && OrderOf(game, lead).order == kOrderSite && sel.hero) {
                Site* site = SiteAt(game, sel.hero->x, sel.hero->y);
                if (site) moves::SearchSite(game, &sel, sel.hero, *site);
            }
            for (Army* army : sel.armies) {
                ClearOrder(game, army);
                MutableOrderOf(game, army).group = 0;
            }
        } else if (code == 3) {
            Fight(game, sel, attack_x, attack_y);
            if (!sel.armies.empty()) again = true;
        }
    }
    if (!keep) Done(sel);
    return code;
}

std::tuple<bool, int, int> AttackCity(Game& game, Sel& sel, City* dest, int attack_x,
                                      int attack_y) {
    int side_index = sel.leader->owner;
    AIData& data = Data(game, side_index);
    int defender = Owner(*dest);
    City* city = CityAt(game, attack_x, attack_y);
    if (defender == kNeutral) {
        Fight(game, sel, attack_x, attack_y);
        if (city && city->owner_index == side_index) SetRole(data, *city, kJustTaken);
    } else if (defender != side_index) {
        // A quest's city is left to the hero; any other may give way to a
        // better one nearby.
        City* pick = dest;
        if (data.quest_city != dest->index || !sel.hero)
            pick = Retarget(game, sel, dest, attack_x, attack_y);
        if (pick != dest) {
            for (Army* army : sel.armies) {
                ClassicArmyOrder& ord = MutableOrderOf(game, army);
                ord.order = kOrderCity;
                ord.dest = pick->index;
            }
            return {true, pick->x, pick->y};
        }
        int previous_owner = city ? Owner(*city) : kNone;
        Fight(game, sel, attack_x, attack_y);
        if (city && city->owner_index == side_index)
            groups::EarlyVengeance(game, side_index, *city, previous_owner);
    }
    for (Army* army : sel.armies) ClearOrder(game, army);
    return {false, kNone, kNone};
}

City* Retarget(Game& game, Sel& sel, City* city, int x, int y) {
    int side_index = sel.leader->owner;
    int odds = Odds(game, sel, x, y);
    int owner = Owner(*city);
    // Fair game: a neutral, a side at war, and now and then a human.
    bool fair = owner == kNeutral || State(game, owner, side_index) == 2;
    if (owner != kNeutral && !IsComputer(game, owner) && game.rng.Dice(1, 4, -1) == 0) fair = true;
    if (!fair || odds < 51) {
        Flood flood = FloodFrom(game, side_index, sel.leader->x, sel.leader->y, 15, &sel);
        City* best = BestCity(game, sel, flood, fair);
        return best ? best : city;
    }
    return city;
}

City* BestCity(Game& game, const Sel& sel, const Flood& flood, bool skip_own) {
    int side_index = sel.leader->owner;
    AIData& data = Data(game, side_index);
    City* best = nullptr;
    int best_score = -1;
    auto& cities = game.map->cities;
    for (int i = static_cast<int>(cities.size()) - 1; i >= 0; i--) {
        City& city = cities[i];
        int owner = Owner(city);
        if (!(skip_own && owner == side_index) && Standing(city) && data.quest_city != city.index &&
            !Cflag(data, city, kCfUnseen) &&
            (owner == kNeutral || owner == side_index || State(game, owner, side_index) == 2)) {
            auto [distance, near_x, near_y] = CityDistance(game, flood, city);
            (void)near_y;
            if (near_x != kNone) {
                // An own city counts as a safe retreat: fixed odds, a little
                // further off.
                int odds;
                if (owner == side_index) {
                    odds = 15;
                    distance += 20;
                } else
                    odds = Odds(game, sel, city.x, city.y);
                int score;
                if (odds >= 51 && distance < sel.min_moves)
                    score = 400 - distance + odds * 10;
                else if (odds > 10)
                    score = 100 - distance + odds * 5;
                else
                    score = -2;
                if (score > best_score) {
                    best = &city;
                    best_score = score;
                }
            }
        }
    }
    return best;
}

}  // namespace w2::ai::core
