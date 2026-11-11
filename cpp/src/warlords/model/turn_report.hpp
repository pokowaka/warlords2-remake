/**
 * @file
 * @brief Transient turn-opening news value objects (`Produced`, `Promotion`, `TurnReport`).
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "warlords/model/ids.hpp"

namespace w2 {

class Army;
struct HeroOffer;
struct QuestResult;

/** @brief Outcome category for an army produced or delivered at the start of a side's turn. */
enum class ProducedKind {
    kBuilt,    ///< Built in place inside its producing city.
    kSent,     ///< Built and dispatched onto road transit toward a vector target.
    kArrived,  ///< Completed road transit and arrived at its destination city or standard.
};

/** @brief Record of one army built, vectored out, or delivered at the start of a side's turn. */
struct Produced {
    ProducedKind kind = ProducedKind::kBuilt;  ///< Whether built, sent, or arrived.
    int type = 0;                              ///< Army type identifier.
    CityId city = CityId::None();              ///< Relevant city, or `CityId::None()`.
    bool standard = false;                     ///< True when delivered to the planted standard.
    std::uintptr_t unit_id = 0;                ///< Pool identity (`Army*`) when newly built.
};

/** @brief Record of a hero promoted as a side's turn opened and the strength gained. */
struct Promotion {
    Army* hero = nullptr;  ///< Promoted hero pointer.
    int gain = 0;          ///< Strength points gained from the promotion.
};

/**
 * @brief Transient news and pending UI notifications produced for one side during a turn.
 *
 * Populated by `TurnEngine` and `QuestSystem` as turns advance and consumed by
 * front-end modals, reports, and test assertions without polluting `Side` domain
 * entity state. Not persisted in save files.
 */
struct TurnReport {
    std::vector<Produced> produced;           ///< Armies built, dispatched, or delivered this turn.
    std::vector<std::string> diplo_news;      ///< Diplomatic treaty/war announcements at turn open.
    std::shared_ptr<HeroOffer> hero_offer;    ///< Pending hero recruitment offer for a human side.
    std::shared_ptr<QuestResult> quest_news;  ///< Pending quest completion/failure notification.
    std::vector<Promotion> promoted;          ///< Heroes promoted at the opening of the turn.

    /** @brief Resets all transient turn notifications in this report. */
    void Clear() {
        produced.clear();
        diplo_news.clear();
        hero_offer.reset();
        quest_news.reset();
        promoted.clear();
    }
};

}  // namespace w2
