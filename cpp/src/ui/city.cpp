// The city dialog: dialog 6 over popup 2, (80, 60) 480x312.
//
// One frame, four modes, switched by the row of buttons along the foot
// (193-196) -- 7204:0000 takes the mode as its argument:
//
//   0  Info        any city; what anyone can see of it
//   1  City        Rename, Raze, Build Prod
//   2  Production  what it builds
//   3  Vector      where what it builds goes
//
// Modes 1-3 are only for a city of the side's own (7204:03a9). The left half
// is the strategic map; the right half is auto_ui_city_info (7204:06de), one
// case per mode, every position out of its data segment (4125:0ea0 on).
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
const kit::Rect kR{80, 60, 480, 312};    // popup 2
const kit::Rect kMap{80, 60, 224, 312};  // area screen 3, region 6
const int kDialog = 6;
const int kDone = 192, kDoneProd = 201, kStop = 202;
const int kModeFirst = 193;
const int kSlotFirst = 197;  // 197-200, Production only
const int kRename = 203, kBuild = 204, kRaze = 205;
const int kVSend = 210, kVMove = 211, kVAll = 212;
const int kVSendLit = 214, kVMoveLit = 215, kVAllLit = 216;

const int kSStats = 0x74, kSCurrent = 0x75;
const int kSRename = 0x77, kSRaze = 0x78, kSBuild = 0x79;
const int kSVector = 0x9a, kSHelP1 = 0x9b, kSHelP2 = 0x9c;

// Rename and Raze keep their words in the executable (4125:1003, 4125:0a6c).
const char* rename_title = "Rename City";
const char* raze_title = "Raze City";

const int kShieldL[2] = {312, 102}, kShieldR[2] = {512, 102};
const int kStatAt[3][2] = {{356, 106}, {356, 126}, {356, 150}};
const int kTextAt[3][2] = {{310, 259}, {310, 279}, {310, 299}};
const int kInfoSlots[4][2] = {{320, 190}, {376, 190}, {432, 190}, {488, 190}};
const int kProdSlots[4][2] = {{312, 142}, {360, 142}, {408, 142}, {456, 142}};
const int kCityText[6][2] = {{376, 183}, {376, 203}, {376, 231},
                             {376, 251}, {376, 279}, {376, 299}};

bool vector_see_all = false;

int CapitalOf(const w2::City* city) {
    for (auto& s : G.g->map->sides)
        if (s.capital == city) return s.index;
    return kNone;
}

// The small shield (8611:0bf7, size 4): BSHIELD.PCK's bottom row.
void SmallShield(int side, int x, int y) {
    gfx::SetColor(1, 1, 1);
    gfx::Draw(G.shield_img, gfx::NewQuad(side * 32, 36, 32, 23), x, y);
}

const w2::Slot* Building(const w2::City* city) {
    return w2::game::Producing(*city) && city->producing < static_cast<int>(city->slots.size())
                   ? &city->slots[city->producing]
                   : nullptr;
}

// Income, defence and owner (7204:0727, :1769).
void Stats(const w2::City* city) {
    const Font& f = kit::GetFont(2);
    gfx::SetColor(1, 1, 1);
    bool razed = city->razed;
    f.Draw(Format(kit::Text(kSStats, 0), razed ? 0 : city->income), kStatAt[0][0], kStatAt[0][1]);
    f.Draw(Format(kit::Text(kSStats, 1), razed ? 0 : city->defence), kStatAt[1][0], kStatAt[1][1]);
    std::string owner;
    if (razed)
        owner = kit::Text(kSStats, 2);
    else if (city->owner_index == kNone)
        owner = kit::Text(kSStats, 3);
    else
        owner = Format(kit::Text(kSStats, 4), G.g->map->sides[city->owner_index].name);
    f.Draw(owner, kStatAt[2][0], kStatAt[2][1]);
}

void Shields(const w2::City* city) {
    int side = city->owner_index == kNone ? 8 : city->owner_index;
    kit::Shield(side, kShieldL[0], kShieldL[1]);
    kit::Shield(side, kShieldR[0], kShieldR[1]);
}

void Tile(int mx, int my, int x, int y) {
    int t = w2::scn::TileAt(*G.g->map, mx, my);
    int sheet = t / 96, i = t % 96;
    gfx::SetColor(1, 1, 1);
    gfx::Draw(G.sheets[std::min(sheet, 1)], gfx::NewQuad((i % 16) * 40, (i / 16) * 40, 40, 40), x,
              y);
}

