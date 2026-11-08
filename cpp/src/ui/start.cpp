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
#include <cstdlib>
#include <ctime>
#include <map>
#include <random>

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
/// The ten options' keys for the game, in the order of string group 4.
const char* const kOptionKeys[10] = {
    "neutralCities",  "diplomacy",     "quests",     "hiddenMap",       "viewEnemies",
    "viewProduction", "intenseCombat", "quickStart", "militaryAdvisor", "randomTurns"};
/// The three presets (Beginner, Intermediate, Advanced), one value per option.
const int kPresets[3][10] = {
    {0, 0, 0, 0, 1, 0, 0, 0, 1, 0},
    {1, 1, 1, 0, 0, 0, 0, 0, 1, 0},
    {2, 1, 1, 1, 0, 1, 0, 0, 1, 0},
};

/**
 * @brief The random world's settings, kept for the session as the original
 * keeps them.
 */
struct World {
    bool on = false;      ///< Whether the menu is on a random world, not a scenario.
    int terrain_set = 0;  ///< Which terrain set the world is made with.
    bool allies = false;  ///< Whether cities can produce allies.
    std::array<int, 4> sliders{{3, 3, 2, 3}};           ///< Water, Hills, Cities, Forest: 0-6.
    std::array<bool, 4> set{{true, true, true, true}};  ///< Else "?": left to chance.
};
World world;

/// The options being set up: one table for the session, the Beginner preset to begin with.
int remembered_options[10] = {0, 0, 0, 0, 1, 0, 0, 0, 1, 0};

/**
 * @brief Puts back the options the last game began with: ten u16s in
 * DATA\OPTIONS.DAT.
 *
 * Used as the setup screen opens and by Recall Options. With no file the
 * table is left as it is.
 */
void RecallOptions() {
    auto file = w2::ReadFile(G.data_dir + "/DATA/OPTIONS.DAT");
    if (!file) return;
    for (size_t i = 0; i <= 9 && 2 * i + 2 <= file->size(); i++)
        remembered_options[i] = w2::U16(*file, 2 * i);
}

/** @brief Keeps the options for next time in DATA\OPTIONS.DAT, as Begin starts the game. */
void KeepOptions() {
    std::string bytes;
    for (int i = 0; i <= 9; i++) {
        bytes += static_cast<char>(remembered_options[i] & 255);
        bytes += static_cast<char>((remembered_options[i] >> 8) & 255);
    }
    w2::WriteFile(G.data_dir + "/DATA/OPTIONS.DAT", bytes);
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

/**
 * @brief SCENARIO.DAT's records, and after them the worlds the scenario
 * generator made (not the original's; see warlords/scenarios and
 * docs/scengen.md).
 */
std::vector<Scenario> Scenarios() {
    std::vector<Scenario> out;
    for (auto& entry : w2::scenarios::List(G.data_dir))
        out.push_back(Scenario{entry.name, entry.dir, entry.text, entry.cities, entry.ruins,
                               entry.players});
    return out;
}

/// Pictures loaded so far, by path; a picture that failed to load is kept as null.
std::map<std::string, gfx::ImageP> art;

/** @brief Loads a .PCK picture once and keeps it; null when it can't be loaded. */
gfx::ImageP Image(const std::string& path, int key = pckimage::kNokey) {
    auto cached = art.find(path);
    if (cached != art.end()) return cached->second;
    gfx::ImageP image;
    try {
        image = pckimage::Load(path, G.palette, key);
    } catch (...) {
    }
    return art[path] = image;
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
    Scenario sc;                        ///< The scenario.
    int* options = remembered_options;  ///< The ten options, the session's table.
    SideSetup sides[8];                 ///< The eight sides.
    bool greatest = false;              ///< "I am the Greatest": every side a Warlord.
};

/**
 * @brief A new setup for a scenario: its sides as the scenario has them.
 *
 * The tutorial plays with the Beginner options, any other scenario with the
 * last game's.
 */
std::shared_ptr<Setup> NewSetup(const Scenario& scenario) {
    std::string dir = w2::Upper(scenario.dir);
    auto map = w2::scn::Load(G.data_dir + "/" + dir, dir);
    auto setup = std::make_shared<Setup>();
    setup->sc = scenario;
    if (map->options.tutorial != 0)
        for (int i = 0; i <= 9; i++) remembered_options[i] = kPresets[0][i];
    else
        RecallOptions();
    for (auto& side : map->sides) {
        SideSetup& side_setup = setup->sides[side.index];
        side_setup.in_use = side.in_use;
        side_setup.name = side.name;
        side_setup.colour = side.colour;
        side_setup.edge = side.edge;
        side_setup.computer = side.computer;
        side_setup.level = side.computer ? side.level : 0;
        side_setup.card = side.card;
    }
    return setup;
}

/** @brief Whether the first six options are those of a preset. */
bool PresetMatches(const Setup& setup, int preset) {
    for (int i = 0; i <= 5; i++)
        if (setup.options[i] != kPresets[preset][i]) return false;
    return true;
}

/** @brief The difficulty rating, a percent: the options' weight and the computers' strength. */
int Rating(const Setup& setup) {
    const int* options = setup.options;
    int options_weight = std::min(20, options[0] * 4 + options[1] * 4 + options[2] * 3 +
                                              options[3] * 4 - options[4] + options[5]);
    int sum = 0, computers = 0;
    for (auto& side : setup.sides) {
        if (side.in_use && side.computer && side.level != 3) {
            sum += side.level + 1;
            computers++;
        }
    }
    int strength = computers == 0 ? 80 : sum * 80 / (computers * 3);
    if (strength >= 78) strength = 80;
    return strength + options_weight;
}

/** @brief Edit Options: popup 4, dialog 4, the ten options and the presets. */
struct Options : kit::Modal {
    std::shared_ptr<Setup> st;  ///< The setup whose options are edited.
    screen::View v;             ///< Dialog 4 and its controls' states.
    kit::Done after;            ///< Called once the dialog is closed.

    /** @brief Sets the controls' states; a preset that matches is lit. */
    void Refresh() {
        auto& state = v.state;
        for (int i = 0; i <= 9; i++) state[159 + i] = w2::uidata::kNormal;
        for (int preset = 0; preset <= 2; preset++)
            state[169 + preset] =
                    PresetMatches(*st, preset) ? w2::uidata::kActive : w2::uidata::kNormal;
        state[172] = w2::uidata::kNormal;
    }

    /** @brief Draws the dialog: two headed sections and each option with its value. */
    void Draw() override {
        const kit::Rect popup_rect{120, 50, 400, 360};  // popup 4
        kit::Popup(popup_rect);
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), kit::Text(8, 0), 320, 55);
        const Font& font = kit::GetFont(2);
        const int section_ys[2] = {111, 250};
        for (int section = 0; section < 2; section++) {
            int y = section_ys[section];
            kit::SetPal(15);
            gfx::Rectangle(gfx::kFill, 160, y, 320, 1);
            kit::SetPal(0);
            gfx::Rectangle(gfx::kFill, 161, y + 1, 320, 1);
            std::string heading = kit::Text(9 + section, 0);
            int width = font.Width(heading);
            kit::SetPal(3);
            gfx::Rectangle(gfx::kFill, 320 - width / 2 - 4, y, width + 8, 2);
            gfx::SetColor(1, 1, 1);
            kit::Centred(font, heading, 320, y - 7);
        }
        for (int i = 0; i <= 9; i++) {
            int x = i % 2 == 0 ? 128 : 320;
            int y = i < 6 ? 131 + 30 * (i / 2) : 270 + 30 * ((i - 6) / 2);
            gfx::SetColor(1, 1, 1);
            font.Draw(kit::Text(4, i), x, y);
            int value = st->options[i];
            // View Production's word is the other way round: its flag hides
            // other sides' production, so set it reads "Off"
            bool enabled = i == 5 ? value == 0 : value != 0;
            std::string word = i == 0 ? kit::Text(5, value) : (enabled ? "On" : "Off");
            font.Colours(7, 0).Draw(word, x + 128, y);
        }
        kit::DrawControls(v);
    }

    /** @brief Closes the dialog and calls `after`. */
    void Finish() {
        auto done = after;
        kit::Pop(this);
        if (done) done();
    }

    /**
     * @brief Handles a click: OK, an option (Neutral Cities goes round three
     * values, the rest toggle), or a preset.
     */
    void Mousepressed(int x, int y, int) override {
        auto control = kit::ControlAt(v, x, y);
        if (!control) return;
        int control_id = control->id;
        if (control_id == 172) return Finish();
        if (control_id >= 159 && control_id <= 168) {
            int option = control_id - 159;
            if (option == 0)
                st->options[0] = (st->options[0] + 1) % 3;
            else
                st->options[option] = st->options[option] != 0 ? 0 : 1;
        } else if (control_id >= 169 && control_id <= 171) {
            for (int option = 0; option <= 5; option++)
                st->options[option] = kPresets[control_id - 169][option];
        }
        Refresh();
    }

    /** @brief Return and Escape close the dialog. */
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") Finish();
    }
};

