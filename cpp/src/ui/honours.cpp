// A hero's promotion and an army's medal, as the original shows them to a
// human: a picture popup at (160, 90) 320x200 with lines of text over it,
// closed by any key or click.
//
// A promotion (hero_promote, 7563:0672) is popup 16, 17 or 18 -- LEVEL2-4.PCK
// -- with music cue 7: STRING.DAT group 101-103 (lines 6 on for a heroine),
// the title in font 1 at y 93, the hero's name at 140 and the rest in font 2
// at 160, 180, 200, 232 and 252, all centred on 320.
//
// A medal (auto_ui_medal_effect, 67cc:2274) is popup 24-27 -- MEDAL0-3.PCK
// for the army's first to fourth -- with music cue 8 and then 1 again: the
// army at (176, 155), and in font 2 a line of group 158 at (216, 155), "your
// unit of %s" (159) at (216, 175), a line of 160 at (176, 195), the medal's
// name (161) at (176, 215), and what grew (162) at (176, 245) and (176, 265).
#include "platform/sound.hpp"
#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/cues.hpp"

namespace honours {

using w2::Format;

namespace {
const kit::Rect kR{160, 90, 320, 200};
const int kPromotionGroup = 101, kMedalFor = 158, kMedalUnit = 159, kMedalGiven = 160;
const int kMedalName = 161, kMedalGrew = 162;

struct Popup : kit::Modal {
    gfx::ImageP pic;
    std::function<void()> lines;
    kit::Done after;
    void Close() {
        auto a = after;
        kit::Pop(this);
        if (a) a();
    }
    void Draw() override {
        kit::PopupFrame(kR);
        gfx::SetColor(1, 1, 1);
        if (pic) gfx::Draw(pic, kR.x, kR.y);
        lines();
    }
    void Mousepressed(int, int, int) override { Close(); }
    void Keypressed(const std::string&) override { Close(); }
};

void Show(gfx::ImageP pic, std::function<void()> lines, kit::Done after) {
    auto d = std::make_shared<Popup>();
    d->pic = pic;
    d->lines = std::move(lines);
    d->after = std::move(after);
    kit::Push(d);
}

void Promotion(std::vector<w2::Promotion> list, size_t i, kit::Done after) {
    if (i >= list.size()) {
        if (after) after();
        return;
    }
    w2::Army* hero = list[i].hero;
    int gain = list[i].gain;
    int level = hero->level ? hero->level : 2;  // the new level, 2-4
    int group = kPromotionGroup + level - 2;
    int base = hero->female ? 6 : 0;
    std::string name = hero->name;
    int strength = hero->strength, moves = hero->max_moves;
    sound::Music(w2::cues::kPromotion);
    auto pic = G.level_pic.count(level) ? G.level_pic[level] : nullptr;
    Show(
            pic,
            [=]() {
                kit::Centred(kit::GetFont(1), kit::Text(group, base), 320, 93);
                const Font& f = kit::GetFont(2);
                kit::Centred(f, name, 320, 140);
                kit::Centred(f, kit::Text(group, base + 1), 320, 160);
                kit::Centred(f, kit::Text(group, base + 2), 320, 180);
                kit::Centred(f, kit::Text(group, base + 3), 320, 200);
                kit::Centred(f, kit::Text(group, base + 4), 320, 232);
                kit::Centred(f, Format(kit::Text(group, base + 5), level, strength, gain, moves, 2),
                             320, 252);
            },
            [list, i, after]() { Promotion(list, i + 1, after); });
}
}  // namespace

void Promotions(const std::vector<w2::Promotion>& list, kit::Done after) {
    Promotion(list, 0, after);
}

void Medal(w2::Army* a, kit::Done after) {
    int n = a->medals ? a->medals : 1;
    bool grew_move = a->strength >= 15;
    int now = grew_move ? a->max_moves : a->strength;
    auto pick = [](int group, int count) { return kit::Text(group, G.g->rng.Dice(1, count, -1)); };
    std::string line1 = pick(kMedalFor, 7), line3 = pick(kMedalGiven, 4);
    const w2::ArmyType* t = G.g->types.ById(a->type);
    std::string tname = t ? t->name : "";
    int type = a->type, owner = a->owner;
    sound::Music(w2::cues::kMedal);
    Show(
            G.medal_pic[n - 1],
            [=]() {
                kit::Army(type, owner, 176, 155, 0);
                gfx::SetColor(1, 1, 1);
                const Font& f = kit::GetFont(2);
                f.Draw(line1, 216, 155);
                f.Draw(Format(kit::Text(kMedalUnit, 0), tname), 216, 175);
                f.Draw(line3, 176, 195);
                f.Draw(kit::Text(kMedalName, n - 1), 176, 215);
                f.Draw(kit::Text(kMedalGrew, grew_move ? 1 : 0), 176, 245);
                f.Draw(Format(kit::Text(kMedalGrew, 2), now - 1, now), 176, 265);
            },
            [after]() {
                sound::Music(w2::cues::kPlay);
                if (after) after();
            });
}

}  // namespace honours
