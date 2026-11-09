/**
 * @file
 * @brief LIFO stack of active `kit::Modal` dialogs (`w2::ModalStack`).
 */
#pragma once

#include <cstddef>
#include <memory>
#include <vector>

namespace kit {
class Modal;
}  // namespace kit

namespace w2 {

/**
 * @brief Owns the active stack of `kit::Modal` dialog instances in bottom-to-top paint order.
 *
 * Drawing iterates a snapshot copy (`Items()`) from bottom to top so modals may safely push or
 * pop during callbacks; input routes exclusively to `Top()` / `TopShared()`.
 *
 * Invariants:
 * - Every entry in `modals_` is non-null.
 *
 * Thread safety: Not thread-safe; mutated and queried exclusively on the main UI thread.
 */
class ModalStack {
  public:
    using ModalP = std::shared_ptr<kit::Modal>;
    using ModalList = std::vector<ModalP>;

    ModalStack() = default;
    ~ModalStack() = default;

    ModalStack(const ModalStack&) = delete;
    ModalStack& operator=(const ModalStack&) = delete;

    /** @brief Appends non-null `modal` to the top of the stack and returns it. */
    ModalP Push(ModalP modal);

    /** @brief Removes the topmost matching occurrence of `modal` from the stack. */
    void Pop(const kit::Modal* modal);

    /** @brief Returns a raw pointer to the topmost modal, or `nullptr` when empty. */
    kit::Modal* Top() const;

    /** @brief Returns a shared pointer to the topmost modal, or `nullptr` when empty. */
    ModalP TopShared() const;

    /** @brief Removes all open modals from the stack. */
    void Clear();

    /** @brief True when no modal is currently open. */
    bool Empty() const { return modals_.empty(); }

    /** @brief Total number of open modals on the stack. */
    size_t Size() const { return modals_.size(); }

    /** @brief Returns a snapshot copy of the stack in bottom-to-top draw order. */
    ModalList Items() const { return modals_; }

    /** @brief True when any open modal suppresses/greys main-screen controls (banner/hero offer).
     */
    bool AnyGreysMainControls() const;

    /** @brief True when a `TurnBannerModal` is currently on the stack. */
    bool HasTurnBanner() const;

    /** @brief True when a `HeroOfferModal` is currently on the stack. */
    bool HasHeroOffer() const;

    /** @brief Count of open modals excluding `TurnBannerModal` and `HeroOfferModal` (for `--script`
     * `eval`). */
    size_t DialogCountExcludingTurnOpeners() const;

  private:
    ModalList modals_;  ///< Active modals, bottom-most first and topmost last.
};

}  // namespace w2