/** @brief Opens Edit Options on a setup; `after` runs once it is closed. */
void OpenOptions(std::shared_ptr<Setup> setup, kit::Done after) {
    auto dialog = std::make_shared<Options>();
    dialog->st = setup;
    dialog->after = after;
    dialog->v = kit::View(4);
    dialog->view = &dialog->v;
    dialog->Refresh();
    kit::Push(dialog);
}

/**
 * @brief SETUPBU.PCK's faces: Knight, Lord, Warlord, Off, the two humans, a
 * side not in the scenario, then a Knight, Lord and Warlord playing a
 * character other than the Standard one.
 */
const int kFaces[10][2] = {{0, 0},   {0, 40},   {0, 80},    {0, 120},   {0, 160},
                           {0, 200}, {440, 40}, {424, 100}, {424, 140}, {424, 180}};
/// SETUPBU.PCK's level buttons' x: Knight, Lord, Warlord, Off, Human.
const int kLevelButton[5] = {120, 200, 280, 360, 40};
/**
 * @brief The Character box in SETUPBU.PCK: ticked with a character chosen,
 * else empty; 24 x 20 at (x + 64, y + 39), its label at (x + 88, y + 41).
 */
const int kCheck[2][2] = {{440, 0}, {440, 20}};

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
    for (auto& side : setup.sides)
        if (side.in_use && side.computer && side.level != 3) count++;
    return count;
}

// Setup Side's controls (dialog 27)
const int kSideOk = 457, kSideCancel = 458, kSideName = 459;
const int kSideUp = 460, kSideDown = 461, kSidePageUp = 462, kSidePageDown = 463, kSideRow = 464;
const kit::Rect kNameBox{229, 138, 188, 22};
/// The colour a Knight's characters are listed in, a Lord's and a Warlord's.
const int kLevelInk[3] = {5, 7, 9};

/**
 * @brief Setup Side: popup 4, dialog 27.
 *
 * A side's name, retyped in the field (15 characters, 128 pixels); for a
 * computer its level's characters, five rows at a time, and the chosen one's
 * description from its .DSC. A human has no character: "N/A". OK keeps what
 * was done, Cancel puts the name and character back.
 */
struct SideDialog : kit::Modal {
    std::shared_ptr<Setup> st;             ///< The setup the side belongs to.
    int index = 0;                         ///< The side's index.
    screen::View v;                        ///< Dialog 27 and its controls' states.
    kit::Done after;                       ///< Called once the dialog is closed.
    std::string saved_name;                ///< The name to put back on Cancel.
    int saved_card = 0;                    ///< The character to put back on Cancel.
    std::vector<std::string> deck;         ///< The level's characters' names.
    int rows[5] = {-1, -1, -1, -1, -1};    ///< The five rows' cards, -1 for none.
    std::optional<input::Editor> editing;  ///< The name field, while it is typed in.
    std::vector<std::string> desc;         ///< The chosen character's description lines.
    int desc_card = -1;                    ///< The card `desc` was read for.

