/**
 * @file
 * @brief Umbrella header for the domain model entities and subsystems (`warlords/model/`).
 *
 * Each domain entity or subsystem lives in its own focused header under
 * `warlords/model/`:
 *   - `ids.hpp`: `SideId`, `CityId`, `ArmyId`, `ArmyTypeId`, `SiteId`, `ItemId`, `VectorTarget`
 *   - `army_type.hpp`: `ArmyType`, `Types`
 *   - `city.hpp`: `Slot`, `City`
 *   - `item.hpp`: `Item`, `PoolItem`
 *   - `site.hpp`: `Site`, `Monster`, `Sign`
 *   - `options.hpp`: `Options`
 *   - `quest.hpp`: `Quest`
 *   - `turn_report.hpp`: `ProducedKind`, `Produced`, `Promotion`, `TurnReport`
 *   - `computer_player.hpp`: `AttitudeFactor`, `AttitudeBreakdown`, `IComputerPlayer`
 *   - `side.hpp`: `Advisor`, `AIThought`, `DiplomaticMissive`, `Side`
 *   - `army.hpp`: `Transit`, `Army`
 *   - `army_registry.hpp`: `ArmyRegistry`
 *   - `map.hpp`: `kMapW`, `kMapH`, `Map`
 *   - `history_types.hpp`: `DeedType`, `Deed`, `HistoryRecord`, `Fallen`, `Ending`
 *   - `diplomacy_state.hpp`: `DiplomacyMatrix`
 *   - `fog_of_war.hpp`: `FogOfWar`
 *   - `turn_schedule.hpp`: `TurnSchedule`
 *   - `game_state.hpp`: `Bugs`, `Game`
 */
#pragma once

#include "warlords/model/army.hpp"
#include "warlords/model/army_registry.hpp"
#include "warlords/model/army_type.hpp"
#include "warlords/model/city.hpp"
#include "warlords/model/computer_player.hpp"
#include "warlords/model/diplomacy_state.hpp"
#include "warlords/model/fog_of_war.hpp"
#include "warlords/model/game_state.hpp"
#include "warlords/model/history_types.hpp"
#include "warlords/model/ids.hpp"
#include "warlords/model/item.hpp"
#include "warlords/model/map.hpp"
#include "warlords/model/options.hpp"
#include "warlords/model/quest.hpp"
#include "warlords/model/side.hpp"
#include "warlords/model/site.hpp"
#include "warlords/model/tile_pos.hpp"
#include "warlords/model/turn_report.hpp"
#include "warlords/model/turn_schedule.hpp"
#include "warlords/rng.hpp"
