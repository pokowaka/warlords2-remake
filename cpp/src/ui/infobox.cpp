/**
 * @file
 * @brief What the right button shows off the map: a box that stays up while
 * the button is held, and goes when it comes up.
 *
 * Two boards, blitted through their masks:
 * - `POPUP.PCK` (bitmap 29), `256x75`: two lines -- the first in colour 7, the
 *   second in white, centred on the box's middle less 8, at `y + 11` and
 *   `y + 35`.
 * - `POPUP3.PCK` (bitmap 40), `240x128`: an army (`ui_army_info`) or an army type
 *   (`ui_army_type_info`): the name, the army on the grey ring with a
 *   non-hero's medals round it, four numbers, and how the type moves.
 *
 * A right-button press on a control looks it up in `HELP\WARLORD2.HLP`
 * (`docs/formats/hlp.md`): when the record's sub-id is 0 its title and
 * description are the two lines; otherwise the sub-id says what the control
 * stands for and the dialog it is on answers through its `Info()`.
 */
#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "absl/strings/str_format.h"

#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/armytype.hpp"
#include "warlords/bytes.hpp"

namespace infobox {

const Board kLines{29, "POPUP.PCK", 256, 75, 1};
const Board kArmy{40, "POPUP3.PCK", 240, 128, 10};
const Board kPopuP2{36, "POPUP2.PCK", 256, 75, 1};

namespace {

using PaintFn = std::function<void(w2::UiContext&, int, int)>;

/// Where in the army bits each way of moving has its 32x10 icon `(x, y)`.
constexpr std::array<int, 2> kMoveFly = {184, 30};    ///< Flies.
constexpr std::array<int, 2> kMoveBoth = {216, 30};   ///< Moves well in woods and hills.
constexpr std::array<int, 2> kMoveWoods = {248, 30};  ///< Moves well in woods.
constexpr std::array<int, 2> kMoveHills = {152, 30};  ///< Moves well in hills.

/**
 * @brief Transient modal board shown while a mouse button is held; closes on any press or release.
 */
class Box final : public kit::Modal {
  public:
    Box(w2::UiContext& ctx, const Board& which, int board_x, int board_y, PaintFn paint)
            : kit::Modal(ctx)
            , which_(which)
            , board_x_(board_x)
            , board_y_(board_y)
            , paint_(std::move(paint)) {
        SetIsInfobox(true);
    }

    void Draw() override {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        auto image = BoardImage(ctx, which_);
        ren.SetColor(1, 1, 1);
        if (image) ren.Draw(image, gfx::NewQuad(0, 0, which_.w, which_.h), board_x_, board_y_);
        ren.SetColor(1, 1, 1);
        paint_(ctx, board_x_, board_y_);
    }

    bool Mousereleased(int, int, int) override {
        PopSelf();
        return true;
    }

    void Mousepressed(int, int, int) override { PopSelf(); }

    void Keypressed(const std::string&) override { PopSelf(); }

