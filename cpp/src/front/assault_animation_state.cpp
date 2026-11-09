#include <algorithm>
#include <array>
#include <utility>

#include "front/map_screen_controller.hpp"
#include "front/map_screen_state.hpp"
#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/armytype.hpp"
#include "warlords/game.hpp"

namespace w2 {

namespace {

constexpr kit::Rect kAssaultWindow{160, 60, 320, 312};
constexpr int kCloudWidth = 128;
constexpr int kCloudHeight = 120;
constexpr int kShieldWidth = 32;
constexpr int kShieldHeight = 36;
constexpr std::array<int, 2> kShieldDefPos = {176, 86};
constexpr std::array<int, 2> kShieldAtkPos = {176, 246};
constexpr std::array<int, 4> kDefRows = {86, 116, 146, 176};
constexpr int kAtkRow = 246;
constexpr std::array<int, 8> kXEven = {216, 248, 280, 312, 344, 376, 408, 440};
constexpr std::array<int, 7> kXOdd = {232, 264, 296, 328, 360, 392, 424};
constexpr std::array<int, 4> kSeaQuad = {0, 162, 32, 18};
constexpr int kSeaDrop = 10;
constexpr int kTextStartY = 298;
constexpr int kTextStepY = 20;
constexpr double kMinCloudSeconds = 0.7;
constexpr double kFellSeconds = 13.0 / 18.2;
constexpr double kFellFastSeconds = 5.0 / 18.2;
constexpr double kBlastSeconds = 5.0 / 18.2;
constexpr double kBlastFastSeconds = 2.0 / 18.2;

constexpr int kGroupFled = 141;
constexpr int kGroupWonCityHero = 142;
constexpr int kGroupWonCity = 143;
constexpr int kGroupWonHero = 144;
constexpr int kGroupWon = 145;
constexpr int kGroupLost = 146;
constexpr int kGroupLoot = 147;

int OwnerOrNeutral(int owner) {
    return owner == kNone ? 8 : owner;
}

int SlotX(int n, int slot_num) {
    if (n >= 8) return kXEven[static_cast<size_t>(slot_num - 1)];
    int from = 4 - (n + 1) / 2;
    size_t idx = static_cast<size_t>(from + slot_num - 1);
    return n % 2 == 1 ? kXOdd[idx] : kXEven[idx];
}

AssaultAnimationState::SlotList LineSlots(int n, absl::Span<const int> rows) {
    AssaultAnimationState::SlotList out(static_cast<size_t>(n));
    int start = 1;
    size_t row = 0;
    while (n - start + 1 >= 8 && row + 1 < rows.size()) {
        for (int col = 1; col <= 8; col++) {
            out[static_cast<size_t>(start + col - 2)] = {kXEven[static_cast<size_t>(col - 1)],
                                                         rows[row]};
        }
        start += 8;
        ++row;
    }
    int y = row < rows.size() ? rows[row] : rows.back();
    for (int col = 1; col <= n - start + 1; col++) {
        out[static_cast<size_t>(start + col - 2)] = {SlotX(n - start + 1, col), y};
    }
    return out;
}

std::string VictorName(const Game& game, const AssaultAnimationState::ArmyList& line) {
    for (Army* army : line) {
        if (army->Hero() && !army->Name().empty()) return army->Name();
    }
    const ArmyType* rec = line.empty() ? nullptr : game.ArmyTypes().ById(line[0]->Type());
    return rec ? rec->name : "Your armies";
}

AssaultAnimationState::LineList OutcomeLines(UiContext& ctx, const Battle& result, const City* city,
                                             bool fled) {
    AssaultAnimationState::LineList out;
    if (!result.won) {
        out.push_back(kit::Text(ctx, kGroupLost, 0));
        return out;
    }
    std::string hero_name;
    for (Army* army : result.lines.attackers) {
        if (army->Hero() && !army->Name().empty()) {
            hero_name = army->Name();
            break;
        }
    }
    if (city) {
        if (fled) {
            out.push_back(kit::Text(ctx, kGroupFled, ctx.ActiveGame().GetRng().Dice(1, 4, -1)));
        }
        if (!hero_name.empty()) {
            out.push_back(Format(kit::Text(ctx, kGroupWonCityHero, 0), hero_name));
        } else {
            out.push_back(kit::Text(ctx, kGroupWonCity, 0));
        }
    } else if (!hero_name.empty()) {
        out.push_back(Format(kit::Text(ctx, kGroupWonHero, 0), hero_name));
        out.push_back(kit::Text(ctx, kGroupWonHero, 1));
    } else {
        out.push_back(kit::Text(ctx, kGroupWon, 0));
    }
    if (result.loot > 0) out.push_back(Format(kit::Text(ctx, kGroupLoot, 0), result.loot));
    return out;
}

}  // namespace

AssaultAnimationState::AssaultAnimationState(MapScreenController& ctrl, AssaultSpec spec,
                                             StatePtr resume_state)
        : tile_x_(spec.x)
        , tile_y_(spec.y)
        , result_(std::move(spec.result))
        , shared_battle_(spec.shared_battle)
        , defenders_(result_.lines.defenders)
        , attackers_(result_.lines.attackers)
        , def_slots_(LineSlots(static_cast<int>(defenders_.size()), kDefRows))
        , atk_slots_(LineSlots(static_cast<int>(attackers_.size()), std::array<int, 1>{kAtkRow}))
        , def_side_(defenders_.empty() ? 8 : OwnerOrNeutral(defenders_[0]->OwnerIndex()))
        , atk_side_(attackers_.empty() ? 8 : OwnerOrNeutral(attackers_[0]->OwnerIndex()))
        , def_fell_(defenders_.size())
        , atk_fell_(attackers_.size())
        , phase_(Phase::kCloud)
        , at_(Now())
        , computer_(spec.computer)
        , window_(spec.window)
        , outcome_(std::move(spec.outcome))
        , after_(std::move(spec.after))
        , resume_state_(resume_state ? std::move(resume_state)
                                     : std::make_unique<HumanTurnState>()) {
    UiContext& ctx = ctrl.Ctx();
    Game& game = ctx.ActiveGame();
    cloud_time_ = (game.CurrentSide() && !game.CurrentSide()->IsComputerControlled())
                          ? ctx.Audio().Effect("war")
                          : 0.0;
    victor_ = VictorName(game, attackers_);
    if (computer_ && !outcome_.empty()) {
        message_ = {outcome_};
    } else {
        message_ = OutcomeLines(ctx, result_, result_.lines.city, defenders_.empty());
    }
}

bool AssaultAnimationState::IsComputerTurn() const {
    return resume_state_ && resume_state_->IsComputerTurn();
}

Side* AssaultAnimationState::ActiveComputerSide() const {
    return resume_state_ ? resume_state_->ActiveComputerSide() : nullptr;
}

const AiStatus* AssaultAnimationState::ActiveAiStatus() const {
    return resume_state_ ? resume_state_->ActiveAiStatus() : nullptr;
}

void AssaultAnimationState::SetAiStatusText(const std::string& text) {
    if (resume_state_) resume_state_->SetAiStatusText(text);
}

void AssaultAnimationState::Close(MapScreenController& ctrl, Army*& out_medal) {
    UiContext& ctx = ctrl.Ctx();
    Battle local_result = std::move(result_);
    Battle* shared = shared_battle_;
    auto after_cb = std::move(after_);
    StatePtr restored = std::move(resume_state_);
    ctrl.SetState(std::move(restored));

    if (shared != nullptr) {
        game::ApplyAttack(ctx.ActiveGame(), *shared);
        out_medal = shared->medal;
    } else {
        game::ApplyAttack(ctx.ActiveGame(), local_result);
        out_medal = local_result.medal;
    }
    ctx.GetViewport().StratDirty();
    if (after_cb) after_cb();
}

void AssaultAnimationState::Advance(MapScreenController& ctrl) {
    UiContext& ctx = ctrl.Ctx();
    double now = Now();
    if (phase_ == Phase::kCloud) {
        if (now - at_ >= std::max(kMinCloudSeconds, cloud_time_)) {
            if (computer_ && !window_) {
                Army* unused_medal = nullptr;
                Close(ctrl, unused_medal);
                return;
            }
            phase_ = Phase::kBattle;
            at_ = computer_ ? (now + 5.0 / 18.2) : now;
        }
        return;
    }
    if (phase_ == Phase::kOver && computer_) {
        if (close_at_ == 0.0) {
            close_at_ = now + 15.0 / 18.2;
            SetAiStatusText(outcome_);
        } else if (now >= close_at_) {
            Army* unused_medal = nullptr;
            Close(ctrl, unused_medal);
        }
        return;
    }
    if (phase_ != Phase::kBattle) return;

    double each = fast_ ? kFellFastSeconds : kFellSeconds;
    const auto& log = result_.log;
    while (step_ < log.size() && now >= at_) {
        ++step_;
        Fell fell{at_, fast_ ? kBlastFastSeconds : kBlastSeconds};
        if (log[step_ - 1] == 1) {
            if (atk_down_ < static_cast<int>(atk_fell_.size())) {
                atk_fell_[static_cast<size_t>(atk_down_)] = fell;
            }
            ++atk_down_;
        } else {
            if (def_down_ < static_cast<int>(def_fell_.size())) {
                def_fell_[static_cast<size_t>(def_down_)] = fell;
            }
            ++def_down_;
        }
        if (!fast_) ctx.Audio().Effect(log[step_ - 1] == 1 ? "army" : "army2");
        at_ += each;
    }
    if (step_ >= log.size() && now >= at_) phase_ = Phase::kOver;
}

void AssaultAnimationState::Press(MapScreenController& ctrl) {
    if (phase_ != Phase::kOver) {
        fast_ = true;
        at_ = std::min(at_, Now() + kFellFastSeconds);
        Advance(ctrl);
        return;
    }
    City* city = result_.captured;
    bool was_computer = computer_;
    std::string victor = victor_;
    Army* medal = nullptr;
    Close(ctrl, medal);

    if (!was_computer) {
        UiContext& ctx = ctrl.Ctx();
        if (ctx.ActiveSelection() && !ctx.ActiveSelection()->stack.empty()) {
            Army* lead = ctx.ActiveSelection()->stack[0];
            City* captured_city = game::CityAt(ctx.ActiveGame(), lead->X(), lead->Y());
            if (captured_city && captured_city->OwnerIndex() == ctx.Player()->Index()) {
                city = captured_city;
            }
        }
        auto then = [&ctrl, city, victor]() {
            if (city) ctrl.PresentVictory(city, victor);
        };
        if (medal && ctx.ActiveGame().Alive(medal)) {
            honours::Medal(ctx, medal, then);
        } else {
            then();
        }
    }
}

bool AssaultAnimationState::HandleKeyPressed(MapScreenController& ctrl, const std::string& key) {
    (void)key;
    if (!ctrl.Ctx().Modals().Empty() || ctrl.OpenMenu() >= 0) return false;
    Press(ctrl);
    return true;
}

bool AssaultAnimationState::HandleMousePressed(MapScreenController& ctrl, int x, int y,
                                               int button) {
    (void)x;
    (void)y;
    (void)button;
    if (!ctrl.Ctx().Modals().Empty()) return false;
    Press(ctrl);
    return true;
}

void AssaultAnimationState::DrawMapOverlay(MapScreenController& ctrl) {
    if (phase_ != Phase::kCloud) return;
    UiContext& ctx = ctrl.Ctx();
    int screen_x =
            tile_x_ * MapScreenController::kTile + (MapScreenController::kTile - kCloudWidth) / 2;
    int screen_y =
            tile_y_ * MapScreenController::kTile + (MapScreenController::kTile - kCloudHeight) / 2;
    ctx.Renderer().SetColor(1, 1, 1);
    ctx.Renderer().Draw(ctx.Assets().WarPic(), gfx::NewQuad(0, 0, kCloudWidth, kCloudHeight),
                        screen_x, screen_y);
}

void AssaultAnimationState::DrawBattleLine(MapScreenController& ctrl, const ArmyList& armies,
                                           const SlotList& slots, int side, int down,
                                           const FellList& fell) const {
    if (side < 0 || side > 8) side = 8;
    UiContext& ctx = ctrl.Ctx();
    gfx::Renderer& ren = ctx.Renderer();
    double now = Now();
    for (size_t i = 0; i < armies.size() && i < slots.size(); i++) {
        bool gone =
                static_cast<int>(i) < down && i < fell.size() && now >= fell[i].at + fell[i].burn;
        if (gone) continue;
        auto pos = slots[i];
        ren.SetColor(1, 1, 1);
        if (armies[i]->IsAtSea()) {
            ren.Draw(ctx.Assets().WarPic(),
                     gfx::NewQuad(kSeaQuad[0], kSeaQuad[1], kSeaQuad[2], kSeaQuad[3]), pos.first,
                     pos.second + kSeaDrop);
        }
        ren.Draw(ctx.Assets().ArmyImg(side), kit::ArmyQuad(armies[i]->TypeIndex()), pos.first,
                 pos.second);
        if (static_cast<int>(i) < down) {
            ren.Draw(ctx.Assets().AtransShields(), gfx::NewQuad(32, 0, 32, 29), pos.first,
                     pos.second);
        }
    }
}

void AssaultAnimationState::DrawBattleWindow(MapScreenController& ctrl) const {
    UiContext& ctx = ctrl.Ctx();
    gfx::Renderer& ren = ctx.Renderer();
    ren.SetColor(0, 0, 0);
    ren.Rectangle(gfx::kLine, kAssaultWindow.x - 0.5, kAssaultWindow.y - 0.5, kAssaultWindow.w + 1,
                  kAssaultWindow.h + 1);
    ren.Rectangle(gfx::kFill, kAssaultWindow.x + 1, kAssaultWindow.y + kAssaultWindow.h + 1,
                  kAssaultWindow.w + 2, 2);
    ren.Rectangle(gfx::kFill, kAssaultWindow.x + kAssaultWindow.w + 1, kAssaultWindow.y + 1, 2,
                  kAssaultWindow.h + 2);
    ren.SetColor(1, 1, 1);
    ren.Draw(ctx.Assets().Marble(), gfx::NewQuad(0, 0, kAssaultWindow.w, kAssaultWindow.h),
             kAssaultWindow.x, kAssaultWindow.y);

    auto draw_shield = [&ctx, &ren](int side, const std::array<int, 2>& pos) {
        ren.SetColor(1, 1, 1);
        ren.Draw(ctx.Assets().ShieldImg(),
                 gfx::NewQuad(side * kShieldWidth, 0, kShieldWidth, kShieldHeight), pos[0], pos[1]);
    };
    draw_shield(def_side_, kShieldDefPos);
    draw_shield(atk_side_, kShieldAtkPos);
    DrawBattleLine(ctrl, defenders_, def_slots_, def_side_, def_down_, def_fell_);
    DrawBattleLine(ctrl, attackers_, atk_slots_, atk_side_, atk_down_, atk_fell_);
    if (phase_ == Phase::kOver) {
        int y = kTextStartY;
        ren.SetColor(1, 1, 1);
        const Font& big_font = ctx.Assets().BigFont();
        for (const auto& line : message_) {
            kit::Centred(big_font, line, 320, y);
            y += kTextStepY;
        }
    }
}

void AssaultAnimationState::DrawDialogOverlay(MapScreenController& ctrl) {
    Advance(ctrl);
    if (&ctrl.State() != this) return;
    if (phase_ != Phase::kCloud) DrawBattleWindow(ctrl);
}

}  // namespace w2
