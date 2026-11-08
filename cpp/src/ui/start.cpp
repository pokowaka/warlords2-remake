// The start screens: the menu the game opens on, choosing a scenario, and
// setting its sides and options up (7f77:0000, 7f77:0725, 7bab:0000).
//
// The start menu (7f77:0000, dialog 1): STARTUP0-3.PCK with New Scenario
// (100), Load Game (101), Random Map (102) and Begin (103), and the
// scenario's own PICS\SCENARIO.PCK under a bar with its name. The scenario
// starts as Erythea (4125:2a6e).
//
// Random Map (7f77:05f5) puts "A Random World" on the bar and, in place of
// the picture (7f77:0332), its settings on colour 3: four sliders -- Water,
// Hills, Cities, Forest, 106-109, STARTBU.PCK at (384, 216) 30 apart -- each
// with a "?" (110-113) that leaves it to chance, the terrain set (104) and
// "Cities can produce allies" (105). Begin then has the advisor say "One
// moment..." and makes the world (random_map_setup, 7bab:10e8) on
// BSCROLL.PCK with a bar (4bed:01ff). New Scenario's choice ends it.
//
// New Scenario (7f77:058d, 0725): popup 19, NEWSCEN.PCK, dialog 29 -- the
// scenarios of SCENARIO.DAT in a black box, seven rows, and on the crystal
// ball the chosen one's name, description, cities, ruins and players. Not
// the original's: generated worlds follow SCENARIO.DAT's (w2scengen).
//
// Begin (7bab:0000) sets the sides up on the main screen's own frame,
// dialog 3: a box a side with its face, its button -- Human, Knight, Lord,
// Warlord or Off -- and its Character box (7bab:0634); Begin (141), Main
// Menu (142), I am the Greatest (143) / No! I really am Normal (144), the
// presets (145-147) and Edit Options (148); Recall Options (157) and Random
// Characters (158) at the foot; the difficulty rating (7bab:0bab); the SSG
// logo (7bab:10c7); and under it all a quote on war from QUOTES.DAT
// (7bab:0f88). The options are DATA\OPTIONS.DAT's, the ones the last game
// began with, not the scenario's own (7bab:223b).
//
// Edit Options (7bab:12a2): popup 4, dialog 4 -- the ten options of group 4
// two to a row; 159-168 change one, 169-171 are the presets, OK (172).
//
// Setup Side (7bab:16ea), from a side's Character box (133-140) or its face
// (149-156): popup 4, dialog 27 -- the side's name to retype, and for a
// computer the characters of its level's deck to choose from, with the
// chosen one's description (7bab:180b). OK (457), Cancel (458).
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
// the ten options, in the order of group 4 and the table at 4125:23b4
const char* const kOptionKeys[10] = {
    "neutralCities",  "diplomacy",     "quests",     "hiddenMap",       "viewEnemies",
    "viewProduction", "intenseCombat", "quickStart", "militaryAdvisor", "randomTurns"};
const int kPresets[3][10] = {
    // 4125:2378
    {0, 0, 0, 0, 1, 0, 0, 0, 1, 0},
    {1, 1, 1, 0, 0, 0, 0, 0, 1, 0},
    {2, 1, 1, 1, 0, 1, 0, 0, 1, 0},
};

// The random world's settings, kept for the session as the original keeps
// them (4125:28c8-28d8): on, the terrain set, allies, and each slider's
// place and whether it is set (else "?", left to chance).
struct World {
    bool on = false;
    int terrain_set = 0;
    bool allies = false;
    std::array<int, 4> sliders{{3, 3, 2, 3}};
    std::array<bool, 4> set{{true, true, true, true}};
};
World world;

// The options being set up (4125:23b4): one table for the session, the
// Beginner preset to begin with.
int remembered_options[10] = {0, 0, 0, 0, 1, 0, 0, 0, 1, 0};

// 7bab:223b: put back the options the last game began with, ten u16s in
// DATA\OPTIONS.DAT -- the setup screen as it opens, and Recall Options.
// With no file the table is left as it is.
void RecallOptions() {
    auto s = w2::ReadFile(G.data_dir + "/DATA/OPTIONS.DAT");
    if (!s) return;
    for (size_t i = 0; i <= 9 && 2 * i + 2 <= s->size(); i++)
        remembered_options[i] = w2::U16(*s, 2 * i);
}

// 7bab:2289: keep them for next time, as Begin starts the game.
void KeepOptions() {
    std::string b;
    for (int i = 0; i <= 9; i++) {
        b += static_cast<char>(remembered_options[i] & 255);
        b += static_cast<char>((remembered_options[i] >> 8) & 255);
    }
    w2::WriteFile(G.data_dir + "/DATA/OPTIONS.DAT", b);
}

struct Scenario {
    std::string name, dir, text;
    int cities = 0, ruins = 0, players = 0;
};

// SCENARIO.DAT's records, and after them the worlds the scenario generator
// made (not the original's; warlords/scenarios, docs/scengen.md).
std::vector<Scenario> Scenarios() {
    std::vector<Scenario> out;
    for (auto& e : w2::scenarios::List(G.data_dir))
        out.push_back(Scenario{e.name, e.dir, e.text, e.cities, e.ruins, e.players});
    return out;
}

std::map<std::string, gfx::ImageP> art;
gfx::ImageP Image(const std::string& path, int key = pckimage::kNokey) {
    auto it = art.find(path);
    if (it != art.end()) return it->second;
    gfx::ImageP img;
    try {
        img = pckimage::Load(path, G.palette, key);
    } catch (...) {
    }
    return art[path] = img;
}

