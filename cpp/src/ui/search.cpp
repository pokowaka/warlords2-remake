/**
 * @file
 * @brief Hero > Search: what a ruin, a temple or a sage says (site_search),
 * and the game's plain message box.
 *
 * A ruin opens popup 4 with "Searching" and SEARCH.PCK in a black frame; its
 * story is told a line at a time from (128, 300), each waiting for a key or a
 * click; then dialog 13: Done (292) and, when an item was found and left on
 * the ground, Take (293).
 *
 * A temple is popup 9 -- TEMPLE.PCK -- with its name and two lines of
 * greeting, over dialog 15: Bless (328) and Quest (329).
 *
 * A sage is announced in the ruin popup, then has a popup of its own --
 * popup 2, the strategic map, dialog 10 -- offering where the rich sites
 * nearby lie (Items, 279), a gem (Money, 280) or a patch of the hidden map
 * (Maps, 281); one of them, then Done (282).
 *
 * The message box is popup 5 -- (144, 179) 352x64 -- closed by any key or
 * click.
 */
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/cues.hpp"
#include "warlords/hero.hpp"
#include "warlords/move.hpp"
#include "warlords/quest.hpp"
#include "warlords/site.hpp"

namespace search {

using w2::Format;

namespace {

using LineList = std::vector<std::string>;
using SharedSearchResult = std::shared_ptr<w2::SearchResult>;
using SageEntryList = std::vector<w2::site::SageEntry>;
using MapPatch = std::array<int, 4>;
using OptionalMapPatch = std::optional<MapPatch>;

constexpr kit::Rect kMessageRect{144, 179, 352, 64};  ///< Popup 5, the message box.

/**
 * @brief The plain message box: one or two centred lines, closed by any key
 * or click.
 */
class MessageModal final : public kit::Modal {
  public:
    MessageModal(w2::UiContext& ctx, std::string line1, std::string line2, bool single_line,
                 kit::Done after)
            : kit::Modal(ctx)
            , line1_(std::move(line1))
            , line2_(std::move(line2))
            , single_line_(single_line)
            , after_(std::move(after)) {}

    /** @brief Draws the popup and its line or lines. */
    void Draw() override {
        w2::UiContext& ctx = Ctx();
        kit::Popup(ctx, kMessageRect);
        ctx.Renderer().SetColor(1, 1, 1);
        const Font& font = kit::GetFont(ctx, 2);
        if (single_line_) {
            kit::Centred(font, line1_, 320, 201);
            return;
        }
        if (!line1_.empty()) kit::Centred(font, line1_, 320, 190);
        if (!line2_.empty()) kit::Centred(font, line2_, 320, 212);
    }

    void Mousepressed(int, int, int) override { Close(); }
    void Keypressed(const std::string&) override { Close(); }

  private:
    /** @brief Closes the box, then runs `after_`. */
    void Close() {
        kit::Done after_copy = std::move(after_);
        PopSelf();
        if (after_copy) after_copy();
    }

