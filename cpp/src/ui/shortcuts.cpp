/**
 * @file
 * @brief Game > Shortcuts: which menu items the four configurable buttons
 * carry.
 *
 * Popup 0, (80, 60) 480x320, dialog 14: the 21 items of UDB.DAT three to a
 * row, each its button and its name, lit when it is on the slot being set;
 * and the four slots, the one being set outlined in colour 9. 316-319 pick a
 * slot, 295-315 put an item on it, OK (294) keeps them, in the remake's own
 * settings.
 */
#include <memory>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/bytes.hpp"

namespace shortcuts {

namespace {

constexpr kit::Rect kPopupRect{80, 60, 480, 320};  ///< Popup 0, the whole dialog.
constexpr int kDialog = 14;                        ///< BUTTON.DAT dialog number.
constexpr int kOk = 294;                           ///< The OK control.
constexpr int kItem = 295;                         ///< First of the 21 item controls.
constexpr int kSlot = 316;                         ///< First of the four slot controls.
/// The empty slot's button: its cell in the shortcut bitmap.
constexpr int kBlank[2] = {256, 84};

/** @brief A menu item that can go on a shortcut button, as UDB.DAT has it. */
struct Entry {
    int id = 0;        ///< The menu item's id.
    std::string name;  ///< Its name, shown beside its button.
    int x = 0;         ///< Left of its button's cell in the shortcut bitmap.
    int y = 0;         ///< Top of the cell.
    int width = 0;     ///< Width of the cell: the plain face, then the lit one.
    int height = 0;    ///< Height of the cell.
};

using EntryList = std::vector<Entry>;

/**
 * @brief The items of UDB.DAT: a 16-bit count, then 68-byte records.
 *
 * A missing file gives none; a short one stops at the last whole record.
 */
EntryList ReadItems(const w2::UiContext& ctx) {
    EntryList entries;
    auto data = w2::ReadBytes(ctx.Assets().DataDir() / "UDB/UDB.DAT");
    if (!data.ok()) return entries;
    int count = w2::U16(*data, 0);
    for (int i = 0; i < count; i++) {
        size_t offset = 2 + 68 * static_cast<size_t>(i);
        if (offset + 68 > data->size()) break;
        entries.push_back(Entry{w2::U16(*data, offset), w2::Cstr(*data, offset + 2, 50),
                                w2::U16(*data, offset + 52), w2::U16(*data, offset + 54),
                                w2::U16(*data, offset + 56), w2::U16(*data, offset + 58)});
    }
    return entries;
}

/** @brief The shortcuts dialog: the items, the four slots and OK. */
class ShortcutsModal final : public kit::Modal {
  public:
    explicit ShortcutsModal(w2::UiContext& ctx)
            : kit::Modal(ctx), dialog_view_(kit::View(ctx, kDialog)), items_(ReadItems(ctx)) {
        SetDialogView(&dialog_view_);
        Refresh();
    }

    /** @brief Draws the items, the one on the slot being set lit, and the slots. */
    void Draw() override {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        kit::Popup(ctx, kPopupRect);
        ren.SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(ctx, 1), "Menu Shortcuts", 320, 63);
        const Font& font = kit::GetFont(ctx, 2);
        int current = ItemIndex(Shortcut(slot_));
        for (size_t i = 0; i < items_.size(); i++) {
            const Entry& entry = items_[i];
            int x = 96 + 152 * (static_cast<int>(i) % 3);
            int y = 100 + 30 * (static_cast<int>(i) / 3);
            DrawButton(x, y, entry.x + (static_cast<int>(i) == current ? entry.width / 2 : 0),
                       entry.y);
            ren.SetColor(1, 1, 1);
            font.Draw(entry.name, x + 40, y + 5);
        }
        kit::Centred(font, "Choose 4 buttons for shortcuts", 320, 320);
        for (int slot_index = 0; slot_index <= 3; slot_index++) {
            int x = 96 + 40 * slot_index;
            int item_index = ItemIndex(Shortcut(slot_index));
            if (item_index != w2::kNone) {
                DrawButton(x, 342, items_[item_index].x, items_[item_index].y);
            } else {
                DrawButton(x, 342, kBlank[0], kBlank[1]);
            }
            if (slot_index == slot_) {
                kit::SetPal(ctx, 9);
                kit::Outline(ctx, x, 342, 32, 29);
            }
        }
        // the items and the slots are drawn above, not as controls
        kit::ControlIdSet drawn_by_hand;
        for (int i = 0; i <= 20; i++) drawn_by_hand.insert(kItem + i);
        for (int i = 0; i <= 3; i++) drawn_by_hand.insert(kSlot + i);
        kit::DrawControls(ctx, dialog_view_, drawn_by_hand);
    }

