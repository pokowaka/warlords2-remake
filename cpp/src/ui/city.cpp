/**
 * @file
 * @brief The city dialog: dialog 6 over popup 2, at (80, 60), 480x312.
 *
 * One frame with four modes, switched by the row of buttons along its foot
 * (controls 193-196):
 *
 *   0  Info        any city; what anyone can see of it
 *   1  City        Rename, Raze, Build Prod
 *   2  Production  what it builds
 *   3  Vector      where what it builds goes
 *
 * Modes 1-3 are only for a city of the player's own side. The left half of
 * the dialog is the strategic map; the right half is drawn per mode, every
 * position taken from the original program's layout tables.
 */
#include <algorithm>
#include <cstdlib>

#include "platform/keys.hpp"
#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/scn.hpp"

namespace city {

using w2::Format;
using w2::kNone;

namespace {
const kit::Rect kPopupRect{80, 60, 480, 312};  // popup 2
const kit::Rect kMap{80, 60, 224, 312};        // area screen 3, region 6
const int kDialog = 6;
const int kDone = 192, kDoneProd = 201, kStop = 202;
const int kModeFirst = 193;
const int kSlotFirst = 197;  // 197-200, Production only
const int kRename = 203, kBuild = 204, kRaze = 205;
const int kVectorSend = 210, kVectorMove = 211, kVectorAll = 212;
const int kVectorSendLit = 214, kVectorMoveLit = 215, kVectorAllLit = 216;

// String groups the dialog's texts come from.
const int kTextStats = 0x74, kTextCurrent = 0x75;
const int kTextRename = 0x77, kTextRaze = 0x78, kTextBuild = 0x79;
const int kTextVector = 0x9a, kTextSendHelp = 0x9b, kTextMoveHelp = 0x9c;

// Rename and Raze keep their titles in the executable, not in a string group.
const char* rename_title = "Rename City";
const char* raze_title = "Raze City";

const int kLeftShieldAt[2] = {312, 102}, kRightShieldAt[2] = {512, 102};
const int kStatAt[3][2] = {{356, 106}, {356, 126}, {356, 150}};
const int kTextAt[3][2] = {{310, 259}, {310, 279}, {310, 299}};
const int kInfoSlots[4][2] = {{320, 190}, {376, 190}, {432, 190}, {488, 190}};
const int kProdSlots[4][2] = {{312, 142}, {360, 142}, {408, 142}, {456, 142}};
const int kCityText[6][2] = {{376, 183}, {376, 203}, {376, 231},
                             {376, 251}, {376, 279}, {376, 299}};

/// Vector mode's "see all" toggle; kept for the session, across dialogs.
bool vector_see_all = false;

/** @brief The index of the side whose capital the city is, or kNone. */
int CapitalOf(const w2::City* city) {
    for (auto& side : g_app.g->map->sides)
        if (side.capital == city) return side.index;
    return kNone;
}

/** @brief Draws a side's small shield: the bottom row of BSHIELD.PCK. */
void SmallShield(int side, int x, int y) {
    gfx::SetColor(1, 1, 1);
    gfx::Draw(g_app.shield_img, gfx::NewQuad(side * 32, 36, 32, 23), x, y);
}

/** @brief The slot the city is building from, or nullptr when it builds nothing. */
const w2::Slot* Building(const w2::City* city) {
    return w2::game::Producing(*city) && city->producing < static_cast<int>(city->slots.size())
                   ? &city->slots[city->producing]
                   : nullptr;
}

/** @brief Draws the city's income, defence and owner; a razed city shows nothing. */
void Stats(const w2::City* city) {
    const Font& font = kit::GetFont(2);
    gfx::SetColor(1, 1, 1);
    bool razed = city->razed;
    font.Draw(Format(kit::Text(kTextStats, 0), razed ? 0 : city->income), kStatAt[0][0],
              kStatAt[0][1]);
    font.Draw(Format(kit::Text(kTextStats, 1), razed ? 0 : city->defence), kStatAt[1][0],
              kStatAt[1][1]);
    std::string owner;
    if (razed)
        owner = kit::Text(kTextStats, 2);
    else if (city->owner_index == kNone)
        owner = kit::Text(kTextStats, 3);
    else
        owner = Format(kit::Text(kTextStats, 4), g_app.g->map->sides[city->owner_index].name);
    font.Draw(owner, kStatAt[2][0], kStatAt[2][1]);
}

/** @brief Draws the owner's shield on both sides of the dialog's title; neutral is side 8. */
void Shields(const w2::City* city) {
    int side = city->owner_index == kNone ? 8 : city->owner_index;
    kit::Shield(side, kLeftShieldAt[0], kLeftShieldAt[1]);
    kit::Shield(side, kRightShieldAt[0], kRightShieldAt[1]);
}

/** @brief Draws the 40x40 map tile at map position (map_x, map_y) at screen (x, y). */
void Tile(int map_x, int map_y, int x, int y) {
    int tile = w2::scn::TileAt(*g_app.g->map, map_x, map_y);
    int sheet = tile / 96, cell = tile % 96;
    gfx::SetColor(1, 1, 1);
    gfx::Draw(g_app.sheets[std::min(sheet, 1)],
              gfx::NewQuad((cell % 16) * 40, (cell / 16) * 40, 40, 40), x, y);
}

/**
 * @brief The city dialog itself: one city, shown in one of four modes.
 *
 * Moving to another city closes this dialog and opens a new one on it.
 */
struct CityDialog : kit::Modal {
    w2::City* city = nullptr;  ///< The city shown.
    screen::View v;            ///< Dialog 6 and its controls' states.
    int mode = kNone;          ///< kInfo, kCity, kProduction or kVector.
    int sub = 0;         ///< Vector mode's step: 0 none, 1 choosing where to send, 2 where to move.
    int chosen = kNone;  ///< The army type chosen in Production mode, or kNone.

