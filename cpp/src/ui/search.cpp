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
#include "platform/sound.hpp"
#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/cues.hpp"
#include "warlords/hero.hpp"
#include "warlords/quest.hpp"
#include "warlords/site.hpp"

namespace search {

using w2::Format;

namespace {
const kit::Rect kMessageRect{144, 179, 352, 64};  ///< Popup 5, the message box.

/**
 * @brief The plain message box: one or two centred lines, closed by any key
 * or click.
 */
struct Message : kit::Modal {
    std::string line1;         ///< The first line (the only one when single_line).
    std::string line2;         ///< The second line; empty for none.
    bool single_line = false;  ///< One line, centred in the box.
    kit::Done after;           ///< Runs once the box is closed.
    /** @brief Closes the box, then runs `after`. */
    void Close() {
        auto after_copy = after;
        kit::Pop(this);
        if (after_copy) after_copy();
    }
    /** @brief Draws the popup and its line or lines. */
    void Draw() override {
        kit::Popup(kMessageRect);
        gfx::SetColor(1, 1, 1);
        const Font& font = kit::GetFont(2);
        if (single_line) {
            kit::Centred(font, line1, 320, 201);
            return;
        }
        if (!line1.empty()) kit::Centred(font, line1, 320, 190);
        if (!line2.empty()) kit::Centred(font, line2, 320, 212);
    }
    void Mousepressed(int, int, int) override { Close(); }
    void Keypressed(const std::string&) override { Close(); }
};

const kit::Rect kRuin{120, 50, 400, 360};  ///< Popup 4, the ruin popup.
const int kRuinDialog = 13;                ///< BUTTON.DAT dialog under the ruin popup.
const int kDone = 292;                     ///< The ruin's Done control.
const int kTake = 293;                     ///< The ruin's Take control.

/**
 * @brief The story a ruin's search tells: the guardian and the fight, then
 * what was found -- an item, gold or allies.
 */
std::vector<std::string> RuinLines(const w2::SearchResult& result) {
    std::vector<std::string> lines;
    std::string name = result.hero ? result.hero->Name() : "";
    const w2::Monster* monster = result.monster ? result.monster : result.guardian;
    if (result.kind != "allies") {
        if (monster) {
            lines.push_back(Format(kit::Text(0x33, 0), name, monster->name));
            lines.push_back(kit::Text(0x33, result.kind == "killed" ? 1 : 2));
        } else {
            lines.push_back(kit::Text(0x32, 0));
        }
    }
    if (result.kind == "item" && result.item) {
        lines.push_back(Format(kit::Text(0x34, 0), name, result.item->name));
    } else if (result.kind == "gold") {
        lines.push_back(Format(kit::Text(0x35, 0), name, result.gold));
    } else if (result.kind == "allies") {
        // the number rolled, placed or not
        int allies_count = result.count > 0 ? result.count : static_cast<int>(result.armies.size());
        std::string type_name = result.type ? result.type->name : "";
        lines.push_back(allies_count == 1
                                ? Format(kit::Text(0x36, 0), type_name, name)
                                : Format(kit::Text(0x36, 1), allies_count, type_name, name));
    }
    return lines;
}

/**
 * @brief The ruin popup telling a story, a line per key or click.
 *
 * With `after`, it has no buttons: the click after the last line closes it
 * and runs `after`. Otherwise Done, and Take when an item lies there.
 */
struct Ruin : kit::Modal {
    std::vector<std::string> lines;  ///< The story, told a line at a time.
    size_t shown = 1;                ///< How many lines are shown so far.
    screen::View dialog_view;        ///< Dialog 13's controls: Done and Take.
    bool guarded = false;            ///< A guardian was met: the orchestra plays.
    bool can_take = false;           ///< An item was left on the ground.
    w2::Army* hero = nullptr;        ///< The hero who searched.
    kit::Done after;                 ///< When set, runs instead of the buttons.

