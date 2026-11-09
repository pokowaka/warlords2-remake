/**
 * @file
 * @brief A hero's promotion and an army's medal, as the original shows them
 * to a human.
 *
 * Both are a picture popup at (160, 90) 320x200 with lines of text over it,
 * closed by any key or click.
 *
 * A promotion (hero_promote) is popup 16, 17 or 18 -- LEVEL2-4.PCK -- with
 * music cue 7: STRING.DAT group 101-103 (lines 6 on for a heroine), the title
 * in font 1 at y 93, the hero's name at 140 and the rest in font 2 at 160,
 * 180, 200, 232 and 252, all centred on 320.
 *
 * A medal (auto_ui_medal_effect) is popup 24-27 -- MEDAL0-3.PCK for the
 * army's first to fourth -- with music cue 8 and then 1 again: the army at
 * (176, 155), and in font 2 a line of group 158 at (216, 155), "your unit of
 * %s" (159) at (216, 175), a line of 160 at (176, 195), the medal's name (161)
 * at (176, 215), and what grew (162) at (176, 245) and (176, 265).
 */
#include "platform/sound.hpp"
#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/cues.hpp"

namespace honours {

using w2::Format;

namespace {
const kit::Rect kPopupRect{160, 90, 320, 200};  ///< Where both popups stand.
const int kPromotionGroup = 101,                ///< STRING.DAT group of level 2; 3 and 4 follow.
        kMedalFor = 158,                        ///< Group: why the army earned it.
        kMedalUnit = 159,                       ///< Group: "your unit of %s".
        kMedalGiven = 160;                      ///< Group: that the medal was given.
const int kMedalName = 161,                     ///< Group: the medals' names, first to fourth.
        kMedalGrew = 162;                       ///< Group: what grew, and from what to what.

/**
 * @brief A picture popup with lines of text over it, gone at any key or
 * click.
 */
struct Popup : kit::Modal {
    gfx::ImageP pic;              ///< The picture, or null when it didn't load.
    std::function<void()> lines;  ///< Draws the text over the picture.
    kit::Done after;              ///< Runs once the popup has closed.
    /** @brief Takes the popup down, then runs `after`. */
    void Close() {
        auto after_copy = after;  // the popup (and its `after`) dies in Pop
        kit::Pop(this);
        if (after_copy) after_copy();
    }
    void Draw() override {
        kit::PopupFrame(kPopupRect);
        gfx::SetColor(1, 1, 1);
        if (pic) gfx::Draw(pic, kPopupRect.x, kPopupRect.y);
        lines();
    }
    void Mousepressed(int, int, int) override { Close(); }
    void Keypressed(const std::string&) override { Close(); }
};

/** @brief Puts up a picture popup with `lines` over it; `after` runs when it closes. */
void Show(gfx::ImageP pic, std::function<void()> lines, kit::Done after) {
    auto popup = std::make_shared<Popup>();
    popup->pic = pic;
    popup->lines = std::move(lines);
    popup->after = std::move(after);
    kit::Push(popup);
}

/**
 * @brief Shows promotion `index` of `list`, then the next one, and `after`
 * once all are told.
 */
void Promotion(std::vector<w2::Promotion> list, size_t index, kit::Done after) {
    if (index >= list.size()) {
        if (after) after();
        return;
    }
    w2::Army* hero = list[index].hero;
    int gain = list[index].gain;
    int level = hero->Level() ? hero->Level() : 2;  // the new level, 2-4
    int group = kPromotionGroup + level - 2;
    int base = hero->IsFemale() ? 6 : 0;
    std::string name = hero->Name();
    int strength = hero->Strength(), moves = hero->MaxMoves();
    sound::Music(w2::cues::kPromotion);
    auto pic = g_app.level_pic.count(level) ? g_app.level_pic[level] : nullptr;
    Show(
            pic,
            [=]() {
                kit::Centred(kit::GetFont(1), kit::Text(group, base), 320, 93);
                const Font& font = kit::GetFont(2);
                kit::Centred(font, name, 320, 140);
                kit::Centred(font, kit::Text(group, base + 1), 320, 160);
                kit::Centred(font, kit::Text(group, base + 2), 320, 180);
                kit::Centred(font, kit::Text(group, base + 3), 320, 200);
                kit::Centred(font, kit::Text(group, base + 4), 320, 232);
                kit::Centred(font,
                             Format(kit::Text(group, base + 5), level, strength, gain, moves, 2),
                             320, 252);
            },
            [list, index, after]() { Promotion(list, index + 1, after); });
}
}  // namespace

/**
 * @brief Shows each hero promoted in its own popup, one after the other,
 * then runs `after`.
 */
void Promotions(const std::vector<w2::Promotion>& list, kit::Done after) {
    Promotion(list, 0, after);
}

/**
 * @brief Shows the medal `army` has just won, with the medal music, then
 * goes back to the play music and runs `after`.
 */
void Medal(w2::Army* army, kit::Done after) {
    int medal_number = army->Medals() ? army->Medals() : 1;  // 1-4: which medal this is
    bool grew_move = army->Strength() >= 15;
    int now = grew_move ? army->MaxMoves() : army->Strength();
    auto pick = [](int group, int count) {
        return kit::Text(group, g_app.g->GetRng().Dice(1, count, -1));
    };
    std::string line1 = pick(kMedalFor, 7), line3 = pick(kMedalGiven, 4);
    const w2::ArmyType* army_type = g_app.g->ArmyTypes().ById(army->Type());
    std::string type_name = army_type ? army_type->name : "";
    int type = army->TypeIndex(), owner = army->OwnerIndex();
    sound::Music(w2::cues::kMedal);
    Show(
            g_app.medal_pic[medal_number - 1],
            [=]() {
                kit::Army(type, owner, 176, 155, 0);
                gfx::SetColor(1, 1, 1);
                const Font& font = kit::GetFont(2);
                font.Draw(line1, 216, 155);
                font.Draw(Format(kit::Text(kMedalUnit, 0), type_name), 216, 175);
                font.Draw(line3, 176, 195);
                font.Draw(kit::Text(kMedalName, medal_number - 1), 176, 215);
                font.Draw(kit::Text(kMedalGrew, grew_move ? 1 : 0), 176, 245);
                font.Draw(Format(kit::Text(kMedalGrew, 2), now - 1, now), 176, 265);
            },
            [after]() {
                sound::Music(w2::cues::kPlay);
                if (after) after();
            });
}

}  // namespace honours
