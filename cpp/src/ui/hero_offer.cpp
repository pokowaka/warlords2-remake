/**
 * @file
 * @brief Turn-start hero recruitment modal (`HeroOfferModal`) pushed onto `w2::ModalStack`.
 *
 * Popup 2 at `(80, 60)` `480x312` (`MARBLE.PCK` cropped) with dialog 12's controls, strategic map,
 * hero portrait (`MHERO.PCK` / `FHERO.PCK`), editable hero name field, and Male/Female checkboxes.
 */
#include <algorithm>
#include <array>
#include <cctype>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/hero.hpp"

namespace hero_offer {

namespace {

constexpr kit::Rect kHeroPopup{80, 60, 480, 312};
constexpr int kHeroDialog = 12;
constexpr int kHeroOk = 287;
constexpr int kHeroCancel = 288;
constexpr int kHeroField = 289;
constexpr int kHeroMale = 290;
constexpr int kHeroFemale = 291;
constexpr size_t kHeroNameMax = 19;

constexpr int kHeroMapX = 80;
constexpr int kHeroMapY = 60;
constexpr int kHeroPicX = 320;
constexpr int kHeroPicY = 110;
constexpr int kHeroPicW = 224;
constexpr int kHeroPicH = 170;
constexpr int kHeroTitleY = 63;
constexpr int kHeroCentre = 432;
constexpr std::array<int, 4> kHeroLineY = {190, 210, 230, 250};
constexpr std::array<int, 2> kMaleLabel = {376, 315};
constexpr std::array<int, 2> kFemaleLabel = {480, 315};
constexpr int kHeroTitleGroup = 0x5f;
constexpr int kAllyGroup = 0x60;
constexpr int kHeroLineGroup = 0x61;
constexpr int kHeroSexGroup = 0x62;

bool IsAllowedHeroNameChars(const std::string& typed) {
    if (typed.empty()) return false;
    for (unsigned char chr : typed) {
        bool allowed = std::isalnum(chr) != 0 || std::isspace(chr) != 0 || chr == '\'' ||
                       chr == '-' || chr == '.';
        if (!allowed) return false;
    }
    return true;
}

std::array<std::string, 4> BuildHeroLines(const w2::UiContext& ctx, const w2::HeroOffer& offer,
                                          bool female) {
    int first = female ? 8 : 0;
    auto line = [&](int idx) { return kit::Text(ctx, kHeroLineGroup, first + idx); };
    if (offer.first) {
        return {line(0), line(1), line(2), w2::Format(line(3), offer.city->Name())};
    }
    int gold = ctx.Player() ? ctx.Player()->Gold() : 0;
    return {w2::Format(line(4), offer.city->Name()), w2::Format(line(5), offer.price),
            w2::Format(line(6), gold), line(7)};
}

/**
 * @brief Modal presenting a turn-opening hero recruitment offer.
 */
class HeroOfferModal final : public kit::Modal {
  public:
    using SharedOffer = std::shared_ptr<w2::HeroOffer>;

    HeroOfferModal(w2::UiContext& ctx, SharedOffer offer, kit::Done on_finished)
            : kit::Modal(ctx)
            , offer_(std::move(offer))
            , view_(kit::View(ctx, kHeroDialog))
            , offer_female_(offer_->female)
            , offer_name_(offer_->name.empty() ? "Hero" : offer_->name)
            , on_finished_(std::move(on_finished)) {
        SetGreysMainControls(true);
        SetIsHeroOffer(true);
        view_.state[kHeroCancel] = offer_->first ? w2::uidata::kDisabled : w2::uidata::kNormal;
        view_.state[kHeroOk] = w2::uidata::kNormal;
    }

    int TypingState() const override { return field_clicked_ ? 2 : 1; }

