/**
 * @file
 * @brief SSG > About Warlords II, and the "?" key.
 *
 * Popup 23, (160, 55) 336x347, BSCROLL.PCK drawn through its mask and with
 * no frame. The game writes up to six lines of group 139 centred on
 * x = 328, black edged in colour 7: "", "Version 1.02", "", and three lines
 * of DOS memory the remake has none of. Any key or click puts it away.
 */
#include "front/pckimage.hpp"
#include "ui/dialogs.hpp"

namespace about {

namespace {
const kit::Rect kPopupRect{160, 55, 336, 347};  ///< Popup 23.
gfx::ImageP bscroll;                            ///< BSCROLL.PCK, loaded on first use.

/** @brief The About box: a scroll with the version lines on it. */
struct About : kit::Modal {
    std::vector<std::string> lines;  ///< One per row, 20 pixels apart; "" leaves a row blank.

    /** @brief Draws the scroll and the non-empty lines centred on it. */
    void Draw() override {
        gfx::SetColor(1, 1, 1);
        gfx::Draw(bscroll, gfx::NewQuad(0, 0, kPopupRect.w, kPopupRect.h), kPopupRect.x,
                  kPopupRect.y);
        const Font& font = kit::GetFont(2).Colours(0, 7);
        for (size_t i = 0; i < lines.size(); i++)
            if (!lines[i].empty())
                kit::Centred(font, lines[i], 328, 141 + 20 * static_cast<int>(i + 1));
    }
    void Mousepressed(int, int, int) override { kit::Pop(this); }
    void Keypressed(const std::string&) override { kit::Pop(this); }
};
}  // namespace

/** @brief Opens the About box, loading the scroll picture the first time. */
void Open() {
    if (!bscroll) bscroll = pckimage::Load(G.data_dir + "/PICS/BSCROLL.PCK", G.palette, 10);
    auto dialog = std::make_shared<About>();
    // Group 139's lines 1 and 3 with the version between them.
    dialog->lines = {kit::Text(0x8b, 1), "Version 1.02", kit::Text(0x8b, 3)};
    kit::Push(dialog);
}

}  // namespace about