    /** @brief Whether the city is the player's and the player has cities at all. */
    bool Owned() const {
        return city->owner_index == g_app.player->index &&
               !w2::game::SideCities(*g_app.g, *g_app.player).empty();
    }

    /** @brief Sets which controls the current mode shows, and in what state. */
    void Refresh() {
        auto& state = v.state;
        bool mine = Owned();
        for (int i = 0; i <= 3; i++) {
            int button_id = kModeFirst + i;
            if (i == mode)
                state[button_id] = w2::uidata::kActive;
            else if (i > 0 && !mine)
                state[button_id] = w2::uidata::kDisabled;
            else
                state[button_id] = w2::uidata::kNormal;
        }
        std::set<int> shown = {193, 194, 195, 196};
        if (mode == kProduction) {
            for (int i = 0; i <= 3; i++) {
                shown.insert(kSlotFirst + i);
                state[kSlotFirst + i] = w2::uidata::kNormal;
            }
            shown.insert(kDoneProd);
            state[kDoneProd] = w2::uidata::kNormal;
            shown.insert(kStop);
            state[kStop] = w2::game::Producing(*city) ? w2::uidata::kNormal : w2::uidata::kDisabled;
        } else {
            shown.insert(kDone);
            state[kDone] = w2::uidata::kNormal;
        }
        if (mode == kCity) {
            for (int button_id : {kRename, kRaze, kBuild}) {
                shown.insert(button_id);
                state[button_id] = w2::uidata::kNormal;
            }
        }
        if (mode == kVector) {
            int all_button = vector_see_all ? kVectorAllLit : kVectorAll;
            shown.insert(all_button);
            state[all_button] = w2::uidata::kNormal;
            int send_button = sub == 1 ? kVectorSendLit : kVectorSend;
            shown.insert(send_button);
            state[send_button] = (sub == 1 || w2::game::Producing(*city)) ? w2::uidata::kNormal
                                                                          : w2::uidata::kDisabled;
            int move_button = sub == 2 ? kVectorMoveLit : kVectorMove;
            shown.insert(move_button);
            state[move_button] = (sub == 2 || !w2::game::VectoredTo(*g_app.g, city).empty())
                                         ? w2::uidata::kNormal
                                         : w2::uidata::kDisabled;
        }
        hidden.clear();
        for (auto& control : v.dialog.controls)
            if (!shown.count(control.id)) hidden.insert(control.id);
    }

