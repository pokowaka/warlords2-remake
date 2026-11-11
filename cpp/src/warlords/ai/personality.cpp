#include "warlords/ai/personality.hpp"

#include <algorithm>
#include <cmath>

#include "absl/strings/str_format.h"

namespace w2::ai::personality {

namespace {

/**
 * @brief The 27 character profiles, by level (Knight, Lord, Warlord) and card.
 *
 * Traits are in TraitIndex order: aggression, honour, avarice, scorched
 * earth, hero focus, coalition drive, cavalry, flier and siege affinity,
 * caution.
 */
const Profile kProfiles[3][9] = {
    // Knights (Level 0)
    {
        {0,
         0,
         "Standard Knight",
         "Chivalrous Defender",
         "We honour our treaties and defend our realm.",
         {-0.2F, 0.7F, -0.2F, -0.8F, 0.2F, 0.3F, 0.0F, 0.0F, 0.0F, 0.6F}},
        {0,
         1,
         "Winston the Woeful",
         "Cautious Spearman",
         "A wall of pikes keeps the wicked at bay.",
         {-0.4F, 0.8F, -0.1F, -0.9F, 0.0F, 0.4F, -0.3F, -0.2F, 0.2F, 0.8F}},
        {0,
         2,
         "Brave Brave Sir Robin",
         "Pacifist Isolationist",
         "When danger rears its ugly head, we bravely turn and flee!",
         {-0.8F, 0.6F, 0.1F, -1.0F, 0.3F, 0.7F, 0.4F, 0.3F, -0.5F, 0.9F}},
        {0,
         3,
         "Morgan the Moribund",
         "Heavy Garrison Turtle",
         "Our stone walls shall outlast your ambition.",
         {-0.5F, 0.7F, 0.2F, -0.7F, -0.2F, 0.3F, -0.5F, -0.3F, 0.5F, 0.8F}},
        {0,
         4,
         "Timmy the Tremulous",
         "Fast Scout & Skirmisher",
         "Swift riders see all and strike where none stand.",
         {-0.2F, 0.5F, 0.0F, -0.6F, 0.4F, 0.4F, 0.7F, 0.5F, -0.4F, 0.6F}},
        {0,
         5,
         "Ulfrid the Unlucky",
         "Overeager Crusader",
         "Glory awaits in the next charge!",
         {0.2F, 0.6F, -0.3F, -0.5F, 0.5F, 0.2F, 0.2F, 0.1F, 0.0F, 0.2F}},
        {0,
         6,
         "Simon the Superfluous",
         "Erratic Opportunist",
         "Who knows where our banners will march tomorrow?",
         {0.1F, -0.1F, 0.3F, -0.2F, 0.3F, 0.1F, 0.2F, 0.3F, 0.1F, 0.3F}},
        {0,
         7,
         "Charles the Craven",
         "Blustering Hoarder",
         "Pay due tribute and our mighty host may spare thee.",
         {-0.3F, 0.2F, 0.8F, -0.4F, 0.1F, 0.5F, 0.0F, 0.1F, 0.2F, 0.7F}},
        {0,
         8,
         "Ethel the Unready",
         "Scattered Builder",
         "Our cities grow prosperous while we prepare.",
         {-0.3F, 0.5F, 0.4F, -0.6F, 0.1F, 0.4F, 0.0F, 0.0F, 0.1F, 0.5F}},
    },
    // Lords (Level 1)
    {
        {1,
         0,
         "Standard Lord",
         "Feudal Tactician",
         "Strength and statecraft rule this land.",
         {0.2F, 0.3F, 0.1F, -0.3F, 0.3F, 0.3F, 0.2F, 0.2F, 0.2F, 0.1F}},
        {1,
         1,
         "Rowan the Rancorous",
         "Treacherous Brawler",
         "A treaty is merely a pause to sharpen the blade.",
         {0.7F, -0.7F, 0.2F, 0.2F, 0.3F, -0.3F, 0.3F, 0.1F, 0.2F, -0.3F}},
        {1,
         2,
         "Roland the Rabid",
         "Pyromaniac Razer",
         "Ashes and cinders leave no rebels behind!",
         {0.8F, -0.5F, 0.4F, 0.95F, 0.2F, -0.4F, 0.4F, 0.2F, 0.3F, -0.4F}},
        {1,
         3,
         "Raymond Ratsbane",
         "Vendetta Assassin",
         "Cross us once and we shall hunt your throne to the end.",
         {0.6F, -0.6F, 0.1F, 0.3F, 0.6F, -0.2F, 0.4F, 0.5F, 0.1F, -0.1F}},
        {1,
         4,
         "Rebecca the Rapacious",
         "Mercenary Extortionist",
         "Gold buys peace, or gold buys the armies that take your cities.",
         {0.3F, -0.2F, 0.95F, 0.4F, 0.4F, 0.4F, 0.3F, 0.2F, 0.1F, 0.2F}},
        {1,
         5,
         "Rodney the Ravager",
         "Siege Juggernaut",
         "Slow and unstoppable, our engines crush every fortress.",
         {0.5F, 0.1F, 0.2F, 0.5F, 0.1F, 0.1F, -0.2F, -0.1F, 0.9F, 0.2F}},
        {1,
         6,
         "Rodrik the Retrograde",
         "Stubborn Traditionalist",
         "What we claim is ours by ancient right.",
         {0.2F, 0.6F, 0.1F, -0.4F, 0.1F, 0.2F, 0.1F, -0.2F, 0.4F, 0.4F}},
        {1,
         7,
         "Randal the Rampageous",
         "Snowball Conqueror",
         "Each victory feeds the next campaign!",
         {0.7F, -0.5F, 0.3F, 0.3F, 0.5F, -0.1F, 0.6F, 0.4F, 0.3F, -0.2F}},
        {1,
         8,
         "Richard the Riotous",
         "Relentless Raider",
         "Your treasuries and border towns are ripe for plunder!",
         {0.6F, -0.4F, 0.8F, 0.7F, 0.3F, -0.2F, 0.6F, 0.3F, 0.0F, -0.2F}},
    },
    // Warlords (Level 2)
    {
        {2,
         0,
         "Standard Warlord",
         "Imperial Warlord",
         "All crowns shall bow before our empire.",
         {0.6F, 0.0F, 0.3F, 0.0F, 0.6F, 0.5F, 0.4F, 0.5F, 0.5F, -0.1F}},
        {2,
         1,
         "Attila the Hun",
         "Outrider Blitzkrieg",
         "Our horsemen cross the plains faster than word of our coming!",
         {0.9F, -0.6F, 0.4F, 0.5F, 0.5F, 0.3F, 0.95F, 0.4F, 0.1F, -0.4F}},
        {2,
         2,
         "Margaret the Ferrous",
         "Iron-Willed Hegemon",
         "This lady is not for turning. Surrender or be broken.",
         {0.6F, 0.5F, 0.2F, -0.2F, 0.5F, 0.6F, 0.3F, 0.4F, 0.7F, 0.2F}},
        {2,
         3,
         "Ian the Infamous",
         "Heavy Monster & Siege Lord",
         "War elephants and ancient beasts trample every wall.",
         {0.7F, -0.3F, 0.3F, 0.2F, 0.6F, 0.4F, 0.2F, 0.3F, 0.95F, 0.0F}},
        {2,
         4,
         "Nikolas the Nefarious",
         "Chaos Machiavellian",
         "Alliances are masks worn until the dagger finds its mark.",
         {0.7F, -0.9F, 0.6F, 0.4F, 0.7F, 0.6F, 0.4F, 0.7F, 0.3F, -0.2F}},
        {2,
         5,
         "Steven the Insidious",
         "Relentless Swarm Master",
         "Cut down one host and three more march from our citadels.",
         {0.8F, -0.4F, 0.3F, 0.1F, 0.5F, 0.5F, 0.5F, 0.6F, 0.5F, -0.2F}},
        {2,
         6,
         "Hand the Hammer",
         "Decapitation Striker",
         "We strike straight for the throat and the capital!",
         {0.95F, -0.3F, 0.2F, 0.3F, 0.7F, 0.3F, 0.6F, 0.8F, 0.6F, -0.4F}},
        {2,
         7,
         "Roger the Unrepentant",
         "Hero & Ruin Warlord",
         "Ancient artifacts and blessed champions make us invincible.",
         {0.75F, -0.5F, 0.5F, 0.3F, 0.95F, 0.4F, 0.4F, 0.8F, 0.4F, -0.1F}},
        {2,
         8,
         "William the Wily",
         "Scheming Coalitionist",
         "Why fight alone when the world can be turned against our rival?",
         {0.5F, -0.6F, 0.7F, -0.1F, 0.6F, 0.95F, 0.4F, 0.6F, 0.4F, 0.2F}},
    },
};

}  // namespace

const Profile& ProfileFor(int level, int card) {
    int clamped_level = std::clamp(level, 0, 2);
    int clamped_card = std::clamp(card, 0, 8);
    return kProfiles[clamped_level][clamped_card];
}

const Profile& ForSide(const Side& side) {
    return ProfileFor(side.Level(), side.Card());
}

const char* ArchetypeName(int arch_id) {
    switch (arch_id) {
    case kArchImperial:
        return "Imperial";
    case kArchBerserker:
        return "Berserker";
    case kArchGuardian:
        return "Guardian";
    case kArchSchemer:
        return "Schemer";
    case kArchQuestor:
        return "Questor";
    default:
        return "Unknown";
    }
}

ArchetypeWeightArray ArchetypeWeights(const Profile& profile) {
    const auto& traits = profile.traits;
    const float aggr = traits[kAggression];
    const float hon = traits[kHonour];
    const float avar = traits[kAvarice];
    const float scor = traits[kScorchedEarth];
    const float hero = traits[kHeroFocus];
    const float coal = traits[kCoalitionDrive];
    const float cav = traits[kCavalryAffinity];
    const float fly = traits[kFlierAffinity];
    const float siege = traits[kSiegeAffinity];
    const float caut = traits[kCaution];

    std::array<float, kNumArchetypes> scores{};
    scores[kArchImperial] = 0.85F + (profile.card == 0 ? 1.65F : 0.0F) -
                            0.25F * (std::fabs(aggr) + std::fabs(caut));
    scores[kArchBerserker] = 1.25F * aggr + 0.95F * scor + 0.70F * cav - 0.80F * caut - 0.30F * hon;
    scores[kArchGuardian] =
            1.10F * caut + 0.95F * hon - 0.85F * aggr - 0.55F * scor + 0.35F * siege;
    scores[kArchSchemer] = 1.25F * avar + 1.10F * coal - 0.70F * hon +
                           0.80F * std::max(0.0F, cav) * std::max(0.0F, caut);
    scores[kArchQuestor] = 1.25F * hero + 0.95F * siege + 0.60F * fly;

    float max_score = scores[0];
    for (int i = 1; i < kNumArchetypes; i++) max_score = std::max(max_score, scores[i]);

    ArchetypeWeightArray weights{};
    float sum = 0.0F;
    for (int i = 0; i < kNumArchetypes; i++) {
        weights[i] = std::exp(1.15F * (scores[i] - max_score));
        sum += weights[i];
    }
    if (sum > 0.0F) {
        for (int i = 0; i < kNumArchetypes; i++) weights[i] /= sum;
    }
    return weights;
}

float DifficultyTemperature(int level, float warlord_temp) {
    if (level <= 0) return std::max(warlord_temp, 0.35F);
    if (level == 1) return std::max(warlord_temp, 0.12F);
    return warlord_temp;
}

float DoctrineScale(int level) {
    if (level <= 0) return 0.80F;
    if (level == 1) return 0.45F;
    return 0.13F;
}

float ScoreMissionCharacterPrior(const Profile& profile, const MissionDoctrineContext& doc_ctx) {
    const float scale = DoctrineScale(profile.level);
    const auto& traits = profile.traits;
    const float aggr = traits[kAggression];
    const float hon = traits[kHonour];
    const float caut = traits[kCaution];
    const float fly = traits[kFlierAffinity];

    float bonus = 0.0F;
    if (doc_ctx.is_enemy_target && doc_ctx.vendetta_score > 0.25F &&
        doc_ctx.win_converged >= (profile.level >= 2 ? 0.65F : 0.45F)) {
        const float vendetta_drive = std::clamp(
                0.45F + 0.55F * std::max(0.0F, aggr) + 0.45F * std::max(0.0F, -hon) +
                        ((profile.level == 1 && profile.card == 3) ? 0.35F : 0.0F),
                0.25F, 1.50F);
        const float odds_gate = std::min(1.0F, (doc_ctx.win_converged - 0.35F) * 2.5F);
        const float tier_damp = profile.level >= 2 ? 0.35F : 1.0F;
        bonus += scale * tier_damp * vendetta_drive * doc_ctx.vendetta_score * odds_gate;
    }

    if (doc_ctx.is_enemy_capital &&
        doc_ctx.win_converged >= (profile.level >= 2 ? 0.68F : 0.52F) &&
        (profile.level < 2 || aggr >= 0.85F)) {
        const float decap_drive = std::max(
                0.0F, 0.55F * aggr + 0.25F * fly - 0.30F * caut +
                              ((profile.level == 2 && profile.card == 6) ? 0.30F : 0.0F));
        bonus += scale * decap_drive;
    }

    if (profile.level < 2 && doc_ctx.is_enemy_target && doc_ctx.win_converged < 0.62F &&
        caut > 0.15F) {
        bonus -= scale * caut * (0.62F - doc_ctx.win_converged) * 1.40F;
    }
    if (profile.level < 2 && doc_ctx.is_hold_garrison && doc_ctx.leaves_city_sparse &&
        caut > 0.25F) {
        bonus += scale * caut * 0.22F;
    }
    if (doc_ctx.is_ruin_or_temple && doc_ctx.stack_has_hero &&
        traits[kHeroFocus] >= (profile.level >= 2 ? 0.80F : 0.25F)) {
        bonus += scale * traits[kHeroFocus] * 0.95F;
    }
    return std::clamp(bonus, -0.65F, 0.65F);
}

void ApplyDiplomacyCharacterPrior(const Profile& profile, const DiplomacyDoctrineContext& doc_ctx,
                                  std::array<float, 4>& stance_logits, float& tribute_logit,
                                  float& joint_war_logit) {
    const float diplo_scale =
            profile.level <= 0 ? 1.15F : (profile.level == 1 ? 0.90F : 0.50F);
    const auto& traits = profile.traits;
    const float aggr = traits[kAggression];
    const float hon = traits[kHonour];
    const float avar = traits[kAvarice];
    const float coal = traits[kCoalitionDrive];
    const float caut = traits[kCaution];

    const bool unprovoked = (doc_ctx.truces_broken_by_rival == 0 &&
                             doc_ctx.cities_razed_by_rival == 0 && doc_ctx.vendetta_score < 0.40F);
    const bool keep_ally = doc_ctx.cur_allied && profile.level <= 1;
    const bool keep_truce = doc_ctx.cur_at_peace && doc_ctx.truce_turns > 0;
    if (hon > 0.20F && (keep_ally || keep_truce) && unprovoked) {
        const float tier_damp = profile.level >= 2 ? 0.25F : 1.0F;
        const float keep = diplo_scale * tier_damp * hon * (keep_ally ? 1.50F : 1.10F);
        stance_logits[0] += keep_ally ? keep : 0.35F * keep;
        stance_logits[1] += keep;
        stance_logits[2] -= 0.85F * keep;
        stance_logits[3] -= 1.25F * keep;
    }

    if (doc_ctx.vendetta_score >= 0.45F &&
        !(doc_ctx.active_wars >= 2 && doc_ctx.border_advantage < 0.10F)) {
        const float atrocity = static_cast<float>(
                std::min(2, doc_ctx.truces_broken_by_rival + doc_ctx.cities_razed_by_rival));
        const float tier_damp = profile.level >= 2 ? 0.20F : 1.0F;
        const float wrath = diplo_scale * tier_damp *
                            (0.55F * doc_ctx.vendetta_score + 0.35F * atrocity) *
                            (0.75F + 0.45F * std::max(0.0F, aggr) + 0.35F * std::max(0.0F, hon));
        stance_logits[3] += wrath;
        stance_logits[0] -= wrath;
        stance_logits[1] -= 0.70F * wrath;
    }

    if (hon <= (profile.level >= 2 ? -0.75F : -0.35F)) {
        if (doc_ctx.cur_at_peace && doc_ctx.truce_turns <= (profile.level >= 2 ? 0 : 2) &&
            doc_ctx.border_advantage > (profile.level >= 2 ? 0.60F : 0.40F) &&
            doc_ctx.active_wars == 0) {
            const float stab = diplo_scale * (-hon) * doc_ctx.border_advantage * 1.15F;
            stance_logits[3] += stab;
            stance_logits[1] -= 0.55F * stab;
            stance_logits[0] -= 0.75F * stab;
        } else if (profile.level <= 1 && doc_ctx.border_advantage < -0.25F &&
                   doc_ctx.active_wars >= 1) {
            const float mask_peace = diplo_scale * (-hon) * 0.65F;
            stance_logits[0] += 0.75F * mask_peace;
            stance_logits[1] += mask_peace;
            stance_logits[3] -= 0.80F * mask_peace;
        }
    }

    if (coal >= (profile.level >= 2 ? 0.80F : 0.50F)) {
        const bool honour_bound =
                hon > 0.45F && (doc_ctx.cur_allied || doc_ctx.truce_turns > 2) && unprovoked;
        if (profile.level <= 1 && doc_ctx.rival_is_runaway && !honour_bound) {
            const float contain =
                    diplo_scale * coal *
                    std::clamp((doc_ctx.runaway_share - 0.25F) * 3.0F, 0.25F, 1.10F);
            stance_logits[2] += 0.65F * contain;
            if (doc_ctx.active_wars == 0) {
                stance_logits[3] += contain;
            }
            stance_logits[0] -= contain;
        } else if (doc_ctx.third_party_runaway && doc_ctx.runaway_share >= 0.38F &&
                   doc_ctx.border_advantage < 0.20F && !doc_ctx.cur_at_war) {
            const float rally =
                    diplo_scale * coal *
                    std::clamp((doc_ctx.runaway_share - 0.22F) * 2.8F, 0.30F, 1.10F);
            stance_logits[0] += 1.35F * rally;
            stance_logits[1] += 0.55F * rally;
            stance_logits[3] -= 0.65F * rally;
            joint_war_logit += 1.80F * rally;
        }
    }

    if (profile.level <= 1 && doc_ctx.cur_at_war && doc_ctx.border_advantage < -0.15F &&
        (caut > 0.40F || avar > 0.60F)) {
        const float plea = diplo_scale *
                           (0.55F * std::max(0.0F, caut) + 0.45F * std::max(0.0F, avar)) *
                           (-doc_ctx.border_advantage);
        stance_logits[1] += plea;
        tribute_logit += 1.25F * plea;
    }
}

float ScoreUnitCharacterPrior(const Profile& profile, const UnitDoctrineContext& unit_ctx) {
    const float econ_scale =
            profile.level <= 0 ? 0.75F : (profile.level == 1 ? 0.55F : 0.12F);
    const auto& traits = profile.traits;
    float bonus = 0.0F;
    if (unit_ctx.is_cavalry) {
        bonus += econ_scale * traits[kCavalryAffinity] * 0.90F;
    }
    if (unit_ctx.is_flier) {
        bonus += econ_scale * traits[kFlierAffinity] * 0.95F;
    }
    if (unit_ctx.is_heavy_or_siege) {
        bonus += econ_scale * traits[kSiegeAffinity] * 1.05F;
    }
    return std::clamp(bonus, -0.75F, 0.75F);
}

float ScoreHoldGoldCharacterPrior(const Profile& profile, int gold) {
    const float hoard =
            std::max(0.0F, profile.traits[kAvarice]) * std::max(0.0F, profile.traits[kCaution]);
    if (hoard <= 0.25F || gold >= 650) return 0.0F;
    const float deficit = std::clamp(static_cast<float>(650 - gold) / 400.0F, 0.0F, 1.0F);
    return DoctrineScale(profile.level) * hoard * deficit * 0.85F;
}

void ApplySpoilsCharacterPrior(const Profile& profile, float local_threat, int city_income,
                               int pillage_gold, int sack_gold, const std::vector<int>& modes,
                               std::vector<float>& scores) {
    const float spoil_scale =
            profile.level <= 0 ? 1.10F : (profile.level == 1 ? 0.90F : 0.45F);
    const auto& traits = profile.traits;
    const float scor = traits[kScorchedEarth];
    const float avar = traits[kAvarice];
    const float hon = traits[kHonour];
    const size_t count = std::min(modes.size(), scores.size());

    for (size_t idx = 0; idx < count; ++idx) {
        const int mode = modes[idx];
        if (scor <= -0.20F || hon >= 0.45F) {
            const float preserve =
                    spoil_scale * (1.35F * std::max(0.0F, -scor) + 0.60F * std::max(0.0F, hon));
            if (profile.level <= 1) {
                if (mode == 0) scores[idx] += 0.95F * preserve;
                if (mode == 1 && local_threat < 8.0F && avar < 0.40F) {
                    scores[idx] -= 0.75F * preserve;
                }
            } else if (scor <= -0.45F && mode == 0) {
                scores[idx] += 0.35F * preserve;
            }
            if (mode == 2) scores[idx] -= 1.10F * preserve;
            if (mode == 3) scores[idx] -= 2.20F * preserve;
        }
        if (scor >= 0.45F) {
            if (profile.level <= 1 && (local_threat > 4.0F || city_income <= 15)) {
                const float burn = spoil_scale * scor * (local_threat > 6.0F ? 3.55F : 2.75F);
                if (mode == 3) scores[idx] += burn;
                if (mode == 1) scores[idx] -= 0.40F * burn;
                if (mode == 0) scores[idx] -= 0.50F * burn;
            } else if (profile.level >= 2 && local_threat > 16.0F && city_income <= 12) {
                if (mode == 3) scores[idx] += 2.60F * scor;
            }
        }
        if (avar >= 0.50F) {
            if (mode == 1 && pillage_gold >= 100) {
                const float pillage_fac =
                        std::min(1.0F, static_cast<float>(pillage_gold) / 220.0F);
                scores[idx] += spoil_scale * avar * pillage_fac * 0.90F;
            }
            if (mode == 2 && sack_gold >= 260 && (local_threat > 5.0F || profile.level <= 1)) {
                const float sack_fac = std::min(1.0F, static_cast<float>(sack_gold) / 400.0F);
                scores[idx] += spoil_scale * avar * sack_fac * 0.85F;
            }
        }
    }
}

std::string FormatCharacterDispatch(const Profile& profile, CharacterEvent event,
                                    const CharacterDispatchContext& ctx) {
    const std::string actor = ctx.actor_faction.empty() ? profile.name : ctx.actor_faction;
    const std::string target = ctx.target_faction.empty() ? "your realm" : ctx.target_faction;
    const std::string third = ctx.third_faction.empty() ? "the hegemon" : ctx.third_faction;
    const std::string city = ctx.city_name.empty() ? "this stronghold" : ctx.city_name;
    const float hon = profile.traits[kHonour];

    switch (event) {
    case CharacterEvent::kVendettaWar:
        if (ctx.heroes_slain > 0 && ctx.cities_lost > 0) {
            return absl::StrFormat(
                    "\"%s\" %s slew %d of our champions and seized %d of our cities—%s answers "
                    "with war without quarter!",
                    profile.motto, target, ctx.heroes_slain, ctx.cities_lost, profile.name);
        }
        if (ctx.heroes_slain > 0) {
            return absl::StrFormat(
                    "\"%s\" %s cut down our Hero in battle—%s marches for vengeance!",
                    profile.motto, target, profile.name);
        }
        if (hon > 0.35F) {
            return absl::StrFormat(
                    "\"%s\" %s wronged our realm (%d cities lost); %s draws the sword in "
                    "righteous retribution!",
                    profile.motto, target, std::max(1, ctx.cities_lost), profile.name);
        }
        return absl::StrFormat("\"%s\" %s shall pay in steel for wronging %s!", profile.motto,
                               target, actor);

    case CharacterEvent::kBetrayalWar:
        return absl::StrFormat(
                "\"%s\" Parchment truces bind only the weak, %s—your frontier lies open to %s!",
                profile.motto, target, profile.name);

    case CharacterEvent::kCoalitionAlliance:
        return absl::StrFormat(
                "\"%s\" %s grows too mighty! Forge a Military Alliance with %s and join our war "
                "against %s!",
                profile.motto, third, actor, third);

    case CharacterEvent::kAllianceOffer:
        return absl::StrFormat(
                "\"%s\" %s (%s) proposes a full Military Alliance with %s and shared vision.",
                profile.motto, actor, profile.name, target);

    case CharacterEvent::kTributeDemand:
        return absl::StrFormat(
                "\"%s\" Yield %d gold in tribute to %s, %s, lest our hosts collect it from your "
                "walls!",
                profile.motto, std::max(50, ctx.gold_amount), profile.name, target);

    case CharacterEvent::kPeacePlea:
        if (ctx.gold_amount > 0) {
            return absl::StrFormat(
                    "\"%s\" %s offers %d gold and a 10-turn truce to end this war with %s.",
                    profile.motto, profile.name, ctx.gold_amount, target);
        }
        return absl::StrFormat(
                "\"%s\" Let %s and %s lay down arms and sign a 10-turn truce.", profile.motto,
                actor, target);

    case CharacterEvent::kCityRazed:
        return absl::StrFormat("%s (%s) burns %s to ashes! \"%s\"", actor, profile.name, city,
                               profile.motto);

    case CharacterEvent::kCityPillaged:
        return absl::StrFormat("%s (%s) plunders %d gold from %s! \"%s\"", actor, profile.name,
                               std::max(1, ctx.gold_amount), city, profile.motto);

    case CharacterEvent::kCapitalCaptured:
        return absl::StrFormat("%s (%s) storms the capital of %s at %s! \"%s\"", actor,
                               profile.name, target, city, profile.motto);
    }
    return profile.motto;
}

}  // namespace w2::ai::personality