    void Draw() override {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        kit::PopupFrame(ctx, kHeroPopup);
        ren.SetColor(1, 1, 1);
        ren.SetScissor(kHeroPopup.x, kHeroPopup.y, kHeroPopup.w, kHeroPopup.h);
        ren.Draw(ctx.Assets().Marble(), kHeroPopup.x, kHeroPopup.y);
        ren.SetScissor();

        ctx.Actions().DrawStrategicMap(kHeroMapX, kHeroMapY);

        ren.SetScissor(kHeroPopup.x, kHeroPopup.y, kHeroPopup.w, kHeroPopup.h);
        ren.SetColor(1, 1, 1);
        int fig_x = std::max(0, (offer_->city->X() * 2 - 2) / 8 * 8);
        int fig_y = std::max(0, offer_->city->Y() * 2 - 6);
        ren.Draw(ctx.Assets().AtransShields(), gfx::NewQuad(96, 0, 16, 15), kHeroMapX + fig_x,
                 kHeroMapY + fig_y);
        ren.Draw(offer_female_ ? ctx.Assets().HeroPicFemale() : ctx.Assets().HeroPicMale(),
                 kHeroPicX, kHeroPicY);
        ren.SetColor(0, 0, 0);
        ren.Rectangle(gfx::kLine, kHeroPicX - 0.5, kHeroPicY - 0.5, kHeroPicW + 1, kHeroPicH + 1);
        ren.SetScissor();

        auto centred = [&ren](const Font& font, const std::string& str, int y) {
            ren.SetColor(1, 1, 1);
            font.Draw(str, kHeroCentre - font.Width(str) / 2, y);
        };
        centred(ctx.Assets().TitleFont(), kit::Text(ctx, kHeroTitleGroup, 0), kHeroTitleY);
        auto lines = BuildHeroLines(ctx, *offer_, offer_female_);
        const Font& big_font = ctx.Assets().BigFont();
        for (size_t i = 0; i < 4; i++) {
            if (!lines[i].empty()) {
                centred(big_font.Colours(15, 14), lines[i], kHeroLineY[i]);
            }
        }
        if (const auto* field = screen::DialogControl(view_, kHeroField)) {
            ren.SetColor(0, 0, 0);
            kit::Outline(ctx, field->x - 2, field->y - 2, field->w + 4, field->h + 4);
            kit::Field(ctx, field->x, field->y, field->w, field->h, offer_name_, &big_font);
        }
        auto draw_label = [&ren, &big_font](const std::array<int, 2>& pos, const std::string& str) {
            ren.SetColor(1, 1, 1);
            big_font.Draw(str, pos[0] - big_font.Width(str), pos[1]);
        };
        draw_label(kMaleLabel, kit::Text(ctx, kHeroSexGroup, 0));
        draw_label(kFemaleLabel, kit::Text(ctx, kHeroSexGroup, 1));

        auto draw_box = [&ctx, &ren, this](int control_id, bool checked) {
            const auto* ctrl = screen::DialogControl(view_, control_id);
            if (!ctrl) return;
            ren.SetColor(1, 1, 1);
            ren.Draw(ctx.Assets().Abits(), gfx::NewQuad(320, checked ? 0 : 20, 24, 20), ctrl->x,
                     ctrl->y);
        };
        draw_box(kHeroMale, !offer_female_);
        draw_box(kHeroFemale, offer_female_);
        kit::DrawControls(ctx, view_);
    }

    void Mousepressed(int x, int y, int button) override {
        if (button == 2) return;
        const auto* ctrl = screen::DialogControlAt(view_, x, y);
        if (!ctrl) return;
        if (ctrl->id == kHeroField) {
            field_clicked_ = true;
        } else if (ctrl->id == kHeroMale) {
            offer_female_ = false;
        } else if (ctrl->id == kHeroFemale) {
            offer_female_ = true;
        } else if (ctrl->id == kHeroOk) {
            Accept();
        } else if (ctrl->id == kHeroCancel && !offer_->first) {
            Refuse();
        }
    }

    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter") {
            Accept();
        } else if (key == "backspace" && !offer_name_.empty()) {
            offer_name_.pop_back();
        } else if (key == "escape" && !offer_->first) {
            Refuse();
        }
    }

    void Textinput(const std::string& text) override {
        if (offer_name_.size() < kHeroNameMax && IsAllowedHeroNameChars(text)) {
            offer_name_ += text;
            if (offer_name_.size() > kHeroNameMax) offer_name_.resize(kHeroNameMax);
        }
    }

  private:
    void Accept() {
        w2::UiContext& ctx = Ctx();
        offer_->name = offer_name_;
        offer_->female = offer_female_;
        auto hired = w2::hero::Recruit(ctx.ActiveGame(), *ctx.Player(), *offer_);
        w2::Army* hero = hired.hero;
        ctx.ActiveGame().TurnNews(ctx.Player()->Id()).hero_offer.reset();
        kit::Done after = std::move(on_finished_);
        PopSelf();
        ctx.CentreOn(hero->X(), hero->Y());
        if (hired.count > 0) {
            std::string line = hired.count == 1
                                       ? kit::Text(ctx, kAllyGroup, 0)
                                       : w2::Format(kit::Text(ctx, kAllyGroup, 1), hired.count);
            search::Say(ctx, line, std::move(after));
            return;
        }
        if (after) after();
    }

    void Refuse() {
        w2::UiContext& ctx = Ctx();
        ctx.ActiveGame().TurnNews(ctx.Player()->Id()).hero_offer.reset();
        kit::Done after = std::move(on_finished_);
        PopSelf();
        if (after) after();
    }

    SharedOffer offer_;           ///< Pending hero offer record.
    screen::View view_;           ///< Dialog 12 controls and states.
    bool offer_female_ = false;   ///< Selected gender checkbox state.
    bool field_clicked_ = false;  ///< True once `kHeroField` was clicked (requests OSK).
    std::string offer_name_;      ///< Edited hero name.
    kit::Done on_finished_;       ///< Continuation invoked once accepted/refused.
};

}  // namespace

bool Present(w2::UiContext& ctx, w2::Side& side, kit::Done on_finished) {
    const auto& side_offer = ctx.ActiveGame().TurnNews(side.Id()).hero_offer;
    if (!side_offer) return false;
    auto modal = std::make_shared<HeroOfferModal>(ctx, side_offer, std::move(on_finished));
    kit::Push(ctx, std::move(modal));
    return true;
}

}  // namespace hero_offer