    std::string line1_;         ///< The first line (the only one when single_line_).
    std::string line2_;         ///< The second line; empty for none.
    bool single_line_ = false;  ///< One line, centred in the box.
    kit::Done after_;           ///< Runs once the box is closed.
};

constexpr kit::Rect kRuin{120, 50, 400, 360};  ///< Popup 4, the ruin popup.
constexpr int kRuinDialog = 13;                ///< BUTTON.DAT dialog under the ruin popup.
constexpr int kDone = 292;                     ///< The ruin's Done control.
constexpr int kTake = 293;                     ///< The ruin's Take control.

/**
 * @brief The story a ruin's search tells: the guardian and the fight, then
 * what was found -- an item, gold or allies.
 */
LineList RuinLines(const w2::UiContext& ctx, const w2::SearchResult& result) {
    LineList lines;
    std::string name = result.hero ? result.hero->Name() : "";
    const w2::Monster* monster = result.monster ? result.monster : result.guardian;
    if (result.kind != "allies") {
        if (monster) {
            lines.push_back(Format(kit::Text(ctx, 0x33, 0), name, monster->name));
            lines.push_back(kit::Text(ctx, 0x33, result.kind == "killed" ? 1 : 2));
        } else {
            lines.push_back(kit::Text(ctx, 0x32, 0));
        }
    }
    if (result.kind == "item" && result.item) {
        lines.push_back(Format(kit::Text(ctx, 0x34, 0), name, result.item->name));
    } else if (result.kind == "gold") {
        lines.push_back(Format(kit::Text(ctx, 0x35, 0), name, result.gold));
    } else if (result.kind == "allies") {
        // the number rolled, placed or not
        int allies_count = result.count > 0 ? result.count : static_cast<int>(result.armies.size());
        std::string type_name = result.type ? result.type->name : "";
        lines.push_back(allies_count == 1
                                ? Format(kit::Text(ctx, 0x36, 0), type_name, name)
                                : Format(kit::Text(ctx, 0x36, 1), allies_count, type_name, name));
    }
    return lines;
}

/**
 * @brief The ruin popup telling a story, a line per key or click.
 *
 * With `after`, it has no buttons: the click after the last line closes it
 * and runs `after`. Otherwise Done, and Take when an item lies there.
 */
class RuinSearchModal final : public kit::Modal {
  public:
    RuinSearchModal(w2::UiContext& ctx, LineList lines, bool guarded, bool can_take, w2::Army* hero,
                    kit::Done after)
            : kit::Modal(ctx)
            , lines_(std::move(lines))
            , dialog_view_(kit::View(ctx, kRuinDialog))
            , guarded_(guarded)
            , hero_(hero)
            , after_(std::move(after)) {
        SetDialogView(&dialog_view_);
        dialog_view_.state[kDone] = w2::uidata::kNormal;
        dialog_view_.state[kTake] = w2::uidata::kNormal;
        if (!can_take) HiddenControls().insert(kTake);
    }

    /** @brief Draws the popup, the picture, the lines shown and, at the end, the buttons. */
    void Draw() override {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        kit::Popup(ctx, kRuin);
        ren.SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(ctx, 1), "Searching", 320, 53);
        ren.Draw(ctx.Assets().SearchPic(), 160, 92);
        kit::SetPal(ctx, 0);
        kit::Outline(ctx, 159, 91, 322, 202);
        const Font& font = kit::GetFont(ctx, 2);
        ren.SetColor(1, 1, 1);
        for (size_t i = 0; i < shown_ && i < lines_.size(); i++) {
            font.Draw(lines_[i], 128, 300 + 20 * static_cast<int>(i));
        }
        if (AllShown() && !after_) kit::DrawControls(ctx, dialog_view_, HiddenControls());
    }

    /** @brief The next line; at the end, `after_` or the button clicked. */
    void Mousepressed(int x, int y, int) override {
        if (!AllShown()) {
            Advance();
            return;
        }
        if (after_) {
            kit::Done after_copy = std::move(after_);
            PopSelf();
            after_copy();
            return;
        }
        const auto* control = kit::ControlAt(dialog_view_, x, y, HiddenControls());
        if (!control) return;
        if (control->id == kTake && hero_) {
            w2::Game& game = Ctx().ActiveGame();
            for (auto& item : game.GetMap().Items()) {
                if (item.status == 1 && item.x == hero_->X() && item.y == hero_->Y()) {
                    w2::hero::TakeItem(game, hero_, &item);
                }
            }
            Close();
        } else if (control->id == kDone) {
            Close();
        }
    }

    /** @brief The next line; at the end, `after_`, or Enter or Escape closes. */
    void Keypressed(const std::string& key) override {
        if (!AllShown()) {
            Advance();
            return;
        }
        if (after_) {
            kit::Done after_copy = std::move(after_);
            PopSelf();
            after_copy();
            return;
        }
        if (key == "return" || key == "kpenter" || key == "escape") Close();
    }

