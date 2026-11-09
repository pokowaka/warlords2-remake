/**
 * @file
 * @brief The Report menu: Army, City, Gold, Production and Winning.
 *
 * Dialog 7 over popup 2, `(80, 60)` `480x312`, the five reports as tabs (`218-222`)
 * and Done (`223`); `auto_ui_reports_menu` draws the strategic map -- a banner on
 * every stack outside a city for Army, See All's vectors for Production --
 * the title, what it measures, a bar per side or the list of what was built
 * this turn, and the summary in the side's colours. The bars are tiled 8
 * pixels at a time from `SHIELDS.PCK`; the Production list shows five at a
 * time, and `206-209` scroll it.
 */
#include <algorithm>
#include <array>
#include <memory>
#include <set>
#include <string>

#include "absl/strings/str_format.h"

#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/report.hpp"

namespace reports {

namespace report = w2::report;
using w2::Format;

namespace {

constexpr kit::Rect kPopupRect{80, 60, 480, 312};  ///< Popup 2.
constexpr int kMapX = 80;                          ///< The strategic map's left edge.
constexpr int kMapY = 60;                          ///< The strategic map's top edge.
constexpr int kDialog = 7;                         ///< The dialog with the tabs and buttons.
constexpr int kTabFirst = 218;                     ///< The first report's tab; four follow.
constexpr int kDone = 223;                         ///< The Done button.
constexpr int kUp = 206;                           ///< Scrolls the Production list up a row.
constexpr int kDown = 207;                         ///< Scrolls it down a row.
constexpr int kPageUp = 208;                       ///< Scrolls it up a page.
constexpr int kPageDown = 209;                     ///< Scrolls it down a page.
constexpr int kTitleGroup = 0x49;                  ///< STRING.DAT group: each report's title.
constexpr int kWhatGroup = 0x4a;                   ///< Group: what each report measures.
/// Each report's summary group.
constexpr std::array<int, 5> kSummaryGroup = {0x4b, 0x4c, 0x4d, 0x4e, 0x4f};
constexpr std::array<int, 5> kTickX = {312, 372, 432, 492, 552};
constexpr int kRows = 5;  ///< The Production list's rows on a page.

/** @brief The summary line under report `which_report`, from its figures. */
std::string Summary(const w2::UiContext& ctx, int which_report, const report::Figures& figures) {
    int result = figures.result;
    if (which_report == report::kArmy || which_report == report::kCity) {
        return result == 1 ? kit::Text(ctx, kSummaryGroup[which_report], 0)
                           : Format(kit::Text(ctx, kSummaryGroup[which_report], 1), result);
    }
    if (which_report == report::kGold) {
        return Format(kit::Text(ctx, kSummaryGroup[which_report], 0), result);
    }
    if (which_report == report::kProduction) {
        return Format(kit::Text(ctx, kSummaryGroup[which_report], result == 1 ? 1 : 0), result);
    }
    return kit::Text(ctx, kSummaryGroup[which_report], result);
}

/** @brief The Army report's map marks: a banner on every seen stack outside a city. */
void DrawArmyBanners(w2::UiContext& ctx) {
    std::set<int> marked_tiles;
    gfx::Renderer& ren = ctx.Renderer();
    w2::Game& game = ctx.ActiveGame();
    ren.SetScissor(kMapX, kMapY, 224, 312);
    ren.SetColor(1, 1, 1);
    int viewer = w2::game::ViewerSide(game, ctx.Player());
    for (w2::Army* army : game.Armies()) {
        if (!army->InTransit() && army->X() != w2::kNone) {
            int tile_key = army->Y() * 1000 + army->X();
            if (!marked_tiles.count(tile_key) &&
                w2::game::Seen(game, viewer, army->X(), army->Y()) &&
                !w2::game::CityAt(game, army->X(), army->Y())) {
                marked_tiles.insert(tile_key);
                int side = army->IsNeutral() ? 8 : army->OwnerIndex();
                ren.Draw(ctx.Assets().AtransShields(), gfx::NewQuad(side * 16, 164, 16, 10),
                         kMapX + std::max(0, army->X() * 2 - 1),
                         kMapY + std::max(0, army->Y() * 2 - 1));
            }
        }
    }
    ren.SetScissor();
}

/**
 * @brief A bar per side still in the game, in its shield's colours, and the
 * axis under them marked 0, half and the most.
 */
void DrawBars(w2::UiContext& ctx, const report::Figures& figures) {
    gfx::Renderer& ren = ctx.Renderer();
    for (int i = 0; i < 8; i++) {
        if (!figures.out[i]) {
            int y = 200 + 14 * i;
            int bar_width = 1;
            if (figures.max > 0) bar_width = std::max(0, figures.value[i]) * 240 / figures.max;
            bar_width = std::max(1, bar_width);
            ren.SetColor(1, 1, 1);
            int shield_x = 320 + 8 * (i % 4);
            int shield_y = 40 + 8 * (i / 4);
            for (int x = 0; x < bar_width; x += 8) {
                int piece = std::min(8, bar_width - x);
                ren.Draw(ctx.Assets().ShieldsImg(), gfx::NewQuad(shield_x, shield_y, piece, 8),
                         312 + x, y);
            }
            kit::Bevel(ctx, 311, y - 1, bar_width + 2, 10, 0, 1);
        }
    }
    // the axis and its ticks, drawn twice: in colour 1 in place, then in
    // colour 0 one pixel up and left (`offset`)
    auto axis = [&ctx, &ren](int colour, int offset) {
        kit::SetPal(ctx, colour);
        ren.Rectangle(gfx::kFill, 312 - offset, 196 - offset, 240, 1);
        for (int tick = 0; tick < 5; tick++) {
            bool long_tick = tick % 2 == 0;
            ren.Rectangle(gfx::kFill, kTickX[tick] - offset, (long_tick ? 190 : 192) - offset, 1,
                          long_tick ? 6 : 4);
        }
    };
    axis(1, 0);
    axis(0, 1);
    const Font& font = kit::GetFont(ctx, 2);
    ren.SetColor(1, 1, 1);
    kit::Right(font, absl::StrFormat("%d", figures.max), 556, 172);
    kit::Centred(font, absl::StrFormat("%d", figures.max / 2), 432, 172);
    kit::Centred(font, "0", 308, 172);
}

/**
 * @brief Modal displaying tabbed Army, City, Gold, Production, and Winning reports.
 */
class Reports final : public kit::Modal {
  public:
    Reports(w2::UiContext& ctx, int which)
            : kit::Modal(ctx), view_(kit::View(ctx, kDialog)), shown_report_(which) {
        SetDialogView(&view_);
        Refresh();
    }