struct SideSetup {
    bool in_use = false;
    std::string name;
    int colour = 15, edge = 0;
    bool computer = false;
    int level = 0, card = 0;
};

struct Setup {
    Scenario sc;
    int* options = remembered_options;
    SideSetup sides[8];
    bool greatest = false;
};

std::shared_ptr<Setup> NewSetup(const Scenario& sc) {
    std::string dir = w2::Upper(sc.dir);
    auto map = w2::scn::Load(G.data_dir + "/" + dir, dir);
    auto st = std::make_shared<Setup>();
    st->sc = sc;
    // 7bab:0000: the tutorial plays with the Beginner options, any other
    // scenario with the last game's
    if (map->options.tutorial != 0)
        for (int i = 0; i <= 9; i++) remembered_options[i] = kPresets[0][i];
    else
        RecallOptions();
    for (auto& s : map->sides) {
        SideSetup& e = st->sides[s.index];
        e.in_use = s.in_use;
        e.name = s.name;
        e.colour = s.colour;
        e.edge = s.edge;
        e.computer = s.computer;
        e.level = s.computer ? s.level : 0;
        e.card = s.card;
    }
    return st;
}

bool PresetMatches(const Setup& st, int p) {
    for (int i = 0; i <= 5; i++)
        if (st.options[i] != kPresets[p][i]) return false;
    return true;
}

// 7bab:0bab: the options' weight and the computers' strength, a percent.
int Rating(const Setup& st) {
    const int* o = st.options;
    int cx = std::min(20, o[0] * 4 + o[1] * 4 + o[2] * 3 + o[3] * 4 - o[4] + o[5]);
    int sum = 0, n = 0;
    for (auto& s : st.sides) {
        if (s.in_use && s.computer && s.level != 3) {
            sum += s.level + 1;
            n++;
        }
    }
    int v = n == 0 ? 80 : sum * 80 / (n * 3);
    if (v >= 78) v = 80;
    return v + cx;
}

struct Options : kit::Modal {
    std::shared_ptr<Setup> st;
    screen::View v;
    kit::Done after;
    void Refresh() {
        auto& s = v.state;
        for (int i = 0; i <= 9; i++) s[159 + i] = w2::uidata::kNormal;
        for (int p = 0; p <= 2; p++)
            s[169 + p] = PresetMatches(*st, p) ? w2::uidata::kActive : w2::uidata::kNormal;
        s[172] = w2::uidata::kNormal;
    }
    void Draw() override {
        const kit::Rect popup_rect{120, 50, 400, 360};  // popup 4
        kit::Popup(popup_rect);
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), kit::Text(8, 0), 320, 55);
        const Font& f = kit::GetFont(2);
        const int ys[2] = {111, 250};
        for (int k = 0; k < 2; k++) {
            int y = ys[k];
            kit::SetPal(15);
            gfx::Rectangle(gfx::kFill, 160, y, 320, 1);
            kit::SetPal(0);
            gfx::Rectangle(gfx::kFill, 161, y + 1, 320, 1);
            std::string t = kit::Text(9 + k, 0);
            int w = f.Width(t);
            kit::SetPal(3);
            gfx::Rectangle(gfx::kFill, 320 - w / 2 - 4, y, w + 8, 2);
            gfx::SetColor(1, 1, 1);
            kit::Centred(f, t, 320, y - 7);
        }
        for (int i = 0; i <= 9; i++) {
            int x = i % 2 == 0 ? 128 : 320;
            int y = i < 6 ? 131 + 30 * (i / 2) : 270 + 30 * ((i - 6) / 2);
            gfx::SetColor(1, 1, 1);
            f.Draw(kit::Text(4, i), x, y);
            int val = st->options[i];
            // View Production's word is the other way round (7bab:1567): its flag
            // hides other sides' production, so set it reads "Off"
            bool on = i == 5 ? val == 0 : val != 0;
            std::string word = i == 0 ? kit::Text(5, val) : (on ? "On" : "Off");
            f.Colours(7, 0).Draw(word, x + 128, y);
        }
        kit::DrawControls(v);
    }
    void Finish() {
        auto a = after;
        kit::Pop(this);
        if (a) a();
    }
    void Mousepressed(int x, int y, int) override {
        auto c = kit::ControlAt(v, x, y);
        if (!c) return;
        int id = c->id;
        if (id == 172) return Finish();
        if (id >= 159 && id <= 168) {
            int i = id - 159;
            if (i == 0)
                st->options[0] = (st->options[0] + 1) % 3;
            else
                st->options[i] = st->options[i] != 0 ? 0 : 1;
        } else if (id >= 169 && id <= 171) {
            for (int k = 0; k <= 5; k++) st->options[k] = kPresets[id - 169][k];
        }
        Refresh();
    }
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") Finish();
    }
};

void OpenOptions(std::shared_ptr<Setup> st, kit::Done after) {
    auto d = std::make_shared<Options>();
    d->st = st;
    d->after = after;
    d->v = kit::View(4);
    d->view = &d->v;
    d->Refresh();
    kit::Push(d);
}

// SETUPBU.PCK's faces (4125:2408): Knight, Lord, Warlord, Off, the two
// humans, a side not in the scenario, then a Knight, Lord and Warlord
// playing a character other than the Standard one
const int kFaces[10][2] = {{0, 0},   {0, 40},   {0, 80},    {0, 120},   {0, 160},
                           {0, 200}, {440, 40}, {424, 100}, {424, 140}, {424, 180}};
const int kLevelButton[5] = {120, 200, 280, 360, 40};  // 4125:24b8: Knight .. Off, Human
// the Character box (4125:24ec): ticked with a character chosen, else empty,
// 24 x 20 at (x + 64, y + 39), its label at (x + 88, y + 41) (4125:24fc, 251c)
const int kCheck[2][2] = {{440, 0}, {440, 20}};

