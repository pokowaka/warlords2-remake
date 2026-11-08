// Hero > Search: what a ruin, a temple or a sage says (site_search,
// 6536:0000), and the game's plain message box.
//
// A ruin (6536:01ab) opens popup 4 with "Searching" and SEARCH.PCK in a black
// frame; its story is told a line at a time from (128, 300), each waiting
// for a key or a click; then dialog 13: Done (292) and, when an item was
// found and left on the ground, Take (293).
//
// A temple (4976:0000) is popup 9 -- TEMPLE.PCK -- with its name and two
// lines of greeting, over dialog 15: Bless (328) and Quest (329).
//
// A sage (6536:0aa0) is announced in the ruin popup, then has a popup of its
// own -- popup 2, the strategic map, dialog 10 -- offering where the rich
// sites nearby lie (Items, 279), a gem (Money, 280) or a patch of the hidden
// map (Maps, 281); one of them, then Done (282).
//
// The message box (8065:1160) is popup 5 -- (144, 179) 352x64 -- closed by
// any key or click.
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
const kit::Rect kMsg{144, 179, 352, 64};  // popup 5

struct Message : kit::Modal {
    std::string line1, line2;
    bool one = false;
    kit::Done after;
    void Close() {
        auto a = after;
        kit::Pop(this);
        if (a) a();
    }
    void Draw() override {
        kit::Popup(kMsg);
        gfx::SetColor(1, 1, 1);
        const Font& f = kit::GetFont(2);
        if (one) {
            kit::Centred(f, line1, 320, 201);
            return;
        }
        if (!line1.empty()) kit::Centred(f, line1, 320, 190);
        if (!line2.empty()) kit::Centred(f, line2, 320, 212);
    }
    void Mousepressed(int, int, int) override { Close(); }
    void Keypressed(const std::string&) override { Close(); }
};

const kit::Rect kRuin{120, 50, 400, 360};  // popup 4
const int kRuinDialog = 13, kDone = 292, kTake = 293;

std::vector<std::string> RuinLines(const w2::SearchResult& r) {
    std::vector<std::string> lines;
    std::string name = r.hero ? r.hero->name : "";
    const w2::Monster* monster = r.monster ? r.monster : r.guardian;
    if (r.kind != "allies") {
        if (monster) {
            lines.push_back(Format(kit::Text(0x33, 0), name, monster->name));
            lines.push_back(kit::Text(0x33, r.kind == "killed" ? 1 : 2));
        } else {
            lines.push_back(kit::Text(0x32, 0));
        }
    }
    if (r.kind == "item" && r.item) {
        lines.push_back(Format(kit::Text(0x34, 0), name, r.item->name));
    } else if (r.kind == "gold") {
        lines.push_back(Format(kit::Text(0x35, 0), name, r.gold));
    } else if (r.kind == "allies") {
        // the number rolled (6536:01ab), placed or not
        int n = r.count > 0 ? r.count : static_cast<int>(r.armies.size());
        std::string tname = r.type ? r.type->name : "";
        lines.push_back(n == 1 ? Format(kit::Text(0x36, 0), tname, name)
                               : Format(kit::Text(0x36, 1), n, tname, name));
    }
    return lines;
}

// The ruin popup telling a story. With `after`, it has no buttons: the click
// after the last line closes it and runs `after`.
struct Ruin : kit::Modal {
    std::vector<std::string> lines;
    size_t shown = 1;
    screen::View v;
    bool guarded = false;
    bool can_take = false;
    w2::Army* hero = nullptr;
    kit::Done after;

    bool Done() const { return shown >= lines.size(); }
    void Advance() {
        // 6536:01ab: the orchestra comes in as the guardian's line is read on
        if (guarded && shown == 1) sound::Effect("orch");
        shown++;
    }
    void Close() {
        kit::Pop(this);
        w2::quest::Notify(*G.g, *G.player, "item");
    }
    void Draw() override {
        kit::Popup(kRuin);
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), "Searching", 320, 53);  // 4125:0c90
        gfx::Draw(G.search_pic, 160, 92);
        kit::SetPal(0);
        kit::Outline(159, 91, 322, 202);
        const Font& f = kit::GetFont(2);
        gfx::SetColor(1, 1, 1);
        for (size_t i = 0; i < shown && i < lines.size(); i++)
            f.Draw(lines[i], 128, 300 + 20 * static_cast<int>(i));
        if (Done() && !after) kit::DrawControls(v, hidden);
    }
    void Mousepressed(int x, int y, int) override {
        if (!Done()) {
            Advance();
            return;
        }
        if (after) {
            auto a = after;
            kit::Pop(this);
            a();
            return;
        }
        auto c = kit::ControlAt(v, x, y, hidden);
        if (!c) return;
        if (c->id == kTake && hero) {
            for (auto& it : G.g->map->items) {
                if (it.status == 1 && it.x == hero->x && it.y == hero->y)
                    w2::hero::TakeItem(*G.g, hero, &it);
            }
            Close();
        } else if (c->id == kDone) {
            Close();
        }
    }
    void Keypressed(const std::string& key) override {
        if (!Done()) {
            Advance();
            return;
        }
        if (after) {
            auto a = after;
            kit::Pop(this);
            a();
            return;
        }
        if (key == "return" || key == "kpenter" || key == "escape") Close();
    }
};