    /**
     * @brief Switches to another mode; only Info for a city that is not the player's.
     *
     * Going into Production chooses what the city builds now.
     */
    void SetMode(int new_mode) {
        if (new_mode != kInfo && city->owner_index != g_app.player->index) new_mode = kInfo;
        if (new_mode == kProduction && mode != kProduction) {
            const w2::Slot* building = Building(city);
            chosen = building ? building->type : kNone;
        }
        mode = new_mode;
        sub = 0;
        Refresh();
    }

    /** @brief Closes the dialog and shows the tutorial's next moment, if any. */
    void Close() {
        kit::Pop(this);
        tutorial::Show("select");
    }

    /** @brief Chooses what the city builds: slot `slot` of its list, from 0. */
    void Pick(int slot) {
        if (slot < static_cast<int>(city->slots.size())) chosen = city->slots[slot].type;
        if (chosen == kNone) return;
        for (size_t i = 0; i < city->slots.size(); i++)
            if (city->slots[i].type == chosen)
                w2::game::SetProduction(*g_app.g, *city, static_cast<int>(i));
        Refresh();
    }

    /** @brief Stops the city's production. */
    void Stop() {
        w2::game::SetProduction(*g_app.g, *city, kNone);
        chosen = kNone;
        Refresh();
    }

    /** @brief Asks for the city's new name (15 characters, 128 pixels) and renames it. */
    void Rename() {
        std::weak_ptr<Modal> self = weak_from_this();
        CityDialog* dialog = this;
        w2::City* renamed = city;
        input::Options options;
        options.title = rename_title;
        options.lines = {"Type the new name for", "this city"};
        options.text = city->name;
        options.max_chars = 15;
        options.max_width = 128;
        options.ok = [self, dialog, renamed](const std::string& name) {
            w2::game::RenameCity(*g_app.g, *renamed, name);
            if (!self.expired()) dialog->SetMode(kCity);
        };
        options.cancelled = [self, dialog]() {
            if (!self.expired()) dialog->SetMode(kCity);
        };
        input::Open(options);
    }

    /** @brief Asks whether to raze the city, and razes it if the player agrees. */
    void Raze() {
        std::weak_ptr<Modal> self = weak_from_this();
        CityDialog* dialog = this;
        w2::City* razed = city;
        input::Options options;
        options.title = raze_title;
        options.lines = {"Are you sure that you", "want to", "raze " + city->name + "?",
                         "You won't be popular!"};
        options.confirm = true;
        options.ok = [self, dialog, razed](const std::string&) {
            w2::game::Raze(*g_app.g, *g_app.player, *razed, {}, true);
            front::StratDirty();
            if (!self.expired()) dialog->SetMode(kInfo);
        };
        options.cancelled = [self, dialog]() {
            if (!self.expired()) dialog->SetMode(kCity);
        };
        input::Open(options);
    }

    /** @brief Opens Build Production, coming back to City mode afterwards. */
    void Build() {
        std::weak_ptr<Modal> self = weak_from_this();
        CityDialog* dialog = this;
        buyprod::Open(city, [self, dialog]() {
            if (!self.expired()) dialog->SetMode(kCity);
        });
    }