    /** @brief The side being set up. */
    SideSetup& EditedSide() { return st->sides[index]; }
    /** @brief How many characters the level's deck has. */
    int DeckSize() const { return static_cast<int>(deck.size()); }

    /**
     * @brief Saves the side's name and character for Cancel, reads the
     * level's deck and scrolls the list so the chosen one shows.
     */
    void Start() {
        SideSetup& side = EditedSide();
        saved_name = side.name;
        saved_card = side.card;
        // Off has no deck: its letter is past the end of "KLW"
        if (side.computer && side.level >= 0 && side.level < 3)
            deck = w2::aicard::Deck(G.data_dir, side.level);
        // the chosen one on the last row when it is past the first five
        for (int row = 0; row < 5; row++) rows[row] = row < DeckSize() ? row : -1;
        if (side.card > 4)
            for (int row = 0; row < 5; row++) rows[row] += side.card - 4;
        Refresh();
    }

    /** @brief Sets the controls' states: empty rows and scrolling past the deck are off. */
    void Refresh() {
        auto& state = v.state;
        for (int row = 0; row < 5; row++)
            state[kSideRow + row] = rows[row] < 0 ? w2::uidata::kDisabled : w2::uidata::kNormal;
        state[kSideOk] = state[kSideCancel] = state[kSideName] = w2::uidata::kNormal;
        bool can_up = EditedSide().computer && rows[0] >= 1;
        bool down = EditedSide().computer && rows[4] > 0 && rows[4] < DeckSize() - 1;
        state[kSideUp] = state[kSidePageUp] = can_up ? w2::uidata::kNormal : w2::uidata::kDisabled;
        state[kSideDown] = state[kSidePageDown] =
                down ? w2::uidata::kNormal : w2::uidata::kDisabled;
    }

    /**
     * @brief Takes the typed name and ends editing; an empty name is not
     * taken, as the setup screen knows a side by its name.
     */
    void KeepName() {
        if (!editing->text.empty()) EditedSide().name = editing->text;
        editing.reset();
    }

    /** @brief Closes the dialog; without `confirmed` the name and character are put back. */
    void Close(bool confirmed) {
        if (!confirmed) {
            EditedSide().name = saved_name;
            EditedSide().card = saved_card;
        }
        auto done = after;
        kit::Pop(this);
        if (done) done();
    }

    /** @brief Draws the dialog: the name field, the list of characters and the description. */
    void Draw() override {
        const kit::Rect popup_rect{120, 50, 400, 360};  // popup 4
        const SideSetup& side = EditedSide();
        kit::Popup(popup_rect);
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), "Setup Side", 320, 55);
        int colour = side.colour, edge = side.edge;
        const Font& font = kit::GetFont(2);
        // two boxes, each outlined in the side's edge colour and twice more in
        // its colour, a pixel further up and left each time
        const int boxes[2][2] = {{107, 67}, {194, 165}};
        for (auto& box : boxes) {
            kit::SetPal(edge);
            kit::Outline(138, box[0], 368, box[1]);
            kit::SetPal(colour);
            kit::Outline(137, box[0] - 1, 368, box[1]);
            kit::Outline(136, box[0] - 2, 368, box[1]);
        }
        const std::pair<const char*, int> tabs[2] = {{"Side Name", 96}, {"Leader", 181}};
        for (auto& tab : tabs) {
            kit::SetPal(3);
            gfx::Rectangle(gfx::kFill, 160, tab.second, font.Width(tab.first), 17);
            gfx::SetColor(1, 1, 1);
            font.Colours(colour, edge).Draw(tab.first, 160, tab.second);
        }
        kit::Shield(index, 144, 122);
        kit::Shield(index, 456, 122);
        kit::Shield(index, 144, 202);
        gfx::SetColor(1, 1, 1);
        kit::Centred(font, "Retype the name of this side", 320, 116);
        kit::Field(kNameBox.x, kNameBox.y, kNameBox.w, kNameBox.h,
                   editing ? editing->text : side.name, &font);
        if (editing) editing->DrawCursor(kNameBox.x, kNameBox.y);
        kit::SetPal(0);
        gfx::Rectangle(gfx::kFill, 196, 218, 72, 1);
        gfx::Rectangle(gfx::kFill, 384, 218, 96, 1);
        gfx::SetColor(1, 1, 1);
        font.Draw("Name", 196, 203);
        font.Draw("Description", 384, 203);
        // the list: a sunk box, the chosen character in white
        kit::SetPal(3);
        gfx::Rectangle(gfx::kFill, 189, 249, 186, 100);
        kit::Bevel(188, 248, 188, 102, 4, 2);
        gfx::SetColor(1, 1, 1);
        if (!side.computer) {
            font.Draw("N/A", 196, 224);
            font.Draw("N/A", 384, 224);
        } else {
            const Font& ink =
                    font.Colours(side.level >= 0 && side.level < 3 ? kLevelInk[side.level] : 15, 0);
            for (int row = 0; row < 5 && rows[row] >= 0; row++) {
                const std::string& name = rows[row] < DeckSize() ? deck[rows[row]] : std::string();
                (rows[row] == side.card ? font : ink).Draw(name, 196, 252 + 19 * row);
            }
            // the name, and the .DSC's next six lines
            if (side.card >= 0 && side.card < DeckSize()) ink.Draw(deck[side.card], 192, 224);
            if (desc_card != side.card) {
                desc_card = side.card;
                desc.clear();
                if (side.level >= 0 && side.level < 3)
                    if (auto description = w2::aicard::Describe(G.data_dir, side.level, side.card))
                        desc = description->second;
            }
            for (int line = 0; line < 6 && line < static_cast<int>(desc.size()); line++)
                if (!desc[line].empty()) ink.Draw(desc[line], 384, 224 + 20 * line);
        }
        kit::DrawControls(v);
    }

    /**
     * @brief Handles a click: OK, Cancel, the name field, scrolling the list
     * (a row run off the deck is left empty) or choosing a row's character.
     */
    void Mousepressed(int x, int y, int) override {
        if (editing) KeepName();
        auto control = kit::ControlAt(v, x, y);
        if (!control) return;
        int control_id = control->id;
        if (control_id == kSideOk) return Close(true);
        if (control_id == kSideCancel) return Close(false);
        if (control_id == kSideName) {
            editing = input::Editor{};
            editing->max_chars = 15;
            editing->max_width = 128;
        } else if (control_id == kSideUp || control_id == kSideDown) {
            int step = control_id == kSideUp ? -1 : 1;
            for (int row = 0; row < 5; row++) {
                rows[row] += step;
                if (rows[row] < 0 || rows[row] >= DeckSize()) rows[row] = -1;
            }
        } else if (control_id == kSidePageUp || control_id == kSidePageDown) {
            // five rows, or as many as there are
            int step = control_id == kSidePageUp ? -std::min(rows[0], 5)
                                                 : std::min(DeckSize() - rows[4] - 1, 5);
            rows[0] += step;
            for (int row = 1; row < 5; row++) rows[row] = rows[row - 1] + 1;
        } else if (control_id >= kSideRow && control_id < kSideRow + 5) {
            int card = rows[control_id - kSideRow];
            if (card >= 0) EditedSide().card = card;
        }
        Refresh();
    }

    /** @brief Keys go to the name field while it is edited; else Return is OK, Escape Cancel. */
    void Keypressed(const std::string& key) override {
        if (editing) {
            std::string result = editing->Key(key);
            if (result == "keep")
                KeepName();
            else if (result == "undo")
                editing.reset();
            return;
        }
        if (key == "return" || key == "kpenter")
            Close(true);
        else if (key == "escape")
            Close(false);
    }

    /** @brief Typed text goes to the name field while it is edited. */
    void Textinput(const std::string& text) override {
        if (editing) editing->Input(text);
    }
    bool Typing() const override { return editing.has_value(); }
};

