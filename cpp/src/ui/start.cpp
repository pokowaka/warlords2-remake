/**
 * @file
 * @brief The start screens: the menu the game opens on, choosing a scenario,
 * and setting its sides and options up.
 *
 * The start menu (dialog 1): STARTUP0-3.PCK with New Scenario (100), Load
 * Game (101), Random Map (102) and Begin (103), and the scenario's own
 * PICS\SCENARIO.PCK under a bar with its name. The scenario starts as
 * Erythea.
 *
 * Random Map puts "A Random World" on the bar and, in place of the picture,
 * its settings on colour 3: four sliders -- Water, Hills, Cities, Forest,
 * 106-109, STARTBU.PCK at (384, 216) 30 apart -- each with a "?" (110-113)
 * that leaves it to chance, the terrain set (104) and "Cities can produce
 * allies" (105). Begin then has the advisor say "One moment..." and makes
 * the world on BSCROLL.PCK with a progress bar. Choosing a scenario with New
 * Scenario ends the random world.
 *
 * New Scenario: popup 19, NEWSCEN.PCK, dialog 29 -- the scenarios of
 * SCENARIO.DAT in a black box, seven rows, and on the crystal ball the chosen
 * one's name, description, cities, ruins and players. Not the original's:
 * generated worlds follow SCENARIO.DAT's (w2scengen).
 *
 * Begin sets the sides up on the main screen's own frame, dialog 3: a box a
 * side with its face, its button -- Human, Knight, Lord, Warlord or Off --
 * and its Character box; Begin (141), Main Menu (142), I am the Greatest
 * (143) / No! I really am Normal (144), the presets (145-147) and Edit
 * Options (148); Recall Options (157) and Random Characters (158) at the
 * foot; the difficulty rating; the SSG logo; and under it all a quote on war
 * from QUOTES.DAT. The options are DATA\OPTIONS.DAT's, the ones the last
 * game began with, not the scenario's own.
 *
 * Edit Options: popup 4, dialog 4 -- the ten options of group 4, two to a
 * row; 159-168 change one, 169-171 are the presets, OK (172).
 *
 * Setup Side, from a side's Character box (133-140) or its face (149-156):
 * popup 4, dialog 27 -- the side's name to retype, and for a computer the
 * characters of its level's deck to choose from, with the chosen one's
 * description. OK (457), Cancel (458).
 */
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/str_format.h"

#include "front/pckimage.hpp"
#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/aicard.hpp"
#include "warlords/bytes.hpp"
#include "warlords/cues.hpp"
#include "warlords/randommap.hpp"
#include "warlords/scenarios.hpp"
#include "warlords/scn.hpp"

namespace start {

using w2::Format;

namespace {

using LineList = std::vector<std::string>;
using OptionalEditor = std::optional<input::Editor>;
using RollCallback = std::function<int(int)>;
using PickScenarioCallback = std::function<void(int)>;
using RunHandle = std::unique_ptr<w2::randommap::Run>;

/// The ten options' keys for the game, in the order of string group 4.
constexpr std::array<const char*, 10> kOptionKeys = {
    "neutralCities",  "diplomacy",     "quests",     "hiddenMap",       "viewEnemies",
    "viewProduction", "intenseCombat", "quickStart", "militaryAdvisor", "randomTurns"};

/// The three presets (Beginner, Intermediate, Advanced), one value per option.
constexpr int kPresets[3][10] = {
    {0, 0, 0, 0, 1, 0, 0, 0, 1, 0},
    {1, 1, 1, 0, 0, 0, 0, 0, 1, 0},
    {2, 1, 1, 1, 0, 1, 0, 0, 1, 0},
};

/**
 * @brief Puts back the options the last game began with: ten u16s in
 * DATA\OPTIONS.DAT.
 *
 * Used as the setup screen opens and by Recall Options. With no file the
 * table is left as it is.
 */
void RecallOptions(w2::UiContext& ctx) {
    auto file = w2::ReadBytes(ctx.Assets().DataDir() / "DATA/OPTIONS.DAT");
    if (!file.ok()) return;
    auto& remembered = ctx.StartWorld().remembered_options;
    for (size_t i = 0; i <= 9 && 2 * i + 2 <= file->size(); i++) {
        remembered[i] = w2::U16(*file, 2 * i);
    }
}

/** @brief Keeps the options for next time in DATA\OPTIONS.DAT, as Begin starts the game. */
void KeepOptions(const w2::UiContext& ctx) {
    const auto& remembered = ctx.StartWorld().remembered_options;
    std::vector<uint8_t> bytes;
    bytes.reserve(20);
    for (int i = 0; i <= 9; i++) {
        bytes.push_back(static_cast<uint8_t>(remembered[i] & 255));
        bytes.push_back(static_cast<uint8_t>((remembered[i] >> 8) & 255));
    }
    w2::WriteBytes(ctx.Assets().DataDir() / "DATA/OPTIONS.DAT", bytes).IgnoreError();
}

/** @brief A scenario to choose, as the chooser shows it. */
struct Scenario {
    std::string name;  ///< Its name, on the bar and in the list.
    std::string dir;   ///< Its directory under the data directory.
    std::string text;  ///< Its description.
    int cities = 0;    ///< How many cities it has.
    int ruins = 0;     ///< How many ruins it has.
    int players = 0;   ///< How many sides it has.
};

using ScenarioList = std::vector<Scenario>;

/**
 * @brief SCENARIO.DAT's records, and after them the worlds the scenario
 * generator made (not the original's; see warlords/scenarios and
 * docs/scengen.md).
 */
ScenarioList Scenarios(const w2::UiContext& ctx) {
    ScenarioList out;
    for (const auto& entry : w2::scenarios::List(ctx.Assets().DataDir())) {
        out.push_back(Scenario{entry.name, entry.dir, entry.text, entry.cities, entry.ruins,
                               entry.players});
    }
    return out;
}

/** @brief Loads a .PCK picture once via `AssetCatalog` and keeps it; null when missing. */
gfx::ImageP LoadStartImage(w2::UiContext& ctx, const std::filesystem::path& path,
                           int key = pckimage::kNokey) {
    return ctx.Assets().CachedImage(ctx.Renderer(), path, key);
}

/** @brief One side as the setup screen sets it up. */
struct SideSetup {
    bool in_use = false;    ///< Whether the scenario has the side at all.
    std::string name;       ///< The side's name.
    int colour = 15;        ///< Its palette colour.
    int edge = 0;           ///< Its edge colour.
    bool computer = false;  ///< Played by the computer.
    int level = 0;          ///< Knight, Lord, Warlord, or 3 for Off.
    int card = 0;           ///< The character (AI card) played; 0 is the Standard one.
};

/** @brief Everything the setup screen sets up before the game begins. */
struct Setup {
    Scenario scenario;                 ///< The scenario.
    int* options = nullptr;            ///< The ten options, the session's table.
    std::array<SideSetup, 8> sides{};  ///< The eight sides.
    bool greatest = false;             ///< "I am the Greatest": every side a Warlord.
};

using SharedSetup = std::shared_ptr<Setup>;
using BeginSetupCallback = std::function<void(SharedSetup)>;

/**
 * @brief A new setup for a scenario: its sides as the scenario has them.
 *
 * The tutorial plays with the Beginner options, any other scenario with the
 * last game's.
 */
SharedSetup NewSetup(w2::UiContext& ctx, const Scenario& scenario) {
    std::string dir = w2::Upper(scenario.dir);
    std::filesystem::path scenario_dir = ctx.Assets().DataDir() / dir;
    w2::StartWorldSettings& world = ctx.StartWorld();
    auto loaded_map = (dir == w2::randommap::kDir && !world.generated_files.empty())
                              ? w2::scn::LoadFromFiles(scenario_dir, dir, world.generated_files)
                              : w2::scn::Load(scenario_dir, dir);
    auto setup = std::make_shared<Setup>();
    setup->scenario = scenario;
    setup->options = world.remembered_options.data();
    if (!loaded_map.ok()) return setup;
    const auto& map = *loaded_map;
    if (map->GameOptions().tutorial != 0) {
        for (int i = 0; i <= 9; i++) world.remembered_options[i] = kPresets[0][i];
    } else {
        RecallOptions(ctx);
    }
    for (const auto& side : map->Sides()) {
        SideSetup& side_setup = setup->sides[side.Index()];
        side_setup.in_use = side.InUse();
        side_setup.name = side.Name();
        side_setup.colour = side.Colour();
        side_setup.edge = side.Edge();
        side_setup.computer = side.IsComputerControlled();
        side_setup.level = side.IsComputerControlled() ? side.Level() : 0;
        side_setup.card = side.Card();
    }
    return setup;
}

/** @brief Whether the first six options are those of a preset. */
bool PresetMatches(const Setup& setup, int preset) {
    for (int i = 0; i <= 5; i++) {
        if (setup.options[i] != kPresets[preset][i]) return false;
    }
    return true;
}

/** @brief The difficulty rating, a percent: the options' weight and the computers' strength. */
int Rating(const Setup& setup) {
    const int* options = setup.options;
    int options_weight = std::min(20, options[0] * 4 + options[1] * 4 + options[2] * 3 +
                                              options[3] * 4 - options[4] + options[5]);
    int sum = 0;
    int computers = 0;
    for (const auto& side : setup.sides) {
        if (side.in_use && side.computer && side.level != 3) {
            sum += side.level + 1;
            computers++;
        }
    }
    int strength = computers == 0 ? 80 : sum * 80 / (computers * 3);
    if (strength >= 78) strength = 80;
    return strength + options_weight;
}

constexpr kit::Rect kOptionsPopupRect{120, 50, 400, 360};  ///< Popup 4, Edit Options / Setup Side.

/** @brief Edit Options: popup 4, dialog 4, the ten options and the presets. */
class OptionsModal final : public kit::Modal {
  public:
    OptionsModal(w2::UiContext& ctx, SharedSetup setup, kit::Done after)
            : kit::Modal(ctx)
            , setup_(std::move(setup))
            , dialog_view_(kit::View(ctx, 4))
            , after_(std::move(after)) {
        SetDialogView(&dialog_view_);
        Refresh();
    }