    /** @brief True once every line is shown. */
    bool Done() const { return shown >= lines.size(); }
    /** @brief Shows the next line. */
    void Advance() {
        // the orchestra comes in as the guardian's line is read on
        if (guarded && shown == 1) sound::Effect("orch");
        shown++;
    }
    /** @brief Closes the popup and tells the quests an item may have changed hands. */
    void Close() {
        kit::Pop(this);
        w2::quest::Notify(*g_app.g, *g_app.player, "item");
    }
    /** @brief Draws the popup, the picture, the lines shown and, at the end, the buttons. */
    void Draw() override {
        kit::Popup(kRuin);
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), "Searching", 320, 53);
        gfx::Draw(g_app.search_pic, 160, 92);
        kit::SetPal(0);
        kit::Outline(159, 91, 322, 202);
        const Font& font = kit::GetFont(2);
        gfx::SetColor(1, 1, 1);
        for (size_t i = 0; i < shown && i < lines.size(); i++)
            font.Draw(lines[i], 128, 300 + 20 * static_cast<int>(i));
        if (Done() && !after) kit::DrawControls(dialog_view, hidden);
    }
    /** @brief The next line; at the end, `after` or the button clicked. */
    void Mousepressed(int x, int y, int) override {
        if (!Done()) {
            Advance();
            return;
        }
        if (after) {
            auto after_copy = after;
            kit::Pop(this);
            after_copy();
            return;
        }
        auto control = kit::ControlAt(dialog_view, x, y, hidden);
        if (!control) return;
        if (control->id == kTake && hero) {
            for (auto& item : g_app.g->map->items) {
                if (item.status == 1 && item.x == hero->X() && item.y == hero->Y())
                    w2::hero::TakeItem(*g_app.g, hero, &item);
            }
            Close();
        } else if (control->id == kDone) {
            Close();
        }
    }
    /** @brief The next line; at the end, `after`, or Enter or Escape closes. */
    void Keypressed(const std::string& key) override {
        if (!Done()) {
            Advance();
            return;
        }
        if (after) {
            auto after_copy = after;
            kit::Pop(this);
            after_copy();
            return;
        }
        if (key == "return" || key == "kpenter" || key == "escape") Close();
    }
};

/** @brief Opens the ruin popup on a story, with its dramatic sound. */
void OpenRuin(std::vector<std::string> lines, bool guarded, bool can_take, w2::Army* hero,
              kit::Done after) {
    auto dialog = std::make_shared<Ruin>();
    dialog->lines = lines;
    dialog->guarded = guarded;
    dialog->can_take = can_take;
    dialog->hero = hero;
    dialog->after = after;
    dialog->dialog_view = kit::View(kRuinDialog);
    dialog->view = &dialog->dialog_view;
    dialog->dialog_view.state[kDone] = w2::uidata::kNormal;
    dialog->dialog_view.state[kTake] = w2::uidata::kNormal;
    if (!can_take) dialog->hidden.insert(kTake);
    sound::Effect("dramatic");
    kit::Push(dialog);
}

const kit::Rect kTemple{160, 60, 320, 280};  ///< Popup 9, the temple.
const int kTempleDialog = 15;                ///< BUTTON.DAT dialog under the temple.
const int kBless = 328;                      ///< The temple's Bless control.
const int kQuest = 329;                      ///< The temple's Quest control.

/** @brief The line telling how many armies a temple blessed (temple_bless). */
std::string BlessLine(int n) {
    if (n == 0) return kit::Text(0x37, 0);
    if (n == 1) return kit::Text(0x38, 0);
    return Format(kit::Text(0x38, 1), n);
}

/** @brief The temple popup: its greeting, and Bless or Quest. */
struct Temple : kit::Modal {
    std::shared_ptr<w2::SearchResult> result;  ///< The search that found the temple.
    std::vector<w2::Army*> stack;              ///< The stack that searched.
    screen::View dialog_view;                  ///< Dialog 15's controls.
    /** @brief Draws TEMPLE.PCK, the temple's name and its greeting. */
    void Draw() override {
        kit::PopupFrame(kTemple);
        gfx::SetColor(1, 1, 1);
        gfx::Draw(g_app.temple_pic, kTemple.x, kTemple.y);
        const Font& font = kit::GetFont(2).Colours(7, 6);
        kit::Centred(font, Format(kit::Text(0x13, 0), result->site->name), 320, 270);
        kit::Centred(font, kit::Text(0x13, 1), 320, 290);
        kit::Centred(font, kit::Text(0x13, 2), 320, 310);
        kit::DrawControls(dialog_view);
    }
    /** @brief Bless blesses the stack; Quest gives the hero a quest. */
    void Mousepressed(int x, int y, int) override {
        auto control = kit::ControlAt(dialog_view, x, y);
        if (!control) return;
        if (control->id == kBless) {
            // temple_bless: how many were blessed, and a parting line
            auto held_result = result;
            auto held_stack = stack;
            kit::Pop(this);
            ShowMessage(BlessLine(w2::site::Bless(*g_app.g, *held_result->site, held_stack)),
                        kit::Text(0x38, 2));
        } else if (control->id == kQuest) {
            auto held_result = result;
            auto held_stack = stack;
            kit::Pop(this);
            // the searching stack's mode decides how far the target is uncovered
            w2::quest::Assign(*g_app.g, *g_app.player, held_result->hero, false,
                              w2::move::ModeOf(*g_app.g, held_stack) == w2::move::kFlying);
            front::OpenQuest();
        }
    }
};