/** @brief Opens Setup Side on one side of a setup; `after` runs once it is closed. */
void OpenSide(std::shared_ptr<Setup> setup, int index, kit::Done after) {
    auto dialog = std::make_shared<SideDialog>();
    dialog->st = setup;
    dialog->index = index;
    dialog->after = after;
    dialog->v = kit::View(27);
    dialog->view = &dialog->v;
    dialog->Start();
    kit::Push(dialog);
}

/** @brief A side's box on the setup screen: two columns of four. */
kit::Rect RectFor(int side) {
    return kit::Rect{side < 4 ? 24 : 208, 40 + 90 * (side % 4), 160, 70};
}

/// Where the quote's lines go, each centred.
const int kQuoteY[5] = {366, 384, 402, 420, 440};

/**
 * @brief A quote from QUOTES.DAT, picked as the original picks it.
 *
 * The file is a two-digit count, then records of a three-digit length and
 * that many bytes less one, lines split by "|" and each closed by one.
 * @param roll Rolls dice(1, count): `roll(count)` says which record.
 * @return Up to five lines; only text closed by a "|" is kept.
 */
std::vector<std::string> Quote(const std::function<int(int)>& roll) {
    std::vector<std::string> lines;
    auto file = w2::ReadFile(G.data_dir + "/DATA/QUOTES.DAT");
    if (!file || file->size() < 2) return lines;
    int count = atoi(file->substr(0, 2).c_str());
    if (count < 1) return lines;
    int pick = roll(count);
    size_t offset = 4;
    std::string text;
    for (int i = 0; i < pick && offset + 3 <= file->size(); i++) {
        int length = atoi(file->substr(offset, 3).c_str());
        text = length > 0 ? file->substr(offset + 3, length - 1) : std::string();
        offset += 3 + length + 1;
    }
    size_t start = 0;
    for (size_t bar; (bar = text.find('|', start)) != std::string::npos && lines.size() < 5;
         start = bar + 1) {
        lines.push_back(text.substr(start, bar - start));
    }
    return lines;
}

/** @brief The setup screen: the sides, the options, the rating and a quote. */
struct SetupScreen : kit::Modal {
    std::shared_ptr<Setup> st;                          ///< What is being set up.
    std::vector<std::string> lines;                     ///< The quote, picked as the screen opens.
    screen::View v;                                     ///< Dialog 3 and its controls' states.
    std::function<void(std::shared_ptr<Setup>)> begin;  ///< Begin: starts the game.
    kit::Done back;                                     ///< Main Menu: back to the start menu.

    /** @brief Sets the controls' states; shows one of the two Greatest/Normal buttons. */
    void Refresh() {
        auto& state = v.state;
        for (int i = 0; i < 8; i++) state[125 + i] = w2::uidata::kNormal;
        for (int preset = 0; preset <= 2; preset++)
            state[145 + preset] =
                    PresetMatches(*st, preset) ? w2::uidata::kActive : w2::uidata::kNormal;
        state[141] = state[142] = state[148] = state[143] = state[144] = w2::uidata::kNormal;
        state[157] = w2::uidata::kNormal;
        state[158] = Computers(*st) > 0 ? w2::uidata::kNormal : w2::uidata::kDisabled;
        hidden = {st->greatest ? 143 : 144};
    }

    /** @brief Draws a piece of SETUPBU.PCK: (source_x, source_y, width, height) at (x, y). */
    void Blit(int source_x, int source_y, int width, int height, int x, int y) {
        auto buttons = G.screen->ArtFor(31);  // SETUPBU.PCK
        if (!buttons) return;
        gfx::SetColor(1, 1, 1);
        gfx::Draw(buttons->image, gfx::NewQuad(source_x, source_y, width, height), x, y);
    }

