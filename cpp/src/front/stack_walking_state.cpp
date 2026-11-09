#include <utility>

#include "front/map_screen_controller.hpp"
#include "front/map_screen_state.hpp"
#include "util/util.hpp"

namespace w2 {

namespace {

constexpr double kWalkStepSeconds = 0.1;

}  // namespace

StackWalkingState::StackWalkingState(WalkSpec spec, StatePtr resume_state)
        : armies_(spec.armies.begin(), spec.armies.end())
        , tiles_(std::move(spec.tiles))
        , step_index_(0)
        , step_at_(Now())
        , top_(spec.armies.empty() ? nullptr : spec.armies[0])
        , computer_(spec.computer)
        , sea_before_(spec.sea_before)
        , put_down_(std::move(spec.put_down))
        , resume_state_(resume_state ? std::move(resume_state)
                                     : std::make_unique<HumanTurnState>()) {}

bool StackWalkingState::IsComputerTurn() const {
    return resume_state_ && resume_state_->IsComputerTurn();
}

Side* StackWalkingState::ActiveComputerSide() const {
    return resume_state_ ? resume_state_->ActiveComputerSide() : nullptr;
}

const AiStatus* StackWalkingState::ActiveAiStatus() const {
    return resume_state_ ? resume_state_->ActiveAiStatus() : nullptr;
}

void StackWalkingState::SetAiStatusText(const std::string& text) {
    if (resume_state_) resume_state_->SetAiStatusText(text);
}

const IMapScreenState::CoordPair* StackWalkingState::WalkingTile() const {
    return step_index_ < tiles_.size() ? &tiles_[step_index_] : nullptr;
}

bool StackWalkingState::IsArmyWalking(const Army* army) const {
    return armies_.count(const_cast<Army*>(army)) > 0;
}

void StackWalkingState::UpdateMapAnimation(MapScreenController& ctrl) {
    double now = Now();
    while (step_index_ + 1 < tiles_.size() && now - step_at_ >= kWalkStepSeconds) {
        ++step_index_;
        step_at_ += kWalkStepSeconds;
        ctrl.Ctx().CentreOn(tiles_[step_index_].first, tiles_[step_index_].second);
    }
    if (step_index_ + 1 >= tiles_.size() && now - step_at_ >= kWalkStepSeconds) {
        bool was_computer = computer_;
        ArmyList down = std::move(put_down_);
        StatePtr restored = std::move(resume_state_);
        ctrl.SetState(std::move(restored));

        if (!down.empty()) ctrl.PutDown(down);
        ctrl.RefreshRoute();
        if (was_computer) ctrl.State().OnSubstateFinished(ctrl);
        if (ctrl.HasMoveAll()) ctrl.MoveAllStep();
        if (!ctrl.State().IsWalking() && !ctrl.HasMoveAll()) ctrl.FlushKeys();
    }
}

void StackWalkingState::CancelComputerWalk(MapScreenController& ctrl) {
    StatePtr restored = std::move(resume_state_);
    ctrl.SetState(std::move(restored));
    ctrl.RefreshRoute();
    ctrl.State().OnSubstateFinished(ctrl);
}

bool StackWalkingState::HandleKeyPressed(MapScreenController& ctrl, const std::string& key) {
    if (!computer_) return false;
    auto* comp = dynamic_cast<ComputerTurnState*>(resume_state_.get());
    if (!comp) return false;
    return comp->HandleComputerKey(ctrl, key, this);
}

bool StackWalkingState::HandleMousePressed(MapScreenController& ctrl, int x, int y, int button) {
    if (!computer_) return false;
    if (!ctrl.Ctx().Modals().Empty()) return false;
    if (!ctrl.HumansLeft()) {
        return resume_state_->HandleMousePressed(ctrl, x, y, button);
    }
    auto* comp = dynamic_cast<ComputerTurnState*>(resume_state_.get());
    if (!comp) return false;
    return comp->HandleComputerKey(ctrl, "", this);
}

}  // namespace w2
