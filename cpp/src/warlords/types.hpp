/**
 * @file
 * @brief Umbrella header for the domain model structs (`warlords/model/`).
 *
 * Each domain entity now lives in its own focused header under
 * `warlords/model/`:
 *   - `army_type.hpp`: `ArmyType`, `Types`, `kNone`
 *   - `city.hpp`: `Slot`, `City`
 *   - `item.hpp`: `Item`, `PoolItem`
 *   - `site.hpp`: `Site`, `Monster`, `Sign`
 *   - `options.hpp`: `Options`
 *   - `side.hpp`: `Produced`, `Quest`, `Advisor`, `Promotion`, `AIThought`,
 *     `DiplomaticMissive`, `Side`
 *   - `army.hpp`: `Transit`, `Army`
 *   - `map.hpp`: `kMapW`, `kMapH`, `Map`
 *   - `history_types.hpp`: `DeedType`, `Deed`, `HistoryRecord`, `Fallen`, `Ending`
 *   - `diplomacy_state.hpp`: `Diplomacy`
 *   - `game_state.hpp`: `Game`
 */
#pragma once

#include "util/json.hpp"
#include "warlords/model/army.hpp"
#include "warlords/model/army_type.hpp"
#include "warlords/model/city.hpp"
#include "warlords/model/diplomacy_state.hpp"
#include "warlords/model/game_state.hpp"
#include "warlords/model/history_types.hpp"
#include "warlords/model/item.hpp"
#include "warlords/model/map.hpp"
#include "warlords/model/options.hpp"
#include "warlords/model/side.hpp"
#include "warlords/model/site.hpp"
#include "warlords/model/tile_pos.hpp"
#include "warlords/rng.hpp"
