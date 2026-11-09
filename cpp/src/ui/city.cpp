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
#include <array>
#include <cstdlib>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "absl/strings/str_format.h"

#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/scn.hpp"

namespace city {

using w2::Format;
using w2::kNone;

namespace {
constexpr kit::Rect kPopupRect{80, 60, 480, 312};  // popup 2
constexpr kit::Rect kMap{80, 60, 224, 312};        // area screen 3, region 6
constexpr int kDialog = 6;
constexpr int kDone = 192, kDoneProd = 201, kStop = 202;
constexpr int kModeFirst = 193;
constexpr int kSlotFirst = 197;  // 197-200, Production only
constexpr int kRename = 203, kBuild = 204, kRaze = 205;
constexpr int kVectorSend = 210, kVectorMove = 211, kVectorAll = 212;
constexpr int kVectorSendLit = 214, kVectorMoveLit = 215, kVectorAllLit = 216;

// String groups the dialog's texts come from.
constexpr int kTextStats = 0x74, kTextCurrent = 0x75;
constexpr int kTextRename = 0x77, kTextRaze = 0x78, kTextBuild = 0x79;
constexpr int kTextVector = 0x9a, kTextSendHelp = 0x9b, kTextMoveHelp = 0x9c;

// Rename and Raze keep their titles in the executable, not in a string group.
constexpr const char* kRenameTitle = "Rename City";
constexpr const char* kRazeTitle = "Raze City";

constexpr std::array<int, 2> kLeftShieldAt = {312, 102};
constexpr std::array<int, 2> kRightShieldAt = {512, 102};
constexpr std::array<std::array<int, 2>, 3> kStatAt = {{{356, 106}, {356, 126}, {356, 150}}};
constexpr std::array<std::array<int, 2>, 3> kTextAt = {{{310, 259}, {310, 279}, {310, 299}}};
constexpr std::array<std::array<int, 2>, 4> kInfoSlots = {
    {{320, 190}, {376, 190}, {432, 190}, {488, 190}}};
constexpr std::array<std::array<int, 2>, 4> kProdSlots = {
    {{312, 142}, {360, 142}, {408, 142}, {456, 142}}};
constexpr std::array<std::array<int, 2>, 6> kCityText = {
    {{376, 183}, {376, 203}, {376, 231}, {376, 251}, {376, 279}, {376, 299}}};

/** @brief The index of the side whose capital the city is, or kNone. */
int CapitalOf(const w2::UiContext& ctx, const w2::City* target_city) {
    const w2::Map& map = ctx.ActiveGame().GetMap();
    for (const auto& side : map.Sides()) {
        if (side.CapitalCity(map) == target_city) return side.Index();
    }
    return kNone;
}

/** @brief Draws a side's small shield: the bottom row of BSHIELD.PCK. */
void SmallShield(w2::UiContext& ctx, int side, int x, int y) {
    ctx.Renderer().SetColor(1, 1, 1);
    ctx.Renderer().Draw(ctx.Assets().ShieldImg(), gfx::NewQuad(side * 32, 36, 32, 23), x, y);
}

/** @brief The slot the city is building from, or nullptr when it builds nothing. */
const w2::Slot* Building(const w2::City* target_city) {
    return w2::game::Producing(*target_city) && target_city->ProducingSlot() >= 0 &&
                           target_city->ProducingSlot() < target_city->SlotCount()
                   ? &target_city->SlotAt(target_city->ProducingSlot())
                   : nullptr;
}

/** @brief Draws the city's income, defence and owner; a razed city shows nothing. */
void Stats(w2::UiContext& ctx, const w2::City* target_city) {
    const Font& font = kit::GetFont(ctx, 2);
    ctx.Renderer().SetColor(1, 1, 1);
    bool razed = target_city->IsRazed();
    font.Draw(Format(kit::Text(ctx, kTextStats, 0), razed ? 0 : target_city->Income()),
              kStatAt[0][0], kStatAt[0][1]);
    font.Draw(Format(kit::Text(ctx, kTextStats, 1), razed ? 0 : target_city->Defence()),
              kStatAt[1][0], kStatAt[1][1]);
    std::string owner;
    if (razed) {
        owner = kit::Text(ctx, kTextStats, 2);
    } else if (target_city->IsNeutral()) {
        owner = kit::Text(ctx, kTextStats, 3);
    } else {
        owner = Format(kit::Text(ctx, kTextStats, 4),
                       ctx.ActiveGame().GetMap().Sides()[target_city->OwnerIndex()].Name());
    }
    font.Draw(owner, kStatAt[2][0], kStatAt[2][1]);
}

/** @brief Draws the owner's shield on both sides of the dialog's title; neutral is side 8. */
void Shields(w2::UiContext& ctx, const w2::City* target_city) {
    int side = target_city->IsNeutral() ? 8 : target_city->OwnerIndex();
    kit::Shield(ctx, side, kLeftShieldAt[0], kLeftShieldAt[1]);
    kit::Shield(ctx, side, kRightShieldAt[0], kRightShieldAt[1]);
}

/** @brief Draws the 40x40 map tile at map position (map_x, map_y) at screen (x, y). */
void Tile(w2::UiContext& ctx, int map_x, int map_y, int x, int y) {
    int tile = w2::scn::TileAt(ctx.ActiveGame().GetMap(), map_x, map_y);
    int sheet = tile / 96, cell = tile % 96;
    ctx.Renderer().SetColor(1, 1, 1);
    ctx.Renderer().Draw(ctx.Assets().Sheet(std::min(sheet, 1)),
                        gfx::NewQuad((cell % 16) * 40, (cell / 16) * 40, 40, 40), x, y);
}

/**
 * @brief The city dialog itself: one city, shown in one of four modes.
 *
 * Moving to another city closes this dialog and opens a new one on it.
 */
class CityDialog final : public kit::Modal {
  public:
    CityDialog(w2::UiContext& ctx, w2::City* target_city)
            : kit::Modal(ctx), city_(target_city), dialog_view_(kit::View(ctx, kDialog)) {
        SetDialogView(&dialog_view_);
    }

