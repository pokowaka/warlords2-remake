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
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/cues.hpp"

namespace honours {

using w2::Format;

namespace {
constexpr kit::Rect kPopupRect{160, 90, 320, 200};  ///< Where both popups stand.
constexpr int kPromotionGroup = 101,  ///< STRING.DAT group of level 2; 3 and 4 follow.
        kMedalFor = 158,              ///< Group: why the army earned it.
        kMedalUnit = 159,             ///< Group: "your unit of %s".
        kMedalGiven = 160;            ///< Group: that the medal was given.
constexpr int kMedalName = 161,       ///< Group: the medals' names, first to fourth.
        kMedalGrew = 162;             ///< Group: what grew, and from what to what.

/**
 * @brief A picture popup with lines of text over it, gone at any key or
 * click.
 */
class Popup final : public kit::Modal {
  public:
    using DrawLinesFn = std::function<void(w2::UiContext&)>;

    Popup(w2::UiContext& ctx, gfx::ImageP pic, DrawLinesFn lines, kit::Done after)
            : kit::Modal(ctx)
            , pic_(std::move(pic))
            , lines_(std::move(lines))
            , after_(std::move(after)) {}

    void Draw() override {
        w2::UiContext& ctx = Ctx();
        kit::PopupFrame(ctx, kPopupRect);
        ctx.Renderer().SetColor(1, 1, 1);
        if (pic_) ctx.Renderer().Draw(pic_, kPopupRect.x, kPopupRect.y);
        lines_(ctx);
    }

    void Mousepressed(int, int, int) override { Close(); }
    void Keypressed(const std::string&) override { Close(); }

  private:
    /** @brief Takes the popup down, then runs `after_`. */
    void Close() {
        kit::Done after_copy = std::move(after_);  // the popup (and its `after_`) dies in Pop
        PopSelf();
        if (after_copy) after_copy();
    }

