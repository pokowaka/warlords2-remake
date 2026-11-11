#include "warlords/ai/personality.hpp"

#include <algorithm>
#include <cmath>

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
    return 0.16F;
}

float ScoreMissionCharacterPrior(const Profile& profile, const MissionDoctrineContext& doc_ctx) {
    const float scale = DoctrineScale(profile.level);
    const auto& traits = profile.traits;
    const float aggr = traits[kAggression];
    const float hon = traits[kHonour];
    const float caut = traits[kCaution];
    const float fly = traits[kFlierAffinity];

    float bonus = 0.0F;
    if (doc_ctx.is_enemy_target && doc_ctx.vendetta_score > 0.05F &&
        doc_ctx.win_converged >= 0.45F) {
        const float vendetta_drive = std::clamp(
                0.45F + 0.55F * std::max(0.0F, aggr) + 0.45F * std::max(0.0F, -hon) +
                        ((profile.level == 1 && profile.card == 3) ? 0.35F : 0.0F),
                0.25F, 1.50F);
        const float odds_gate = std::min(1.0F, (doc_ctx.win_converged - 0.35F) * 2.5F);
        bonus += scale * vendetta_drive * doc_ctx.vendetta_score * odds_gate;
    }

    if (doc_ctx.is_enemy_capital && doc_ctx.win_converged >= 0.48F) {
        const float decap_drive = std::max(
                0.0F, 0.55F * aggr + 0.25F * fly - 0.30F * caut +
                              ((profile.level == 2 && profile.card == 6) ? 0.30F : 0.0F));
        bonus += scale * decap_drive;
    }

    if (doc_ctx.is_enemy_target && doc_ctx.win_converged < 0.62F && caut > 0.15F) {
        bonus -= scale * caut * (0.62F - doc_ctx.win_converged) * 1.40F;
    }
    if (doc_ctx.is_hold_garrison && doc_ctx.leaves_city_sparse && caut > 0.25F) {
        bonus += scale * caut * 0.22F;
    }
    return std::clamp(bonus, -0.65F, 0.65F);
}

void ApplyDiplomacyCharacterPrior(const Profile& profile, const DiplomacyDoctrineContext& doc_ctx,
                                  std::array<float, 4>& stance_logits, float& tribute_logit,
                                  float& joint_war_logit) {
    const float diplo_scale =
            profile.level <= 0 ? 1.15F : (profile.level == 1 ? 0.95F : 0.65F);
    const auto& traits = profile.traits;
    const float aggr = traits[kAggression];
    const float hon = traits[kHonour];
    const float avar = traits[kAvarice];
    const float coal = traits[kCoalitionDrive];
    const float caut = traits[kCaution];

    const bool unprovoked = (doc_ctx.truces_broken_by_rival == 0 &&
                             doc_ctx.cities_razed_by_rival == 0 && doc_ctx.vendetta_score < 0.40F);
    if (hon > 0.20F && (doc_ctx.cur_allied || (doc_ctx.cur_at_peace && doc_ctx.truce_turns > 0)) &&
        unprovoked) {
        const float keep = diplo_scale * hon * (doc_ctx.cur_allied ? 1.50F : 1.10F);
        stance_logits[0] += doc_ctx.cur_allied ? keep : 0.35F * keep;
        stance_logits[1] += keep;
        stance_logits[2] -= 0.85F * keep;
        stance_logits[3] -= 1.25F * keep;
    }

    if (doc_ctx.truces_broken_by_rival > 0 || doc_ctx.cities_razed_by_rival > 0 ||
        doc_ctx.vendetta_score > 0.35F) {
        const float atrocity = static_cast<float>(
                std::min(2, doc_ctx.truces_broken_by_rival + doc_ctx.cities_razed_by_rival));
        const float wrath = diplo_scale * (0.55F * doc_ctx.vendetta_score + 0.35F * atrocity) *
                            (0.75F + 0.45F * std::max(0.0F, aggr) + 0.35F * std::max(0.0F, hon));
        stance_logits[3] += wrath;
        stance_logits[0] -= wrath;
        stance_logits[1] -= 0.70F * wrath;
    }

    if (hon < -0.30F) {
        if (doc_ctx.cur_at_peace && doc_ctx.truce_turns <= 2 && doc_ctx.border_advantage > 0.30F &&
            doc_ctx.active_wars <= 1) {
            const float stab = diplo_scale * (-hon) * doc_ctx.border_advantage * 1.15F;
            stance_logits[3] += stab;
            stance_logits[1] -= 0.55F * stab;
            stance_logits[0] -= 0.75F * stab;
        } else if (doc_ctx.border_advantage < -0.30F && doc_ctx.active_wars >= 1) {
            const float mask_peace = diplo_scale * (-hon) * 0.55F;
            stance_logits[0] += 0.65F * mask_peace;
            stance_logits[1] += mask_peace;
            stance_logits[3] -= 0.70F * mask_peace;
        }
    }

    if (coal > 0.30F) {
        const bool honour_bound =
                hon > 0.45F && (doc_ctx.cur_allied || doc_ctx.truce_turns > 2) && unprovoked;
        if (doc_ctx.rival_is_runaway && !honour_bound) {
            const float contain =
                    diplo_scale * coal *
                    std::clamp((doc_ctx.runaway_share - 0.22F) * 3.0F, 0.25F, 1.10F);
            stance_logits[2] += 0.65F * contain;
            stance_logits[3] += contain;
            stance_logits[0] -= contain;
        } else if (doc_ctx.third_party_runaway) {
            const float rally =
                    diplo_scale * coal *
                    std::clamp((doc_ctx.runaway_share - 0.22F) * 2.8F, 0.30F, 1.10F);
            stance_logits[0] += 1.15F * rally;
            stance_logits[1] += 0.55F * rally;
            stance_logits[3] -= 0.80F * rally;
            joint_war_logit += 1.65F * rally;
        }
    }

    if (doc_ctx.cur_at_war && doc_ctx.border_advantage < -0.15F && (caut > 0.40F || avar > 0.60F)) {
        const float plea = diplo_scale *
                           (0.55F * std::max(0.0F, caut) + 0.45F * std::max(0.0F, avar)) *
                           (-doc_ctx.border_advantage);
        stance_logits[1] += plea;
        tribute_logit += 1.25F * plea;
    }
}

}  // namespace w2::ai::personality