    /**
     * @brief Switches to another mode; only Info for a city that is not the player's.
     *
     * Going into Production chooses what the city builds now.
     */
    void SetMode(int new_mode) {
        if (new_mode != kInfo && city_->OwnerIndex() != Ctx().Player()->Index()) new_mode = kInfo;
        if (new_mode == kProduction && mode_ != kProduction) {
            const w2::Slot* building = Building(city_);
            chosen_ = building ? building->type : kNone;
        }
        mode_ = new_mode;
        sub_ = 0;
        Refresh();
    }

    /** @brief Draws the dialog: the map on the left, the mode's half on the right. */
    void Draw() override {
        w2::UiContext& ctx = Ctx();
        kit::Popup(ctx, kPopupRect);
        if (mode_ == kVector) {
            int filter = kNone;
            if (sub_ == 1) {
                filter = -1;
            } else if (sub_ == 2) {
                filter = static_cast<int>(w2::game::VectoredTo(ctx.ActiveGame(), city_).size());
            }
            ctx.Actions().DrawVectorMap(kMap.x, kMap.y, city_, filter,
                                        ctx.VectorSeeAll() && sub_ == 0);
        } else {
            ctx.Actions().DrawStrategicPanel(kMap.x, kMap.y, city_);
        }
        ctx.Renderer().SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(ctx, 1), city_->Name(), 432, 62);
        if (mode_ == kInfo) {
            DrawInfo();
        } else if (mode_ == kCity) {
            DrawCityMode();
        } else if (mode_ == kProduction) {
            DrawProduction();
        } else {
            DrawVector();
        }
        kit::DrawControls(ctx, dialog_view_, HiddenControls());
    }

    /** @brief Handles a left click: a control, or the map (two pixels a tile). */
    void Mousepressed(int x, int y, int) override {
        w2::UiContext& ctx = Ctx();
        const auto* control = kit::ControlAt(dialog_view_, x, y, HiddenControls());
        if (control) {
            int control_id = control->id;
            if (control_id == kDone || control_id == kDoneProd) {
                Close();
            } else if (control_id >= kModeFirst && control_id < kModeFirst + 4) {
                SetMode(control_id - kModeFirst);
            } else if (control_id >= kSlotFirst && control_id < kSlotFirst + 4) {
                Pick(control_id - kSlotFirst);
            } else if (control_id == kStop) {
                Stop();
            } else if (control_id == kRename) {
                Rename();
            } else if (control_id == kRaze) {
                Raze();
            } else if (control_id == kBuild) {
                Build();
            } else if (control_id == kVectorSend || control_id == kVectorSendLit) {
                sub_ = sub_ == 1 ? 0 : 1;
                Refresh();
            } else if (control_id == kVectorMove || control_id == kVectorMoveLit) {
                sub_ = sub_ == 2 ? 0 : 2;
                Refresh();
            } else if (control_id == kVectorAll || control_id == kVectorAllLit) {
                ctx.SetVectorSeeAll(!ctx.VectorSeeAll());
                Refresh();
            }
            return;
        }
        if (kMap.Contains(x, y)) {
            int map_x = (x - kMap.x) / 2, map_y = (y - kMap.y) / 2;
            if (mode_ == kVector) {
                MapClick(map_x, map_y);
            } else {
                PickOnMap(map_x, map_y);
            }
        }
    }