// A side's face (7bab:0634), an index into FACES.
int FaceOf(const SideSetup& s, int i) {
    if (!s.in_use) return 6;
    if (!s.computer) return i % 2 == 1 ? 5 : 4;
    int level = std::clamp(s.level, 0, 3);
    if (s.card != 0 && level < 3) return level + 7;
    return level;
}

// How many computers are in play: Random Characters needs one (7bab:0416).
int Computers(const Setup& st) {
    int n = 0;
    for (auto& s : st.sides)
        if (s.in_use && s.computer && s.level != 3) n++;
    return n;
}

// Setup Side's controls (dialog 27)
const int kSideOk = 457, kSideCancel = 458, kSideName = 459;
const int kSideUp = 460, kSideDown = 461, kSidePageUp = 462, kSidePageDown = 463, kSideRow = 464;
const kit::Rect kNameBox{229, 138, 188, 22};
// a Knight's characters are listed in colour 5, a Lord's 7, a Warlord's 9
const int kLevelInk[3] = {5, 7, 9};

// Setup Side (7bab:16ea, drawn by 7bab:180b): popup 4, dialog 27. A side's
// name, retyped in the field (15 characters, 128 pixels, 7bab:1f8a); for a
// computer its level's characters, five rows at a time, and the chosen one's
// description from its .DSC. A human has no character: "N/A". OK keeps what
// was done, Cancel puts the name and character back.
struct SideDialog : kit::Modal {
    std::shared_ptr<Setup> st;
    int index = 0;
    screen::View v;
    kit::Done after;
    std::string saved_name;
    int saved_card = 0;
    std::vector<std::string> deck;
    int rows[5] = {-1, -1, -1, -1, -1};  // the five rows' cards, -1 for none
    std::optional<input::Editor> editing;
    std::vector<std::string> desc;
    int desc_card = -1;

    SideSetup& Setup() { return st->sides[index]; }
    int N() const { return static_cast<int>(deck.size()); }

    void Start() {
        SideSetup& s = Setup();
        saved_name = s.name;
        saved_card = s.card;
        // Off has no deck: its letter is past the end of "KLW"
        if (s.computer && s.level >= 0 && s.level < 3) deck = w2::aicard::Deck(G.data_dir, s.level);
        // the chosen one on the last row when it is past the first five
        for (int r = 0; r < 5; r++) rows[r] = r < N() ? r : -1;
        if (s.card > 4)
            for (int r = 0; r < 5; r++) rows[r] += s.card - 4;
        Refresh();
    }

    // 7bab:1ae4
    void Refresh() {
        auto& vs = v.state;
        for (int r = 0; r < 5; r++)
            vs[kSideRow + r] = rows[r] < 0 ? w2::uidata::kDisabled : w2::uidata::kNormal;
        vs[kSideOk] = vs[kSideCancel] = vs[kSideName] = w2::uidata::kNormal;
        bool up = Setup().computer && rows[0] >= 1;
        bool down = Setup().computer && rows[4] > 0 && rows[4] < N() - 1;
        vs[kSideUp] = vs[kSidePageUp] = up ? w2::uidata::kNormal : w2::uidata::kDisabled;
        vs[kSideDown] = vs[kSidePageDown] = down ? w2::uidata::kNormal : w2::uidata::kDisabled;
    }

    // an empty name is not taken: the setup screen knows a side by its name
    void KeepName() {
        if (!editing->text.empty()) Setup().name = editing->text;
        editing.reset();
    }

    void Close(bool ok) {
        if (!ok) {
            Setup().name = saved_name;
            Setup().card = saved_card;
        }  // 7bab:1fd7
        auto a = after;
        kit::Pop(this);
        if (a) a();
    }

