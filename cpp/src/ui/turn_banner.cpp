/**
 * @file
 * @brief Start-of-turn faction banner modal (`TurnBannerModal`) pushed onto `w2::ModalStack`.
 *
 * Popup 6 at `(160, 60)` `320x312` (`CITY.PCK`), framed in the active side's banner and edge
 * colours with the faction name and `"Turn %d"` over it. Dismissed by any mouse click or key.
 */
#include <memory>
#include <string>
#include <utility>

#include "absl/strings/str_format.h"

#include "ui/dialogs.hpp"

namespace turn_banner {

namespace {

constexpr kit::Rect kBannerRect{160, 60, 320, 312};
constexpr int kBannerNameY = 85;
constexpr int kBannerTurnY = 130;

/**
 * @brief Modal displaying the start-of-turn `CITY.PCK` banner until dismissed by click or key.
 */
class TurnBannerModal final : public kit::Modal {
  public:
    TurnBannerModal(w2::UiContext& ctx, std::string name, int turn, int colour, int edge,
                    kit::Done on_dismiss)
            : kit::Modal(ctx)
            , name_(std::move(name))
            , turn_(turn)
            , colour_(colour)
            , edge_(edge)
            , on_dismiss_(std::move(on_dismiss)) {
        SetGreysMainControls(true);
        SetIsTurnBanner(true);
    }

    void Draw() override {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        auto fill = [&ren](int x, int y, int width, int height) {
            ren.Rectangle(gfx::kFill, x, y, width, height);
        };
        kit::PopupFrame(ctx, kBannerRect);
        ren.SetColor(1, 1, 1);
        ren.Draw(ctx.Assets().CityPic(), kBannerRect.x, kBannerRect.y);
        auto outline = [&fill](int x, int y, int width, int height) {
            fill(kBannerRect.x + x, kBannerRect.y + y, width, 1);
            fill(kBannerRect.x + x, kBannerRect.y + y + height - 1, width, 1);
            fill(kBannerRect.x + x, kBannerRect.y + y, 1, height);
            fill(kBannerRect.x + x + width - 1, kBannerRect.y + y, 1, height);
        };
        kit::SetPal(ctx, edge_);
        outline(0, 0, kBannerRect.w, kBannerRect.h);
        kit::SetPal(ctx, colour_);
        fill(kBannerRect.x + 1, kBannerRect.y + 1, kBannerRect.w - 2, 9);
        fill(kBannerRect.x + 1, kBannerRect.y + 1, 9, kBannerRect.h - 2);
        fill(kBannerRect.x + 1, kBannerRect.y + kBannerRect.h - 10, kBannerRect.w - 2, 9);
        fill(kBannerRect.x + kBannerRect.w - 10, kBannerRect.y + 1, 9, kBannerRect.h - 2);
        kit::SetPal(ctx, edge_);
        outline(10, 10, kBannerRect.w - 20, kBannerRect.h - 20);
        ren.SetColor(1, 1, 1);
        const Font& title_font = ctx.Assets().TitleFont();
        kit::Centred(title_font, name_, 320, kBannerNameY);
        kit::Centred(title_font, absl::StrFormat("Turn %d", turn_), 320, kBannerTurnY);
    }

    void Mousepressed(int, int, int) override { Dismiss(); }
    void Keypressed(const std::string&) override { Dismiss(); }

  private:
    void Dismiss() {
        kit::Done after = std::move(on_dismiss_);
        PopSelf();
        if (after) after();
    }

    std::string name_;      ///< Faction display name.
    int turn_ = 1;          ///< 1-based round number.
    int colour_ = 15;       ///< Faction fill colour index.
    int edge_ = 0;          ///< Faction border outline colour index.
    kit::Done on_dismiss_;  ///< Continuation run once the banner closes.
};

}  // namespace

void Open(w2::UiContext& ctx, const w2::Side& side, kit::Done on_dismiss) {
    ctx.Audio().Effect("turn");
    auto modal =
            std::make_shared<TurnBannerModal>(ctx, side.Name(), ctx.ActiveGame().Turn(),
                                              side.Colour(), side.Edge(), std::move(on_dismiss));
    kit::Push(ctx, std::move(modal));
}

}  // namespace turn_banner