  private:
    /** @brief True once every line is shown. */
    bool AllShown() const { return shown_ >= lines_.size(); }

    /** @brief Shows the next line. */
    void Advance() {
        // the orchestra comes in as the guardian's line is read on
        if (guarded_ && shown_ == 1) Ctx().Audio().Effect("orch");
        shown_++;
    }

    /** @brief Closes the popup and tells the quests an item may have changed hands. */
    void Close() {
        w2::UiContext& ctx = Ctx();
        PopSelf();
        w2::quest::Notify(ctx.ActiveGame(), *ctx.Player(), "item");
    }

    LineList lines_;            ///< The story, told a line at a time.
    size_t shown_ = 1;          ///< How many lines are shown so far.
    screen::View dialog_view_;  ///< Dialog 13's controls: Done and Take.
    bool guarded_ = false;      ///< A guardian was met: the orchestra plays.
    w2::Army* hero_ = nullptr;  ///< The hero who searched.
    kit::Done after_;           ///< When set, runs instead of the buttons.
};

/** @brief Opens the ruin popup on a story, with its dramatic sound. */
void OpenRuin(w2::UiContext& ctx, LineList lines, bool guarded, bool can_take, w2::Army* hero,
              kit::Done after) {
    auto dialog = std::make_shared<RuinSearchModal>(ctx, std::move(lines), guarded, can_take, hero,
                                                    std::move(after));
    ctx.Audio().Effect("dramatic");
    kit::Push(ctx, std::move(dialog));
}

constexpr kit::Rect kTemple{160, 60, 320, 280};  ///< Popup 9, the temple.
constexpr int kTempleDialog = 15;                ///< BUTTON.DAT dialog under the temple.
constexpr int kBless = 328;                      ///< The temple's Bless control.
constexpr int kQuest = 329;                      ///< The temple's Quest control.

/** @brief The line telling how many armies a temple blessed (temple_bless). */
std::string BlessLine(const w2::UiContext& ctx, int n) {
    if (n == 0) return kit::Text(ctx, 0x37, 0);
    if (n == 1) return kit::Text(ctx, 0x38, 0);
    return Format(kit::Text(ctx, 0x38, 1), n);
}

/** @brief The temple popup: its greeting, and Bless or Quest. */
class TempleModal final : public kit::Modal {
  public:
    TempleModal(w2::UiContext& ctx, SharedSearchResult result, ArmyStack stack)
            : kit::Modal(ctx)
            , result_(std::move(result))
            , stack_(std::move(stack))
            , dialog_view_(kit::View(ctx, kTempleDialog)) {
        SetDialogView(&dialog_view_);
        dialog_view_.state[kBless] = w2::uidata::kNormal;
        bool can_quest = ctx.ActiveGame().GetMap().GameOptions().quests != 0 &&
                         !ctx.Player()->HasQuest() && result_->hero;
        dialog_view_.state[kQuest] = can_quest ? w2::uidata::kNormal : w2::uidata::kDisabled;
    }

    /** @brief Draws TEMPLE.PCK, the temple's name and its greeting. */
    void Draw() override {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        kit::PopupFrame(ctx, kTemple);
        ren.SetColor(1, 1, 1);
        ren.Draw(ctx.Assets().TemplePic(), kTemple.x, kTemple.y);
        const Font& font = kit::GetFont(ctx, 2).Colours(7, 6);
        kit::Centred(font, Format(kit::Text(ctx, 0x13, 0), result_->site->name), 320, 270);
        kit::Centred(font, kit::Text(ctx, 0x13, 1), 320, 290);
        kit::Centred(font, kit::Text(ctx, 0x13, 2), 320, 310);
        kit::DrawControls(ctx, dialog_view_);
    }