    /** @brief Draws the dialog: two headed sections and each option with its value. */
    void Draw() override {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        kit::Popup(ctx, kOptionsPopupRect);
        ren.SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(ctx, 1), kit::Text(ctx, 8, 0), 320, 55);
        const Font& font = kit::GetFont(ctx, 2);
        const int section_ys[2] = {111, 250};
        for (int section = 0; section < 2; section++) {
            int y = section_ys[section];
            kit::SetPal(ctx, 15);
            ren.Rectangle(gfx::kFill, 160, y, 320, 1);
            kit::SetPal(ctx, 0);
            ren.Rectangle(gfx::kFill, 161, y + 1, 320, 1);
            std::string heading = kit::Text(ctx, 9 + section, 0);
            int width = font.Width(heading);
            kit::SetPal(ctx, 3);
            ren.Rectangle(gfx::kFill, 320 - width / 2 - 4, y, width + 8, 2);
            ren.SetColor(1, 1, 1);
            kit::Centred(font, heading, 320, y - 7);
        }
        for (int i = 0; i <= 9; i++) {
            int x = i % 2 == 0 ? 128 : 320;
            int y = i < 6 ? 131 + 30 * (i / 2) : 270 + 30 * ((i - 6) / 2);
            ren.SetColor(1, 1, 1);
            font.Draw(kit::Text(ctx, 4, i), x, y);
            int value = setup_->options[i];
            // View Production's word is the other way round: its flag hides
            // other sides' production, so set it reads "Off"
            bool enabled = i == 5 ? value == 0 : value != 0;
            std::string word = i == 0 ? kit::Text(ctx, 5, value) : (enabled ? "On" : "Off");
            font.Colours(7, 0).Draw(word, x + 128, y);
        }
        kit::DrawControls(ctx, dialog_view_);
    }

    /**
     * @brief Handles a click: OK, an option (Neutral Cities goes round three
     * values, the rest toggle), or a preset.
     */
    void Mousepressed(int x, int y, int) override {
        const auto* control = kit::ControlAt(dialog_view_, x, y);
        if (!control) return;
        int control_id = control->id;
        if (control_id == 172) {
            Finish();
            return;
        }
        if (control_id >= 159 && control_id <= 168) {
            int option = control_id - 159;
            if (option == 0) {
                setup_->options[0] = (setup_->options[0] + 1) % 3;
            } else {
                setup_->options[option] = setup_->options[option] != 0 ? 0 : 1;
            }
        } else if (control_id >= 169 && control_id <= 171) {
            for (int option = 0; option <= 5; option++) {
                setup_->options[option] = kPresets[control_id - 169][option];
            }
        }
        Refresh();
    }

    /** @brief Return and Escape close the dialog. */
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") Finish();
    }

  private:
    /** @brief Sets the controls' states; a preset that matches is lit. */
    void Refresh() {
        auto& state = dialog_view_.state;
        for (int i = 0; i <= 9; i++) state[159 + i] = w2::uidata::kNormal;
        for (int preset = 0; preset <= 2; preset++) {
            state[169 + preset] =
                    PresetMatches(*setup_, preset) ? w2::uidata::kActive : w2::uidata::kNormal;
        }
        state[172] = w2::uidata::kNormal;
    }

    /** @brief Closes the dialog and calls `after_`. */
    void Finish() {
        kit::Done done = std::move(after_);
        PopSelf();
        if (done) done();
    }

    SharedSetup setup_;         ///< The setup whose options are edited.
    screen::View dialog_view_;  ///< Dialog 4 and its controls' states.
    kit::Done after_;           ///< Called once the dialog is closed.
};

/** @brief Opens Edit Options on a setup; `after` runs once it is closed. */
void OpenOptions(w2::UiContext& ctx, SharedSetup setup, kit::Done after) {
    kit::Push(ctx, std::make_shared<OptionsModal>(ctx, std::move(setup), std::move(after)));
}

/**
 * @brief SETUPBU.PCK's faces: Knight, Lord, Warlord, Off, the two humans, a
 * side not in the scenario, then a Knight, Lord and Warlord playing a
 * character other than the Standard one.
 */
constexpr int kFaces[10][2] = {{0, 0},   {0, 40},   {0, 80},    {0, 120},   {0, 160},
                               {0, 200}, {440, 40}, {424, 100}, {424, 140}, {424, 180}};
/// SETUPBU.PCK's level buttons' x: Knight, Lord, Warlord, Off, Human.
constexpr int kLevelButton[5] = {120, 200, 280, 360, 40};
/**
 * @brief The Character box in SETUPBU.PCK: ticked with a character chosen,
 * else empty; 24 x 20 at (x + 64, y + 39), its label at (x + 88, y + 41).
 */
constexpr int kCheck[2][2] = {{440, 0}, {440, 20}};

/**
 * @brief A side's face, an index into kFaces.
 * @param index The side's index: the two human faces take turns by side.
 */
int FaceOf(const SideSetup& side, int index) {
    if (!side.in_use) return 6;
    if (!side.computer) return index % 2 == 1 ? 5 : 4;
    int level = std::clamp(side.level, 0, 3);
    if (side.card != 0 && level < 3) return level + 7;
    return level;
}

