#include "warlords/armytype.hpp"

#include <stdexcept>

#include "util/util.hpp"
#include "warlords/bytes.hpp"

namespace w2::armytype {

void Load(const std::string& path, Types& out) {
    const int count = 29, stride = 62, n_bonus = 15;
    std::string s = MustRead(path);
    if (static_cast<int>(s.size()) != count * stride)
        throw std::runtime_error(
                Fmt("unexpected %s size: %d", path.c_str(), static_cast<int>(s.size())));
    out.all.clear();
    out.all.reserve(count);
    for (int i = 0; i < count; i++) {
        int o = i * stride;
        ArmyType a;
        // signed: the Elephants' -1 to the enemy stack (+50) is 0xffff
        for (int b = 0; b < n_bonus; b++) a.bonus[32 + b * 2] = I16(s, o + 32 + b * 2);
        a.id = U16(s, o);
        a.name = Cstr(s, o + 2, 16);
        a.strength = U16(s, o + 22);
        a.time = U16(s, o + 24);
        a.cost = U16(s, o + 26);
        a.move = U16(s, o + 28);
        a.price = I16(s, o + 30);
        a.flies = a.bonus[kFlies] != 0;
        a.siege = a.bonus[kSiege] == 1;
        a.boat = a.bonus[kBoat] != 0;
        a.woods_move = a.bonus[kWoodsMove] != 0;
        a.hills_move = a.bonus[kHillsMove] != 0;
        out.all.push_back(a);
    }
    out.ids.fill(nullptr);
    for (auto& a : out.all)
        if (a.id >= 0 && a.id < 64) out.ids[a.id] = &a;
}

}  // namespace w2::armytype