struct CityDialog : kit::Modal {
    w2::City* city = nullptr;
    screen::View v;
    int mode = kNone, sub = 0;
    int chosen = kNone;

    bool Owned() const {
        return city->owner_index == G.player->index &&
               !w2::game::SideCities(*G.g, *G.player).empty();
    }

    // Which controls this mode shows, and in what state (7204:03a9).
    void Refresh() {
        auto& st = v.state;
        bool mine = Owned();
        for (int i = 0; i <= 3; i++) {
            int id = kModeFirst + i;
            if (i == mode)
                st[id] = w2::uidata::kActive;
            else if (i > 0 && !mine)
                st[id] = w2::uidata::kDisabled;
            else
                st[id] = w2::uidata::kNormal;
        }
        std::set<int> shown = {193, 194, 195, 196};
        if (mode == kProduction) {
            for (int i = 0; i <= 3; i++) {
                shown.insert(kSlotFirst + i);
                st[kSlotFirst + i] = w2::uidata::kNormal;
            }
            shown.insert(kDoneProd);
            st[kDoneProd] = w2::uidata::kNormal;
            shown.insert(kStop);
            st[kStop] = w2::game::Producing(*city) ? w2::uidata::kNormal : w2::uidata::kDisabled;
        } else {
            shown.insert(kDone);
            st[kDone] = w2::uidata::kNormal;
        }
        if (mode == kCity) {
            for (int id : {kRename, kRaze, kBuild}) {
                shown.insert(id);
                st[id] = w2::uidata::kNormal;
            }
        }
        if (mode == kVector) {
            int all = vector_see_all ? kVAllLit : kVAll;
            shown.insert(all);
            st[all] = w2::uidata::kNormal;
            int send = sub == 1 ? kVSendLit : kVSend;
            shown.insert(send);
            st[send] = (sub == 1 || w2::game::Producing(*city)) ? w2::uidata::kNormal
                                                                : w2::uidata::kDisabled;
            int mv = sub == 2 ? kVMoveLit : kVMove;
            shown.insert(mv);
            st[mv] = (sub == 2 || !w2::game::VectoredTo(*G.g, city).empty())
                             ? w2::uidata::kNormal
                             : w2::uidata::kDisabled;
        }
        hidden.clear();
        for (auto& c : v.dialog.controls)
            if (!shown.count(c.id)) hidden.insert(c.id);
    }

    void SetMode(int m) {
        if (m != kInfo && city->owner_index != G.player->index) m = kInfo;
        // 7204:0000: going into Production chooses what the city builds now
        if (m == kProduction && mode != kProduction) {
            const w2::Slot* b = Building(city);
            chosen = b ? b->type : kNone;
        }
        mode = m;
        sub = 0;
        Refresh();
    }

    void Close() {
        kit::Pop(this);
        tutorial::Show("select");  // 7204:0321
    }

    // Choose what the city builds: slot n of the list, from 0 (7087:00c8).
    void Pick(int n) {
        if (n < static_cast<int>(city->slots.size())) chosen = city->slots[n].type;
        if (chosen == kNone) return;
        for (size_t i = 0; i < city->slots.size(); i++)
            if (city->slots[i].type == chosen)
                w2::game::SetProduction(*G.g, *city, static_cast<int>(i));
        Refresh();
    }

    void Stop() {
        w2::game::SetProduction(*G.g, *city, kNone);
        chosen = kNone;
        Refresh();
    }

    void Rename() {
        std::weak_ptr<Modal> self = weak_from_this();
        CityDialog* me = this;
        w2::City* c = city;
        input::Options o;
        o.title = rename_title;
        o.lines = {"Type the new name for", "this city"};
        o.text = city->name;
        o.max_chars = 15;
        o.max_width = 128;  // 7204:2013
        o.ok = [self, me, c](const std::string& name) {
            w2::game::RenameCity(*G.g, *c, name);
            if (!self.expired()) me->SetMode(kCity);
        };
        o.cancelled = [self, me]() {
            if (!self.expired()) me->SetMode(kCity);
        };
        input::Open(o);
    }

