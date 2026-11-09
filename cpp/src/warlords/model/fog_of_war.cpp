/**
 * @file
 * @brief Implementation of `w2::FogOfWar`.
 */
#include "warlords/model/fog_of_war.hpp"

#include <algorithm>
#include <cstddef>

namespace w2 {

void FogOfWar::Clear() {
    masks_.clear();
}

bool FogOfWar::HasMask(SideId side) const {
    return masks_.contains(side);
}

void FogOfWar::ResetSide(SideId side, int map_width, int map_height, bool explored) {
    if (map_width <= 0 || map_height <= 0) return;
    size_t total_tiles = static_cast<size_t>(map_width) * static_cast<size_t>(map_height);
    masks_[side].assign(total_tiles, explored ? uint8_t{1} : uint8_t{0});
}

void FogOfWar::EnsureSide(SideId side, int map_width, int map_height) {
    if (map_width <= 0 || map_height <= 0) return;
    size_t total_tiles = static_cast<size_t>(map_width) * static_cast<size_t>(map_height);
    auto [mask_it, inserted] = masks_.try_emplace(side);
    if (inserted || mask_it->second.size() != total_tiles) {
        mask_it->second.assign(total_tiles, uint8_t{0});
    }
}

absl::Span<const uint8_t> FogOfWar::MaskFor(SideId side) const {
    auto found = masks_.find(side);
    if (found == masks_.end()) return {};
    return found->second;
}

void FogOfWar::SetMask(SideId side, std::vector<uint8_t> mask) {
    for (uint8_t& entry : mask) {
        entry = entry != 0 ? uint8_t{1} : uint8_t{0};
    }
    masks_[side] = std::move(mask);
}

bool FogOfWar::IsExplored(SideId side, int map_width, int map_height, int x, int y) const {
    if (x < 0 || y < 0 || x >= map_width || y >= map_height) return false;
    auto found = masks_.find(side);
    if (found == masks_.end()) return false;
    size_t tile_index =
            static_cast<size_t>(y) * static_cast<size_t>(map_width) + static_cast<size_t>(x);
    return tile_index < found->second.size() && found->second[tile_index] != 0;
}

bool FogOfWar::MarkExplored(SideId side, int map_width, int map_height, int x, int y) {
    if (x < 0 || y < 0 || x >= map_width || y >= map_height) return false;
    EnsureSide(side, map_width, map_height);
    auto& mask = masks_[side];
    size_t tile_index =
            static_cast<size_t>(y) * static_cast<size_t>(map_width) + static_cast<size_t>(x);
    if (mask[tile_index] != 0) return false;
    mask[tile_index] = 1;
    return true;
}

int FogOfWar::ExploredCount(SideId side) const {
    auto found = masks_.find(side);
    if (found == masks_.end()) return 0;
    int count = 0;
    for (uint8_t seen : found->second) {
        if (seen != 0) ++count;
    }
    return count;
}

std::vector<SideId> FogOfWar::SidesWithMasks() const {
    std::vector<SideId> sides;
    sides.reserve(masks_.size());
    for (const auto& [side_id, mask] : masks_) {
        (void)mask;
        sides.push_back(side_id);
    }
    std::sort(sides.begin(), sides.end());
    return sides;
}

}  // namespace w2
