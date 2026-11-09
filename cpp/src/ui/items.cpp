/**
 * @file
 * @brief View > Items: the scenario's fourteen magic items.
 *
 * Popup 4, (120, 50) 400x360: "Items" (group 166) in font 1, "Items in this
 * scenario" (group 167) at the foot, and a row for each item from record 8
 * on, sorted by kind and then by value, 20 apart from y = 90: its name in
 * colour 7 ending at x = 304, and what it does from x = 336. Dialog 35: Done
 * (495) and Help (496), which shows HELP\HITEM.GFX.
 */
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/rules.hpp"

namespace items {

namespace {
constexpr kit::Rect kPopupRect{120, 50, 400, 360};  ///< Popup 4.
constexpr int kDialog = 35,                         ///< The dialog with the buttons.
        kDone = 495,                                ///< The Done button.
        kHelp = 496;                                ///< The Help button.
constexpr int kFirst = 8,                           ///< The first item record shown.
        kLast = 21;                                 ///< The last item record shown.

/** @brief Returns the line of group 167 describing `item_type`, if recognized. */
constexpr std::optional<int> KindLine(int item_type) {
    switch (item_type) {
    case w2::rules::kItemBattle:
        return 1;
    case w2::rules::kItemCommand:
        return 2;
    case w2::rules::kItemFlight:
        return 3;
    case w2::rules::kItemDoubleMove:
        return 4;
    case w2::rules::kItemGoldPerCity:
        return 5;
    default:
        return std::nullopt;
    }
}

/** @brief The Items dialog: the list, Done and Help. */
class Items final : public kit::Modal {
  public:
    explicit Items(w2::UiContext& ctx) : kit::Modal(ctx), dialog_view_(kit::View(ctx, kDialog)) {
        SetDialogView(&dialog_view_);
        dialog_view_.state[kDone] = w2::uidata::kNormal;
        dialog_view_.state[kHelp] = w2::uidata::kNormal;
        for (auto& item : ctx.ActiveGame().GetMap().Items()) {
            if (item.index >= kFirst && item.index <= kLast) list_.push_back(&item);
        }
        // An insertion sort, by kind then value: equal ones keep their order.
        for (size_t i = 1; i < list_.size(); i++) {
            size_t position = i;
            while (position > 0 && (list_[position]->type < list_[position - 1]->type ||
                                    (list_[position]->type == list_[position - 1]->type &&
                                     list_[position]->value < list_[position - 1]->value))) {
                std::swap(list_[position], list_[position - 1]);
                position--;
            }
        }
    }

    void Draw() override {
        w2::UiContext& ctx = Ctx();
        kit::Popup(ctx, kPopupRect);
        ctx.Renderer().SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(ctx, 1), kit::Text(ctx, 0xa6, 0), 320, 52);
        kit::Centred(kit::GetFont(ctx, 2), kit::Text(ctx, 0xa7, 0), 320, 382);
        const Font& font = kit::GetFont(ctx, 2).Colours(7, 0);
        // The kind's line carries over when an item is of no kind it knows.
        int kind_line = 0;
        for (size_t i = 0; i < list_.size(); i++) {
            int y = 90 + 20 * static_cast<int>(i);
            if (auto known_kind = KindLine(list_[i]->type)) kind_line = *known_kind;
            kit::Right(font, list_[i]->name, 304, y);
            font.Draw(w2::Format(kit::Text(ctx, 0xa7, kind_line), list_[i]->value), 336, y);
        }
        kit::DrawControls(ctx, dialog_view_);
    }

    void Mousepressed(int x, int y, int) override {
        const auto* control = kit::ControlAt(dialog_view_, x, y);
        if (!control) return;
        if (control->id == kDone) {
            PopSelf();
        } else if (control->id == kHelp) {
            help::Open(Ctx(), "HELP\\HITEM.GFX");
        }
    }

    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") PopSelf();
    }

  private:
    using ItemList = std::vector<w2::Item*>;

    screen::View dialog_view_;  ///< Dialog 35's buttons.
    ItemList list_;             ///< The items shown, in the order shown.
};
}  // namespace

/**
 * @brief Opens View > Items: the scenario's magic items, sorted by kind and
 * then by value.
 */
void Open(w2::UiContext& ctx) {
    kit::Push(ctx, std::make_shared<Items>(ctx));
}

}  // namespace items
