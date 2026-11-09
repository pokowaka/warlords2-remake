/**
 * @file
 * @brief History > City, Events, Gold, Winners, and Triumphs.
 *
 * Plays back what warlords/history recorded, a round at a time. With nothing
 * on record yet it says so (group 93). Popup 10, (32, 60) 576x312, dialog
 * 19: the strategic map with the city shields as they stood that turn, the
 * title, the four tabs (352-355); for City, Gold and Winners a graph of
 * every side, one step a turn, scaled to the largest on record; for Events
 * that round's deeds and a timeline. The arrows (357/358) step a turn; a
 * click on the graph or the timeline picks the turn under it. Done (356).
 */
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>

#include "absl/strings/str_format.h"

#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/history.hpp"

namespace historyui {

namespace history = w2::history;
using w2::DeedType;
using w2::Format;

namespace {
using SideValues = std::array<int, 8>;

constexpr kit::Rect kPopupRect{32, 60, 576, 312};  ///< Popup 10.
/// Dialog 19 and its controls: the first of the four tabs, Done, a turn back and on.
constexpr int kDialog = 19, kTab = 352, kDone = 356, kPrev = 357, kNext = 358;
/// The graph's axes: left, top, width and height.
constexpr int kGraphX = 300, kGraphY = 150, kGraphW = 292, kGraphH = 140;
constexpr kit::Rect kTimeline{275, 322, 314, 11};  ///< The Events tab's timeline bar.
/// The tabs, which are also the modes: History > City, Events, Gold, Winners.
constexpr int kCity = 0, kEvents = 1, kGold = 2, kWinners = 3;

/** @brief The least top a mode's graph is scaled to. */
int FloorOf(int mode) {
    return mode == kCity ? 10 : mode == kGold ? 500 : 100;
}

/** @brief The eight sides' values a mode graphs, from one turn's record. */
const SideValues& Values(const w2::HistoryRecord& record, int mode) {
    if (mode == kCity) return record.cities;
    if (mode == kGold) return record.gold;
    return record.score;
}

/** @brief A one-pixel horizontal line, `length` long, in the current colour. */
void Hline(gfx::Renderer& ren, int x, int y, int length) {
    ren.Rectangle(gfx::kFill, x, y, length, 1);
}

/** @brief A one-pixel vertical line, `length` long, in the current colour. */
void Vline(gfx::Renderer& ren, int x, int y, int length) {
    ren.Rectangle(gfx::kFill, x, y, 1, length);
}

/** @brief A side's small shield (ATRANS, 16x14) at (x, y); nothing for no side. */
void SmallShield(w2::UiContext& ctx, int side, int x, int y) {
    if (side < 0 || side > 7) return;
    ctx.Renderer().SetColor(1, 1, 1);
    ctx.Renderer().Draw(ctx.Assets().AtransShields(),
                        gfx::NewQuad(112 + 16 * (side / 4), 94 + 14 * (side % 4), 16, 14), x, y);
}

/** @brief A deed in words: a line of STRING.DAT group 94 filled in. */
std::string EventText(const w2::UiContext& ctx, const w2::Deed& deed) {
    const w2::Map& map = ctx.ActiveGame().GetMap();
    auto line = [&ctx](int line_index) { return kit::Text(ctx, 0x5e, line_index); };
    auto side_name = [&map](int side_index) {
        return side_index >= 0 && side_index < 8 ? map.Sides()[side_index].Name() : std::string();
    };
    auto city_name = [&map](int city_index) {
        const w2::City* city = map.CityAt(city_index);
        return city ? city->Name() : std::string();
    };
    // What a deed's two values mean depends on its type.
    int subject = deed.v1, object = deed.v2;
    // The lines are group 94 of STRING.DAT, in the order below.
    switch (deed.type) {
    case DeedType::kHeroEmerges:
        return Format(line(0), deed.name, city_name(subject));
    case DeedType::kHeroKilled:
        if (subject == history::kKilledInBattle) return Format(line(1), deed.name);
        if (subject == history::kKilledSearching) return Format(line(2), deed.name);
        return Format(line(3), deed.name, city_name(subject));
    case DeedType::kQuestCompleted:
        return Format(line(4), deed.name);
    case DeedType::kQuestReceived:
        return Format(line(5), deed.name);
    case DeedType::kSideVanquished:
        return Format(line(6), side_name(subject));
    case DeedType::kCityWon:
        if (subject == history::kWonByName) return Format(line(7), deed.name, city_name(object));
        return Format(line(8), side_name(subject), city_name(object));
    case DeedType::kFound: {
        std::string what;
        if (subject == history::kFoundAllies) {
            what = line(13);
        } else if (subject == history::kFoundSage) {
            what = line(14);
        } else if (subject == history::kFoundGold) {
            what = line(15);
        } else {
            for (const auto& item : map.Items()) {
                if (item.index == subject) what = item.name;
            }
        }
        return Format(line(9), deed.name, what);
    }
    case DeedType::kSideVictorious:
        return Format(line(10), side_name(subject));
    case DeedType::kAtWar:
        return Format(line(11), side_name(subject));
    case DeedType::kAtPeace:
        return Format(line(12), side_name(subject));
    case DeedType::kTreachery:
        return Format(line(16), side_name(subject));
    }
    return "";
}

/** @brief The History dialog: one tab's graph or events, a turn at a time. */
class History final : public kit::Modal {
  public:
    History(w2::UiContext& ctx, int which, int turns)
            : kit::Modal(ctx)
            , dialog_view_(kit::View(ctx, kDialog))
            , mode_(which)
            , turn_(turns)
            , turns_(turns) {
        SetDialogView(&dialog_view_);
        Refresh();
    }

