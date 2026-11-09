/**
 * @file
 * @brief City-captured spoils choice modal (`CityVictoryModal`) pushed onto `w2::ModalStack`.
 *
 * Dialog 11 over popup 7 (`VICTORY.PCK` at `(160, 90)` `320x200`) offering Occupy (`285`),
 * Pillage (`283`), Sack (`286`), and Raze (`284`). When Pillage, Sack, or Raze opens a follow-up
 * modal (`spoils::Open` or `search::Say`), `CityVictoryModal` remains underneath on `ModalStack`
 * until the follow-up closes.
 */
#include <array>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/ai/groups.hpp"
#include "warlords/armytype.hpp"
#include "warlords/game.hpp"
#include "warlords/quest.hpp"
#include "warlords/uidata.hpp"

namespace city_victory {

namespace {

constexpr kit::Rect kVictoryPopup{160, 90, 320, 200};
constexpr int kVictoryDialog = 11;
constexpr int kPillage = 283;
constexpr int kRaze = 284;
constexpr int kOccupy = 285;
constexpr int kSack = 286;
constexpr int kVictoryTextGroup = 65;
constexpr int kVictoryWhoGroup = 66;
constexpr int kVictoryWhereGroup = 67;
constexpr int kVictoryTitleY = 93;
constexpr std::array<int, 4> kVictoryLineY = {140, 160, 180, 200};

/**
 * @brief Modal asking how to treat a newly conquered city (`Occupy`, `Pillage`, `Sack`, `Raze`).
 */
class CityVictoryModal final : public kit::Modal {
  public:
    CityVictoryModal(w2::UiContext& ctx, w2::City* city, const std::string& victor)
            : kit::Modal(ctx), city_(city), view_(kit::View(ctx, kVictoryDialog)) {
        SetIsCityVictory(true);
        w2::Game& game = ctx.ActiveGame();
        who_ = w2::Format(kit::Text(ctx, kVictoryWhoGroup, game.GetRng().Dice(1, 4, -1)), victor);
        where_ = w2::Format(kit::Text(ctx, kVictoryWhereGroup, game.GetRng().Dice(1, 3, -1)),
                            city_->Name());

        auto& states = view_.state;
        states[kOccupy] = w2::uidata::kNormal;
        states[kRaze] = w2::uidata::kNormal;
        int pillage_gold =
                city_->Slots().empty()
                        ? 0
                        : std::abs(game.ArmyTypes().ById(city_->Slots().back().type)->price) / 2;
        int sack_gold = w2::ai::groups::SackValue(game, *city_);
        states[kPillage] = pillage_gold != 0 ? w2::uidata::kNormal : w2::uidata::kDisabled;
        states[kSack] = sack_gold != 0 ? w2::uidata::kNormal : w2::uidata::kDisabled;

        enter_ = kOccupy;
        w2::Quest* quest = ctx.Player() ? ctx.Player()->ActiveQuest() : nullptr;
        w2::Army* hero = nullptr;
        if (const w2::Selection* sel = ctx.ActiveSelection()) {
            for (w2::Army* army : sel->stack) {
                if (army->TypeIndex() == w2::armytype::kHero) {
                    hero = army;
                    break;
                }
            }
        }
        if (game.GetMap().GameOptions().quests != 0 && quest && hero &&
            hero->Id() == quest->HeroId()) {
            if (quest->Type() == w2::quest::kRaze && quest->TargetCityId() == city_->Id()) {
                enter_ = kRaze;
            } else if (quest->Type() == w2::quest::kPillageGold) {
                if (sack_gold != 0) {
                    enter_ = kSack;
                } else if (pillage_gold != 0) {
                    enter_ = kPillage;
                }
            }
        }
    }

