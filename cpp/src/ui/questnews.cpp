/**
 * @file
 * @brief A quest's end, as quest_check tells a human it.
 *
 * Failed: the message box with the two lines of its cause, STRING.DAT groups
 * 32-42. Done: the triumph's music, and the reward: popup 2 with dialog 17 --
 * Done (331); the strategic map with no city shields and the hero's figure
 * on it; "Quest" in font 1; SCROLL.PCK at (304, 95) and on it, black on
 * yellow, the hero's quest, the priests' words and what was given.
 */
#include "platform/sound.hpp"
#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/cues.hpp"

namespace questnews {

using w2::Format;

namespace {
const kit::Rect kPopupRect{80, 60, 480, 312};  ///< Popup 2.
const int kMapX = 80,                          ///< The strategic map's left edge.
        kMapY = 60;                            ///< The strategic map's top edge.
const int kDialog = 17,                        ///< The dialog with the button.
        kDone = 331;                           ///< The Done button.

/** @brief The reward popup for a quest done. */
struct Reward : kit::Modal {
    std::shared_ptr<w2::QuestResult> news;  ///< The quest's end being told.
    screen::View dialog_view;               ///< Dialog 17's button.
    kit::Done after;                        ///< Runs once the popup has closed.
    int reward_group = 0x1f;                ///< STRING.DAT group that words the reward.
    std::string reward_line;                ///< The last line: what was given.
    /** @brief Takes the popup down, then runs `after`. */
    void Close() {
        auto after_copy = after;  // the popup (and its `after`) dies in Pop
        kit::Pop(this);
        if (after_copy) after_copy();
    }
    void Draw() override {
        const w2::Quest& quest = *news->quest;
        const w2::Reward& reward = *news->reward;
        w2::Army* hero = quest.hero;
        kit::Popup(kPopupRect);
        front::DrawStrategicMap(kMapX, kMapY, nullptr, true);
        if (reward.kind == "revealed" && reward.site && hero && hero->x != w2::kNone)
            kit::MapTarget(kMapX, kMapY, hero->x, hero->y, reward.site->x, reward.site->y);
        if (hero && hero->x != w2::kNone) front::DrawHeroFigure(kMapX, kMapY, hero->x, hero->y);
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), "Quest", 432, 64);
        gfx::Draw(G.scroll_pic, 304, 95);
        const Font& font = kit::GetFont(2).Colours(0, 7);
        kit::Centred(font, (hero ? hero->name : std::string()) + "'s Quest", 440, 145);
        kit::SetPal(0);
        gfx::Rectangle(gfx::kFill, 360, 165, 160, 1);
        kit::Centred(font, kit::Text(0x1c, 0), 440, 175);
        kit::Centred(font, kit::Text(0x1c, 1), 440, 195);
        kit::Centred(font, kit::Text(reward_group, 0), 440, 225);
        kit::Centred(font, kit::Text(reward_group, 1), 440, 245);
        kit::Centred(font, reward_line, 440, 265);
        kit::DrawControls(dialog_view);
    }
    void Mousepressed(int x, int y, int) override {
        auto control = kit::ControlAt(dialog_view, x, y);
        if (control && control->id == kDone) Close();
    }
    void Keypressed(const std::string& key) override {
        if (key == "escape" || key == "return" || key == "kpenter") Close();
    }
};
}  // namespace

/**
 * @brief Tells a quest's end: a failure's cause in the message box, or a
 * success's reward in its own popup to the triumph's music; then `after`.
 */
void Show(std::shared_ptr<w2::QuestResult> news, kit::Done after) {
    if (!news) return;
    if (!news->failed.empty() || !news->reward) {
        int cause_group = news->why ? news->why : 0x20;
        search::ShowMessage(kit::Text(cause_group, 0), kit::Text(cause_group, 1), after);
        return;
    }
    sound::Music(w2::cues::kTriumph);
    auto dialog = std::make_shared<Reward>();
    dialog->news = news;
    dialog->after = after;
    dialog->dialog_view = kit::View(kDialog);
    dialog->view = &dialog->dialog_view;
    dialog->dialog_view.state[kDone] = w2::uidata::kNormal;
    const w2::Reward& reward = *news->reward;
    if (reward.kind == "revealed") {
        dialog->reward_group = 0x1d;
        dialog->reward_line = reward.site ? reward.site->name : "";
    } else if (reward.kind == "item") {
        dialog->reward_group = 0x1e;
        dialog->reward_line = reward.item ? reward.item->name : "";
    } else if (reward.kind == "allies") {
        dialog->reward_group = 0x1f;
        dialog->reward_line = Format(kit::Text(0x1f, 2), static_cast<int>(reward.armies.size()),
                                     reward.type ? reward.type->name : "");
    } else {
        dialog->reward_group = 0x1f;
        dialog->reward_line = Format(kit::Text(0x1f, 2), reward.gold, "gold");
    }
    kit::Push(dialog);
}

/**
 * @brief Tells the human player of a quest that ended, once, when the screen
 * is theirs again.
 */
void Poll() {
    w2::Side* side = G.player;
    if (!side || side->computer || !side->quest_news) return;
    auto news = side->quest_news;
    side->quest_news.reset();
    Show(news);
}

}  // namespace questnews
