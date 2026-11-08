/**
 * @file
 * @brief Modern AI EconNet economy phase (retention, slot buying, production, vectoring) and hero
 * city selection.
 */
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "absl/strings/str_format.h"

#include "util/util.hpp"
#include "warlords/ai.hpp"
#include "warlords/ai/belief.hpp"
#include "warlords/ai/core.hpp"
#include "warlords/ai/groups.hpp"
#include "warlords/ai/modern_internal.hpp"
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
    int our_side = side.index;
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
            if (!game.Alive(army) || army->Hero() || army->transit || army->x == kNone) continue;
            City* city = game::CityAt(game, army->x, army->y);
            if (!city || city->owner_index != our_side) continue;
            int city_armies = 0;
            for (int dy = 0; dy <= 1; dy++) {
                for (int dx = 0; dx <= 1; dx++) {
                    city_armies += static_cast<int>(
                            game::ArmiesAt(game, city->x + dx, city->y + dy).size());
                }
            }
            if (city_armies <= 2) continue;

            const ArmyType* army_type = game.types.ById(army->type);
            float threat = (city->index < static_cast<int>(belief.city_threat2_turn.size()))
                                   ? belief.city_threat2_turn[city->index]
                                   : 0.0F;
            float dist_e = (city->index < static_cast<int>(belief.city_nearest_enemy_dist.size()))
                                   ? belief.city_nearest_enemy_dist[city->index]
                                   : 100.0F;

            ml::ContextVec cur_ctx = ml::BuildContext(game, side);

            ml::EconVec econ_vec{};
            econ_vec[5] = 1.0F;
            econ_vec[7] = static_cast<float>(army->strength) / 9.0F;
            econ_vec[8] = static_cast<float>(army->max_moves) / 24.0F;
            econ_vec[10] = static_cast<float>(army->upkeep) / 15.0F;
            econ_vec[11] = (army_type && army_type->flies) ? 1.0F : 0.0F;
            econ_vec[12] =
                    (army_type && (army_type->siege || army_type->IsMagical())) ? 1.0F : 0.0F;
            econ_vec[13] =
                    (army->max_moves >= 14 && (!army_type || !army_type->flies)) ? 1.0F : 0.0F;
            econ_vec[14] = std::tanh(threat / 25.0F);
            econ_vec[15] = std::tanh(dist_e / 60.0F);
            econ_vec[ml::kEconCurrent] = 1.0F;  // keeping the army is the status quo
            ml::EconInVec keep_in = ml::PackEconInput(cur_ctx, econ_vec);

            ml::EconVec disband_vec = econ_vec;
            disband_vec[10] = -econ_vec[10];
            disband_vec[ml::kEconCurrent] = 0.0F;
            ml::EconInVec disband_in = ml::PackEconInput(cur_ctx, disband_vec);

            std::vector<ml::EconInVec> cands = {keep_in, disband_in};
            std::vector<float> logits = {policy.econ_net.Forward(keep_in)[0],
                                         policy.econ_net.Forward(disband_in)[0]};
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

    std::vector<City*> own_cities = game::SideCities(game, side);

    // Empire-wide slot upgrade rounds (up to 3 purchases/turn): each round asks EconNet
    // to compare HoldGold against every legal (city, unit) upgrade, stopping immediately
    // as soon as EconNet chooses HoldGold so multi-city empires neither hoard 10k gold
    // nor suffer 1 - p^N treasury-draining coin-flip cascades across N cities.
    auto menu = game::BuyableTypes(game);
    for (int buy_round = 0; buy_round < 3 && !own_cities.empty() && side.gold >= 250; buy_round++) {
        /** @brief A candidate production-slot upgrade in one city, or HoldGold when city is null.
         */
        struct BuyOption {
            City* city = nullptr;    ///< City to buy the slot in, or nullptr for HoldGold.
            int type_id = kNone;     ///< ArmyType ID to purchase, or kNone.
            ml::EconInVec in_vec{};  ///< Packed EconNet input.
        };
        ml::ContextVec buy_ctx = ml::BuildContext(game, side);
        std::vector<BuyOption> buy_opts;
        buy_opts.push_back(BuyOption{nullptr, kNone, make_hold_gold_in(buy_ctx)});

        for (City* city : own_cities) {
            bool full_slots = static_cast<int>(city->slots.size()) >= rules::kProductionSlots;
            int replace_price = (!city->slots.empty()) ? city->slots[0].price : 0;
            int best_existing_str = 0;
            for (const auto& slot : city->slots)
                best_existing_str = std::max(best_existing_str, slot.strength);
            float threat = city->index < static_cast<int>(belief.city_threat2_turn.size())
                                   ? belief.city_threat2_turn[city->index]
                                   : 0.0F;
            float dist_e = city->index < static_cast<int>(belief.city_nearest_enemy_dist.size())
                                   ? belief.city_nearest_enemy_dist[city->index]
                                   : 100.0F;
            for (const ArmyType* army_type : menu) {
                int price = std::abs(army_type->price);
                if (price <= 0 || price + 100 > side.gold) continue;
                if (full_slots && price <= replace_price) continue;
                if (!game::CannotBuy(game, side, *city, *army_type).empty()) continue;

                ml::EconVec econ_vec{};
                econ_vec[3] = 1.0F;
                econ_vec[7] = static_cast<float>(army_type->strength) / 9.0F;
                econ_vec[8] = static_cast<float>(army_type->move) / 24.0F;
                econ_vec[9] = static_cast<float>(army_type->time) / 6.0F;
                econ_vec[10] = static_cast<float>(price) /
                               std::max(100.0F, static_cast<float>(side.gold - 80));
                econ_vec[11] = army_type->flies ? 1.0F : 0.0F;
                econ_vec[12] = (army_type->siege || army_type->IsMagical()) ? 1.0F : 0.0F;
                econ_vec[13] = (army_type->move >= 14 && !army_type->flies) ? 1.0F : 0.0F;
                econ_vec[14] = std::tanh(threat / 25.0F);
                econ_vec[15] = std::tanh(dist_e / 60.0F);
                econ_vec[16] = static_cast<float>(city->slots.size()) /
                               static_cast<float>(rules::kProductionSlots);
                econ_vec[17] = static_cast<float>(army_type->strength - best_existing_str) / 9.0F;
                econ_vec[18] = static_cast<float>(army_type->strength * army_type->strength) /
                               static_cast<float>(std::max(1, army_type->time * 16));
                buy_opts.push_back(
                        BuyOption{city, army_type->id, ml::PackEconInput(buy_ctx, econ_vec)});
            }
        }

        if (buy_opts.size() <= 1) break;
        std::vector<float> logits(buy_opts.size());
        std::vector<float> log_mult(buy_opts.size(), 0.0F);
        for (size_t i = 0; i < buy_opts.size(); i++) {
            logits[i] = policy.econ_net.Forward(buy_opts[i].in_vec)[0];
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
        for (int si = 0; si < static_cast<int>(best_buy_city->slots.size()); si++) {
            if (best_buy_city->slots[si].type == bought_type) {
                game::SetProduction(game, *best_buy_city, si);
                break;
            }
        }
    }

    // City production macro-action: queried whenever a city has no active build
    // or has just completed a build (`city->countdown == slot.time`, matching
    // game.cpp:1331), so multi-turn unit builds complete without mid-countdown
    // cancellation while cities remain free to switch units after each build.
    ml::ContextVec prod_ctx = ml::BuildContext(game, side);
    for (City* city : own_cities) {
        bool mid_countdown =
                (city->producing >= 0 && city->producing < static_cast<int>(city->slots.size()) &&
                 city->countdown > 0 && city->countdown < city->slots[city->producing].time);
        if (city->slots.empty() || mid_countdown) continue;
        float threat = city->index < static_cast<int>(belief.city_threat2_turn.size())
                               ? belief.city_threat2_turn[city->index]
                               : 0.0F;
        float dist_e = city->index < static_cast<int>(belief.city_nearest_enemy_dist.size())
                               ? belief.city_nearest_enemy_dist[city->index]
                               : 100.0F;
        int gar_count = 0;
        for (int dy = 0; dy <= 1; dy++) {
            for (int dx = 0; dx <= 1; dx++) {
                gar_count +=
                        static_cast<int>(game::ArmiesAt(game, city->x + dx, city->y + dy).size());
            }
        }

        ml::EconVec stop_vec{};
        stop_vec[2] = 1.0F;
        stop_vec[10] = std::tanh(
                static_cast<float>(game::Upkeep(game, side) - game::Income(game, side)) / 30.0F);
        stop_vec[14] = std::tanh(threat / 25.0F);
        stop_vec[15] = std::tanh(dist_e / 60.0F);
        stop_vec[16] = std::tanh(static_cast<float>(gar_count) / 6.0F);
        stop_vec[ml::kEconCurrent] = (city->producing == kNone) ? 1.0F : 0.0F;

        std::vector<int> slot_indices = {kNone};
        std::vector<ml::EconInVec> cands = {ml::PackEconInput(prod_ctx, stop_vec)};
        for (size_t si = 0; si < city->slots.size(); si++) {
            const Slot& slot = city->slots[si];
            const ArmyType* army_type = game.types.ById(slot.type);
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
            econ_vec[ml::kEconCurrent] = (city->producing == static_cast<int>(si)) ? 1.0F : 0.0F;
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
            logits[i] = policy.econ_net.Forward(cands[i])[0];
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
        } else if (city->producing != slot_indices[pick] || !game::Producing(*city)) {
            game::SetProduction(game, *city, slot_indices[pick]);
        }
    }

    // City vectoring: re-evaluated each turn, but the vector already set is a
    // flagged candidate (ECON_CURRENT) rather than wiped, so keeping it is a choice
    for (City* src : own_cities) {
        if (!game::Producing(*src) || src->producing < 0 ||
            src->producing >= static_cast<int>(src->slots.size())) {
            src->vector_to = kNone;
            continue;
        }
        const Slot& slot = src->slots[src->producing];
        float src_threat = src->index < static_cast<int>(belief.city_threat2_turn.size())
                                   ? belief.city_threat2_turn[src->index]
                                   : 0.0F;
        float src_dist_e = src->index < static_cast<int>(belief.city_nearest_enemy_dist.size())
                                   ? belief.city_nearest_enemy_dist[src->index]
                                   : 100.0F;

        ml::EconVec keep_here{};
        keep_here[4] = 1.0F;
        keep_here[14] = std::tanh(src_threat / 25.0F);
        keep_here[15] = std::tanh(src_dist_e / 60.0F);
        keep_here[17] = 0.0F;
        keep_here[ml::kEconCurrent] = src->vector_to == kNone ? 1.0F : 0.0F;

        std::vector<City*> dest_cities = {nullptr};
        std::vector<ml::EconInVec> cands = {ml::PackEconInput(prod_ctx, keep_here)};

        for (City* dst : own_cities) {
            if (dst == src) continue;
            bool is_current = (src->vector_to == dst->index);
            if (!is_current &&
                static_cast<int>(game::VectoredTo(game, dst).size()) >= game::kMaxVectoredTo)
                continue;
            int v_turns = std::max(1, move::Distance(src->x, src->y, dst->x, dst->y) / 14 + 1);
            if (v_turns > 4) continue;
            float dst_threat = dst->index < static_cast<int>(belief.city_threat2_turn.size())
                                       ? belief.city_threat2_turn[dst->index]
                                       : 0.0F;
            float dst_dist_e = dst->index < static_cast<int>(belief.city_nearest_enemy_dist.size())
                                       ? belief.city_nearest_enemy_dist[dst->index]
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
            dest_cities.push_back(dst);
            cands.push_back(ml::PackEconInput(prod_ctx, vec_econ));
        }

        if (cands.size() > 1) {
            std::vector<float> logits(cands.size());
            std::vector<float> log_mult(cands.size(), 0.0F);
            for (size_t i = 0; i < cands.size(); i++) {
                logits[i] = policy.econ_net.Forward(cands[i])[0];
                if (i > 0 && cands.size() > 2)
                    log_mult[i] = std::log(static_cast<float>(cands.size() - 1));
            }
            float log_prob = 0.0F;
            int pick = ml::SelectFromLogits(logits, sampler, &log_prob, &log_mult);
            if (sampler && sampler->current_turn) {
                sampler->current_turn->econ_steps.push_back(
                        ml::EconStepRecord{std::move(cands), std::move(log_mult), pick, log_prob});
            }
            City* best_dest = dest_cities[pick];
            game::Vector(game, *src, best_dest);
            if (best_dest) {
                side.ai_thoughts.push_back(
                        AIThought{"Economy", src->name, best_dest->name, src->x, src->y,
                                  best_dest->x, best_dest->y, logits[pick], 1.0F, false,
                                  absl::StrFormat("Vectoring %s from %s -> %s", slot.name,
                                                  src->name, best_dest->name)});
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
        float threat = city->index < static_cast<int>(belief.city_threat2_turn.size())
                               ? belief.city_threat2_turn[city->index]
                               : 0.0F;
        if (threat > best_threat) {
            best_threat = threat;
            best = city;
        }
    }
    return best;
}

}  // namespace w2::ai::modern
