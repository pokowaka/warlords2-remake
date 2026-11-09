/**
 * @file
 * @brief A quest's end, as `quest_check` tells a human it.
 *
 * Failed: the message box with the two lines of its cause, `STRING.DAT` groups
 * 32-42. Done: the triumph's music, and the reward: popup 2 with dialog 17 --
 * Done (`331`); the strategic map with no city shields and the hero's figure
 * on it; `"Quest"` in font 1; `SCROLL.PCK` at `(304, 95)` and on it, black on
 * yellow, the hero's quest, the priests' words and what was given.
 */
#include <memory>
#include <string>
#include <utility>

#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/cues.hpp"

namespace questnews {

using w2::Format;

namespace {

constexpr kit::Rect kPopupRect{80, 60, 480, 312};  ///< Popup 2.
constexpr int kMapX = 80;                          ///< The strategic map's left edge.
constexpr int kMapY = 60;                          ///< The strategic map's top edge.
constexpr int kDialog = 17;                        ///< The dialog with the button.
constexpr int kDone = 331;                         ///< The Done button.

/**
 * @brief Modal displaying the completed quest reward scroll and strategic map target.
 */
class Reward final : public kit::Modal {
  public:
    Reward(w2::UiContext& ctx, SharedQuestResult news, kit::Done after, int reward_group,
           std::string reward_line)
            : kit::Modal(ctx)
            , news_(std::move(news))
            , view_(kit::View(ctx, kDialog))
            , after_(std::move(after))
            , reward_group_(reward_group)
            , reward_line_(std::move(reward_line)) {
        view_.state[kDone] = w2::uidata::kNormal;
        SetDialogView(&view_);
    }

    void Draw() override {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        const w2::Quest& quest = *news_->quest;
        const w2::Reward& reward = *news_->reward;
        const w2::Army* hero = ctx.GamePtr() ? quest.ResolveHero(*ctx.GamePtr()) : nullptr;
        kit::Popup(ctx, kPopupRect);
        ctx.Actions().DrawStrategicMap(kMapX, kMapY, nullptr, true);
        if (reward.kind == "revealed" && reward.site && hero && hero->X() != w2::kNone) {
            kit::MapTarget(ctx, kMapX, kMapY, hero->X(), hero->Y(), reward.site->x, reward.site->y);
        }
        if (hero && hero->X() != w2::kNone) {
            ctx.Actions().DrawHeroFigure(kMapX, kMapY, hero->X(), hero->Y());
        }
        ren.SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(ctx, 1), "Quest", 432, 64);
        ren.Draw(ctx.Assets().ScrollPic(), 304, 95);
        const Font& font = kit::GetFont(ctx, 2).Colours(0, 7);
        kit::Centred(font, (hero ? hero->Name() : std::string()) + "'s Quest", 440, 145);
        kit::SetPal(ctx, 0);
        ren.Rectangle(gfx::kFill, 360, 165, 160, 1);
        kit::Centred(font, kit::Text(ctx, 0x1c, 0), 440, 175);
        kit::Centred(font, kit::Text(ctx, 0x1c, 1), 440, 195);
        kit::Centred(font, kit::Text(ctx, reward_group_, 0), 440, 225);
        kit::Centred(font, kit::Text(ctx, reward_group_, 1), 440, 245);
        kit::Centred(font, reward_line_, 440, 265);
        kit::DrawControls(ctx, view_);
    }

    void Mousepressed(int x, int y, int) override {
        const auto* control = kit::ControlAt(view_, x, y);
        if (control && control->id == kDone) Close();
    }

    void Keypressed(const std::string& key) override {
        if (key == "escape" || key == "return" || key == "kpenter") Close();
    }

  private:
    void Close() {
        kit::Done after_copy = std::move(after_);
        PopSelf();
        if (after_copy) after_copy();
    }

    SharedQuestResult news_;   ///< The quest's end being told.
    screen::View view_;        ///< Dialog 17's button.
    kit::Done after_;          ///< Runs once the popup has closed.
    int reward_group_ = 0x1f;  ///< STRING.DAT group that words the reward.
    std::string reward_line_;  ///< The last line: what was given.
};

}  // namespace

void Show(w2::UiContext& ctx, SharedQuestResult news, kit::Done after) {
    if (!news) return;
    if (!news->failed.empty() || !news->reward) {
        int cause_group = news->why ? news->why : 0x20;
        search::ShowMessage(ctx, kit::Text(ctx, cause_group, 0), kit::Text(ctx, cause_group, 1),
                            std::move(after));
        return;
    }
    if (ctx.GamePtr()) {
        ctx.Audio().Music(w2::cues::kTriumph, ctx.GamePtr()->Sides());
    } else {
        ctx.Audio().Music(w2::cues::kTriumph);
    }
    int reward_group = 0x1f;
    std::string reward_line;
    const w2::Reward& reward = *news->reward;
    if (reward.kind == "revealed") {
        reward_group = 0x1d;
        reward_line = reward.site ? reward.site->name : "";
    } else if (reward.kind == "item") {
        reward_group = 0x1e;
        reward_line = reward.item ? reward.item->name : "";
    } else if (reward.kind == "allies") {
        reward_group = 0x1f;
        reward_line = Format(kit::Text(ctx, 0x1f, 2), static_cast<int>(reward.armies.size()),
                             reward.type ? reward.type->name : "");
    } else {
        reward_group = 0x1f;
        reward_line = Format(kit::Text(ctx, 0x1f, 2), reward.gold, "gold");
    }
    kit::Push(ctx, std::make_shared<Reward>(ctx, std::move(news), std::move(after), reward_group,
                                            std::move(reward_line)));
}

void Poll(w2::UiContext& ctx) {
    w2::Side* side = ctx.Player();
    if (!ctx.GamePtr() || !side || side->IsComputerControlled()) return;
    auto& quest_news = ctx.ActiveGame().TurnNews(side->Id()).quest_news;
    if (!quest_news) return;
    SharedQuestResult news = quest_news;
    quest_news.reset();
    Show(ctx, std::move(news));
}

}  // namespace questnews