    /** @brief Draws the screen: marble, a box a side, the logo, rating and quote. */
    void Draw() override {
        screen::DrawBackground(*G.screen, layout::Compute(640, 480));
        gfx::SetColor(1, 1, 1);
        gfx::Draw(G.marble, gfx::NewQuad(0, 0, 360, 360), 16, 30);
        gfx::Draw(G.marble, gfx::NewQuad(0, 0, 224, 312), 400, 30);
        gfx::Draw(G.marble, gfx::NewQuad(0, 60, 360, 66), 16, 403);
        gfx::Draw(G.marble, gfx::NewQuad(0, 0, 224, 114), 400, 355);
        const Font& font = kit::GetFont(2);
        for (int i = 0; i < 8; i++) {
            const SideSetup& side = st->sides[i];
            kit::Rect slot_rect = RectFor(i);
            if (side.name.empty()) continue;
            int colour = side.colour, edge = side.edge;
            kit::SetPal(colour);
            kit::Outline(slot_rect.x, slot_rect.y, slot_rect.w, slot_rect.h);
            gfx::Rectangle(gfx::kFill, slot_rect.x - 1, slot_rect.y - 1, slot_rect.w + 2, 1);
            gfx::Rectangle(gfx::kFill, slot_rect.x - 1, slot_rect.y - 1, 1, slot_rect.h + 2);
            gfx::Rectangle(gfx::kFill, slot_rect.x + 1, slot_rect.y + slot_rect.h - 2,
                           slot_rect.w - 2, 1);
            gfx::Rectangle(gfx::kFill, slot_rect.x + slot_rect.w - 2, slot_rect.y + 1, 1,
                           slot_rect.h - 2);
            kit::SetPal(edge);
            gfx::Rectangle(gfx::kFill, slot_rect.x + 1, slot_rect.y + 1, slot_rect.w - 2, 1);
            gfx::Rectangle(gfx::kFill, slot_rect.x + 1, slot_rect.y + 1, 1, slot_rect.h - 2);
            gfx::Rectangle(gfx::kFill, slot_rect.x - 1, slot_rect.y + slot_rect.h, slot_rect.w + 2,
                           1);
            gfx::Rectangle(gfx::kFill, slot_rect.x + slot_rect.w, slot_rect.y - 1, 1,
                           slot_rect.h + 2);
            int name_x = slot_rect.x + 24, name_y = slot_rect.y - 8;
            int width = font.Width(side.name);
            kit::SetPal(3);
            gfx::Rectangle(gfx::kFill, name_x - 4, name_y, width + 8, 20);
            gfx::SetColor(1, 1, 1);
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
            gfx::SetColor(1, 1, 1);
            font.Colours(colour, edge).Draw("Character", slot_rect.x + 88, slot_rect.y + 41);
        }
        // the logo's (0, 0, 144, 64) at (440, 277)
        gfx::SetColor(1, 1, 1);
        if (auto logo = G.screen->ArtFor(52))
            gfx::Draw(logo->image, gfx::NewQuad(0, 0, 144, 64), 440, 277);  // SSG.PCK
        kit::Centred(font, kit::Text(7, 0), 512, 48);
        kit::Centred(font, kit::Text(7, 1), 512, 206);
        kit::Centred(font, Format(kit::Text(6, 0), Rating(*st)), 196, 426);
        for (size_t i = 0; i < lines.size(); i++) kit::Centred(font, lines[i], 512, kQuoteY[i]);
        kit::DrawControls(v, hidden);
    }

    /** @brief Handles a click on one of the screen's controls. */
    void Mousepressed(int x, int y, int) override {
        auto control = kit::ControlAt(v, x, y, hidden);
        if (!control) return;
        int control_id = control->id;
        if (control_id >= 125 && control_id <= 132) {
            // a side's button: Human, Knight, Lord, Warlord, Off, and round again
            SideSetup& side = st->sides[control_id - 125];
            if (side.in_use) {
                if (!side.computer) {
                    side.computer = true;
                    side.level = 0;
                } else if (side.level == 3) {
                    side.computer = false;
                    side.level = 0;
                } else
                    side.level++;
                side.card = 0;  // the level's Standard character
            }
        } else if (control_id == 141) {
            int playing = 0;
            for (auto& side : st->sides)
                if (side.in_use && !(side.computer && side.level == 3)) playing++;
            if (playing < 1) return;
            KeepOptions();
            auto on_begin = begin;
            auto setup = st;
            kit::Pop(this);
            on_begin(setup);
            return;
        } else if (control_id == 142) {
            auto on_back = back;
            kit::Pop(this);
            on_back();
            return;
        } else if (control_id == 143) {
            st->greatest = true;
            for (auto& side : st->sides) {
                if (side.in_use) {
                    if (!(side.computer && side.level == 2)) side.card = 0;
                    side.computer = true;
                    side.level = 2;
                }
            }
        } else if (control_id == 144) {
            st->greatest = false;
        } else if (control_id >= 145 && control_id <= 147) {
            for (int option = 0; option <= 5; option++)
                st->options[option] = kPresets[control_id - 145][option];
        } else if (control_id == 148) {
            std::weak_ptr<Modal> self = weak_from_this();
            SetupScreen* setup_screen = this;
            OpenOptions(st, [self, setup_screen]() {
                if (!self.expired()) setup_screen->Refresh();
            });
            return;
        } else if ((control_id >= 133 && control_id <= 140) ||
                   (control_id >= 149 && control_id <= 156)) {
            // the Character box or the face; a side not in the scenario has
            // neither
            int side_index = control_id >= 149 ? control_id - 149 : control_id - 133;
            if (st->sides[side_index].in_use) {
                std::weak_ptr<Modal> self = weak_from_this();
                SetupScreen* setup_screen = this;
                OpenSide(st, side_index, [self, setup_screen]() {
                    if (!self.expired()) setup_screen->Refresh();
                });
                return;
            }
        } else if (control_id == 157) {
            RecallOptions();
        } else if (control_id == 158) {
            // each computer in play gets 1d(n - 1) of its level's n
            // characters -- any but the Standard one
            static std::mt19937 dice{std::random_device{}()};
            for (auto& side : st->sides) {
                if (side.in_use && side.computer && side.level >= 0 && side.level < 3) {
                    int deck_size =
                            static_cast<int>(w2::aicard::Deck(G.data_dir, side.level).size());
                    side.card = deck_size > 1
                                        ? 1 + static_cast<int>(dice() %
                                                               static_cast<unsigned>(deck_size - 1))
                                        : 0;
                }
            }
        }
        Refresh();
    }
};

