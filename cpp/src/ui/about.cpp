// SSG > About Warlords II, and the "?" key (7721:0084).
//
// Popup 23, (160, 55) 336x347, BSCROLL.PCK through its mask and with no
// frame. 8065:1471 writes up to six lines of group 139 centred on x = 328,
// black edged in colour 7: "", "Version 1.02", "", and three lines of DOS
// memory the remake has none of. Any key or click puts it away.
#include "front/pckimage.hpp"
#include "ui/dialogs.hpp"

namespace about {

namespace {
const kit::Rect kR{160, 55, 336, 347};  // popup 23
gfx::ImageP bscroll;

struct About : kit::Modal {
    std::vector<std::string> lines;
    void Draw() override {
        gfx::SetColor(1, 1, 1);
        gfx::Draw(bscroll, gfx::NewQuad(0, 0, kR.w, kR.h), kR.x, kR.y);
        const Font& f = kit::GetFont(2).Colours(0, 7);
        for (size_t i = 0; i < lines.size(); i++)
            if (!lines[i].empty())
                kit::Centred(f, lines[i], 328, 141 + 20 * static_cast<int>(i + 1));
    }
    void Mousepressed(int, int, int) override { kit::Pop(this); }
    void Keypressed(const std::string&) override { kit::Pop(this); }
};
}  // namespace

void Open() {
    if (!bscroll) bscroll = pckimage::Load(G.data_dir + "/PICS/BSCROLL.PCK", G.palette, 10);
    auto d = std::make_shared<About>();
    d->lines = {kit::Text(0x8b, 1), "Version 1.02", kit::Text(0x8b, 3)};  // 4125:1292
    kit::Push(d);
}

}  // namespace about