/** @brief How many computers are in play: Random Characters needs one. */
int Computers(const Setup& setup) {
    int count = 0;
    for (const auto& side : setup.sides) {
        if (side.in_use && side.computer && side.level != 3) count++;
    }
    return count;
}

// Setup Side's controls (dialog 27)
constexpr int kSideOk = 457;
constexpr int kSideCancel = 458;
constexpr int kSideName = 459;
constexpr int kSideUp = 460;
constexpr int kSideDown = 461;
constexpr int kSidePageUp = 462;
constexpr int kSidePageDown = 463;
constexpr int kSideRow = 464;
constexpr kit::Rect kNameBox{229, 138, 188, 22};
/// The colour a Knight's characters are listed in, a Lord's and a Warlord's.
constexpr int kLevelInk[3] = {5, 7, 9};

/**
 * @brief Setup Side: popup 4, dialog 27.
 *
 * A side's name, retyped in the field (15 characters, 128 pixels); for a
 * computer its level's characters, five rows at a time, and the chosen one's
 * description from its .DSC. A human has no character: "N/A". OK keeps what
 * was done, Cancel puts the name and character back.
 */
class SideDialogModal final : public kit::Modal {
  public:
    SideDialogModal(w2::UiContext& ctx, SharedSetup setup, int index, kit::Done after)
            : kit::Modal(ctx)
            , setup_(std::move(setup))
            , index_(index)
            , dialog_view_(kit::View(ctx, 27))
            , after_(std::move(after)) {
        SetDialogView(&dialog_view_);
        Start();
    }

    /** @brief Draws the dialog: the name field, the list of characters and the description. */
    void Draw() override {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        const SideSetup& side = EditedSide();
        kit::Popup(ctx, kOptionsPopupRect);
        ren.SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(ctx, 1), "Setup Side", 320, 55);
        int colour = side.colour;
        int edge = side.edge;
        const Font& font = kit::GetFont(ctx, 2);
        // two boxes, each outlined in the side's edge colour and twice more in
        // its colour, a pixel further up and left each time
        const int boxes[2][2] = {{107, 67}, {194, 165}};
        for (const auto& box : boxes) {
            kit::SetPal(ctx, edge);
            kit::Outline(ctx, 138, box[0], 368, box[1]);
            kit::SetPal(ctx, colour);
            kit::Outline(ctx, 137, box[0] - 1, 368, box[1]);
            kit::Outline(ctx, 136, box[0] - 2, 368, box[1]);
        }
        const std::pair<const char*, int> tabs[2] = {{"Side Name", 96}, {"Leader", 181}};
        for (const auto& tab : tabs) {
            kit::SetPal(ctx, 3);
            ren.Rectangle(gfx::kFill, 160, tab.second, font.Width(tab.first), 17);
            ren.SetColor(1, 1, 1);
            font.Colours(colour, edge).Draw(tab.first, 160, tab.second);
        }
        kit::Shield(ctx, index_, 144, 122);
        kit::Shield(ctx, index_, 456, 122);
        kit::Shield(ctx, index_, 144, 202);
        ren.SetColor(1, 1, 1);
        kit::Centred(font, "Retype the name of this side", 320, 116);
        kit::Field(ctx, kNameBox.x, kNameBox.y, kNameBox.w, kNameBox.h,
                   editing_ ? editing_->text : side.name, &font);
        if (editing_) editing_->DrawCursor(ctx, kNameBox.x, kNameBox.y);
        kit::SetPal(ctx, 0);
        ren.Rectangle(gfx::kFill, 196, 218, 72, 1);
        ren.Rectangle(gfx::kFill, 384, 218, 96, 1);
        ren.SetColor(1, 1, 1);
        font.Draw("Name", 196, 203);
        font.Draw("Description", 384, 203);
        // the list: a sunk box, the chosen character in white
        kit::SetPal(ctx, 3);
        ren.Rectangle(gfx::kFill, 189, 249, 186, 100);
        kit::Bevel(ctx, 188, 248, 188, 102, 4, 2);
        ren.SetColor(1, 1, 1);
        if (!side.computer) {
            font.Draw("N/A", 196, 224);
            font.Draw("N/A", 384, 224);
        } else {
            const Font& ink =
                    font.Colours(side.level >= 0 && side.level < 3 ? kLevelInk[side.level] : 15, 0);
            for (int row = 0; row < 5 && rows_[row] >= 0; row++) {
                const std::string& name =
                        rows_[row] < DeckSize() ? deck_[rows_[row]] : std::string();
                (rows_[row] == side.card ? font : ink).Draw(name, 196, 252 + 19 * row);
            }
            // the name, and the .DSC's next six lines
            if (side.card >= 0 && side.card < DeckSize()) ink.Draw(deck_[side.card], 192, 224);
            if (desc_card_ != side.card) {
                desc_card_ = side.card;
                desc_.clear();
                if (side.level >= 0 && side.level < 3) {
                    if (auto description =
                                w2::aicard::Describe(ctx.Assets().DataDir(), side.level, side.card))
                        desc_ = description->second;
                }
            }
            for (int line = 0; line < 6 && line < static_cast<int>(desc_.size()); line++) {
                if (!desc_[line].empty()) ink.Draw(desc_[line], 384, 224 + 20 * line);
            }
        }
        kit::DrawControls(ctx, dialog_view_);
    }

    /**
     * @brief Handles a click: OK, Cancel, the name field, scrolling the list
     * (a row run off the deck is left empty) or choosing a row's character.
     */
    void Mousepressed(int x, int y, int) override {
        if (editing_) KeepName();
        const auto* control = kit::ControlAt(dialog_view_, x, y);
        if (!control) return;
        int control_id = control->id;
        if (control_id == kSideOk) {
            Close(true);
            return;
        }
        if (control_id == kSideCancel) {
            Close(false);
            return;
        }
        if (control_id == kSideName) {
            editing_ = input::Editor{};
            editing_->max_chars = 15;
            editing_->max_width = 128;
        } else if (control_id == kSideUp || control_id == kSideDown) {
            int step = control_id == kSideUp ? -1 : 1;
            for (int row = 0; row < 5; row++) {
                rows_[row] += step;
                if (rows_[row] < 0 || rows_[row] >= DeckSize()) rows_[row] = -1;
            }
        } else if (control_id == kSidePageUp || control_id == kSidePageDown) {
            // five rows, or as many as there are
            int step = control_id == kSidePageUp ? -std::min(rows_[0], 5)
                                                 : std::min(DeckSize() - rows_[4] - 1, 5);
            rows_[0] += step;
            for (int row = 1; row < 5; row++) rows_[row] = rows_[row - 1] + 1;
        } else if (control_id >= kSideRow && control_id < kSideRow + 5) {
            int card = rows_[control_id - kSideRow];
            if (card >= 0) EditedSide().card = card;
        }
        Refresh();
    }

    /** @brief Keys go to the name field while it is edited; else Return is OK, Escape Cancel. */
    void Keypressed(const std::string& key) override {
        if (editing_) {
            std::string result = editing_->Key(key);
            if (result == "keep") {
                KeepName();
            } else if (result == "undo") {
                editing_.reset();
            }
            return;
        }
        if (key == "return" || key == "kpenter") {
            Close(true);
        } else if (key == "escape") {
            Close(false);
        }
    }

    /** @brief Typed text goes to the name field while it is edited. */
    void Textinput(const std::string& text) override {
        if (editing_) editing_->Input(Ctx(), text);
    }

    bool Typing() const override { return editing_.has_value(); }

  private:
    /** @brief The side being set up. */
    SideSetup& EditedSide() { return setup_->sides[index_]; }
    /** @brief How many characters the level's deck has. */
    int DeckSize() const { return static_cast<int>(deck_.size()); }

    /**
     * @brief Saves the side's name and character for Cancel, reads the
     * level's deck and scrolls the list so the chosen one shows.
     */
    void Start() {
        SideSetup& side = EditedSide();
        saved_name_ = side.name;
        saved_card_ = side.card;
        // Off has no deck: its letter is past the end of "KLW"
        if (side.computer && side.level >= 0 && side.level < 3) {
            deck_ = w2::aicard::Deck(Ctx().Assets().DataDir(), side.level);
        }
        // the chosen one on the last row when it is past the first five
        for (int row = 0; row < 5; row++) rows_[row] = row < DeckSize() ? row : -1;
        if (side.card > 4) {
            for (int row = 0; row < 5; row++) rows_[row] += side.card - 4;
        }
        Refresh();
    }

    /** @brief Sets the controls' states: empty rows and scrolling past the deck are off. */
    void Refresh() {
        auto& state = dialog_view_.state;
        for (int row = 0; row < 5; row++) {
            state[kSideRow + row] = rows_[row] < 0 ? w2::uidata::kDisabled : w2::uidata::kNormal;
        }
        state[kSideOk] = state[kSideCancel] = state[kSideName] = w2::uidata::kNormal;
        bool can_up = EditedSide().computer && rows_[0] >= 1;
        bool down = EditedSide().computer && rows_[4] > 0 && rows_[4] < DeckSize() - 1;
        state[kSideUp] = state[kSidePageUp] = can_up ? w2::uidata::kNormal : w2::uidata::kDisabled;
        state[kSideDown] = state[kSidePageDown] =
                down ? w2::uidata::kNormal : w2::uidata::kDisabled;
    }

    /**
     * @brief Takes the typed name and ends editing; an empty name is not
     * taken, as the setup screen knows a side by its name.
     */
    void KeepName() {
        if (!editing_->text.empty()) EditedSide().name = editing_->text;
        editing_.reset();
    }

    /** @brief Closes the dialog; without `confirmed` the name and character are put back. */
    void Close(bool confirmed) {
        if (!confirmed) {
            EditedSide().name = saved_name_;
            EditedSide().card = saved_card_;
        }
        kit::Done done = std::move(after_);
        PopSelf();
        if (done) done();
    }

    SharedSetup setup_;                              ///< The setup the side belongs to.
    int index_ = 0;                                  ///< The side's index.
    screen::View dialog_view_;                       ///< Dialog 27 and its controls' states.
    kit::Done after_;                                ///< Called once the dialog is closed.
    std::string saved_name_;                         ///< The name to put back on Cancel.
    int saved_card_ = 0;                             ///< The character to put back on Cancel.
    LineList deck_;                                  ///< The level's characters' names.
    std::array<int, 5> rows_{{-1, -1, -1, -1, -1}};  ///< The five rows' cards, -1 for none.
    OptionalEditor editing_;                         ///< The name field, while it is typed in.
    LineList desc_;                                  ///< The chosen character's description lines.
    int desc_card_ = -1;                             ///< The card `desc_` was read for.
};