    /** @brief Bless blesses the stack; Quest gives the hero a quest. */
    void Mousepressed(int x, int y, int) override {
        w2::UiContext& ctx = Ctx();
        const auto* control = kit::ControlAt(dialog_view_, x, y);
        if (!control) return;
        if (control->id == kBless) {
            // temple_bless: how many were blessed, and a parting line
            SharedSearchResult held_result = result_;
            ArmyStack held_stack = stack_;
            PopSelf();
            ShowMessage(ctx,
                        BlessLine(ctx, w2::site::Bless(ctx.ActiveGame(), *held_result->site,
                                                       held_stack)),
                        kit::Text(ctx, 0x38, 2));
        } else if (control->id == kQuest) {
            SharedSearchResult held_result = result_;
            ArmyStack held_stack = stack_;
            PopSelf();
            // the searching stack's mode decides how far the target is uncovered
            w2::quest::Assign(ctx.ActiveGame(), *ctx.Player(), held_result->hero, false,
                              w2::move::ModeOf(ctx.ActiveGame(), held_stack) == w2::move::kFlying);
            ctx.Actions().OpenQuest();
        }
    }

  private:
    SharedSearchResult result_;  ///< The search that found the temple.
    ArmyStack stack_;            ///< The stack that searched.
    screen::View dialog_view_;   ///< Dialog 15's controls.
};

/**
 * @brief Opens the temple popup, to its music.
 *
 * Quest is greyed when quests are off, the side already has one, or no hero
 * searched.
 */
void OpenTemple(w2::UiContext& ctx, SharedSearchResult result, const ArmyStack& stack) {
    ctx.Audio().Music(w2::cues::kTemple, ctx.ActiveGame().Sides());
    kit::Push(ctx, std::make_shared<TempleModal>(ctx, std::move(result), stack));
}

constexpr kit::Rect kSage{80, 60, 480, 312};  ///< Popup 2, the sage's popup.
constexpr int kSageX = 80;                    ///< Left edge of the sage's strategic map.
constexpr int kSageY = 60;                    ///< Top edge of the sage's strategic map.
constexpr int kSageDialog = 10;               ///< BUTTON.DAT dialog under the sage.
constexpr int kItems = 279;                   ///< Items: where the rich sites lie.
constexpr int kGem = 280;                     ///< Money: a gem.
constexpr int kMap = 281;                     ///< Maps: a patch of the hidden map.
constexpr int kSageDone = 282;                ///< The sage's Done control.

/**
 * @brief The sage's popup: one offer taken, its words, then Done.
 */
class SageModal final : public kit::Modal {
  public:
    SageModal(w2::UiContext& ctx, SharedSearchResult result)
            : kit::Modal(ctx)
            , result_(std::move(result))
            , hero_(result_->hero)
            , dialog_view_(kit::View(ctx, kSageDialog))
            , entries_(w2::site::SageList(ctx.ActiveGame(), *ctx.Player(), hero_->X(), hero_->Y()))
            , hidden_map_(ctx.ActiveGame().GetMap().GameOptions().hidden_map != 0) {
        SetDialogView(&dialog_view_);
        Offers(true);
    }

    /** @brief Draws the map with the way, or the patch, and the sage's words. */
    void Draw() override {
        w2::UiContext& ctx = Ctx();
        kit::Popup(ctx, kSage);
        ctx.Actions().DrawStrategicMap(kSageX, kSageY);
        if (!target_) ctx.Actions().DrawSiteMarkers(kSageX, kSageY, result_->site);
        if (target_) {
            kit::MapTarget(ctx, kSageX, kSageY, hero_->X(), hero_->Y(), target_->x, target_->y);
            ctx.Actions().DrawHeroFigure(kSageX, kSageY, hero_->X(), hero_->Y());
        }
        if (patch_) {
            kit::SetPal(ctx, 15);
            kit::Outline(ctx, kSageX + 2 * (*patch_)[0], kSageY + 2 * (*patch_)[1],
                         2 * (*patch_)[2], 2 * (*patch_)[3]);
        }
        ctx.Renderer().SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(ctx, 1), "A Sage!", 432, 62);
        const Font& font = kit::GetFont(ctx, 2);
        for (int i = 0; i <= 4; i++) {
            kit::Centred(font, kit::Text(ctx, 0x3f, i), 432, 120 + 20 * i);
        }
        for (size_t i = 0; i < said_.size(); i++) {
            kit::Centred(font, said_[i], 432, 240 + 20 * static_cast<int>(i));
        }
        kit::DrawControls(ctx, dialog_view_, HiddenControls());
    }

