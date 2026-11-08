/**
 * @file
 * @brief What a pillage or a sack took (auto_ui_pillage_sack_raze), shown
 * over the spoils dialog and closed, the two together, by any key or click.
 *
 * Popup 12, (176, 60) 288x300, marble.
 */
#include "ui/dialogs.hpp"
#include "util/util.hpp"

namespace spoils {

using w2::Format;

namespace {
const kit::Rect kPopupRect{176, 60, 288, 300};  ///< Popup 12, the whole box.
const int kTitle = 0x44;  ///< STRING.DAT group: the title, then the list's headings.
const int kLine = 0x45;   ///< STRING.DAT group: the city pillaged or sacked.
const int kGold = 0x46;   ///< STRING.DAT group: the gold taken.
const int kLost = 0x47;   ///< STRING.DAT group: how many production slots went.
const int kLeft = 0x48;   ///< STRING.DAT group: how many are left.

/** @brief The spoils box: the city, the gold, and the production sold for it. */
struct Spoils : kit::Modal {
    bool sacked = false;                    ///< A sack; else a pillage.
    w2::City* city = nullptr;               ///< The city taken.
    int gold = 0;                           ///< The gold the spoils brought.
    int left = 0;                           ///< How many production slots the city has left.
    std::vector<std::pair<int, int>> lost;  ///< The production lost: army type id, gold got.
    kit::Done after;                        ///< Runs once the box is closed.
    /** @brief Closes the box, then runs `after`. */
    void Close() {
        auto after_copy = after;
        kit::Pop(this);
        if (after_copy) after_copy();
    }
    /** @brief Draws the title, the totals and up to three things lost. */
    void Draw() override {
        int which = sacked ? 1 : 0;
        kit::Popup(kPopupRect);
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), kit::Text(kTitle, which), 320, 64);
        const Font& font = kit::GetFont(2);
        kit::Centred(font, Format(kit::Text(kLine, which), city->name), 320, 115);
        kit::Centred(font, Format(kit::Text(kGold, 0), gold), 320, 135);
        kit::Centred(
                font,
                Format(kit::Text(kLost, lost.size() == 1 ? 0 : 1), static_cast<int>(lost.size())),
                320, 165);
        kit::Centred(font, Format(kit::Text(kLeft, left == 1 ? 0 : 1), left), 320, 185);
        kit::Bevel(184, 220, 272, 130, 4, 2);
        kit::Bevel(185, 221, 270, 128, 2, 4);
        gfx::SetColor(1, 1, 1);
        font.Draw(kit::Text(kTitle, 2), 192, 230);
        font.Draw(kit::Text(kTitle, 3), 368, 230);
        for (int i = 0; i <= 2; i++) {
            int y = 250 + 30 * i;
            bool have = i < static_cast<int>(lost.size());
            kit::Army(have ? lost[i].first : w2::kNone, G.player->index, 208, y, 1);
            if (have) {
                gfx::SetColor(1, 1, 1);
                const w2::ArmyType* type = G.g->types.ById(lost[i].first);
                font.Draw(type ? type->name : "", 248, y + 6);
                font.Draw(w2::Fmt("%d gp", lost[i].second), 368, y + 6);
            }
        }
    }
    void Mousepressed(int, int, int) override { Close(); }
    void Keypressed(const std::string&) override { Close(); }
};
}  // namespace

/** @brief Opens the spoils box; `after` runs when it is closed. */
void Open(bool sacked, w2::City* city, int gold, const std::vector<std::pair<int, int>>& lost,
          int left, kit::Done after) {
    auto dialog = std::make_shared<Spoils>();
    dialog->sacked = sacked;
    dialog->city = city;
    dialog->gold = gold;
    dialog->lost = lost;
    dialog->left = left;
    dialog->after = after;
    kit::Push(dialog);
}

}  // namespace spoils