    /**
     * @brief The right button on a production slot: the army type's info box.
     * @param sub_id The slot's sub-id, 1-4.
     */
    bool Info(int sub_id, int screen_x, int screen_y) override {
        int slot = sub_id - 1;
        if (slot < 0 || slot >= city_->SlotCount()) return false;
        infobox::ArmyType(Ctx(), screen_x, screen_y, city_->SlotAt(slot).type, city_->SlotAt(slot));
        return true;
    }

    /**
     * @brief The right button off the controls: help on the map, or in Info
     * mode on the production slots (or the city picture standing in for them).
     */
    void Rightpressed(int x, int y, int screen_x, int screen_y) override {
        w2::UiContext& ctx = Ctx();
        if (kMap.Contains(x, y)) {
            int first = mode_ == kProduction ? 4 : mode_ == kVector ? 6 : 0;
            infobox::ShowLines(ctx, screen_x, screen_y, kit::Text(ctx, 0x7a, first),
                               kit::Text(ctx, 0x7a, first + 1));
            return;
        }
        if (mode_ == kInfo && x >= 308 && x < 532 && y >= 180 && y < 230) {
            if (city_->IsRazed() || (ctx.ActiveGame().GetMap().GameOptions().view_production != 0 &&
                                     city_->OwnerIndex() != ctx.Player()->Index())) {
                infobox::ShowLines(ctx, screen_x, screen_y, city_->Name(),
                                   "A picture of the city!");
                return;
            }
            int slot = std::min(3, (x - 308) / 56);
            if (slot < city_->SlotCount()) {
                infobox::ArmyType(ctx, screen_x, screen_y, city_->SlotAt(slot).type,
                                  city_->SlotAt(slot),
                                  city_->IsNeutral() ? 8 : city_->OwnerIndex());
            } else {
                infobox::ShowLines(ctx, screen_x, screen_y, city_->Name(),
                                   "Info about city production");
            }
        }
    }

    /** @brief Escape and Return close the dialog. */
    void Keypressed(const std::string& key) override {
        if (key == "escape" || key == "return" || key == "kpenter") Close();
    }

  private:
    /** @brief Whether the city is the player's and the player has cities at all. */
    bool Owned() const {
        const w2::UiContext& ctx = Ctx();
        return city_->OwnerIndex() == ctx.Player()->Index() &&
               !w2::game::SideCities(ctx.ActiveGame(), *ctx.Player()).empty();
    }

    /** @brief Sets which controls the current mode shows, and in what state. */
    void Refresh() {
        w2::UiContext& ctx = Ctx();
        auto& state = dialog_view_.state;
        bool mine = Owned();
        for (int i = 0; i <= 3; i++) {
            int button_id = kModeFirst + i;
            if (i == mode_) {
                state[button_id] = w2::uidata::kActive;
            } else if (i > 0 && !mine) {
                state[button_id] = w2::uidata::kDisabled;
            } else {
                state[button_id] = w2::uidata::kNormal;
            }
        }
        std::set<int> shown = {193, 194, 195, 196};
        if (mode_ == kProduction) {
            for (int i = 0; i <= 3; i++) {
                shown.insert(kSlotFirst + i);
                state[kSlotFirst + i] = w2::uidata::kNormal;
            }
            shown.insert(kDoneProd);
            state[kDoneProd] = w2::uidata::kNormal;
            shown.insert(kStop);
            state[kStop] =
                    w2::game::Producing(*city_) ? w2::uidata::kNormal : w2::uidata::kDisabled;
        } else {
            shown.insert(kDone);
            state[kDone] = w2::uidata::kNormal;
        }
        if (mode_ == kCity) {
            for (int button_id : {kRename, kRaze, kBuild}) {
                shown.insert(button_id);
                state[button_id] = w2::uidata::kNormal;
            }
        }
        if (mode_ == kVector) {
            int all_button = ctx.VectorSeeAll() ? kVectorAllLit : kVectorAll;
            shown.insert(all_button);
            state[all_button] = w2::uidata::kNormal;
            int send_button = sub_ == 1 ? kVectorSendLit : kVectorSend;
            shown.insert(send_button);
            state[send_button] = (sub_ == 1 || w2::game::Producing(*city_)) ? w2::uidata::kNormal
                                                                            : w2::uidata::kDisabled;
            int move_button = sub_ == 2 ? kVectorMoveLit : kVectorMove;
            shown.insert(move_button);
            state[move_button] =
                    (sub_ == 2 || !w2::game::VectoredTo(ctx.ActiveGame(), city_).empty())
                            ? w2::uidata::kNormal
                            : w2::uidata::kDisabled;
        }
        HiddenControls().clear();
        for (const auto& control : dialog_view_.dialog.controls) {
            if (!shown.count(control.id)) HiddenControls().insert(control.id);
        }
    }