    /** @brief A gift's button, Done, or -- after Maps -- the tile to uncover. */
    void Mousepressed(int x, int y, int) override {
        w2::UiContext& ctx = Ctx();
        if (pointing_) {
            // the map is region 15: a click on it picks the tile
            int tile_x = (x - kSageX) / 2;
            int tile_y = (y - kSageY) / 2;
            if (x >= kSageX && y >= kSageY && x < kSageX + 224 && y < kSageY + 312) {
                patch_ = w2::site::SageMap(ctx.ActiveGame(), *ctx.Player(), tile_x, tile_y);
                ctx.GetViewport().StratDirty();
                Finished();
            }
            return;
        }
        const auto* control = kit::ControlAt(dialog_view_, x, y, HiddenControls());
        if (!control) return;
        if (control->id == kItems) {
            ChooseItems();
        } else if (control->id == kGem) {
            int gem_value = w2::site::SageGem(ctx.ActiveGame(), *ctx.Player());
            said_.push_back(kit::Text(ctx, 0x3b, 0));
            said_.push_back(Format(kit::Text(ctx, 0x3b, 1), gem_value));
            Finished();
        } else if (control->id == kMap) {
            Offers(false);
            for (int i = 0; i <= 2; i++) said_.push_back(kit::Text(ctx, 0x3c, i));
            pointing_ = true;
        } else if (control->id == kSageDone) {
            PopSelf();
            ctx.GetViewport().StratDirty();
        }
    }

    /** @brief Once Done shows, Enter or Escape closes. */
    void Keypressed(const std::string& key) override {
        if (HiddenControls().count(kSageDone)) return;
        if (key == "escape" || key == "return" || key == "kpenter") {
            w2::UiContext& ctx = Ctx();
            PopSelf();
            ctx.GetViewport().StratDirty();
        }
    }

  private:
    /** @brief Offers the three gifts (`offer`) or greys them all; Done is hidden. */
    void Offers(bool offer) {
        auto& states = dialog_view_.state;
        if (offer) {
            states[kItems] = !entries_.empty() ? w2::uidata::kNormal : w2::uidata::kDisabled;
            states[kGem] = w2::uidata::kNormal;
            states[kMap] = hidden_map_ ? w2::uidata::kNormal : w2::uidata::kDisabled;
        } else {
            states[kItems] = states[kGem] = states[kMap] = w2::uidata::kDisabled;
        }
        HiddenControls() = {kSageDone};
    }

    /** @brief The gift is given: the offers go, Done comes. */
    void Finished() {
        dialog_view_.state[kSageDone] = w2::uidata::kNormal;
        HiddenControls() = {kItems, kGem, kMap};
        pointing_ = false;
    }

    /** @brief Applies the rich-site selection `chosen` from the Items chooser. */
    void OnItemChosen(int chosen) {
        if (chosen == w2::kNone) {
            Offers(true);
            return;
        }
        w2::UiContext& ctx = Ctx();
        const auto& entry = entries_[chosen];
        if (entry.kind == "gold") {
            said_.push_back(kit::Text(ctx, 0x3e, 2));
        } else if (entry.kind == "allies") {
            said_.push_back(kit::Text(ctx, 0x3e, 3));
        } else {
            said_.push_back(Format(kit::Text(ctx, 0x3e, 0), entry.name));
        }
        said_.push_back(kit::Text(ctx, 0x3e, 1));
        if (w2::Site* site = w2::site::SageShow(ctx.ActiveGame(), *ctx.Player(), entry, hero_->X(),
                                                hero_->Y())) {
            said_.push_back(Format(kit::Text(ctx, 0x3e, 7), site->name));
            target_ = site;
        }
        ctx.GetViewport().StratDirty();
        Finished();
    }

