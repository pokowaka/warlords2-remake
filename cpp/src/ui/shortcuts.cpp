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
#include "front/prefs.hpp"
#include "ui/dialogs.hpp"
#include "util/json.hpp"
#include "util/util.hpp"
#include "warlords/bytes.hpp"

namespace shortcuts {

namespace {
const kit::Rect kPopupRect{80, 60, 480, 320};  ///< Popup 0, the whole dialog.
const int kDialog = 14;                        ///< BUTTON.DAT dialog number.
const int kOk = 294;                           ///< The OK control.
const int kItem = 295;                         ///< First of the 21 item controls.
const int kSlot = 316;                         ///< First of the four slot controls.
/// The empty slot's button: its cell in the shortcut bitmap.
const int kBlank[2] = {256, 84};

/** @brief A menu item that can go on a shortcut button, as UDB.DAT has it. */
struct Entry {
    int id = 0;        ///< The menu item's id.
    std::string name;  ///< Its name, shown beside its button.
    int x = 0;         ///< Left of its button's cell in the shortcut bitmap.
    int y = 0;         ///< Top of the cell.
    int width = 0;     ///< Width of the cell: the plain face, then the lit one.
    int height = 0;    ///< Height of the cell.
};

/**
 * @brief The items of UDB.DAT: a 16-bit count, then 68-byte records.
 *
 * A missing file gives none; a short one stops at the last whole record.
 */
std::vector<Entry> ReadItems() {
    std::vector<Entry> entries;
    auto data = w2::ReadFile(g_app.data_dir + "/UDB/UDB.DAT");
    if (!data) return entries;
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
struct Shortcuts : kit::Modal {
    screen::View dialog_view;  ///< Dialog 14's controls.
    std::vector<Entry> items;  ///< The items that can go on a button.
    int slot = 0;              ///< The slot being set, 0-3.
    /** @brief The index in `items` of the item with id `menu_id`, or kNone. */
    int ItemIndex(int menu_id) const {
        for (size_t i = 0; i < items.size(); i++)
            if (items[i].id == menu_id) return static_cast<int>(i);
        return w2::kNone;
    }
    /** @brief The id of the item on slot `shortcut_slot`, or kNone. */
    int Shortcut(int shortcut_slot) const {
        auto found = g_app.screen->ui.shortcuts.find(shortcut_slot);
        return found == g_app.screen->ui.shortcuts.end() ? w2::kNone : found->second;
    }
    /** @brief Enables OK, the items and the slots. */
    void Refresh() {
        auto& states = dialog_view.state;
        states[kOk] = w2::uidata::kNormal;
        for (int i = 0; i <= 20; i++) states[kItem + i] = w2::uidata::kNormal;
        for (int i = 0; i <= 3; i++) states[kSlot + i] = w2::uidata::kNormal;
    }
    /** @brief Draws the shortcut bitmap's 32x29 cell at (cell_x, cell_y) at (x, y). */
    void Button(int x, int y, int cell_x, int cell_y) {
        auto art = g_app.screen->ArtFor(w2::uidata::kShortcutBitmap);
        if (!art) return;
        gfx::SetColor(1, 1, 1);
        gfx::Draw(art->image, gfx::NewQuad(cell_x, cell_y, 32, 29), x, y);
    }
    /** @brief Draws the items, the one on the slot being set lit, and the slots. */
    void Draw() override {
        kit::Popup(kPopupRect);
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), "Menu Shortcuts", 320, 63);
        const Font& font = kit::GetFont(2);
        int current = ItemIndex(Shortcut(slot));
        for (size_t i = 0; i < items.size(); i++) {
            const Entry& entry = items[i];
            int x = 96 + 152 * (static_cast<int>(i) % 3);
            int y = 100 + 30 * (static_cast<int>(i) / 3);
            Button(x, y, entry.x + (static_cast<int>(i) == current ? entry.width / 2 : 0), entry.y);
            gfx::SetColor(1, 1, 1);
            font.Draw(entry.name, x + 40, y + 5);
        }
        kit::Centred(font, "Choose 4 buttons for shortcuts", 320, 320);
        for (int slot_index = 0; slot_index <= 3; slot_index++) {
            int x = 96 + 40 * slot_index;
            int item_index = ItemIndex(Shortcut(slot_index));
            if (item_index != w2::kNone)
                Button(x, 342, items[item_index].x, items[item_index].y);
            else
                Button(x, 342, kBlank[0], kBlank[1]);
            if (slot_index == slot) {
                kit::SetPal(9);
                kit::Outline(x, 342, 32, 29);
            }
        }
        // the items and the slots are drawn above, not as controls
        std::set<int> drawn_by_hand;
        for (int i = 0; i <= 20; i++) drawn_by_hand.insert(kItem + i);
        for (int i = 0; i <= 3; i++) drawn_by_hand.insert(kSlot + i);
        kit::DrawControls(dialog_view, drawn_by_hand);
    }
    /** @brief OK saves the shortcuts; a slot is picked; an item goes on the slot. */
    void Mousepressed(int x, int y, int) override {
        auto control = kit::ControlAt(dialog_view, x, y);
        if (!control) return;
        auto& screen_ui = g_app.screen->ui;
        if (control->id == kOk) {
            w2::Json json = w2::Json::Array();
            for (int slot_index = 0; slot_index < 4; slot_index++) json.Push(Shortcut(slot_index));
            prefs::Set("shortcuts", json.Dump());
            kit::Pop(this);
            return;
        }
        if (control->id >= kSlot && control->id < kSlot + 4) {
            slot = control->id - kSlot;
        } else if (control->id >= kItem && control->id < kItem + 21) {
            int item_index = control->id - kItem;
            if (item_index < static_cast<int>(items.size())) {
                screen_ui.shortcuts[slot] = items[item_index].id;
                if (!screen_ui.shortcut_names.count(items[item_index].id))
                    screen_ui.shortcut_names[items[item_index].id] = items[item_index].name;
            }
        }
        Refresh();
    }
    /** @brief Enter or Escape closes without saving. */
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") kit::Pop(this);
    }
};
}  // namespace

/** @brief Opens the shortcuts dialog. */
void Open() {
    auto dialog = std::make_shared<Shortcuts>();
    dialog->items = ReadItems();
    dialog->dialog_view = kit::View(kDialog);
    dialog->view = &dialog->dialog_view;
    dialog->Refresh();
    kit::Push(dialog);
}

}  // namespace shortcuts