    /** @brief Closes the dialog and shows the tutorial's next moment, if any. */
    void Close() {
        w2::UiContext& ctx = Ctx();
        PopSelf();
        tutorial::Show(ctx, "select");
    }

    /** @brief Chooses what the city builds: slot `slot` of its list, from 0. */
    void Pick(int slot) {
        if (slot < city_->SlotCount()) chosen_ = city_->SlotAt(slot).type;
        if (chosen_ == kNone) return;
        for (int i = 0; i < city_->SlotCount(); i++) {
            if (city_->SlotAt(i).type == chosen_) {
                w2::game::SetProduction(Ctx().ActiveGame(), *city_, i);
            }
        }
        Refresh();
    }

    /** @brief Stops the city's production. */
    void Stop() {
        w2::game::SetProduction(Ctx().ActiveGame(), *city_, kNone);
        chosen_ = kNone;
        Refresh();
    }

    /** @brief Asks for the city's new name (15 characters, 128 pixels) and renames it. */
    void Rename() {
        w2::UiContext& ctx = Ctx();
        std::weak_ptr<Modal> self = weak_from_this();
        CityDialog* dialog = this;
        w2::City* renamed = city_;
        input::Options options;
        options.title = kRenameTitle;
        options.lines = {"Type the new name for", "this city"};
        options.text = city_->Name();
        options.max_chars = 15;
        options.max_width = 128;
        options.ok = [&ctx, self, dialog, renamed](const std::string& name) {
            w2::game::RenameCity(ctx.ActiveGame(), *renamed, name);
            if (!self.expired()) dialog->SetMode(kCity);
        };
        options.cancelled = [self, dialog]() {
            if (!self.expired()) dialog->SetMode(kCity);
        };
        input::Open(ctx, options);
    }

    /** @brief Asks whether to raze the city, and razes it if the player agrees. */
    void Raze() {
        w2::UiContext& ctx = Ctx();
        std::weak_ptr<Modal> self = weak_from_this();
        CityDialog* dialog = this;
        w2::City* razed = city_;
        input::Options options;
        options.title = kRazeTitle;
        options.lines = {"Are you sure that you", "want to", "raze " + city_->Name() + "?",
                         "You won't be popular!"};
        options.confirm = true;
        options.ok = [&ctx, self, dialog, razed](const std::string&) {
            w2::game::Raze(ctx.ActiveGame(), *ctx.Player(), *razed, {}, true);
            ctx.GetViewport().StratDirty();
            if (!self.expired()) dialog->SetMode(kInfo);
        };
        options.cancelled = [self, dialog]() {
            if (!self.expired()) dialog->SetMode(kCity);
        };
        input::Open(ctx, options);
    }

