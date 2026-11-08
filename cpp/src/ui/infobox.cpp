// What the right button shows off the map: a box that stays up while the
// button is held, and goes when it comes up (740d:11cf).
//
// Two boards, blitted through their masks:
//   POPUP.PCK (bitmap 29), 256x75: two lines -- the first in colour 7, the
//     second in white, centred on the box's middle less 8, at y + 11 and
//     y + 35 (740d:1201, 740d:1158).
//   POPUP3.PCK (bitmap 40), 240x128: an army (ui_army_info, 740d:0626) or an
//     army type (ui_army_type_info, 740d:032a): the name, the army on the
//     grey ring with a non-hero's medals round it, four numbers, and how the
//     type moves.
//
// A right-button press on a control looks it up in HELP\WARLORD2.HLP
// (54bd:0000): when the record's sub-id is 0 its title and description are
// the two lines; otherwise the sub-id says what the control stands for and
// the dialog it is on answers through its info().
#include <algorithm>
#include <map>

#include "front/pckimage.hpp"
#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/armytype.hpp"
#include "warlords/bytes.hpp"

namespace infobox {

const Board kLines{29, "POPUP.PCK", 256, 75, 1};
const Board kArmy{40, "POPUP3.PCK", 240, 128, 10};
const Board kPopuP2{36, "POPUP2.PCK", 256, 75, 1};

namespace {
std::map<int, gfx::ImageP> boards;

const int kMoveFly[2] = {184, 30}, kMoveBoth[2] = {216, 30}, kMoveWoods[2] = {248, 30},
          kMoveHills[2] = {152, 30};

struct Box : kit::Modal {
    Board which;
    int bx = 0, by = 0;
    std::function<void(int, int)> paint;
    void Draw() override {
        auto b = BoardImage(which);
        gfx::SetColor(1, 1, 1);
        if (b) gfx::Draw(b, gfx::NewQuad(0, 0, which.w, which.h), bx, by);
        gfx::SetColor(1, 1, 1);
        paint(bx, by);
    }
    bool Mousereleased(int, int, int) override {
        kit::Pop(this);
        return true;
    }
    void Mousepressed(int, int, int) override { kit::Pop(this); }
    void Keypressed(const std::string&) override { kit::Pop(this); }
};

void Show(const Board& which, int sx, int sy, std::function<void(int, int)> paint) {
    auto d = std::make_shared<Box>();
    d->which = which;
    auto [bx, by] = Place(which, sx, sy);
    d->bx = bx;
    d->by = by;
    d->paint = paint;
    d->infobox = true;
    kit::Push(d);
}

void ArmyBoard(int x, int y, const std::string& name, int type_id, int side, int medals) {
    const Font& f = kit::GetFont(2);
    kit::Centred(f, name, x + 112, y + 8);
    kit::Army(type_id, side, x + 96, y + 31, 1);
    (void)medals;  // the remake keeps no medals yet
    const w2::ArmyType* t = G.g->types.ById(type_id);
    const int* src = nullptr;
    if (t && t->flies)
        src = kMoveFly;
    else if (t && t->woods_move && t->hills_move)
        src = kMoveBoth;
    else if (t && t->woods_move)
        src = kMoveWoods;
    else if (t && t->hills_move)
        src = kMoveHills;
    if (src) {
        gfx::SetColor(1, 1, 1);
        gfx::Draw(G.abits, gfx::NewQuad(src[0], src[1], 32, 10), x + 96, y + 103);
    }
}

void Numbers(int x, int y, const std::string& a, const std::string& b, const std::string& c,
             const std::string& d) {
    const Font& f = kit::GetFont(2);
    gfx::SetColor(1, 1, 1);
    kit::Right(f, a, x + 104, y + 66);
    f.Draw(b, x + 120, y + 66);
    kit::Right(f, c, x + 104, y + 86);
    f.Draw(d, x + 120, y + 86);
}

struct HelpRecord {
    int sub = 0;
    std::string title, text;
};
std::map<int, HelpRecord>& HelpFile() {
    static std::map<int, HelpRecord> out;
    static bool loaded = false;
    if (loaded) return out;
    loaded = true;
    // HELP\WARLORD2.HLP (docs/formats/hlp.md): a u16 count, then 104-byte
    // records -- u16 id, u16 sub-id, title and description, 50 bytes each.
    // Only the first record with an id counts.
    if (auto s = w2::ReadFile(G.data_dir + "/HELP/WARLORD2.HLP")) {
        int n = w2::U16(*s, 0);
        for (int i = 0; i < n; i++) {
            size_t at = 2 + static_cast<size_t>(i) * 104;
            if (at + 104 > s->size()) break;
            int id = w2::U16(*s, at);
            if (!out.count(id))
                out[id] = HelpRecord{w2::U16(*s, at + 2), w2::Cstr(*s, at + 4, 50),
                                     w2::Cstr(*s, at + 54, 50)};
        }
    }
    return out;
}
}  // namespace

gfx::ImageP BoardImage(const Board& which) {
    auto it = boards.find(which.bitmap);
    if (it != boards.end()) return it->second;
    gfx::ImageP img;
    try {
        img = pckimage::Load(G.data_dir + "/PICS/" + which.file, G.palette, which.key);
    } catch (...) {
    }
    return boards[which.bitmap] = img;
}

std::pair<int, int> Place(const Board& which, int sx, int sy) {
    const auto& l = G.layout;
    int hw = which.w / 2, hh = which.h / 2;
    int cx = (sx + 4) / 8 * 8;
    cx = std::max(hw, std::min(l.w - hw, cx));
    int cy = std::max(hh, std::min(l.h - 2 - hh, sy));
    return {cx - hw - l.dialog.x, cy - hh - l.dialog.y};
}

void ShowLines(int sx, int sy, const std::string& title, const std::string& text) {
    Show(kLines, sx, sy, [title, text](int x, int y) {
        const Font& f = kit::GetFont(2);
        kit::Centred(f.Colours(7, 0), title, x + kLines.w / 2 - 8, y + 11);
        kit::Centred(f, text, x + kLines.w / 2 - 8, y + 35);
    });
}

void Army(int sx, int sy, const w2::Army* a) {
    const w2::ArmyType* t = G.g->types.ById(a->type);
    bool hero = a->Hero();
    std::string name = hero ? a->name : (t ? t->name : "");
    w2::Army copy = *a;
    int side = G.player->index;
    Show(kArmy, sx, sy, [name, copy, side](int x, int y) {
        ArmyBoard(x, y, name, copy.type, side, 0);
        Numbers(x, y, w2::Fmt("Strength: %d", copy.strength),  // 4125:10a8
                w2::Fmt("Movement: %d", copy.max_moves),       // 4125:10b5
                w2::Fmt("Remain: %d", copy.moves),             // 4125:10c2
                w2::Fmt("Upkeep: %d", copy.upkeep));           // 4125:10cd
    });
}

void ArmyType(int sx, int sy, int type_id, const w2::Slot& s, int side) {
    const w2::ArmyType* t = G.g->types.ById(type_id);
    std::string name = t ? t->name : "";
    int sd = side == w2::kNone ? G.player->index : side;
    w2::Slot slot = s;
    Show(kArmy, sx, sy, [name, type_id, sd, slot](int x, int y) {
        ArmyBoard(x, y, name, type_id, sd, 0);
        Numbers(x, y, w2::Fmt("Strength: %d", slot.strength),  // 4125:107c
                w2::Fmt("Movement: %d", slot.move),            // 4125:1089
                w2::Fmt("Time: %d", slot.time),                // 4125:1096
                w2::Fmt("Cost: %d", slot.cost));               // 4125:109f
    });
}

bool ExplainControl(int sx, int sy, int id, kit::Modal* d) {
    auto& recs = HelpFile();
    auto it = recs.find(id);
    if (it == recs.end()) return false;
    const HelpRecord& rec = it->second;
    if (rec.sub == 0) {
        ShowLines(sx, sy, rec.title, rec.text);
        return true;
    }
    int sub = rec.sub;
    if (sub >= 33 && sub <= 36) {
        // the configurable buttons (545c:03f8): the menu item each one runs
        const auto& ui = G.screen->ui;
        auto item = ui.shortcuts.find(sub - 33);
        std::string name;
        if (item != ui.shortcuts.end()) {
            auto n = ui.shortcut_names.find(item->second);
            if (n != ui.shortcut_names.end()) name = n->second;
        }
        ShowLines(sx, sy, "- User-Defined Button -", name);  // 4125:05e6
        return true;
    }
    if (d) return d->Info(sub, sx, sy);
    return false;
}

const w2::uidata::Control* ControlAt(const screen::View& v, int x, int y,
                                     const std::set<int>* hidden) {
    const w2::uidata::Control* found = nullptr;
    for (const auto& c : v.dialog.controls) {
        if (c.w > 0 && c.h > 0 && !(hidden && hidden->count(c.id)) && x >= c.x && x < c.x + c.w &&
            y >= c.y && y < c.y + c.h) {
            found = &c;
        }
    }
    return found;
}

}  // namespace infobox