    /**
     * @brief Items: choose a rich site from a list; the sage tells of it and
     * points the way. Cancelling offers the gifts again.
     */
    void ChooseItems() {
        Offers(false);
        LineList names;
        for (const auto& entry : entries_) names.push_back(entry.name);
        std::weak_ptr<Modal> self = weak_from_this();
        choose::Open(Ctx(), "Items", names, 0, [this, self](int chosen) {
            if (!self.expired()) OnItemChosen(chosen);
        });
    }

    SharedSearchResult result_;   ///< The search that found the sage.
    w2::Army* hero_ = nullptr;    ///< The hero who searched.
    screen::View dialog_view_;    ///< Dialog 10's controls.
    LineList said_;               ///< The sage's words so far.
    SageEntryList entries_;       ///< The rich sites nearby, for Items.
    bool hidden_map_ = false;     ///< The game hides the map: Maps is offered.
    bool pointing_ = false;       ///< Maps taken: waiting for a click on the map.
    w2::Site* target_ = nullptr;  ///< The site Items pointed at.
    OptionalMapPatch patch_;      ///< The map patch uncovered: x, y, w, h.
};

/** @brief Opens the sage's popup for the hero who found it. */
void OpenSage(w2::UiContext& ctx, SharedSearchResult result) {
    kit::Push(ctx, std::make_shared<SageModal>(ctx, std::move(result)));
}

}  // namespace

/** @brief Opens the message box with two lines. */
void ShowMessage(w2::UiContext& ctx, const std::string& line1, const std::string& line2,
                 kit::Done after) {
    kit::Push(ctx, std::make_shared<MessageModal>(ctx, line1, line2, false, std::move(after)));
}

/** @brief Opens the message box with one line, centred. */
void Say(w2::UiContext& ctx, const std::string& line, kit::Done after) {
    kit::Push(ctx, std::make_shared<MessageModal>(ctx, line, "", true, std::move(after)));
}

/**
 * @brief Searches where the stack stands and shows what the site says: a
 * temple, a sage or a ruin's story.
 */
void Open(w2::UiContext& ctx, const ArmyStack& stack) {
    if (stack.empty()) return;
    auto result = w2::game::SearchHere(ctx.ActiveGame(), stack, true);
    if (!result || result->kind == "no hero") return;
    // with no hero a temple blesses at once, with only its message
    if (result->kind == "temple" && !result->hero) {
        ShowMessage(ctx, BlessLine(ctx, result->blessed), kit::Text(ctx, 0x38, 2));
        return;
    }
    if (result->kind == "temple") {
        OpenTemple(ctx, std::move(result), stack);
        return;
    }
    if (result->kind == "sage") {
        ctx.Audio().Music(w2::cues::kSage, ctx.ActiveGame().Sides());
        std::string hero_name = result->hero ? result->hero->Name() : "";
        OpenRuin(ctx, {Format(kit::Text(ctx, 0x3a, 0), hero_name)}, false, false, nullptr,
                 [&ctx, result]() { OpenSage(ctx, result); });
        return;
    }
    ctx.GetViewport().StratDirty();
    bool guarded = result->kind != "allies" && (result->monster || result->guardian);
    bool can_take = result->kind == "item" && result->item && result->item->status == 1;
    OpenRuin(ctx, RuinLines(ctx, *result), guarded, can_take, result->hero, nullptr);
}

}  // namespace search
