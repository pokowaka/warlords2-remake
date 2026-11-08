/**
 * @file
 * @brief Classic random map feature placement: cities, sites, terrain transitions, bridges,
 * crossings, roads.
 */
#include "warlords/worldgen/randommap_impl.hpp"

namespace w2::randommap {

// --- cities and sites ----------------------------------------------------

/**
 * @brief Places a city's 2x2 footprint clear of water, hills, and other
 *        cities by a tile; on the coast if one of four tries finds it.
 */
void Generator::PlaceCity() {
    int x = 0, y = 0, tries = 0;
    for (int step = 0; step < kSafety; step++) {
        x = Dice(1, 102, 5);
        y = Dice(1, 146, 5);
        bool valid = true;
        for (auto& cell : kFootprint) {
            int dx = cell[0], dy = cell[1];
            int terrain = At(x + dx, y + dy);
            if (terrain == kShore || terrain == kWater || terrain == kCity ||
                terrain == kMountains || terrain == kHills)
                valid = false;
            if (Near(x + dx, y + dy, kCity) || Near(x + dx + 1, y + dy + 1, kCity) ||
                Near(x + dx - 1, y + dy - 1, kCity))
                valid = false;
        }
        if (!valid) continue;
        if (tries >= 4) break;
        tries++;
        if (Near(x, y, kShore) || Near(x + 1, y, kShore) || Near(x, y + 1, kShore) ||
            Near(x + 1, y + 1, kShore))
            break;
    }
    int city_idx = CityCount();
    Ws16(Crec(city_idx), x);
    Ws16(Crec(city_idx) + 2, y);
    SetCityCount(city_idx + 1);
    for (auto& cell : kFootprint) Put(x + cell[0], y + cell[1], kCity);
}

/** @brief Places all cities on the terrain grid. */
void Generator::Cities() {
    SetCityCount(0);
    for (int i = R16(0xa84); i < R16(kPCities); i++) PlaceCity();
}

/**
 * @brief Finds a spot for a site, cycling through the map's 16 cells in turn.
 */
Point Generator::SiteSpot() {
    if (site_cell == -1) site_cell = Dice(1, 16, -1);
    int cell_x = site_cell % 4, cell_y = site_cell / 4;
    int x = 0, y = 0;
    bool found = false;
    for (int i = 0; i < 50 && !found; i++) {
        x = Dice(1, 20, cell_x * kMapW / 4 + 3);
        y = Dice(1, 31, cell_y * kMapH / 4 + 3);
        found = At(x, y) == kPlain && !Near(x, y, kCity) && !Near(x, y, kSite) &&
                !Near(x - 3, y, kSite) && !Near(x + 3, y, kSite) && !Near(x, y - 3, kSite) &&
                !Near(x, y + 3, kSite);
    }
    for (int i = 0; i < 50 && !found; i++) {
        x = Dice(1, 102, 5);
        y = Dice(1, 146, 5);
        found = At(x, y) == kPlain && !Near(x, y, kCity) && !Near(x, y, kSite);
    }
    if (!found) {
        x = Dice(1, 102, 5);
        y = Dice(1, 146, 5);
        for (int i = 0; i < 50 && !found; i++) {
            x = Dice(1, 102, 5);
            y = Dice(1, 146, 5);
            found = At(x, y) != kCity && At(x, y) != kSite;
        }
    }
    site_cell = (site_cell + 1) % 16;
    return Point{x, y};
}

/** @brief Initialises the 40 site records as unplaced. */
void Generator::SitesBegin() {
    Ws16(kSSiteCount, kNSites);
    for (int i = 0; i < kNSites; i++) {
        Ws16(kSSites + kSiteStride * i + 2, -1);
        Ws16(kSSites + kSiteStride * i, -1);
    }
}

/** @brief Writes site `site_idx` at `spot` with its generated name and description. */
void Generator::SiteAt(int site_idx, Point spot) {
    int offset = kSSites + kSiteStride * site_idx;
    Ws16(offset, spot.x);
    Ws16(offset + 2, spot.y);
    Put(spot.x, spot.y, kSite);
    std::string name, text;
    if (scn[offset + 24] == 1) {
        int word_idx = Dice(1, 10, -1);
        name = Str(0x1000 + 10 * word_idx, 10) + " Temple";
        text = "#%03d|The %s can bless your|armies or give you|quests|\n";
    } else {
        int fmt_idx = Dice(1, R16(kSiteFormats), -1);
        int idx1 = Dice(1, 10, -1);
        std::string suffix = Str(0xf9c + 10 * idx1, 10);
        int idx2 = Dice(1, 10, -1);
        std::string word = Str(0xf38 + 10 * idx2, 10) + suffix;
        name = Format(Str(kSiteFormats + 2 + 20 * fmt_idx, 20), word);
        text = "#%03d|%s is|inhabited by monsters and|full of treasure!|\n";
    }
    Wstr(offset + 4, name);
    spc += Format(text, site_idx, name);
}

// --- terrain to tiles ----------------------------------------------------

/**
 * @brief Assigns initial tile indices for every terrain cell, castle tiles
 *        for cities, and temple tiles for the first eight sites.
 */
void Generator::ToTiles() {
    for (int y = 0; y < kMapH; y++) {
        for (int x = 0; x < kMapW; x++) {
            int terrain = At(x, y);
            if (terrain == kCity && At(x, y - 1) != kCity && At(x - 1, y) != kCity && x > 0 &&
                y > 0) {
                int castle = R16(kTiles + 64 * kCity) & 0xff;
                SetTile(x, y, castle);
                SetTile(x + 1, y, castle + 1);
                SetTile(x, y + 1, castle + 16);
                SetTile(x + 1, y + 1, castle + 17);
            } else if (terrain == kSite) {
                SetTile(x, y, TileFor(kSite, 0, x, y));
            } else if (terrain != kCity) {
                SetTile(x, y, TileFor(terrain, 0, x, y));
            }
        }
    }
    for (int i = 0; i < 8; i++) {
        int offset = kSSites + kSiteStride * i;
        SetTile(S16(offset), S16(offset + 2), R16(kTiles + 64 * kSite + 4) & 0xff);
    }
}

/**
 * @brief Builds an 8-bit neighbour mask (bit i for neighbour i), counting
 *        the map's outer edge as matching.
 */
int Generator::Mask(int x, int y, const std::function<bool(int)>& test) const {
    int mask = 0;
    for (int i = 0; i < 8; i++) {
        int adj_x = x + Nx(i), adj_y = y + Ny(i);
        bool off = adj_x < 0 || adj_y < 0 || adj_x >= kMapW || adj_y >= kMapH;
        if (off || test(At(adj_x, adj_y))) mask |= 1 << i;
    }
    return mask;
}

/** @brief Calls `func(x, y)` for every tile whose terrain equals `terrain`. */

/**
 * @brief Selects transition tile variants that join each terrain type to
 *        its neighbours, replacing unrepresentable shapes with fallback terrain.
 */
void Generator::Shapes() {
    for (int y = 0; y < kMapH; y++) {
        for (int x = 0; x < kMapW; x++) {
            int terrain = At(x, y);
            if (terrain != kWater && terrain != kShore) continue;
            int variant = Shape(Mask(x, y, [](int adj) {
                return adj == kWater || adj == kShore || adj == kBridge;
            }));
            if (variant < 0) {
                Put(x, y, kMarsh);
                SetTile(x, y, TileFor(kMarsh, 0, x, y));
                MarshAround(x, y);
                tiles[y * kMapW + x] &= 0x7fff;
            } else {
                Put(x, y, kWater);
                SetTile(x, y, TileFor(kWater, variant, x, y));
            }
        }
    }
    Each(kForest, [&](int x, int y) {
        int variant = Shape(Mask(x, y, [](int adj) { return adj == kForest; }));
        if (variant >= 0) {
            SetTile(x, y, TileFor(kForest, variant, x, y));
            Put(x, y, kForest);
        } else if (Dice(1, 10, 0) < 6) {
            SetTile(x, y, TileFor(kPlain, 0, x, y));
            Put(x, y, kPlain);
        } else {
            SetTile(x, y, TileFor(kForest, 13, x, y));
        }
    });
    Each(kMountains, [&](int x, int y) {
        int variant = Shape(Mask(x, y, [](int adj) { return adj == kMountains; }));
        if (variant >= 0) {
            SetTile(x, y, TileFor(kMountains, variant, x, y));
            Put(x, y, kMountains);
        } else {
            SetTile(x, y, TileFor(kHills, 0, x, y));
            Put(x, y, kHills);
        }
    });
    Each(kHills, [&](int x, int y) {
        int variant = Shape(Mask(x, y, [](int adj) { return adj == kMountains || adj == kHills; }));
        if (variant >= 0) {
            SetTile(x, y, TileFor(kHills, variant, x, y));
            Put(x, y, kHills);
        } else if (Dice(1, 10, 0) < 6) {
            SetTile(x, y, TileFor(kPlain, 0, x, y));
            Put(x, y, kPlain);
        } else {
            Put(x, y, kHills);
            SetTile(x, y, TileFor(kHills, 13, x, y));
        }
    });
    Each(kMarsh, [&](int x, int y) {
        // mostly the plain marsh; three in ten one of four others
        int offset = Dice(1, 10, 0) < 7 ? 2 * Parity(x, y) : 4 * Dice(1, 4, 0);
        SetTile(x, y, R16(kTiles + 64 * kMarsh + offset) & 0xff);
        Put(x, y, kMarsh);
    });
}

// --- bridges, crossings and roads ----------------------------------------

/** @brief True if (x, y) is firm ground a bridge may land on. */
bool Generator::Firm(int x, int y) const {
    int terrain = At(x, y);
    return terrain == kPlain || terrain == kForest || terrain == kHills;
}

/** @brief Computes the distance from each spot in `list` to its nearest city. */
void Generator::NearestCities(SpotList& list, int count) {
    if (list.city_dist[0] != -1) return;
    for (int i = count - 1; i >= 0; i--) {
        if (list.x[i] == -1) continue;
        int best = 10000;
        for (int city_idx = CityCount() - 1; city_idx >= 0; city_idx--)
            best = std::min(best, Distance(list.x[i], list.y[i], Cx(city_idx), Cy(city_idx)));
        list.city_dist[i] = best;
    }
}

/**
 * @brief Picks the highest-scoring unchosen spot in `list`, favouring
 *        spots near cities and (if `apart`) keeping spots at least 10 apart.
 */
int Generator::PickSpot(SpotList& list, int count, bool apart) {
    NearestCities(list, count);
    for (int i = count - 1; i >= 0; i--) {
        if (list.chosen[i] != 0 || list.x[i] == -1) continue;
        int best = 10000;
        for (int j = count - 1; j >= 0; j--) {
            if (list.chosen[j] != 0)
                best = std::min(best, Distance(list.x[i], list.y[i], list.x[j], list.y[j]));
        }
        list.chosen_dist[i] = best;
    }
    int best_score = -1, pick = -1;
    for (int i = count - 1; i >= 0; i--) {
        if (list.chosen[i] != 0 || list.x[i] == -1) continue;
        if (apart && !(list.chosen_dist[i] > 9)) continue;
        int roll = Dice(1, 15, 1);
        int near_by = list.city_dist[i] < 31 ? 30 - list.city_dist[i] : 0;
        if (best_score < near_by + roll) {
            best_score = near_by + roll;
            pick = i;
        }
    }
    return pick;
}

/** @brief Places bridges across two-tile rivers. */
void Generator::Bridges(SpotList& list) {
    int count = 0;
    for (int y = 1; y < kMapH - 2; y++) {
        for (int x = 1; x < kMapW - 2; x++) {
            int tile = Tile(x, y);
            bool across = (tile == 0x26 || tile == 0x16) &&
                          (Tile(x + 1, y) == 0x28 || Tile(x + 1, y) == 0x18);
            bool along = (tile == 0x21 || tile == 0x11) &&
                         (Tile(x, y + 1) == 0x24 || Tile(x, y + 1) == 0x14);
            if (!across && !along) continue;
            int east_west = tile == 0x26 ? 1 : 0;
            bool valid = east_west &&
                         ((Firm(x - 1, y) && Firm(x + 2, y)) || (Firm(x, y - 1) && Firm(x, y + 2)));
            if (valid && count < 50) {
                list.x[count] = x;
                list.y[count] = y;
                list.east_west[count] = east_west;
                count++;
            }
        }
    }
    if (count == 0) return;
    for (int i; (i = PickSpot(list, count, true)) != -1;) {
        list.chosen[i] = 1;
        int x = list.x[i], y = list.y[i];
        if (list.east_west[i] == 0) {
            SetTile(x, y, 0x84);
            SetTile(x, y + 1, 0x94);
            Put(x, y + 1, kBridge);
            Put(x, y, kBridge);
            SetRoad(x, y + 2, 1);
            SetRoad(x, y - 1, 1);
        } else {
            SetTile(x, y, 0x85);
            SetTile(x + 1, y, 0x86);
            Put(x + 1, y, kBridge);
            Put(x, y, kBridge);
            SetRoad(x + 2, y, 1);
            SetRoad(x - 1, y, 1);
        }
    }
}

/**
 * @brief Marks up to ten sea crossings on shore tiles that face three
 *        tiles of open water and lie at least 15 tiles from other candidates.
 */
void Generator::Crossings(SpotList& list) {
    for (int y = 1; y < kMapH - 2; y++) {
        for (int x = 1; x < kMapW - 2; x++) {
            if (Dice(1, 2, -1) != 0) continue;
            int dx = 0, dy = 0;
            switch (Tile(x, y)) {
            case 0x20:
            case 0x23:
                dx = -1;
                break;
            case 0x21:
                dy = -1;
                break;
            case 0x22:
            case 0x25:
                dx = 1;
                break;
            case 0x24:
                dy = 1;
                break;
            default:
                continue;
            }
            bool open = true;
            for (int dist = 1; dist <= 3; dist++)
                if (At(x + dist * dx, y + dist * dy) != kWater) open = false;
            if (!open) continue;
            int near_by = 10000;
            for (int i = 49; i >= 0; i--) {
                if (list.x[i] != -1)
                    near_by = std::min(near_by, Distance(x, y, list.x[i], list.y[i]));
            }
            if (near_by <= 14) continue;
            for (int i = 0; i < 50; i++) {
                if (list.x[i] == -1) {
                    list.x[i] = x;
                    list.y[i] = y;
                    break;
                }
            }
        }
    }
    list.city_dist[0] = -1;
    for (int count = 0, i; count < 10 && (i = PickSpot(list, 50, false)) != -1; count++) {
        list.chosen[i] = 1;
        tiles[list.y[i] * kMapW + list.x[i]] |= 0x8000;
    }
}

/** @brief True if city `city_idx` has already been connected by road. */
bool Generator::IsServed(int city_idx) const {
    return city_idx >= 0 && city_idx < static_cast<int>(served.size()) ? served[city_idx] != 0
                                                                       : false;
}
/** @brief Marks city `city_idx` as connected by road. */
void Generator::Serve(int city_idx) {
    if (city_idx >= 0 && city_idx < static_cast<int>(served.size())) served[city_idx] = 1;
}

/**
 * @brief Picks a pair of unserved cities 30..70 tiles apart to connect by road.
 */
std::optional<std::pair<int, int>> Generator::RoadPair() {
    for (;;) {
        int first = -1;
        for (int tries = 80; tries > 0; tries--) {
            first = Dice(1, CityCount(), -1);
            if (!IsServed(first)) break;
            first = -1;
        }
        if (first == -1) return std::nullopt;
        Serve(first);
        int second = -1, best = -1;
        for (int city_idx = CityCount() - 1; city_idx >= 0; city_idx--) {
            if (IsServed(city_idx)) continue;
            int dist = Distance(Cx(first), Cy(first), Cx(city_idx), Cy(city_idx));
            if (dist > 29 && dist < 71) {
                int roll = Dice(1, 1000, 0);
                if (best < roll) {
                    best = roll;
                    second = city_idx;
                }
            }
        }
        if (second != -1) {
            Serve(second);
            return std::make_pair(first, second);
        }
    }
}

/** @brief Picks a firm tile adjacent to city `city_idx`'s footprint to start a road. */
std::optional<Point> Generator::RoadEnd(int city_idx) {
    for (int tries = 8; tries > 0; tries--) {
        const int* off = kRoundCity[Dice(1, 12, -1)];
        int x = Cx(city_idx) + off[0], y = Cy(city_idx) + off[1];
        int terrain = At(x, y);
        if (terrain == kPlain || terrain == kForest || terrain == kHills) return Point{x, y};
    }
    return std::nullopt;
}

/**
 * @brief Lays a road along the pathfinder route from `from` to `to_pt`,
 *        marking cities within 10 tiles every six steps as served.
 */
void Generator::LayRoad(Point from, Point to_pt) {
    move::RoadGround ground;
    ground.terrain = [this](int x, int y) { return TerrainOf(Tile(x, y)); };
    ground.road = [this](int x, int y) { return Road(x, y) != 0; };
    ground.crossing = [this](int x, int y) {
        return x >= 0 && y >= 0 && x < kMapW && y < kMapH && (tiles[y * kMapW + x] & 0x8000) != 0;
    };
    auto route = move::RoadRoute(ground, kRoadCost, kMapW, kMapH, from.x, from.y, to_pt.x, to_pt.y);
    if (!route) return;
    int x = from.x, y = from.y, since = 0;
    for (size_t i = 0; i < route->size() && i < 200; i++) {
        since++;
        if (x == to_pt.x && y == to_pt.y) break;
        x = (*route)[i].first;
        y = (*route)[i].second;
        int terrain = At(x, y);
        if (terrain != kShore && terrain != kWater && terrain != kBridge) SetRoad(x, y, 1);
        if (since > 5) {
            since = 0;
            for (int city_idx = CityCount() - 1; city_idx >= 0; city_idx--) {
                if (Distance(x, y, Cx(city_idx), Cy(city_idx)) < 10) Serve(city_idx);
            }
        }
    }
}

/** @brief Converts terrain to tiles, applies transition shapes twice, and clears road/crossing
 * bits. */
void Generator::RoadTiles() {
    ToTiles();
    Shapes();
    Shapes();
    for (int i = 0; i < kMapW * kMapH; i++) {
        tiles[i] &= 0x7fff;
        roads[i] &= 0xe0;
    }
}

/** @brief Places bridges and sea crossings and resets road service tracking. */
void Generator::RoadCrossings() {
    SpotList list;
    for (int i = 0; i < 50; i++) {
        list.x[i] = list.y[i] = list.east_west[i] = list.city_dist[i] = list.chosen_dist[i] = -1;
        list.chosen[i] = 0;
    }
    Bridges(list);
    for (int i = 49; i >= 0; i--) {
        if (list.chosen[i] == 0)
            list.x[i] = list.y[i] = list.east_west[i] = list.city_dist[i] = list.chosen_dist[i] =
                    -1;
    }
    Crossings(list);
    served.assign(100, 0);
    shown = 4;
}

/**
 * @brief Connects cities by road a pair at a time, returning a progress
 *        percentage when the bar advances or `std::nullopt` when finished.
 */
std::optional<int> Generator::RoadsUntilShown() {
    for (;;) {
        std::pair<int, int> pair;
        if (pending) {
            pair = *pending;
            pending.reset();
        } else {
            auto next_pair = RoadPair();
            if (!next_pair) break;
            pair = *next_pair;
            if (shown < 10 && Dice(1, 3, -1) == 0) {
                pending = pair;
                return 80 + shown++;
            }
        }
        auto start_pt = RoadEnd(pair.first);
        std::optional<Point> end_pt;
        if (start_pt) end_pt = RoadEnd(pair.second);
        if (start_pt && end_pt && !(start_pt->x == end_pt->x && start_pt->y == end_pt->y))
            LayRoad(*start_pt, *end_pt);
    }
    for (int i = 0; i < kMapW * kMapH; i++)
        if (memory[kGrid + i] == kSite) roads[i] &= 0xe0;
    return std::nullopt;
}

/**
 * @brief Assigns road piece shapes from neighbouring roads and bridges,
 *        and randomly swaps a few straight pieces to their alternate look.
 */
void Generator::RoadShapes() {
    for (int y = 0; y < kMapH; y++) {
        for (int x = 0; x < kMapW; x++) {
            if (Road(x, y) == 0) continue;
            int mask = 0;
            for (int i = 0; i < 8; i++) {
                int adj_x = x + R16(kNbRoad + 4 * i), adj_y = y + R16(kNbRoad + 4 * i + 2);
                if (adj_x >= 0 && adj_y >= 0 && adj_x < kMapW && adj_y < kMapH &&
                    (Road(adj_x, adj_y) != 0 || TerrainOf(Tile(adj_x, adj_y)) == kBridge))
                    mask |= 1 << i;
            }
            int variant = I8(memory[kRoadShape + mask]);
            if (variant >= 0)
                SetRoad(x, y, variant + 1);
            else if (Dice(1, 10, 0) < 6) {
                SetTile(x, y, TileFor(kPlain, 0, x, y));
                Put(x, y, kPlain);
            } else {
                SetTile(x, y, TileFor(kHills, 13, x, y));
            }
        }
    }
    const int swaps[2][2] = {{2, 17}, {1, 16}};
    for (auto& swap_pair : swaps) {
        int count = Dice(1, 10, 10);
        for (int idx = 0; idx < count; idx++) {
            for (int tries = 0; tries < 10000; tries++) {
                int x = Dice(1, kMapW, -1);
                int y = Dice(1, kMapH, -1);
                if (Road(x, y) == swap_pair[0]) {
                    SetRoad(x, y, swap_pair[1]);
                    break;
                }
            }
        }
    }
}

}  // namespace w2::randommap
