/**
 * @file
 * @brief Implementation of `w2::Map` spatial indices, copy/move rebinding, and domain mutators.
 */
#include "warlords/model/map.hpp"

#include <algorithm>
#include <utility>

#include "absl/log/absl_check.h"

namespace w2 {

Map::Map() = default;

Map::Map(const Map& other)
        : name_(other.name_)
        , sides_(other.sides_)
        , cities_(other.cities_)
        , sites_(other.sites_)
        , items_(other.items_)
        , monsters_(other.monsters_)
        , crossing_(other.crossing_)
        , item_pool_(other.item_pool_)
        , city_text_(other.city_text_)
        , site_text_(other.site_text_)
        , signs_(other.signs_)
        , options_(other.options_)
        , terrain_type_(other.terrain_type_)
        , fight_order_(other.fight_order_)
        , combat_cap_(other.combat_cap_)
        , tiles_(other.tiles_)
        , roads_(other.roads_)
        , source_files_(other.source_files_)
        , width_(other.width_)
        , height_(other.height_)
        , active_x0_(other.active_x0_)
        , active_y0_(other.active_y0_)
        , active_x1_(other.active_x1_)
        , active_y1_(other.active_y1_) {
    RebuildSpatialIndices();
}

Map& Map::operator=(const Map& other) {
    if (this == &other) return *this;
    name_ = other.name_;
    sides_ = other.sides_;
    cities_ = other.cities_;
    sites_ = other.sites_;
    items_ = other.items_;
    monsters_ = other.monsters_;
    crossing_ = other.crossing_;
    item_pool_ = other.item_pool_;
    city_text_ = other.city_text_;
    site_text_ = other.site_text_;
    signs_ = other.signs_;
    options_ = other.options_;
    terrain_type_ = other.terrain_type_;
    fight_order_ = other.fight_order_;
    combat_cap_ = other.combat_cap_;
    tiles_ = other.tiles_;
    roads_ = other.roads_;
    source_files_ = other.source_files_;
    width_ = other.width_;
    height_ = other.height_;
    active_x0_ = other.active_x0_;
    active_y0_ = other.active_y0_;
    active_x1_ = other.active_x1_;
    active_y1_ = other.active_y1_;
    RebuildSpatialIndices();
    return *this;
}

Map::Map(Map&& other) noexcept
        : name_(std::move(other.name_))
        , sides_(std::move(other.sides_))
        , cities_(std::move(other.cities_))
        , sites_(std::move(other.sites_))
        , items_(std::move(other.items_))
        , monsters_(std::move(other.monsters_))
        , crossing_(std::move(other.crossing_))
        , item_pool_(std::move(other.item_pool_))
        , city_text_(std::move(other.city_text_))
        , site_text_(std::move(other.site_text_))
        , signs_(std::move(other.signs_))
        , options_(other.options_)
        , terrain_type_(other.terrain_type_)
        , fight_order_(std::move(other.fight_order_))
        , combat_cap_(other.combat_cap_)
        , tiles_(std::move(other.tiles_))
        , roads_(std::move(other.roads_))
        , source_files_(std::move(other.source_files_))
        , width_(other.width_)
        , height_(other.height_)
        , active_x0_(other.active_x0_)
        , active_y0_(other.active_y0_)
        , active_x1_(other.active_x1_)
        , active_y1_(other.active_y1_) {
    other.city_at_.clear();
    other.city_tile_.clear();
    other.site_at_.clear();
    RebuildSpatialIndices();
}

Map& Map::operator=(Map&& other) noexcept {
    if (this == &other) return *this;
    name_ = std::move(other.name_);
    sides_ = std::move(other.sides_);
    cities_ = std::move(other.cities_);
    sites_ = std::move(other.sites_);
    items_ = std::move(other.items_);
    monsters_ = std::move(other.monsters_);
    crossing_ = std::move(other.crossing_);
    item_pool_ = std::move(other.item_pool_);
    city_text_ = std::move(other.city_text_);
    site_text_ = std::move(other.site_text_);
    signs_ = std::move(other.signs_);
    options_ = other.options_;
    terrain_type_ = other.terrain_type_;
    fight_order_ = std::move(other.fight_order_);
    combat_cap_ = other.combat_cap_;
    tiles_ = std::move(other.tiles_);
    roads_ = std::move(other.roads_);
    source_files_ = std::move(other.source_files_);
    width_ = other.width_;
    height_ = other.height_;
    active_x0_ = other.active_x0_;
    active_y0_ = other.active_y0_;
    active_x1_ = other.active_x1_;
    active_y1_ = other.active_y1_;
    other.city_at_.clear();
    other.city_tile_.clear();
    other.site_at_.clear();
    RebuildSpatialIndices();
    return *this;
}

void Map::SetName(std::string name) {
    name_ = std::move(name);
}

void Map::SetDimensions(int width, int height) {
    ABSL_DCHECK(width > 0 && height > 0);
    width_ = std::max(1, width);
    height_ = std::max(1, height);
    CropActiveArea(active_x0_, active_y0_, std::min(active_x1_, width_ - 1),
                   std::min(active_y1_, height_ - 1));
    RebuildSpatialIndices();
}

void Map::CropActiveArea(int col0, int row0, int col1, int row1) {
    int clamped_x0 = std::clamp(col0, 0, width_ - 1);
    int clamped_y0 = std::clamp(row0, 0, height_ - 1);
    int clamped_x1 = std::clamp(col1, clamped_x0, width_ - 1);
    int clamped_y1 = std::clamp(row1, clamped_y0, height_ - 1);
    active_x0_ = clamped_x0;
    active_y0_ = clamped_y0;
    active_x1_ = clamped_x1;
    active_y1_ = clamped_y1;
}

Side* Map::SideAt(int index) {
    return (index >= 0 && index < static_cast<int>(sides_.size()))
                   ? &sides_[static_cast<size_t>(index)]
                   : nullptr;
}

const Side* Map::SideAt(int index) const {
    return (index >= 0 && index < static_cast<int>(sides_.size()))
                   ? &sides_[static_cast<size_t>(index)]
                   : nullptr;
}

City* Map::CityAt(int index) {
    return (index >= 0 && index < static_cast<int>(cities_.size()))
                   ? &cities_[static_cast<size_t>(index)]
                   : nullptr;
}

const City* Map::CityAt(int index) const {
    return (index >= 0 && index < static_cast<int>(cities_.size()))
                   ? &cities_[static_cast<size_t>(index)]
                   : nullptr;
}

City* Map::CityAtTopLeft(int x, int y) {
    if (!InBounds(x, y) || city_at_.empty()) return nullptr;
    return city_at_[static_cast<size_t>(y) * static_cast<size_t>(width_) + static_cast<size_t>(x)];
}

const City* Map::CityAtTopLeft(int x, int y) const {
    if (!InBounds(x, y) || city_at_.empty()) return nullptr;
    return city_at_[static_cast<size_t>(y) * static_cast<size_t>(width_) + static_cast<size_t>(x)];
}

City* Map::CityAtFootprint(int x, int y) {
    if (!InBounds(x, y) || city_tile_.empty()) return nullptr;
    return city_tile_[static_cast<size_t>(y) * static_cast<size_t>(width_) +
                      static_cast<size_t>(x)];
}

const City* Map::CityAtFootprint(int x, int y) const {
    if (!InBounds(x, y) || city_tile_.empty()) return nullptr;
    return city_tile_[static_cast<size_t>(y) * static_cast<size_t>(width_) +
                      static_cast<size_t>(x)];
}

void Map::SetCities(CityList cities) {
    cities_ = std::move(cities);
    RebuildCityIndices();
}

City& Map::AddCity(City city) {
    cities_.push_back(std::move(city));
    RebuildCityIndices();
    return cities_.back();
}

void Map::TruncateCities(size_t count) {
    if (cities_.size() > count) {
        cities_.resize(count);
        RebuildCityIndices();
    }
}

Site* Map::SiteById(SiteId site_id) {
    int index = site_id.Value();
    return (index >= 0 && index < static_cast<int>(sites_.size()))
                   ? &sites_[static_cast<size_t>(index)]
                   : nullptr;
}

const Site* Map::SiteById(SiteId site_id) const {
    int index = site_id.Value();
    return (index >= 0 && index < static_cast<int>(sites_.size()))
                   ? &sites_[static_cast<size_t>(index)]
                   : nullptr;
}

Site* Map::SiteAt(int x, int y) {
    if (!InBounds(x, y) || site_at_.empty()) return nullptr;
    return site_at_[static_cast<size_t>(y) * static_cast<size_t>(width_) + static_cast<size_t>(x)];
}

const Site* Map::SiteAt(int x, int y) const {
    if (!InBounds(x, y) || site_at_.empty()) return nullptr;
    return site_at_[static_cast<size_t>(y) * static_cast<size_t>(width_) + static_cast<size_t>(x)];
}

void Map::SetSites(SiteList sites) {
    sites_ = std::move(sites);
    RebuildSiteIndex();
}

Site& Map::AddSite(Site site) {
    sites_.push_back(std::move(site));
    RebuildSiteIndex();
    return sites_.back();
}

void Map::RebuildSpatialIndices() {
    RebuildCityIndices();
    RebuildSiteIndex();
}

void Map::RebuildCityIndices() {
    if (cities_.empty()) {
        city_at_.clear();
        city_tile_.clear();
        return;
    }
    const size_t grid_size = static_cast<size_t>(width_) * static_cast<size_t>(height_);
    city_at_.assign(grid_size, nullptr);
    city_tile_.assign(grid_size, nullptr);
    for (City& city : cities_) {
        if (InBounds(city.X(), city.Y())) {
            city_at_[static_cast<size_t>(city.Y()) * static_cast<size_t>(width_) +
                     static_cast<size_t>(city.X())] = &city;
        }
        for (int dx = 0; dx <= 1; ++dx) {
            for (int dy = 0; dy <= 1; ++dy) {
                int tile_x = city.X() + dx;
                int tile_y = city.Y() + dy;
                if (InBounds(tile_x, tile_y)) {
                    city_tile_[static_cast<size_t>(tile_y) * static_cast<size_t>(width_) +
                               static_cast<size_t>(tile_x)] = &city;
                }
            }
        }
    }
}

void Map::RebuildSiteIndex() {
    if (sites_.empty()) {
        site_at_.clear();
        return;
    }
    const size_t grid_size = static_cast<size_t>(width_) * static_cast<size_t>(height_);
    site_at_.assign(grid_size, nullptr);
    for (Site& site : sites_) {
        if (InBounds(site.x, site.y)) {
            site_at_[static_cast<size_t>(site.y) * static_cast<size_t>(width_) +
                     static_cast<size_t>(site.x)] = &site;
        }
    }
}

Item* Map::ItemById(ItemId item_id) {
    int index = item_id.Value();
    if (index < 0) return nullptr;
    if (index < static_cast<int>(items_.size()) &&
        items_[static_cast<size_t>(index)].index == index) {
        return &items_[static_cast<size_t>(index)];
    }
    for (Item& item : items_) {
        if (item.index == index) return &item;
    }
    return nullptr;
}

const Item* Map::ItemById(ItemId item_id) const {
    int index = item_id.Value();
    if (index < 0) return nullptr;
    if (index < static_cast<int>(items_.size()) &&
        items_[static_cast<size_t>(index)].index == index) {
        return &items_[static_cast<size_t>(index)];
    }
    for (const Item& item : items_) {
        if (item.index == index) return &item;
    }
    return nullptr;
}

void Map::SetItems(ItemList items) {
    items_ = std::move(items);
}

Item& Map::AddItem(Item item) {
    items_.push_back(std::move(item));
    return items_.back();
}

void Map::SetItemPool(OptionalItemPool pool) {
    item_pool_ = std::move(pool);
}

const Monster* Map::MonsterAt(int index) const {
    if (index < 0 || index >= static_cast<int>(monsters_.size())) return nullptr;
    const OptionalMonster& slot = monsters_[static_cast<size_t>(index)];
    return slot.has_value() ? &*slot : nullptr;
}

void Map::SetMonster(int index, OptionalMonster monster) {
    if (index >= 0 && index < static_cast<int>(monsters_.size())) {
        monsters_[static_cast<size_t>(index)] = std::move(monster);
    }
}

void Map::SetTerrainType(int tile_byte, int terrain_id) {
    if (tile_byte >= 0 && tile_byte < static_cast<int>(terrain_type_.size())) {
        terrain_type_[static_cast<size_t>(tile_byte)] = terrain_id;
    }
}

void Map::SetFightOrder(FightOrderTable fight_order) {
    fight_order_ = std::move(fight_order);
}

int Map::TileWord(int x, int y) const {
    if (!InBounds(x, y)) return 0;
    size_t idx = static_cast<size_t>(y) * static_cast<size_t>(width_) + static_cast<size_t>(x);
    return idx < tiles_.size() ? tiles_[idx] : 0;
}

void Map::SetTileWord(int x, int y, int word) {
    if (!InBounds(x, y)) return;
    const size_t grid_size = static_cast<size_t>(width_) * static_cast<size_t>(height_);
    if (tiles_.size() < grid_size) tiles_.resize(grid_size, 0);
    tiles_[static_cast<size_t>(y) * static_cast<size_t>(width_) + static_cast<size_t>(x)] = word;
}

void Map::SetTiles(TileGrid tiles) {
    tiles_ = std::move(tiles);
}

uint8_t Map::RoadByte(int x, int y) const {
    if (!InBounds(x, y)) return 0;
    size_t idx = static_cast<size_t>(y) * static_cast<size_t>(width_) + static_cast<size_t>(x);
    return idx < roads_.size() ? roads_[idx] : 0;
}

void Map::SetRoadByte(int x, int y, uint8_t road) {
    if (!InBounds(x, y)) return;
    const size_t grid_size = static_cast<size_t>(width_) * static_cast<size_t>(height_);
    if (roads_.size() < grid_size) roads_.resize(grid_size, 0);
    roads_[static_cast<size_t>(y) * static_cast<size_t>(width_) + static_cast<size_t>(x)] = road;
}

void Map::SetRoads(RoadGrid roads) {
    roads_ = std::move(roads);
}

bool Map::IsCrossing(int x, int y) const {
    if (!InBounds(x, y)) return false;
    size_t idx = static_cast<size_t>(y) * static_cast<size_t>(width_) + static_cast<size_t>(x);
    return idx < crossing_.size() ? crossing_[idx] : false;
}

void Map::SetCrossing(int x, int y, bool is_crossing) {
    if (!InBounds(x, y)) return;
    const size_t grid_size = static_cast<size_t>(width_) * static_cast<size_t>(height_);
    if (crossing_.size() < grid_size) crossing_.resize(grid_size, false);
    crossing_[static_cast<size_t>(y) * static_cast<size_t>(width_) + static_cast<size_t>(x)] =
            is_crossing;
}

void Map::SetCrossings(CrossingGrid crossings) {
    crossing_ = std::move(crossings);
}

void Map::SetSourceFiles(ScenarioFiles files) {
    source_files_ = std::move(files);
}

const Map::DescriptionLines* Map::CityDescription(int city_index) const {
    auto found = city_text_.find(city_index);
    return found != city_text_.end() ? &found->second : nullptr;
}

void Map::SetCityDescriptions(DescriptionMap descriptions) {
    city_text_ = std::move(descriptions);
}

const Map::DescriptionLines* Map::SiteDescription(int site_index) const {
    auto found = site_text_.find(site_index);
    return found != site_text_.end() ? &found->second : nullptr;
}

void Map::SetSiteDescriptions(DescriptionMap descriptions) {
    site_text_ = std::move(descriptions);
}

Sign* Map::SignAt(int x, int y) {
    for (Sign& sign : signs_) {
        if (sign.x == x && sign.y == y) return &sign;
    }
    return nullptr;
}

const Sign* Map::SignAt(int x, int y) const {
    for (const Sign& sign : signs_) {
        if (sign.x == x && sign.y == y) return &sign;
    }
    return nullptr;
}

void Map::SetSigns(SignList signs) {
    signs_ = std::move(signs);
}

Sign& Map::AddSign(Sign sign) {
    signs_.push_back(std::move(sign));
    return signs_.back();
}

}  // namespace w2
