#include "warlords/armytype.hpp"

#include <stdexcept>

#include "util/util.hpp"
#include "warlords/bytes.hpp"

namespace w2::armytype {

void Load(const std::string& path, Types& out) {
    const int count = 29, stride = 62, n_bonus = 15;
    std::string bytes = MustRead(path);
    if (static_cast<int>(bytes.size()) != count * stride)
        throw std::runtime_error(
                Fmt("unexpected %s size: %d", path.c_str(), static_cast<int>(bytes.size())));
    out.all.clear();
    out.all.reserve(count);
    for (int i = 0; i < count; i++) {
        int record = i * stride;
        ArmyType army_type;
        // signed: the Elephants' -1 to the enemy stack (+50) is 0xffff
        for (int bonus = 0; bonus < n_bonus; bonus++)
            army_type.bonus[32 + bonus * 2] = I16(bytes, record + 32 + bonus * 2);
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
    for (auto& army_type : out.all)
        if (army_type.id >= 0 && army_type.id < 64) out.ids[army_type.id] = &army_type;
}

}  // namespace w2::armytype
