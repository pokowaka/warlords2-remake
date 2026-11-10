#include <string>
#include <utility>

#include "front/map_screen_controller.hpp"
#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/game.hpp"
#include "warlords/uidata.hpp"

namespace w2 {

bool MapScreenController::Held(std::initializer_list<const char*> key_names) const {
    return Ctx().Keyboard().IsDown(key_names);
}

bool MapScreenController::BusyWalking() const {
    return (state_->IsHumanBusyWalking() || move_all_.has_value()) && Ctx().Modals().Empty();
}

void MapScreenController::BufferKey(const std::string& key) {
    if (key_buffer_.size() < 15) key_buffer_.push_back(key);
}

void MapScreenController::FlushKeys() {
    auto buffered = std::move(key_buffer_);
    key_buffer_.clear();
    for (const auto& key : buffered) Keypressed(key);
}

void MapScreenController::RefreshControls() {
    UiContext& ctx = Ctx();
    auto& states = ctx.Assets().MainScreen().state;
    if (ctx.Modals().AnyGreysMainControls()) {
        for (auto& [control_id, state] : states) state = uidata::kDisabled;
        return;
    }
    auto set_control = [&](int control_id, bool live) {
        if (!states.count(control_id)) return;
        if (!(live && control_actions_.contains(control_id))) {
            states[control_id] = uidata::kDisabled;
        } else if (control_id == pressed_control_) {
            states[control_id] = uidata::kActive;
        } else {
            states[control_id] = uidata::kNormal;
        }
    };
    const Selection* sel = ctx.ActiveSelection();
    bool walk_on =
            sel && route_ &&
            static_cast<int>(route_->path.size()) >
                    (state_->IsWalking() ? static_cast<int>(state_->WalkingStepIndex()) + 1 : 0);
    set_control(173, walk_on);
    bool more = false;
    for (Army* army : ctx.ActiveGame().Armies()) {
        if (army->OwnerIndex() == ctx.Player()->Index() && !army->InTransit() &&
            !army->IsFortified() && !army->IsDone()) {
            more = true;
            break;
        }
    }
    set_control(174, more);
    set_control(175, more && sel != nullptr);
    set_control(176, sel != nullptr);
    set_control(177, sel != nullptr);
    set_control(178, sel != nullptr);
    set_control(186, sel != nullptr);
    set_control(187, sel && !sel->stack.empty() && sel->stack[0]->HasTarget());
    set_control(188, true);
    for (int i = 0; i < kSlotCount; i++) {
        bool live = sel && i < sel->slots.n;
        set_control(kSlotFirst + i, live);
        set_control(kBarFirst + i, live);
    }
    bool grouped = sel && slots::Grouped(sel->slots);
    set_control(240, sel && !grouped);
    set_control(241, sel && grouped);
    for (int i = 0; i < uidata::kShortcutCount; i++) {
        std::string key = ShortcutKey(i);
        set_control(uidata::kShortcutFirst + i, !key.empty() && MenuEnabled(key));
    }
    for (int i = 0; i < 8; i++) set_control(320 + i, true);
    int face = diplomacyui::ButtonFor(ctx.ActiveGame(), ctx.Player()->Index());
    for (int control_id = 183; control_id <= 185; control_id++) {
        set_control(control_id, control_id == face);
    }
}

void MapScreenController::Mousepressed(int x, int y, int button) {
    if (BusyWalking()) return;
    SyncLayout();
    UiContext& ctx = Ctx();
    const Viewport& viewport = ctx.GetViewport();
    const auto& layout = viewport.GetLayout();
    int frame_x = x - layout.dialog.x;
    int frame_y = y - layout.dialog.y;

    if (state_->HandleMousePressed(*this, x, y, button)) return;

    auto top = ctx.Modals().TopShared();
    if (top && top->UsesMenuBar() &&
        (open_menu_ >= 0 || menu::TitleAt(viewport.MenuLayout(), x, y) >= 0)) {
        top = nullptr;
    }
    if (top) {
        if (button == 2 && top->DialogView()) {
            const auto* ctrl = infobox::ControlAt(*top->DialogView(), frame_x, frame_y,
                                                  &top->HiddenControls());
            if (!(ctrl && infobox::ExplainControl(ctx, x, y, ctrl->id, top.get()))) {
                top->Rightpressed(frame_x, frame_y, x, y);
            }
            return;
        }
        top->Mousepressed(frame_x, frame_y, button);
        return;
    }
    if (state_->IsGameOver()) return;

    int hit = menu::TitleAt(viewport.MenuLayout(), x, y);
    if (hit >= 0) {
        open_menu_ = open_menu_ == hit ? -1 : hit;
        return;
    }
    if (open_menu_ >= 0) {
        const menu::Row* row = menu::RowAt(viewport.MenuLayout(), open_menu_, x, y);
        open_menu_ = -1;
        std::string row_key = row ? (row->key.empty() ? row->act : row->key) : "";
        if (!row_key.empty() && MenuEnabled(row_key)) ExecuteMenuAction(row_key);
        return;
    }
    if (!ctx.GamePtr()) return;

    screen::Screen& main_screen = ctx.Assets().MainScreen();
    if (const auto* ctrl = screen::ControlAt(main_screen, x, y)) {
        if (button == 2) {
            if (infobox::ExplainControl(ctx, x, y, ctrl->id)) return;
        } else {
            if (main_screen.StateOf(ctrl->id) == uidata::kDisabled) return;
            pressed_control_ = ctrl->id;
            main_screen.state[ctrl->id] = uidata::kActive;
            return;
        }
    }

    if (button == 1 && PointerKind(x, y) == static_cast<int>(PointerCursor::kHand)) {
        EnterDrag();
        return;
    }

    const auto* reg = screen::RegionAt(main_screen, x, y);
    if (!reg) return;
    if (reg->id == screen::kMap) {
        int ui_scale = ctx.Display().GetState().scale;
        auto tile = viewport.TileAtPoint(x, y, ui_scale);
        if (!tile) return;
        auto [tile_x, tile_y] = *tile;
        if (button == 2) {
            tileinfo::Open(ctx, tile_x, tile_y);
            return;
        }
        auto ptr_kind = static_cast<PointerCursor>(PointerKind(x, y));
        if (ptr_kind == PointerCursor::kWalk || ptr_kind == PointerCursor::kBoat) {
            MoveSelection(tile_x, tile_y);
        } else if (ptr_kind == PointerCursor::kAttack || ptr_kind == PointerCursor::kPeace) {
            AttackTile(tile_x, tile_y);
        } else if (ptr_kind == PointerCursor::kCity || ptr_kind == PointerCursor::kSite) {
            if (City* city = game::CityAt(ctx.ActiveGame(), tile_x, tile_y)) OpenCity(city);
        } else if (ptr_kind == PointerCursor::kAdvise) {
            miladvisor::Open(ctx, ctx.ActiveSelection() ? &ctx.ActiveSelection()->stack : nullptr,
                             tile_x, tile_y);
        } else if (ptr_kind == PointerCursor::kSelect) {
            SelectClick(tile_x, tile_y);
        } else if (ptr_kind == PointerCursor::kAlt) {
            PlanRoute(tile_x, tile_y);
            RefreshControls();
        }
    } else if (reg->id == screen::kStrategic) {
        int tile_x = (x - reg->x) / 2;
        int tile_y = (y - reg->y + viewport.StratScroll()) / 2;
        if (button != 1) return;
        if (Held({"lalt", "ralt"})) {
            PlanRoute(tile_x, tile_y);
            RefreshControls();
        } else {
            ctx.CentreOn(tile_x, tile_y);
        }
    } else if (reg->id == screen::kBottombar && button == 2) {
        BarInfo(x, y);
    } else if (reg->id == screen::kMappanel && button == 2) {
        infobox::ShowLines(ctx, x, y, "- Drag Screen -", "Move mouse to drag the screen");
    }
}

void MapScreenController::Mousemoved(double dx, double dy) {
    state_->HandleMouseMoved(*this, dx, dy);
}

void MapScreenController::Wheelmoved(int wheel_y) {
    SyncLayout();
    UiContext& ctx = Ctx();
    if (ctx.Starting() || !ctx.GamePtr() || state_->BlocksWheelAndLeftStick() ||
        !ctx.Modals().Empty() || wheel_y == 0) {
        return;
    }
    const Viewport& viewport = ctx.GetViewport();
    int next_zoom = viewport.Zoom() + (wheel_y > 0 ? 1 : -1);
    if (viewport.MapRect().Contains(mouse_x_, mouse_y_)) {
        SetZoomAt(next_zoom, mouse_x_, mouse_y_, false);
    } else {
        SetZoomAt(next_zoom, 0.0, 0.0, true);
    }
}

void MapScreenController::Mousereleased(int x, int y, int button) {
    SyncLayout();
    if (state_->HandleMouseReleased(*this, x, y, button)) return;
    UiContext& ctx = Ctx();
    const auto& layout = ctx.GetViewport().GetLayout();
    if (auto top = ctx.Modals().TopShared()) {
        if (top->Mousereleased(x - layout.dialog.x, y - layout.dialog.y, button)) return;
    }
    int control_id = pressed_control_;
    if (control_id == kNone) return;
    pressed_control_ = kNone;
    screen::Screen& main_screen = ctx.Assets().MainScreen();
    main_screen.state[control_id] = uidata::kNormal;
    const auto* ctrl = screen::ControlAt(main_screen, x, y);
    if (!ctrl || ctrl->id != control_id) return;
    auto act_it = control_actions_.find(control_id);
    if (act_it != control_actions_.end()) act_it->second();
    if (ctx.GamePtr()) RefreshControls();
}

void MapScreenController::Keypressed(const std::string& key) {
    if (BusyWalking()) {
        BufferKey(key);
        return;
    }
    SyncLayout();
    if (state_->HandleKeyPressed(*this, key)) return;

    UiContext& ctx = Ctx();
    bool alt = Held({"lalt", "ralt"});
    bool ctrl = Held({"lctrl", "rctrl", "lgui", "rgui"});
    bool shift = Held({"lshift", "rshift"});
    if (auto top = ctx.Modals().TopShared()) {
        if (top->UsesMenuBar()) {
            std::string accel;
            if (ctrl && key == "q") {
                accel = "^Q";
            } else if (alt && key.size() == 1) {
                accel = "alt " + Upper(key);
            }
            if (!accel.empty()) {
                open_menu_ = -1;
                if (MenuEnabled(accel)) ExecuteMenuAction(accel);
                return;
            }
        }
        if (open_menu_ >= 0) {
            open_menu_ = -1;
            return;
        }
        top->Keypressed(key);
        return;
    }
    if (open_menu_ >= 0) {
        open_menu_ = -1;
        return;
    }
    if (state_->IsGameOver() || !ctx.GamePtr()) return;

    auto press_control = [this](int control_id) {
        if (Ctx().Assets().MainScreen().StateOf(control_id) != uidata::kDisabled) {
            auto action_it = control_actions_.find(control_id);
            if (action_it != control_actions_.end()) action_it->second();
        }
        RefreshControls();
    };

    std::string digit;
    if (key.size() == 1 && key[0] >= '0' && key[0] <= '9') {
        digit = key;
    } else if (key.size() == 3 && key.starts_with("kp") && key[2] >= '0' && key[2] <= '9') {
        digit = key.substr(2);
    }

    Viewport& viewport = ctx.GetViewport();
    if (key == "return" || key == "kpenter") {
        press_control(174);
    } else if (key == "kp+" || key == "pageup" || key == "=") {
        SetZoom(viewport.Zoom() + 1);
    } else if (key == "kp-" || key == "pagedown" || key == "-") {
        SetZoom(viewport.Zoom() - 1);
    } else if (key == "escape") {
        press_control(175);
    } else if (key == "space") {
        press_control(240);
    } else if (key == "tab") {
        press_control(186);
    } else if (key == "backspace") {
        press_control(187);
    } else if (key == "home") {
        press_control(177);
    } else if (key == "end") {
        press_control(173);
    } else if (key == "delete") {
        Deselect();
    } else if (digit == "5") {
        if (Selection* sel = ctx.ActiveSelection()) ctx.CentreOn(sel->x, sel->y);
    } else if (!digit.empty()) {
        static const absl::flat_hash_map<char, CoordPair> kStep = {
            {'8', {0, -1}}, {'9', {1, -1}}, {'6', {1, 0}},  {'3', {1, 1}},
            {'2', {0, 1}},  {'1', {-1, 1}}, {'4', {-1, 0}}, {'7', {-1, -1}},
        };
        auto step_it = kStep.find(digit[0]);
        if (step_it != kStep.end() && ctx.ActiveSelection()) {
            Selection& sel = *ctx.ActiveSelection();
            for (Army* army : sel.stack) army->ClearTarget();
            MoveSelection(sel.x + step_it->second.first, sel.y + step_it->second.second);
        }
    } else if (key == "up") {
        press_control(320);
    } else if (key == "right") {
        press_control(322);
    } else if (key == "down") {
        press_control(324);
    } else if (key == "left") {
        press_control(326);
    } else if (key == "f5") {
        if (savegame::WriteSlot("quick", ctx.ActiveGame())) {
            Say("Saved.");
        } else {
            Say("Could not save.");
        }
    } else if (key == "f8") {
        if (auto loaded = savegame::ReadSlot(ctx, "auto")) {
            TakeLoaded(std::move(loaded));
            Say("Auto-save loaded.");
        } else {
            Say("No auto-save found.");
        }
    } else if (key == "f9") {
        if (auto loaded = savegame::ReadSlot(ctx, "quick")) TakeLoaded(std::move(loaded));
    } else if (ctx.ModernUi() && key == "f10") {
        diplomacyui::OpenBrainInspector(ctx);
    } else {
        std::string accel;
        if (ctrl && key == "q") {
            accel = "^Q";
        } else if (alt && key.size() == 1) {
            accel = "alt " + Upper(key);
        } else if (shift && key == "/") {
            accel = "?";
        } else if (key.size() == 1) {
            accel = key;
        }
        if (!accel.empty() && MenuEnabled(accel)) ExecuteMenuAction(accel);
    }
}

void MapScreenController::Textinput(const std::string& text) {
    if (auto top = Ctx().Modals().TopShared()) {
        top->Textinput(text);
    }
}

}  // namespace w2