    gfx::ImageP pic_;    ///< The picture, or null when it didn't load.
    DrawLinesFn lines_;  ///< Draws the text over the picture.
    kit::Done after_;    ///< Runs once the popup has closed.
};

/** @brief Puts up a picture popup with `lines` over it; `after` runs when it closes. */
void Show(w2::UiContext& ctx, gfx::ImageP pic, Popup::DrawLinesFn lines, kit::Done after) {
    kit::Push(ctx,
              std::make_shared<Popup>(ctx, std::move(pic), std::move(lines), std::move(after)));
}

/** @brief Snapshot of promotion text fields rendered over `LEVEL2..4.PCK`. */
struct PromotionCard {
    int group = 0;     ///< `STRING.DAT` group (`101..103`).
    int base = 0;      ///< Line offset (`0` male, `6` female).
    std::string name;  ///< Hero display name.
    int level = 2;     ///< Promoted level (`2..4`).
    int strength = 0;  ///< Post-promotion strength.
    int gain = 0;      ///< Strength gained on promotion.
    int moves = 0;     ///< Post-promotion max movement points.
};

/** @brief Draws `card`'s promotion lines over the backdrop. */
void DrawPromotionCard(w2::UiContext& ctx, const PromotionCard& card) {
    kit::Centred(kit::GetFont(ctx, 1), kit::Text(ctx, card.group, card.base), 320, 93);
    const Font& font = kit::GetFont(ctx, 2);
    kit::Centred(font, card.name, 320, 140);
    kit::Centred(font, kit::Text(ctx, card.group, card.base + 1), 320, 160);
    kit::Centred(font, kit::Text(ctx, card.group, card.base + 2), 320, 180);
    kit::Centred(font, kit::Text(ctx, card.group, card.base + 3), 320, 200);
    kit::Centred(font, kit::Text(ctx, card.group, card.base + 4), 320, 232);
    kit::Centred(font,
                 Format(kit::Text(ctx, card.group, card.base + 5), card.level, card.strength,
                        card.gain, card.moves, 2),
                 320, 252);
}

/** @brief Snapshot of medal award text fields rendered over `MEDAL0..3.PCK`. */
struct MedalCard {
    int type = 0;            ///< Army type index.
    int owner = 0;           ///< Owning faction index.
    int medal_number = 1;    ///< Medal tier (`1..4`).
    bool grew_move = false;  ///< True when moves grew instead of strength.
    int now = 0;             ///< Post-medal stat value.
    std::string line1;       ///< Chosen citation line from group 158.
    std::string line3;       ///< Chosen award line from group 160.
    std::string type_name;   ///< Army type display name.
};

/** @brief Draws `card`'s army icon and medal text over the backdrop. */
void DrawMedalCard(w2::UiContext& ctx, const MedalCard& card) {
    kit::Army(ctx, card.type, card.owner, 176, 155, 0);
    ctx.Renderer().SetColor(1, 1, 1);
    const Font& font = kit::GetFont(ctx, 2);
    font.Draw(card.line1, 216, 155);
    font.Draw(Format(kit::Text(ctx, kMedalUnit, 0), card.type_name), 216, 175);
    font.Draw(card.line3, 176, 195);
    font.Draw(kit::Text(ctx, kMedalName, card.medal_number - 1), 176, 215);
    font.Draw(kit::Text(ctx, kMedalGrew, card.grew_move ? 1 : 0), 176, 245);
    font.Draw(Format(kit::Text(ctx, kMedalGrew, 2), card.now - 1, card.now), 176, 265);
}

/**
 * @brief Shows promotion `index` of `list`, then the next one, and `after`
 * once all are told.
 */
void Promotion(w2::UiContext& ctx, PromotionList list, size_t index, kit::Done after) {
    if (index >= list.size()) {
        if (after) after();
        return;
    }
    w2::Army* hero = list[index].hero;
    int level = hero->Level() ? hero->Level() : 2;  // the new level, 2-4
    PromotionCard card{
        .group = kPromotionGroup + level - 2,
        .base = hero->IsFemale() ? 6 : 0,
        .name = hero->Name(),
        .level = level,
        .strength = hero->Strength(),
        .gain = list[index].gain,
        .moves = hero->MaxMoves(),
    };
    ctx.Audio().Music(w2::cues::kPromotion);
    gfx::ImageP pic = ctx.Assets().LevelPic(level);
    Show(
            ctx, std::move(pic),
            [card = std::move(card)](w2::UiContext& draw_ctx) {
                DrawPromotionCard(draw_ctx, card);
            },
            [&ctx, remaining = std::move(list), index, after]() {
                Promotion(ctx, remaining, index + 1, after);
            });
}
}  // namespace

/**
 * @brief Shows each hero promoted in its own popup, one after the other,
 * then runs `after`.
 */
void Promotions(w2::UiContext& ctx, const PromotionList& list, kit::Done after) {
    Promotion(ctx, list, 0, std::move(after));
}

/**
 * @brief Shows the medal `army` has just won, with the medal music, then
 * goes back to the play music and runs `after`.
 */
void Medal(w2::UiContext& ctx, w2::Army* army, kit::Done after) {
    int medal_number = army->Medals() ? army->Medals() : 1;  // 1-4: which medal this is
    bool grew_move = army->Strength() >= 15;
    int now = grew_move ? army->MaxMoves() : army->Strength();
    auto pick = [&ctx](int group, int count) {
        return kit::Text(ctx, group, ctx.ActiveGame().GetRng().Dice(1, count, -1));
    };
    std::string line1 = pick(kMedalFor, 7);
    std::string line3 = pick(kMedalGiven, 4);
    const w2::ArmyType* army_type = ctx.ActiveGame().ArmyTypes().ById(army->Type());
    MedalCard card{
        .type = army->TypeIndex(),
        .owner = army->OwnerIndex(),
        .medal_number = medal_number,
        .grew_move = grew_move,
        .now = now,
        .line1 = std::move(line1),
        .line3 = std::move(line3),
        .type_name = army_type ? army_type->name : "",
    };
    ctx.Audio().Music(w2::cues::kMedal);
    Show(
            ctx, ctx.Assets().MedalPic(medal_number - 1),
            [card = std::move(card)](w2::UiContext& draw_ctx) { DrawMedalCard(draw_ctx, card); },
            [&ctx, after]() {
                ctx.Audio().Music(w2::cues::kPlay);
                if (after) after();
            });
}

}  // namespace honours
