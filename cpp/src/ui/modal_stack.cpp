#include "ui/modal_stack.hpp"

#include <utility>

#include "ui/kit.hpp"

namespace w2 {

ModalStack::ModalP ModalStack::Push(ModalP modal) {
    if (modal) modals_.push_back(modal);
    return modal;
}

void ModalStack::Pop(const kit::Modal* modal) {
    for (int i = static_cast<int>(modals_.size()) - 1; i >= 0; i--) {
        if (modals_[static_cast<size_t>(i)].get() == modal) {
            modals_.erase(modals_.begin() + i);
            return;
        }
    }
}

kit::Modal* ModalStack::Top() const {
    return modals_.empty() ? nullptr : modals_.back().get();
}

ModalStack::ModalP ModalStack::TopShared() const {
    return modals_.empty() ? nullptr : modals_.back();
}

void ModalStack::Clear() {
    modals_.clear();
}

bool ModalStack::AnyGreysMainControls() const {
    for (const auto& modal : modals_) {
        if (modal && modal->GreysMainControls()) return true;
    }
    return false;
}

bool ModalStack::HasTurnBanner() const {
    for (const auto& modal : modals_) {
        if (modal && modal->IsTurnBanner()) return true;
    }
    return false;
}

bool ModalStack::HasHeroOffer() const {
    for (const auto& modal : modals_) {
        if (modal && modal->IsHeroOffer()) return true;
    }
    return false;
}

size_t ModalStack::DialogCountExcludingTurnOpeners() const {
    size_t count = 0;
    for (const auto& modal : modals_) {
        if (modal && !modal->IsTurnBanner() && !modal->IsHeroOffer() && !modal->IsCityVictory()) {
            ++count;
        }
    }
    return count;
}

}  // namespace w2
