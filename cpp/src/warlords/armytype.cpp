#include "warlords/armytype.hpp"

#include "absl/strings/str_format.h"
#include "absl/types/span.h"

#include "util/util.hpp"
#include "warlords/bytes.hpp"

namespace w2::armytype {

absl::Status Load(const std::filesystem::path& path, Types& out) {
    constexpr int kCount = 29;
    constexpr int kStride = 62;
    constexpr int kBonusCount = 15;
    auto file_bytes = ReadBytes(path);
    if (!file_bytes.ok()) return file_bytes.status();
    absl::Span<const uint8_t> bytes(*file_bytes);
    if (bytes.size() != static_cast<size_t>(kCount * kStride)) {
        return absl::InvalidArgumentError(absl::StrFormat(
                "Failed to load army types from '%s': expected %d bytes (%d records x %d bytes), "
                "got %zu bytes; verify TERRAIN0/ARMYTYPE.DAT in your --data directory.",
                path.string(), kCount * kStride, kCount, kStride, bytes.size()));
    }
    out.all.clear();
    out.all.reserve(kCount);
    for (int i = 0; i < kCount; i++) {
        size_t record = static_cast<size_t>(i) * kStride;
        ArmyType army_type;
        // signed: the Elephants' -1 to the enemy stack (+50) is 0xffff
        for (int bonus = 0; bonus < kBonusCount; bonus++) {
            army_type.bonus[32 + bonus * 2] =
                    I16(bytes, record + 32 + static_cast<size_t>(bonus) * 2);
        }
        army_type.id = U16(bytes, record);
        army_type.name = Cstr(bytes, record + 2, 16);
        army_type.strength = U16(bytes, record + 22);
        army_type.time = U16(bytes, record + 24);
        army_type.cost = U16(bytes, record + 26);
        army_type.move = U16(bytes, record + 28);
        army_type.price = I16(bytes, record + 30);
        army_type.flies = army_type.bonus[kFlies] != 0;
        army_type.siege = army_type.bonus[kSiege] == 1;
        army_type.boat = army_type.bonus[kBoat] != 0;
        army_type.woods_move = army_type.bonus[kWoodsMove] != 0;
        army_type.hills_move = army_type.bonus[kHillsMove] != 0;
        out.all.push_back(army_type);
    }
    out.ids.fill(nullptr);
    for (auto& army_type : out.all) {
        if (army_type.id >= 0 && army_type.id < 64) out.ids[army_type.id] = &army_type;
    }
    return absl::OkStatus();
}

}  // namespace w2::armytype