    /**
     * @brief Draws Info mode: the stats, what the city can build and its texts.
     *
     * When the View Production option hides other sides' production, or the
     * city is razed, a picture of the city's four tiles stands in for the
     * slots.
     */
    void DrawInfo() {
        Shields(city);
        Stats(city);
        int owner = city->owner_index == kNone ? 8 : city->owner_index;
        if (!city->razed && (g_app.g->map->options.view_production == 0 ||
                             city->owner_index == g_app.player->index)) {
            for (int i = 0; i < 4; i++) {
                bool have = i < static_cast<int>(city->slots.size());
                kit::Army(have ? city->slots[i].type : kNone, owner, kInfoSlots[i][0],
                          kInfoSlots[i][1], 1);
            }
        } else {
            kit::SetPal(0);
            kit::Outline(311, 169, 242, 88);
            kit::Bevel(310, 168, 244, 90, 4, 2);
            gfx::SetColor(1, 1, 1);
            if (g_app.city_back) gfx::Draw(g_app.city_back, gfx::NewQuad(0, 0, 240, 86), 312, 170);
            Tile(city->x, city->y, 392, 176);
            Tile(city->x, city->y + 1, 392, 216);
            Tile(city->x + 1, city->y, 432, 176);
            Tile(city->x + 1, city->y + 1, 432, 216);
        }
        int capital_side = CapitalOf(city);
        if (capital_side != kNone) SmallShield(capital_side, 408, 170);
        auto text = g_app.g->map->city_text.find(city->index);
        const Font& font = kit::GetFont(2);
        gfx::SetColor(1, 1, 1);
        if (text != g_app.g->map->city_text.end())
            for (int i = 0; i < 3; i++)
                if (!text->second[i].empty())
                    font.Draw(text->second[i], kTextAt[i][0], kTextAt[i][1]);
    }

    /** @brief Draws City mode: the stats and two lines on each of Rename, Raze and Build. */
    void DrawCityMode() {
        Shields(city);
        Stats(city);
        const Font& font = kit::GetFont(2);
        gfx::SetColor(1, 1, 1);
        int line = 0;
        for (int group : {kTextRename, kTextRaze, kTextBuild}) {
            for (int i = 0; i <= 1; i++) {
                font.Draw(kit::Text(group, i), kCityText[line][0], kCityText[line][1]);
                line++;
            }
        }
    }

    /**
     * @brief Draws Production mode: what is built now, how long it takes and
     * where it goes, the four slots, and the chosen army type's figures.
     */
    void DrawProduction() {
        const Font& font = kit::GetFont(2);
        int owner = city->owner_index == kNone ? 8 : city->owner_index;
        int capital_side = CapitalOf(city);
        if (capital_side != kNone) SmallShield(capital_side, 312, 110);
        gfx::SetColor(1, 1, 1);
        kit::Right(font, kit::Text(kTextCurrent, 0), 408, 110);
        const w2::Slot* building = Building(city);
        kit::Army(building ? building->type : kNone, owner, 416, 104, 1);
        std::string turns_left = w2::game::Producing(*city) ? w2::Fmt("%dt", city->countdown) : "-";
        gfx::SetColor(1, 1, 1);
        if (city->vector_to != kNone) {
            std::string where;
            if (city->vector_to == w2::game::kStandard) {
                where = kit::Text(
                        kTextCurrent,
                        w2::game::StandardAt(*g_app.g, *g_app.player).first != kNone ? 2 : 3);
            } else {
                w2::City* dest = g_app.g->map->City(city->vector_to);
                where = dest ? dest->name : kit::Text(kTextCurrent, 3);
            }
            font.Draw(turns_left + kit::Text(kTextCurrent, 1), 456, 102);
            font.Draw(where, 456, 122);
        } else {
            font.Draw(turns_left, 456, 110);
        }
        const w2::Slot* chosen_slot = nullptr;
        for (int i = 0; i < 4; i++) {
            bool have = i < static_cast<int>(city->slots.size());
            bool mine = have && city->slots[i].type == chosen;
            if (mine) chosen_slot = &city->slots[i];
            kit::Army(have ? city->slots[i].type : kNone, owner, kProdSlots[i][0], kProdSlots[i][1],
                      mine ? g_app.player->index + 2 : 1);
        }
        if (chosen_slot) {
            gfx::SetColor(1, 1, 1);
            gfx::Draw(g_app.big_army, 320, 182);
            int x = 320 + 136;
            font.Draw(chosen_slot->name, x, 182);
            font.Draw(w2::Fmt("Time: %d", chosen_slot->time), x, 212);
            font.Draw(w2::Fmt("Cost: %d", chosen_slot->cost), x, 232);
            font.Draw(w2::Fmt("Strength: %d", chosen_slot->strength), x, 252);
            font.Draw(w2::Fmt("Move: %d", chosen_slot->move), x, 272);
        }
    }

