/**
 * @file
 * @brief Modern AI EconNet economy phase (retention, slot buying, production, vectoring) and hero
 * city selection.
 */
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <variant>

#include "absl/strings/str_format.h"

#include "util/util.hpp"
#include "warlords/ai.hpp"
#include "warlords/ai/belief.hpp"
#include "warlords/ai/core.hpp"
#include "warlords/ai/groups.hpp"
#include "warlords/ai/modern_internal.hpp"
#include "warlords/ai/modern_ml_player.hpp"
#include "warlords/ai/personality.hpp"
#include "warlords/combat.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/game.hpp"
#include "warlords/hero.hpp"
#include "warlords/move.hpp"
#include "warlords/quest.hpp"
#include "warlords/rules.hpp"
#include "warlords/scn.hpp"
#include "warlords/site.hpp"
#include "warlords/slots.hpp"

namespace w2::ai::modern {

/**
 * @brief Uses EconNet to decide unit retention/disbanding, empire-wide production slot purchases,
 * city production, and vectoring.
 */
void RunEconomyPhase(Game& game, Side& side, const ml::PolicyNetwork& policy,
                     const ml::ContextVec& ctx, ml::RolloutSampler* sampler) {
    (void)ctx;
    int our_side = side.Index();
    const SideBelief& belief = belief::Ensure(side);

    auto make_hold_gold_in = [&](const ml::ContextVec& cur_ctx) {
        ml::EconVec econ_vec{};
        econ_vec[0] = 1.0F;
        econ_vec[10] = std::tanh(
                static_cast<float>(game::Upkeep(game, side) - game::Income(game, side)) / 40.0F);
        return ml::PackEconInput(cur_ctx, econ_vec);
    };

    // Army retention vs disbanding: only offered when upkeep exceeds income, the
    // one situation where the rules make disbanding worth anything. Keeping an
    // irreversible action off the table otherwise stops an untrained policy from
    // dissolving its own army during early training.
    std::vector<Army*> my_armies = game::SideArmies(game, side);
    bool over_budget = game::Upkeep(game, side) > game::Income(game, side);
    if (over_budget && my_armies.size() > 3) {
        for (Army* army : my_armies) {
            if (!game.Alive(army) || army->Hero() || army->InTransit() || army->X() == kNone)
                continue;
            City* city = game::CityAt(game, army->X(), army->Y());
            if (!city || city->OwnerIndex() != our_side) continue;
            int city_armies = 0;
            for (int dy = 0; dy <= 1; dy++) {
                for (int dx = 0; dx <= 1; dx++) {
                    city_armies += static_cast<int>(
                            game::ArmiesOnTileSpan(game, city->X() + dx, city->Y() + dy).size());
                }
            }
            if (city_armies <= 2) continue;

            const ArmyType* army_type = game.ArmyTypes().ById(army->Type());
            float threat = (city->Index() < static_cast<int>(belief.city_threat2_turn.size()))
                                   ? belief.city_threat2_turn[city->Index()]
                                   : 0.0F;
            float dist_e = (city->Index() < static_cast<int>(belief.city_nearest_enemy_dist.size()))
                                   ? belief.city_nearest_enemy_dist[city->Index()]
                                   : 100.0F;

            ml::ContextVec cur_ctx = ml::BuildContext(game, side, &policy);

            ml::EconVec econ_vec{};
            econ_vec[5] = 1.0F;
            econ_vec[7] = static_cast<float>(army->Strength()) / 9.0F;
            econ_vec[8] = static_cast<float>(army->MaxMoves()) / 24.0F;
            econ_vec[10] = static_cast<float>(army->Upkeep()) / 15.0F;
            econ_vec[11] = (army_type && army_type->flies) ? 1.0F : 0.0F;
            econ_vec[12] =
                    (army_type && (army_type->siege || army_type->IsMagical())) ? 1.0F : 0.0F;
            econ_vec[13] =
                    (army->MaxMoves() >= 14 && (!army_type || !army_type->flies)) ? 1.0F : 0.0F;
            econ_vec[14] = std::tanh(threat / 25.0F);
            econ_vec[15] = std::tanh(dist_e / 60.0F);
            econ_vec[ml::kEconCurrent] = 1.0F;  // keeping the army is the status quo
            ml::EconInVec keep_in = ml::PackEconInput(cur_ctx, econ_vec);

            ml::EconVec disband_vec = econ_vec;
            disband_vec[10] = -econ_vec[10];
            disband_vec[ml::kEconCurrent] = 0.0F;
            ml::EconInVec disband_in = ml::PackEconInput(cur_ctx, disband_vec);

            std::vector<ml::EconInVec> cands = {keep_in, disband_in};
            std::vector<float> logits = {policy.ForwardEcon(keep_in)[0],
                                         policy.ForwardEcon(disband_in)[0]};
            float log_prob = 0.0F;
            int pick = ml::SelectFromLogits(logits, sampler, &log_prob);
            if (sampler && sampler->current_turn) {
                sampler->current_turn->econ_steps.push_back(
                        ml::EconStepRecord{std::move(cands), {}, pick, log_prob});
            }
            if (pick == 1) {
                game::Disband(game, side, {army});
            }
        }
    }

    Map& map = game.GetMap();
    std::vector<City*> own_cities = game::SideCities(game, side);
    auto is_muster_city = [&](int city_idx) {
        return policy.Version() >= 8 && policy.UseMuster() && city_idx != kNone &&
               (city_idx == belief.muster_cities[0] || city_idx == belief.muster_cities[1]);
    };
    // Precompute land reachability from each active Muster Hub so non-flying land
    // production is only vectored into a Muster Hub on the SAME landmass.
    std::array<core::Flood, 2> hub_land_flood{};
    std::array<bool, 2> hub_has_land_target{false, false};
    if (policy.Version() >= 8 && policy.UseMuster()) {
        for (int hub_idx = 0; hub_idx < 2; hub_idx++) {
            int city_idx = belief.muster_cities[hub_idx];
            if (city_idx < 0 || city_idx >= map.CityCount()) continue;
            const City& hub_city = map.Cities()[city_idx];
            hub_land_flood[hub_idx] = core::FloodFrom(game, our_side, hub_city.X(), hub_city.Y(),
                                                      72, move::kLand, our_side);
            for (City* vis_city : belief.visible_cities) {
                if (vis_city->IsRazed() || vis_city->OwnerIndex() == our_side) continue;
                if (!diplomacy::IsAllied(game, our_side, vis_city->OwnerIndex()) &&
                    core::FloodAt(hub_land_flood[hub_idx], vis_city->X(), vis_city->Y()) <
                            core::kUnreached) {
                    hub_has_land_target[hub_idx] = true;
                    break;
                }
            }
        }
    }
    auto hub_usable_for_unit = [&](const City& from_city, int hub_city_idx, bool unit_flies) {
        if (!is_muster_city(hub_city_idx)) return false;
        if (unit_flies) return true;
        for (int hub_idx = 0; hub_idx < 2; hub_idx++) {
            if (hub_city_idx == belief.muster_cities[hub_idx] && hub_has_land_target[hub_idx]) {
                if (from_city.Index() == hub_city_idx) return true;
                for (int dy = -1; dy <= 2; dy++) {
                    for (int dx = -1; dx <= 2; dx++) {
                        int tile_x = from_city.X() + dx;
                        int tile_y = from_city.Y() + dy;
                        if (tile_x >= 0 && tile_y >= 0 && tile_x < map.Width() &&
                            tile_y < map.Height() &&
                            core::FloodAt(hub_land_flood[hub_idx], tile_x, tile_y) <
                                    core::kUnreached) {
                            return true;
                        }
                    }
                }
            }
        }
        return false;
    };
    auto city_army_stats = [&](const City& city, bool& has_hero_out) {
        int army_cnt = 0;
        has_hero_out = false;
        for (int dy = 0; dy <= 1; dy++) {
            for (int dx = 0; dx <= 1; dx++) {
                for (Army* army : game::ArmiesOnTileSpan(game, city.X() + dx, city.Y() + dy)) {
                    if (army->OwnerIndex() == our_side) {
                        army_cnt++;
                        if (army->Hero()) has_hero_out = true;
                    }
                }
            }
        }
        return army_cnt;
    };

    // Empire-wide slot upgrade rounds (up to 3 purchases/turn): each round asks EconNet
    // to compare HoldGold against every legal (city, unit) upgrade, stopping immediately
    // as soon as EconNet chooses HoldGold so multi-city empires neither hoard 10k gold
    // nor suffer 1 - p^N treasury-draining coin-flip cascades across N cities.
    auto menu = game::BuyableTypes(game);
    for (int buy_round = 0; buy_round < 3 && !own_cities.empty() && side.Gold() >= 250;
         buy_round++) {
        /** @brief A candidate production-slot upgrade in one city, or HoldGold when city is null.
         */
        struct BuyOption {
            City* city = nullptr;    ///< City to buy the slot in, or nullptr for HoldGold.
            int type_id = kNone;     ///< ArmyType ID to purchase, or kNone.
            ml::EconInVec in_vec{};  ///< Packed EconNet input.
        };
        ml::ContextVec buy_ctx = ml::BuildContext(game, side, &policy);
        std::vector<BuyOption> buy_opts;
        buy_opts.push_back(BuyOption{nullptr, kNone, make_hold_gold_in(buy_ctx)});

        for (City* city : own_cities) {
            bool full_slots = city->SlotCount() >= rules::kProductionSlots;
            int replace_price = (!city->Slots().empty()) ? city->SlotAt(0).price : 0;
            int best_existing_str = 0;
            for (const auto& slot : city->Slots())
                best_existing_str = std::max(best_existing_str, slot.strength);
            float threat = city->Index() < static_cast<int>(belief.city_threat2_turn.size())
                                   ? belief.city_threat2_turn[city->Index()]
                                   : 0.0F;
            float dist_e = city->Index() < static_cast<int>(belief.city_nearest_enemy_dist.size())
                                   ? belief.city_nearest_enemy_dist[city->Index()]
                                   : 100.0F;
            for (const ArmyType* army_type : menu) {
                int price = std::abs(army_type->price);
                if (price <= 0 || price + 100 > side.Gold()) continue;
                if (full_slots && price <= replace_price) continue;
                if (!game::CannotBuy(game, side, *city, *army_type).empty()) continue;

                ml::EconVec econ_vec{};
                econ_vec[3] = 1.0F;
                econ_vec[7] = static_cast<float>(army_type->strength) / 9.0F;
                econ_vec[8] = static_cast<float>(army_type->move) / 24.0F;
                econ_vec[9] = static_cast<float>(army_type->time) / 6.0F;
                econ_vec[10] = static_cast<float>(price) /
                               std::max(100.0F, static_cast<float>(side.Gold() - 80));
                econ_vec[11] = army_type->flies ? 1.0F : 0.0F;
                econ_vec[12] = (army_type->siege || army_type->IsMagical()) ? 1.0F : 0.0F;
                econ_vec[13] = (army_type->move >= 14 && !army_type->flies) ? 1.0F : 0.0F;
                econ_vec[14] = std::tanh(threat / 25.0F);
                econ_vec[15] = std::tanh(dist_e / 60.0F);
                econ_vec[16] = static_cast<float>(city->SlotCount()) /
                               static_cast<float>(rules::kProductionSlots);
                econ_vec[17] = static_cast<float>(army_type->strength - best_existing_str) / 9.0F;
                econ_vec[18] = static_cast<float>(army_type->strength * army_type->strength) /
                               static_cast<float>(std::max(1, army_type->time * 16));
                if (policy.Version() >= 8 && policy.UseMuster()) {
                    bool feeds_hub = is_muster_city(city->Index()) ||
                                     (city->VectoredCityId().IsValid() &&
                                      is_muster_city(city->VectoredCityId().Value()));
                    bool city_hero = false;
                    int city_armies = city_army_stats(*city, city_hero);
                    econ_vec[ml::kEconMusterHub] = feeds_hub ? 1.0F : 0.0F;
                    econ_vec[ml::kEconMusterNeed] = std::clamp(
                            (8.0F - static_cast<float>(city_armies)) / 8.0F, -0.5F, 1.0F);
                    econ_vec[ml::kEconMusterFlow] = city_hero ? 1.0F : 0.0F;
                }
                buy_opts.push_back(
                        BuyOption{city, army_type->id, ml::PackEconInput(buy_ctx, econ_vec)});
            }
        }

        if (buy_opts.size() <= 1) break;
        std::vector<float> logits(buy_opts.size());
        std::vector<float> log_mult(buy_opts.size(), 0.0F);
        for (size_t i = 0; i < buy_opts.size(); i++) {
            logits[i] = policy.ForwardEcon(buy_opts[i].in_vec)[0];
            if (i > 0 && buy_opts.size() > 2)
                log_mult[i] = std::log(static_cast<float>(buy_opts.size() - 1));
        }
        float log_prob = 0.0F;
        int pick = ml::SelectFromLogits(logits, sampler, &log_prob, &log_mult);
        if (sampler && sampler->current_turn) {
            std::vector<ml::EconInVec> cands;
            cands.reserve(buy_opts.size());
            for (const auto& buy_opt : buy_opts) cands.push_back(buy_opt.in_vec);
            sampler->current_turn->econ_steps.push_back(
                    ml::EconStepRecord{std::move(cands), std::move(log_mult), pick, log_prob});
        }
        if (pick == 0 || !buy_opts[pick].city) break;

        City* best_buy_city = buy_opts[pick].city;
        int bought_type = buy_opts[pick].type_id;
        int slot_idx = game::BuySlot(*best_buy_city);
        game::BuyProduction(game, side, *best_buy_city, slot_idx, bought_type);
        game::SortProduction(*best_buy_city);
        for (int si = 0; si < best_buy_city->SlotCount(); si++) {
            if (best_buy_city->SlotAt(si).type == bought_type) {
                game::SetProduction(game, *best_buy_city, si);
                break;
            }
        }
    }

    // City production macro-action: queried whenever a city has no active build
    // or has just completed a build (`city->Countdown() == slot.time`, matching
    // game.cpp:1331), so multi-turn unit builds complete without mid-countdown
    // cancellation while cities remain free to switch units after each build.
    ml::ContextVec prod_ctx = ml::BuildContext(game, side, &policy);
    for (City* city : own_cities) {
        int prod_slot = city->ProducingSlot();
        bool mid_countdown =
                (prod_slot >= 0 && prod_slot < city->SlotCount() && city->Countdown() > 0 &&
                 city->Countdown() < city->SlotAt(prod_slot).time);
        if (city->Slots().empty() || mid_countdown) continue;
        float threat = city->Index() < static_cast<int>(belief.city_threat2_turn.size())
                               ? belief.city_threat2_turn[city->Index()]
                               : 0.0F;
        float dist_e = city->Index() < static_cast<int>(belief.city_nearest_enemy_dist.size())
                               ? belief.city_nearest_enemy_dist[city->Index()]
                               : 100.0F;
        int gar_count = 0;
        for (int dy = 0; dy <= 1; dy++) {
            for (int dx = 0; dx <= 1; dx++) {
                gar_count += static_cast<int>(
                        game::ArmiesOnTileSpan(game, city->X() + dx, city->Y() + dy).size());
            }
        }

        ml::EconVec stop_vec{};
        stop_vec[2] = 1.0F;
        stop_vec[10] = std::tanh(
                static_cast<float>(game::Upkeep(game, side) - game::Income(game, side)) / 30.0F);
        stop_vec[14] = std::tanh(threat / 25.0F);
        stop_vec[15] = std::tanh(dist_e / 60.0F);
        stop_vec[16] = std::tanh(static_cast<float>(gar_count) / 6.0F);
        stop_vec[ml::kEconCurrent] = (prod_slot == kNone) ? 1.0F : 0.0F;

        std::vector<int> slot_indices = {kNone};
        std::vector<ml::EconInVec> cands = {ml::PackEconInput(prod_ctx, stop_vec)};
        for (size_t si = 0; si < city->Slots().size(); si++) {
            const Slot& slot = city->SlotAt(si);
            const ArmyType* army_type = game.ArmyTypes().ById(slot.type);
            bool is_flier = army_type && army_type->flies;
            bool is_siege_or_spec = army_type && (army_type->siege || army_type->IsMagical() ||
                                                  army_type->Ability() != 0);
            bool is_cav = slot.move >= 14 && !is_flier;

            ml::EconVec econ_vec{};
            econ_vec[1] = 1.0F;
            econ_vec[7] = static_cast<float>(slot.strength) / 9.0F;
            econ_vec[8] = static_cast<float>(slot.move) / 24.0F;
            econ_vec[9] = static_cast<float>(slot.time) / 6.0F;
            econ_vec[10] = static_cast<float>(slot.cost) / 20.0F;
            econ_vec[11] = is_flier ? 1.0F : 0.0F;
            econ_vec[12] = is_siege_or_spec ? 1.0F : 0.0F;
            econ_vec[13] = is_cav ? 1.0F : 0.0F;
            econ_vec[14] = std::tanh(threat / 25.0F);
            econ_vec[15] = std::tanh(dist_e / 60.0F);
            econ_vec[16] = std::tanh(static_cast<float>(gar_count) / 6.0F);
            econ_vec[18] = static_cast<float>(slot.strength * slot.strength) /
                           static_cast<float>(std::max(1, slot.time * 16));
            econ_vec[ml::kEconCurrent] = (prod_slot == static_cast<int>(si)) ? 1.0F : 0.0F;
            slot_indices.push_back(static_cast<int>(si));
            cands.push_back(ml::PackEconInput(prod_ctx, econ_vec));
        }

        if (cands.size() == 1) {
            game::SetProduction(game, *city, kNone);
            continue;
        }

        std::vector<float> logits(cands.size());
        std::vector<float> log_mult(cands.size(), 0.0F);
        for (size_t i = 0; i < cands.size(); i++) {
            logits[i] = policy.ForwardEcon(cands[i])[0];
            if (i > 0 && cands.size() > 2)
                log_mult[i] = std::log(static_cast<float>(cands.size() - 1));
        }
        float log_prob = 0.0F;
        int pick = ml::SelectFromLogits(logits, sampler, &log_prob, &log_mult);
        if (sampler && sampler->current_turn) {
            sampler->current_turn->econ_steps.push_back(
                    ml::EconStepRecord{std::move(cands), std::move(log_mult), pick, log_prob});
        }

        if (pick == 0) {
            game::SetProduction(game, *city, kNone);
        } else if (city->ProducingSlot() != slot_indices[pick] || !game::Producing(*city)) {
            game::SetProduction(game, *city, slot_indices[pick]);
        }
    }

    // Precompute planted Standard state so EconNet can choose Standard (-2)
    // alongside static owned castles whenever the side's Standard is planted.
    constexpr int kStandardTarget = -2;
    auto [std_x, std_y] = game::StandardAt(game, side);
    bool std_valid = false;
    int std_armies = 0;
    int std_vectored_count = 0;
    bool std_hero = false;
    float std_threat = 0.0F;
    float std_dist_e = 100.0F;
    core::Flood std_land_flood{};
    if (std_x != kNone && std_y != kNone) {
        absl::Span<Army* const> std_here = game::ArmiesOnTileSpan(game, std_x, std_y);
        if ((std_here.empty() || std_here[0]->OwnerIndex() == our_side) &&
            static_cast<int>(std_here.size()) < rules::kMaxStack) {
            std_valid = true;
            std_armies = static_cast<int>(std_here.size());
            for (Army* army : std_here) {
                if (army->Hero()) std_hero = true;
            }
            std_vectored_count = static_cast<int>(game::VectoredTo(game, nullptr, &side).size());
            std_land_flood =
                    core::FloodFrom(game, our_side, std_x, std_y, 72, move::kLand, our_side);
            for (City* vis_city : belief.visible_cities) {
                if (!vis_city->IsRazed() && vis_city->OwnerIndex() >= 0 &&
                    vis_city->OwnerIndex() != our_side &&
                    !diplomacy::IsAllied(game, our_side, vis_city->OwnerIndex())) {
                    std_dist_e = std::min(std_dist_e,
                                          static_cast<float>(move::Distance(
                                                  std_x, std_y, vis_city->X(), vis_city->Y())));
                }
            }
            for (const auto& [tile_key, obs] : belief.stacks_by_tile) {
                (void)tile_key;
                if (obs.owner == our_side || obs.owner == kNone ||
                    diplomacy::IsAllied(game, our_side, obs.owner))
                    continue;
                int dist_tile = move::Distance(std_x, std_y, obs.x, obs.y);
                std_dist_e = std::min(std_dist_e, static_cast<float>(dist_tile));
                const ArmyType* top_at = game.ArmyTypes().ById(obs.top_type);
                int reach2 = std::max(8, ((top_at ? std::max(10, top_at->move) : 12) * 3) / 4) * 2;
                if (dist_tile <= reach2) {
                    float top_str = top_at ? static_cast<float>(top_at->strength) : 4.0F;
                    float rest_str = (obs.owner >= 0 && obs.owner < 8)
                                             ? belief.seen_enemy_avg_strength[obs.owner]
                                             : 4.0F;
                    std_threat += top_str +
                                  static_cast<float>(std::max(0, obs.visible_count - 1)) * rest_str;
                }
            }
        }
    }
    auto std_usable_for_unit = [&](const City& from_city, bool unit_flies) {
        if (!std_valid) return false;
        if (unit_flies) return true;
        for (int dy = -1; dy <= 2; dy++) {
            for (int dx = -1; dx <= 2; dx++) {
                int tile_x = from_city.X() + dx;
                int tile_y = from_city.Y() + dy;
                if (tile_x >= 0 && tile_y >= 0 && tile_x < map.Width() && tile_y < map.Height() &&
                    core::FloodAt(std_land_flood, tile_x, tile_y) < core::kUnreached) {
                    return true;
                }
            }
        }
        return false;
    };

    // City vectoring: re-evaluated each turn, but the vector already set is a
    // flagged candidate (ECON_CURRENT) rather than wiped, so keeping it is a choice
    const bool want_thoughts = ml::RecordThoughts();
    std::vector<int> vec_to_city_count(static_cast<size_t>(map.CityCount()), 0);
    std::vector<int> own_city_armies(static_cast<size_t>(map.CityCount()), 0);
    std::vector<uint8_t> own_city_hero(static_cast<size_t>(map.CityCount()), uint8_t{0});
    if (policy.Version() >= 8 && policy.UseMuster()) {
        for (City* city : own_cities) {
            bool has_hero = false;
            int cnt = city_army_stats(*city, has_hero);
            if (city->Index() >= 0 && city->Index() < map.CityCount()) {
                own_city_armies[static_cast<size_t>(city->Index())] = cnt;
                own_city_hero[static_cast<size_t>(city->Index())] =
                        has_hero ? uint8_t{1} : uint8_t{0};
            }
        }
    }
    for (City* src : own_cities) {
        if (!game::Producing(*src) || src->ProducingSlot() < 0 ||
            src->ProducingSlot() >= src->SlotCount()) {
            src->ClearVector();
            continue;
        }
        if (!std_valid && src->IsVectoredToStandard()) {
            src->ClearVector();
        }
        std::fill(vec_to_city_count.begin(), vec_to_city_count.end(), 0);
        for (const auto& city : map.Cities()) {
            if (city.VectoredCityId().IsValid() && city.VectoredCityId() != city.Id()) {
                int target_idx = city.VectoredCityId().Value();
                if (target_idx >= 0 && target_idx < map.CityCount()) {
                    vec_to_city_count[static_cast<size_t>(target_idx)]++;
                }
            }
        }

        const Slot& slot = src->SlotAt(src->ProducingSlot());
        const ArmyType* slot_at = game.ArmyTypes().ById(slot.type);
        const bool slot_flies = slot_at && slot_at->flies;
        float src_threat = src->Index() < static_cast<int>(belief.city_threat2_turn.size())
                                   ? belief.city_threat2_turn[src->Index()]
                                   : 0.0F;
        float src_dist_e = src->Index() < static_cast<int>(belief.city_nearest_enemy_dist.size())
                                   ? belief.city_nearest_enemy_dist[src->Index()]
                                   : 100.0F;

        ml::EconVec keep_here{};
        keep_here[4] = 1.0F;
        keep_here[14] = std::tanh(src_threat / 25.0F);
        keep_here[15] = std::tanh(src_dist_e / 60.0F);
        keep_here[17] = 0.0F;
        keep_here[ml::kEconCurrent] = !src->IsVectored() ? 1.0F : 0.0F;
        if (policy.Version() >= 8 && policy.UseMuster() &&
            hub_usable_for_unit(*src, src->Index(), slot_flies)) {
            int src_armies = own_city_armies[static_cast<size_t>(src->Index())];
            bool src_hero = own_city_hero[static_cast<size_t>(src->Index())] != 0;
            keep_here[ml::kEconMusterHub] = 1.0F;
            keep_here[ml::kEconMusterNeed] =
                    std::clamp((8.0F - static_cast<float>(src_armies)) / 8.0F, -0.5F, 1.0F);
            keep_here[ml::kEconMusterFlow] =
                    0.5F * (src_hero ? 1.0F : 0.0F) +
                    0.5F * std::min(1.0F, static_cast<float>(vec_to_city_count[static_cast<size_t>(
                                                  src->Index())]) /
                                                  4.0F);
        }

        std::vector<int> dest_targets = {kNone};
        std::vector<ml::EconInVec> cands = {ml::PackEconInput(prod_ctx, keep_here)};

        if (std_usable_for_unit(*src, slot_flies)) {
            bool is_current_std = src->IsVectoredToStandard();
            if (is_current_std || std_vectored_count < game::kMaxVectoredTo) {
                float threat_gain =
                        (std_threat - src_threat) / 25.0F + (src_dist_e - std_dist_e) / 45.0F;
                ml::EconVec std_vec{};
                std_vec[4] = 1.0F;
                std_vec[7] = static_cast<float>(slot.strength) / 9.0F;
                std_vec[8] = static_cast<float>(slot.move) / 24.0F;
                std_vec[9] = 0.5F;  // TRANSIT_TURNS = 2 turns (normalized / 4.0F)
                std_vec[14] = std::tanh(std_threat / 25.0F);
                std_vec[15] = std::tanh(std_dist_e / 60.0F);
                std_vec[17] = std::tanh(threat_gain);
                std_vec[ml::kEconCurrent] = is_current_std ? 1.0F : 0.0F;
                if (policy.Version() >= 8 && (policy.UseMuster() || policy.UseStandard())) {
                    std_vec[ml::kEconMusterHub] = 1.0F;
                    std_vec[ml::kEconMusterNeed] =
                            std::clamp((8.0F - static_cast<float>(std_armies)) / 8.0F, -0.5F, 1.0F);
                    std_vec[ml::kEconMusterFlow] =
                            0.5F * (std_hero ? 1.0F : 0.0F) +
                            0.5F * std::min(1.0F, static_cast<float>(std_vectored_count) / 4.0F);
                }
                dest_targets.push_back(kStandardTarget);
                cands.push_back(ml::PackEconInput(prod_ctx, std_vec));
            }
        }

        for (City* dst : own_cities) {
            if (dst == src) continue;
            bool is_current = (src->VectoredCityId() == dst->Id());
            int dst_vec_count = vec_to_city_count[static_cast<size_t>(dst->Index())];
            if (!is_current && dst_vec_count >= game::kMaxVectoredTo) continue;
            int v_turns =
                    std::max(1, move::Distance(src->X(), src->Y(), dst->X(), dst->Y()) / 14 + 1);
            if (v_turns > 4) continue;
            float dst_threat = dst->Index() < static_cast<int>(belief.city_threat2_turn.size())
                                       ? belief.city_threat2_turn[dst->Index()]
                                       : 0.0F;
            float dst_dist_e =
                    dst->Index() < static_cast<int>(belief.city_nearest_enemy_dist.size())
                            ? belief.city_nearest_enemy_dist[dst->Index()]
                            : 100.0F;
            float threat_gain =
                    (dst_threat - src_threat) / 25.0F + (src_dist_e - dst_dist_e) / 45.0F;

            ml::EconVec vec_econ{};
            vec_econ[4] = 1.0F;
            vec_econ[7] = static_cast<float>(slot.strength) / 9.0F;
            vec_econ[8] = static_cast<float>(slot.move) / 24.0F;
            vec_econ[9] = static_cast<float>(v_turns) / 4.0F;
            vec_econ[14] = std::tanh(dst_threat / 25.0F);
            vec_econ[15] = std::tanh(dst_dist_e / 60.0F);
            vec_econ[17] = std::tanh(threat_gain);
            vec_econ[ml::kEconCurrent] = is_current ? 1.0F : 0.0F;
            if (policy.Version() >= 8 && policy.UseMuster()) {
                int dst_armies = own_city_armies[static_cast<size_t>(dst->Index())];
                bool dst_hero = own_city_hero[static_cast<size_t>(dst->Index())] != 0;
                vec_econ[ml::kEconMusterHub] =
                        hub_usable_for_unit(*src, dst->Index(), slot_flies) ? 1.0F : 0.0F;
                vec_econ[ml::kEconMusterNeed] =
                        std::clamp((8.0F - static_cast<float>(dst_armies)) / 8.0F, -0.5F, 1.0F);
                vec_econ[ml::kEconMusterFlow] =
                        0.5F * (dst_hero ? 1.0F : 0.0F) +
                        0.5F * std::min(1.0F, static_cast<float>(dst_vec_count) / 4.0F);
            }
            dest_targets.push_back(dst->Index());
            cands.push_back(ml::PackEconInput(prod_ctx, vec_econ));
        }

        if (cands.size() > 1) {
            std::vector<float> logits(cands.size());
            std::vector<float> log_mult(cands.size(), 0.0F);
            for (size_t i = 0; i < cands.size(); i++) {
                logits[i] = policy.ForwardEcon(cands[i])[0];
                if (i > 0 && cands.size() > 2)
                    log_mult[i] = std::log(static_cast<float>(cands.size() - 1));
            }
            float log_prob = 0.0F;
            int pick = ml::SelectFromLogits(logits, sampler, &log_prob, &log_mult);
            if (sampler && sampler->current_turn) {
                sampler->current_turn->econ_steps.push_back(
                        ml::EconStepRecord{std::move(cands), std::move(log_mult), pick, log_prob});
            }
            int chosen_target = dest_targets[pick];
            if (chosen_target == kStandardTarget) {
                bool was_std = src->IsVectoredToStandard();
                if (game::VectorToStandard(game, *src, side)) {
                    if (!was_std) std_vectored_count++;
                    if (want_thoughts) {
                        if (ModernMlPlayer* modern_player = ModernMlPlayer::FindMutableOn(side)) {
                            modern_player->PushThought(AIThought{
                                "Economy", src->Name(), "Standard", src->X(), src->Y(), std_x,
                                std_y, logits[pick], 1.0F, false,
                                absl::StrFormat("Vectoring %s from %s -> Standard (%d,%d)",
                                                slot.name, src->Name(), std_x, std_y)});
                        }
                    }
                }
            } else {
                if (src->IsVectoredToStandard())
                    std_vectored_count = std::max(0, std_vectored_count - 1);
                City* best_dest = (chosen_target >= 0 && chosen_target < map.CityCount())
                                          ? &map.Cities()[chosen_target]
                                          : nullptr;
                game::Vector(game, *src, best_dest);
                if (best_dest && want_thoughts) {
                    if (ModernMlPlayer* modern_player = ModernMlPlayer::FindMutableOn(side)) {
                        modern_player->PushThought(AIThought{
                            "Economy", src->Name(), best_dest->Name(), src->X(), src->Y(),
                            best_dest->X(), best_dest->Y(), logits[pick], 1.0F, false,
                            absl::StrFormat("Vectoring %s from %s -> %s", slot.name, src->Name(),
                                            best_dest->Name())});
                    }
                }
            }
        }
    }
}

void RunCityGuardPhase(Game& game, Side& side, const ml::PolicyNetwork& policy,
                       const ml::ContextVec& ctx, ml::RolloutSampler* sampler,
                       MovedArmySet& moved_armies) {
    if (policy.Version() < 7 || !policy.EnableCityGuard()) return;
    int our_side = side.Index();
    const SideBelief& belief = belief::Ensure(side);
    for (City* city : game::SideCities(game, side)) {
        int immobile_guards = 0;
        std::vector<Army*> mobile;
        for (int dy = 0; dy <= 1; dy++) {
            for (int dx = 0; dx <= 1; dx++) {
                for (Army* army : game::ArmiesOnTileSpan(game, city->X() + dx, city->Y() + dy)) {
                    if (army->OwnerIndex() != our_side || !game.Alive(army) || army->InTransit())
                        continue;
                    if (army->Moves() <= 0 || moved_armies.contains(army->Id())) {
                        immobile_guards++;
                    } else if (!army->Hero()) {
                        mobile.push_back(army);
                    }
                }
            }
        }
        if (mobile.empty() || immobile_guards >= 2) continue;

        // Slowest/cheapest non-hero defenders guard first when EconNet chooses k > 0.
        std::sort(mobile.begin(), mobile.end(), [&](Army* lhs, Army* rhs) {
            bool lhs_flies = core::Flies(game, lhs);
            bool rhs_flies = core::Flies(game, rhs);
            if (lhs_flies != rhs_flies) return lhs_flies < rhs_flies;
            if (lhs->MaxMoves() != rhs->MaxMoves()) return lhs->MaxMoves() < rhs->MaxMoves();
            return lhs->Strength() < rhs->Strength();
        });

        int max_keep = std::min(2 - immobile_guards, static_cast<int>(mobile.size()));
        if (max_keep <= 0) continue;

        float threat_1 = city->Index() < static_cast<int>(belief.city_threat1_turn.size())
                                 ? belief.city_threat1_turn[city->Index()]
                                 : 0.0F;
        float threat_2 = city->Index() < static_cast<int>(belief.city_threat2_turn.size())
                                 ? belief.city_threat2_turn[city->Index()]
                                 : 0.0F;
        float dist_e = city->Index() < static_cast<int>(belief.city_nearest_enemy_dist.size())
                               ? belief.city_nearest_enemy_dist[city->Index()]
                               : 100.0F;
        float city_val =
                (static_cast<float>(city->Income()) + static_cast<float>(city->Defence()) * 3.0F) /
                35.0F;

        std::vector<ml::EconInVec> cands;
        std::vector<float> logits;
        cands.reserve(static_cast<size_t>(max_keep) + 1);
        logits.reserve(static_cast<size_t>(max_keep) + 1);
        for (int keep_cnt = 0; keep_cnt <= max_keep; keep_cnt++) {
            ml::EconVec econ_vec{};
            econ_vec[14] = std::tanh(threat_1 / 20.0F);
            econ_vec[15] = std::tanh(dist_e / 60.0F);
            econ_vec[16] = std::tanh(threat_2 / 25.0F);
            econ_vec[17] = std::tanh(city_val);
            econ_vec[18] = std::tanh(static_cast<float>(mobile.size()) / 6.0F);
            econ_vec[ml::kEconGuard1] = (keep_cnt >= 1) ? 1.0F : 0.0F;
            econ_vec[ml::kEconGuard2] = (keep_cnt >= 2) ? 1.0F : 0.0F;
            ml::EconInVec in_vec = ml::PackEconInput(ctx, econ_vec);
            logits.push_back(policy.ForwardEcon(in_vec)[0]);
            cands.push_back(in_vec);
        }

        float log_prob = 0.0F;
        int pick = ml::SelectFromLogits(logits, sampler, &log_prob);
        if (sampler && sampler->current_turn) {
            ml::EconStepRecord rec;
            rec.candidates = std::move(cands);
            rec.chosen_idx = pick;
            rec.old_log_prob = log_prob;
            rec.guard_city = city->Index();
            sampler->current_turn->econ_steps.push_back(std::move(rec));
        }
        for (int i = 0; i < pick && i < static_cast<int>(mobile.size()); i++) {
            moved_armies.insert(mobile[i]->Id());
            mobile[i]->SetAiOrder(100);
            mobile[i]->SetAiDest(city->Index());
            mobile[i]->SetAiGroup(1);
        }
    }
}

void RunMusterSelectionPhase(Game& game, Side& side, const ml::PolicyNetwork& policy,
                             const ml::ContextVec& ctx, ml::RolloutSampler* sampler) {
    SideBelief& belief = belief::Ensure(side);
    std::array<int, 2> prev_hubs = belief.muster_cities;
    belief.muster_cities = {kNone, kNone};
    if (policy.Version() < 8 || !policy.UseMuster()) return;

    int our_side = side.Index();
    std::vector<City*> own_cities = game::SideCities(game, side);
    if (own_cities.size() < 2) return;

    // Dedicated deterministic sampler so Muster Hub selection never shifts the
    // turn's main deploySampler PRNG stream when NoHub is chosen.
    ml::RolloutSampler local_muster_sampler;
    ml::RolloutSampler* muster_sampler = sampler;
    if (!sampler || !sampler->current_turn) {
        local_muster_sampler.temperature = sampler ? sampler->temperature : ml::DeployTemperature();
        local_muster_sampler.rng_state = (game.GetRng().State() * 2246822519U) ^
                                         static_cast<uint32_t>(game.Turn() * 65521U) ^
                                         static_cast<uint32_t>((side.Index() + 1) * 0x517cc1b7U);
        muster_sampler = &local_muster_sampler;
    }

    const Map& map = game.GetMap();
    const bool want_thoughts = ml::RecordThoughts();
    struct EligibleMusterCity {
        City* city = nullptr;
        ml::MusterVec base_vec{};
    };
    std::vector<EligibleMusterCity> eligible_cities;
    eligible_cities.reserve(own_cities.size());

    std::vector<int> vec_to_city_count(static_cast<size_t>(map.CityCount()), 0);
    for (const auto& city_entry : map.Cities()) {
        if (city_entry.VectoredCityId().IsValid() &&
            city_entry.VectoredCityId() != city_entry.Id()) {
            int target_idx = city_entry.VectoredCityId().Value();
            if (target_idx >= 0 && target_idx < map.CityCount()) {
                vec_to_city_count[static_cast<size_t>(target_idx)]++;
            }
        }
    }

    for (City* city : own_cities) {
        // Measure true land-path distance to visible enemy & neutral cities so
        // water-locked island cities are not mistaken for frontline land hubs.
        core::Flood city_land =
                core::FloodFrom(game, our_side, city->X(), city->Y(), 72, move::kLand, our_side);
        float dist_e = 100.0F;
        float dist_n = 100.0F;
        int land_obj_count = 0;
        for (City* vis_city : belief.visible_cities) {
            if (vis_city->IsRazed() || vis_city->OwnerIndex() == our_side) continue;
            int best_step = core::kUnreached;
            for (int dy = -1; dy <= 2; dy++) {
                for (int dx = -1; dx <= 2; dx++) {
                    int tile_x = vis_city->X() + dx;
                    int tile_y = vis_city->Y() + dy;
                    if (tile_x >= 0 && tile_y >= 0 && tile_x < map.Width() &&
                        tile_y < map.Height()) {
                        best_step = std::min(best_step, core::FloodAt(city_land, tile_x, tile_y));
                    }
                }
            }
            if (best_step >= core::kUnreached) continue;
            float land_tiles = 0.5F * static_cast<float>(best_step);
            if (vis_city->OwnerIndex() == kNone) {
                dist_n = std::min(dist_n, land_tiles);
                land_obj_count++;
            } else if (!diplomacy::IsAllied(game, our_side, vis_city->OwnerIndex())) {
                dist_e = std::min(dist_e, land_tiles);
                land_obj_count++;
            }
        }
        if (land_obj_count < 2) continue;  // tiny water-locked islet (<2 land targets)
        float threat_2 = city->Index() < static_cast<int>(belief.city_threat2_turn.size())
                                 ? belief.city_threat2_turn[city->Index()]
                                 : 0.0F;

        int city_armies = 0;
        int city_str = 0;
        bool near_hero = false;
        float rear_str = 0.0F;
        int in_transit = 0;
        for (Army* army : game.Armies()) {
            if (army->OwnerIndex() != our_side || army->IsDisbanded()) continue;
            if (army->InTransit()) {
                if (std::holds_alternative<CityId>(army->TransitState()->dest) &&
                    std::get<CityId>(army->TransitState()->dest) == city->Id()) {
                    in_transit++;
                }
                continue;
            }
            if (army->X() == kNone) continue;
            int dist_tile = move::Distance(army->X(), army->Y(), city->X(), city->Y());
            if (game::CityAt(game, army->X(), army->Y()) == city) {
                city_armies++;
                city_str += army->Strength();
                if (army->Hero()) near_hero = true;
            } else {
                if (army->Hero() && dist_tile <= 6) near_hero = true;
                if (army->Moves() > 0 && dist_tile <= 22)
                    rear_str += static_cast<float>(army->Strength());
            }
        }

        int best_slot_str = 0;
        bool has_fly_or_spec = false;
        for (const auto& slot : city->Slots()) {
            best_slot_str = std::max(best_slot_str, slot.strength);
            const ArmyType* army_type = game.ArmyTypes().ById(slot.type);
            if (army_type && (army_type->flies || army_type->siege || army_type->IsMagical() ||
                              army_type->Ability() != 0)) {
                has_fly_or_spec = true;
            }
        }

        int vec_in = (city->Index() >= 0 && city->Index() < map.CityCount())
                             ? vec_to_city_count[static_cast<size_t>(city->Index())]
                             : 0;
        ml::MusterVec muster_vec{};
        muster_vec[0] = 0.0F;
        muster_vec[1] =
                (city->Index() == prev_hubs[0] || city->Index() == prev_hubs[1]) ? 1.0F : 0.0F;
        muster_vec[2] = std::tanh(dist_e / 35.0F);
        muster_vec[3] = std::tanh(dist_n / 35.0F);
        muster_vec[4] = std::tanh(threat_2 / 25.0F);
        muster_vec[5] = std::min(1.5F, static_cast<float>(city_armies) / 8.0F);
        muster_vec[6] = std::tanh(static_cast<float>(city_str) / 35.0F);
        muster_vec[7] = near_hero ? 1.0F : 0.0F;
        muster_vec[8] = std::min(1.5F, static_cast<float>(vec_in + in_transit) / 4.0F);
        muster_vec[9] = std::tanh(rear_str / 30.0F);
        muster_vec[10] = static_cast<float>(best_slot_str) / 9.0F;
        muster_vec[11] = has_fly_or_spec ? 1.0F : 0.0F;
        eligible_cities.push_back(EligibleMusterCity{city, muster_vec});
    }

    for (int round = 0; round < 2; round++) {
        ml::MusterVec stop_feat{};
        stop_feat[0] = 1.0F;
        stop_feat[12] = static_cast<float>(round);
        stop_feat[13] = 1.0F;

        std::vector<City*> cand_cities = {nullptr};
        std::vector<ml::MusterInVec> cands = {ml::PackMusterInput(ctx, stop_feat)};

        const City* hub0 = (round == 1 && belief.muster_cities[0] >= 0 &&
                            belief.muster_cities[0] < map.CityCount())
                                   ? &map.Cities()[belief.muster_cities[0]]
                                   : nullptr;

        for (const EligibleMusterCity& entry : eligible_cities) {
            City* city = entry.city;
            if (round == 1 && city->Index() == belief.muster_cities[0]) continue;
            ml::MusterVec muster_vec = entry.base_vec;
            muster_vec[12] = static_cast<float>(round);
            muster_vec[13] = hub0 ? std::tanh(static_cast<float>(move::Distance(
                                                      city->X(), city->Y(), hub0->X(), hub0->Y())) /
                                              35.0F)
                                  : std::tanh(static_cast<float>(own_cities.size()) / 10.0F);

            cand_cities.push_back(city);
            cands.push_back(ml::PackMusterInput(ctx, muster_vec));
        }

        if (cands.size() <= 1) break;
        std::vector<float> logits(cands.size());
        std::vector<float> log_mult(cands.size(), 0.0F);
        for (size_t i = 0; i < cands.size(); i++) {
            logits[i] = policy.ForwardMuster(cands[i])[0];
            if (i > 0 && cands.size() > 2)
                log_mult[i] = std::log(static_cast<float>(cands.size() - 1));
        }
        float log_prob = 0.0F;
        int pick = ml::SelectFromLogits(logits, muster_sampler, &log_prob, &log_mult);
        if (pick == 0 || !cand_cities[pick]) break;

        City* chosen = cand_cities[pick];
        belief.muster_cities[round] = chosen->Index();
        if (want_thoughts) {
            if (ModernMlPlayer* modern_player = ModernMlPlayer::FindMutableOn(side)) {
                modern_player->PushThought(AIThought{
                    "Empire", absl::StrFormat("Muster Hub #%d", round + 1), chosen->Name(),
                    chosen->X(), chosen->Y(), chosen->X(), chosen->Y(), logits[pick], 1.0F, false,
                    absl::StrFormat("Meeting point at %s (score %.2f)", chosen->Name(),
                                    logits[pick])});
            }
        }
    }
}

void RunStandardPhase(Game& game, Side& side, const ml::PolicyNetwork& policy,
                      const ml::ContextVec& ctx, ml::RolloutSampler* sampler) {
    Map& map = game.GetMap();
    if (!policy.UseStandard() || side.Index() < 0 || side.Index() >= map.ItemCount()) return;
    Item& std_item = map.Items()[side.Index()];
    if (std_item.type != rules::kItemStandard) return;
    std::vector<City*> own_cities = game::SideCities(game, side);
    if (own_cities.size() < 2) return;

    int our_side = side.Index();
    const SideBelief& belief = belief::Ensure(side);

    ml::RolloutSampler local_std_sampler;
    ml::RolloutSampler* std_sampler = sampler;
    if (!sampler || !sampler->current_turn) {
        local_std_sampler.temperature = sampler ? sampler->temperature : ml::DeployTemperature();
        local_std_sampler.rng_state = (game.GetRng().State() * 3266489917U) ^
                                      static_cast<uint32_t>(game.Turn() * 49157U) ^
                                      static_cast<uint32_t>((side.Index() + 1) * 0x9e3779b9U);
        std_sampler = &local_std_sampler;
    }

    auto build_field_std_features = [&](int field_x, int field_y, bool is_currently_planted,
                                        int cur_flow) {
        float dist_e = 100.0F;
        float dist_n = 100.0F;
        float dist_own = 100.0F;
        float threat_2 = 0.0F;
        for (City* vis_city : belief.visible_cities) {
            if (vis_city->IsRazed()) continue;
            float dist_tile = static_cast<float>(
                    move::Distance(field_x, field_y, vis_city->X(), vis_city->Y()));
            if (vis_city->OwnerIndex() == our_side) {
                dist_own = std::min(dist_own, dist_tile);
            } else if (vis_city->OwnerIndex() == kNone) {
                dist_n = std::min(dist_n, dist_tile);
            } else if (!diplomacy::IsAllied(game, our_side, vis_city->OwnerIndex())) {
                dist_e = std::min(dist_e, dist_tile);
            }
        }
        for (const auto& [tile_key, obs] : belief.stacks_by_tile) {
            (void)tile_key;
            if (obs.owner == our_side || obs.owner == kNone ||
                diplomacy::IsAllied(game, our_side, obs.owner))
                continue;
            int dist_tile = move::Distance(field_x, field_y, obs.x, obs.y);
            dist_e = std::min(dist_e, static_cast<float>(dist_tile));
            const ArmyType* top_at = game.ArmyTypes().ById(obs.top_type);
            int reach2 = std::max(8, ((top_at ? std::max(10, top_at->move) : 12) * 3) / 4) * 2;
            if (dist_tile <= reach2) {
                float top_str = top_at ? static_cast<float>(top_at->strength) : 4.0F;
                float rest_str = (obs.owner >= 0 && obs.owner < 8)
                                         ? belief.seen_enemy_avg_strength[obs.owner]
                                         : 4.0F;
                threat_2 +=
                        top_str + static_cast<float>(std::max(0, obs.visible_count - 1)) * rest_str;
            }
        }
        int tile_armies = 0;
        int tile_str = 0;
        for (Army* army : game::ArmiesOnTileSpan(game, field_x, field_y)) {
            if (army->OwnerIndex() == our_side) {
                tile_armies++;
                tile_str += army->Strength();
            }
        }
        float near_rear_str = 0.0F;
        for (Army* army : game.Armies()) {
            if (army->OwnerIndex() == our_side && !army->IsDisbanded() && !army->InTransit() &&
                army->X() != kNone &&
                move::Distance(field_x, field_y, army->X(), army->Y()) <= 20) {
                near_rear_str += static_cast<float>(army->Strength());
            }
        }
        bool on_road = (scn::RoadAt(map, field_x, field_y) % 0x20 != 0);

        ml::MusterVec muster_vec{};
        muster_vec[0] = 0.0F;
        muster_vec[1] = is_currently_planted ? 1.0F : 0.0F;
        muster_vec[2] = std::tanh(dist_e / 35.0F);
        muster_vec[3] = std::tanh(dist_n / 35.0F);
        muster_vec[4] = std::tanh(threat_2 / 25.0F);
        muster_vec[5] = std::min(1.5F, static_cast<float>(tile_armies) / 8.0F);
        muster_vec[6] = std::tanh(static_cast<float>(tile_str) / 35.0F);
        muster_vec[7] = 1.0F;
        muster_vec[8] = std::min(1.5F, static_cast<float>(cur_flow) / 4.0F);
        muster_vec[9] = std::tanh(near_rear_str / 30.0F);
        muster_vec[10] = std::tanh(dist_own / 20.0F);
        muster_vec[11] = on_road ? 1.0F : 0.0F;
        muster_vec[12] = 2.0F;  // Standard round tag (distinct from Castle Hub rounds 0 and 1)
        muster_vec[13] = std::tanh(static_cast<float>(own_cities.size()) / 10.0F);
        return muster_vec;
    };

    if (std_item.status == 3) {
        Army* carrier = nullptr;
        for (Army* army : game.Armies()) {
            if (army->OwnerIndex() == our_side && army->Hero() && !army->InTransit() &&
                army->X() != kNone && army->HasItem(&std_item)) {
                carrier = army;
                break;
            }
        }
        if (!carrier) return;
        int terrain = scn::TerrainAt(map, carrier->X(), carrier->Y());
        if (terrain == move::kWater || terrain == move::kShore || terrain == move::kCity ||
            terrain == move::kSite) {
            return;
        }
        for (const auto& item : map.Items()) {
            if (item.planted && item.x == carrier->X() && item.y == carrier->Y()) return;
        }

        // Ensure at least one owned city is land-connected (or builds fliers) to feed this tile.
        core::Flood flood_land = core::FloodFrom(game, our_side, carrier->X(), carrier->Y(), 72,
                                                 move::kLand, our_side);
        bool any_feeder = false;
        for (City* city : own_cities) {
            for (const auto& slot : city->Slots()) {
                const ArmyType* army_type = game.ArmyTypes().ById(slot.type);
                if (army_type && army_type->flies) {
                    any_feeder = true;
                    break;
                }
            }
            for (int dy = -1; dy <= 2 && !any_feeder; dy++) {
                for (int dx = -1; dx <= 2 && !any_feeder; dx++) {
                    int tile_x = city->X() + dx;
                    int tile_y = city->Y() + dy;
                    if (tile_x >= 0 && tile_y >= 0 && tile_x < map.Width() &&
                        tile_y < map.Height() &&
                        core::FloodAt(flood_land, tile_x, tile_y) < core::kUnreached) {
                        any_feeder = true;
                    }
                }
            }
            if (any_feeder) break;
        }
        if (!any_feeder) return;

        ml::MusterVec carry_m{};
        carry_m[0] = 1.0F;
        carry_m[1] = 1.0F;
        carry_m[12] = 2.0F;
        carry_m[13] = std::tanh(static_cast<float>(own_cities.size()) / 10.0F);
        ml::MusterVec plant_m = build_field_std_features(carrier->X(), carrier->Y(), false, 0);

        std::vector<float> logits = {
            policy.ForwardMuster(ml::PackMusterInput(ctx, carry_m))[0],
            policy.ForwardMuster(ml::PackMusterInput(ctx, plant_m))[0],
        };
        float log_prob = 0.0F;
        int pick = ml::SelectFromLogits(logits, std_sampler, &log_prob);
        if (pick == 1 && game::PlantFlag(game, side, carrier)) {
            if (ml::RecordThoughts()) {
                if (ModernMlPlayer* modern_player = ModernMlPlayer::FindMutableOn(side)) {
                    modern_player->PushThought(AIThought{
                        "Empire", "Standard", carrier->Name(), carrier->X(), carrier->Y(),
                        carrier->X(), carrier->Y(), logits[1], 1.0F, false,
                        absl::StrFormat("Planted Standard at (%d,%d) (score %.2f)", carrier->X(),
                                        carrier->Y(), logits[1])});
                }
            }
        }
    } else if (std_item.status == 1 && std_item.planted && std_item.x != kNone) {
        int std_x = std_item.x;
        int std_y = std_item.y;
        Army* hero_here = nullptr;
        for (Army* army : game::ArmiesOnTileSpan(game, std_x, std_y)) {
            if (army->OwnerIndex() == our_side && army->Hero() && !army->InTransit()) {
                hero_here = army;
                break;
            }
        }
        if (!hero_here) return;
        int cur_flow = static_cast<int>(game::VectoredTo(game, nullptr, &side).size());
        for (Army* army : game.Armies()) {
            if (army->OwnerIndex() == our_side && army->InTransit() &&
                std::holds_alternative<PlantedStandard>(army->TransitState()->dest)) {
                cur_flow++;
            }
        }

        ml::MusterVec pickup_m{};
        pickup_m[0] = 1.0F;
        pickup_m[1] = 0.0F;
        pickup_m[12] = 2.0F;
        pickup_m[13] = std::tanh(static_cast<float>(own_cities.size()) / 10.0F);
        ml::MusterVec keep_m = build_field_std_features(std_x, std_y, true, cur_flow);

        std::vector<float> logits = {
            policy.ForwardMuster(ml::PackMusterInput(ctx, pickup_m))[0],
            policy.ForwardMuster(ml::PackMusterInput(ctx, keep_m))[0],
        };
        float log_prob = 0.0F;
        int pick = ml::SelectFromLogits(logits, std_sampler, &log_prob);
        if (pick == 0) {
            hero::TakeItem(game, hero_here, &std_item);
            for (City* city : own_cities) {
                if (city->IsVectoredToStandard()) city->ClearVector();
            }
        }
    }
}

City* SelectHeroCity(Game& game, Side& side, City* default_city,
                     const ml::PolicyNetwork* custom_policy) {
    // No policy head is trained for where a new hero appears, so this is an
    // explicit rule rather than an untrained proxy: the own city under the
    // highest visible threat, else the game's default (the capital).
    (void)custom_policy;
    belief::Update(game, side);
    const SideBelief& belief = belief::Ensure(side);
    City* best = default_city;
    float best_threat = 0.0F;
    for (City* city : game::SideCities(game, side)) {
        float threat = city->Index() < static_cast<int>(belief.city_threat2_turn.size())
                               ? belief.city_threat2_turn[city->Index()]
                               : 0.0F;
        if (threat > best_threat) {
            best_threat = threat;
            best = city;
        }
    }
    return best;
}

}  // namespace w2::ai::modern