/**
 * @brief Opens the setup screen.
 * @param begin Called with the setup when Begin starts the game.
 * @param back Called when Main Menu goes back to the start menu.
 */
void OpenSetup(std::shared_ptr<Setup> setup, std::function<void(std::shared_ptr<Setup>)> begin,
               kit::Done back) {
    auto setup_screen = std::make_shared<SetupScreen>();
    setup_screen->st = setup;
    setup_screen->begin = begin;
    setup_screen->back = back;
    setup_screen->v = kit::View(3);
    setup_screen->v.screen = true;  // a screen, not a dialog: Begin has no ring
    setup_screen->view = &setup_screen->v;
    setup_screen->menu_bar = true;  // the menu bar stays live over it
    // picked once as the screen is drawn whole; Edit Options draws over it
    static std::mt19937 dice{std::random_device{}()};
    setup_screen->lines =
            Quote([](int n) { return 1 + static_cast<int>(dice() % static_cast<unsigned>(n)); });
    setup_screen->Refresh();
    kit::Push(setup_screen);
}

/** @brief New Scenario: the list of scenarios, seven rows, and the chosen one's details. */
struct Chooser : kit::Modal {
    std::vector<Scenario> list;      ///< Every scenario to choose from.
    screen::View v;                  ///< Dialog 29 and its controls' states.
    int top_row = 0;                 ///< The scenario on the list's first row.
    int cur = 0;                     ///< The chosen row, 0-6.
    std::function<void(int)> after;  ///< Called with the scenario chosen, or kNone.

    /** @brief Sets the controls' states; scrolling past the list is off. */
    void Refresh() {
        auto& state = v.state;
        for (auto& control : v.dialog.controls) state[control.id] = w2::uidata::kNormal;
        int can_up = top_row > 0 ? w2::uidata::kNormal : w2::uidata::kDisabled;
        int down = top_row + 7 < static_cast<int>(list.size()) ? w2::uidata::kNormal
                                                               : w2::uidata::kDisabled;
        state[470] = can_up;
        state[472] = can_up;
        state[471] = down;
        state[473] = down;
    }

    /** @brief Draws the list and, on the crystal ball, the chosen scenario's details. */
    void Draw() override {
        const kit::Rect popup_rect{80, 45, 480, 360};  // popup 19
        kit::PopupFrame(popup_rect);
        auto picture = Image(G.data_dir + "/PICS/NEWSCEN.PCK");
        gfx::SetColor(1, 1, 1);
        if (picture) gfx::Draw(picture, popup_rect.x, popup_rect.y);
        kit::SetPal(0);
        gfx::Rectangle(gfx::kFill, 112, 153, 136, 140);
        gfx::SetColor(1, 1, 1);
        const Font& font = kit::GetFont(2);
        for (int i = 0; i <= 6; i++) {
            if (top_row + i < static_cast<int>(list.size()))
                font.Colours(i == cur ? 15 : 5, 0).Draw(list[top_row + i].name, 112, 153 + 20 * i);
        }
        if (top_row + cur < static_cast<int>(list.size())) {
            const Scenario& scenario = list[top_row + cur];
            const Font& ink = font.Colours(5, 0);
            kit::Centred(ink, scenario.name, 416, 101);
            kit::Centred(ink, scenario.text, 416, 174);
            kit::Right(ink, w2::Fmt("%d", scenario.cities), 368, 234);
            ink.Draw(w2::Fmt("%d", scenario.ruins), 472, 234);
            kit::Centred(ink, w2::Fmt("%d", scenario.players), 416, 254);
        }
        std::set<int> row_controls = {476, 477, 478, 479, 480, 481, 482};
        kit::DrawControls(v, row_controls);
    }

    /** @brief Closes the chooser and calls `after` with the scenario chosen, or kNone. */
    void Take(bool confirmed) {
        int pick = confirmed ? top_row + cur : w2::kNone;
        auto done = after;
        kit::Pop(this);
        done(pick);
    }

    /** @brief Handles a click: OK, Cancel, a row, or scrolling a row or a page. */
    void Mousepressed(int x, int y, int) override {
        auto control = kit::ControlAt(v, x, y);
        if (!control || v.StateOf(control->id) == w2::uidata::kDisabled) return;
        int control_id = control->id;
        if (control_id == 474) return Take(true);
        if (control_id == 475) return Take(false);
        if (control_id >= 476 && control_id <= 482) {
            if (top_row + control_id - 476 < static_cast<int>(list.size())) cur = control_id - 476;
        } else if (control_id == 470 || control_id == 472) {
            top_row = std::max(0, top_row - (control_id == 472 ? 7 : 1));
        } else if (control_id == 471 || control_id == 473) {
            top_row = std::min(std::max(0, static_cast<int>(list.size()) - 7),
                               top_row + (control_id == 473 ? 7 : 1));
        }
        Refresh();
    }

    /** @brief Return takes the chosen scenario, Escape cancels. */
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter")
            Take(true);
        else if (key == "escape")
            Take(false);
    }
};

/**
 * @brief Opens New Scenario with `current` chosen, scrolled so it shows.
 * @param after Called with the index of the scenario chosen, or kNone.
 */
