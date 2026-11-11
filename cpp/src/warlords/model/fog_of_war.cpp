/**
 * @file
 * @brief Implementation of `w2::FogOfWar`.
 */
#include "warlords/model/fog_of_war.hpp"

#include <algorithm>
#include <cstddef>

namespace w2 {

namespace {

constexpr bool IsInlineSide(SideId side) {
    return static_cast<unsigned int>(side.Value()) < FogOfWar::kInlineSides;
}

}  // namespace

void FogOfWar::Clear() {
    for (size_t i = 0; i < kInlineSides; ++i) {
        side_masks_[i].clear();
        has_mask_[i] = false;
    }
    extra_masks_.clear();
    ++version_;
}

bool FogOfWar::HasMask(SideId side) const {
    if (IsInlineSide(side)) {
        return has_mask_[static_cast<size_t>(side.Value())];
    }
    return extra_masks_.contains(side);
}

void FogOfWar::ResetSide(SideId side, int map_width, int map_height, bool explored) {
    if (map_width <= 0 || map_height <= 0) return;
    size_t total_tiles = static_cast<size_t>(map_width) * static_cast<size_t>(map_height);
    const uint8_t fill_val = explored ? uint8_t{1} : uint8_t{0};
    ++version_;
    if (IsInlineSide(side)) {
        size_t idx = static_cast<size_t>(side.Value());
        side_masks_[idx].assign(total_tiles, fill_val);
        has_mask_[idx] = true;
        return;
    }
    extra_masks_[side].assign(total_tiles, fill_val);
}

void FogOfWar::EnsureSide(SideId side, int map_width, int map_height) {
    if (map_width <= 0 || map_height <= 0) return;
    size_t total_tiles = static_cast<size_t>(map_width) * static_cast<size_t>(map_height);
    if (IsInlineSide(side)) {
        size_t idx = static_cast<size_t>(side.Value());
        if (!has_mask_[idx] || side_masks_[idx].size() != total_tiles) {
            side_masks_[idx].assign(total_tiles, uint8_t{0});
            has_mask_[idx] = true;
        }
        return;
    }
    auto [mask_it, inserted] = extra_masks_.try_emplace(side);
    if (inserted || mask_it->second.size() != total_tiles) {
        mask_it->second.assign(total_tiles, uint8_t{0});
    }
}

absl::Span<const uint8_t> FogOfWar::MaskFor(SideId side) const {
    if (IsInlineSide(side)) {
        size_t idx = static_cast<size_t>(side.Value());
        return has_mask_[idx] ? absl::Span<const uint8_t>(side_masks_[idx])
                              : absl::Span<const uint8_t>{};
    }
    auto found = extra_masks_.find(side);
    if (found == extra_masks_.end()) return {};
    return found->second;
}

absl::Span<uint8_t> FogOfWar::MutableMaskFor(SideId side) {
    if (IsInlineSide(side)) {
        size_t idx = static_cast<size_t>(side.Value());
        return has_mask_[idx] ? absl::MakeSpan(side_masks_[idx]) : absl::Span<uint8_t>{};
    }
    auto found = extra_masks_.find(side);
    if (found == extra_masks_.end()) return {};
    return absl::MakeSpan(found->second);
}

void FogOfWar::SetMask(SideId side, std::vector<uint8_t> mask) {
    for (uint8_t& entry : mask) {
        entry = entry != 0 ? uint8_t{1} : uint8_t{0};
    }
    ++version_;
    if (IsInlineSide(side)) {
        size_t idx = static_cast<size_t>(side.Value());
        side_masks_[idx] = std::move(mask);
        has_mask_[idx] = true;
        return;
    }
    extra_masks_[side] = std::move(mask);
}

bool FogOfWar::IsExplored(SideId side, int map_width, int map_height, int x, int y) const {
    if (static_cast<unsigned int>(x) >= static_cast<unsigned int>(map_width) ||
        static_cast<unsigned int>(y) >= static_cast<unsigned int>(map_height)) {
        return false;
    }
    size_t tile_index =
            static_cast<size_t>(y) * static_cast<size_t>(map_width) + static_cast<size_t>(x);
    if (IsInlineSide(side)) {
        size_t idx = static_cast<size_t>(side.Value());
        if (!has_mask_[idx]) return false;
        const auto& mask = side_masks_[idx];
        return tile_index < mask.size() && mask[tile_index] != 0;
    }
    auto found = extra_masks_.find(side);
    if (found == extra_masks_.end()) return false;
    return tile_index < found->second.size() && found->second[tile_index] != 0;
}

bool FogOfWar::MarkExplored(SideId side, int map_width, int map_height, int x, int y) {
    if (static_cast<unsigned int>(x) >= static_cast<unsigned int>(map_width) ||
        static_cast<unsigned int>(y) >= static_cast<unsigned int>(map_height)) {
        return false;
    }
    size_t tile_index =
            static_cast<size_t>(y) * static_cast<size_t>(map_width) + static_cast<size_t>(x);
    if (IsInlineSide(side)) {
        size_t idx = static_cast<size_t>(side.Value());
        size_t total_tiles = static_cast<size_t>(map_width) * static_cast<size_t>(map_height);
        auto& mask = side_masks_[idx];
        if (!has_mask_[idx] || mask.size() != total_tiles) {
            mask.assign(total_tiles, uint8_t{0});
            has_mask_[idx] = true;
        }
        if (mask[tile_index] != 0) return false;
        mask[tile_index] = 1;
        ++version_;
        return true;
    }
    EnsureSide(side, map_width, map_height);
    auto& mask = extra_masks_[side];
    if (mask[tile_index] != 0) return false;
    mask[tile_index] = 1;
    ++version_;
    return true;
}

int FogOfWar::ExploredCount(SideId side) const {
    absl::Span<const uint8_t> mask = MaskFor(side);
    int count = 0;
    for (uint8_t seen : mask) {
        if (seen != 0) ++count;
    }
    return count;
}

std::vector<SideId> FogOfWar::SidesWithMasks() const {
    std::vector<SideId> sides;
    sides.reserve(kInlineSides + extra_masks_.size());
    for (size_t i = 0; i < kInlineSides; ++i) {
        if (has_mask_[i]) sides.emplace_back(static_cast<int>(i));
    }
    for (const auto& [side_id, mask] : extra_masks_) {
        (void)mask;
        sides.push_back(side_id);
    }
    std::sort(sides.begin(), sides.end());
    return sides;
}

}  // namespace w2