/** @brief Opens Setup Side on one side of a setup; `after` runs once it is closed. */
void OpenSide(w2::UiContext& ctx, SharedSetup setup, int index, kit::Done after) {
    kit::Push(ctx,
              std::make_shared<SideDialogModal>(ctx, std::move(setup), index, std::move(after)));
}

/** @brief A side's box on the setup screen: two columns of four. */
kit::Rect RectFor(int side) {
    return kit::Rect{side < 4 ? 24 : 208, 40 + 90 * (side % 4), 160, 70};
}

/// Where the quote's lines go, each centred.
constexpr int kQuoteY[5] = {366, 384, 402, 420, 440};

/**
 * @brief A quote from QUOTES.DAT, picked as the original picks it.
 *
 * The file is a two-digit count, then records of a three-digit length and
 * that many bytes less one, lines split by "|" and each closed by one.
 * @param roll Rolls dice(1, count): `roll(count)` says which record.
 * @return Up to five lines; only text closed by a "|" is kept.
 */
LineList Quote(const w2::UiContext& ctx, const RollCallback& roll) {
    LineList lines;
    auto file = w2::ReadFile(ctx.Assets().DataDir() / "DATA/QUOTES.DAT");
    if (!file.ok() || file->size() < 2) return lines;
    int count = std::atoi(file->substr(0, 2).c_str());
    if (count < 1) return lines;
    int pick = roll(count);
    size_t offset = 4;
    std::string text;
    for (int i = 0; i < pick && offset + 3 <= file->size(); i++) {
        int length = std::atoi(file->substr(offset, 3).c_str());
        text = length > 0 ? file->substr(offset + 3, static_cast<size_t>(length - 1))
                          : std::string();
        offset += 3 + static_cast<size_t>(length) + 1;
    }
    size_t start = 0;
    for (size_t bar; (bar = text.find('|', start)) != std::string::npos && lines.size() < 5;
         start = bar + 1) {
        lines.push_back(text.substr(start, bar - start));
    }
    return lines;
}

/** @brief The setup screen: the sides, the options, the rating and a quote. */
class SetupScreenModal final : public kit::Modal {
  public:
    SetupScreenModal(w2::UiContext& ctx, SharedSetup setup, BeginSetupCallback begin,
                     kit::Done back)
            : kit::Modal(ctx)
            , setup_(std::move(setup))
            , lines_(Quote(ctx, [&ctx](int n) { return ctx.Actions().Roll(n); }))
            , dialog_view_(kit::View(ctx, 3))
            , begin_(std::move(begin))
            , back_(std::move(back)) {
        dialog_view_.screen = true;  // a screen, not a dialog: Begin has no ring
        SetDialogView(&dialog_view_);
        SetUsesMenuBar(true);  // the menu bar stays live over it
        Refresh();
    }

    /** @brief Sets the controls' states; shows one of the two Greatest/Normal buttons. */
    void Refresh() {
        auto& state = dialog_view_.state;
        for (int i = 0; i < 8; i++) state[125 + i] = w2::uidata::kNormal;
        for (int preset = 0; preset <= 2; preset++) {
            state[145 + preset] =
                    PresetMatches(*setup_, preset) ? w2::uidata::kActive : w2::uidata::kNormal;
        }
        state[141] = state[142] = state[148] = state[143] = state[144] = w2::uidata::kNormal;
        state[157] = w2::uidata::kNormal;
        state[158] = Computers(*setup_) > 0 ? w2::uidata::kNormal : w2::uidata::kDisabled;
        HiddenControls() = {setup_->greatest ? 143 : 144};
    }