    void Draw() override {
        const kit::Rect popup_rect{120, 50, 400, 360};  // popup 4
        const SideSetup& s = Setup();
        kit::Popup(popup_rect);
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), "Setup Side", 320, 55);
        int c = s.colour, e = s.edge;
        const Font& f = kit::GetFont(2);
        // two boxes, each outlined in the side's edge colour and twice more in
        // its colour, a pixel further up and left each time
        const int boxes[2][2] = {{107, 67}, {194, 165}};
        for (auto& b : boxes) {
            kit::SetPal(e);
            kit::Outline(138, b[0], 368, b[1]);
            kit::SetPal(c);
            kit::Outline(137, b[0] - 1, 368, b[1]);
            kit::Outline(136, b[0] - 2, 368, b[1]);
        }
        const std::pair<const char*, int> tabs[2] = {{"Side Name", 96}, {"Leader", 181}};
        for (auto& t : tabs) {
            kit::SetPal(3);
            gfx::Rectangle(gfx::kFill, 160, t.second, f.Width(t.first), 17);
            gfx::SetColor(1, 1, 1);
            f.Colours(c, e).Draw(t.first, 160, t.second);
        }
        kit::Shield(index, 144, 122);
        kit::Shield(index, 456, 122);
        kit::Shield(index, 144, 202);
        gfx::SetColor(1, 1, 1);
        kit::Centred(f, "Retype the name of this side", 320, 116);
        kit::Field(kNameBox.x, kNameBox.y, kNameBox.w, kNameBox.h, editing ? editing->text : s.name,
                   &f);
        if (editing) editing->DrawCursor(kNameBox.x, kNameBox.y);
        kit::SetPal(0);
        gfx::Rectangle(gfx::kFill, 196, 218, 72, 1);
        gfx::Rectangle(gfx::kFill, 384, 218, 96, 1);
        gfx::SetColor(1, 1, 1);
        f.Draw("Name", 196, 203);
        f.Draw("Description", 384, 203);
        // the list (7bab:1be6): a sunk box, the chosen character in white
        kit::SetPal(3);
        gfx::Rectangle(gfx::kFill, 189, 249, 186, 100);
        kit::Bevel(188, 248, 188, 102, 4, 2);
        gfx::SetColor(1, 1, 1);
        if (!s.computer) {
            f.Draw("N/A", 196, 224);  // 7bab:1d0f
            f.Draw("N/A", 384, 224);
        } else {
            const Font& ink = f.Colours(s.level >= 0 && s.level < 3 ? kLevelInk[s.level] : 15, 0);
            for (int r = 0; r < 5 && rows[r] >= 0; r++) {
                const std::string& name = rows[r] < N() ? deck[rows[r]] : std::string();
                (rows[r] == s.card ? f : ink).Draw(name, 196, 252 + 19 * r);
            }
            // the name, and the .DSC's next six lines (7bab:20dc)
            if (s.card >= 0 && s.card < N()) ink.Draw(deck[s.card], 192, 224);
            if (desc_card != s.card) {
                desc_card = s.card;
                desc.clear();
                if (s.level >= 0 && s.level < 3)
                    if (auto dd = w2::aicard::Describe(G.data_dir, s.level, s.card))
                        desc = dd->second;
            }
            for (int k = 0; k < 6 && k < static_cast<int>(desc.size()); k++)
                if (!desc[k].empty()) ink.Draw(desc[k], 384, 224 + 20 * k);
        }
        kit::DrawControls(v);
    }

    void Mousepressed(int x, int y, int) override {
        if (editing) KeepName();
        auto ctl = kit::ControlAt(v, x, y);
        if (!ctl) return;
        int id = ctl->id;
        if (id == kSideOk) return Close(true);  // 7bab:2024
        if (id == kSideCancel) return Close(false);
        if (id == kSideName) {
            editing = input::Editor{};
            editing->max_chars = 15;
            editing->max_width = 128;
        } else if (id == kSideUp || id == kSideDown) {
            // 7bab:1ed1: a row up or down, a row run off the deck left empty
            int step = id == kSideUp ? -1 : 1;
            for (int r = 0; r < 5; r++) {
                rows[r] += step;
                if (rows[r] < 0 || rows[r] >= N()) rows[r] = -1;
            }
        } else if (id == kSidePageUp || id == kSidePageDown) {
            // 7bab:1f23: five rows, or as many as there are
            int step = id == kSidePageUp ? -std::min(rows[0], 5) : std::min(N() - rows[4] - 1, 5);
            rows[0] += step;
            for (int r = 1; r < 5; r++) rows[r] = rows[r - 1] + 1;
        } else if (id >= kSideRow && id < kSideRow + 5) {
            int card = rows[id - kSideRow];  // 7bab:1e8a
            if (card >= 0) Setup().card = card;
        }
        Refresh();
    }

    void Keypressed(const std::string& key) override {
        if (editing) {
            std::string r = editing->Key(key);
            if (r == "keep")
                KeepName();
            else if (r == "undo")
                editing.reset();
            return;
        }
        if (key == "return" || key == "kpenter")
            Close(true);
        else if (key == "escape")
            Close(false);
    }

    void Textinput(const std::string& t) override {
        if (editing) editing->Input(t);
    }
    bool Typing() const override { return editing.has_value(); }
};

void OpenSide(std::shared_ptr<Setup> st, int index, kit::Done after) {
    auto d = std::make_shared<SideDialog>();
    d->st = st;
    d->index = index;
    d->after = after;
    d->v = kit::View(27);
    d->view = &d->v;
    d->Start();
    kit::Push(d);
}

kit::Rect RectFor(int i) {
    return kit::Rect{i < 4 ? 24 : 208, 40 + 90 * (i % 4), 160, 70};
}  // 4125:23c8

// where the quote's lines go, centred (4125:253c)
const int kQuoteY[5] = {366, 384, 402, 420, 440};

// A quote from QUOTES.DAT, as 7bab:0f88 picks it: a two-digit count, then
// records of a three-digit length and that many bytes less one, lines split
// by "|" and each closed by one. `roll(count)`, dice(1, count), says which.
std::vector<std::string> Quote(const std::function<int(int)>& roll) {
    std::vector<std::string> lines;
    auto s = w2::ReadFile(G.data_dir + "/DATA/QUOTES.DAT");
    if (!s || s->size() < 2) return lines;
    int count = atoi(s->substr(0, 2).c_str());
    if (count < 1) return lines;
    int pick = roll(count);
    size_t o = 4;
    std::string text;
    for (int i = 0; i < pick && o + 3 <= s->size(); i++) {
        int n = atoi(s->substr(o, 3).c_str());
        text = n > 0 ? s->substr(o + 3, n - 1) : std::string();
        o += 3 + n + 1;
    }
    // only text closed by a "|" is drawn
    size_t start = 0;
    for (size_t bar; (bar = text.find('|', start)) != std::string::npos && lines.size() < 5;
         start = bar + 1) {
        lines.push_back(text.substr(start, bar - start));
    }
    return lines;
}

struct SetupScreen : kit::Modal {
    std::shared_ptr<Setup> st;
    std::vector<std::string> lines;  // the quote, picked as the screen opens
    screen::View v;
    std::function<void(std::shared_ptr<Setup>)> begin;
    kit::Done back;

