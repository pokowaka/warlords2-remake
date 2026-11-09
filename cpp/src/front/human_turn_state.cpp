#include <utility>

#include "front/map_screen_controller.hpp"
#include "front/map_screen_state.hpp"

namespace w2 {

MapDragState::MapDragState(double start_cx, double start_cy, StatePtr resume_state)
        : start_cx_(start_cx)
        , start_cy_(start_cy)
        , resume_state_(resume_state ? std::move(resume_state)
                                     : std::make_unique<HumanTurnState>()) {}

bool MapDragState::IsComputerTurn() const {
    return resume_state_ && resume_state_->IsComputerTurn();
}

Side* MapDragState::ActiveComputerSide() const {
    return resume_state_ ? resume_state_->ActiveComputerSide() : nullptr;
}

const AiStatus* MapDragState::ActiveAiStatus() const {
    return resume_state_ ? resume_state_->ActiveAiStatus() : nullptr;
}

void MapDragState::SetAiStatusText(const std::string& text) {
    if (resume_state_) resume_state_->SetAiStatusText(text);
}

void MapDragState::StepFrame(MapScreenController& ctrl) {
    (void)ctrl;
}

void MapDragState::HandleMouseMoved(MapScreenController& ctrl, double dx, double dy) {
    drag_dx_ += dx;
    drag_dy_ += dy;
    UiContext& ctx = ctrl.Ctx();
    Viewport& viewport = ctx.GetViewport();
    int ui_scale = ctx.Display().GetState().scale;
    double tile_scale =
            static_cast<double>(ui_scale) / (MapScreenController::kTile * viewport.Zoom());
    viewport.SetCameraPos(start_cx_ - drag_dx_ * tile_scale, start_cy_ - drag_dy_ * tile_scale,
                          ctx.GamePtr(), ui_scale);
}

bool MapDragState::HandleMouseReleased(MapScreenController& ctrl, int x, int y, int button) {
    (void)x;
    (void)y;
    (void)button;
    StatePtr restored = std::move(resume_state_);
    ctrl.SetState(std::move(restored));
    return ctrl.PressedControl() == kNone;
}

}  // namespace w2
