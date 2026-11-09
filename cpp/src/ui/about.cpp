/**
 * @file
 * @brief SSG > About Warlords II, and the "?" key.
 *
 * Popup 23, (160, 55) 336x347, BSCROLL.PCK drawn through its mask and with
 * no frame. The game writes up to six lines of group 139 centred on
 * x = 328, black edged in colour 7: "", "Version 1.02", "", and three lines
 * of DOS memory the remake has none of. Any key or click puts it away.
 */
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ui/dialogs.hpp"

namespace about {

namespace {
constexpr kit::Rect kPopupRect{160, 55, 336, 347};  ///< Popup 23.
constexpr int kScrollMaskKey = 10;                  ///< Transparent palette index in BSCROLL.PCK.

/** @brief The About box: a scroll with the version lines on it. */
class About final : public kit::Modal {
  public:
    using LineList = std::vector<std::string>;

    About(w2::UiContext& ctx, LineList lines) : kit::Modal(ctx), lines_(std::move(lines)) {}

    /** @brief Draws the scroll and the non-empty lines centred on it. */
    void Draw() override {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        gfx::ImageP bscroll = ctx.Assets().CachedImage(
                ren, ctx.Assets().DataDir() / "PICS" / "BSCROLL.PCK", kScrollMaskKey);
        ren.SetColor(1, 1, 1);
        ren.Draw(bscroll, gfx::NewQuad(0, 0, kPopupRect.w, kPopupRect.h), kPopupRect.x,
                 kPopupRect.y);
        const Font& font = kit::GetFont(ctx, 2).Colours(0, 7);
        for (size_t i = 0; i < lines_.size(); i++) {
            if (!lines_[i].empty()) {
                kit::Centred(font, lines_[i], 328, 141 + 20 * static_cast<int>(i + 1));
            }
        }
    }

    void Mousepressed(int, int, int) override { PopSelf(); }
    void Keypressed(const std::string&) override { PopSelf(); }

  private:
    LineList lines_;  ///< One per row, 20 pixels apart; "" leaves a row blank.
};
}  // namespace

/** @brief Opens the About box, loading the scroll picture the first time. */
void Open(w2::UiContext& ctx) {
    ctx.Assets().CachedImage(ctx.Renderer(), ctx.Assets().DataDir() / "PICS" / "BSCROLL.PCK",
                             kScrollMaskKey);
    // Group 139's lines 1 and 3 with the version between them.
    About::LineList lines = {kit::Text(ctx, 0x8b, 1), "Version 1.02", kit::Text(ctx, 0x8b, 3)};
    kit::Push(ctx, std::make_shared<About>(ctx, std::move(lines)));
}

}  // namespace about
