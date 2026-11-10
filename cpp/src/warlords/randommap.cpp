/**
 * @file
 * @brief Random map and modern scenario generator entry points, stage state machine, and modern
 *        road/signpost bridge passes.
 */
#include "warlords/randommap.hpp"

#include <algorithm>
#include <bit>

#include "absl/status/status.h"
#include "absl/strings/str_format.h"

#include "util/util.hpp"
#include "warlords/armytype.hpp"
#include "warlords/bytes.hpp"
#include "warlords/worldgen/randommap_impl.hpp"

namespace w2::randommap {

const char* const kFiles[6] = {"SCN", "MAP", "RD", "SGN", "CTY", "SPC"};
const int kSliderShows[4][7] = {{5, 7, 9, 11, 13, 15, 17},
                                {5, 7, 9, 11, 13, 15, 17},
                                {70, 75, 80, 85, 90, 95, 100},
                                {9, 11, 13, 15, 17, 19, 21}};
const char* const kSliderFormats[4] = {"(%d%%)", "(%d%%)", "(%d)", "(%d%%)"};

// --- the modern scenario generator (warlords/scengen) --------------------

/** @brief Returns a copy of the 112x156 terrain grid. */
std::vector<uint8_t> Generator::Grid() const {
    std::vector<uint8_t> grid(static_cast<size_t>(kMapW) * kMapH);
    for (int i = 0; i < kMapW * kMapH; i++) grid[i] = memory_[kGrid + i];
    return grid;
}

/** @brief Finds all two-tile river stretches that can be bridged. */
std::vector<scengen::Span> Generator::Spans() const {
    std::vector<scengen::Span> out;
    for (int y = 1; y < kMapH - 2; y++)
        for (int x = 1; x < kMapW - 2; x++) {
            int tile = Tile(x, y);
            bool across = (tile == 0x26 || tile == 0x16) &&
                          (Tile(x + 1, y) == 0x28 || Tile(x + 1, y) == 0x18);
            bool along = (tile == 0x21 || tile == 0x11) &&
                         (Tile(x, y + 1) == 0x24 || Tile(x, y + 1) == 0x14);
            if (across && Firm(x - 1, y) && Firm(x + 2, y)) out.push_back({x, y, true});
            if (along && Firm(x, y - 1) && Firm(x, y + 2)) out.push_back({x, y, false});
        }
    return out;
}

/** @brief Lays the modern scenario generator's planned roads, bridges, and crossings. */
void Generator::LayPlannedRoads() {
    std::vector<CityPair> where;
    for (int city_idx = 0; city_idx < CityCount(); city_idx++)
        where.push_back({Cx(city_idx), Cy(city_idx)});
    auto spans = Spans();
    auto road_plan = scengen::PlanRoads(Grid(), kMapW, kMapH, where, spans, *rng_);
    SpotList list;
    for (int i = 0; i < 50; i++) {
        list.x[i] = list.y[i] = list.east_west[i] = list.city_dist[i] = list.chosen_dist[i] = -1;
        list.chosen[i] = 0;
    }
    int count = 0;
    for (int span_idx : road_plan.bridges) {
        auto& bridge = spans[span_idx];
        if (bridge.ew) {
            SetTile(bridge.x, bridge.y, 0x85);
            SetTile(bridge.x + 1, bridge.y, 0x86);
            Put(bridge.x, bridge.y, kBridge);
            Put(bridge.x + 1, bridge.y, kBridge);
        } else {
            SetTile(bridge.x, bridge.y, 0x84);
            SetTile(bridge.x, bridge.y + 1, 0x94);
            Put(bridge.x, bridge.y, kBridge);
            Put(bridge.x, bridge.y + 1, kBridge);
        }
        if (count < 50) {
            list.x[count] = bridge.x;
            list.y[count] = bridge.y;
            list.chosen[count] = 1;
            count++;
        }
    }
    for (int i = 0; i < kMapW * kMapH; i++)
        if (road_plan.road[i]) SetRoad(i % kMapW, i / kMapW, 1);
    // The crossings keep their distance from the bridges.
    Crossings(list);
    Landings();
}

/**
 * @brief Ensures every landmass containing a city or site has at least one
 *        port city or sea crossing.
 */
void Generator::Landings() {
    auto sea = [&](int x, int y) {
        int terrain = At(x, y);
        return terrain == kWater || terrain == kShore;
    };
    std::vector<int> part(static_cast<size_t>(kMapW) * kMapH, -1);
    int parts = 0;
    for (int start = 0; start < kMapW * kMapH; start++) {
        if (part[start] >= 0 || sea(start % kMapW, start / kMapW)) continue;
        std::vector<int> todo{start};
        part[start] = parts;
        for (size_t idx = 0; idx < todo.size(); idx++) {
            int x = todo[idx] % kMapW, y = todo[idx] / kMapW;
            for (int dir = 0; dir < 8; dir++) {
                int adj_x = x + Nx(dir), adj_y = y + Ny(dir);
                if (adj_x < 0 || adj_y < 0 || adj_x >= kMapW || adj_y >= kMapH || sea(adj_x, adj_y))
                    continue;
                int adj_idx = adj_y * kMapW + adj_x;
                if (part[adj_idx] < 0) {
                    part[adj_idx] = parts;
                    todo.push_back(adj_idx);
                }
            }
        }
        parts++;
    }
    std::vector<uint8_t> needs(parts, 0), has_landing(parts, 0);
    for (int city_idx = 0; city_idx < CityCount(); city_idx++) {
        int part_id = part[Cy(city_idx) * kMapW + Cx(city_idx)];
        if (part_id < 0) continue;
        needs[part_id] = 1;
        for (const auto& ring : kRoundCity)
            if (sea(Cx(city_idx) + ring[0], Cy(city_idx) + ring[1])) has_landing[part_id] = 1;
    }
    for (int i = 0; i < kNSites; i++) {
        int x = S16(kSSites + kSiteStride * i), y = S16(kSSites + kSiteStride * i + 2);
        if (x >= 0 && y >= 0 && x < kMapW && y < kMapH && part[y * kMapW + x] >= 0)
            needs[part[y * kMapW + x]] = 1;
    }
    for (int i = 0; i < kMapW * kMapH; i++)
        if (tiles_[i] & 0x8000) {
            int x = i % kMapW, y = i / kMapW;
            for (int dir = 0; dir < 8; dir++) {
                int adj_x = x + Nx(dir), adj_y = y + Ny(dir);
                if (adj_x >= 0 && adj_y >= 0 && adj_x < kMapW && adj_y < kMapH &&
                    part[adj_y * kMapW + adj_x] >= 0)
                    has_landing[part[adj_y * kMapW + adj_x]] = 1;
            }
        }
    for (int i = 0; i < kMapW * kMapH; i++) {
        int x = i % kMapW, y = i / kMapW;
        if (!sea(x, y) || (tiles_[i] & 0x8000)) continue;
        int touched = -1;
        for (int dir = 0; dir < 8; dir += 2) {  // The four cardinal sides.
            int adj_x = x + Nx(dir), adj_y = y + Ny(dir);
            if (adj_x >= 0 && adj_y >= 0 && adj_x < kMapW && adj_y < kMapH &&
                part[adj_y * kMapW + adj_x] >= 0)
                touched = part[adj_y * kMapW + adj_x];
        }
        if (touched >= 0 && needs[touched] && !has_landing[touched]) {
            tiles_[i] |= 0x8000;
            has_landing[touched] = 1;
        }
    }
}

/**
 * @brief Assigns road pieces for the modern scenario generator from the 4
 *        cardinal neighbours and extends road ends into adjacent cities.
 */
void Generator::ModernRoadShapes() {
    // The piece for each N 1, E 2, S 4, W 8 combination.
    constexpr int kPiece[16] = {0, 15, 14, 10, 13, 2, 11, 7, 12, 9, 1, 6, 8, 5, 4, 3};
    const int dx[4] = {0, 1, 0, -1}, dy[4] = {-1, 0, 1, 0};
    std::vector<int> shape(static_cast<size_t>(kMapW) * kMapH, 0);
    for (int y = 0; y < kMapH; y++)
        for (int x = 0; x < kMapW; x++) {
            if (Road(x, y) == 0) continue;
            int mask = 0, city = -1;
            for (int dir = 0; dir < 4; dir++) {
                int adj_x = x + dx[dir], adj_y = y + dy[dir];
                if (adj_x < 0 || adj_y < 0 || adj_x >= kMapW || adj_y >= kMapH) continue;
                if (Road(adj_x, adj_y) != 0 || At(adj_x, adj_y) == kBridge)
                    mask |= 1 << dir;
                else if (At(adj_x, adj_y) == kCity && city < 0)
                    city = dir;
            }
            // A road's end runs into the city beside it.
            if (city >= 0 && std::popcount(static_cast<unsigned>(mask)) <= 1) mask |= 1 << city;
            if (mask == 0) {
                SetRoad(x, y, 0);
                continue;
            }
            shape[y * kMapW + x] = kPiece[mask];
        }
    for (int i = 0; i < kMapW * kMapH; i++)
        if (shape[i]) SetRoad(i % kMapW, i / kMapW, shape[i]);
    // A few straight pieces take their standing-stone look.
    const int swaps[2][2] = {{2, 17}, {1, 16}};
    for (const auto& swap_pair : swaps) {
        int count = Dice(1, 6, 3);
        for (int idx = 0; idx < count; idx++)
            for (int tries = 0; tries < 2000; tries++) {
                int x = Dice(1, kMapW, -1), y = Dice(1, kMapH, -1);
                if (Road(x, y) == swap_pair[0]) {
                    SetRoad(x, y, swap_pair[1]);
                    break;
                }
            }
    }
}

/** @brief Places signposts along roads for the modern scenario generator. */
std::vector<uint8_t> Generator::ModernSigns() {
    std::vector<Point> spots;
    for (int tries = 0; tries < 20000 && static_cast<int>(spots.size()) < 60; tries++) {
        int x = Dice(1, kMapW - 6, 2), y = Dice(1, kMapH - 6, 2);
        if (At(x, y) != kPlain || Road(x, y) != 0 || Tile(x, y) == 0) continue;
        bool by_road = false, clear = true;
        for (int dyy = -1; dyy <= 1; dyy++)
            for (int dxx = -1; dxx <= 1; dxx++)
                if (Road(x + dxx, y + dyy) != 0) by_road = true;
        for (int dyy = -3; dyy <= 3 && clear; dyy++)
            for (int dxx = -3; dxx <= 3 && clear; dxx++) {
                int terrain = At(x + dxx, y + dyy);
                if (terrain == kCity || terrain == kSite) clear = false;
            }
        for (const auto& spot : spots)
            if (Reach(spot.x, spot.y, x, y) < 7) clear = false;
        if (by_road && clear) spots.push_back(Point{x, y});
    }
    int count = static_cast<int>(spots.size());
    std::vector<uint8_t> out(2 + 104 * count, 0);
    SetI16(out, 0, count);
    auto put_str = [&](int offset, std::string_view text) {
        for (size_t i = 0; i < text.size() && i < 49; i++)
            out[offset + i] = static_cast<uint8_t>(text[i]);
    };
    int fixed = 0;
    for (int i = 0; i < count; i++) {
        int x = spots[i].x, y = spots[i].y, offset = 2 + 104 * i;
        SetTile(x, y, 0);
        SetI16(out, offset, x);
        SetI16(out, offset + 2, y);
        if (Dice(1, 10, 0) < 3 && fixed < R16(kSigns)) {
            put_str(offset + 4, Str(kSigns + 2 + 60 * fixed, 30));
            put_str(offset + 54, Str(kSigns + 2 + 60 * fixed + 30, 30));
            fixed++;
            continue;
        }
        int city_idx = -1, best = 1 << 30;
        for (int j = 0; j < CityCount(); j++) {
            int dist = Distance(x, y, Cx(j), Cy(j));
            if (dist < best) {
                best = dist;
                city_idx = j;
            }
        }
        if (city_idx < 0) continue;
        put_str(offset + 4, Format(Str(kSignCity, 20), CstrOf(scn_, Crec(city_idx) + 4, 16)));
        put_str(offset + 54,
                Format(Str(kSignLeagues, 20), Distance(x, y, Cx(city_idx), Cy(city_idx)) * 2,
                       std::string(kCompass[Compass(x, y, Cx(city_idx), Cy(city_idx))])));
    }
    return out;
}

/** @brief Runs one stage of the modern scenario generator, returning progress %. */
std::optional<int> Generator::NextModern(Files& files) {
    switch (stage_++) {
    case 0:
        return 0;
    case 1: {
        plan_ = scengen::MakeWorld(world_, *rng_, kMapW, kMapH);
        for (int i = 0; i < kMapW * kMapH; i++) {
            memory_[kGrid + i] = plan_.terrain[i];
            roads_[i] &= 0xe0;
        }
        Shores();
        return 40;
    }
    case 2: {
        SetCityCount(0);
        for (const auto& [x, y] : plan_.cities) {
            int city_idx = CityCount();
            Ws16(Crec(city_idx), x);
            Ws16(Crec(city_idx) + 2, y);
            SetCityCount(city_idx + 1);
        }
        SitesBegin();
        for (int i = 0; i < kNSites && i < static_cast<int>(plan_.sites.size()); i++)
            SiteAt(i, Point{plan_.sites[i].first, plan_.sites[i].second});
        return 60;
    }
    case 3:
        RoadTiles();
        return 70;
    case 4:
        LayPlannedRoads();
        return 85;
    case 5:
        ModernRoadShapes();
        return 90;
    case 6:
        CityList();
        FairCapitals();
        Sides();
        return 94;
    case 7:
        CityDetails();
        return 96;
    case 8:
        sgn_ = ModernSigns();
        return 98;
    case 9:
        scn_[kSRandom] = 0;
        scn_[kSRandom + 1] = 0;  // Ordinary scenario header flag.
        WriteFiles(files);
        return 100;
    default:
        return std::nullopt;
    }
}

std::optional<int> Generator::Next(Files& files) {
    if (modern_) return NextModern(files);
    switch (stage_++) {
    case 0:
        return 0;
    case 1:
        // The map starts as sea.
        for (int i = 0; i < kMapW * kMapH; i++) {
            memory_[kGrid + i] = kWater;
            roads_[i] &= 0xe0;
        }
        Coast();
        return 10;
    case 2:
        Highlands();
        return 20;
    case 3:
        Erosion();
        return 30;
    case 4:
        Rivers();
        return 40;
    case 5:
        Forests();
        return 50;
    case 6:
        Marshes();
        return 60;
    case 7:
        Cities();
        return 70;
    case 8:
        SitesBegin();
        [[fallthrough]];
    case 9:
        // Advance the progress bar by one point for each tenth of the sites placed.
        while (site_i_ < kNSites) {
            int now = site_i_ * 10 / kNSites;
            if (now != tenth_) {
                tenth_ = now;
                stage_ = 9;
                return 70 + now;
            }
            Site(site_i_++);
        }
        return 80;
    case 10:
        return 81;
    case 11:
        RoadTiles();
        return 82;
    case 12:
        RoadCrossings();
        return 83;
    case 13:
        if (auto pct = RoadsUntilShown()) {
            stage_ = 13;
            return *pct;
        }
        return 90;
    case 14:
        RoadShapes();
        return 92;
    case 15:
        CityList();
        if (fair_)
            FairCapitals();
        else
            Capitals();
        Sides();
        return 94;
    case 16:
        CityDetails();
        return 96;
    case 17:
        sgn_ = Signs();
        return 98;
    case 18:
        scn_[kSRandom] = 1;
        scn_[kSRandom + 1] = 0;
        WriteFiles(files);
        return 100;
    default:
        return std::nullopt;
    }
}

std::array<int, 4> Settle(std::array<int, 4> sliders, Rng& rng) {
    for (int& val : sliders) {
        if (val == kRandomSlider) val = rng.Dice(1, 7, -1);
    }
    return sliders;
}

scengen::Settings SettingsFromSliders(const std::array<int, 4>& sliders, scengen::Shape shape) {
    scengen::Settings world;
    world.shape = shape;
    int water = std::clamp(sliders[0], 0, 6);
    int hills = std::clamp(sliders[1], 0, 6);
    int cities = std::clamp(sliders[2], 0, 6);
    int forest = std::clamp(sliders[3], 0, 6);

    double default_land = shape == scengen::Shape::kPangaea   ? 0.60
                          : shape == scengen::Shape::kIslands ? 0.42
                                                              : 0.55;
    world.land = std::clamp(default_land - (water - 3) * 0.03, 0.25, 0.75);
    world.rivers = std::max(2, 2 + water);

    double relief_scale = kSliderShows[1][hills] / 11.0;
    world.mountains = 0.08 * relief_scale;
    world.hills = 0.10 * relief_scale;

    world.cities = kSliderShows[2][cities];
    world.forest = kSliderShows[3][forest] / 100.0;
    return world;
}

namespace {
/** @brief Loads `RANDOM.DAT` and adds the chosen slider offsets to the parameters. */
absl::StatusOr<std::vector<uint8_t>> Params(const std::filesystem::path& data_dir,
                                            const std::array<int, 4>& sliders) {
    auto bytes = ReadBytes(data_dir / "RANDOM/RANDOM.DAT");
    if (!bytes.ok()) return bytes.status();
    std::vector<uint8_t> memory = std::move(*bytes);
    if (memory.size() < static_cast<size_t>(kDatSize)) memory.resize(kDatSize, 0);
    auto add = [&](int offset, int table, int idx) {
        SetI16(memory, offset, GetI16(memory, offset) + GetI16(memory, table + 2 * idx));
    };
    int water = sliders[0], hills = sliders[1], cities = sliders[2], forest = sliders[3];
    add(kPCities, kSliderCities, cities);
    add(kPForest, kSliderForest, forest);
    add(kPHills, kSliderHills, hills);
    add(kPMountains, kSliderHills, hills);
    add(kPWideRivers, kSliderWater, water);
    add(kPRivers, kSliderWater, water);
    return memory;
}
}  // namespace

Run::Run(std::unique_ptr<Generator> gen) : gen_(std::move(gen)) {}

absl::StatusOr<std::unique_ptr<Run>> Run::Create(const Options& opts) {
    if (!opts.rng) {
        return absl::InvalidArgumentError(
                "Failed to start random map generation: Options::rng is null. Provide a non-null "
                "Rng instance.");
    }
    auto sliders = Settle(opts.sliders, *opts.rng);
    auto params = Params(opts.data_dir, sliders);
    if (!params.ok()) return params.status();
    // Erythea's .SCN supplies the template header, army slots, and terrain type table.
    auto tmpl = ReadBytes(opts.data_dir / "ERYTHEA/ERYTHEA.SCN");
    if (!tmpl.ok()) return tmpl.status();
    if (tmpl->size() <= static_cast<size_t>(kSTerrainSet)) {
        return absl::InvalidArgumentError(absl::StrFormat(
                "Failed to load random map template '%s': file is %zu bytes, shorter than header "
                "offset %d. Verify original/ERYTHEA/ERYTHEA.SCN.",
                (opts.data_dir / "ERYTHEA/ERYTHEA.SCN").string(), tmpl->size(), kSTerrainSet));
    }
    Types types;
    if (absl::Status status = armytype::Load(opts.data_dir / "TERRAIN0/ARMYTYPE.DAT", types);
        !status.ok()) {
        return status;
    }
    auto gen = std::make_unique<Generator>(std::move(*params), std::move(*tmpl), std::move(types),
                                           opts);
    return std::unique_ptr<Run>(new Run(std::move(gen)));
}

Run::~Run() = default;

bool Run::Step() {
    auto pct = gen_->Next(files_);
    if (!pct) return false;
    pct_ = *pct;
    return true;
}

absl::StatusOr<Files> GenerateNow(const Options& opts) {
    auto run = Run::Create(opts);
    if (!run.ok()) return run.status();
    while ((*run)->Step()) {
    }
    return (*run)->GetFiles();
}

std::string TerrainSetName(const std::filesystem::path& data_dir, int set) {
    auto bytes = ReadBytes(data_dir / "DATA/TERRAIN.DAT");
    if (!bytes.ok() || set < 0 || set >= U16(*bytes, 0)) return "";
    return Cstr(*bytes, 2 + 52 * static_cast<size_t>(set) + 2, 50);
}

int TerrainSets(const std::filesystem::path& data_dir) {
    auto bytes = ReadBytes(data_dir / "DATA/TERRAIN.DAT");
    return bytes.ok() ? std::max(1, U16(*bytes, 0)) : 1;
}

}  // namespace w2::randommap