void OpenRuin(std::vector<std::string> lines, bool guarded, bool can_take, w2::Army* hero,
              kit::Done after) {
    auto d = std::make_shared<Ruin>();
    d->lines = lines;
    d->guarded = guarded;
    d->can_take = can_take;
    d->hero = hero;
    d->after = after;
    d->v = kit::View(kRuinDialog);
    d->view = &d->v;
    d->v.state[kDone] = w2::uidata::kNormal;
    d->v.state[kTake] = w2::uidata::kNormal;
    if (!can_take) d->hidden.insert(kTake);
    sound::Effect("dramatic");
    kit::Push(d);
}

const kit::Rect kTemple{160, 60, 320, 280};  // popup 9
const int kTempleDialog = 15, kBless = 328, kQuest = 329;

// How many a temple blessed (temple_bless, 6536:08a1).
std::string BlessLine(int n) {
    if (n == 0) return kit::Text(0x37, 0);
    if (n == 1) return kit::Text(0x38, 0);
    return Format(kit::Text(0x38, 1), n);
}

struct Temple : kit::Modal {
    std::shared_ptr<w2::SearchResult> r;
    std::vector<w2::Army*> stack;
    screen::View v;
    void Draw() override {
        kit::PopupFrame(kTemple);
        gfx::SetColor(1, 1, 1);
        gfx::Draw(G.temple_pic, kTemple.x, kTemple.y);
        const Font& f = kit::GetFont(2).Colours(7, 6);
        kit::Centred(f, Format(kit::Text(0x13, 0), r->site->name), 320, 270);
        kit::Centred(f, kit::Text(0x13, 1), 320, 290);
        kit::Centred(f, kit::Text(0x13, 2), 320, 310);
        kit::DrawControls(v);
    }
    void Mousepressed(int x, int y, int) override {
        auto c = kit::ControlAt(v, x, y);
        if (!c) return;
        if (c->id == kBless) {
            // temple_bless (6536:08a1): how many were blessed, and a parting line
            auto res = r;
            auto st = stack;
            kit::Pop(this);
            ShowMessage(BlessLine(w2::site::Bless(*G.g, *res->site, st)), kit::Text(0x38, 2));
        } else if (c->id == kQuest) {
            auto res = r;
            auto st = stack;
            kit::Pop(this);
            // the searching stack's mode decides how far the target is uncovered
            w2::quest::Assign(*G.g, *G.player, res->hero, false,
                              w2::move::ModeOf(*G.g, st) == w2::move::kFlying);
            front::OpenQuest();
        }
    }
};

void OpenTemple(std::shared_ptr<w2::SearchResult> r, const std::vector<w2::Army*>& stack) {
    sound::Music(w2::cues::kTemple);  // 4976:0000
    auto d = std::make_shared<Temple>();
    d->r = r;
    d->stack = stack;
    d->v = kit::View(kTempleDialog);
    d->view = &d->v;
    d->v.state[kBless] = w2::uidata::kNormal;
    bool can_quest = G.g->map->options.quests != 0 && !G.player->quest && r->hero;
    d->v.state[kQuest] = can_quest ? w2::uidata::kNormal : w2::uidata::kDisabled;
    kit::Push(d);
}

const kit::Rect kSage{80, 60, 480, 312};  // popup 2
const int kSageX = 80, kSageY = 60;
const int kSageDialog = 10, kItems = 279, kGem = 280, kMap = 281, kSageDone = 282;

struct Sage : kit::Modal {
    std::shared_ptr<w2::SearchResult> r;
    w2::Army* h = nullptr;
    screen::View v;
    std::vector<std::string> said;
    std::vector<w2::site::SageEntry> list;
    bool hidden_map = false, pointing = false;
    w2::Site* target = nullptr;
    std::optional<std::array<int, 4>> patch;