    void Raze() {
        std::weak_ptr<Modal> self = weak_from_this();
        CityDialog* me = this;
        w2::City* c = city;
        input::Options o;
        o.title = raze_title;
        o.lines = {"Are you sure that you", "want to", "raze " + city->name + "?",
                   "You won't be popular!"};
        o.confirm = true;
        o.ok = [self, me, c](const std::string&) {
            w2::game::Raze(*G.g, *G.player, *c, {}, true);
            front::StratDirty();
            if (!self.expired()) me->SetMode(kInfo);
        };
        o.cancelled = [self, me]() {
            if (!self.expired()) me->SetMode(kCity);
        };
        input::Open(o);
    }

    void Build() {
        std::weak_ptr<Modal> self = weak_from_this();
        CityDialog* me = this;
        buyprod::Open(city, [self, me]() {
            if (!self.expired()) me->SetMode(kCity);
        });
    }

    void DrawInfo() {
        Shields(city);
        Stats(city);
        int owner = city->owner_index == kNone ? 8 : city->owner_index;
        if (!city->razed &&
            (G.g->map->options.view_production == 0 || city->owner_index == G.player->index)) {
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
            if (G.city_back) gfx::Draw(G.city_back, gfx::NewQuad(0, 0, 240, 86), 312, 170);
            Tile(city->x, city->y, 392, 176);
            Tile(city->x, city->y + 1, 392, 216);
            Tile(city->x + 1, city->y, 432, 176);
            Tile(city->x + 1, city->y + 1, 432, 216);
        }
        int cap = CapitalOf(city);
        if (cap != kNone) SmallShield(cap, 408, 170);
        auto it = G.g->map->city_text.find(city->index);
        const Font& f = kit::GetFont(2);
        gfx::SetColor(1, 1, 1);
        if (it != G.g->map->city_text.end())
            for (int i = 0; i < 3; i++)
                if (!it->second[i].empty()) f.Draw(it->second[i], kTextAt[i][0], kTextAt[i][1]);
    }

    void DrawCityMode() {
        Shields(city);
        Stats(city);
        const Font& f = kit::GetFont(2);
        gfx::SetColor(1, 1, 1);
        int k = 0;
        for (int group : {kSRename, kSRaze, kSBuild}) {
            for (int i = 0; i <= 1; i++) {
                f.Draw(kit::Text(group, i), kCityText[k][0], kCityText[k][1]);
                k++;
            }
        }
    }

    void DrawProduction() {
        const Font& f = kit::GetFont(2);
        int owner = city->owner_index == kNone ? 8 : city->owner_index;
        int cap = CapitalOf(city);
        if (cap != kNone) SmallShield(cap, 312, 110);
        gfx::SetColor(1, 1, 1);
        kit::Right(f, kit::Text(kSCurrent, 0), 408, 110);
        const w2::Slot* b = Building(city);
        kit::Army(b ? b->type : kNone, owner, 416, 104, 1);
        std::string text = w2::game::Producing(*city) ? w2::Fmt("%dt", city->countdown) : "-";
        gfx::SetColor(1, 1, 1);
        if (city->vector_to != kNone) {
            std::string where;
            if (city->vector_to == w2::game::kStandard) {
                where = kit::Text(kSCurrent,
                                  w2::game::StandardAt(*G.g, *G.player).first != kNone ? 2 : 3);
            } else {
                w2::City* dest = G.g->map->City(city->vector_to);
                where = dest ? dest->name : kit::Text(kSCurrent, 3);
            }
            f.Draw(text + kit::Text(kSCurrent, 1), 456, 102);
            f.Draw(where, 456, 122);
        } else {
            f.Draw(text, 456, 110);
        }
        const w2::Slot* ch = nullptr;
        for (int i = 0; i < 4; i++) {
            bool have = i < static_cast<int>(city->slots.size());
            bool mine = have && city->slots[i].type == chosen;
            if (mine) ch = &city->slots[i];
            kit::Army(have ? city->slots[i].type : kNone, owner, kProdSlots[i][0], kProdSlots[i][1],
                      mine ? G.player->index + 2 : 1);
        }
        if (ch) {
            gfx::SetColor(1, 1, 1);
            gfx::Draw(G.big_army, 320, 182);
            int x = 320 + 136;  // 4125:060a
            f.Draw(ch->name, x, 182);
            f.Draw(w2::Fmt("Time: %d", ch->time), x, 212);
            f.Draw(w2::Fmt("Cost: %d", ch->cost), x, 232);
            f.Draw(w2::Fmt("Strength: %d", ch->strength), x, 252);
            f.Draw(w2::Fmt("Move: %d", ch->move), x, 272);
        }
    }