    void Draw() override {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        ren.SetColor(0, 0, 0);
        ren.Rectangle(gfx::kLine, kVictoryPopup.x - 0.5, kVictoryPopup.y - 0.5, kVictoryPopup.w + 1,
                      kVictoryPopup.h + 1);
        ren.Rectangle(gfx::kFill, kVictoryPopup.x + 1, kVictoryPopup.y + kVictoryPopup.h + 1,
                      kVictoryPopup.w + 2, 2);
        ren.Rectangle(gfx::kFill, kVictoryPopup.x + kVictoryPopup.w + 1, kVictoryPopup.y + 1, 2,
                      kVictoryPopup.h + 2);
        ren.SetColor(1, 1, 1);
        ren.Draw(ctx.Assets().VictoryPic(), kVictoryPopup.x, kVictoryPopup.y);
        kit::Centred(ctx.Assets().TitleFont(), kit::Text(ctx, kVictoryTextGroup, 0), 320,
                     kVictoryTitleY);
        const Font& big_font = ctx.Assets().BigFont();
        kit::Centred(big_font, who_, 320, kVictoryLineY[0]);
        kit::Centred(big_font, where_, 320, kVictoryLineY[1]);
        kit::Centred(big_font, kit::Text(ctx, kVictoryTextGroup, 1), 320, kVictoryLineY[2]);
        kit::Centred(big_font, kit::Text(ctx, kVictoryTextGroup, 2), 320, kVictoryLineY[3]);
        kit::DrawControls(ctx, view_);
    }

    void Mousepressed(int x, int y, int button) override {
        if (resolved_ || button == 2) return;
        const auto* ctrl = screen::DialogControlAt(view_, x, y);
        if (ctrl && view_.StateOf(ctrl->id) != w2::uidata::kDisabled) {
            TakeCity(ctrl->id);
        }
    }

    void Keypressed(const std::string& key) override {
        if (resolved_) return;
        if (key == "return" || key == "kpenter" || key == "escape") {
            TakeCity(enter_);
        }
    }

  private:
    void TakeCity(int action_id) {
        resolved_ = true;
        w2::UiContext& ctx = Ctx();
        w2::City* target_city = city_;
        std::vector<w2::Army*> stack =
                ctx.ActiveSelection() ? ctx.ActiveSelection()->stack : std::vector<w2::Army*>{};
        auto self = shared_from_this();
        auto done = [self]() {
            self->PopSelf();
            self->Ctx().GetViewport().StratDirty();
        };

        if (action_id == kPillage || action_id == kSack) {
            bool sacked = action_id == kSack;
            auto spoils_result =
                    sacked ? w2::game::Sack(ctx.ActiveGame(), *ctx.Player(), *target_city, stack)
                           : w2::game::Pillage(ctx.ActiveGame(), *ctx.Player(), *target_city,
                                               stack);
            spoils::Open(ctx, sacked, target_city, spoils_result.gold, spoils_result.lost,
                         target_city->SlotCount(), done);
        } else if (action_id == kRaze) {
            search::Say(ctx, target_city->Name() + " is in ruins!",
                        [self, target_city, stack, done]() {
                            w2::UiContext& inner_ctx = self->Ctx();
                            w2::game::Raze(inner_ctx.ActiveGame(), *inner_ctx.Player(),
                                           *target_city, stack);
                            done();
                        });
        } else {
            PopSelf();
            auto news = w2::game::Occupy(ctx.ActiveGame(), *ctx.Player(), *target_city, stack);
            auto production = [&ctx, target_city]() {
                city::Open(ctx, target_city, city::kProduction);
            };
            if (news) {
                ctx.ActiveGame().TurnNews(ctx.Player()->Id()).quest_news.reset();
                if (!news->failed.empty()) {
                    questnews::Show(ctx, news, production);
                } else {
                    questnews::Show(ctx, news);
                }
            } else {
                production();
            }
        }
    }

    w2::City* city_ = nullptr;  ///< Conquered city.
    screen::View view_;         ///< Dialog 11 controls and states.
    std::string who_;           ///< Formatted conqueror headline.
    std::string where_;         ///< Formatted conquered-city line.
    int enter_ = kOccupy;       ///< Default button ID chosen by Enter/Escape.
    bool resolved_ = false;     ///< True while waiting beneath a Pillage/Sack/Raze follow-up modal.
};

}  // namespace

void Open(w2::UiContext& ctx, w2::City* city, const std::string& victor) {
    auto modal = std::make_shared<CityVictoryModal>(ctx, city, victor);
    kit::Push(ctx, std::move(modal));
}

}  // namespace city_victory