    /** @brief Opens Build Production, coming back to City mode afterwards. */
    void Build() {
        w2::UiContext& ctx = Ctx();
        std::weak_ptr<Modal> self = weak_from_this();
        CityDialog* dialog = this;
        buyprod::Open(ctx, city_, [self, dialog]() {
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
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        Shields(ctx, city_);
        Stats(ctx, city_);
        int owner = city_->IsNeutral() ? 8 : city_->OwnerIndex();
        if (!city_->IsRazed() && (ctx.ActiveGame().GetMap().GameOptions().view_production == 0 ||
                                  city_->OwnerIndex() == ctx.Player()->Index())) {
            for (int i = 0; i < 4; i++) {
                bool have = i < city_->SlotCount();
                kit::Army(ctx, have ? city_->SlotAt(i).type : kNone, owner, kInfoSlots[i][0],
                          kInfoSlots[i][1], 1);
            }
        } else {
            kit::SetPal(ctx, 0);
            kit::Outline(ctx, 311, 169, 242, 88);
            kit::Bevel(ctx, 310, 168, 244, 90, 4, 2);
            ren.SetColor(1, 1, 1);
            if (ctx.Assets().CityBack()) {
                ren.Draw(ctx.Assets().CityBack(), gfx::NewQuad(0, 0, 240, 86), 312, 170);
            }
            Tile(ctx, city_->X(), city_->Y(), 392, 176);
            Tile(ctx, city_->X(), city_->Y() + 1, 392, 216);
            Tile(ctx, city_->X() + 1, city_->Y(), 432, 176);
            Tile(ctx, city_->X() + 1, city_->Y() + 1, 432, 216);
        }
        int capital_side = CapitalOf(ctx, city_);
        if (capital_side != kNone) SmallShield(ctx, capital_side, 408, 170);
        const auto* desc_lines = ctx.ActiveGame().GetMap().CityDescription(city_->Index());
        const Font& font = kit::GetFont(ctx, 2);
        ren.SetColor(1, 1, 1);
        if (desc_lines) {
            for (int i = 0; i < 3; i++) {
                if (!(*desc_lines)[i].empty()) {
                    font.Draw((*desc_lines)[i], kTextAt[i][0], kTextAt[i][1]);
                }
            }
        }
    }

    /** @brief Draws City mode: the stats and two lines on each of Rename, Raze and Build. */
    void DrawCityMode() {
        w2::UiContext& ctx = Ctx();
        Shields(ctx, city_);
        Stats(ctx, city_);
        const Font& font = kit::GetFont(ctx, 2);
        ctx.Renderer().SetColor(1, 1, 1);
        int line = 0;
        for (int group : {kTextRename, kTextRaze, kTextBuild}) {
            for (int i = 0; i <= 1; i++) {
                font.Draw(kit::Text(ctx, group, i), kCityText[line][0], kCityText[line][1]);
                line++;
            }
        }
    }

    /**
     * @brief Draws Production mode: what is built now, how long it takes and
     * where it goes, the four slots, and the chosen army type's figures.
     */
    void DrawProduction() {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        const Font& font = kit::GetFont(ctx, 2);
        int owner = city_->IsNeutral() ? 8 : city_->OwnerIndex();
        int capital_side = CapitalOf(ctx, city_);
        if (capital_side != kNone) SmallShield(ctx, capital_side, 312, 110);
        ren.SetColor(1, 1, 1);
        kit::Right(font, kit::Text(ctx, kTextCurrent, 0), 408, 110);
        const w2::Slot* building = Building(city_);
        kit::Army(ctx, building ? building->type : kNone, owner, 416, 104, 1);
        std::string turns_left =
                w2::game::Producing(*city_) ? absl::StrFormat("%dt", city_->Countdown()) : "-";
        ren.SetColor(1, 1, 1);
        if (city_->IsVectored()) {
            std::string where;
            if (city_->IsVectoredToStandard()) {
                where = kit::Text(
                        ctx, kTextCurrent,
                        w2::game::StandardAt(ctx.ActiveGame(), *ctx.Player()).first != kNone ? 2
                                                                                             : 3);
            } else {
                w2::City* dest = ctx.ActiveGame().GetMap().CityById(city_->VectoredCityId());
                where = dest ? dest->Name() : kit::Text(ctx, kTextCurrent, 3);
            }
            font.Draw(turns_left + kit::Text(ctx, kTextCurrent, 1), 456, 102);
            font.Draw(where, 456, 122);
        } else {
            font.Draw(turns_left, 456, 110);
        }
        const w2::Slot* chosen_slot = nullptr;
        for (int i = 0; i < 4; i++) {
            bool have = i < city_->SlotCount();
            bool mine = have && city_->SlotAt(i).type == chosen_;
            if (mine) chosen_slot = &city_->SlotAt(i);
            kit::Army(ctx, have ? city_->SlotAt(i).type : kNone, owner, kProdSlots[i][0],
                      kProdSlots[i][1], mine ? ctx.Player()->Index() + 2 : 1);
        }
        if (chosen_slot) {
            ren.SetColor(1, 1, 1);
            ren.Draw(ctx.Assets().BigArmy(), 320, 182);
            int x = 320 + 136;
            font.Draw(chosen_slot->name, x, 182);
            font.Draw(absl::StrFormat("Time: %d", chosen_slot->time), x, 212);
            font.Draw(absl::StrFormat("Cost: %d", chosen_slot->cost), x, 232);
            font.Draw(absl::StrFormat("Strength: %d", chosen_slot->strength), x, 252);
            font.Draw(absl::StrFormat("Move: %d", chosen_slot->move), x, 272);
        }
    }

    /**
     * @brief Draws Vector mode: what is built, the armies on the road out of
     * and into the city, and the help text for the current step.
     */
    void DrawVector() {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        const Font& font = kit::GetFont(ctx, 2);
        int owner = city_->IsNeutral() ? 8 : city_->OwnerIndex();
        ren.SetColor(1, 1, 1);
        kit::Right(font, kit::Text(ctx, kTextVector, 0), 360, 109);
        const w2::Slot* building = Building(city_);
        kit::Army(ctx, building ? building->type : kNone, owner, 368, 103, 1);
        ren.SetColor(1, 1, 1);
        font.Draw(w2::game::Producing(*city_)
                          ? Format(kit::Text(ctx, kTextVector, 1), city_->Countdown())
                          : "-",
                  408, 109);
        w2::Army* out = nullptr;
        w2::Army* on_road = nullptr;
        std::vector<w2::Army*> next;
        std::vector<w2::Army*> after;
        for (w2::Army* army : ctx.ActiveGame().Armies()) {
            if (army->InTransit() && army->OwnerIndex() == ctx.Player()->Index()) {
                if (city_->IsVectored() && army->HomeCity() == city_->Id() &&
                    army->TransitState()->dest == city_->VectorDestination()) {
                    if (army->TransitState()->turns >= 2) {
                        out = army;
                    } else {
                        on_road = army;
                    }
                }
                if (army->TransitState()->dest == w2::VectorTarget{city_->Id()} &&
                    army->HomeCity() != city_->Id()) {
                    auto& row = army->TransitState()->turns >= 2 ? after : next;
                    if (row.size() < 4) row.push_back(army);
                }
            }
        }
        kit::Army(ctx, out ? out->TypeIndex() : kNone, owner, 432, 103, 1);
        kit::Army(ctx, on_road ? on_road->TypeIndex() : kNone, owner, 472, 103, 1);
        ren.SetColor(1, 1, 1);
        kit::Right(font, kit::Text(ctx, kTextVector, 2), 392, 155);
        kit::Right(font, kit::Text(ctx, kTextVector, 3), 392, 188);
        for (int i = 0; i < 4; i++) {
            int x = 400 + i * 40;
            kit::Army(ctx, i < static_cast<int>(next.size()) ? next[i]->TypeIndex() : kNone,
                      ctx.Player()->Index(), x, 149, 1);
            kit::Army(ctx, i < static_cast<int>(after.size()) ? after[i]->TypeIndex() : kNone,
                      ctx.Player()->Index(), x, 182, 1);
        }
        ren.SetColor(1, 1, 1);
        int send_help_line = sub_ == 1 ? 2 : 0;
        font.Draw(kit::Text(ctx, kTextSendHelp, send_help_line), 368, 221);
        font.Draw(kit::Text(ctx, kTextSendHelp, send_help_line + 1), 368, 241);
        int move_help_line = sub_ == 2 ? 2 : 0;
        font.Draw(kit::Text(ctx, kTextMoveHelp, move_help_line), 368, 272);
        font.Draw(kit::Text(ctx, kTextMoveHelp, move_help_line + 1), 368, 292);
    }

    /**
     * @brief Moves the dialog to another city.
     * @param new_mode The mode to open it in; kNone keeps the current one.
     */
    void SwitchTo(w2::City* target, int new_mode) {
        w2::UiContext& ctx = Ctx();
        int keep = new_mode == kNone ? mode_ : new_mode;
        PopSelf();
        Open(ctx, target, keep);
    }

    /**
     * @brief Sends what this city builds to the player's city nearest map
     * position (map_x, map_y), or to the planted standard if that is nearer.
     */
    void SendTo(int map_x, int map_y) {
        if (!w2::game::Producing(*city_)) return;
        w2::UiContext& ctx = Ctx();
        w2::Game& game = ctx.ActiveGame();
        w2::Side* player = ctx.Player();
        w2::City* target = w2::game::NearestCity(game, map_x, map_y, player);
        if (!target) return;
        auto [standard_x, standard_y] = w2::game::StandardAt(game, *player);
        bool to_standard =
                standard_x != kNone &&
                std::max(std::abs(standard_x - map_x), std::abs(standard_y - map_y)) <
                        std::max(std::abs(target->X() - map_x), std::abs(target->Y() - map_y));
        if (to_standard) {
            w2::game::VectorToStandard(game, *city_, *player);
        } else {
            w2::game::Vector(game, *city_, target);
        }
    }

    /**
     * @brief Handles a click on the map in Info, City or Production mode.
     *
     * Shift-click in Production sends the production there; any other click
     * moves the dialog to the nearest city the mode may show.
     */
    void PickOnMap(int map_x, int map_y) {
        w2::UiContext& ctx = Ctx();
        bool shift = ctx.Keyboard().IsDown({"lshift", "rshift"});
        if (mode_ == kProduction && shift) {
            SendTo(map_x, map_y);
            Refresh();
            return;
        }
        w2::Game& game = ctx.ActiveGame();
        w2::Side* player = ctx.Player();
        w2::City* target = nullptr;
        if (mode_ == kInfo) {
            target = w2::game::NearestCity(game, map_x, map_y, nullptr, player);
        } else if (mode_ == kProduction) {
            target = w2::game::NearestCity(game, map_x, map_y, player);
        } else {
            target = w2::game::NearestCity(game, map_x, map_y, player, player);
        }
        if (target && target != city_) SwitchTo(target, kNone);
    }

    /**
     * @brief Handles a click on the map in Vector mode.
     *
     * Sends the production there (step 1 or shift), moves every city vectored
     * here to vector there instead (step 2, within the limit of cities
     * vectored to one), and otherwise moves the dialog to the city clicked.
     */
    void MapClick(int map_x, int map_y) {
        w2::UiContext& ctx = Ctx();
        w2::Game& game = ctx.ActiveGame();
        w2::City* target = w2::game::NearestCity(game, map_x, map_y, ctx.Player());
        if (!target) return;
        bool shift = ctx.Keyboard().IsDown({"lshift", "rshift"});
        int was = sub_;
        if (shift || was == 1) {
            SendTo(map_x, map_y);
            if (shift) {
                sub_ = 0;
                Refresh();
                return;
            }
        } else if (was == 2) {
            auto incoming = w2::game::VectoredTo(game, city_);
            auto there = w2::game::VectoredTo(game, target);
            bool allowed = !incoming.empty() && static_cast<int>(incoming.size() + there.size()) <=
                                                        w2::game::kMaxVectoredTo;
            if (std::find(incoming.begin(), incoming.end(), target) != incoming.end()) {
                allowed = false;
            }
            if (allowed) {
                for (w2::City* source : incoming) source->VectorToCity(target->Id());
            }
        }
        sub_ = 0;
        if (was != 1 && target != city_) {
            SwitchTo(target, kVector);
            return;
        }
        Refresh();
    }

    w2::City* city_ = nullptr;  ///< The city shown.
    screen::View dialog_view_;  ///< Dialog 6 and its controls' states.
    int mode_ = kNone;          ///< kInfo, kCity, kProduction or kVector.
    int sub_ = 0;               ///< Vector mode's step: 0 none, 1 choosing send, 2 choosing move.
    int chosen_ = kNone;        ///< The army type chosen in Production mode, or kNone.
};
}  // namespace

/**
 * @brief Opens the city dialog on a city.
 * @param mode The mode to open in; kNone picks Production for one of the
 *     player's own cities and Info for any other.
 *
 * Also runs the tutorial's moments on production.
 */
void Open(w2::UiContext& ctx, w2::City* target_city, int mode) {
    auto dialog = std::make_shared<CityDialog>(ctx, target_city);
    kit::Push(ctx, dialog);
    dialog->SetMode(
            mode == kNone
                    ? (target_city->OwnerIndex() == ctx.Player()->Index() ? kProduction : kInfo)
                    : mode);
    tutorial::MomentList moments = {"prod"};
    if (w2::game::SideCities(ctx.ActiveGame(), *ctx.Player()).size() >= 2) {
        moments.push_back("prod2");
    }
    tutorial::Chain(ctx, moments);
}

}  // namespace city
