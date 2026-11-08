// A quest's end, as quest_check (4976:1ded) tells a human it.
//
// Failed: the message box with the two lines of its cause, STRING.DAT groups
// 32-42. Done: the triumph's music, and the reward (4976:1520): popup 2 with
// dialog 17 -- Done (331); the strategic map with no city shields and the
// hero's figure on it; "Quest" in font 1; SCROLL.PCK at (304, 95) and on it,
// black on yellow, the hero's quest, the priests' words and what was given.
#include "platform/sound.hpp"
#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/cues.hpp"

namespace questnews {

using w2::Format;

namespace {
const kit::Rect kR{80, 60, 480, 312};  // popup 2
const int kMapX = 80, kMapY = 60;
const int kDialog = 17, kDone = 331;

struct Reward : kit::Modal {
    std::shared_ptr<w2::QuestResult> news;
    screen::View v;
    kit::Done after;
    int grp = 0x1f;
    std::string last;
    void Close() {
        auto a = after;
        kit::Pop(this);
        if (a) a();
    }
    void Draw() override {
        const w2::Quest& q = *news->quest;
        const w2::Reward& r = *news->reward;
        w2::Army* h = q.hero;
        kit::Popup(kR);
        front::DrawStrategicMap(kMapX, kMapY, nullptr, true);
        if (r.kind == "revealed" && r.site && h && h->x != w2::kNone)
            kit::MapTarget(kMapX, kMapY, h->x, h->y, r.site->x, r.site->y);
        if (h && h->x != w2::kNone) front::DrawHeroFigure(kMapX, kMapY, h->x, h->y);
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), "Quest", 432, 64);  // 4125:00c8
        gfx::Draw(G.scroll_pic, 304, 95);
        const Font& f = kit::GetFont(2).Colours(0, 7);
        kit::Centred(f, (h ? h->name : std::string()) + "'s Quest", 440, 145);  // 4125:00ce
        kit::SetPal(0);
        gfx::Rectangle(gfx::kFill, 360, 165, 160, 1);
        kit::Centred(f, kit::Text(0x1c, 0), 440, 175);
        kit::Centred(f, kit::Text(0x1c, 1), 440, 195);
        kit::Centred(f, kit::Text(grp, 0), 440, 225);
        kit::Centred(f, kit::Text(grp, 1), 440, 245);
        kit::Centred(f, last, 440, 265);
        kit::DrawControls(v);
    }
    void Mousepressed(int x, int y, int) override {
        auto c = kit::ControlAt(v, x, y);
        if (c && c->id == kDone) Close();
    }
    void Keypressed(const std::string& key) override {
        if (key == "escape" || key == "return" || key == "kpenter") Close();
    }
};
}  // namespace

void Show(std::shared_ptr<w2::QuestResult> news, kit::Done after) {
    if (!news) return;
    if (!news->failed.empty() || !news->reward) {
        int grp = news->why ? news->why : 0x20;
        search::ShowMessage(kit::Text(grp, 0), kit::Text(grp, 1), after);
        return;
    }
    sound::Music(w2::cues::kTriumph);
    auto d = std::make_shared<Reward>();
    d->news = news;
    d->after = after;
    d->v = kit::View(kDialog);
    d->view = &d->v;
    d->v.state[kDone] = w2::uidata::kNormal;
    const w2::Reward& r = *news->reward;
    if (r.kind == "revealed") {
        d->grp = 0x1d;
        d->last = r.site ? r.site->name : "";
    } else if (r.kind == "item") {
        d->grp = 0x1e;
        d->last = r.item ? r.item->name : "";
    } else if (r.kind == "allies") {
        d->grp = 0x1f;
        d->last = Format(kit::Text(0x1f, 2), static_cast<int>(r.armies.size()),
                         r.type ? r.type->name : "");
    } else {
        d->grp = 0x1f;
        d->last = Format(kit::Text(0x1f, 2), r.gold, "gold");  // 4125:00d9
    }
    kit::Push(d);
}

void Poll() {
    w2::Side* side = G.player;
    if (!side || side->computer || !side->quest_news) return;
    auto news = side->quest_news;
    side->quest_news.reset();
    Show(news);
}

}  // namespace questnews