    void Refresh() {
        auto& s = v.state;
        for (int i = 0; i < 8; i++) s[125 + i] = w2::uidata::kNormal;
        for (int p = 0; p <= 2; p++)
            s[145 + p] = PresetMatches(*st, p) ? w2::uidata::kActive : w2::uidata::kNormal;
        s[141] = s[142] = s[148] = s[143] = s[144] = w2::uidata::kNormal;
        s[157] = w2::uidata::kNormal;
        s[158] = Computers(*st) > 0 ? w2::uidata::kNormal : w2::uidata::kDisabled;
        hidden = {st->greatest ? 143 : 144};
    }
    void Blit(int sx, int sy, int w, int h, int x, int y) {
        auto a = G.screen->ArtFor(31);  // SETUPBU.PCK
        if (!a) return;
        gfx::SetColor(1, 1, 1);
        gfx::Draw(a->image, gfx::NewQuad(sx, sy, w, h), x, y);
    }
    void Draw() override {
        screen::DrawBackground(*G.screen, layout::Compute(640, 480));
        gfx::SetColor(1, 1, 1);
        gfx::Draw(G.marble, gfx::NewQuad(0, 0, 360, 360), 16, 30);
        gfx::Draw(G.marble, gfx::NewQuad(0, 0, 224, 312), 400, 30);
        gfx::Draw(G.marble, gfx::NewQuad(0, 60, 360, 66), 16, 403);
        gfx::Draw(G.marble, gfx::NewQuad(0, 0, 224, 114), 400, 355);
        const Font& f = kit::GetFont(2);
        for (int i = 0; i < 8; i++) {
            const SideSetup& s = st->sides[i];
            kit::Rect slot_rect = RectFor(i);
            if (s.name.empty()) continue;
            int c = s.colour, e = s.edge;
            kit::SetPal(c);
            kit::Outline(slot_rect.x, slot_rect.y, slot_rect.w, slot_rect.h);
            gfx::Rectangle(gfx::kFill, slot_rect.x - 1, slot_rect.y - 1, slot_rect.w + 2, 1);
            gfx::Rectangle(gfx::kFill, slot_rect.x - 1, slot_rect.y - 1, 1, slot_rect.h + 2);
            gfx::Rectangle(gfx::kFill, slot_rect.x + 1, slot_rect.y + slot_rect.h - 2,
                           slot_rect.w - 2, 1);
            gfx::Rectangle(gfx::kFill, slot_rect.x + slot_rect.w - 2, slot_rect.y + 1, 1,
                           slot_rect.h - 2);
            kit::SetPal(e);
            gfx::Rectangle(gfx::kFill, slot_rect.x + 1, slot_rect.y + 1, slot_rect.w - 2, 1);
            gfx::Rectangle(gfx::kFill, slot_rect.x + 1, slot_rect.y + 1, 1, slot_rect.h - 2);
            gfx::Rectangle(gfx::kFill, slot_rect.x - 1, slot_rect.y + slot_rect.h, slot_rect.w + 2,
                           1);
            gfx::Rectangle(gfx::kFill, slot_rect.x + slot_rect.w, slot_rect.y - 1, 1,
                           slot_rect.h + 2);
            int nx = slot_rect.x + 24, ny = slot_rect.y - 8;
            int w = f.Width(s.name);
            kit::SetPal(3);
            gfx::Rectangle(gfx::kFill, nx - 4, ny, w + 8, 20);
            gfx::SetColor(1, 1, 1);
            f.Colours(c, e).Draw(s.name, nx, ny);
            // the face (7bab:0634): Off, a human (two faces, turn about), or the
            // computer's level, another face for a character; the button the
            // same, Off and Human mapped in
            int face = FaceOf(s, i);
            Blit(kFaces[face][0], kFaces[face][1], 40, 40, slot_rect.x + 16, slot_rect.y + 16);
            int lv = face >= 7 ? face - 7 : face;
            int btn = (lv == 6 || lv == 3) ? 3 : (lv >= 4 ? 4 : lv);
            Blit(kLevelButton[btn], 0, 80, 25, slot_rect.x + 72, slot_rect.y + 12);
            const int* ck = kCheck[s.card != 0 ? 0 : 1];
            Blit(ck[0], ck[1], 24, 20, slot_rect.x + 64, slot_rect.y + 39);
            gfx::SetColor(1, 1, 1);
            f.Colours(c, e).Draw("Character", slot_rect.x + 88, slot_rect.y + 41);  // 4125:258b
        }
        // 7bab:10c7: the logo's (0, 0, 144, 64) to (440, 277) (4125:2550)
        gfx::SetColor(1, 1, 1);
        if (auto logo = G.screen->ArtFor(52))
            gfx::Draw(logo->image, gfx::NewQuad(0, 0, 144, 64), 440, 277);  // SSG.PCK
        kit::Centred(f, kit::Text(7, 0), 512, 48);
        kit::Centred(f, kit::Text(7, 1), 512, 206);
        kit::Centred(f, Format(kit::Text(6, 0), Rating(*st)), 196, 426);
        for (size_t i = 0; i < lines.size(); i++) kit::Centred(f, lines[i], 512, kQuoteY[i]);
        kit::DrawControls(v, hidden);
    }
    void Mousepressed(int x, int y, int) override {
        auto c = kit::ControlAt(v, x, y, hidden);
        if (!c) return;
        int id = c->id;
        if (id >= 125 && id <= 132) {
            // 7bab:0a4e: Human, Knight, Lord, Warlord, Off, and round again
            SideSetup& s = st->sides[id - 125];
            if (s.in_use) {
                if (!s.computer) {
                    s.computer = true;
                    s.level = 0;
                } else if (s.level == 3) {
                    s.computer = false;
                    s.level = 0;
                } else
                    s.level++;
                s.card = 0;  // the level's Standard character
            }
        } else if (id == 141) {
            int playing = 0;
            for (auto& s : st->sides)
                if (s.in_use && !(s.computer && s.level == 3)) playing++;
            if (playing < 1) return;
            KeepOptions();  // 7bab:0cfe
            auto b = begin;
            auto s = st;
            kit::Pop(this);
            b(s);
            return;
        } else if (id == 142) {
            auto b = back;
            kit::Pop(this);
            b();
            return;
        } else if (id == 143) {
            st->greatest = true;
            for (auto& s : st->sides) {
                if (s.in_use) {
                    if (!(s.computer && s.level == 2)) s.card = 0;
                    s.computer = true;
                    s.level = 2;
                }
            }
        } else if (id == 144) {
            st->greatest = false;
        } else if (id >= 145 && id <= 147) {
            for (int k = 0; k <= 5; k++) st->options[k] = kPresets[id - 145][k];
        } else if (id == 148) {
            std::weak_ptr<Modal> self = weak_from_this();
            SetupScreen* me = this;
            OpenOptions(st, [self, me]() {
                if (!self.expired()) me->Refresh();
            });
            return;
        } else if ((id >= 133 && id <= 140) || (id >= 149 && id <= 156)) {
            // 7bab:16ea: the Character box or the face; a side not in the
            // scenario has neither
            int i = id >= 149 ? id - 149 : id - 133;
            if (st->sides[i].in_use) {
                std::weak_ptr<Modal> self = weak_from_this();
                SetupScreen* me = this;
                OpenSide(st, i, [self, me]() {
                    if (!self.expired()) me->Refresh();
                });
                return;
            }
        } else if (id == 157) {
            RecallOptions();  // 7bab:2229
        } else if (id == 158) {
            // 7bab:2051: each computer in play gets 1d(n - 1) of its level's n
            // characters -- any but the Standard one
            static std::mt19937 dice{std::random_device{}()};
            for (auto& s : st->sides) {
                if (s.in_use && s.computer && s.level >= 0 && s.level < 3) {
                    int n = static_cast<int>(w2::aicard::Deck(G.data_dir, s.level).size());
                    s.card =
                            n > 1 ? 1 + static_cast<int>(dice() % static_cast<unsigned>(n - 1)) : 0;
                }
            }
        }
        Refresh();
    }
};