    /** @brief Draws the screen: marble, a box a side, the logo, rating and quote. */
    void Draw() override {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        screen::DrawBackground(ren, ctx.Assets().MainScreen(), layout::Compute(640, 480));
        ren.SetColor(1, 1, 1);
        const gfx::ImageP& marble = ctx.Assets().Marble();
        ren.Draw(marble, gfx::NewQuad(0, 0, 360, 360), 16, 30);
        ren.Draw(marble, gfx::NewQuad(0, 0, 224, 312), 400, 30);
        ren.Draw(marble, gfx::NewQuad(0, 60, 360, 66), 16, 403);
        ren.Draw(marble, gfx::NewQuad(0, 0, 224, 114), 400, 355);
        const Font& font = kit::GetFont(ctx, 2);
        for (int i = 0; i < 8; i++) {
            const SideSetup& side = setup_->sides[i];
            kit::Rect slot_rect = RectFor(i);
            if (side.name.empty()) continue;
            int colour = side.colour;
            int edge = side.edge;
            kit::SetPal(ctx, colour);
            kit::Outline(ctx, slot_rect.x, slot_rect.y, slot_rect.w, slot_rect.h);
            ren.Rectangle(gfx::kFill, slot_rect.x - 1, slot_rect.y - 1, slot_rect.w + 2, 1);
            ren.Rectangle(gfx::kFill, slot_rect.x - 1, slot_rect.y - 1, 1, slot_rect.h + 2);
            ren.Rectangle(gfx::kFill, slot_rect.x + 1, slot_rect.y + slot_rect.h - 2,
                          slot_rect.w - 2, 1);
            ren.Rectangle(gfx::kFill, slot_rect.x + slot_rect.w - 2, slot_rect.y + 1, 1,
                          slot_rect.h - 2);
            kit::SetPal(ctx, edge);
            ren.Rectangle(gfx::kFill, slot_rect.x + 1, slot_rect.y + 1, slot_rect.w - 2, 1);
            ren.Rectangle(gfx::kFill, slot_rect.x + 1, slot_rect.y + 1, 1, slot_rect.h - 2);
            ren.Rectangle(gfx::kFill, slot_rect.x - 1, slot_rect.y + slot_rect.h, slot_rect.w + 2,
                          1);
            ren.Rectangle(gfx::kFill, slot_rect.x + slot_rect.w, slot_rect.y - 1, 1,
                          slot_rect.h + 2);
            int name_x = slot_rect.x + 24;
            int name_y = slot_rect.y - 8;
            int width = font.Width(side.name);
            kit::SetPal(ctx, 3);
            ren.Rectangle(gfx::kFill, name_x - 4, name_y, width + 8, 20);
            ren.SetColor(1, 1, 1);
            font.Colours(colour, edge).Draw(side.name, name_x, name_y);
            // the face: Off, a human (two faces, turn about), or the
            // computer's level, another face for a character; the button the
            // same, Off and Human mapped in
            int face = FaceOf(side, i);
            Blit(kFaces[face][0], kFaces[face][1], 40, 40, slot_rect.x + 16, slot_rect.y + 16);
            int level = face >= 7 ? face - 7 : face;
            int button = (level == 6 || level == 3) ? 3 : (level >= 4 ? 4 : level);
            Blit(kLevelButton[button], 0, 80, 25, slot_rect.x + 72, slot_rect.y + 12);
            const int* check = kCheck[side.card != 0 ? 0 : 1];
            Blit(check[0], check[1], 24, 20, slot_rect.x + 64, slot_rect.y + 39);
            ren.SetColor(1, 1, 1);
            font.Colours(colour, edge).Draw("Character", slot_rect.x + 88, slot_rect.y + 41);
        }
        // the logo's (0, 0, 144, 64) at (440, 277)
        ren.SetColor(1, 1, 1);
        if (const screen::Art* logo = ctx.ControlArt(52)) {
            ren.Draw(logo->image, gfx::NewQuad(0, 0, 144, 64), 440, 277);  // SSG.PCK
        }
        kit::Centred(font, kit::Text(ctx, 7, 0), 512, 48);
        kit::Centred(font, kit::Text(ctx, 7, 1), 512, 206);
        kit::Centred(font, Format(kit::Text(ctx, 6, 0), Rating(*setup_)), 196, 426);
        for (size_t i = 0; i < lines_.size(); i++) kit::Centred(font, lines_[i], 512, kQuoteY[i]);
        kit::DrawControls(ctx, dialog_view_, HiddenControls());
    }

    /** @brief Handles a click on one of the screen's controls. */
    void Mousepressed(int x, int y, int) override {
        w2::UiContext& ctx = Ctx();
        const auto* control = kit::ControlAt(dialog_view_, x, y, HiddenControls());
        if (!control) return;
        int control_id = control->id;
        if (control_id >= 125 && control_id <= 132) {
            // a side's button: Human, Knight, Lord, Warlord, Off, and round again
            SideSetup& side = setup_->sides[control_id - 125];
            if (side.in_use) {
                if (!side.computer) {
                    side.computer = true;
                    side.level = 0;
                } else if (side.level == 3) {
                    side.computer = false;
                    side.level = 0;
                } else {
                    side.level++;
                }
                side.card = 0;  // the level's Standard character
            }
        } else if (control_id == 141) {
            int playing = 0;
            for (const auto& side : setup_->sides) {
                if (side.in_use && !(side.computer && side.level == 3)) playing++;
            }
            if (playing < 1) return;
            KeepOptions(ctx);
            BeginSetupCallback on_begin = begin_;
            SharedSetup chosen_setup = setup_;
            PopSelf();
            on_begin(std::move(chosen_setup));
            return;
        } else if (control_id == 142) {
            kit::Done on_back = back_;
            PopSelf();
            on_back();
            return;
        } else if (control_id == 143) {
            setup_->greatest = true;
            for (auto& side : setup_->sides) {
                if (side.in_use) {
                    if (!(side.computer && side.level == 2)) side.card = 0;
                    side.computer = true;
                    side.level = 2;
                }
            }
        } else if (control_id == 144) {
            setup_->greatest = false;
        } else if (control_id >= 145 && control_id <= 147) {
            for (int option = 0; option <= 5; option++) {
                setup_->options[option] = kPresets[control_id - 145][option];
            }
        } else if (control_id == 148) {
            std::weak_ptr<Modal> self = weak_from_this();
            OpenOptions(ctx, setup_, [self, this]() {
                if (!self.expired()) Refresh();
            });
            return;
        } else if ((control_id >= 133 && control_id <= 140) ||
                   (control_id >= 149 && control_id <= 156)) {
            // the Character box or the face; a side not in the scenario has
            // neither
            int side_index = control_id >= 149 ? control_id - 149 : control_id - 133;
            if (setup_->sides[side_index].in_use) {
                std::weak_ptr<Modal> self = weak_from_this();
                OpenSide(ctx, setup_, side_index, [self, this]() {
                    if (!self.expired()) Refresh();
                });
                return;
            }
        } else if (control_id == 157) {
            RecallOptions(ctx);
        } else if (control_id == 158) {
            // each computer in play gets 1d(n - 1) of its level's n
            // characters -- any but the Standard one
            for (auto& side : setup_->sides) {
                if (side.in_use && side.computer && side.level >= 0 && side.level < 3) {
                    int deck_size = static_cast<int>(
                            w2::aicard::Deck(ctx.Assets().DataDir(), side.level).size());
                    side.card = deck_size > 1 ? ctx.Actions().Roll(deck_size - 1) : 0;
                }
            }
        }
        Refresh();
    }

  private:
    /** @brief Draws a piece of SETUPBU.PCK: (source_x, source_y, width, height) at (x, y). */
    void Blit(int source_x, int source_y, int width, int height, int x, int y) {
        w2::UiContext& ctx = Ctx();
        const screen::Art* buttons = ctx.ControlArt(31);  // SETUPBU.PCK
        if (!buttons) return;
        ctx.Renderer().SetColor(1, 1, 1);
        ctx.Renderer().Draw(buttons->image, gfx::NewQuad(source_x, source_y, width, height), x, y);
    }

    SharedSetup setup_;         ///< What is being set up.
    LineList lines_;            ///< The quote, picked as the screen opens.
    screen::View dialog_view_;  ///< Dialog 3 and its controls' states.
    BeginSetupCallback begin_;  ///< Begin: starts the game.
    kit::Done back_;            ///< Main Menu: back to the start menu.
};

/**
 * @brief Opens the setup screen.
 * @param begin Called with the setup when Begin starts the game.
 * @param back Called when Main Menu goes back to the start menu.
 */
void OpenSetup(w2::UiContext& ctx, SharedSetup setup, BeginSetupCallback begin, kit::Done back) {
    kit::Push(ctx, std::make_shared<SetupScreenModal>(ctx, std::move(setup), std::move(begin),
                                                      std::move(back)));
}

