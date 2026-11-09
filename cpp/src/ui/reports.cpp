/**
 * @file
 * @brief The Report menu: Army, City, Gold, Production and Winning.
 *
 * Dialog 7 over popup 2, (80, 60) 480x312, the five reports as tabs (218-222)
 * and Done (223); auto_ui_reports_menu draws the strategic map -- a banner on
 * every stack outside a city for Army, See All's vectors for Production --
 * the title, what it measures, a bar per side or the list of what was built
 * this turn, and the summary in the side's colours. The bars are tiled 8
 * pixels at a time from SHIELDS.PCK; the Production list shows five at a
 * time, and 206-209 scroll it.
 */
#include <set>

#include "absl/strings/str_format.h"

#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/report.hpp"

namespace reports {

namespace report = w2::report;
using w2::Format;

namespace {
const kit::Rect kPopupRect{80, 60, 480, 312};  ///< Popup 2.
const int kMapX = 80,                          ///< The strategic map's left edge.
        kMapY = 60;                            ///< The strategic map's top edge.
const int kDialog = 7;                         ///< The dialog with the tabs and buttons.
const int kTabFirst = 218,                     ///< The first report's tab; the other four follow.
        kDone = 223;                           ///< The Done button.
const int kUp = 206,                           ///< Scrolls the Production list up a row.
        kDown = 207,                           ///< Scrolls it down a row.
        kPageUp = 208,                         ///< Scrolls it up a page.
        kPageDown = 209;                       ///< Scrolls it down a page.
const int kTitleGroup = 0x49,                  ///< STRING.DAT group: each report's title.
        kWhatGroup = 0x4a;                     ///< Group: what each report measures.
/// Each report's summary group.
const int kSummaryGroup[5] = {0x4b, 0x4c, 0x4d, 0x4e, 0x4f};
const int kRows = 5;  ///< The Production list's rows on a page.

/** @brief The summary line under report `which_report`, from its figures. */
std::string Summary(int which_report, const report::Figures& figures) {
    int result = figures.result;
    if (which_report == report::kArmy || which_report == report::kCity)
        return result == 1 ? kit::Text(kSummaryGroup[which_report], 0)
                           : Format(kit::Text(kSummaryGroup[which_report], 1), result);
    if (which_report == report::kGold)
        return Format(kit::Text(kSummaryGroup[which_report], 0), result);
    if (which_report == report::kProduction)
        return Format(kit::Text(kSummaryGroup[which_report], result == 1 ? 1 : 0), result);
    return kit::Text(kSummaryGroup[which_report], result);
}

/** @brief The Army report's map marks: a banner on every seen stack outside a city. */
void DrawArmyBanners() {
    std::set<int> marked_tiles;
    gfx::SetScissor(kMapX, kMapY, 224, 312);
    gfx::SetColor(1, 1, 1);
    int viewer = w2::game::ViewerSide(*g_app.g, g_app.player);
    for (w2::Army* army : g_app.g->armies) {
        if (!army->transit && army->x != w2::kNone) {
            int tile_key = army->y * 1000 + army->x;
            if (!marked_tiles.count(tile_key) &&
                w2::game::Seen(*g_app.g, viewer, army->x, army->y) &&
                !w2::game::CityAt(*g_app.g, army->x, army->y)) {
                marked_tiles.insert(tile_key);
                int side = army->owner == w2::kNone ? 8 : army->owner;
                gfx::Draw(g_app.atrans_shields, gfx::NewQuad(side * 16, 164, 16, 10),
                          kMapX + std::max(0, army->x * 2 - 1),
                          kMapY + std::max(0, army->y * 2 - 1));
            }
        }
    }
    gfx::SetScissor();
}

/**
 * @brief A bar per side still in the game, in its shield's colours, and the
 * axis under them marked 0, half and the most.
 */
void DrawBars(const report::Figures& figures) {
    for (int i = 0; i < 8; i++) {
        if (!figures.out[i]) {
            int y = 200 + 14 * i;
            int bar_width = 1;
            if (figures.max > 0) bar_width = std::max(0, figures.value[i]) * 240 / figures.max;
            bar_width = std::max(1, bar_width);
            gfx::SetColor(1, 1, 1);
            int shield_x = 320 + 8 * (i % 4), shield_y = 40 + 8 * (i / 4);
            for (int x = 0; x < bar_width; x += 8) {
                int piece = std::min(8, bar_width - x);
                gfx::Draw(g_app.shields_img, gfx::NewQuad(shield_x, shield_y, piece, 8), 312 + x,
                          y);
            }
            kit::Bevel(311, y - 1, bar_width + 2, 10, 0, 1);
        }
    }
    // the axis and its ticks, drawn twice: in colour 1 in place, then in
    // colour 0 one pixel up and left (`offset`)
    auto axis = [](int colour, int offset) {
        kit::SetPal(colour);
        gfx::Rectangle(gfx::kFill, 312 - offset, 196 - offset, 240, 1);
        const int tick_x[5] = {312, 372, 432, 492, 552};
        for (int tick = 0; tick < 5; tick++) {
            bool long_tick = tick % 2 == 0;
            gfx::Rectangle(gfx::kFill, tick_x[tick] - offset, (long_tick ? 190 : 192) - offset, 1,
                           long_tick ? 6 : 4);
        }
    };
    axis(1, 0);
    axis(0, 1);
    const Font& font = kit::GetFont(2);
    gfx::SetColor(1, 1, 1);
    kit::Right(font, absl::StrFormat("%d", figures.max), 556, 172);
    kit::Centred(font, absl::StrFormat("%d", figures.max / 2), 432, 172);
    kit::Centred(font, "0", 308, 172);
}

/** @brief The Report dialog: one report at a time, picked by its tab. */
struct Reports : kit::Modal {
    screen::View dialog_view;  ///< Dialog 7's tabs and buttons.
    int shown_report = 0,      ///< The report shown: report::kArmy and on.
            top_row = 0;       ///< The Production list's first row shown.
    /** @brief Sets the tabs' and scroll buttons' states for the report shown. */
    void Refresh() {
        auto& states = dialog_view.state;
        for (int i = 0; i <= 4; i++)
            states[kTabFirst + i] = i == shown_report ? w2::uidata::kActive : w2::uidata::kNormal;
        states[kDone] = w2::uidata::kNormal;
        hidden.clear();
        if (shown_report == report::kProduction) {
            int count = static_cast<int>(
                    g_app.g->TurnNews(w2::SideId(g_app.player->index)).produced.size());
            states[kUp] = top_row > 0 ? w2::uidata::kNormal : w2::uidata::kDisabled;
            states[kPageUp] = states[kUp];
            states[kDown] = top_row + kRows < count ? w2::uidata::kNormal : w2::uidata::kDisabled;
            states[kPageDown] = states[kDown];
        } else {
            for (int control_id : {kUp, kDown, kPageUp, kPageDown}) hidden.insert(control_id);
        }
    }
    /** @brief Scrolls the Production list by `rows`, kept within the list. */
    void Scroll(int rows) {
        int count = static_cast<int>(
                g_app.g->TurnNews(w2::SideId(g_app.player->index)).produced.size());
        top_row = std::max(0, std::min(std::max(0, count - kRows), top_row + rows));
        Refresh();
    }
    /** @brief The Production list: a page of what was built or sent this turn. */
    void DrawProduction() {
        const auto& list = g_app.g->TurnNews(w2::SideId(g_app.player->index)).produced;
        const Font& font = kit::GetFont(2);
        for (int i = 0; i < kRows; i++) {
            if (top_row + i >= static_cast<int>(list.size())) break;
            const w2::Produced& entry = list[top_row + i];
            int y = 176 + 30 * i;
            gfx::SetColor(1, 1, 1);
            font.Draw(absl::StrFormat("%d", top_row + i + 1), 312, y);
            kit::Army(entry.type, g_app.player->index, 328, 170 + 30 * i, 1);
            w2::City* city = g_app.g->map->City(entry.city);
            std::string name = entry.standard ? "Standard" : (city ? city->name : "");
            std::string text;
            switch (entry.kind) {
            case w2::ProducedKind::kBuilt:
                text = name;
                break;
            case w2::ProducedKind::kSent:
                text = name + " ...";
                break;
            case w2::ProducedKind::kArrived:
                text = "... " + name;
                break;
            }
            gfx::SetColor(1, 1, 1);
            font.Draw(text, 368, y);
        }
    }
    void Draw() override {
        auto figures = report::ComputeFigures(*g_app.g, *g_app.player, shown_report);
        kit::Popup(kPopupRect);
        if (shown_report == report::kProduction) {
            front::DrawVectorMap(kMapX, kMapY, nullptr, w2::kNone, true);
        } else {
            front::DrawStrategicMap(kMapX, kMapY);
            if (shown_report == report::kArmy) DrawArmyBanners();
        }
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), kit::Text(kTitleGroup, shown_report), 432, 62);
        kit::Centred(kit::GetFont(2), kit::Text(kWhatGroup, shown_report), 432, 149);
        if (shown_report == report::kProduction)
            DrawProduction();
        else
            DrawBars(figures);
        kit::Centred(kit::GetFont(2).Colours(g_app.player->colour, g_app.player->edge),
                     Summary(shown_report, figures), 432, 322);
        kit::DrawControls(dialog_view, hidden);
    }
    void Mousepressed(int x, int y, int) override {
        auto control = kit::ControlAt(dialog_view, x, y, hidden);
        if (!control) return;
        if (control->id == kDone)
            kit::Pop(this);
        else if (control->id >= kTabFirst && control->id < kTabFirst + 5) {
            shown_report = control->id - kTabFirst;
            top_row = 0;
            Refresh();
        } else if (control->id == kUp)
            Scroll(-1);
        else if (control->id == kDown)
            Scroll(1);
        else if (control->id == kPageUp)
            Scroll(-kRows);
        else if (control->id == kPageDown)
            Scroll(kRows);
    }
    void Keypressed(const std::string& key) override {
        if (key == "escape" || key == "return" || key == "kpenter") kit::Pop(this);
    }
};
}  // namespace

/**
 * @brief Opens the Report dialog.
 * @param which The report shown first: report::kArmy, kCity, kGold,
 *     kProduction or the winning report.
 */
void Open(int which) {
    auto dialog = std::make_shared<Reports>();
    dialog->dialog_view = kit::View(kDialog);
    dialog->view = &dialog->dialog_view;
    dialog->shown_report = which;
    dialog->Refresh();
    kit::Push(dialog);
}

}  // namespace reports