    /** @brief OK saves the shortcuts; a slot is picked; an item goes on the slot. */
    void Mousepressed(int x, int y, int) override {
        w2::UiContext& ctx = Ctx();
        const auto* control = kit::ControlAt(dialog_view_, x, y);
        if (!control) return;
        auto& screen_ui = ctx.Assets().MainScreen().ui;
        if (control->id == kOk) {
            nlohmann::ordered_json json = nlohmann::ordered_json::array();
            for (int slot_index = 0; slot_index < 4; slot_index++) {
                json.push_back(Shortcut(slot_index));
            }
            ctx.SavePref("shortcuts", json.dump());
            PopSelf();
            return;
        }
        if (control->id >= kSlot && control->id < kSlot + 4) {
            slot_ = control->id - kSlot;
        } else if (control->id >= kItem && control->id < kItem + 21) {
            int item_index = control->id - kItem;
            if (item_index < static_cast<int>(items_.size())) {
                screen_ui.shortcuts[slot_] = items_[item_index].id;
                if (!screen_ui.shortcut_names.count(items_[item_index].id)) {
                    screen_ui.shortcut_names[items_[item_index].id] = items_[item_index].name;
                }
            }
        }
        Refresh();
    }

    /** @brief Enter or Escape closes without saving. */
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") PopSelf();
    }

  private:
    /** @brief The index in `items_` of the item with id `menu_id`, or kNone. */
    int ItemIndex(int menu_id) const {
        for (size_t i = 0; i < items_.size(); i++) {
            if (items_[i].id == menu_id) return static_cast<int>(i);
        }
        return w2::kNone;
    }

    /** @brief The id of the item on slot `shortcut_slot`, or kNone. */
    int Shortcut(int shortcut_slot) const {
        const auto& shortcuts_map = Ctx().Assets().MainScreen().ui.shortcuts;
        auto found = shortcuts_map.find(shortcut_slot);
        return found == shortcuts_map.end() ? w2::kNone : found->second;
    }

    /** @brief Enables OK, the items and the slots. */
    void Refresh() {
        auto& states = dialog_view_.state;
        states[kOk] = w2::uidata::kNormal;
        for (int i = 0; i <= 20; i++) states[kItem + i] = w2::uidata::kNormal;
        for (int i = 0; i <= 3; i++) states[kSlot + i] = w2::uidata::kNormal;
    }

    /** @brief Draws the shortcut bitmap's 32x29 cell at (cell_x, cell_y) at (x, y). */
    void DrawButton(int x, int y, int cell_x, int cell_y) {
        w2::UiContext& ctx = Ctx();
        const screen::Art* art = ctx.ControlArt(w2::uidata::kShortcutBitmap);
        if (!art) return;
        ctx.Renderer().SetColor(1, 1, 1);
        ctx.Renderer().Draw(art->image, gfx::NewQuad(cell_x, cell_y, 32, 29), x, y);
    }

    screen::View dialog_view_;  ///< Dialog 14's controls.
    EntryList items_;           ///< The items that can go on a button.
    int slot_ = 0;              ///< The slot being set, 0-3.
};

}  // namespace

/** @brief Opens the shortcuts dialog. */
void Open(w2::UiContext& ctx) {
    kit::Push(ctx, std::make_shared<ShortcutsModal>(ctx));
}

}  // namespace shortcuts