constexpr kit::Rect kChooserPopupRect{80, 45, 480, 360};  ///< Popup 19, New Scenario.

/** @brief New Scenario: the list of scenarios, seven rows, and the chosen one's details. */
class ChooserModal final : public kit::Modal {
  public:
    ChooserModal(w2::UiContext& ctx, ScenarioList list, int current, PickScenarioCallback after)
            : kit::Modal(ctx)
            , list_(std::move(list))
            , dialog_view_(kit::View(ctx, 29))
            , cur_(current)
            , after_(std::move(after)) {
        SetDialogView(&dialog_view_);
        while (cur_ >= 7) {
            cur_--;
            top_row_++;
        }
        Refresh();
    }

    /** @brief Draws the list and, on the crystal ball, the chosen scenario's details. */
    void Draw() override {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        kit::PopupFrame(ctx, kChooserPopupRect);
        gfx::ImageP picture = LoadStartImage(ctx, ctx.Assets().DataDir() / "PICS/NEWSCEN.PCK");
        ren.SetColor(1, 1, 1);
        if (picture) ren.Draw(picture, kChooserPopupRect.x, kChooserPopupRect.y);
        kit::SetPal(ctx, 0);
        ren.Rectangle(gfx::kFill, 112, 153, 136, 140);
        ren.SetColor(1, 1, 1);
        const Font& font = kit::GetFont(ctx, 2);
        for (int i = 0; i <= 6; i++) {
            if (top_row_ + i < static_cast<int>(list_.size())) {
                font.Colours(i == cur_ ? 15 : 5, 0)
                        .Draw(list_[top_row_ + i].name, 112, 153 + 20 * i);
            }
        }
        if (top_row_ + cur_ < static_cast<int>(list_.size())) {
            const Scenario& scenario = list_[top_row_ + cur_];
            const Font& ink = font.Colours(5, 0);
            kit::Centred(ink, scenario.name, 416, 101);
            kit::Centred(ink, scenario.text, 416, 174);
            kit::Right(ink, absl::StrFormat("%d", scenario.cities), 368, 234);
            ink.Draw(absl::StrFormat("%d", scenario.ruins), 472, 234);
            kit::Centred(ink, absl::StrFormat("%d", scenario.players), 416, 254);
        }
        kit::ControlIdSet row_controls = {476, 477, 478, 479, 480, 481, 482};
        kit::DrawControls(ctx, dialog_view_, row_controls);
    }

    /** @brief Handles a click: OK, Cancel, a row, or scrolling a row or a page. */
    void Mousepressed(int x, int y, int) override {
        const auto* control = kit::ControlAt(dialog_view_, x, y);
        if (!control || dialog_view_.StateOf(control->id) == w2::uidata::kDisabled) return;
        int control_id = control->id;
        if (control_id == 474) {
            Take(true);
            return;
        }
        if (control_id == 475) {
            Take(false);
            return;
        }
        if (control_id >= 476 && control_id <= 482) {
            if (top_row_ + control_id - 476 < static_cast<int>(list_.size())) {
                cur_ = control_id - 476;
            }
        } else if (control_id == 470 || control_id == 472) {
            top_row_ = std::max(0, top_row_ - (control_id == 472 ? 7 : 1));
        } else if (control_id == 471 || control_id == 473) {
            top_row_ = std::min(std::max(0, static_cast<int>(list_.size()) - 7),
                                top_row_ + (control_id == 473 ? 7 : 1));
        }
        Refresh();
    }

    /** @brief Return takes the chosen scenario, Escape cancels. */
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter") {
            Take(true);
        } else if (key == "escape") {
            Take(false);
        }
    }

  private:
    /** @brief Sets the controls' states; scrolling past the list is off. */
    void Refresh() {
        auto& state = dialog_view_.state;
        for (const auto& control : dialog_view_.dialog.controls) {
            state[control.id] = w2::uidata::kNormal;
        }
        int can_up = top_row_ > 0 ? w2::uidata::kNormal : w2::uidata::kDisabled;
        int down = top_row_ + 7 < static_cast<int>(list_.size()) ? w2::uidata::kNormal
                                                                 : w2::uidata::kDisabled;
        state[470] = can_up;
        state[472] = can_up;
        state[471] = down;
        state[473] = down;
    }

    /** @brief Closes the chooser and calls `after_` with the scenario chosen, or kNone. */
    void Take(bool confirmed) {
        int pick = confirmed ? top_row_ + cur_ : w2::kNone;
        PickScenarioCallback done = std::move(after_);
        PopSelf();
        done(pick);
    }

    ScenarioList list_;           ///< Every scenario to choose from.
    screen::View dialog_view_;    ///< Dialog 29 and its controls' states.
    int top_row_ = 0;             ///< The scenario on the list's first row.
    int cur_ = 0;                 ///< The chosen row, 0-6.
    PickScenarioCallback after_;  ///< Called with the scenario chosen, or kNone.
};

/**
 * @brief Opens New Scenario with `current` chosen, scrolled so it shows.
 * @param after Called with the index of the scenario chosen, or kNone.
 */
void OpenChooser(w2::UiContext& ctx, const ScenarioList& list, int current,
                 PickScenarioCallback after) {
    kit::Push(ctx, std::make_shared<ChooserModal>(ctx, list, current, std::move(after)));
}

/// The sliders' left edge and width on the start menu.
constexpr int kSliderX = 384;
constexpr int kSliderW = 120;
constexpr kit::Rect kGeneratorToggleRect{328, 386, 264, 24};

/** @brief Returns the display label for the currently selected random map generator and shape. */
std::string GeneratorLabel(const w2::StartWorldSettings& world) {
    if (!world.modern_mapgen) return "Generator: Classic (1993)";
    switch (static_cast<w2::scengen::Shape>(std::clamp(world.shape, 0, 2))) {
    case w2::scengen::Shape::kContinents:
        return "Generator: Modern (Continents)";
    case w2::scengen::Shape::kPangaea:
        return "Generator: Modern (Pangaea)";
    case w2::scengen::Shape::kIslands:
        return "Generator: Modern (Islands)";
    }
    return "Generator: Modern (Continents)";
}

/** @brief Cycles the random map generator mode through Classic -> Continents -> Pangaea -> Islands.
 */
void CycleGeneratorMode(w2::UiContext& ctx) {
    w2::StartWorldSettings& world = ctx.StartWorld();
    if (!world.modern_mapgen) {
        world.modern_mapgen = true;
        world.shape = static_cast<int>(w2::scengen::Shape::kContinents);
        ctx.SavePref("mapgen", w2::scengen::ShapeName(w2::scengen::Shape::kContinents));
    } else if (world.shape == static_cast<int>(w2::scengen::Shape::kContinents)) {
        world.shape = static_cast<int>(w2::scengen::Shape::kPangaea);
        ctx.SavePref("mapgen", w2::scengen::ShapeName(w2::scengen::Shape::kPangaea));
    } else if (world.shape == static_cast<int>(w2::scengen::Shape::kPangaea)) {
        world.shape = static_cast<int>(w2::scengen::Shape::kIslands);
        ctx.SavePref("mapgen", w2::scengen::ShapeName(w2::scengen::Shape::kIslands));
    } else {
        world.modern_mapgen = false;
        ctx.SavePref("mapgen", "classic");
    }
}