    /** @brief Draws the map as it stood that turn, the title, the tab's page and the turn. */
    void Draw() override {
        w2::UiContext& ctx = Ctx();
        kit::Popup(ctx, kPopupRect);
        ctx.Actions().DrawStrategicMap(kPopupRect.x, kPopupRect.y, nullptr, false,
                                       &Rec(turn_).owners);
        ctx.Renderer().SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(ctx, 1), kit::Text(ctx, 0x5b, mode_), 432, 64);
        if (mode_ == kEvents) {
            DrawEvents();
        } else {
            DrawGraph();
        }
        ctx.Renderer().SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(ctx, 2), absl::StrFormat("Turn %d", turn_), 312, 350);
        kit::DrawControls(ctx, dialog_view_);
    }

    /** @brief A click on the graph or timeline picks a turn; else Done, the arrows, a tab. */
    void Mousepressed(int x, int y, int) override {
        if (mode_ != kEvents && x >= kGraphX && x < kGraphX + kGraphW && y >= kGraphY &&
            y < kGraphY + 160) {
            Pick(x, kGraphX, kGraphW);
            Refresh();
            return;
        }
        if (mode_ == kEvents && kTimeline.Contains(x, y)) {
            Pick(x, kTimeline.x, kTimeline.w);
            Refresh();
            return;
        }
        const auto* control = kit::ControlAt(dialog_view_, x, y);
        if (!control || dialog_view_.StateOf(control->id) == w2::uidata::kDisabled) return;
        if (control->id == kDone) {
            PopSelf();
            return;
        }
        if (control->id == kPrev) {
            turn_--;
        } else if (control->id == kNext) {
            turn_++;
        } else if (control->id >= kTab && control->id < kTab + 4) {
            mode_ = control->id - kTab;
        }
        Refresh();
    }