    /**
     * @brief Draws Vector mode: what is built, the armies on the road out of
     * and into the city, and the help text for the current step.
     */
    void DrawVector() {
        const Font& font = kit::GetFont(2);
        int owner = city->owner_index == kNone ? 8 : city->owner_index;
        gfx::SetColor(1, 1, 1);
        kit::Right(font, kit::Text(kTextVector, 0), 360, 109);
        const w2::Slot* building = Building(city);
        kit::Army(building ? building->type : kNone, owner, 368, 103, 1);
        gfx::SetColor(1, 1, 1);
        font.Draw(w2::game::Producing(*city) ? Format(kit::Text(kTextVector, 1), city->countdown)
                                             : "-",
                  408, 109);
        w2::Army* out = nullptr;
        w2::Army* on_road = nullptr;
        std::vector<w2::Army*> next, after;
        for (w2::Army* army : g_app.g->armies) {
            if (army->transit && army->owner == g_app.player->index) {
                if (army->home_city == city->index && army->transit->dest == city->vector_to) {
                    if (army->transit->turns >= 2)
                        out = army;
                    else
                        on_road = army;
                }
                if (army->transit->dest == city->index && army->home_city != city->index) {
                    auto& row = army->transit->turns >= 2 ? after : next;
                    if (row.size() < 4) row.push_back(army);
                }
            }
        }
        kit::Army(out ? out->type : kNone, owner, 432, 103, 1);
        kit::Army(on_road ? on_road->type : kNone, owner, 472, 103, 1);
        gfx::SetColor(1, 1, 1);
        kit::Right(font, kit::Text(kTextVector, 2), 392, 155);
        kit::Right(font, kit::Text(kTextVector, 3), 392, 188);
        for (int i = 0; i < 4; i++) {
            int x = 400 + i * 40;
            kit::Army(i < static_cast<int>(next.size()) ? next[i]->type : kNone,
                      g_app.player->index, x, 149, 1);
            kit::Army(i < static_cast<int>(after.size()) ? after[i]->type : kNone,
                      g_app.player->index, x, 182, 1);
        }
        gfx::SetColor(1, 1, 1);
        int send_help_line = sub == 1 ? 2 : 0;
        font.Draw(kit::Text(kTextSendHelp, send_help_line), 368, 221);
        font.Draw(kit::Text(kTextSendHelp, send_help_line + 1), 368, 241);
        int move_help_line = sub == 2 ? 2 : 0;
        font.Draw(kit::Text(kTextMoveHelp, move_help_line), 368, 272);
        font.Draw(kit::Text(kTextMoveHelp, move_help_line + 1), 368, 292);
    }

    /** @brief Draws the dialog: the map on the left, the mode's half on the right. */
    void Draw() override {
        kit::Popup(kPopupRect);
        if (mode == kVector) {
            int filter = kNone;
            if (sub == 1)
                filter = -1;
            else if (sub == 2)
                filter = static_cast<int>(w2::game::VectoredTo(*g_app.g, city).size());
            front::DrawVectorMap(kMap.x, kMap.y, city, filter, vector_see_all && sub == 0);
        } else {
            front::DrawStrategicPanel(kMap.x, kMap.y, city);
        }
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), city->name, 432, 62);
        if (mode == kInfo)
            DrawInfo();
        else if (mode == kCity)
            DrawCityMode();
        else if (mode == kProduction)
            DrawProduction();
        else
            DrawVector();
        kit::DrawControls(v, hidden);
    }