void OpenChooser(const std::vector<Scenario>& list, int current, std::function<void(int)> after) {
    auto dialog = std::make_shared<Chooser>();
    dialog->list = list;
    dialog->cur = current;
    dialog->after = after;
    dialog->v = kit::View(29);
    dialog->view = &dialog->v;
    while (dialog->cur >= 7) {
        dialog->cur--;
        dialog->top_row++;
    }
    dialog->Refresh();
    kit::Push(dialog);
}

/// The sliders' left edge and width on the start menu.
const int kSliderX = 384, kSliderW = 120;

/** @brief Draws the random world's settings, in place of the scenario's picture. */
void DrawWorld() {
    kit::SetPal(3);
    gfx::Rectangle(gfx::kFill, 328, 200, 264, 225);
    const Font& font = kit::GetFont(2);
    auto buttons = G.screen->ArtFor(1);  // STARTBU.PCK
    for (int i = 0; i < 4; i++) {
        int y = 215 + 30 * i;
        gfx::SetColor(1, 1, 1);
        kit::Right(font, kit::Text(2, i), 376, y);
        // the slider at its place, or bare for "?"
        int source_y = world.set[i] ? 80 + 20 * world.sliders[i] : 220;
        if (buttons)
            gfx::Draw(buttons->image, gfx::NewQuad(496, source_y, kSliderW, 20), kSliderX, y + 1);
        std::string shows = world.set[i] ? Format(w2::randommap::kSliderFormats[i],
                                                  w2::randommap::kSliderShows[i][world.sliders[i]])
                                         : "(?)";
        font.Draw(shows, 504, y);
    }
    font.Draw(w2::randommap::TerrainSetName(G.data_dir, world.terrain_set), 336, 340);
    font.Draw(kit::Text(3, world.allies ? 1 : 0), 336, 365);
}

/**
 * @brief Makes the random world, showing its progress on popup 23, then
 * hands it on.
 *
 * The scroll shows group 136's lines, the bar RMAPBAR.PCK at (232, 257)
 * grows a tenth at a time, the percentage over it. A "?" slider is rolled,
 * 1d7-1.
 */
struct MakeWorldDialog : kit::Modal {
    w2::Rng rng;                              ///< The dice the world is made with.
    std::unique_ptr<w2::randommap::Run> run;  ///< The generator, a phase at a time.
    kit::Done after;                          ///< Called once the world is installed.
    gfx::ImageP scroll;                       ///< BSCROLL.PCK.
    gfx::ImageP bar;                          ///< RMAPBAR.PCK.

    /** @brief Runs a phase of the generator a frame, so the bar moves as it works. */
    void Update() override {
        if (run->Step()) return;
        w2::randommap::Install(G.data_dir, run->GetFiles());
        auto done = after;
        kit::Pop(this);
        if (done) done();
    }

    /** @brief Draws the scroll, its two lines, and the progress bar. */
    void Draw() override {
        const kit::Rect popup_rect{160, 55, 336, 347};  // popup 23
        gfx::SetColor(1, 1, 1);
        if (scroll)
            gfx::Draw(scroll, gfx::NewQuad(0, 0, popup_rect.w, popup_rect.h), popup_rect.x,
                      popup_rect.y);
        const Font& font = kit::GetFont(2).Colours(0, 7);
        const int lines[2] = {2, 3};
        for (int i = 0; i < 2; i++)
            kit::Centred(font, kit::Text(0x88, lines[i]), 328, 181 + 20 * i);
        // a black frame, the scroll showing through it
        kit::SetPal(0);
        gfx::Rectangle(gfx::kLine, 232.5, 255.5, 191, 24);
        gfx::SetColor(1, 1, 1);
        int percent = run->Progress();
        int width = (percent + 10) / 10 * 16 + 16;
        if (bar) gfx::Draw(bar, gfx::NewQuad(0, 0, width, 21), 232, 257);
        kit::Centred(font, w2::Fmt("%d%%", percent), 328, 237);
    }
};

/** @brief Starts making the random world with the menu's settings; `after` runs once made. */
void MakeWorld(kit::Done after) {
    auto dialog = std::make_shared<MakeWorldDialog>();
    dialog->rng = w2::Rng(static_cast<double>(time(nullptr) % 1000000007));
    w2::randommap::Options options;
    options.data_dir = G.data_dir;
    options.rng = &dialog->rng;
    for (int i = 0; i < 4; i++)
        options.sliders[i] = world.set[i] ? world.sliders[i] : w2::randommap::kRandomSlider;
    options.allies = world.allies;
    options.terrain_set = world.terrain_set;
    dialog->run = std::make_unique<w2::randommap::Run>(options);
    dialog->after = after;
    dialog->scroll = Image(G.data_dir + "/PICS/BSCROLL.PCK", 10);
    dialog->bar = Image(G.data_dir + "/PICS/RMAPBAR.PCK", 10);
    kit::Push(dialog);
}

/** @brief The start menu the game opens on. */
struct Menu : kit::Modal {
    std::vector<Scenario> list;  ///< Every scenario to choose from.
    screen::View v;              ///< Dialog 1 and its controls' states.
    int cur = 0;                 ///< The scenario chosen, an index into `list`.
    /// Starts a new game in a scenario directory with the options set up.
    std::function<void(const std::string&, const w2::game::NewGameOptions&)> begin_game;
    std::function<void(std::unique_ptr<w2::Game>)> loaded;  ///< Takes a loaded game.

    /** @brief Sets the controls' states; the random world's controls show only for it. */
    void Refresh() {
        auto& state = v.state;
        state[100] = state[101] = state[103] = w2::uidata::kNormal;
        state[102] = world.on ? w2::uidata::kDisabled : w2::uidata::kNormal;
        hidden.clear();
        if (!world.on) {
            for (int control_id = 104; control_id <= 113; control_id++) hidden.insert(control_id);
            return;
        }
        state[104] = state[105] = w2::uidata::kNormal;
        for (int i = 0; i < 4; i++) {
            state[106 + i] = w2::uidata::kNormal;
            state[110 + i] =
                    world.set[i] ? w2::uidata::kNormal : w2::uidata::kActive;  // lit while "?"
        }
    }