    void DrawVector() {
        const Font& f = kit::GetFont(2);
        int owner = city->owner_index == kNone ? 8 : city->owner_index;
        gfx::SetColor(1, 1, 1);
        kit::Right(f, kit::Text(kSVector, 0), 360, 109);
        const w2::Slot* b = Building(city);
        kit::Army(b ? b->type : kNone, owner, 368, 103, 1);
        gfx::SetColor(1, 1, 1);
        f.Draw(w2::game::Producing(*city) ? Format(kit::Text(kSVector, 1), city->countdown) : "-",
               408, 109);
        w2::Army* out = nullptr;
        w2::Army* on_road = nullptr;
        std::vector<w2::Army*> next, after;
        for (w2::Army* a : G.g->armies) {
            if (a->transit && a->owner == G.player->index) {
                if (a->home_city == city->index && a->transit->dest == city->vector_to) {
                    if (a->transit->turns >= 2)
                        out = a;
                    else
                        on_road = a;
                }
                if (a->transit->dest == city->index && a->home_city != city->index) {
                    auto& row = a->transit->turns >= 2 ? after : next;
                    if (row.size() < 4) row.push_back(a);
                }
            }
        }
        kit::Army(out ? out->type : kNone, owner, 432, 103, 1);
        kit::Army(on_road ? on_road->type : kNone, owner, 472, 103, 1);
        gfx::SetColor(1, 1, 1);
        kit::Right(f, kit::Text(kSVector, 2), 392, 155);
        kit::Right(f, kit::Text(kSVector, 3), 392, 188);
        for (int i = 0; i < 4; i++) {
            int x = 400 + i * 40;
            kit::Army(i < static_cast<int>(next.size()) ? next[i]->type : kNone, G.player->index, x,
                      149, 1);
            kit::Army(i < static_cast<int>(after.size()) ? after[i]->type : kNone, G.player->index,
                      x, 182, 1);
        }
        gfx::SetColor(1, 1, 1);
        int a0 = sub == 1 ? 2 : 0;
        f.Draw(kit::Text(kSHelP1, a0), 368, 221);
        f.Draw(kit::Text(kSHelP1, a0 + 1), 368, 241);
        int b0 = sub == 2 ? 2 : 0;
        f.Draw(kit::Text(kSHelP2, b0), 368, 272);
        f.Draw(kit::Text(kSHelP2, b0 + 1), 368, 292);
    }