void OpenSetup(std::shared_ptr<Setup> st, std::function<void(std::shared_ptr<Setup>)> begin,
               kit::Done back) {
    auto d = std::make_shared<SetupScreen>();
    d->st = st;
    d->begin = begin;
    d->back = back;
    d->v = kit::View(3);
    d->v.screen = true;  // a screen, not a dialog: Begin has no ring
    d->view = &d->v;
    d->menu_bar = true;  // the menu bar stays live over it (7bab:0034)
    // picked once as the screen is drawn whole; Edit Options draws over it
    static std::mt19937 dice{std::random_device{}()};
    d->lines = Quote([](int n) { return 1 + static_cast<int>(dice() % static_cast<unsigned>(n)); });
    d->Refresh();
    kit::Push(d);
}

struct Chooser : kit::Modal {
    std::vector<Scenario> list;
    screen::View v;
    int top_row = 0, cur = 0;
    std::function<void(int)> after;
    void Refresh() {
        auto& s = v.state;
        for (auto& c : v.dialog.controls) s[c.id] = w2::uidata::kNormal;
        int up = top_row > 0 ? w2::uidata::kNormal : w2::uidata::kDisabled;
        int down = top_row + 7 < static_cast<int>(list.size()) ? w2::uidata::kNormal
                                                               : w2::uidata::kDisabled;
        s[470] = up;
        s[472] = up;
        s[471] = down;
        s[473] = down;
    }
    void Draw() override {
        const kit::Rect popup_rect{80, 45, 480, 360};  // popup 19
        kit::PopupFrame(popup_rect);
        auto pic = Image(G.data_dir + "/PICS/NEWSCEN.PCK");
        gfx::SetColor(1, 1, 1);
        if (pic) gfx::Draw(pic, popup_rect.x, popup_rect.y);
        kit::SetPal(0);
        gfx::Rectangle(gfx::kFill, 112, 153, 136, 140);
        gfx::SetColor(1, 1, 1);
        const Font& f = kit::GetFont(2);
        for (int i = 0; i <= 6; i++) {
            if (top_row + i < static_cast<int>(list.size()))
                f.Colours(i == cur ? 15 : 5, 0).Draw(list[top_row + i].name, 112, 153 + 20 * i);
        }
        if (top_row + cur < static_cast<int>(list.size())) {
            const Scenario& e = list[top_row + cur];
            const Font& g5 = f.Colours(5, 0);
            kit::Centred(g5, e.name, 416, 101);
            kit::Centred(g5, e.text, 416, 174);
            kit::Right(g5, w2::Fmt("%d", e.cities), 368, 234);
            g5.Draw(w2::Fmt("%d", e.ruins), 472, 234);
            kit::Centred(g5, w2::Fmt("%d", e.players), 416, 254);
        }
        std::set<int> h = {476, 477, 478, 479, 480, 481, 482};
        kit::DrawControls(v, h);
    }
    void Take(bool ok) {
        int pick = ok ? top_row + cur : w2::kNone;
        auto a = after;
        kit::Pop(this);
        a(pick);
    }
    void Mousepressed(int x, int y, int) override {
        auto c = kit::ControlAt(v, x, y);
        if (!c || v.StateOf(c->id) == w2::uidata::kDisabled) return;
        int id = c->id;
        if (id == 474) return Take(true);
        if (id == 475) return Take(false);
        if (id >= 476 && id <= 482) {
            if (top_row + id - 476 < static_cast<int>(list.size())) cur = id - 476;
        } else if (id == 470 || id == 472) {
            top_row = std::max(0, top_row - (id == 472 ? 7 : 1));
        } else if (id == 471 || id == 473) {
            top_row = std::min(std::max(0, static_cast<int>(list.size()) - 7),
                               top_row + (id == 473 ? 7 : 1));
        }
        Refresh();
    }
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter")
            Take(true);
        else if (key == "escape")
            Take(false);
    }
};