    /**
     * @brief Moves the dialog to another city.
     * @param new_mode The mode to open it in; kNone keeps the current one.
     */
    void SwitchTo(w2::City* target, int new_mode) {
        int keep = new_mode == kNone ? mode : new_mode;
        kit::Pop(this);
        Open(target, keep);
    }

    /**
     * @brief Sends what this city builds to the player's city nearest map
     * position (map_x, map_y), or to the planted standard if that is nearer.
     */
    void SendTo(int map_x, int map_y) {
        if (!w2::game::Producing(*city)) return;
        w2::City* target = w2::game::NearestCity(*g_app.g, map_x, map_y, g_app.player);
        if (!target) return;
        auto [standard_x, standard_y] = w2::game::StandardAt(*g_app.g, *g_app.player);
        bool to_standard =
                standard_x != kNone &&
                std::max(std::abs(standard_x - map_x), std::abs(standard_y - map_y)) <
                        std::max(std::abs(target->x - map_x), std::abs(target->y - map_y));
        if (to_standard)
            w2::game::VectorToStandard(*g_app.g, *city, *g_app.player);
        else
            w2::game::Vector(*g_app.g, *city, target);
    }

    /**
     * @brief Handles a click on the map in Info, City or Production mode.
     *
     * Shift-click in Production sends the production there; any other click
     * moves the dialog to the nearest city the mode may show.
     */
    void PickOnMap(int map_x, int map_y) {
        bool shift = keys::IsDown({"lshift", "rshift"});
        if (mode == kProduction && shift) {
            SendTo(map_x, map_y);
            Refresh();
            return;
        }
        w2::City* target;
        if (mode == kInfo)
            target = w2::game::NearestCity(*g_app.g, map_x, map_y, nullptr, g_app.player);
        else if (mode == kProduction)
            target = w2::game::NearestCity(*g_app.g, map_x, map_y, g_app.player);
        else
            target = w2::game::NearestCity(*g_app.g, map_x, map_y, g_app.player, g_app.player);
        if (target && target != city) SwitchTo(target, kNone);
    }

    /**
     * @brief Handles a click on the map in Vector mode.
     *
     * Sends the production there (step 1 or shift), moves every city vectored
     * here to vector there instead (step 2, within the limit of cities
     * vectored to one), and otherwise moves the dialog to the city clicked.
     */
    void MapClick(int map_x, int map_y) {
        w2::City* target = w2::game::NearestCity(*g_app.g, map_x, map_y, g_app.player);
        if (!target) return;
        bool shift = keys::IsDown({"lshift", "rshift"});
        int was = sub;
        if (shift || was == 1) {
            SendTo(map_x, map_y);
            if (shift) {
                sub = 0;
                Refresh();
                return;
            }
        } else if (was == 2) {
            auto incoming = w2::game::VectoredTo(*g_app.g, city);
            auto there = w2::game::VectoredTo(*g_app.g, target);
            bool allowed = !incoming.empty() && static_cast<int>(incoming.size() + there.size()) <=
                                                        w2::game::kMaxVectoredTo;
            if (std::find(incoming.begin(), incoming.end(), target) != incoming.end())
                allowed = false;
            if (allowed)
                for (w2::City* source : incoming) source->vector_to = target->index;
        }
        sub = 0;
        if (was != 1 && target != city) {
            SwitchTo(target, kVector);
            return;
        }
        Refresh();
    }

