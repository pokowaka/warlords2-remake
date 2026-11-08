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
#include <map>

#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/rules.hpp"

namespace items {

namespace {
const kit::Rect kPopupRect{120, 50, 400, 360};  ///< Popup 4.
const int kDialog = 35,                         ///< The dialog with the buttons.
        kDone = 495,                            ///< The Done button.
        kHelp = 496;                            ///< The Help button.
const int kFirst = 8,                           ///< The first item record shown.
        kLast = 21;                             ///< The last item record shown.
/// The line of group 167 that says what an item of each kind does.
const std::map<int, int> kKindLine = {{w2::rules::kItemBattle, 1},
                                      {w2::rules::kItemCommand, 2},
                                      {w2::rules::kItemFlight, 3},
                                      {w2::rules::kItemDoubleMove, 4},
                                      {w2::rules::kItemGoldPerCity, 5}};

/** @brief The Items dialog: the list, Done and Help. */
struct Items : kit::Modal {
    screen::View dialog_view;     ///< Dialog 35's buttons.
    std::vector<w2::Item*> list;  ///< The items shown, in the order shown.
    void Draw() override {
        kit::Popup(kPopupRect);
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), kit::Text(0xa6, 0), 320, 52);
        kit::Centred(kit::GetFont(2), kit::Text(0xa7, 0), 320, 382);
        const Font& font = kit::GetFont(2).Colours(7, 0);
        // the kind's line carries over when an item is of no kind it knows
        int kind_line = 0;
        for (size_t i = 0; i < list.size(); i++) {
            int y = 90 + 20 * static_cast<int>(i);
            auto known_kind = kKindLine.find(list[i]->type);
            if (known_kind != kKindLine.end()) kind_line = known_kind->second;
            kit::Right(font, list[i]->name, 304, y);
            font.Draw(w2::Format(kit::Text(0xa7, kind_line), list[i]->value), 336, y);
        }
        kit::DrawControls(dialog_view);
    }
    void Mousepressed(int x, int y, int) override {
        auto control = kit::ControlAt(dialog_view, x, y);
        if (!control) return;
        if (control->id == kDone)
            kit::Pop(this);
        else if (control->id == kHelp)
            help::Open("HELP\\HITEM.GFX");
    }
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") kit::Pop(this);
    }
};
}  // namespace

/**
 * @brief Opens View > Items: the scenario's magic items, sorted by kind and
 * then by value.
 */
void Open() {
    auto dialog = std::make_shared<Items>();
    dialog->dialog_view = kit::View(kDialog);
    dialog->view = &dialog->dialog_view;
    dialog->dialog_view.state[kDone] = w2::uidata::kNormal;
    dialog->dialog_view.state[kHelp] = w2::uidata::kNormal;
    for (auto& item : g_app.g->map->items)
        if (item.index >= kFirst && item.index <= kLast) dialog->list.push_back(&item);
    // an insertion sort, by kind then value: equal ones keep their order
    auto& sorted = dialog->list;
    for (size_t i = 1; i < sorted.size(); i++) {
        size_t position = i;
        while (position > 0 && (sorted[position]->type < sorted[position - 1]->type ||
                                (sorted[position]->type == sorted[position - 1]->type &&
                                 sorted[position]->value < sorted[position - 1]->value))) {
            std::swap(sorted[position], sorted[position - 1]);
            position--;
        }
    }
    kit::Push(dialog);
}

}  // namespace items