/** @brief Draws the random world's settings, in place of the scenario's picture. */
void DrawWorld(w2::UiContext& ctx) {
    gfx::Renderer& ren = ctx.Renderer();
    const w2::StartWorldSettings& world = ctx.StartWorld();
    kit::SetPal(ctx, 3);
    ren.Rectangle(gfx::kFill, 328, 200, 264, 225);
    const Font& font = kit::GetFont(ctx, 2);
    const screen::Art* buttons = ctx.ControlArt(1);  // STARTBU.PCK
    for (int i = 0; i < 4; i++) {
        int y = 215 + 30 * i;
        ren.SetColor(1, 1, 1);
        kit::Right(font, kit::Text(ctx, 2, i), 376, y);
        // the slider at its place, or bare for "?"
        int source_y = world.set[i] ? 80 + 20 * world.sliders[i] : 220;
        if (buttons) {
            ren.Draw(buttons->image, gfx::NewQuad(496, source_y, kSliderW, 20), kSliderX, y + 1);
        }
        std::string shows = world.set[i] ? Format(w2::randommap::kSliderFormats[i],
                                                  w2::randommap::kSliderShows[i][world.sliders[i]])
                                         : "(?)";
        font.Draw(shows, 504, y);
    }
    font.Draw(w2::randommap::TerrainSetName(ctx.Assets().DataDir(), world.terrain_set), 336, 340);
    font.Draw(kit::Text(ctx, 3, world.allies ? 1 : 0), 336, 365);
    font.Draw(GeneratorLabel(world), 336, 390);
}

constexpr kit::Rect kMakeWorldPopupRect{160, 55, 336, 347};  ///< Popup 23, random world scroll.

/**
 * @brief Makes the random world, showing its progress on popup 23, then
 * hands it on.
 *
 * The scroll shows group 136's lines, the bar RMAPBAR.PCK at (232, 257)
 * grows a tenth at a time, the percentage over it. A "?" slider is rolled,
 * 1d7-1.
 */
class MakeWorldModal final : public kit::Modal {
  public:
    MakeWorldModal(w2::UiContext& ctx, kit::Done after)
            : kit::Modal(ctx)
            , rng_(static_cast<double>(std::time(nullptr) % 1000000007))
            , after_(std::move(after))
            , scroll_(LoadStartImage(ctx, ctx.Assets().DataDir() / "PICS/BSCROLL.PCK", 10))
            , bar_(LoadStartImage(ctx, ctx.Assets().DataDir() / "PICS/RMAPBAR.PCK", 10)) {}

    /** @brief Initializes `run_` bound to `this->rng_`; returns false and shows error on failure.
     */
    bool StartGeneration() {
        w2::UiContext& ctx = Ctx();
        const w2::StartWorldSettings& world = ctx.StartWorld();
        w2::randommap::Options options;
        options.data_dir = ctx.Assets().DataDir();
        options.rng = &rng_;
        for (int i = 0; i < 4; i++) {
            options.sliders[i] = world.set[i] ? world.sliders[i] : w2::randommap::kRandomSlider;
        }
        options.allies = world.allies;
        options.terrain_set = world.terrain_set;
        options.scenario = world.modern_mapgen;
        if (world.modern_mapgen) {
            options.sliders = w2::randommap::Settle(options.sliders, rng_);
            auto shape = static_cast<w2::scengen::Shape>(std::clamp(world.shape, 0, 2));
            options.world = w2::randommap::SettingsFromSliders(options.sliders, shape);
        }
        auto created_run = w2::randommap::Run::Create(options);
        if (!created_run.ok()) {
            ctx.Actions().Say(std::string(created_run.status().message()));
            return false;
        }
        run_ = std::move(*created_run);
        return true;
    }

    /** @brief Runs a phase of the generator a frame, so the bar moves as it works. */
    void Update() override {
        if (run_ && run_->Step()) return;
        if (run_) Ctx().StartWorld().generated_files = run_->GetFiles();
        kit::Done done = std::move(after_);
        PopSelf();
        if (done) done();
    }

    /** @brief Draws the scroll, its two lines, and the progress bar. */
    void Draw() override {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        ren.SetColor(1, 1, 1);
        if (scroll_) {
            ren.Draw(scroll_, gfx::NewQuad(0, 0, kMakeWorldPopupRect.w, kMakeWorldPopupRect.h),
                     kMakeWorldPopupRect.x, kMakeWorldPopupRect.y);
        }
        const Font& font = kit::GetFont(ctx, 2).Colours(0, 7);
        const int lines[2] = {2, 3};
        for (int i = 0; i < 2; i++) {
            kit::Centred(font, kit::Text(ctx, 0x88, lines[i]), 328, 181 + 20 * i);
        }
        // a black frame, the scroll showing through it
        kit::SetPal(ctx, 0);
        ren.Rectangle(gfx::kLine, 232.5, 255.5, 191, 24);
        ren.SetColor(1, 1, 1);
        int percent = run_ ? run_->Progress() : 100;
        int width = (percent + 10) / 10 * 16 + 16;
        if (bar_) ren.Draw(bar_, gfx::NewQuad(0, 0, width, 21), 232, 257);
        kit::Centred(font, absl::StrFormat("%d%%", percent), 328, 237);
    }

  private:
    w2::Rng rng_;         ///< The dice the world is made with.
    RunHandle run_;       ///< The generator, a phase at a time.
    kit::Done after_;     ///< Called once the world is installed.
    gfx::ImageP scroll_;  ///< BSCROLL.PCK.
    gfx::ImageP bar_;     ///< RMAPBAR.PCK.
};

/** @brief Starts making the random world with the menu's settings; `after` runs once made. */
void MakeWorld(w2::UiContext& ctx, kit::Done after) {
    auto dialog = std::make_shared<MakeWorldModal>(ctx, std::move(after));
    if (!dialog->StartGeneration()) return;
    kit::Push(ctx, std::move(dialog));
}

/** @brief Builds `w2::game::NewGameOptions` from a completed `Setup`. */
w2::game::NewGameOptions BuildNewGameOptions(w2::UiContext& ctx, const Setup& chosen_setup,
                                             const std::string& chosen_dir) {
    w2::game::NewGameOptions options;
    for (int i = 0; i <= 9; i++) {
        options.options.emplace_back(kOptionKeys[i], chosen_setup.options[i]);
    }
    for (int i = 0; i < 8; i++) {
        const SideSetup& side = chosen_setup.sides[i];
        w2::game::SideSetup game_side;
        game_side.computer = side.computer;
        game_side.level = side.level;
        game_side.card = side.card;
        game_side.name = side.name;
        game_side.off = side.computer && side.level == 3;
        options.sides[i] = game_side;
    }
    options.greatest = chosen_setup.greatest;
    const auto& generated = ctx.StartWorld().generated_files;
    if (chosen_dir == w2::randommap::kDir && !generated.empty()) {
        options.memory_files = &generated;
    }
    return options;
}

/** @brief The start menu the game opens on. */
class MenuModal final : public kit::Modal {
  public:
    MenuModal(w2::UiContext& ctx, BeginScenarioCallback begin_game, LoadedGameCallback loaded)
            : kit::Modal(ctx)
            , list_(Scenarios(ctx))
            , dialog_view_(kit::View(ctx, 1))
            , begin_game_(std::move(begin_game))
            , loaded_(std::move(loaded)) {
        for (size_t i = 0; i < list_.size(); i++) {
            if (list_[i].dir == "Erythea") cur_ = static_cast<int>(i);
        }
        if (ctx.Scenario() == w2::randommap::kDir) ctx.StartWorld().enabled = true;
        dialog_view_.screen = true;  // a screen, not a dialog: Begin has no ring
        SetDialogView(&dialog_view_);
        SetUsesMenuBar(true);
        Refresh();
    }