    /** @brief Enter or Escape closes. */
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") PopSelf();
    }

  private:
    /** @brief The record of turn `turn_number`, counted from 1. */
    const w2::HistoryRecord& Rec(int turn_number) const {
        return Ctx().ActiveGame().History()[turn_number - 1];
    }

    /** @brief Marks the tab shown and enables the arrows that have a turn to go to. */
    void Refresh() {
        auto& states = dialog_view_.state;
        for (int i = 0; i <= 3; i++) {
            states[kTab + i] = i == mode_ ? w2::uidata::kActive : w2::uidata::kNormal;
        }
        states[kPrev] = turn_ == 1 ? w2::uidata::kDisabled : w2::uidata::kNormal;
        states[kNext] = turn_ == turns_ ? w2::uidata::kDisabled : w2::uidata::kNormal;
        states[kDone] = w2::uidata::kNormal;
    }

    /** @brief The x of turn `turn_number` on the graph. */
    int TurnX(int turn_number) const { return kGraphX + turn_number * kGraphW / turns_; }

    /**
     * @brief Draws the graph of every side's values over all turns on record.
     *
     * Scaled to the largest value on record (at least the mode's floor), with
     * a line at the turn shown and, under it, the player's value that turn
     * (for Winners, who led).
     */
    void DrawGraph() {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        const w2::Map& map = ctx.ActiveGame().GetMap();
        int top = FloorOf(mode_);
        for (int turn_number = 1; turn_number <= turns_; turn_number++) {
            for (int side = 0; side < 8; side++) {
                top = std::max(top, Values(Rec(turn_number), mode_)[side]);
            }
        }
        // The axes, with ticks; drawn once in black one pixel up and left, as a shadow.
        auto axes = [&ctx, &ren](int colour, int offset) {
            kit::SetPal(ctx, colour);
            Vline(ren, kGraphX - offset, kGraphY - offset, kGraphH);
            Hline(ren, kGraphX - offset, kGraphY + kGraphH - 1 - offset, kGraphW);
            for (int y : {kGraphY, kGraphY + kGraphH / 2, kGraphY + kGraphH - 1}) {
                Hline(ren, kGraphX - 4 - offset, y - offset, 4);
            }
            for (int x : {kGraphX, kGraphX + kGraphW / 2, kGraphX + kGraphW - 1}) {
                Vline(ren, x - offset, kGraphY + kGraphH - 1 - offset, 4);
            }
        };
        axes(0, 1);
        axes(15, 0);
        const Font& font = kit::GetFont(ctx, 2);
        ren.SetColor(1, 1, 1);
        kit::Right(font, absl::StrFormat("%d", top), kGraphX - 8, kGraphY - 6);
        kit::Right(font, "0", kGraphX - 8, kGraphY + kGraphH - 8);
        kit::Centred(font, "0", kGraphX, kGraphY + kGraphH + 8);
        kit::Right(font, absl::StrFormat("%d", turns_), kGraphX + kGraphW + 8,
                   kGraphY + kGraphH + 8);
        kit::Centred(font, "Turns", kGraphX + kGraphW / 2, kGraphY + kGraphH + 8);
        // Each side's line, from where its last step ended.
        SideValues last_x{};
        SideValues last_y{};
        for (int side = 0; side < 8; side++) {
            last_x[side] = kGraphX + 1;
            last_y[side] = kGraphY + kGraphH - 2;
        }
        for (int turn_number = 1; turn_number <= turns_; turn_number++) {
            int x = TurnX(turn_number);
            const auto& values = Values(Rec(turn_number), mode_);
            for (int side = 0; side < 8; side++) {
                int y = kGraphY + kGraphH - 1 -
                        static_cast<int>(
                                std::floor(static_cast<double>(values[side]) / top * kGraphH));
                kit::Line(ctx, map.Sides()[side].Colour(), last_x[side], last_y[side], x, y);
                last_x[side] = x;
                last_y[side] = y;
            }
        }
        int x = TurnX(turn_);
        kit::Line(ctx, 13, x, kGraphY - 2, x, kGraphY + kGraphH + 6);
        const auto& record = Rec(turn_);
        std::string text;
        if (mode_ == kWinners) {
            int best = 0, lead = 0;
            for (int side = 0; side < 8; side++) {
                if (record.score[side] > best) {
                    best = record.score[side];
                    lead = side;
                }
            }
            text = Format(kit::Text(ctx, 0x5c, 3), turn_, map.Sides()[lead].Name());
        } else {
            text = Format(kit::Text(ctx, 0x5c, mode_), turn_,
                          Values(record, mode_)[ctx.Player()->Index()]);
        }
        ren.SetColor(1, 1, 1);
        kit::Centred(font, text, 432, 318);
    }

    /**
     * @brief Draws the turn's deeds, each by its side's shield, and the timeline.
     *
     * A deed between two sides (war, peace, treachery) gets the other side's
     * shield after its text. The timeline fills in the player's colour up to
     * the turn shown, the rest in the player's edge colour.
     */
    void DrawEvents() {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        const Font& font = kit::GetFont(ctx, 2);
        const auto& events = Rec(turn_).events;
        for (size_t i = 0; i < events.size(); i++) {
            const w2::Deed& deed = events[i];
            int y = 149 + 17 * static_cast<int>(i);
            SmallShield(ctx, deed.side, 264, y + 1);
            std::string text = EventText(ctx, deed);
            ren.SetColor(1, 1, 1);
            font.Draw(text, 280, y);
            if (deed.type == DeedType::kAtWar || deed.type == DeedType::kAtPeace ||
                deed.type == DeedType::kTreachery) {
                SmallShield(ctx, deed.v2, 280 + font.Width(text) / 8 * 8 + 16, y + 1);
            }
        }
        kit::Bevel(ctx, 272, 319, 320, 17, 4, 2);
        kit::Bevel(ctx, 273, 320, 318, 15, 4, 2);
        kit::SetPal(ctx, 0);
        kit::Outline(ctx, 274, 321, 316, 13);
        int filled = std::min(kTimeline.w, turn_ * kTimeline.w / turns_);
        kit::SetPal(ctx, ctx.Player()->Colour());
        ren.Rectangle(gfx::kFill, kTimeline.x, kTimeline.y, filled, kTimeline.h);
        if (filled < kTimeline.w - 8) {
            kit::SetPal(ctx, ctx.Player()->Edge());
            ren.Rectangle(gfx::kFill, kTimeline.x + filled, kTimeline.y, kTimeline.w - filled,
                          kTimeline.h);
        }
    }

    /** @brief Shows the turn under `x` on a bar `width` wide starting at `left`. */
    void Pick(int x, int left, int width) {
        int picked =
                static_cast<int>(std::floor(static_cast<double>(x - left) / width * turns_)) + 1;
        turn_ = std::max(1, std::min(turns_, picked));
    }

    screen::View dialog_view_;  ///< Dialog 19's controls and their states.
    int mode_ = 0;              ///< The tab shown: kCity, kEvents, kGold or kWinners.
    int turn_ = 1;              ///< The turn shown, counted from 1.
    int turns_ = 1;             ///< The turns on record, the last one shown.
};