/**
 * @brief Opens the temple popup, to its music.
 *
 * Quest is greyed when quests are off, the side already has one, or no hero
 * searched.
 */
void OpenTemple(std::shared_ptr<w2::SearchResult> result, const std::vector<w2::Army*>& stack) {
    sound::Music(w2::cues::kTemple);
    auto dialog = std::make_shared<Temple>();
    dialog->result = result;
    dialog->stack = stack;
    dialog->dialog_view = kit::View(kTempleDialog);
    dialog->view = &dialog->dialog_view;
    dialog->dialog_view.state[kBless] = w2::uidata::kNormal;
    bool can_quest = g_app.g->map->options.quests != 0 && !g_app.player->HasQuest() && result->hero;
    dialog->dialog_view.state[kQuest] = can_quest ? w2::uidata::kNormal : w2::uidata::kDisabled;
    kit::Push(dialog);
}

const kit::Rect kSage{80, 60, 480, 312};  ///< Popup 2, the sage's popup.
const int kSageX = 80;                    ///< Left edge of the sage's strategic map.
const int kSageY = 60;                    ///< Top edge of the sage's strategic map.
const int kSageDialog = 10;               ///< BUTTON.DAT dialog under the sage.
const int kItems = 279;                   ///< Items: where the rich sites lie.
const int kGem = 280;                     ///< Money: a gem.
const int kMap = 281;                     ///< Maps: a patch of the hidden map.
const int kSageDone = 282;                ///< The sage's Done control.

/**
 * @brief The sage's popup: one offer taken, its words, then Done.
 */
struct Sage : kit::Modal {
    std::shared_ptr<w2::SearchResult> result;  ///< The search that found the sage.
    w2::Army* hero = nullptr;                  ///< The hero who searched.
    screen::View dialog_view;                  ///< Dialog 10's controls.
    std::vector<std::string> said;             ///< The sage's words so far.
    std::vector<w2::site::SageEntry> entries;  ///< The rich sites nearby, for Items.
    bool hidden_map = false;                   ///< The game hides the map: Maps is offered.
    bool pointing = false;                     ///< Maps taken: waiting for a click on the map.
    w2::Site* target = nullptr;                ///< The site Items pointed at.
    std::optional<std::array<int, 4>> patch;   ///< The map patch uncovered: x, y, w, h.

    /** @brief Offers the three gifts (`offer`) or greys them all; Done is hidden. */
    void Offers(bool offer) {
        auto& states = dialog_view.state;
        if (offer) {
            states[kItems] = !entries.empty() ? w2::uidata::kNormal : w2::uidata::kDisabled;
            states[kGem] = w2::uidata::kNormal;
            states[kMap] = hidden_map ? w2::uidata::kNormal : w2::uidata::kDisabled;
        } else {
            states[kItems] = states[kGem] = states[kMap] = w2::uidata::kDisabled;
        }
        hidden = {kSageDone};
    }
    /** @brief The gift is given: the offers go, Done comes. */
    void Finished() {
        dialog_view.state[kSageDone] = w2::uidata::kNormal;
        hidden = {kItems, kGem, kMap};
        pointing = false;
    }
    /** @brief Draws the map with the way, or the patch, and the sage's words. */
    void Draw() override {
        kit::Popup(kSage);
        front::DrawStrategicMap(kSageX, kSageY);
        if (!target) front::DrawSiteMarkers(kSageX, kSageY, result->site);
        if (target) {
            kit::MapTarget(kSageX, kSageY, hero->X(), hero->Y(), target->x, target->y);
            front::DrawHeroFigure(kSageX, kSageY, hero->X(), hero->Y());
        }
        if (patch) {
            kit::SetPal(15);
            kit::Outline(kSageX + 2 * (*patch)[0], kSageY + 2 * (*patch)[1], 2 * (*patch)[2],
                         2 * (*patch)[3]);
        }
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), "A Sage!", 432, 62);
        const Font& font = kit::GetFont(2);
        for (int i = 0; i <= 4; i++) kit::Centred(font, kit::Text(0x3f, i), 432, 120 + 20 * i);
        for (size_t i = 0; i < said.size(); i++)
            kit::Centred(font, said[i], 432, 240 + 20 * static_cast<int>(i));
        kit::DrawControls(dialog_view, hidden);
    }
    /**
     * @brief Items: choose a rich site from a list; the sage tells of it and
     * points the way. Cancelling offers the gifts again.
     */
    void Items() {
        Offers(false);
        std::vector<std::string> names;
        for (auto& entry : entries) names.push_back(entry.name);
        std::weak_ptr<Modal> self = weak_from_this();
        Sage* sage = this;
        choose::Open("Items", names, 0, [sage, self](int chosen) {
            if (self.expired()) return;
            if (chosen == w2::kNone) return sage->Offers(true);
            const auto& entry = sage->entries[chosen];
            if (entry.kind == "gold")
                sage->said.push_back(kit::Text(0x3e, 2));
            else if (entry.kind == "allies")
                sage->said.push_back(kit::Text(0x3e, 3));
            else
                sage->said.push_back(Format(kit::Text(0x3e, 0), entry.name));
            sage->said.push_back(kit::Text(0x3e, 1));
            if (w2::Site* site = w2::site::SageShow(*g_app.g, *g_app.player, entry, sage->hero->X(),
                                                    sage->hero->Y())) {
                sage->said.push_back(Format(kit::Text(0x3e, 7), site->name));
                sage->target = site;
            }
            front::StratDirty();
            sage->Finished();
        });
    }
    /** @brief A gift's button, Done, or -- after Maps -- the tile to uncover. */
    void Mousepressed(int x, int y, int) override {
        if (pointing) {
            // the map is region 15: a click on it picks the tile
            int tile_x = (x - kSageX) / 2, tile_y = (y - kSageY) / 2;
            if (x >= kSageX && y >= kSageY && x < kSageX + 224 && y < kSageY + 312) {
                patch = w2::site::SageMap(*g_app.g, *g_app.player, tile_x, tile_y);
                front::StratDirty();
                Finished();
            }
            return;
        }
        auto control = kit::ControlAt(dialog_view, x, y, hidden);
        if (!control) return;
        if (control->id == kItems) {
            Items();
        } else if (control->id == kGem) {
            int gem_value = w2::site::SageGem(*g_app.g, *g_app.player);
            said.push_back(kit::Text(0x3b, 0));
            said.push_back(Format(kit::Text(0x3b, 1), gem_value));
            Finished();
        } else if (control->id == kMap) {
            Offers(false);
            for (int i = 0; i <= 2; i++) said.push_back(kit::Text(0x3c, i));
            pointing = true;
        } else if (control->id == kSageDone) {
            kit::Pop(this);
            front::StratDirty();
        }
    }
    /** @brief Once Done shows, Enter or Escape closes. */
    void Keypressed(const std::string& key) override {
        if (hidden.count(kSageDone)) return;
        if (key == "escape" || key == "return" || key == "kpenter") {
            kit::Pop(this);
            front::StratDirty();
        }
    }
};