  private:
    Board which_;      ///< The board drawn under the contents.
    int board_x_ = 0;  ///< The board's left edge in the dialog frame.
    int board_y_ = 0;  ///< The board's top edge in the dialog frame.
    PaintFn paint_;    ///< Draws the contents at the board's top left.
};

/**
 * @brief Puts up board `which` for the screen point (`screen_x`, `screen_y`),
 * its contents drawn by `paint`.
 */
void Show(w2::UiContext& ctx, const Board& which, int screen_x, int screen_y, PaintFn paint) {
    auto [board_x, board_y] = Place(ctx, which, screen_x, screen_y);
    kit::Push(ctx, std::make_shared<Box>(ctx, which, board_x, board_y, std::move(paint)));
}

/**
 * @brief Draws the army board's name, the army on its ring and the icon for
 * how the type moves, at the board's top left (`x`, `y`).
 */
void ArmyBoard(w2::UiContext& ctx, int x, int y, const std::string& name, int type_id, int side,
               int medals) {
    const Font& font = kit::GetFont(ctx, 2);
    kit::Centred(font, name, x + 112, y + 8);
    kit::Army(ctx, type_id, side, x + 96, y + 31, 1);
    (void)medals;  // the remake keeps no medals yet
    const w2::ArmyType* army_type = ctx.ActiveGame().ArmyTypes().ById(type_id);
    const std::array<int, 2>* move_icon = nullptr;
    if (army_type && army_type->flies) {
        move_icon = &kMoveFly;
    } else if (army_type && army_type->woods_move && army_type->hills_move) {
        move_icon = &kMoveBoth;
    } else if (army_type && army_type->woods_move) {
        move_icon = &kMoveWoods;
    } else if (army_type && army_type->hills_move) {
        move_icon = &kMoveHills;
    }
    if (move_icon) {
        ctx.Renderer().SetColor(1, 1, 1);
        ctx.Renderer().Draw(ctx.Assets().Abits(),
                            gfx::NewQuad((*move_icon)[0], (*move_icon)[1], 32, 10), x + 96,
                            y + 103);
    }
}

/**
 * @brief Draws the army board's four numbers in two rows: each row's left
 * one ends at `x + 104`, its right one starts at `x + 120`.
 */
void Numbers(w2::UiContext& ctx, int x, int y, const std::string& top_left,
             const std::string& top_right, const std::string& bottom_left,
             const std::string& bottom_right) {
    const Font& font = kit::GetFont(ctx, 2);
    ctx.Renderer().SetColor(1, 1, 1);
    kit::Right(font, top_left, x + 104, y + 66);
    font.Draw(top_right, x + 120, y + 66);
    kit::Right(font, bottom_left, x + 104, y + 86);
    font.Draw(bottom_right, x + 120, y + 86);
}

/** @brief One record of `HELP\WARLORD2.HLP`: the help for one control. */
struct HelpRecord {
    int sub = 0;        ///< 0: title and text are the help; else what the control stands for.
    std::string title;  ///< The first line.
    std::string text;   ///< The second line.
};

using OptionalHelpRecord = std::optional<HelpRecord>;

/**
 * @brief Looks up the first `HELP\WARLORD2.HLP` record matching `target_control_id`.
 */
OptionalHelpRecord FindHelpRecord(const std::filesystem::path& data_dir, int target_control_id) {
    // HELP\WARLORD2.HLP (docs/formats/hlp.md): a u16 count, then 104-byte
    // records -- u16 id, u16 sub-id, title and description, 50 bytes each.
    // Only the first record with an id counts.
    auto contents = w2::ReadBytes(data_dir / "HELP/WARLORD2.HLP");
    if (!contents.ok() || contents->size() < 2) return std::nullopt;
    int count = w2::U16(*contents, 0);
    for (int i = 0; i < count; i++) {
        size_t offset = 2 + static_cast<size_t>(i) * 104;
        if (offset + 104 > contents->size()) break;
        int record_id = w2::U16(*contents, offset);
        if (record_id == target_control_id) {
            return HelpRecord{w2::U16(*contents, offset + 2), w2::Cstr(*contents, offset + 4, 50),
                              w2::Cstr(*contents, offset + 54, 50)};
        }
    }
    return std::nullopt;
}

}  // namespace

gfx::ImageP BoardImage(w2::UiContext& ctx, const Board& which) {
    return ctx.Assets().BoardImage(ctx.Renderer(), which.bitmap, which.file, which.key);
}

FramePos Place(const w2::UiContext& ctx, const Board& which, int screen_x, int screen_y) {
    const auto& layout = ctx.GetViewport().GetLayout();
    int half_width = which.w / 2;
    int half_height = which.h / 2;
    int centre_x = (screen_x + 4) / 8 * 8;
    centre_x = std::max(half_width, std::min(layout.w - half_width, centre_x));
    int centre_y = std::max(half_height, std::min(layout.h - 2 - half_height, screen_y));
    return {centre_x - half_width - layout.dialog.x, centre_y - half_height - layout.dialog.y};
}

void ShowLines(w2::UiContext& ctx, int screen_x, int screen_y, const std::string& title,
               const std::string& text) {
    Show(ctx, kLines, screen_x, screen_y, [title, text](w2::UiContext& paint_ctx, int x, int y) {
        const Font& font = kit::GetFont(paint_ctx, 2);
        kit::Centred(font.Colours(7, 0), title, x + kLines.w / 2 - 8, y + 11);
        kit::Centred(font, text, x + kLines.w / 2 - 8, y + 35);
    });
}

void Army(w2::UiContext& ctx, int screen_x, int screen_y, const w2::Army* army) {
    const w2::ArmyType* army_type = ctx.ActiveGame().ArmyTypes().ById(army->Type());
    bool hero = army->Hero();
    std::string name = hero ? army->Name() : (army_type ? army_type->name : "");
    w2::Army army_copy = *army;
    int side = ctx.Player()->Index();
    Show(ctx, kArmy, screen_x, screen_y,
         [name, army_copy, side](w2::UiContext& paint_ctx, int x, int y) {
             ArmyBoard(paint_ctx, x, y, name, army_copy.TypeIndex(), side, 0);
             Numbers(paint_ctx, x, y, absl::StrFormat("Strength: %d", army_copy.Strength()),
                     absl::StrFormat("Movement: %d", army_copy.MaxMoves()),
                     absl::StrFormat("Remain: %d", army_copy.Moves()),
                     absl::StrFormat("Upkeep: %d", army_copy.Upkeep()));
         });
}

void ArmyType(w2::UiContext& ctx, int screen_x, int screen_y, int type_id, const w2::Slot& slot,
              int side) {
    const w2::ArmyType* army_type = ctx.ActiveGame().ArmyTypes().ById(type_id);
    std::string name = army_type ? army_type->name : "";
    int shown_side = side == w2::kNone ? ctx.Player()->Index() : side;
    w2::Slot slot_copy = slot;
    Show(ctx, kArmy, screen_x, screen_y,
         [name, type_id, shown_side, slot_copy](w2::UiContext& paint_ctx, int x, int y) {
             ArmyBoard(paint_ctx, x, y, name, type_id, shown_side, 0);
             Numbers(paint_ctx, x, y, absl::StrFormat("Strength: %d", slot_copy.strength),
                     absl::StrFormat("Movement: %d", slot_copy.move),
                     absl::StrFormat("Time: %d", slot_copy.time),
                     absl::StrFormat("Cost: %d", slot_copy.cost));
         });
}

bool ExplainControl(w2::UiContext& ctx, int screen_x, int screen_y, int control_id,
                    kit::Modal* modal) {
    OptionalHelpRecord record = FindHelpRecord(ctx.Assets().DataDir(), control_id);
    if (!record.has_value()) return false;
    if (record->sub == 0) {
        ShowLines(ctx, screen_x, screen_y, record->title, record->text);
        return true;
    }
    int sub = record->sub;
    if (sub >= 33 && sub <= 36) {
        // the configurable buttons: the menu item each one runs
        const auto& screen_ui = ctx.Assets().MainScreen().ui;
        auto item = screen_ui.shortcuts.find(sub - 33);
        std::string name;
        if (item != screen_ui.shortcuts.end()) {
            auto item_name = screen_ui.shortcut_names.find(item->second);
            if (item_name != screen_ui.shortcut_names.end()) name = item_name->second;
        }
        ShowLines(ctx, screen_x, screen_y, "- User-Defined Button -", name);
        return true;
    }
    if (modal) return modal->Info(sub, screen_x, screen_y);
    return false;
}

const w2::uidata::Control* ControlAt(const screen::View& view, int x, int y,
                                     const HiddenSet* hidden) {
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