    /** @brief Handles a left click: a control, or the map (two pixels a tile). */
    void Mousepressed(int x, int y, int) override {
        auto control = kit::ControlAt(v, x, y, hidden);
        if (control) {
            int control_id = control->id;
            if (control_id == kDone || control_id == kDoneProd)
                Close();
            else if (control_id >= kModeFirst && control_id < kModeFirst + 4)
                SetMode(control_id - kModeFirst);
            else if (control_id >= kSlotFirst && control_id < kSlotFirst + 4)
                Pick(control_id - kSlotFirst);
            else if (control_id == kStop)
                Stop();
            else if (control_id == kRename)
                Rename();
            else if (control_id == kRaze)
                Raze();
            else if (control_id == kBuild)
                Build();
            else if (control_id == kVectorSend || control_id == kVectorSendLit) {
                sub = sub == 1 ? 0 : 1;
                Refresh();
            } else if (control_id == kVectorMove || control_id == kVectorMoveLit) {
                sub = sub == 2 ? 0 : 2;
                Refresh();
            } else if (control_id == kVectorAll || control_id == kVectorAllLit) {
                vector_see_all = !vector_see_all;
                Refresh();
            }
            return;
        }
        if (kMap.Contains(x, y)) {
            int map_x = (x - kMap.x) / 2, map_y = (y - kMap.y) / 2;
            if (mode == kVector)
                MapClick(map_x, map_y);
            else
                PickOnMap(map_x, map_y);
        }
    }

    /**
     * @brief The right button on a production slot: the army type's info box.
     * @param sub_id The slot's sub-id, 1-4.
     */
    bool Info(int sub_id, int screen_x, int screen_y) override {
        int slot = sub_id - 1;
        if (slot < 0 || slot >= static_cast<int>(city->slots.size())) return false;
        infobox::ArmyType(screen_x, screen_y, city->slots[slot].type, city->slots[slot]);
        return true;
    }

    /**
     * @brief The right button off the controls: help on the map, or in Info
     * mode on the production slots (or the city picture standing in for them).
     */
    void Rightpressed(int x, int y, int screen_x, int screen_y) override {
        if (kMap.Contains(x, y)) {
            int first = mode == kProduction ? 4 : mode == kVector ? 6 : 0;
            infobox::ShowLines(screen_x, screen_y, kit::Text(0x7a, first),
                               kit::Text(0x7a, first + 1));
            return;
        }
        if (mode == kInfo && x >= 308 && x < 532 && y >= 180 && y < 230) {
            if (city->razed || (g_app.g->map->options.view_production != 0 &&
                                city->owner_index != g_app.player->index)) {
                infobox::ShowLines(screen_x, screen_y, city->name, "A picture of the city!");
                return;
            }
            int slot = std::min(3, (x - 308) / 56);
            if (slot < static_cast<int>(city->slots.size()))
                infobox::ArmyType(screen_x, screen_y, city->slots[slot].type, city->slots[slot],
                                  city->owner_index == kNone ? 8 : city->owner_index);
            else
                infobox::ShowLines(screen_x, screen_y, city->name, "Info about city production");
        }
    }

    /** @brief Escape and Return close the dialog. */
    void Keypressed(const std::string& key) override {
        if (key == "escape" || key == "return" || key == "kpenter") Close();
    }
};
}  // namespace

/**
 * @brief Opens the city dialog on a city.
 * @param mode The mode to open in; kNone picks Production for one of the
 *     player's own cities and Info for any other.
 *
 * Also runs the tutorial's moments on production.
 */
void Open(w2::City* city, int mode) {
    auto dialog = std::make_shared<CityDialog>();
    dialog->city = city;
    dialog->v = kit::View(kDialog);
    dialog->view = &dialog->v;
    kit::Push(dialog);
    dialog->SetMode(mode == kNone ? (city->owner_index == g_app.player->index ? kProduction : kInfo)
                                  : mode);
    std::vector<std::string> moments = {"prod"};
    if (w2::game::SideCities(*g_app.g, *g_app.player).size() >= 2) moments.push_back("prod2");
    tutorial::Chain(moments);
}

}  // namespace city