    void Draw() override {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        w2::Game& game = ctx.ActiveGame();
        w2::Side& player = *ctx.Player();
        auto figures = report::ComputeFigures(game, player, shown_report_);
        kit::Popup(ctx, kPopupRect);
        if (shown_report_ == report::kProduction) {
            ctx.Actions().DrawVectorMap(kMapX, kMapY, nullptr, w2::kNone, true);
        } else {
            ctx.Actions().DrawStrategicMap(kMapX, kMapY);
            if (shown_report_ == report::kArmy) DrawArmyBanners(ctx);
        }
        ren.SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(ctx, 1), kit::Text(ctx, kTitleGroup, shown_report_), 432, 62);
        kit::Centred(kit::GetFont(ctx, 2), kit::Text(ctx, kWhatGroup, shown_report_), 432, 149);
        if (shown_report_ == report::kProduction) {
            DrawProduction();
        } else {
            DrawBars(ctx, figures);
        }
        kit::Centred(kit::GetFont(ctx, 2).Colours(player.Colour(), player.Edge()),
                     Summary(ctx, shown_report_, figures), 432, 322);
        kit::DrawControls(ctx, view_, HiddenControls());
    }

    void Mousepressed(int x, int y, int) override {
        const auto* control = kit::ControlAt(view_, x, y, HiddenControls());
        if (!control) return;
        if (control->id == kDone) {
            PopSelf();
        } else if (control->id >= kTabFirst && control->id < kTabFirst + 5) {
            shown_report_ = control->id - kTabFirst;
            top_row_ = 0;
            Refresh();
        } else if (control->id == kUp) {
            Scroll(-1);
        } else if (control->id == kDown) {
            Scroll(1);
        } else if (control->id == kPageUp) {
            Scroll(-kRows);
        } else if (control->id == kPageDown) {
            Scroll(kRows);
        }
    }

    void Keypressed(const std::string& key) override {
        if (key == "escape" || key == "return" || key == "kpenter") PopSelf();
    }

  private:
    /** @brief Sets the tabs' and scroll buttons' states for the report shown. */
    void Refresh() {
        w2::UiContext& ctx = Ctx();
        auto& states = view_.state;
        for (int i = 0; i <= 4; i++) {
            states[kTabFirst + i] = i == shown_report_ ? w2::uidata::kActive : w2::uidata::kNormal;
        }
        states[kDone] = w2::uidata::kNormal;
        HiddenControls().clear();
        if (shown_report_ == report::kProduction) {
            int count =
                    static_cast<int>(ctx.ActiveGame().TurnNews(ctx.Player()->Id()).produced.size());
            states[kUp] = top_row_ > 0 ? w2::uidata::kNormal : w2::uidata::kDisabled;
            states[kPageUp] = states[kUp];
            states[kDown] = top_row_ + kRows < count ? w2::uidata::kNormal : w2::uidata::kDisabled;
            states[kPageDown] = states[kDown];
        } else {
            for (int control_id : {kUp, kDown, kPageUp, kPageDown}) {
                HiddenControls().insert(control_id);
            }
        }
    }

    /** @brief Scrolls the Production list by `rows`, kept within the list. */
    void Scroll(int rows) {
        w2::UiContext& ctx = Ctx();
        int count = static_cast<int>(ctx.ActiveGame().TurnNews(ctx.Player()->Id()).produced.size());
        top_row_ = std::max(0, std::min(std::max(0, count - kRows), top_row_ + rows));
        Refresh();
    }

    /** @brief The Production list: a page of what was built or sent this turn. */
    void DrawProduction() {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        w2::Game& game = ctx.ActiveGame();
        const auto& list = game.TurnNews(ctx.Player()->Id()).produced;
        const Font& font = kit::GetFont(ctx, 2);
        for (int i = 0; i < kRows; i++) {
            if (top_row_ + i >= static_cast<int>(list.size())) break;
            const w2::Produced& entry = list[top_row_ + i];
            int y = 176 + 30 * i;
            ren.SetColor(1, 1, 1);
            font.Draw(absl::StrFormat("%d", top_row_ + i + 1), 312, y);
            kit::Army(ctx, entry.type, ctx.Player()->Index(), 328, 170 + 30 * i, 1);
            w2::City* city = game.GetMap().CityById(entry.city);
            std::string name = entry.standard ? "Standard" : (city ? city->Name() : "");
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
            ren.SetColor(1, 1, 1);
            font.Draw(text, 368, y);
        }
    }

    screen::View view_;     ///< Dialog 7's tabs and buttons.
    int shown_report_ = 0;  ///< The report shown: `report::kArmy` and on.
    int top_row_ = 0;       ///< The Production list's first row shown.
};

}  // namespace

void Open(w2::UiContext& ctx, int which) {
    kit::Push(ctx, std::make_shared<Reports>(ctx, which));
}

}  // namespace reports