// History > Triumphs: what the side has done to each other side, or lost
// itself. Popup 0, dialog 20: eight tabs of BUTTON.PCK with the sides'
// shields on them (360-367), then five kinds of army with their counts
// (groups 81-85 for your own losses, 86-90 for the others').
constexpr kit::Rect kTriumphsRect{80, 60, 480, 320};  ///< Popup 0.
/// Dialog 20 and its controls: Done and the first of the eight side tabs.
constexpr int kTriDialog = 20, kTriDone = 359, kTriTab = 360;
constexpr std::array<int, 5> kKinds = {4, 25, 28, 5, 29};  ///< Army type drawn per kind of loss.

/** @brief The Triumphs dialog: the player's tallies against one side at a time. */
class TriumphsDialog final : public kit::Modal {
  public:
    explicit TriumphsDialog(w2::UiContext& ctx)
            : kit::Modal(ctx)
            , dialog_view_(kit::View(ctx, kTriDialog))
            , opponent_(ctx.Player()->Index()) {
        SetDialogView(&dialog_view_);
        Refresh();
    }

    /** @brief Draws the side tabs with their shields, and the five kinds with their counts. */
    void Draw() override {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        int own_side = ctx.Player()->Index();
        kit::Popup(ctx, kTriumphsRect);
        ren.SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(ctx, 1), kit::Text(ctx, 0x50, 0), 320, 64);
        const screen::Art* button = ctx.ControlArt(4);
        const screen::Art* big_shields = ctx.ControlArt(38);
        for (int i = 0; i < 8; i++) {
            int x = 128 + 48 * i;
            ren.SetColor(1, 1, 1);
            if (button) {
                ren.Draw(button->image, gfx::NewQuad(400, i == opponent_ ? 40 : 0, 48, 40), x, 109);
            }
            if (big_shields) {
                ren.Draw(big_shields->image, gfx::NewQuad(i * 32, 0, 32, 36), x + 8, 111);
            }
        }
        const Font& font = kit::GetFont(ctx, 2);
        int base = opponent_ == own_side ? 0x51 : 0x56;
        for (int kind = 0; kind <= 4; kind++) {
            int y = 168 + 35 * kind;
            kit::Army(ctx, kKinds[kind], opponent_, 104, y, 1);
            int count = history::Triumph(ctx.ActiveGame(), own_side, opponent_,
                                         static_cast<history::Loss>(kind));
            if (count > 0) {
                ren.SetColor(1, 1, 1);
                font.Draw(Format(kit::Text(ctx, base + kind, count == 1 ? 0 : 1), count), 144,
                          y + 9);
            }
        }
        // The side tabs are drawn above, by hand.
        kit::ControlIdSet tab_controls;
        for (int i = 0; i < 8; i++) tab_controls.insert(kTriTab + i);
        kit::DrawControls(ctx, dialog_view_, tab_controls);
    }

    /** @brief Done closes; a side's tab shows that side. */
    void Mousepressed(int x, int y, int) override {
        const auto* control = kit::ControlAt(dialog_view_, x, y);
        if (!control) return;
        if (control->id == kTriDone) {
            PopSelf();
            return;
        }
        if (control->id >= kTriTab && control->id < kTriTab + 8) {
            opponent_ = control->id - kTriTab;
            Refresh();
        }
    }

    /** @brief Enter or Escape closes. */
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") PopSelf();
    }

  private:
    /** @brief Marks the chosen side's tab. */
    void Refresh() {
        for (int i = 0; i < 8; i++) {
            dialog_view_.state[kTriTab + i] =
                    i == opponent_ ? w2::uidata::kActive : w2::uidata::kNormal;
        }
        dialog_view_.state[kTriDone] = w2::uidata::kNormal;
    }

    screen::View dialog_view_;  ///< Dialog 20's controls and their states.
    int opponent_ = 0;          ///< The side whose tab is shown; the player's own shows its losses.
};
}  // namespace

/**
 * @brief Opens History on tab `which` (City, Events, Gold or Winners) at the last turn.
 *
 * With nothing on record yet, a message says so instead.
 */
void Open(w2::UiContext& ctx, int which) {
    int turns = std::min(static_cast<int>(ctx.ActiveGame().History().size()),
                         history::kLastRecordedTurn);
    if (turns < 1) {
        search::ShowMessage(ctx, kit::Text(ctx, 0x5d, 0), kit::Text(ctx, 0x5d, 1));
        return;
    }
    // The turn shown counts from 1, as the original's does.
    kit::Push(ctx, std::make_shared<History>(ctx, which, turns));
}

/** @brief Opens History > Triumphs on the player's own losses. */
void Triumphs(w2::UiContext& ctx) {
    kit::Push(ctx, std::make_shared<TriumphsDialog>(ctx));
}

}  // namespace historyui
