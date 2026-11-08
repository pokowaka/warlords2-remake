/**
 * @file
 * @brief What the right button shows off the map: a box that stays up while
 * the button is held, and goes when it comes up.
 *
 * Two boards, blitted through their masks:
 * - POPUP.PCK (bitmap 29), 256x75: two lines -- the first in colour 7, the
 *   second in white, centred on the box's middle less 8, at y + 11 and
 *   y + 35.
 * - POPUP3.PCK (bitmap 40), 240x128: an army (ui_army_info) or an army type
 *   (ui_army_type_info): the name, the army on the grey ring with a
 *   non-hero's medals round it, four numbers, and how the type moves.
 *
 * A right-button press on a control looks it up in HELP\WARLORD2.HLP
 * (docs/formats/hlp.md): when the record's sub-id is 0 its title and
 * description are the two lines; otherwise the sub-id says what the control
 * stands for and the dialog it is on answers through its Info().
 */
#include <algorithm>
#include <map>

#include "front/pckimage.hpp"
#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/armytype.hpp"
#include "warlords/bytes.hpp"

namespace infobox {

const Board kLines{29, "POPUP.PCK", 256, 75, 1};
const Board kArmy{40, "POPUP3.PCK", 240, 128, 10};
const Board kPopuP2{36, "POPUP2.PCK", 256, 75, 1};

namespace {
std::map<int, gfx::ImageP> boards;  ///< The boards loaded so far, by bitmap number.

/// Where in the army bits each way of moving has its 32x10 icon (x, y).
const int kMoveFly[2] = {184, 30},  ///< Flies.
        kMoveBoth[2] = {216, 30},   ///< Moves well in woods and hills.
        kMoveWoods[2] = {248, 30},  ///< Moves well in woods.
        kMoveHills[2] = {152, 30};  ///< Moves well in hills.

/** @brief A board up while the right button is held; any press or release closes it. */
struct Box : kit::Modal {
    Board which;                          ///< The board drawn under the contents.
    int board_x = 0, board_y = 0;         ///< The board's top left, in the dialogs' frame.
    std::function<void(int, int)> paint;  ///< Draws the contents at the board's top left.
    void Draw() override {
        auto image = BoardImage(which);
        gfx::SetColor(1, 1, 1);
        if (image) gfx::Draw(image, gfx::NewQuad(0, 0, which.w, which.h), board_x, board_y);
        gfx::SetColor(1, 1, 1);
        paint(board_x, board_y);
    }
    bool Mousereleased(int, int, int) override {
        kit::Pop(this);
        return true;
    }
    void Mousepressed(int, int, int) override { kit::Pop(this); }
    void Keypressed(const std::string&) override { kit::Pop(this); }
};

/**
 * @brief Puts up board `which` for the screen point (`screen_x`,
 * `screen_y`), its contents drawn by `paint`.
 */
void Show(const Board& which, int screen_x, int screen_y, std::function<void(int, int)> paint) {
    auto box = std::make_shared<Box>();
    box->which = which;
    auto [board_x, board_y] = Place(which, screen_x, screen_y);
    box->board_x = board_x;
    box->board_y = board_y;
    box->paint = paint;
    box->infobox = true;
    kit::Push(box);
}

/**
 * @brief Draws the army board's name, the army on its ring and the icon for
 * how the type moves, at the board's top left (`x`, `y`).
 */
void ArmyBoard(int x, int y, const std::string& name, int type_id, int side, int medals) {
    const Font& font = kit::GetFont(2);
    kit::Centred(font, name, x + 112, y + 8);
    kit::Army(type_id, side, x + 96, y + 31, 1);
    (void)medals;  // the remake keeps no medals yet
    const w2::ArmyType* army_type = g_app.g->types.ById(type_id);
    const int* move_icon = nullptr;
    if (army_type && army_type->flies)
        move_icon = kMoveFly;
    else if (army_type && army_type->woods_move && army_type->hills_move)
        move_icon = kMoveBoth;
    else if (army_type && army_type->woods_move)
        move_icon = kMoveWoods;
    else if (army_type && army_type->hills_move)
        move_icon = kMoveHills;
    if (move_icon) {
        gfx::SetColor(1, 1, 1);
        gfx::Draw(g_app.abits, gfx::NewQuad(move_icon[0], move_icon[1], 32, 10), x + 96, y + 103);
    }
}

/**
 * @brief Draws the army board's four numbers in two rows: each row's left
 * one ends at x + 104, its right one starts at x + 120.
 */
void Numbers(int x, int y, const std::string& top_left, const std::string& top_right,
             const std::string& bottom_left, const std::string& bottom_right) {
    const Font& font = kit::GetFont(2);
    gfx::SetColor(1, 1, 1);
    kit::Right(font, top_left, x + 104, y + 66);
    font.Draw(top_right, x + 120, y + 66);
    kit::Right(font, bottom_left, x + 104, y + 86);
    font.Draw(bottom_right, x + 120, y + 86);
}

/** @brief One record of HELP\WARLORD2.HLP: the help for one control. */
struct HelpRecord {
    int sub = 0;        ///< 0: title and text are the help; else what the control stands for.
    std::string title,  ///< The first line.
            text;       ///< The second line.
};

/**
 * @brief The records of HELP\WARLORD2.HLP by control id, read on first use;
 * empty when the file can't be read.
 */
std::map<int, HelpRecord>& HelpFile() {
    static std::map<int, HelpRecord> records;
    static bool loaded = false;
    if (loaded) return records;
    loaded = true;
    // HELP\WARLORD2.HLP (docs/formats/hlp.md): a u16 count, then 104-byte
    // records -- u16 id, u16 sub-id, title and description, 50 bytes each.
    // Only the first record with an id counts.
    if (auto contents = w2::ReadFile(g_app.data_dir + "/HELP/WARLORD2.HLP")) {
        int count = w2::U16(*contents, 0);
        for (int i = 0; i < count; i++) {
            size_t offset = 2 + static_cast<size_t>(i) * 104;
            if (offset + 104 > contents->size()) break;
            int control_id = w2::U16(*contents, offset);
            if (!records.count(control_id))
                records[control_id] = HelpRecord{w2::U16(*contents, offset + 2),
                                                 w2::Cstr(*contents, offset + 4, 50),
                                                 w2::Cstr(*contents, offset + 54, 50)};
        }
    }
    return records;
}
}  // namespace

/**
 * @brief A board's picture, loaded on first use and keyed on its mask
 * colour; null when it can't be loaded.
 */
gfx::ImageP BoardImage(const Board& which) {
    auto cached = boards.find(which.bitmap);
    if (cached != boards.end()) return cached->second;
    gfx::ImageP image;
    try {
        image = pckimage::Load(g_app.data_dir + "/PICS/" + which.file, g_app.palette, which.key);
    } catch (...) {
    }
    return boards[which.bitmap] = image;
}

/**
 * @brief Where board `which` goes for a point on the screen: centred on it
 * (x snapped to 8 pixels), kept on the screen.
 * @return The board's top left, in the dialogs' frame.
 */
std::pair<int, int> Place(const Board& which, int screen_x, int screen_y) {
    const auto& layout = g_app.layout;
    int half_width = which.w / 2, half_height = which.h / 2;
    int centre_x = (screen_x + 4) / 8 * 8;
    centre_x = std::max(half_width, std::min(layout.w - half_width, centre_x));
    int centre_y = std::max(half_height, std::min(layout.h - 2 - half_height, screen_y));
    return {centre_x - half_width - layout.dialog.x, centre_y - half_height - layout.dialog.y};
}

/**
 * @brief Puts up the two-line board for a point on the screen: `title` in
 * colour 7 over `text` in white.
 */
void ShowLines(int screen_x, int screen_y, const std::string& title, const std::string& text) {
    Show(kLines, screen_x, screen_y, [title, text](int x, int y) {
        const Font& font = kit::GetFont(2);
        kit::Centred(font.Colours(7, 0), title, x + kLines.w / 2 - 8, y + 11);
        kit::Centred(font, text, x + kLines.w / 2 - 8, y + 35);
    });
}

/**
 * @brief Puts up the army board for `army`: its name (a hero's own, else
 * its type's), strength, movement, moves left and upkeep, in the player's
 * colours.
 */
void Army(int screen_x, int screen_y, const w2::Army* army) {
    const w2::ArmyType* army_type = g_app.g->types.ById(army->type);
    bool hero = army->Hero();
    std::string name = hero ? army->name : (army_type ? army_type->name : "");
    w2::Army army_copy = *army;
    int side = g_app.player->index;
    Show(kArmy, screen_x, screen_y, [name, army_copy, side](int x, int y) {
        ArmyBoard(x, y, name, army_copy.type, side, 0);
        Numbers(x, y, w2::Fmt("Strength: %d", army_copy.strength),
                w2::Fmt("Movement: %d", army_copy.max_moves),
                w2::Fmt("Remain: %d", army_copy.moves), w2::Fmt("Upkeep: %d", army_copy.upkeep));
    });
}

/**
 * @brief Puts up the army board for an army type a city can build: its
 * strength, movement, time and cost from production `slot`.
 * @param side The side whose colours to show it in; kNone for the player's.
 */
void ArmyType(int screen_x, int screen_y, int type_id, const w2::Slot& slot, int side) {
    const w2::ArmyType* army_type = g_app.g->types.ById(type_id);
    std::string name = army_type ? army_type->name : "";
    int shown_side = side == w2::kNone ? g_app.player->index : side;
    w2::Slot slot_copy = slot;
    Show(kArmy, screen_x, screen_y, [name, type_id, shown_side, slot_copy](int x, int y) {
        ArmyBoard(x, y, name, type_id, shown_side, 0);
        Numbers(x, y, w2::Fmt("Strength: %d", slot_copy.strength),
                w2::Fmt("Movement: %d", slot_copy.move), w2::Fmt("Time: %d", slot_copy.time),
                w2::Fmt("Cost: %d", slot_copy.cost));
    });
}

/**
 * @brief Shows the help for control `control_id` at a point on the screen.
 *
 * A plain help record shows its two lines; the user-defined buttons show
 * the menu item they run; any other control that stands for something is
 * explained by `dialog` through its Info().
 * @return True when a box went up.
 */
bool ExplainControl(int screen_x, int screen_y, int control_id, kit::Modal* dialog) {
    auto& records = HelpFile();
    auto found = records.find(control_id);
    if (found == records.end()) return false;
    const HelpRecord& record = found->second;
    if (record.sub == 0) {
        ShowLines(screen_x, screen_y, record.title, record.text);
        return true;
    }
    int sub = record.sub;
    if (sub >= 33 && sub <= 36) {
        // the configurable buttons: the menu item each one runs
        const auto& screen_ui = g_app.screen->ui;
        auto item = screen_ui.shortcuts.find(sub - 33);
        std::string name;
        if (item != screen_ui.shortcuts.end()) {
            auto item_name = screen_ui.shortcut_names.find(item->second);
            if (item_name != screen_ui.shortcut_names.end()) name = item_name->second;
        }
        ShowLines(screen_x, screen_y, "- User-Defined Button -", name);
        return true;
    }
    if (dialog) return dialog->Info(sub, screen_x, screen_y);
    return false;
}

/**
 * @brief The control on `view` under (`x`, `y`), disabled ones included and
 * `hidden` ones left out; the last one listed wins where they overlap.
 * @return The control, or null when there is none.
 */
const w2::uidata::Control* ControlAt(const screen::View& view, int x, int y,
                                     const std::set<int>* hidden) {
    const w2::uidata::Control* found = nullptr;
    for (const auto& control : view.dialog.controls) {
        if (control.w > 0 && control.h > 0 && !(hidden && hidden->count(control.id)) &&
            x >= control.x && x < control.x + control.w && y >= control.y &&
            y < control.y + control.h) {
            found = &control;
        }
    }
    return found;
}

}  // namespace infobox