void OpenChooser(const std::vector<Scenario>& list, int current, std::function<void(int)> after) {
    auto d = std::make_shared<Chooser>();
    d->list = list;
    d->cur = current;
    d->after = after;
    d->v = kit::View(29);
    d->view = &d->v;
    while (d->cur >= 7) {
        d->cur--;
        d->top_row++;
    }
    d->Refresh();
    kit::Push(d);
}

const int kSliderX = 384, kSliderW = 120;  // 4125:29ac

// 7f77:0332: the random world's settings, in place of the picture.
void DrawWorld() {
    kit::SetPal(3);
    gfx::Rectangle(gfx::kFill, 328, 200, 264, 225);  // 4125:2930
    const Font& f = kit::GetFont(2);
    auto art = G.screen->ArtFor(1);  // STARTBU.PCK
    for (int i = 0; i < 4; i++) {
        int y = 215 + 30 * i;
        gfx::SetColor(1, 1, 1);
        kit::Right(f, kit::Text(2, i), 376, y);
        // the slider at its place (4125:29cc), or bare for "?" (4125:2a04)
        int sy = world.set[i] ? 80 + 20 * world.sliders[i] : 220;
        if (art) gfx::Draw(art->image, gfx::NewQuad(496, sy, kSliderW, 20), kSliderX, y + 1);
        std::string shows = world.set[i] ? Format(w2::randommap::kSliderFormats[i],
                                                  w2::randommap::kSliderShows[i][world.sliders[i]])
                                         : "(?)";
        f.Draw(shows, 504, y);
    }
    f.Draw(w2::randommap::TerrainSetName(G.data_dir, world.terrain_set), 336, 340);
    f.Draw(kit::Text(3, world.allies ? 1 : 0), 336, 365);
}

// random_map_setup (7bab:10e8): make the world, showing its progress as
// 4bed:01ff does on popup 23 -- the scroll with group 136's lines, the bar
// RMAPBAR.PCK at (232, 257) growing a tenth at a time, the percentage over
// it -- then hand it on. A "?" slider is rolled, 1d7-1.
struct MakeWorldDialog : kit::Modal {
    w2::Rng rng;
    std::unique_ptr<w2::randommap::Run> run;
    kit::Done after;
    gfx::ImageP scroll, bar;
    void Update() override {
        // a phase of the generator a frame, so the bar moves as it works
        if (run->Step()) return;
        w2::randommap::Install(G.data_dir, run->GetFiles());
        auto a = after;
        kit::Pop(this);
        if (a) a();
    }
    void Draw() override {
        const kit::Rect popup_rect{160, 55, 336, 347};  // popup 23
        gfx::SetColor(1, 1, 1);
        if (scroll)
            gfx::Draw(scroll, gfx::NewQuad(0, 0, popup_rect.w, popup_rect.h), popup_rect.x,
                      popup_rect.y);
        const Font& f = kit::GetFont(2).Colours(0, 7);
        const int lines[2] = {2, 3};
        for (int i = 0; i < 2; i++) kit::Centred(f, kit::Text(0x88, lines[i]), 328, 181 + 20 * i);
        // 4bed:01b5: a black frame (216d:01fd), the scroll showing through it
        kit::SetPal(0);
        gfx::Rectangle(gfx::kLine, 232.5, 255.5, 191, 24);
        gfx::SetColor(1, 1, 1);
        int pct = run->Progress();
        int w = (pct + 10) / 10 * 16 + 16;
        if (bar) gfx::Draw(bar, gfx::NewQuad(0, 0, w, 21), 232, 257);
        kit::Centred(f, w2::Fmt("%d%%", pct), 328, 237);
    }
};

void MakeWorld(kit::Done after) {
    auto d = std::make_shared<MakeWorldDialog>();
    d->rng = w2::Rng(static_cast<double>(time(nullptr) % 1000000007));
    w2::randommap::Options o;
    o.data_dir = G.data_dir;
    o.rng = &d->rng;
    for (int i = 0; i < 4; i++)
        o.sliders[i] = world.set[i] ? world.sliders[i] : w2::randommap::kRandomSlider;
    o.allies = world.allies;
    o.terrain_set = world.terrain_set;
    d->run = std::make_unique<w2::randommap::Run>(o);
    d->after = after;
    d->scroll = Image(G.data_dir + "/PICS/BSCROLL.PCK", 10);
    d->bar = Image(G.data_dir + "/PICS/RMAPBAR.PCK", 10);
    kit::Push(d);
}

