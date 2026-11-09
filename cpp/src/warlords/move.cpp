/**
 * @file
 * @brief Stack movement modes, terrain step costs, shoreline transitions, and path walking.
 */
#include "warlords/move.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <map>
#include <string>

#include "warlords/armytype.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/game.hpp"
#include "warlords/rules.hpp"
#include "warlords/scn.hpp"

namespace w2 {

namespace move {

const char* const kTerrainNames[12] = {"road",      "bridge", "water", "shore", "forest", "hills",
                                       "mountains", "plain",  "marsh", "tower", "city",   "site"};

Mode StackMode(const Game& game, const Stack& stack) {
    bool at_sea = false, any_boat = false;
    bool all_fly = true, any_fly = false, all_non_hero_fly = true, hero_flight = false;
    bool woods = false, hills = false;
    for (Army* army : stack) {
        const ArmyType* army_type = game.ArmyTypes().ById(army->Type());
        if (army->IsAtSea()) at_sea = true;
        if (army_type->boat) any_boat = true;
        if (army_type->flies && !army->IsAtSea()) any_fly = true;
        if (!army_type->flies || army->IsAtSea()) {
            all_fly = false;
            if (!army->Hero()) all_non_hero_fly = false;
        }
        if (army->Hero()) {
            for (Item* item : army->Items())
                if (item->type == rules::kItemFlight) hero_flight = true;
        }
        if (army_type->woods_move) woods = true;
        if (army_type->hills_move) hills = true;
    }
    if (at_sea) return Mode{kLand, false, false, true};
    if (any_boat) return Mode{kBoat, woods, hills, false};
    if (hero_flight || all_fly || (all_non_hero_fly && any_fly))
        return Mode{kFlying, woods, hills, false};
    return Mode{kLand, woods, hills, false};
}

namespace {

const int kPortTour[12] = {0, 2, 2, 4, 4, 4, 6, 6, 6, 0, 0, 0};

bool OutOfMoves(const Stack& stack, const Path& path, int landed, WalkStop stopped) {
    int moves_left = StackMoves(stack);
    if (moves_left < 2) return true;
    if (landed == 0 || stopped == WalkStop::kAttack || stopped == WalkStop::kAtPeace) return false;
    if (landed >= static_cast<int>(path.size())) return false;
    int total = 0;
    for (int i = 0; i <= landed; i++) total += path[i].cost;
    if (total >= 255) return false;
    return moves_left < total;
}

WalkResult WalkPath(Game& game, const Stack& stack, const Path& path) {
    int side = stack.empty() ? kNone : stack[0]->OwnerIndex();
    int left = StackMoves(stack);
    WalkResult result;
    result.stopped = WalkStop::kArrived;
    for (Army* army : stack)
        if (army->IsAtSea()) result.sea_before = true;
    int last_ok = 0, cumulative = 0;
    std::map<int, int> cost_to;

    for (int i = 1; i <= static_cast<int>(path.size()); i++) {
        const Step& step = path[i - 1];
        if (step.cost > left) {
            result.stopped = WalkStop::kOutOfMoves;
            break;
        }
        City* city = game.GetMap().CityAtFootprint(step.x, step.y);
        auto here = game::ArmiesAt(game, step.x, step.y);
        if (city && !city->IsRazed() && !city->OwnedBy(side)) {
            if (!diplomacy::MayAttack(game, side, city->OwnerIndex())) {
                result.stopped = WalkStop::kAtPeace;
                break;
            }
            result.stopped = WalkStop::kAttack;
            result.attack = Attack{step.x, step.y, city};
            break;
        }
        bool theirs = !here.empty() && !here[0]->OwnedBy(side);
        if (theirs && diplomacy::MayAttack(game, side, here[0]->OwnerIndex())) {
            result.stopped = WalkStop::kAttack;
            result.attack = Attack{step.x, step.y, nullptr};
            break;
        }
        left -= step.cost;
        cumulative += step.cost;
        if (!theirs && static_cast<int>(here.size() + stack.size()) <= rules::kMaxStack) {
            last_ok = i;
            cost_to[i] = cumulative;
        }
    }

    if (last_ok == 0) {
        if (result.stopped == WalkStop::kArrived) result.stopped = WalkStop::kBlocked;
        result.out_of_moves = OutOfMoves(stack, path, 0, result.stopped);
        return result;
    }

    int cost = cost_to[last_ok];
    const Step& dest = path[last_ok - 1];
    Mode movement = StackMode(game, stack);
    for (Army* army : stack) {
        army->PlaceAt(TilePos(dest.x, dest.y));
        army->SpendMoves(cost);
    }
    std::string change = SettleSea(game, stack, dest.x, dest.y, movement.mode, movement.at_sea);
    if (!change.empty()) {
        for (Army* army : stack) {
            if (army->IsAtSea() == (change == "to sea")) army->SetMoves(0);
        }
        result.stopped = WalkStop::kOutOfMoves;
        result.attack.reset();
    }

    if (game.GetMap().GameOptions().hidden_map != 0 && side != kNone) {
        bool flying = ModeOf(game, stack) == kFlying;
        int found = 0;
        for (int i = 0; i < static_cast<int>(path.size()); i++) {
            if (i == last_ok - 1) break;
            found += game::Reveal(game, side, path[i].x, path[i].y, flying);
        }
        found += game::Reveal(game, side, dest.x, dest.y, flying);
        if (found > 0) game.GetPathfinder().Invalidate();
    }

    result.steps = last_ok;
    result.spent = cost;
    for (int i = 0; i < last_ok; i++) result.walked.emplace_back(path[i].x, path[i].y);
    if (last_ok < static_cast<int>(path.size()) && result.stopped == WalkStop::kArrived)
        result.stopped = WalkStop::kBlocked;
    result.out_of_moves = !change.empty() || OutOfMoves(stack, path, last_ok, result.stopped);
    return result;
}

}  // namespace

bool IsPort(const Game& game, const City& city) {
    const Map& map = game.GetMap();
    int x = city.X(), y = city.Y();
    for (int direction : kPortTour) {
        x += kDirs[direction][0];
        y += kDirs[direction][1];
        if (!map.InBounds(x, y)) return false;
        int terrain = scn::TerrainAt(map, x, y);
        if (terrain == kBridge || terrain == kWater || terrain == kShore) return true;
    }
    return false;
}

std::optional<int> StepCost(int from_byte, int to_byte, int mode, bool woods, bool hills,
                            int penalty) {
    int cost = to_byte % 8;
    bool to_water = Has(to_byte, kWaterF), from_water = Has(from_byte, kWaterF);
    bool to_cross = Has(to_byte, kCrossF);
    if (mode == kFlying) {
        if (cost == 0 && Has(to_byte, kCityF)) return std::nullopt;
        if (to_water && !to_cross) return 2;
        if (cost == 0 || cost > 2) return 2;
        return cost;
    }
    if (mode == kBoat) {
        if (!to_water && !Has(to_byte, kCityF)) return std::nullopt;
        if (cost == 0) return std::nullopt;
        return cost;
    }
    if (cost == 0) return std::nullopt;
    if (to_water != from_water && !to_cross && !Has(from_byte, kCrossF)) return std::nullopt;
    if (cost > 2) {
        if (woods && Has(to_byte, kForestF)) cost = 2;
        if (hills && Has(to_byte, kHillsF)) cost = 2;
    }
    if ((!from_water || Has(from_byte, kCrossF)) && to_water && !to_cross) cost += penalty;
    return cost;
}

int WalkCost(int byte, int mode, bool woods, bool hills) {
    int cost = byte % 8;
    if (mode == kFlying) {
        if (Has(byte, kWaterF) && !Has(byte, kCrossF)) return 2;
        if (cost == 0 || cost > 2) return 2;
        return cost;
    }
    if (mode == kLand && cost > 2) {
        if (woods && Has(byte, kForestF)) cost = 2;
        if (hills && Has(byte, kHillsF)) cost = 2;
    }
    return cost;
}

bool CrossesShore(int byte, bool at_sea) {
    if (Has(byte, kCrossF)) return false;
    return Has(byte, kWaterF) != at_sea;
}

int StackMoves(const Stack& stack) {
    if (stack.empty()) return 0;
    int least = INT_MAX;
    for (Army* army : stack) least = std::min(least, army->Moves());
    return least;
}

int Direction(int from_x, int from_y, int to_x, int to_y) {
    if (from_x == to_x && from_y == to_y) return -1;
    if (from_x == to_x) return to_y < from_y ? 0 : 4;
    if (from_y == to_y) return to_x < from_x ? 6 : 2;
    if (from_x <= to_x) return to_y < from_y ? 1 : 3;
    return to_y < from_y ? 7 : 5;
}

int Distance(int from_x, int from_y, int to_x, int to_y) {
    double delta_x = from_x - to_x, delta_y = from_y - to_y;
    return static_cast<int>(std::floor(std::sqrt(delta_x * delta_x + delta_y * delta_y)));
}

std::string SettleSea(Game& game, const Stack& stack, int x, int y, int mode, bool was_at_sea) {
    if (mode != kLand) return "";
    int terrain = scn::TerrainAt(game.GetMap(), x, y);
    if (!was_at_sea) {
        if (terrain != kWater && terrain != kShore) return "";
        bool flier = false, walker = false;
        for (Army* army : stack) {
            if (game.ArmyTypes().ById(army->Type())->flies)
                flier = true;
            else if (!army->Hero())
                walker = true;
        }
        if (walker) flier = false;
        std::string change;
        for (Army* army : stack) {
            if (!game.ArmyTypes().ById(army->Type())->flies && (!army->Hero() || !flier)) {
                army->BoardShip();
                change = "to sea";
            }
        }
        return change;
    }
    if (terrain != kWater && terrain != kShore && terrain != kBridge) {
        for (Army* army : stack) army->ComeAshore();
        return "ashore";
    }
    return "";
}

std::string_view WalkStopName(WalkStop stop) {
    switch (stop) {
    case WalkStop::kNone:
        return "";
    case WalkStop::kArrived:
        return "arrived";
    case WalkStop::kOutOfMoves:
        return "out of moves";
    case WalkStop::kAttack:
        return "attack";
    case WalkStop::kAtPeace:
        return "at peace";
    case WalkStop::kBlocked:
        return "blocked";
    case WalkStop::kNoRoute:
        return "no route";
    }
    return "";
}

std::vector<int>& Grid(Game& game, int side_index) {
    return game.GetPathfinder().GridFor(game, side_index);
}

void Invalidate(Game& game) {
    game.GetPathfinder().Invalidate();
}

std::optional<Path> FindPath(Game& game, const Stack& stack, int start_x, int start_y, int dest_x,
                             int dest_y) {
    return game.GetPathfinder().FindPath(game, stack, start_x, start_y, dest_x, dest_y);
}

std::optional<Path> FindPathClear(Game& game, const Stack& stack, int start_x, int start_y,
                                  int dest_x, int dest_y) {
    return game.GetPathfinder().FindPathClear(game, stack, start_x, start_y, dest_x, dest_y);
}

std::optional<Path> FindPathNoCache(Game& game, const Stack& stack, int start_x, int start_y,
                                    int dest_x, int dest_y) {
    return game.GetPathfinder().FindPathNoCache(game, stack, start_x, start_y, dest_x, dest_y);
}

int PathMoveCost(Game& game, const Stack& stack, const Path& path) {
    if (path.empty()) return 0;
    int side = stack.empty() ? kNone : stack[0]->OwnerIndex();
    Mode movement = StackMode(game, stack);
    const std::vector<int>& grid = Grid(game, side);
    int width = game.GetMap().Width();
    int total = 0;
    bool at_sea = movement.at_sea;
    for (const Step& step : path) {
        int byte = grid[static_cast<size_t>(step.y) * width + step.x];
        total += WalkCost(byte, movement.mode, movement.woods, movement.hills);
        if (movement.mode == kLand && CrossesShore(byte, at_sea)) {
            total += kWaterPenalty;
            at_sea = !at_sea;
        }
    }
    return total;
}

std::optional<Preview> PreviewRoute(Game& game, const Stack& stack, int x, int y) {
    return game.GetPathfinder().PreviewRoute(game, stack, x, y);
}

std::vector<int> Flood(Game& game, int grid_side, int x, int y, int range, int mode, bool woods,
                       bool hills, int fog_side) {
    return game.GetPathfinder().Flood(game, grid_side, x, y, range, mode, woods, hills, fog_side);
}

std::vector<int> FloodClear(Game& game, int grid_side, int x, int y, int range, int mode,
                            bool woods, bool hills, int fog_side) {
    return game.GetPathfinder().FloodClear(game, grid_side, x, y, range, mode, woods, hills,
                                           fog_side);
}

WalkResult Walk(Game& game, const Stack& stack, const Path& path) {
    return game.GetPathfinder().Walk(game, stack, path);
}

WalkResult MoveTo(Game& game, const Stack& stack, int x, int y) {
    return game.GetPathfinder().MoveTo(game, stack, x, y);
}

}  // namespace move

std::optional<move::Preview> Pathfinder::PreviewRoute(Game& game, const move::Stack& stack, int x,
                                                      int y) {
    if (stack.empty()) return std::nullopt;
    auto path = FindPath(game, stack, stack[0]->X(), stack[0]->Y(), x, y);
    if (!path || path->empty()) return std::nullopt;
    int left = move::StackMoves(stack);
    int reach = 0;
    for (size_t i = 0; i < path->size(); i++) {
        const move::Step& step = (*path)[i];
        if (step.cost > left) break;
        left -= step.cost;
        reach = static_cast<int>(i) + 1;
    }
    return move::Preview{*path, reach};
}

move::WalkResult Pathfinder::Walk(Game& game, const move::Stack& stack, const move::Path& path) {
    game::TidyTowers(game);
    move::WalkResult result = move::WalkPath(game, stack, path);
    game::TidyTowers(game);
    return result;
}

move::WalkResult Pathfinder::MoveTo(Game& game, const move::Stack& stack, int x, int y) {
    move::WalkResult result;
    if (stack.empty()) {
        result.stopped = move::WalkStop::kBlocked;
        return result;
    }
    auto path = FindPath(game, stack, stack[0]->X(), stack[0]->Y(), x, y);
    if (!path) {
        result.stopped = move::WalkStop::kNoRoute;
        return result;
    }
    if (path->empty()) {
        result.stopped = move::WalkStop::kArrived;
        return result;
    }
    return Walk(game, stack, *path);
}

}  // namespace w2
