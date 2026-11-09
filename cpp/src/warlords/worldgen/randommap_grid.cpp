/**
 * @file
 * @brief Geometry helpers, binary buffer accessors, and `Generator` grid/dice primitives.
 */
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "absl/log/absl_check.h"

#include "warlords/worldgen/randommap_impl.hpp"

namespace w2::randommap {

int Reach(int from_x, int from_y, int to_x, int to_y) {
    return std::max(std::abs(from_x - to_x), std::abs(from_y - to_y));
}

int Distance(int from_x, int from_y, int to_x, int to_y) {
    double dx = from_x - to_x, dy = from_y - to_y;
    return static_cast<int>(std::floor(std::sqrt(dx * dx + dy * dy)));
}

Point Step(int from_x, int from_y, int to_x, int to_y) {
    if (to_x == from_x) return Point{0, to_y == from_y ? 0 : (to_y < from_y ? -1 : 1)};
    if (to_y == from_y) return Point{to_x < from_x ? -1 : 1, 0};
    double slope = static_cast<double>(to_y - from_y) / static_cast<double>(to_x - from_x);
    if (to_x < from_x) {
        if (slope >= 2.414) return Point{0, -1};
        if (slope >= 0.414) return Point{-1, -1};
        if (slope >= -0.414) return Point{-1, 0};
        if (slope >= -2.414) return Point{-1, 1};
        return Point{0, 1};
    }
    if (slope >= 2.414) return Point{0, 1};
    if (slope >= 0.414) return Point{1, 1};
    if (slope >= -0.414) return Point{1, 0};
    if (slope >= -2.414) return Point{1, -1};
    return Point{0, -1};
}

int Compass(int from_x, int from_y, int to_x, int to_y) {
    if (from_x == to_x) return from_y < to_y ? 4 : 0;
    if (from_y == to_y) return from_x < to_x ? 2 : 6;
    if (to_x < from_x && to_y < from_y) return 7;
    if (to_x < from_x && from_y < to_y) return 5;
    if (from_x < to_x && to_y < from_y) return 1;
    return 3;
}

int Parity(int x, int y) {
    int sum = x + y;
    return sum == 0 || sum / 2 != (sum - 1) / 2 ? 1 : 0;
}

void Clamp(Point& point) {
    if (point.x < 1) point.x = 0;
    if (point.y < 1) point.y = 0;
    if (point.x > kMapW - 1) point.x = kMapW - 1;
    if (point.y > kMapH - 1) point.y = kMapH - 1;
}

int GetI16(absl::Span<const uint8_t> table, int offset) {
    return static_cast<int16_t>(table[offset] | (table[offset + 1] << 8));
}

void SetI16(std::vector<uint8_t>& table, int offset, int value) {
    table[offset] = static_cast<uint8_t>(value & 0xff);
    table[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xff);
}

std::string CstrOf(absl::Span<const uint8_t> table, int offset, int max_len) {
    std::string out;
    for (int i = offset; i < offset + max_len && i < static_cast<int>(table.size()); i++) {
        if (table[i] == 0) break;
        out += static_cast<char>(table[i]);
    }
    return out;
}

Generator::Generator(std::vector<uint8_t> memory, std::vector<uint8_t> scn, Types types,
                     const Options& opts)
        : memory_(std::move(memory))
        , scn_(std::move(scn))
        , tiles_(static_cast<size_t>(kMapW) * kMapH, 0)
        , roads_(static_cast<size_t>(kMapW) * kMapH, 0)
        , types_(std::move(types))
        , world_(opts.world)
        , rng_(opts.rng)
        , allies_(opts.allies)
        , fair_(opts.fair_capitals)
        , modern_(opts.scenario) {
    ABSL_DCHECK(rng_ != nullptr);
    ABSL_DCHECK_GE(memory_.size(), static_cast<size_t>(kDatSize));
    ABSL_DCHECK_GT(scn_.size(), static_cast<size_t>(kSTerrainSet));
    scn_[kSTerrainSet] = static_cast<uint8_t>(opts.terrain_set);
}

std::string Generator::Str(int offset, int max_len) const {
    return CstrOf(memory_, offset, max_len);
}

void Generator::Wstr(int offset, std::string_view text) {
    for (size_t i = 0; i < text.size(); i++) scn_[offset + i] = static_cast<uint8_t>(text[i]);
    scn_[offset + text.size()] = 0;
}

int Generator::At(int x, int y) const {
    // Matches the reference: out-of-grid reads inspect the surrounding RANDOM.DAT bytes.
    int offset = kGrid + y * kMapW + x;
    return offset >= 0 && offset < kDatSize ? memory_[offset] : 0;
}

void Generator::Put(int x, int y, int terrain) {
    int idx = y * kMapW + x;
    if (idx >= 0 && idx < kMapW * kMapH) memory_[kGrid + idx] = static_cast<uint8_t>(terrain);
}

int Generator::NbIndex(int dx, int dy) const {
    int found = -1;
    for (int i = 0; i < 8; i++)
        if (Nx(i) == dx && Ny(i) == dy) found = i;
    return found;
}

int Generator::Tile(int x, int y) const {
    return x >= 0 && y >= 0 && x < kMapW && y < kMapH ? tiles_[y * kMapW + x] & 0xff : 0;
}

void Generator::SetTile(int x, int y, int tile_byte) {
    if (x < 0 || y < 0 || x >= kMapW || y >= kMapH) return;
    uint16_t& word = tiles_[y * kMapW + x];
    word = static_cast<uint16_t>((word & 0xff00) | (tile_byte & 0xff));
}

int Generator::Road(int x, int y) const {
    return x >= 0 && y >= 0 && x < kMapW && y < kMapH ? roads_[y * kMapW + x] & 0x1f : 0;
}

void Generator::SetRoad(int x, int y, int piece) {
    if (x < 0 || y < 0 || x >= kMapW || y >= kMapH) return;
    uint8_t& byte_val = roads_[y * kMapW + x];
    byte_val = static_cast<uint8_t>((byte_val & 0xe0) | (piece & 0x1f));
}

int Generator::TileFor(int terrain, int variant, int x, int y) const {
    return R16(kTiles + 64 * terrain + 4 * variant + 2 * Parity(x, y)) & 0xff;
}

int Generator::Dice(int count, int sides, int bonus) {
    if (sides == 0) return bonus;
    if (sides < 0) return count + bonus;
    return rng_->Dice(count, sides, bonus);
}

bool Generator::Near(int x, int y, int terrain) const {
    for (int i = 0; i < 8; i++) {
        int adj_x = x + Nx(i), adj_y = y + Ny(i);
        if (adj_x >= 0 && adj_y >= 0 && adj_x < kMapW && adj_y < kMapH &&
            At(adj_x, adj_y) == terrain)
            return true;
    }
    return false;
}

void Generator::Jitter(Point& point, int range, bool to_edge) {
    int step_x = Dice(1, 3, -2);
    point.x += step_x * Dice(1, range, 0);
    int step_y = Dice(1, 3, -2);
    point.y += step_y * Dice(1, range, 0);
    if (Dice(1, 100, 0) < 30 && to_edge) {
        int left = point.x, top = point.y;
        int right = kMapW - 1 - point.x, bottom = kMapH - 1 - point.y;
        if (left <= top && left <= right && left <= bottom) point.x = 0;
        if (top <= left && top <= right && top <= bottom) point.y = 0;
        if (right <= top && right <= left && right <= bottom) point.x = kMapW - 1;
        if (bottom <= top && bottom <= left && bottom <= right) point.y = kMapH - 1;
    }
    Clamp(point);
}

void Generator::SetPt(int offset, Point point) {
    W16(offset, point.x);
    W16(offset + 2, point.y);
}

}  // namespace w2::randommap
