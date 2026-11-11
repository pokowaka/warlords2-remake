#include "warlords/ai/moves.hpp"

#include <algorithm>
#include <cstdlib>

#include "util/util.hpp"
#include "warlords/ai/cities.hpp"
#include "warlords/ai/diplomacy.hpp"
#include "warlords/game.hpp"
#include "warlords/move.hpp"
#include "warlords/quest.hpp"
#include "warlords/scn.hpp"
#include "warlords/site.hpp"

namespace w2::ai::moves {

using namespace core;

/** @brief The city with the given index, or nullptr for kNone. */
static City* CityOrNull(Game& game, int index) {
    return index != kNone ? game.GetMap().CityAt(index) : nullptr;
}

/** @brief Whether the army stands on a city that has not been razed. */
static bool OnCityTile(const Game& game, const Army* army) {
    City* city = CityAt(game, army->X(), army->Y());
    return city && Standing(*city);
}

/**
 * @brief The computer's visit to a sage.
 *
 * With the map hidden, the sage shows the land round the unseen neutral
 * city next to the side that has the most unseen neutral cities within 20
 * tiles (more than three), the one farthest from the side's own cities on a
 * tie; otherwise the side takes the gold.
 */
static void Sage(Game& game, Side& side) {
    AIData& data = Data(game, side);
    int gold = game.GetRng().Dice(3, 500, 500);
    City* best = nullptr;
    int best_count = -1, best_distance = 0;
    Map& map = game.GetMap();
    for (int i = map.CityCount() - 1; i >= 0; i--) {
        City& city = map.Cities()[i];
        if (city.IsNeutral() && Cflag(data, city, kCfUnseen)) {
            bool next_to_us = false;
            for (City* neighbour : FindNeighbours(game, city).first)
                if (neighbour->OwnerIndex() == side.Index()) next_to_us = true;
            if (next_to_us) {
                int count = 0, own_distance = 1000;
                for (auto& other : map.Cities()) {
                    if (other.IsNeutral()) {
                        if (Cflag(data, other, kCfUnseen) &&
                            Dist(city.X(), city.Y(), other.X(), other.Y()) < 20)
                            count++;
                    } else if (other.OwnerIndex() == side.Index()) {
                        int distance = Dist(city.X(), city.Y(), other.X(), other.Y());
                        if (distance < own_distance) own_distance = distance;
                    }
                }
                if (count > 3 &&
                    (best_count < count || (count == best_count && best_distance < own_distance))) {
                    best = &city;
                    best_count = count;
                    best_distance = own_distance;
                }
            }
        }
    }
    if (map.GameOptions().hidden_map == 0 || !best) {
        side.AddGold(gold);
        return;
    }
    // The map is shown round a point up to five tiles off the chosen city.
    int x = std::max(0, best->X() + game.GetRng().Dice(1, 11, -6));
    int y = std::max(0, best->Y() + game.GetRng().Dice(1, 11, -6));
    x = std::min(x, map.Width() - 1);
    y = std::min(y, map.Height() - 1);
    site::SageMap(game, side, x, y);
    move::Invalidate(game);
}

int SearchSite(Game& game, Sel* sel, Army* hero, Site& place) {
    if (!(hero && Alive(game, hero) && hero->X() == place.x && hero->Y() == place.y &&
          hero->Moves() != 0))
        return 0;
    Map& map = game.GetMap();
    Side& side = *map.SideAt(hero->OwnerIndex());
    if (SiteOpen(place)) {
        std::vector<Army*> stack = sel ? sel->armies : std::vector<Army*>{hero};
        auto result = site::Search(game, stack, place.x, place.y, false);
        if (result && result->kind == "sage") Sage(game, side);
        // At a temple the computer takes a quest, by its own rules (the
        // computer's variant of the quest assignment).
        if (place.content == site::kTemple && map.GameOptions().quests != 0 && !side.HasQuest() &&
            IsHero(hero)) {
            quest::Assign(game, side, hero, true, move::ModeOf(game, stack) == move::kFlying);
        }
        if (!IsHero(hero)) return 0;
        for (Army* member : stack) member->SetDone(false);
        if (map.GameOptions().hidden_map != 0 && place.content == site::kAllies) {
            for (Army* ally : OnTile(game, side.Index(), place.x, place.y)) {
                if (OrderOf(game, ally).order == 0 && Magical(game, ally)) {
                    MutableOrderOf(game, ally).explore = true;
                    Explore(game, side, ally, nullptr);
                }
            }
        }
    }
    return 2;
}

int Party(Game& game, Side& side, std::vector<Army*> list) {
    Army* hero = nullptr;
    Army* flier = nullptr;
    for (Army* army : list) {
        if (!hero && IsHero(army)) hero = army;
        if (!flier && Flies(game, army)) flier = army;
    }
    if (!hero) return 0;
    if (!flier) return 1;
    AIData& data = Data(game, side);
    list = {hero, flier};
    ClearOrder(game, hero);
    ClearOrder(game, flier);
    // A near site (under 15 tiles) scores 215 less its distance, a far one
    // (under 40) 90 less; a candidate replaces the best only if it scores more.
    int score = -1;
    Site* best_site = nullptr;
    City* best_city = nullptr;
    Map& map = game.GetMap();
    for (int i = map.SiteCount() - 1; i >= 0; i--) {
        Site& candidate = map.Sites()[i];
        bool taken = false;
        for (Army* other : Armies(game, side.Index())) {
            const ClassicArmyOrder& other_ord = OrderOf(game, other);
            if (other != hero && IsHero(other) && other_ord.order == kOrderSite &&
                other_ord.dest == candidate.index)
                taken = true;
        }
        if (!taken && Explored(game, side.Index(), candidate.x, candidate.y) &&
            candidate.content != site::kTemple && !candidate.searched) {
            int distance = Dist(hero->X(), hero->Y(), candidate.x, candidate.y);
            int base = kNone;
            if (distance < 15 && score < 215 - distance)
                base = 215;
            else if (!(distance > 39 || 90 - distance <= score))
                base = 90;
            if (base != kNone) {
                score = base - distance;
                best_site = &candidate;
            }
        }
    }
    // Own cities compete on the same score (115 near, 40 far); a rally city
    // counts as 80 tiles nearer.
    for (int i = map.CityCount() - 1; i >= 0; i--) {
        City& city = map.Cities()[i];
        if (city.OwnerIndex() == side.Index()) {
            int distance = Dist(hero->X(), hero->Y(), city.X(), city.Y());
            if (Role(data, city) == kRally) distance -= 80;
            int base = kNone;
            if (distance < 15 && score < 115 - distance)
                base = 115;
            else if (!(distance > 39 || 40 - distance <= score))
                base = 40;
            if (base != kNone) {
                score = base - distance;
                best_city = &city;
            }
        }
    }
    if (best_site) {
        auto sel = Order(game, list, kOrderSite, best_site->index, 0x100);
        if (sel) MoveTo(game, *sel, sel->leader->Target()->x, sel->leader->Target()->y);
        return SearchSite(game, sel ? &*sel : nullptr, hero, *best_site);
    }
    if (best_city && Dist(hero->X(), hero->Y(), best_city->X(), best_city->Y()) > 2) {
        auto sel = Order(game, list, kOrderCity, best_city->Index(), 0);
        if (sel) MoveTo(game, *sel, sel->leader->Target()->x, sel->leader->Target()->y);
    }
    return 0;
}

namespace {

/** @brief What an explorer's look round found: where to walk, and what lies near. */
struct ScanResult {
    bool found = false;          ///< Whether an unseen tile worth walking to was found.
    int tx = kNone, ty = kNone;  ///< The unseen tile to walk to.
    int kind = 0;                ///< What lies near: 0 nothing, 1 a city to try, 2 a site.
    int ex = kNone, ey = kNone;  ///< Where that city or site is.
};

/**
 * @brief Looks over the 10 x 10 tiles round an explorer for where to go.
 *
 * Notes a foreign city within reach (else a site) next to seen land, and
 * picks the unseen tile that scores best: far from home and from the explorer,
 * with many unseen tiles round it, and either wet (for a coastal explorer) or
 * dry land and roads (for one on land), plus 1d10.
 */
ScanResult Scan(Game& game, Side& side, Army* explorer, const Sel& sel) {
    Map& map = game.GetMap();
    int home_x, home_y;
    if (City* home_city = CityOrNull(game, explorer->HomeCityIndex())) {
        home_x = home_city->X();
        home_y = home_city->Y();
    } else if (City* capital = side.CapitalCity(map)) {
        home_x = capital->X();
        home_y = capital->Y();
    } else {
        home_x = explorer->X();
        home_y = explorer->Y();
    }
    // An explorer at sea explores the coast; so does a non-hero in a port,
    // one time in four.
    bool coast = explorer->IsAtSea();
    if (game.GetRng().Dice(1, 4, -1) == 0 && OnCityTile(game, explorer) && !IsHero(explorer)) {
        City* city = CityAt(game, explorer->X(), explorer->Y());
        if (city && move::IsPort(game, *city)) coast = true;
    }
    bool flying = sel.mode == move::kFlying;
    bool crosses_hills = move::StackMode(game, sel.armies).hills;
    ScanResult out;
    int best_score = 0;
    auto explored_round = [&](int x, int y) {
        for (int around_x = x - 1; around_x <= x + 1; around_x++) {
            for (int around_y = y - 1; around_y <= y + 1; around_y++) {
                if (around_x >= 0 && around_y >= 0 && around_x < map.Width() &&
                    around_y < map.Height() && game::Seen(game, side.Index(), around_x, around_y))
                    return true;
            }
        }
        return false;
    };
    for (int x = explorer->X() - 5; x <= explorer->X() + 4; x++) {
        for (int y = explorer->Y() - 5; y <= explorer->Y() + 4; y++) {
            if (x >= 0 && y >= 0 && x < map.Width() && y < map.Height()) {
                int terrain = Terrain(game, x, y);
                City* city = CityAt(game, x, y);
                if (terrain == move::kCity && (!city || city->OwnerIndex() != side.Index())) {
                    // A flier counts every city as two tiles away.
                    int distance = flying ? 2 : Dist(explorer->X(), explorer->Y(), x, y);
                    if (distance < 4 &&
                        (!explorer->IsAtSea() || (city && move::IsPort(game, *city))) &&
                        explored_round(x, y)) {
                        out.kind = 1;
                        out.ex = x;
                        out.ey = y;
                    }
                }
                if (terrain == move::kSite && out.kind == 0 && !explorer->IsAtSea() &&
                    explored_round(x, y)) {
                    out.kind = 2;
                    out.ex = x;
                    out.ey = y;
                }
                bool road = scn::RoadAt(map, x, y) % 0x20 != 0;
                if (terrain != move::kCity &&
                    (flying || road || crosses_hills || terrain != move::kHills) &&
                    !game::Seen(game, side.Index(), x, y) &&
                    (flying || move::kCost[terrain] != 0)) {
                    int unseen = 0;
                    for (int around_x = x - 1; around_x <= x + 1; around_x++) {
                        for (int around_y = y - 1; around_y <= y + 1; around_y++) {
                            if (around_x >= 0 && around_y >= 0 && around_x < map.Width() &&
                                around_y < map.Height() &&
                                !game::Seen(game, side.Index(), around_x, around_y))
                                unseen++;
                        }
                    }
                    if (unseen > 1) {
                        int bonus = 0;
                        bool wet = terrain == move::kWater || terrain == move::kShore;
                        if (coast) {
                            if (wet) bonus += 200;
                        } else if (!wet) {
                            if (terrain != move::kHills) bonus = 100;
                            if (road) bonus += 200;
                        }
                        int from_home = Dist(x, y, home_x, home_y);
                        int score = game.GetRng().Dice(1, 10, 0) +
                                    Dist(x, y, explorer->X(), explorer->Y()) * 2 + from_home +
                                    unseen * 10 + bonus;
                        if (coast && bonus != 0) score += std::min(from_home, 50) * 10;
                        if (best_score < score) {
                            out.tx = x;
                            out.ty = y;
                            out.found = true;
                            best_score = score;
                        }
                    }
                }
            }
        }
    }
    return out;
}

/**
 * @brief A flier's fallback target: the nearest unseen city, give or take 1d10.
 * @return The tile one up and left of that city (that is where the flier
 *         aims), or {kNone, kNone} when no unseen city is left.
 */
std::pair<int, int> UnseenCity(Game& game, Side& side, Army* flier) {
    AIData& data = Data(game, side);
    Map& map = game.GetMap();
    if (IsFairClassicActive(game, side.Index())) {
        int best_x = kNone;
        int best_y = kNone;
        int best_dist = kNone;
        const int min_x = std::max(1, map.ActiveX0());
        const int max_x = std::min(map.Width() - 2, map.ActiveX1());
        const int min_y = std::max(1, map.ActiveY0());
        const int max_y = std::min(map.Height() - 2, map.ActiveY1());
        for (int ty = min_y; ty <= max_y; ty += 3) {
            for (int tx = min_x; tx <= max_x; tx += 3) {
                if (game::Seen(game, side.Index(), tx, ty)) continue;
                bool touches_seen = game::Seen(game, side.Index(), tx - 1, ty) ||
                                    game::Seen(game, side.Index(), tx + 1, ty) ||
                                    game::Seen(game, side.Index(), tx, ty - 1) ||
                                    game::Seen(game, side.Index(), tx, ty + 1);
                if (!touches_seen) continue;
                int dist = Dist(tx, ty, flier->X(), flier->Y()) + game.GetRng().Dice(1, 10, 0);
                if (best_dist == kNone || dist < best_dist) {
                    best_dist = dist;
                    best_x = tx;
                    best_y = ty;
                }
            }
        }
        return {best_x, best_y};
    }
    City* best = nullptr;
    int best_distance = kNone;
    for (int i = map.CityCount() - 1; i >= 0; i--) {
        City& city = map.Cities()[i];
        if (Standing(city) && Cflag(data, city, kCfUnseen)) {
            int distance =
                    Dist(city.X(), city.Y(), flier->X(), flier->Y()) + game.GetRng().Dice(1, 10, 0);
            if (best_distance == kNone || distance < best_distance) {
                best = &city;
                best_distance = distance;
            }
        }
    }
    if (best) return {best->X() - 1, best->Y() - 1};
    return {kNone, kNone};
}

/**
 * @brief An explorer next to a city goes for it, if it may and the odds are good.
 *
 * It needs 75% odds, or less for a weak non-hero early on (50% while few
 * cities are neutral, 40% while the side owns few cities).
 * @param near_x, near_y Where the scan saw the city.
 * @return True if the explorer went for the city.
 */
bool TryCity(Game& game, Side& side, Army* explorer, int near_x, int near_y) {
    AIData& data = Data(game, side);
    City* pick = nullptr;
    Map& map = game.GetMap();
    for (int i = map.CityCount() - 1; i >= 0; i--) {
        City& city = map.Cities()[i];
        if (Standing(city) && city.OwnerIndex() != side.Index() &&
            data.quest_city != city.Index() && Dist(city.X(), city.Y(), near_x, near_y) <= 2 &&
            diplo::CanAttack(game, side, city)) {
            if (Cflag(data, city, kCfUnseen)) cities::ClearUnseen(game, side);
            if (!Cflag(data, city, kCfUnseen)) {
                pick = &city;
                break;
            }
        }
    }
    if (!pick) return false;
    auto sel = Order(game, {explorer}, kOrderRoam, 0, 0x20);
    if (!sel) return false;
    int odds = Odds(game, *sel, pick->X(), pick->Y());
    int need = 75;
    if (!IsHero(explorer) && explorer->Strength() < 4) {
        if (data.neutral < map.CityCount() / 10)
            need = 50;
        else if (data.own <= std::min(game.Turn() / 2, 10))
            need = 40;
    }
    if (odds < need) return false;
    MutableOrderOf(game, explorer).explore = false;
    sel = Order(game, {explorer}, kOrderCity, pick->Index(), 0x80);
    if (sel) MoveTo(game, *sel, sel->leader->Target()->x, sel->leader->Target()->y);
    return true;
}

/**
 * @brief An explorer next to a site visits it.
 *
 * A hero visits any site, anything else only a temple; nobody goes back to
 * a temple that has blessed it.
 * @return True if the explorer went (or tried to go) to the site.
 */
bool TrySite(Game& game, Side& side, Army* explorer, int site_x, int site_y) {
    Site* place = SiteAt(game, site_x, site_y);
    if (!place || !SiteOpen(*place) || explorer->IsAtSea()) return false;
    bool temple = place->content == site::kTemple;
    if (temple && place->temple_index != kNone && explorer->HasBlessing(place->temple_index))
        return false;
    if (!(IsHero(explorer) || temple)) return false;
    auto there = game::ArmiesAt(game, site_x, site_y);
    if (!there.empty() && there[0]->OwnerIndex() != side.Index()) return false;
    auto sel = Order(game, {explorer}, kOrderRoam, 0, 0);
    if (!sel) return false;
    sel->leader->SetTarget(TilePos(site_x, site_y));
    int code = MoveTo(game, *sel, site_x, site_y);
    if (code == 1) return false;  // the walk found no path
    if (Alive(game, explorer)) {
        if (explorer->X() == site_x && explorer->Y() == site_y) {
            if (IsHero(explorer))
                SearchSite(game, &*sel, explorer, *place);
            else
                site::Search(game, {explorer}, site_x, site_y, false);
            if (place->content == site::kAllies) {
                for (Army* ally : OnTile(game, side.Index(), site_x, site_y)) {
                    if (OrderOf(game, ally).order == 0 && Magical(game, ally))
                        MutableOrderOf(game, ally).explore = true;
                }
            }
        } else {
            // Stopped at sea short of the temple: count it as blessed there so
            // the explorer does not try it again.
            if (temple && explorer->IsAtSea() && place->temple_index != kNone)
                explorer->AddBlessing(place->temple_index);
            explorer->SetDone(true);
        }
        MutableOrderOf(game, explorer).explore = true;
        ClearOrder(game, explorer);
        explorer->ClearTarget();
    }
    return true;
}

/**
 * @brief Walks an explorer's stack to (x, y).
 *
 * An explorer that finds no path stops exploring; either way it is done for
 * the turn.
 */
void WalkTo(Game& game, Army* explorer, int x, int y) {
    explorer->SetTarget(TilePos(x, y));
    Sel sel = SelectStackOf(game, explorer);
    int code = MoveTo(game, sel, x, y);
    if (Alive(game, explorer)) {
        if (code == 1) {  // the walk found no path
            MutableOrderOf(game, explorer).explore = false;
            ClearOrder(game, explorer);
        }
        explorer->SetDone(true);
        explorer->ClearTarget();
        explorer->SetGroup(0);
    }
}

/**
 * @brief One step of an explorer.
 *
 * It tries a city or site the scan saw near it; otherwise it walks to the
 * scan's unseen tile, a flier's unseen city, or the home city's open neutral
 * neighbour, and stops exploring when there is none of these.
 */
void ExploreStep(Game& game, Side& side, Army* explorer, City* home) {
    Sel sel = Select(game, {explorer});
    ScanResult scan = Scan(game, side, explorer, sel);
    int target_x = scan.tx, target_y = scan.ty;
    if (!scan.found) {
        if (Flies(game, explorer)) std::tie(target_x, target_y) = UnseenCity(game, side, explorer);
        if (target_x == kNone) {
            std::map<int, int> no_headings;
            City* neutral = home ? cities::OpenNeutral(game, side, *home, no_headings) : nullptr;
            if (!neutral) {
                ClearOrder(game, explorer);
                MutableOrderOf(game, explorer).explore = false;
                explorer->ClearTarget();
                explorer->SetGroup(0);
                return;
            }
            target_x = neutral->X();
            target_y = neutral->Y();
        }
    } else if (scan.kind == 1) {
        if (TryCity(game, side, explorer, scan.ex, scan.ey)) return;
    } else if (scan.kind == 2) {
        if (TrySite(game, side, explorer, scan.ex, scan.ey)) return;
    }
    WalkTo(game, explorer, target_x, target_y);
}

}  // namespace

void Explore(Game& game, Side& side, Army* army, City* home) {
    if (!Alive(game, army)) return;
    Order(game, {army}, kOrderRoam, 0, 0x20);
    int steps = army->Moves() / 5 + 1;
    // Step until the army stops, runs low on moves or steps, or stops exploring.
    for (;;) {
        if (!Alive(game, army) || army->Moves() < 2) return;
        int start_x = army->X(), start_y = army->Y();
        ExploreStep(game, side, army, home);
        if (!Alive(game, army) || (army->X() == start_x && army->Y() == start_y)) return;
        if (army->Moves() < 4) return;
        if (steps == 0) return;
        if (!OrderOf(game, army).explore) return;
        steps--;
    }
}

void Search(Game& game, Side& side) {
    AIData& data = Data(game, side);
    bool rally = false;
    for (auto& city : game.GetMap().Cities()) {
        if (city.OwnerIndex() == side.Index() && Role(data, city) == kRally) rally = true;
    }
    data.searchers = 0;
    data.explorers = 0;
    // Pass 0 walks the heroes, pass 1 everything else. Heroes stop exploring
    // when the side has a rally city, weak fliers when there are enough.
    for (int pass = 0; pass <= 1; pass++) {
        for (Army* army : Armies(game, side.Index())) {
            if (Alive(game, army) && !army->InTransit() && OrderOf(game, army).explore &&
                ((pass == 0) == IsHero(army))) {
                if ((IsHero(army) && rally) ||
                    (Flies(game, army) && data.explorers > 7 && army->Strength() < 4)) {
                    ClearOrder(game, army);
                    MutableOrderOf(game, army).explore = false;
                } else {
                    Explore(game, side, army, nullptr);
                    if (Alive(game, army) && army->OwnerIndex() == side.Index()) {
                        if (Flies(game, army))
                            data.explorers++;
                        else
                            data.searchers++;
                    }
                }
            }
        }
    }
}

void HeroParties(Game& game, Side& side) {
    for (Army* army : Armies(game, side.Index())) {
        if (Alive(game, army) && !army->InTransit() && OrderOf(game, army).party) {
            MutableOrderOf(game, army).party = false;
            if (IsHero(army)) {
                // Keep sending the hero's party (at most 20 times) while it
                // moves and finds more to do.
                bool going = true;
                int guard = 0;
                while (going && guard < 20) {
                    guard++;
                    if (!Alive(game, army)) break;
                    const ClassicArmyOrder& ord = OrderOf(game, army);
                    auto list = CollectOrdered(game, side.Index(), army->X(), army->Y(), ord.group,
                                               ord.order, 0);
                    if (list.empty() || army->Moves() < 3) break;
                    int start_x = army->X(), start_y = army->Y();
                    int result = Party(game, side, list);
                    if (result == 0 || result == 1) going = false;
                    if (Alive(game, army) && army->X() == start_x && army->Y() == start_y &&
                        army->MaxMoves() > army->Moves())
                        break;
                }
            }
        }
    }
}

/**
 * @brief Whether the stack's neutral target is still the thing to do.
 *
 * A stack whose target city is already the side's own is retargeted to the
 * nearest seen neutral neighbour it has good odds (over 74%) against; early
 * on a hidden map it goes exploring when there is none. A stack whose target
 * may no longer be attacked drops its order.
 * @return False when the stack should not walk on this turn.
 */
static bool NeutralStillOn(Game& game, Side& side, Sel& sel) {
    AIData& data = Data(game, side);
    Army* lead = sel.leader;
    City* dest = CityOrNull(game, OrderOf(game, lead).dest);
    if (!dest) return true;
    if (dest->OwnerIndex() == side.Index()) {
        City* here = CityAt(game, lead->X(), lead->Y());
        auto neighbours = here ? cities::NeutralNeighbours(game, side, *here)
                               : std::vector<cities::NeutralNeighbour>{};
        if (neighbours.empty()) neighbours = cities::NeutralNeighbours(game, side, *dest);
        // With the neutral-cities option off the odds count as 100.
        int odds = 100;
        City* best = nullptr;
        int best_distance = 1000;
        const Options& options = game.GetMap().GameOptions();
        for (int j = static_cast<int>(neighbours.size()) - 1; j >= 0; j--) {
            City* neighbour = neighbours[j].city;
            if (!Cflag(data, *neighbour, kCfUnseen)) {
                if (options.neutral_cities != 0)
                    odds = Odds(game, sel, neighbour->X(), neighbour->Y());
                int distance = Dist(lead->X(), lead->Y(), neighbour->X(), neighbour->Y());
                if (odds > 74 && distance < best_distance) {
                    best = neighbour;
                    best_distance = distance;
                }
            }
        }
        if (!best) {
            if (options.hidden_map != 0 && game.Turn() < 10) {
                std::vector<Army*> list = sel.armies;
                for (Army* army : list) Explore(game, side, army, nullptr);
                return false;
            }
            return true;
        }
        for (Army* army : sel.armies) {
            ClassicArmyOrder& ord = MutableOrderOf(game, army);
            ord.order = kOrderCity;
            ord.dest = best->Index();
            SetTarget(game, army, kOrderCity, best->Index());
        }
        return true;
    }
    if (!diplo::CanAttack(game, side, *dest)) {
        for (Army* army : sel.armies) {
            ClearOrder(game, army);
            army->ClearTarget();
        }
        return false;
    }
    return true;
}

void MoveAll(Game& game, Side& side) {
    AIData& data = Data(game, side);
    for (Army* army : Armies(game, side.Index())) {
        if (!army->InTransit()) MutableOrderOf(game, army).moved = army->Moves() < 2;
    }
    if (!data.cursor) {
        TilePos cap_pos = side.OriginCapitalPos();
        data.cursor = std::make_pair(cap_pos.x, cap_pos.y);
    }
    for (;;) {
        // The next stack is the one nearest the cursor; one right under it
        // goes last.
        Army* pick = nullptr;
        int pick_distance = kNone;
        for (Army* army : Armies(game, side.Index())) {
            if (!army->InTransit() && army->X() != kNone && !army->IsDone() &&
                !OrderOf(game, army).moved) {
                int distance = std::abs(army->X() - data.cursor->first) +
                               std::abs(army->Y() - data.cursor->second);
                if (distance == 0) distance = 9000;
                if (pick_distance == kNone || distance < pick_distance) {
                    pick = army;
                    pick_distance = distance;
                }
            }
        }
        if (!pick) break;
        data.cursor = std::make_pair(pick->X(), pick->Y());
        MutableOrderOf(game, pick).moved = true;
        if (pick->Target() && pick->Target()->x != kNone && pick->Target()->x >= 0 &&
            pick->Moves() > 1) {
            Sel sel = SelectStackOf(game, pick);
            Army* lead = sel.leader;
            const ClassicArmyOrder& lead_ord = OrderOf(game, lead);
            int order = lead_ord.order;
            City* dest = CityOrNull(game, lead_ord.dest);
            if (order == kOrderNone ||
                (order == kOrderCity &&
                 (!dest || lead_ord.dest == data.quest_city || !Standing(*dest)))) {
                for (Army* army : sel.armies) {
                    ClearOrder(game, army);
                    army->ClearTarget();
                    army->SetDone(true);
                }
            } else if (!lead_ord.neutral || NeutralStillOn(game, side, sel)) {
                const ClassicArmyOrder& cur_ord = OrderOf(game, lead);
                dest = CityOrNull(game, cur_ord.dest);
                // A stack bound for one of its own cities aims at the city's
                // tile (x + 1, y + 1).
                if (cur_ord.order == kOrderCity && dest && dest->OwnerIndex() == side.Index()) {
                    lead->SetTarget(TilePos(dest->X() + 1, dest->Y() + 1));
                }
                if (lead->Target() && Alive(game, lead)) {
                    MoveTo(game, sel, lead->Target()->x, lead->Target()->y);
                }
            }
        }
    }
}

/**
 * @brief The best city for an idle stack, by the flood's distance.
 *
 * Candidates are seen, standing cities, not the quest city, that are
 * neutral, the side's own, or held by a side it is at war with (state 2). An own city
 * costs 10 more (a rally city) or 30 more, and 100 more for a stack of more
 * than three; another city needs odds over 74%.
 * @param stack_size The number of armies in the stack.
 * @return The nearest candidate, or nullptr when none is within 1000.
 */
static City* IdleTarget(Game& game, Side& side, const Sel& sel, const Flood& flood,
                        int stack_size) {
    AIData& data = Data(game, side);
    City* best = nullptr;
    int best_distance = 1000;
    Map& map = game.GetMap();
    for (int i = map.CityCount() - 1; i >= 0; i--) {
        City& city = map.Cities()[i];
        int owner = Owner(city);
        if (Standing(city) && !Cflag(data, city, kCfUnseen) && data.quest_city != city.Index() &&
            (owner == kNeutral || owner == side.Index() || State(game, side.Index(), owner) == 2)) {
            auto [distance, near_x, near_y] = CityDistance(game, flood, city);
            (void)near_y;
            if (near_x != kNone) {
                if (owner == side.Index()) {
                    distance += Role(data, city) == kRally ? 10 : 30;
                    if (stack_size > 3) distance += 100;
                    if (distance < best_distance) {
                        best = &city;
                        best_distance = distance;
                    }
                } else if (Odds(game, sel, city.X(), city.Y()) > 74 && distance < best_distance) {
                    best = &city;
                    best_distance = distance;
                }
            }
        }
    }
    return best;
}

void SendIdle(Game& game, Side& side, Army* army) {
    int home_distance = 1000;
    for (auto& city : game.GetMap().Cities()) {
        if (city.OwnerIndex() == side.Index() && Standing(city)) {
            int distance = Dist(army->X(), army->Y(), city.X(), city.Y());
            if (distance < home_distance) home_distance = distance;
        }
    }
    bool weak = army->Strength() < 3 || army->MaxMoves() < 8;
    if (!Flies(game, army) && weak) {
        game::Disband(game, side, {army});
        return;
    }
    auto list = Collect(game, side.Index(), army->X(), army->Y(), 8);
    if (list.empty()) return;
    // A stack of fliers, heroes and others leaves the others behind.
    int fliers = 0, heroes = 0, others = 0;
    for (Army* member : list) {
        if (Flies(game, member))
            fliers++;
        else if (IsHero(member))
            heroes++;
        else
            others++;
    }
    if (fliers != 0 && heroes != 0 && others != 0) {
        std::vector<Army*> keep;
        for (Army* member : list)
            if (Flies(game, member) || IsHero(member)) keep.push_back(member);
        list = keep;
    }
    if (list.size() < 2 && weak) {
        game::Disband(game, side, {army});
        return;
    }
    Sel sel = Select(game, list);
    // A stack with a hero floods 50 rounds out, others ten per two strength
    // points, never more than 10 past the nearest own city.
    int range = sel.hero ? 50 : (army->Strength() / 2) * 10;
    range = std::min(range, home_distance + 10);
    Flood flood = FloodFrom(game, side.Index(), army->X(), army->Y(), range, &sel);
    City* target = IdleTarget(game, side, sel, flood, static_cast<int>(list.size()));
    if (!target) {
        game::Disband(game, side, list);
        return;
    }
    auto ordered = Order(game, list, kOrderCity, target->Index(), 0);
    if (ordered) MoveTo(game, *ordered, ordered->leader->Target()->x, ordered->leader->Target()->y);
}

void Rescue(Game& game, Side& side) {
    for (Army* army : Armies(game, side.Index())) {
        if (Alive(game, army) && !army->InTransit() && army->X() != kNone &&
            army->MaxMoves() <= army->Moves()) {
            // An army that has not moved (two moves over its maximum) whose
            // target city is gone: its stack drops that order.
            if (army->Moves() == army->MaxMoves() + 2 && !OnCityTile(game, army) &&
                OrderOf(game, army).order == kOrderCity) {
                int army_dest = OrderOf(game, army).dest;
                City* dest = CityOrNull(game, army_dest);
                if (!dest || !Standing(*dest)) {
                    int gone = army_dest;
                    for (Army* member : OnTile(game, side.Index(), army->X(), army->Y())) {
                        const ClassicArmyOrder& member_ord = OrderOf(game, member);
                        if (member_ord.order == kOrderCity && member_ord.dest == gone) {
                            ClearOrder(game, member);
                            MutableOrderOf(game, member).group = 0;
                        }
                    }
                }
            }
            if (Alive(game, army) && !OrderOf(game, army).explore &&
                OrderOf(game, army).order == 0 && !OnCityTile(game, army))
                SendIdle(game, side, army);
        }
    }
}

void LastRescue(Game& game, Side& side) {
    for (Army* army : Armies(game, side.Index())) {
        if (Alive(game, army) && !army->InTransit() && army->X() != kNone &&
            army->Moves() == army->MaxMoves() + 2 && !OnCityTile(game, army)) {
            ClassicArmyOrder& ord = MutableOrderOf(game, army);
            ord.explore = false;
            ord.neutral = false;
            ClearOrder(game, army);
            army->ClearTarget();
            SendIdle(game, side, army);
        }
    }
}

void Specials(Game& game, Side& side) {
    AIData& data = Data(game, side);
    for (City* city : cities::Own(game, side)) {
        if (Cflag(data, *city, kCfHero) && Role(data, *city) != kRally) {
            // The party forms from the armies on the city's tile (x + 1, y);
            // a hero without a flier makes the city an explorer base.
            auto list = CollectOrdered(game, side.Index(), city->X() + 1, city->Y(), 0, 0, 0);
            if (!list.empty() && Party(game, side, list) == 1 && !cities::Building(*city))
                SetRole(data, *city, kExplorer);
        }
    }
}

}  // namespace w2::ai::moves
