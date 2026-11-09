/**
 * @file
 * @brief Implementation of `w2::TurnSchedule`.
 */
#include "warlords/model/turn_schedule.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

#include "absl/log/absl_check.h"

namespace w2 {

void TurnSchedule::Reset() {
    order_ = {0, 1, 2, 3, 4, 5, 6, 7};
    cursor_ = -1;
}

void TurnSchedule::StartNewRound(bool random_turns, Rng& rng) {
    Reset();
    if (random_turns) {
        for (int i = 0; i < 20; ++i) {
            int first = rng.Dice(1, 8, -1);
            int second = rng.Dice(1, 8, -1);
            std::swap(order_[static_cast<size_t>(first)], order_[static_cast<size_t>(second)]);
        }
    }
}

SideId TurnSchedule::Advance() {
    ABSL_DCHECK_LT(cursor_, 7);
    ++cursor_;
    return SideId(order_[static_cast<size_t>(cursor_)]);
}

void TurnSchedule::SetCursor(int cursor) {
    cursor_ = std::clamp(cursor, -1, 7);
}

void TurnSchedule::SyncCursorToSide(SideId side) {
    cursor_ = -1;
    for (int i = 0; i < 8; ++i) {
        if (order_[static_cast<size_t>(i)] == side.Value()) {
            cursor_ = i;
            return;
        }
    }
}

SideId TurnSchedule::SideAt(int slot_index) const {
    if (slot_index < 0 || slot_index >= 8) return SideId::None();
    return SideId(order_[static_cast<size_t>(slot_index)]);
}

void TurnSchedule::SetOrder(const std::array<int, 8>& order) {
    for (size_t i = 0; i < 8; ++i) {
        order_[i] = std::clamp(order[i], 0, 7);
    }
}

}  // namespace w2