/** @brief Opens the sage's popup for the hero who found it. */
void OpenSage(std::shared_ptr<w2::SearchResult> result) {
    auto dialog = std::make_shared<Sage>();
    dialog->result = result;
    dialog->hero = result->hero;
    dialog->dialog_view = kit::View(kSageDialog);
    dialog->view = &dialog->dialog_view;
    dialog->entries =
            w2::site::SageList(*g_app.g, *g_app.player, dialog->hero->X(), dialog->hero->Y());
    dialog->hidden_map = g_app.g->map->options.hidden_map != 0;
    dialog->Offers(true);
    kit::Push(dialog);
}
}  // namespace

/** @brief Opens the message box with two lines. */
void ShowMessage(const std::string& line1, const std::string& line2, kit::Done after) {
    auto dialog = std::make_shared<Message>();
    dialog->line1 = line1;
    dialog->line2 = line2;
    dialog->after = after;
    kit::Push(dialog);
}

/** @brief Opens the message box with one line, centred. */
void Say(const std::string& line, kit::Done after) {
    auto dialog = std::make_shared<Message>();
    dialog->line1 = line;
    dialog->single_line = true;
    dialog->after = after;
    kit::Push(dialog);
}

/**
 * @brief Searches where the stack stands and shows what the site says: a
 * temple, a sage or a ruin's story.
 */
void Open(const std::vector<w2::Army*>& stack) {
    if (stack.empty()) return;
    auto result = w2::game::SearchHere(*g_app.g, stack, true);
    if (!result || result->kind == "no hero") return;
    // with no hero a temple blesses at once, with only its message
    if (result->kind == "temple" && !result->hero)
        return ShowMessage(BlessLine(result->blessed), kit::Text(0x38, 2));
    if (result->kind == "temple") return OpenTemple(result, stack);
    if (result->kind == "sage") {
        sound::Music(w2::cues::kSage);
        OpenRuin({Format(kit::Text(0x3a, 0), result->hero ? result->hero->Name() : "")}, false,
                 false, nullptr, [result]() { OpenSage(result); });
        return;
    }
    front::StratDirty();
    bool guarded = result->kind != "allies" && (result->monster || result->guardian);
    bool can_take = result->kind == "item" && result->item && result->item->status == 1;
    OpenRuin(RuinLines(*result), guarded, can_take, result->hero, nullptr);
}

}  // namespace search
