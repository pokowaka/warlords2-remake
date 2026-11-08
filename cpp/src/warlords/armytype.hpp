// TERRAIN0/ARMYTYPE.DAT -- the 29 army types.
// See docs/formats/armytype.md. Records are keyed by their type id (+0), not
// by position in the file: the file is in display order.
#pragma once

#include <string>

#include "warlords/types.hpp"

namespace w2::armytype {

// bonus field offsets worth naming (docs/formats/armytype.md, docs/rules.md)
constexpr int kSiege = 52;  // value 1 = siege ability (+2 strength vs cities)
constexpr int kFlies = 54;
constexpr int kWoodsMove = 56;
constexpr int kHillsMove = 58;
constexpr int kBoat = 60;

constexpr int kNavy = 5;  // removed from every city's production at game start
constexpr int kHero = 28;
constexpr int kScouts = 11;  // placeholder garrison when Neutral Cities is off

void Load(const std::string& path, Types& out);

}  // namespace w2::armytype