    /** @brief Draws the menu: its four pictures, and the scenario or the random world. */
    void Draw() override {
        gfx::SetColor(1, 1, 1);
        for (int quarter = 0; quarter < 4; quarter++) {
            auto image = Image(G.data_dir + "/PICS/STARTUP" + std::to_string(quarter) + ".PCK");
            if (image) gfx::Draw(image, (quarter % 2) * 320, (quarter / 2) * 240);
        }
        bool have = cur < static_cast<int>(list.size());
        if (world.on) {
            DrawWorld();
        } else if (have) {
            std::string dir = w2::Upper(list[cur].dir);
            if (auto picture = Image(G.data_dir + "/" + dir + "/PICS/SCENARIO.PCK")) {
                gfx::SetColor(1, 1, 1);
                gfx::Draw(picture, gfx::NewQuad(0, 0, 264, 225), 328, 200);
            }
        }
        if (world.on || have) {
            // the scenario's name, or "A Random World"
            kit::SetPal(3);
            gfx::Rectangle(gfx::kFill, 336, 166, 248, 28);
            gfx::SetColor(1, 1, 1);
            kit::Centred(kit::GetFont(2), world.on ? kit::Text(0, 0) : list[cur].name, 460, 172);
        }
        kit::DrawControls(v, hidden);
    }

    /**
     * @brief Opens the setup screen on a scenario. Begin there starts the
     * game; Main Menu brings this menu back.
     */
    void SetUp(const Scenario& scenario) {
        auto keep = shared_from_this();
        auto setup = NewSetup(scenario);
        auto on_begin = begin_game;
        OpenSetup(
                setup,
                [on_begin](std::shared_ptr<Setup> chosen_setup) {
                    w2::game::NewGameOptions options;
                    for (int i = 0; i <= 9; i++)
                        options.options.emplace_back(kOptionKeys[i], chosen_setup->options[i]);
                    for (int i = 0; i < 8; i++) {
                        const SideSetup& side = chosen_setup->sides[i];
                        w2::game::SideSetup game_side;
                        game_side.computer = side.computer;
                        game_side.level = side.level;
                        game_side.card = side.card;
                        game_side.name = side.name;
                        game_side.off = side.computer && side.level == 3;
                        options.sides[i] = game_side;
                    }
                    options.greatest = chosen_setup->greatest;
                    on_begin(w2::Upper(chosen_setup->sc.dir), options);
                },
                [keep]() {
                    auto menu = std::static_pointer_cast<Menu>(keep);
                    menu->Refresh();
                    kit::Push(keep);
                });
    }

    /** @brief Handles a click on one of the menu's controls. */
    void Mousepressed(int x, int y, int) override {
        auto control = kit::ControlAt(v, x, y, hidden);
        if (!control || v.StateOf(control->id) == w2::uidata::kDisabled) return;
        int control_id = control->id;
        std::weak_ptr<Modal> self = weak_from_this();
        Menu* menu = this;
        if (control_id == 100) {
            // a scenario chosen ends the random world
            OpenChooser(list, cur, [self, menu](int chosen) {
                if (self.expired()) return;
                if (chosen != w2::kNone) {
                    menu->cur = chosen;
                    world.on = false;
                }
                menu->Refresh();
            });
        } else if (control_id == 101) {
            auto on_loaded = loaded;
            savegame::Load([self, on_loaded](std::unique_ptr<w2::Game> game) {
                if (auto alive = self.lock()) kit::Pop(alive.get());
                on_loaded(std::move(game));
            });
        } else if (control_id == 102) {
            world.on = true;
        } else if (control_id == 104) {
            world.terrain_set = (world.terrain_set + 1) % w2::randommap::TerrainSets(G.data_dir);
        } else if (control_id == 105) {
            world.allies = !world.allies;
        } else if (control_id >= 106 && control_id <= 109) {
            // a "?" slider is set again where it was; a set one moves to
            // where it was clicked
            int slider = control_id - 106;
            if (!world.set[slider])
                world.set[slider] = true;
            else
                world.sliders[slider] = std::clamp((x - kSliderX) * 7 / kSliderW, 0, 6);
        } else if (control_id >= 110 && control_id <= 113) {
            world.set[control_id - 110] = false;
        } else if (control_id == 103 && world.on) {
            // "One moment...", the world, then the sides as for any scenario
            advisor::Say(w2::cues::kMoment, [self, menu]() {
                MakeWorld([self, menu]() {
                    auto keep = self.lock();
                    if (!keep) return;
                    kit::Pop(menu);
                    Scenario scenario;
                    scenario.name = kit::Text(0, 0);
                    scenario.dir = w2::randommap::kDir;
                    menu->SetUp(scenario);
                });
            });
        } else if (control_id == 103 && cur < static_cast<int>(list.size())) {
            auto keep = shared_from_this();
            kit::Pop(this);
            SetUp(list[cur]);
            return;
        }
        Refresh();
    }
};
}  // namespace

/**
 * @brief Opens the start menu on Erythea, or on the random world when coming
 * back from one.
 * @param begin Called to start a new game set up on the start screens.
 * @param loaded Called with a saved game loaded from the menu.
 */
void Open(std::function<void(const std::string& dir, const w2::game::NewGameOptions& opts)> begin,
          std::function<void(std::unique_ptr<w2::Game>)> loaded) {
    auto menu = std::make_shared<Menu>();
    menu->list = Scenarios();
    for (size_t i = 0; i < menu->list.size(); i++)
        if (menu->list[i].dir == "Erythea") menu->cur = static_cast<int>(i);
    menu->begin_game = begin;
    menu->loaded = loaded;
    if (G.scenario == w2::randommap::kDir) world.on = true;
    menu->v = kit::View(1);
    menu->v.screen = true;  // a screen, not a dialog: Begin has no ring
    menu->view = &menu->v;
    menu->menu_bar = true;
    menu->Refresh();
    kit::Push(menu);
}

}  // namespace start