    void Offers(bool on) {
        auto& st = v.state;
        if (on) {
            st[kItems] = !list.empty() ? w2::uidata::kNormal : w2::uidata::kDisabled;
            st[kGem] = w2::uidata::kNormal;
            st[kMap] = hidden_map ? w2::uidata::kNormal : w2::uidata::kDisabled;
        } else {
            st[kItems] = st[kGem] = st[kMap] = w2::uidata::kDisabled;
        }
        hidden = {kSageDone};
    }
    void Finished() {
        v.state[kSageDone] = w2::uidata::kNormal;
        hidden = {kItems, kGem, kMap};
        pointing = false;
    }
    void Draw() override {
        kit::Popup(kSage);
        front::DrawStrategicMap(kSageX, kSageY);
        if (!target) front::DrawSiteMarkers(kSageX, kSageY, r->site);
        if (target) {
            kit::MapTarget(kSageX, kSageY, h->x, h->y, target->x, target->y);
            front::DrawHeroFigure(kSageX, kSageY, h->x, h->y);
        }
        if (patch) {
            kit::SetPal(15);
            kit::Outline(kSageX + 2 * (*patch)[0], kSageY + 2 * (*patch)[1], 2 * (*patch)[2],
                         2 * (*patch)[3]);
        }
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), "A Sage!", 432, 62);  // 4125:0c7c
        const Font& f = kit::GetFont(2);
        for (int i = 0; i <= 4; i++) kit::Centred(f, kit::Text(0x3f, i), 432, 120 + 20 * i);
        for (size_t i = 0; i < said.size(); i++)
            kit::Centred(f, said[i], 432, 240 + 20 * static_cast<int>(i));
        kit::DrawControls(v, hidden);
    }
    void Items() {
        Offers(false);
        std::vector<std::string> names;
        for (auto& e : list) names.push_back(e.name);
        std::weak_ptr<Modal> self = weak_from_this();
        Sage* me = this;
        choose::Open("Items", names, 0, [me, self](int i) {
            if (self.expired()) return;
            if (i == w2::kNone) return me->Offers(true);
            const auto& e = me->list[i];
            if (e.kind == "gold")
                me->said.push_back(kit::Text(0x3e, 2));
            else if (e.kind == "allies")
                me->said.push_back(kit::Text(0x3e, 3));
            else
                me->said.push_back(Format(kit::Text(0x3e, 0), e.name));
            me->said.push_back(kit::Text(0x3e, 1));
            if (w2::Site* s = w2::site::SageShow(*G.g, *G.player, e, me->h->x, me->h->y)) {
                me->said.push_back(Format(kit::Text(0x3e, 7), s->name));
                me->target = s;
            }
            front::StratDirty();
            me->Finished();
        });
    }
    void Mousepressed(int x, int y, int) override {
        if (pointing) {
            // the map is region 15: a click on it picks the tile (6536:0cd6)
            int tx = (x - kSageX) / 2, ty = (y - kSageY) / 2;
            if (x >= kSageX && y >= kSageY && x < kSageX + 224 && y < kSageY + 312) {
                patch = w2::site::SageMap(*G.g, *G.player, tx, ty);
                front::StratDirty();
                Finished();
            }
            return;
        }
        auto c = kit::ControlAt(v, x, y, hidden);
        if (!c) return;
        if (c->id == kItems) {
            Items();
        } else if (c->id == kGem) {
            int n = w2::site::SageGem(*G.g, *G.player);
            said.push_back(kit::Text(0x3b, 0));
            said.push_back(Format(kit::Text(0x3b, 1), n));
            Finished();
        } else if (c->id == kMap) {
            Offers(false);
            for (int i = 0; i <= 2; i++) said.push_back(kit::Text(0x3c, i));
            pointing = true;
        } else if (c->id == kSageDone) {
            kit::Pop(this);
            front::StratDirty();
        }
    }
    void Keypressed(const std::string& key) override {
        if (hidden.count(kSageDone)) return;
        if (key == "escape" || key == "return" || key == "kpenter") {
            kit::Pop(this);
            front::StratDirty();
        }
    }
};

void OpenSage(std::shared_ptr<w2::SearchResult> r) {
    auto d = std::make_shared<Sage>();
    d->r = r;
    d->h = r->hero;
    d->v = kit::View(kSageDialog);
    d->view = &d->v;
    d->list = w2::site::SageList(*G.g, *G.player, d->h->x, d->h->y);
    d->hidden_map = G.g->map->options.hidden_map != 0;
    d->Offers(true);
    kit::Push(d);
}
}  // namespace

void ShowMessage(const std::string& line1, const std::string& line2, kit::Done after) {
    auto d = std::make_shared<Message>();
    d->line1 = line1;
    d->line2 = line2;
    d->after = after;
    kit::Push(d);
}

void Say(const std::string& line, kit::Done after) {
    auto d = std::make_shared<Message>();
    d->line1 = line;
    d->one = true;
    d->after = after;
    kit::Push(d);
}

void Open(const std::vector<w2::Army*>& stack) {
    if (stack.empty()) return;
    auto r = w2::game::SearchHere(*G.g, stack, true);
    if (!r || r->kind == "no hero") return;
    // with no hero a temple blesses at once, with only its message (6536:0000)
    if (r->kind == "temple" && !r->hero)
        return ShowMessage(BlessLine(r->blessed), kit::Text(0x38, 2));
    if (r->kind == "temple") return OpenTemple(r, stack);
    if (r->kind == "sage") {
        sound::Music(w2::cues::kSage);
        OpenRuin({Format(kit::Text(0x3a, 0), r->hero ? r->hero->name : "")}, false, false, nullptr,
                 [r]() { OpenSage(r); });
        return;
    }
    front::StratDirty();
    bool guarded = r->kind != "allies" && (r->monster || r->guardian);
    bool can_take = r->kind == "item" && r->item && r->item->status == 1;
    OpenRuin(RuinLines(*r), guarded, can_take, r->hero, nullptr);
}

}  // namespace search