struct Menu : kit::Modal {
    std::vector<Scenario> list;
    screen::View v;
    int cur = 0;
    std::function<void(const std::string&, const w2::game::NewGameOptions&)> begin_game;
    std::function<void(std::unique_ptr<w2::Game>)> loaded;
    // 7f77:011e
    void Refresh() {
        auto& s = v.state;
        s[100] = s[101] = s[103] = w2::uidata::kNormal;
        s[102] = world.on ? w2::uidata::kDisabled : w2::uidata::kNormal;
        hidden.clear();
        if (!world.on) {
            for (int id = 104; id <= 113; id++) hidden.insert(id);
            return;
        }
        s[104] = s[105] = w2::uidata::kNormal;
        for (int i = 0; i < 4; i++) {
            s[106 + i] = w2::uidata::kNormal;
            s[110 + i] = world.set[i] ? w2::uidata::kNormal : w2::uidata::kActive;  // lit while "?"
        }
    }
    void Draw() override {
        gfx::SetColor(1, 1, 1);
        for (int q = 0; q < 4; q++) {
            auto img = Image(G.data_dir + "/PICS/STARTUP" + std::to_string(q) + ".PCK");
            if (img) gfx::Draw(img, (q % 2) * 320, (q / 2) * 240);
        }
        bool have = cur < static_cast<int>(list.size());
        if (world.on) {
            DrawWorld();
        } else if (have) {
            std::string dir = w2::Upper(list[cur].dir);
            if (auto pic = Image(G.data_dir + "/" + dir + "/PICS/SCENARIO.PCK")) {
                gfx::SetColor(1, 1, 1);
                gfx::Draw(pic, gfx::NewQuad(0, 0, 264, 225), 328, 200);
            }
        }
        if (world.on || have) {
            // 7f77:02bf: the scenario's name, or "A Random World"
            kit::SetPal(3);
            gfx::Rectangle(gfx::kFill, 336, 166, 248, 28);
            gfx::SetColor(1, 1, 1);
            kit::Centred(kit::GetFont(2), world.on ? kit::Text(0, 0) : list[cur].name, 460, 172);
        }
        kit::DrawControls(v, hidden);
    }
    void SetUp(const Scenario& sc) {
        auto keep = shared_from_this();
        auto st = NewSetup(sc);
        auto bg = begin_game;
        OpenSetup(
                st,
                [bg](std::shared_ptr<Setup> s) {
                    w2::game::NewGameOptions o;
                    for (int i = 0; i <= 9; i++)
                        o.options.emplace_back(kOptionKeys[i], s->options[i]);
                    for (int i = 0; i < 8; i++) {
                        const SideSetup& e = s->sides[i];
                        w2::game::SideSetup ss;
                        ss.computer = e.computer;
                        ss.level = e.level;
                        ss.card = e.card;
                        ss.name = e.name;
                        ss.off = e.computer && e.level == 3;
                        o.sides[i] = ss;
                    }
                    o.greatest = s->greatest;
                    bg(w2::Upper(s->sc.dir), o);
                },
                [keep]() {
                    auto m = std::static_pointer_cast<Menu>(keep);
                    m->Refresh();
                    kit::Push(keep);
                });
    }
    void Mousepressed(int x, int y, int) override {
        auto c = kit::ControlAt(v, x, y, hidden);
        if (!c || v.StateOf(c->id) == w2::uidata::kDisabled) return;
        int id = c->id;
        std::weak_ptr<Modal> self = weak_from_this();
        Menu* me = this;
        if (id == 100) {
            // 7f77:067c: a scenario chosen ends the random world
            OpenChooser(list, cur, [self, me](int i) {
                if (self.expired()) return;
                if (i != w2::kNone) {
                    me->cur = i;
                    world.on = false;
                }
                me->Refresh();
            });
        } else if (id == 101) {
            auto l = loaded;
            savegame::Load([self, l](std::unique_ptr<w2::Game> g) {
                if (auto s = self.lock()) kit::Pop(s.get());
                l(std::move(g));
            });
        } else if (id == 102) {
            world.on = true;  // 7f77:05f5
        } else if (id == 104) {
            world.terrain_set =
                    (world.terrain_set + 1) % w2::randommap::TerrainSets(G.data_dir);  // 7f77:063a
        } else if (id == 105) {
            world.allies = !world.allies;  // 7f77:0661
        } else if (id >= 106 && id <= 109) {
            // 7f77:0512: a "?" slider is set again where it was; a set one moves
            // to where it was clicked
            int i = id - 106;
            if (!world.set[i])
                world.set[i] = true;
            else
                world.sliders[i] = std::clamp((x - kSliderX) * 7 / kSliderW, 0, 6);
        } else if (id >= 110 && id <= 113) {
            world.set[id - 110] = false;  // 7f77:0571
        } else if (id == 103 && world.on) {
            // 7f77:060f: "One moment...", the world, then the sides as for any scenario
            advisor::Say(w2::cues::kMoment, [self, me]() {
                MakeWorld([self, me]() {
                    auto keep = self.lock();
                    if (!keep) return;
                    kit::Pop(me);
                    Scenario sc;
                    sc.name = kit::Text(0, 0);
                    sc.dir = w2::randommap::kDir;
                    me->SetUp(sc);
                });
            });
        } else if (id == 103 && cur < static_cast<int>(list.size())) {
            auto keep = shared_from_this();
            kit::Pop(this);
            SetUp(list[cur]);
            return;
        }
        Refresh();
    }
};
}  // namespace

void Open(std::function<void(const std::string& dir, const w2::game::NewGameOptions& opts)> begin,
          std::function<void(std::unique_ptr<w2::Game>)> loaded) {
    auto d = std::make_shared<Menu>();
    d->list = Scenarios();
    for (size_t i = 0; i < d->list.size(); i++)
        if (d->list[i].dir == "Erythea") d->cur = static_cast<int>(i);  // 4125:2a6e
    d->begin_game = begin;
    d->loaded = loaded;
    // back from a random world, the menu is still on one (7f77:0000)
    if (G.scenario == w2::randommap::kDir) world.on = true;
    d->v = kit::View(1);
    d->v.screen = true;  // a screen, not a dialog: Begin has no ring
    d->view = &d->v;
    d->menu_bar = true;
    d->Refresh();
    kit::Push(d);
}

}  // namespace start