    /** @brief Draws the menu: its four pictures, and the scenario or the random world. */
    void Draw() override {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        const w2::StartWorldSettings& world = ctx.StartWorld();
        ren.SetColor(1, 1, 1);
        for (int quarter = 0; quarter < 4; quarter++) {
            gfx::ImageP image =
                    LoadStartImage(ctx, ctx.Assets().DataDir() / "PICS" /
                                                ("STARTUP" + std::to_string(quarter) + ".PCK"));
            if (image) ren.Draw(image, (quarter % 2) * 320, (quarter / 2) * 240);
        }
        bool have = cur_ < static_cast<int>(list_.size());
        if (world.enabled) {
            DrawWorld(ctx);
        } else if (have) {
            std::string dir = w2::Upper(list_[cur_].dir);
            if (gfx::ImageP picture =
                        LoadStartImage(ctx, ctx.Assets().DataDir() / dir / "PICS/SCENARIO.PCK")) {
                ren.SetColor(1, 1, 1);
                ren.Draw(picture, gfx::NewQuad(0, 0, 264, 225), 328, 200);
            }
        }
        if (world.enabled || have) {
            // the scenario's name, or "A Random World"
            kit::SetPal(ctx, 3);
            ren.Rectangle(gfx::kFill, 336, 166, 248, 28);
            ren.SetColor(1, 1, 1);
            kit::Centred(kit::GetFont(ctx, 2),
                         world.enabled ? kit::Text(ctx, 0, 0) : list_[cur_].name, 460, 172);
        }
        kit::DrawControls(ctx, dialog_view_, HiddenControls());
    }

    /** @brief Handles a click on one of the menu's controls. */
    void Mousepressed(int x, int y, int) override {
        w2::UiContext& ctx = Ctx();
        w2::StartWorldSettings& world = ctx.StartWorld();
        if (world.enabled && kGeneratorToggleRect.Contains(x, y)) {
            CycleGeneratorMode(ctx);
            Refresh();
            return;
        }
        const auto* control = kit::ControlAt(dialog_view_, x, y, HiddenControls());
        if (!control || dialog_view_.StateOf(control->id) == w2::uidata::kDisabled) return;
        int control_id = control->id;
        std::weak_ptr<Modal> self = weak_from_this();
        if (control_id == 100) {
            // a scenario chosen ends the random world
            OpenChooser(ctx, list_, cur_, [self, this](int chosen) {
                if (self.expired()) return;
                if (chosen != w2::kNone) {
                    cur_ = chosen;
                    Ctx().StartWorld().enabled = false;
                }
                Refresh();
            });
        } else if (control_id == 101) {
            LoadedGameCallback on_loaded = loaded_;
            savegame::Load(ctx, [self, on_loaded](std::unique_ptr<w2::Game> game) {
                if (auto alive = self.lock()) alive->PopSelf();
                on_loaded(std::move(game));
            });
        } else if (control_id == 102) {
            world.enabled = true;
        } else if (control_id == 104) {
            world.terrain_set =
                    (world.terrain_set + 1) % w2::randommap::TerrainSets(ctx.Assets().DataDir());
        } else if (control_id == 105) {
            world.allies = !world.allies;
        } else if (control_id >= 106 && control_id <= 109) {
            // a "?" slider is set again where it was; a set one moves to
            // where it was clicked
            int slider = control_id - 106;
            if (!world.set[slider]) {
                world.set[slider] = true;
            } else {
                world.sliders[slider] = std::clamp((x - kSliderX) * 7 / kSliderW, 0, 6);
            }
        } else if (control_id >= 110 && control_id <= 113) {
            world.set[control_id - 110] = false;
        } else if (control_id == 103 && world.enabled) {
            BeginRandomWorld();
        } else if (control_id == 103 && cur_ < static_cast<int>(list_.size())) {
            auto keep = shared_from_this();
            PopSelf();
            SetUp(list_[cur_]);
            return;
        }
        Refresh();
    }

    /** @brief Shows right-click help for the custom Generator toggle in the Random Map panel. */
    void Rightpressed(int frame_x, int frame_y, int screen_x, int screen_y) override {
        w2::UiContext& ctx = Ctx();
        if (ctx.StartWorld().enabled && kGeneratorToggleRect.Contains(frame_x, frame_y)) {
            infobox::ShowLines(ctx, screen_x, screen_y, "Map Generator",
                               "Switch Classic 1993 or Modern world");
        }
    }

  private:
    /** @brief Sets the controls' states; the random world's controls show only for it. */
    void Refresh() {
        const w2::StartWorldSettings& world = Ctx().StartWorld();
        auto& state = dialog_view_.state;
        state[100] = state[101] = state[103] = w2::uidata::kNormal;
        state[102] = world.enabled ? w2::uidata::kDisabled : w2::uidata::kNormal;
        HiddenControls().clear();
        if (!world.enabled) {
            for (int control_id = 104; control_id <= 113; control_id++) {
                HiddenControls().insert(control_id);
            }
            return;
        }
        state[104] = state[105] = w2::uidata::kNormal;
        for (int i = 0; i < 4; i++) {
            state[106 + i] = w2::uidata::kNormal;
            state[110 + i] =
                    world.set[i] ? w2::uidata::kNormal : w2::uidata::kActive;  // lit while "?"
        }
    }

    /** @brief Speaks `"One moment..."`, generates the random world, and opens Setup. */
    void BeginRandomWorld() {
        std::weak_ptr<Modal> self = weak_from_this();
        advisor::Say(Ctx(), w2::cues::kMoment, [self, this]() {
            if (self.expired()) return;
            StartWorldGeneration();
        });
    }

    /** @brief Launches `MakeWorld` and transitions to `SetUp` once complete. */
    void StartWorldGeneration() {
        std::weak_ptr<Modal> self = weak_from_this();
        MakeWorld(Ctx(), [self, this]() {
            auto keep = self.lock();
            if (!keep) return;
            PopSelf();
            Scenario scenario;
            scenario.name = kit::Text(Ctx(), 0, 0);
            scenario.dir = w2::randommap::kDir;
            SetUp(scenario);
        });
    }

    /**
     * @brief Opens the setup screen on a scenario. Begin there starts the
     * game; Main Menu brings this menu back.
     */
    void SetUp(const Scenario& scenario) {
        w2::UiContext& ctx = Ctx();
        auto keep = shared_from_this();
        SharedSetup setup = NewSetup(ctx, scenario);
        BeginScenarioCallback on_begin = begin_game_;
        OpenSetup(
                ctx, std::move(setup),
                [&ctx, on_begin](SharedSetup chosen_setup) {
                    std::string chosen_dir = w2::Upper(chosen_setup->scenario.dir);
                    w2::game::NewGameOptions options =
                            BuildNewGameOptions(ctx, *chosen_setup, chosen_dir);
                    on_begin(chosen_dir, options);
                },
                [keep]() {
                    auto menu = std::static_pointer_cast<MenuModal>(keep);
                    menu->Refresh();
                    kit::Push(menu->Ctx(), keep);
                });
    }

    ScenarioList list_;                 ///< Every scenario to choose from.
    screen::View dialog_view_;          ///< Dialog 1 and its controls' states.
    int cur_ = 0;                       ///< The scenario chosen, an index into `list_`.
    BeginScenarioCallback begin_game_;  ///< Starts a new game in a scenario directory.
    LoadedGameCallback loaded_;         ///< Takes a loaded game.
};

}  // namespace

/**
 * @brief Opens the start menu on Erythea, or on the random world when coming
 * back from one.
 * @param begin Called to start a new game set up on the start screens.
 * @param loaded Called with a saved game loaded from the menu.
 */
void Open(w2::UiContext& ctx, BeginScenarioCallback begin, LoadedGameCallback loaded) {
    kit::Push(ctx, std::make_shared<MenuModal>(ctx, std::move(begin), std::move(loaded)));
}

}  // namespace start
