/**
 * @file
 * @brief Random map capital selection, fair capitals, city naming/production, signposts, and file
 * output.
 */
#include "warlords/worldgen/randommap_impl.hpp"

namespace w2::randommap {

// --- the sides and the cities' details -----------------------------------

/** @brief Reads the cities back off the tile map in row order. */
void Generator::CityList() {
    int count = 0;
    for (int y = 0; y < kMapH; y++) {
        for (int x = 0; x < kMapW; x++) {
            if (Tile(x, y) != 0x60) continue;
            int offset = Crec(count);
            Ws16(offset, x);
            Ws16(offset + 2, y);
            scn[offset + 21] = 15;
            scn[offset + 20] = static_cast<uint8_t>(Dice(1, 3, 2));
            count++;
        }
    }
    SetCityCount(count);
}

/**
 * @brief Picks a random city in one of the map's eight regions (4x2);
 *        after 202 tries, returns whatever the last draw was.
 */
int Generator::CityIn(int region) {
    int min_x = (region % 4) * 28, min_y = region / 4 * 78;
    int city_idx = 0;
    for (int tries = 0; tries < 202; tries++) {
        city_idx = Dice(1, CityCount(), -1);
        int x = Cx(city_idx), y = Cy(city_idx);
        if (x >= min_x && x < min_x + 28 && y >= min_y && y < min_y + 78) break;
    }
    return city_idx;
}

/**
 * @brief Assigns each side a capital in its own region, at least 12 tiles
 *        from the map edge and 20 from other capitals along both axes.
 */
void Generator::Capitals() {
    for (int round = 1;; round++) {
        bool used[8] = {false};
        for (int side_idx = 0; side_idx < 8; side_idx++) {
            Ws16(SideRec(side_idx) + 6, -100);
            Ws16(SideRec(side_idx) + 8, -100);
        }
        bool again = false;
        for (int side_idx = 0; side_idx < 8 && !again; side_idx++) {
            int region;
            do region = Dice(1, 8, -1);
            while (used[region]);
            used[region] = true;
            int city_idx = 0, tries = 0;
            bool valid = false;
            for (; !valid && tries < 100; tries++) {
                city_idx = CityIn(region);
                int x = Cx(city_idx), y = Cy(city_idx);
                if (x < 12 || x > 100 || y < 12 || y > 144) continue;
                valid = true;
                for (int other = 0; other < 8; other++) {
                    if (std::abs(x - S16(SideRec(other) + 6)) < 20 ||
                        std::abs(y - S16(SideRec(other) + 8)) < 20) {
                        valid = false;
                        break;
                    }
                }
            }
            // a find on the hundredth try counts as none, as the original has it
            if (tries > 99 && round < 10) {
                again = true;
                break;
            }
            W16(kRegionClass + 2 * region, R16(kSideClass + 2 * side_idx));
            Ws16(SideRec(side_idx) + 6, Cx(city_idx));
            Ws16(SideRec(side_idx) + 8, Cy(city_idx));
        }
        if (!again) break;
    }
    Crown();
}

/** @brief Marks each capital's owner byte and paints its castle in the side's colours. */
void Generator::Crown() {
    for (int side_idx = 0; side_idx < 8; side_idx++) {
        int x = S16(SideRec(side_idx) + 6), y = S16(SideRec(side_idx) + 8);
        for (int city_idx = 0; city_idx < CityCount(); city_idx++) {
            if (Cx(city_idx) == x && Cy(city_idx) == y) {
                scn[Crec(city_idx) + 21] = static_cast<uint8_t>(side_idx);
                break;
            }
        }
        int tile_base = side_idx < 6 ? 0x62 + 2 * side_idx : 0x80 + 2 * (side_idx - 6);
        SetTile(x, y, tile_base);
        SetTile(x + 1, y, tile_base + 1);
        SetTile(x, y + 1, tile_base + 16);
        SetTile(x + 1, y + 1, tile_base + 17);
    }
}

/**
 * @brief Chooses the eight capitals by local search over travel-cost
 *        fairness (Options::fair_capitals).
 */
void Generator::FairCapitals() {
    constexpr int kSides = 8;
    constexpr double kFairGap = 3.0;  // turns
    constexpr double kRemote = 5.0;   // turns from the nearest capital
    constexpr int kStarts = 12, kPasses = 40;
    const int num_cities = CityCount();
    if (num_cities < kSides) {
        Capitals();
        return;
    }
    travel::Ground ground;
    ground.w = kMapW;
    ground.h = kMapH;
    ground.terrain.resize(static_cast<size_t>(kMapW) * kMapH);
    ground.road.resize(static_cast<size_t>(kMapW) * kMapH);
    ground.crossing.resize(static_cast<size_t>(kMapW) * kMapH);
    for (int y = 0; y < kMapH; y++)
        for (int x = 0; x < kMapW; x++) {
            int cell = y * kMapW + x, terrain = TerrainOf(Tile(x, y));
            ground.terrain[cell] = terrain;
            ground.road[cell] = Road(x, y) != 0;
            ground.crossing[cell] = (tiles[cell] & 0x8000) != 0 || terrain == kBridge;
        }
    auto footprint = [&](int city_idx) {
        std::vector<int> cells;
        for (auto& off : kFootprint)
            if (Cx(city_idx) + off[0] < kMapW && Cy(city_idx) + off[1] < kMapH)
                cells.push_back((Cy(city_idx) + off[1]) * kMapW + Cx(city_idx) + off[0]);
        return cells;
    };
    // a port city is a crossing: its footprint touches the sea
    for (int city_idx = 0; city_idx < num_cities; city_idx++) {
        bool port = false;
        for (auto& ring : kRoundCity) {
            int terrain = TerrainOf(Tile(Cx(city_idx) + ring[0], Cy(city_idx) + ring[1]));
            if (terrain == kBridge || terrain == kWater || terrain == kShore) port = true;
        }
        if (port)
            for (int cell : footprint(city_idx)) ground.crossing[cell] = 1;
    }
    std::vector<std::vector<int>> cost_matrix(num_cities,
                                              std::vector<int>(num_cities, travel::kUnreached));
    for (int city_idx = 0; city_idx < num_cities; city_idx++) {
        auto field = travel::Costs(ground, footprint(city_idx));
        for (int other = 0; other < num_cities; other++)
            for (int cell : footprint(other))
                cost_matrix[city_idx][other] = std::min(cost_matrix[city_idx][other], field[cell]);
    }
    // how unfair a choice is; lower is better
    std::vector<int> owner(num_cities);
    auto unfairness = [&](const std::array<int, kSides>& caps) {
        double gap = 1e9;
        for (int i = 0; i < kSides; i++)
            for (int j = i + 1; j < kSides; j++)
                gap = std::min(gap, static_cast<double>(cost_matrix[caps[i]][caps[j]]));
        if (gap >= travel::kUnreached) return 1e12;  // a capital cut off from another
        gap /= travel::kTurn;
        std::array<int, kSides> share{};
        int remote = 0;
        double reach = 0;
        for (int city_idx = 0; city_idx < num_cities; city_idx++) {
            if (std::find(caps.begin(), caps.end(), city_idx) != caps.end()) continue;
            int best = travel::kUnreached, who = 0;
            for (int side_idx = 0; side_idx < kSides; side_idx++)
                if (cost_matrix[caps[side_idx]][city_idx] < best) {
                    best = cost_matrix[caps[side_idx]][city_idx];
                    who = side_idx;
                }
            if (best == travel::kUnreached) {
                remote++;
                continue;
            }
            share[who]++;
            reach += static_cast<double>(best) / travel::kTurn;
            if (best > kRemote * travel::kTurn) remote++;
        }
        auto [min_it, max_it] = std::minmax_element(share.begin(), share.end());
        return 3.0 * remote + 1.0 * (*max_it - *min_it) + reach / num_cities +
               (gap < kFairGap ? 10.0 * (kFairGap - gap) : 0.0);
    };
    std::array<int, kSides> best{};
    double best_score = 1e18;
    for (int start = 0; start < kStarts; start++) {
        std::array<int, kSides> caps{};
        for (int side_idx = 0; side_idx < kSides; side_idx++) {
            int city_idx;
            do city_idx = Dice(1, num_cities, -1);
            while (std::find(caps.begin(), caps.begin() + side_idx, city_idx) !=
                   caps.begin() + side_idx);
            caps[side_idx] = city_idx;
        }
        double score = unfairness(caps);
        for (int pass = 0; pass < kPasses; pass++) {
            bool better = false;
            for (int side_idx = 0; side_idx < kSides; side_idx++)
                for (int city_idx = 0; city_idx < num_cities; city_idx++) {
                    if (std::find(caps.begin(), caps.end(), city_idx) != caps.end()) continue;
                    int was = caps[side_idx];
                    caps[side_idx] = city_idx;
                    double now = unfairness(caps);
                    if (now < score - 1e-9) {
                        score = now;
                        better = true;
                    } else
                        caps[side_idx] = was;
                }
            if (!better) break;
        }
        if (score < best_score) {
            best_score = score;
            best = caps;
        }
    }
    // the sides take the capitals in a random order; a side's armies favour
    // the region its capital stands in
    for (int side_idx = kSides - 1; side_idx > 0; side_idx--)
        std::swap(best[side_idx], best[Dice(1, side_idx + 1, -1)]);
    for (int side_idx = 0; side_idx < kSides; side_idx++) {
        int x = Cx(best[side_idx]), y = Cy(best[side_idx]);
        int region = std::min(3, x / 28) + 4 * std::min(1, y / 78);
        W16(kRegionClass + 2 * region, R16(kSideClass + 2 * side_idx));
        Ws16(SideRec(side_idx) + 6, x);
        Ws16(SideRec(side_idx) + 8, y);
    }
    Crown();
}

/** @brief Assigns each side one of five names and 3d50+20 starting gold. */
void Generator::Sides() {
    for (int side_idx = 0; side_idx < 8; side_idx++) {
        int name_idx = Dice(1, 5, -1);
        std::string name = Str(kSideNames + 100 * side_idx + 20 * name_idx, 20);
        for (int i = 0; i < 20; i++) scn[20 * side_idx + i] = 0;
        Wstr(20 * side_idx, name);
    }
    for (int side_idx = 0; side_idx < 8; side_idx++) Ws16(SideRec(side_idx) + 2, Dice(3, 50, 20));
}

/** @brief Inspects which terrain features touch city `city_idx`'s 2x2 footprint. */
Surroundings Generator::SurroundingsOf(int city_idx) const {
    int x = Cx(city_idx), y = Cy(city_idx);
    auto touches = [&](int terrain) {
        return Near(x, y, terrain) || Near(x + 1, y, terrain) || Near(x, y + 1, terrain) ||
               Near(x + 1, y + 1, terrain);
    };
    return Surroundings{touches(kShore), touches(kForest), touches(kRoad), touches(kHills),
                        touches(kMarsh)};
}

/**
 * @brief Computes a city's value (0..9): 9 for a capital, otherwise
 *        1d4-1 +4 by shore, +2 by road, -1 by forest, -2 by marsh.
 */
int Generator::Value(int city_idx, const Surroundings& surr) {
    if (scn[Crec(city_idx) + 21] != 15) return 9;
    int val = Dice(1, 4, -1);
    if (surr.shore) val += 4;
    if (surr.road) val += 2;
    if (surr.forest) val -= 1;
    if (surr.marsh) val -= 2;
    if (val > 8) val = 9;
    if (val < 1) val = 0;
    return val;
}

/** @brief Generates a two- or three-syllable name for city `city_idx`. */
std::string Generator::CityName(int city_idx, const Surroundings& surr) {
    bool three = Dice(1, 10, 0) < 7;
    int idx1 = Dice(1, 20, -1);
    std::string name = Str(0xa86 + 10 * idx1, 10);
    if (three) {
        int idx2 = Dice(1, 20, -1);
        name += Str(0xb4e + 10 * idx2, 10);
    }
    int suffix_table = -1;
    if (Dice(1, 10, 0) < 7) {
        if (surr.marsh)
            suffix_table = 0xda6;
        else if (surr.forest)
            suffix_table = 0xcde;
        else if (surr.shore)
            suffix_table = 0xd42;
        else if (surr.hills)
            suffix_table = 0xe0a;
        else if (Dice(1, 10, 0) < 5)
            suffix_table = 0xe6e;
    }
    if (suffix_table != -1) {
        int idx3 = Dice(1, 10, -1);
        name += Str(suffix_table + 10 * idx3, 10);
    } else {
        int idx3 = Dice(1, 20, -1);
        name += Str(0xc16 + 10 * idx3, 10);
    }
    name = name.substr(0, 15);
    int offset = Crec(city_idx) + 4;
    for (int i = 0; i < 16; i++) scn[offset + i] = 0;
    Wstr(offset, name);
    return name;
}

/** @brief True if any city before `city_idx` already has `name`. */
bool Generator::NameTaken(int city_idx, const std::string& name) const {
    for (int j = 0; j < city_idx; j++)
        if (CstrOf(scn, Crec(j) + 4, 16) == name) return true;
    return false;
}

/**
 * @brief Generates city `city_idx`'s name, income, and three-line `.CTY`
 *        description.
 */
void Generator::CityText(int city_idx, int val, const Surroundings& surr) {
    std::string name = CityName(city_idx, surr);
    // the scenario generator's names never repeat (the original's may)
    for (int tries = 0; modern && tries < 200 && NameTaken(city_idx, name); tries++)
        name = CityName(city_idx, surr);
    Ws16(Crec(city_idx) + 42, val * 2 + Dice(1, 8, 0) + 14);
    auto grade = [&]() { return std::min(9, std::max(0, Dice(1, 3, val - 2))); };
    int grade1 = grade();
    int grade2 = grade();
    auto word_at = [&](int offset, int idx) { return Str(offset + 16 * idx, 16); };
    std::string part_a, part_b, part_c;
    bool plain = !surr.forest && !surr.marsh && !surr.hills;
    if (plain || Dice(1, 100, 0) > 79) {
        if (Dice(1, 100, 0) < 50) {
            part_a = word_at(0x16a8, Dice(1, 10, -1));
            part_b = word_at(0x1748, Dice(1, 10, -1));
            part_c = word_at(0x17e8, Dice(1, 10, -1));
        } else {
            part_a = word_at(0x14c8, Dice(1, 10, -1));
            part_b = word_at(0x1568, Dice(1, 10, -1));
            part_c = word_at(0x1608, Dice(1, 10, -1));
        }
    } else {
        int sub_idx;
        if (surr.marsh)
            sub_idx = Dice(1, 2, 7);
        else if (surr.hills)
            sub_idx = Dice(1, 4, 3);
        else
            sub_idx = Dice(1, 4, -1);
        part_a = word_at(0x12e8, Dice(1, 10, -1));
        part_b = word_at(0x1388, Dice(1, 10, -1));
        part_c = word_at(0x1428, sub_idx);
    }
    std::string kind = word_at(0x1248, grade2);
    int adj_idx = Dice(1, 3, -1);
    std::string adj = Str(0x1068 + 0x30 * grade1 + 16 * adj_idx, 16);
    cty += Format("#%03d|%s is a %s|%s, %s|%s %s|\n", city_idx, name, adj, kind, part_a, part_b,
                  part_c);
}

/** @brief True if army type `type_id` has the flying flag (ARMYTYPE +54). */
bool Generator::Flies(int type_id) const {
    const ArmyType* army_type = types.ById(type_id);
    return army_type && army_type->flies;
}
/** @brief True if army type `type_id` is magical / an ally (ARMYTYPE +48). */
bool Generator::Magical(int type_id) const {
    const ArmyType* army_type = types.ById(type_id);
    return army_type && army_type->IsMagical();
}

/** @brief Populates production slot `slot_idx` of city `city_idx` with army type `type_id`. */
void Generator::SetSlot(int city_idx, int slot_idx, int type_id) {
    int offset = Crec(city_idx);
    const ArmyType* army_type = types.ById(type_id);
    scn[offset + 22 + slot_idx] = static_cast<uint8_t>(type_id);
    scn[offset + 26 + slot_idx] = static_cast<uint8_t>(army_type ? army_type->time : 0);
    scn[offset + 30 + slot_idx] = static_cast<uint8_t>(army_type ? army_type->strength : 0);
    scn[offset + 34 + slot_idx] = static_cast<uint8_t>(army_type ? army_type->move : 0);
    scn[offset + 38 + slot_idx] = static_cast<uint8_t>(army_type ? army_type->cost : 0);
}

/** @brief Chooses the up to four army production slots for city `city_idx`. */
void Generator::Production(int city_idx, int val, const Surroundings& surr) {
    int x = Cx(city_idx), y = Cy(city_idx);
    int region = x * 4 / kMapW + y * 2 / kMapH * 4;
    int slots = val / 2;
    slots += Dice(1, 4, -1);
    if (surr.forest) slots++;
    if (surr.hills) slots++;
    if (slots > 3) slots = 4;
    if (slots < 1) slots = 0;
    auto rec = [](int entry) { return kProduction + 16 * entry; };
    auto suits = [&](int entry) {
        int terr = R16(rec(entry) + 6);
        return terr == 7 || (terr == 4 && surr.forest) || (terr == 5 && surr.hills) ||
               R16(rec(entry) + 4) == R16(kRegionClass + 2 * region);
    };
    int slot_idx = 0;
    for (int entry = 0; entry < 29 && slot_idx < slots; entry++) {
        int type_id = R16(rec(entry));
        if (!allies && Magical(type_id)) continue;
        if (Dice(1, 10, -1) >= R16(rec(entry) + 2)) continue;
        bool valid = suits(entry) || (R16(rec(entry) + 6) == 3 && surr.shore);
        if (type_id == 5 && !surr.shore) valid = false;
        if (valid) SetSlot(city_idx, slot_idx++, type_id);
    }
    if (slot_idx == 0) SetSlot(city_idx, slot_idx++, R16(rec(0)));
    if (val > 6 && Dice(1, 10, -1) < 5 && slot_idx < 4) {
        bool flier = false;
        for (int j = 0; j < slot_idx; j++)
            if (Flies(scn[Crec(city_idx) + 22 + j])) flier = true;
        if (!flier && slot_idx < slots) {
            for (int entry = 0; entry < 29; entry++) {
                int type_id = R16(rec(entry));
                if (Flies(type_id) && !Magical(type_id) && suits(entry)) {
                    SetSlot(city_idx, slot_idx++, type_id);
                    break;
                }
            }
        }
    }
    for (; slot_idx < 4; slot_idx++) scn[Crec(city_idx) + 22 + slot_idx] = 255;
}

/** @brief Picks a random magical (ally) army type id. */
int Generator::MagicalType() {
    int count = 0;
    for (int type_id = 0; type_id < 28; type_id++)
        if (Magical(type_id)) count++;
    int pick = Dice(1, count, -1);
    int match_idx = 0, type_id = 0;
    for (; type_id < 28; type_id++) {
        if (!Magical(type_id)) continue;
        if (match_idx == pick) break;
        match_idx++;
    }
    return type_id > 27 || !Magical(type_id) ? 25 : type_id;
}

/** @brief Adds a magical army type to 2d3 cities when allies are enabled. */
void Generator::AllyProduction() {
    int count = Dice(2, 3, 0);
    for (int tries = 0; count > 0 && tries < 100; tries++) {
        int city_idx = Dice(1, CityCount(), -1), offset = Crec(city_idx);
        int slot_idx = 0;
        for (int j = 0; j < 4; j++)
            if (scn[offset + 22 + j] < 128) slot_idx++;
        if (slot_idx < 4) {
            SetSlot(city_idx, slot_idx, MagicalType());
            count--;
        }
    }
}

/**
 * @brief Adds a strong non-magical, non-navy army type (strength >= 5) to
 *        each capital's last empty production slot.
 */
void Generator::CapitalArmies() {
    for (int side_idx = 0; side_idx < 8; side_idx++) {
        int x = S16(SideRec(side_idx) + 6), y = S16(SideRec(side_idx) + 8);
        int city_idx = -1;
        for (int i = 0; i < CityCount() && city_idx < 0; i++)
            if (Cx(i) == x && Cy(i) == y) city_idx = i;
        if (city_idx < 0) continue;
        int offset = Crec(city_idx);
        // the highest empty slot, else the last
        int slot_idx = 3;
        while (slot_idx >= 0 && scn[offset + 22 + slot_idx] < 128) slot_idx--;
        if (slot_idx < 0) slot_idx = 3;
        int draws = 0, pick = -1;
        while (draws <= 39) {
            int type_id = Dice(1, 28, -1);
            if (type_id == 5 || Magical(type_id)) continue;
            draws++;
            bool has = false;
            for (int j = 0; j < 4; j++)
                if (scn[offset + 22 + j] == type_id) has = true;
            const ArmyType* army_type = types.ById(type_id);
            if (!has && army_type && army_type->strength >= 5) {
                pick = type_id;
                break;
            }
        }
        // one found on the fortieth draw is dropped, as the original has it
        if (pick >= 0 && draws < 40) scn[offset + 22 + slot_idx] = static_cast<uint8_t>(pick);
    }
}

/** @brief Populates names, descriptions, and army production for all cities. */
void Generator::CityDetails() {
    for (int city_idx = 0; city_idx < CityCount(); city_idx++) {
        Surroundings surr = SurroundingsOf(city_idx);
        int val = Value(city_idx, surr);
        CityText(city_idx, val, surr);
        Production(city_idx, val, surr);
    }
    if (allies) AllyProduction();
    CapitalArmies();
}

/**
 * @brief Places 41..70 signposts on open plain tiles off the roads, most
 *        pointing to the nearest city and a few bearing fixed messages.
 */
std::string Generator::Signs() {
    int count = Dice(1, 30, 40);
    std::vector<uint8_t> out(2 + 104 * count, 0);
    SetI16(out, 0, count);
    auto put_str = [&](int offset, const std::string& text) {
        for (size_t i = 0; i < text.size() && i < 49; i++)
            out[offset + i] = static_cast<uint8_t>(text[i]);
    };
    int fixed = 0;
    for (int i = 0; i < count; i++) {
        int x = 0, y = 0;
        for (int tries = 0; tries < kSafety; tries++) {
            x = Dice(1, kMapW, -1);
            y = Dice(1, kMapH, -1);
            if (TerrainOf(Tile(x, y)) == kPlain && !Near(x, y, kCity) && !Near(x, y, kTower) &&
                Road(x, y) == 0)
                break;
        }
        SetTile(x, y, 0);
        int offset = 2 + 104 * i;
        SetI16(out, offset, x);
        SetI16(out, offset + 2, y);
        if (Dice(1, 10, 0) < 4 && fixed < R16(kSigns)) {
            put_str(offset + 4, Str(kSigns + 2 + 60 * fixed, 30));
            put_str(offset + 54, Str(kSigns + 2 + 60 * fixed + 30, 30));
            fixed++;
        } else {
            int city_idx = -1, best = 1000;
            for (int j = 0; j < CityCount(); j++) {
                int dist = Distance(x, y, Cx(j), Cy(j));
                if (dist < best) {
                    best = dist;
                    city_idx = j;
                }
            }
            if (city_idx < 0) continue;
            int city_x = Cx(city_idx), city_y = Cy(city_idx);
            put_str(offset + 4, Format(Str(kSignCity, 20), CstrOf(scn, Crec(city_idx) + 4, 16)));
            put_str(offset + 54, Format(Str(kSignLeagues, 20), Distance(x, y, city_x, city_y) * 2,
                                        std::string(kCompass[Compass(x, y, city_x, city_y)])));
        }
    }
    return std::string(out.begin(), out.end());
}

// --- the whole -------------------------------------------------------------

/** @brief Packs the generated scenario, tile map, roads, signposts, and texts into `files`. */
void Generator::WriteFiles(Files& files) {
    std::string map(2UZ * kMapW * kMapH, '\0');
    for (int i = 0; i < kMapW * kMapH; i++) {
        map[2UZ * i] = static_cast<char>(tiles[i] & 0xff);
        map[2 * i + 1] = static_cast<char>(tiles[i] >> 8);
    }
    std::string road(static_cast<size_t>(kMapW) * kMapH, '\0');
    for (int i = 0; i < kMapW * kMapH; i++) road[i] = static_cast<char>(roads[i] & 0x1f);
    files["SCN"] = std::string(scn.begin(), scn.end());
    files["MAP"] = map;
    files["RD"] = road;
    files["SGN"] = sgn;
    files["CTY"] = cty;
    files["SPC"] = spc;
}

}  // namespace w2::randommap