    void Draw() override {
        kit::Popup(kR);
        if (mode == kVector) {
            int filter = kNone;
            if (sub == 1)
                filter = -1;
            else if (sub == 2)
                filter = static_cast<int>(w2::game::VectoredTo(*G.g, city).size());
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

    // Move the dialog to another city, in the mode it is in (7204:0000).
    void SwitchTo(w2::City* target, int m) {
        int keep = m == kNone ? mode : m;
        kit::Pop(this);
        Open(target, keep);
    }

    // Send what this city builds to the side's city nearest (mx, my), or its
    // planted standard if that is nearer (828e:0651).
    void SendTo(int mx, int my) {
        if (!w2::game::Producing(*city)) return;
        w2::City* target = w2::game::NearestCity(*G.g, mx, my, G.player);
        if (!target) return;
        auto [sx, sy] = w2::game::StandardAt(*G.g, *G.player);
        bool to_standard =
                sx != kNone && std::max(std::abs(sx - mx), std::abs(sy - my)) <
                                       std::max(std::abs(target->x - mx), std::abs(target->y - my));
        if (to_standard)
            w2::game::VectorToStandard(*G.g, *city, *G.player);
        else
            w2::game::Vector(*G.g, *city, target);
    }

    // A click on the map in Info, City or Production (7204:1afa).
    void PickOnMap(int mx, int my) {
        bool shift = keys::IsDown({"lshift", "rshift"});
        if (mode == kProduction && shift) {
            SendTo(mx, my);
            Refresh();
            return;
        }
        w2::City* target;
        if (mode == kInfo)
            target = w2::game::NearestCity(*G.g, mx, my, nullptr, G.player);
        else if (mode == kProduction)
            target = w2::game::NearestCity(*G.g, mx, my, G.player);
        else
            target = w2::game::NearestCity(*G.g, mx, my, G.player, G.player);
        if (target && target != city) SwitchTo(target, kNone);
    }

    // A click on the map in Vector mode (7087:072e, 7087:028b).
    void MapClick(int mx, int my) {
        w2::City* target = w2::game::NearestCity(*G.g, mx, my, G.player);
        if (!target) return;
        bool shift = keys::IsDown({"lshift", "rshift"});
        int was = sub;
        if (shift || was == 1) {
            SendTo(mx, my);
            if (shift) {
                sub = 0;
                Refresh();
                return;
            }
        } else if (was == 2) {
            auto incoming = w2::game::VectoredTo(*G.g, city);
            auto there = w2::game::VectoredTo(*G.g, target);
            bool ok = !incoming.empty() &&
                      static_cast<int>(incoming.size() + there.size()) <= w2::game::kMaxVectoredTo;
            if (std::find(incoming.begin(), incoming.end(), target) != incoming.end()) ok = false;
            if (ok)
                for (w2::City* c : incoming) c->vector_to = target->index;
        }
        sub = 0;
        if (was != 1 && target != city) {
            SwitchTo(target, kVector);
            return;
        }
        Refresh();
    }

    void Mousepressed(int x, int y, int) override {
        auto c = kit::ControlAt(v, x, y, hidden);
        if (c) {
            int id = c->id;
            if (id == kDone || id == kDoneProd)
                Close();
            else if (id >= kModeFirst && id < kModeFirst + 4)
                SetMode(id - kModeFirst);
            else if (id >= kSlotFirst && id < kSlotFirst + 4)
                Pick(id - kSlotFirst);
            else if (id == kStop)
                Stop();
            else if (id == kRename)
                Rename();
            else if (id == kRaze)
                Raze();
            else if (id == kBuild)
                Build();
            else if (id == kVSend || id == kVSendLit) {
                sub = sub == 1 ? 0 : 1;
                Refresh();
            } else if (id == kVMove || id == kVMoveLit) {
                sub = sub == 2 ? 0 : 2;
                Refresh();
            } else if (id == kVAll || id == kVAllLit) {
                vector_see_all = !vector_see_all;
                Refresh();
            }
            return;
        }
        if (kMap.Contains(x, y)) {
            int mx = (x - kMap.x) / 2, my = (y - kMap.y) / 2;
            if (mode == kVector)
                MapClick(mx, my);
            else
                PickOnMap(mx, my);
        }
    }

    // the right button on a production slot (sub-ids 1-4, 54bd:00df)
    bool Info(int s, int sx, int sy) override {
        int n = s - 1;
        if (n < 0 || n >= static_cast<int>(city->slots.size())) return false;
        infobox::ArmyType(sx, sy, city->slots[n].type, city->slots[n]);
        return true;
    }

    // The right button off the controls (1726:0009, screen 3).
    void Rightpressed(int x, int y, int sx, int sy) override {
        if (kMap.Contains(x, y)) {
            int first = mode == kProduction ? 4 : mode == kVector ? 6 : 0;
            infobox::ShowLines(sx, sy, kit::Text(0x7a, first), kit::Text(0x7a, first + 1));
            return;
        }
        if (mode == kInfo && x >= 308 && x < 532 && y >= 180 && y < 230) {
            if (city->razed ||
                (G.g->map->options.view_production != 0 && city->owner_index != G.player->index)) {
                infobox::ShowLines(sx, sy, city->name, "A picture of the city!");  // 4125:0fe7
                return;
            }
            int n = std::min(3, (x - 308) / 56);
            if (n < static_cast<int>(city->slots.size()))
                infobox::ArmyType(sx, sy, city->slots[n].type, city->slots[n],
                                  city->owner_index == kNone ? 8 : city->owner_index);
            else
                infobox::ShowLines(sx, sy, city->name, "Info about city production");  // 4125:0fcc
        }
    }

    void Keypressed(const std::string& key) override {
        if (key == "escape" || key == "return" || key == "kpenter") Close();
    }
};
}  // namespace

void Open(w2::City* c, int mode) {
    auto d = std::make_shared<CityDialog>();
    d->city = c;
    d->v = kit::View(kDialog);
    d->view = &d->v;
    kit::Push(d);
    d->SetMode(mode == kNone ? (c->owner_index == G.player->index ? kProduction : kInfo) : mode);
    // the tutorial on production (7204:025d)
    std::vector<std::string> moments = {"prod"};
    if (w2::game::SideCities(*G.g, *G.player).size() >= 2) moments.push_back("prod2");
    tutorial::Chain(moments);
}

}  // namespace city
