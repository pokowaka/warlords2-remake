// What a pillage or a sack took (auto_ui_pillage_sack_raze, 63fa:0508),
// shown over the spoils dialog and closed, the two together, by any key or
// click. Popup 12, (176, 60) 288x300, marble.
#include "ui/dialogs.hpp"
#include "util/util.hpp"

namespace spoils {

using w2::format;

namespace {
const kit::Rect kR{176, 60, 288, 300};  // popup 12
const int kTitle = 0x44, kLine = 0x45, kGold = 0x46, kLost = 0x47, kLeft = 0x48;

struct Spoils : kit::Modal {
    bool sacked = false;
    w2::City* city = nullptr;
    int gold = 0, left = 0;
    std::vector<std::pair<int, int>> lost;
    kit::Done after;
    void Close() {
        auto a = after;
        kit::pop(this);
        if (a) a();
    }
    void draw() override {
        int which = sacked ? 1 : 0;
        kit::popup(kR);
        gfx::setColor(1, 1, 1);
        kit::centred(kit::font(1), kit::text(kTitle, which), 320, 64);
        const Font& f = kit::font(2);
        kit::centred(f, format(kit::text(kLine, which), city->name), 320, 115);
        kit::centred(f, format(kit::text(kGold, 0), gold), 320, 135);
        kit::centred(
                f,
                format(kit::text(kLost, lost.size() == 1 ? 0 : 1), static_cast<int>(lost.size())),
                320, 165);
        kit::centred(f, format(kit::text(kLeft, left == 1 ? 0 : 1), left), 320, 185);
        kit::bevel(184, 220, 272, 130, 4, 2);
        kit::bevel(185, 221, 270, 128, 2, 4);
        gfx::setColor(1, 1, 1);
        f.draw(kit::text(kTitle, 2), 192, 230);
        f.draw(kit::text(kTitle, 3), 368, 230);
        for (int i = 0; i <= 2; i++) {
            int y = 250 + 30 * i;
            bool have = i < static_cast<int>(lost.size());
            kit::army(have ? lost[i].first : w2::NONE, G.player->index, 208, y, 1);
            if (have) {
                gfx::setColor(1, 1, 1);
                const w2::ArmyType* t = G.g->types.byId(lost[i].first);
                f.draw(t ? t->name : "", 248, y + 6);
                f.draw(w2::fmt("%d gp", lost[i].second), 368, y + 6);
            }
        }
    }
    void mousepressed(int, int, int) override { Close(); }
    void keypressed(const std::string&) override { Close(); }
};
}  // namespace

void open(bool sacked, w2::City* city, int gold, const std::vector<std::pair<int, int>>& lost,
          int left, kit::Done after) {
    auto d = std::make_shared<Spoils>();
    d->sacked = sacked;
    d->city = city;
    d->gold = gold;
    d->lost = lost;
    d->left = left;
    d->after = after;
    kit::push(d);
}

}  // namespace spoils
