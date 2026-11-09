#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"

#include "front/map_screen_controller.hpp"
#include "front/map_screen_state.hpp"
#include "front/menu.hpp"
#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/ai.hpp"
#include "warlords/cues.hpp"
#include "warlords/game.hpp"

namespace w2 {

namespace {

int PhasePercent(std::string_view phase, int fallback) {
    static const absl::flat_hash_map<std::string_view, int> kProgress = {
        {"diplomacy", 10},  {"move hero", 15}, {"move search", 20}, {"move explore", 25},
        {"assault", 30},    {"move #1", 40},   {"rescue", 45},      {"evaluate", 50},
        {"clean city", 55}, {"neutral", 60},   {"missions", 60},    {"move #2", 65},
        {"assault XX", 70}, {"specials", 75},  {"rebuilding", 80},  {"last rescue", 85},
        {"production", 90}, {"economy", 92},   {"vectoring", 95},
    };
    auto prog_it = kProgress.find(phase);
    return prog_it == kProgress.end() ? fallback : prog_it->second;
}

bool IsShownSide(const MapScreenController& ctrl, const Side* side) {
    return side != nullptr && (side->IsObserved() || !ctrl.HumansLeft());
}

/**
 * @brief Bridges headless `IAiTurnEvents` callbacks inside `w2::ai::PlayTurn` onto
 * `AiTurnRunner`'s worker `ITurnObserver` channel.
 */
class WorkerTurnEventsBridge final : public IAiTurnEvents {
  public:
    WorkerTurnEventsBridge(MapScreenController& ctrl, ITurnObserver& channel, Side* ai_side,
                           const bool& ai_skip, int& last_progress,
                           std::optional<TilePos>& pending_centre, std::string& latest_text,
                           Battle*& pending_battle, AssaultSpec& pending_assault,
                           bool& has_pending_assault)
            : ctrl_(ctrl)
            , channel_(channel)
            , ai_side_(ai_side)
            , ai_skip_(ai_skip)
            , last_progress_(last_progress)
            , pending_centre_(pending_centre)
            , latest_text_(latest_text)
            , pending_battle_(pending_battle)
            , pending_assault_(pending_assault)
            , has_pending_assault_(has_pending_assault) {}

    bool IsAborted() const override { return channel_.IsAborted(); }

    bool OnPhase(std::string_view phase) override {
        if (channel_.IsAborted()) return false;
        last_progress_ = PhasePercent(phase, last_progress_);
        PhaseProgressEvent progress_ev{ai_side_->Id(), last_progress_, std::string(phase)};
        return channel_.OnTurnEvent(progress_ev) == TurnControl::kContinue;
    }

    bool OnWalk(Game& game, const ArmyStack& stack, const TileCoords& walked,
                bool sea_before) override {
        if (channel_.IsAborted()) return false;
        if (ai_skip_ || !IsShownSide(ctrl_, ai_side_)) return true;
        bool seen = !ctrl_.HumansLeft();
        const Side* viewer_player = ctrl_.Ctx().Player();
        for (const auto& tile : walked) {
            if (seen) break;
            if (viewer_player &&
                game::Seen(game, viewer_player->Index(), tile.first, tile.second)) {
                seen = true;
            }
        }
        if (!seen) return true;

        WalkEvent walk_ev;
        walk_ev.sea_before = sea_before;
        walk_ev.armies.reserve(stack.size());
        for (const Army* army : stack) {
            if (army) walk_ev.armies.push_back(army->Id());
        }
        walk_ev.tiles.reserve(walked.size());
        for (const auto& [tile_x, tile_y] : walked) {
            walk_ev.tiles.emplace_back(tile_x, tile_y);
        }
        return channel_.OnTurnEvent(walk_ev) == TurnControl::kContinue;
    }

    bool OnFight(Game& game, const ArmyStack&, int x, int y, Battle& result) override {
        if (channel_.IsAborted()) return false;
        UiContext& ctx = ctrl_.Ctx();
        int defender = kNone;
        if (result.lines.city) {
            defender = result.lines.city->OwnerIndex();
        } else if (!result.lines.defenders.empty()) {
            defender = result.lines.defenders[0]->OwnerIndex();
        }
        Side* def_side = game.GetMap().SideAt(defender);
        bool human_def = def_side && !def_side->IsComputerControlled();
        if (!(IsShownSide(ctrl_, ai_side_) || human_def)) return true;

        std::vector<Side*> humans;
        for (Side* side : game.Sides()) {
            if (side->IsAlive() && !side->IsComputerControlled()) humans.push_back(side);
        }
        if (game.GetMap().GameOptions().hidden_map != 0 && humans.size() == 1 &&
            !game::Seen(game, humans[0]->Index(), x, y)) {
            return true;
        }

        Army* attacking_hero = nullptr;
        for (Army* army : result.lines.attackers) {
            if (army->Hero()) {
                attacking_hero = army;
                break;
            }
        }
        std::string attacker_name = ai_side_->Name();
        std::string outcome;
        if (!result.won) {
            outcome = Format(kit::Text(ctx, 0x98, 0), attacker_name);
        } else if (attacking_hero) {
            outcome = Format(kit::Text(ctx, 0x8e, 0), attacking_hero->Name());
        } else {
            outcome = Format(kit::Text(ctx, 0x96, 0), attacker_name);
        }

        bool hidden_many = game.GetMap().GameOptions().hidden_map != 0 && humans.size() > 1;
        bool window = !hidden_many && ((IsShownSide(ctrl_, ai_side_) && def_side) || human_def);
        bool cloud = !hidden_many;
        if (!hidden_many) {
            pending_centre_ = TilePos(x, y);
        }

        std::string def_text = def_side ? Format(kit::Text(ctx, 0x94, 0), def_side->Name())
                                        : kit::Text(ctx, 0x95, 0);
        if (!EmitStatus(def_text, 5)) return false;

        if (cloud && !ai_skip_) {
            pending_battle_ = &result;
            pending_assault_ =
                    AssaultSpec{x, y, result, &result, /*computer=*/true, window, outcome, nullptr};
            has_pending_assault_ = true;
            AssaultEvent assault_ev{TilePos(x, y), ai_side_->Id(),
                                    def_side ? def_side->Id() : SideId::None(), window, outcome};
            TurnControl ctl = channel_.OnTurnEvent(assault_ev);
            pending_battle_ = nullptr;
            has_pending_assault_ = false;
            if (ctl == TurnControl::kAbort) return false;
            if (window) return true;
        }
        return EmitStatus(outcome, 5);
    }

    bool EmitStatus(const std::string& text, int ticks) {
        latest_text_ = text;
        if (ticks > 0 && !ai_skip_) {
            return channel_.OnTurnEvent(PauseEvent{ticks, text}) == TurnControl::kContinue;
        }
        return !channel_.IsAborted();
    }

  private:
    MapScreenController& ctrl_;
    ITurnObserver& channel_;
    Side* ai_side_ = nullptr;
    const bool& ai_skip_;
    int& last_progress_;
    std::optional<TilePos>& pending_centre_;
    std::string& latest_text_;
    Battle*& pending_battle_;
    AssaultSpec& pending_assault_;
    bool& has_pending_assault_;
};

/**
 * @brief Main-thread `ITurnObserver` adapter forwarding one `TurnEvent` to `ComputerTurnState`.
 */
class MainThreadEventSink final : public ITurnObserver {
  public:
    using HandlerFn = std::function<TurnControl(const TurnEvent&)>;
    explicit MainThreadEventSink(HandlerFn handler) : handler_(std::move(handler)) {}
    TurnControl OnTurnEvent(const TurnEvent& event) override { return handler_(event); }

  private:
    HandlerFn handler_;
};

}  // namespace

ComputerTurnState::ComputerTurnState(MapScreenController& ctrl, Side* first_side)
        : ai_mods_held_(ctrl.Held({"lshift", "rshift"}) || ctrl.Held({"lalt", "ralt"})) {
    runner_ = std::make_unique<AiTurnRunner>([this, &ctrl, first_side](ITurnObserver& channel) {
        RunWorkerTurns(ctrl, first_side, channel);
    });
}

ComputerTurnState::~ComputerTurnState() {
    if (runner_) runner_->Abort();
}

void ComputerTurnState::SetAiStatusText(const std::string& text) {
    if (ai_status_) ai_status_->text = text;
}

void ComputerTurnState::OnSubstateFinished(MapScreenController& ctrl) {
    Resume(ctrl);
}

void ComputerTurnState::RunWorkerTurns(MapScreenController& ctrl, Side* start_side,
                                       ITurnObserver& channel) {
    UiContext& ctx = ctrl.Ctx();
    Game& game = ctx.ActiveGame();
    std::string latest_text;

    auto emit_status = [&](const std::string& text, int ticks) -> bool {
        latest_text = text;
        if (ai_status_) ai_status_->text = text;
        if (ticks > 0 && !ai_skip_) {
            return channel.OnTurnEvent(PauseEvent{ticks, text}) == TurnControl::kContinue;
        }
        return !channel.IsAborted();
    };

    auto announce = [&]() -> bool {
        Ending& round_end = game.MutableEndingState();
        if (round_end.told) return true;
        round_end.told = true;
        auto fallen = round_end.fallen;
        bool no_humans = round_end.no_humans;
        for (const auto& fall : fallen) {
            std::string text = ending::FallenText(ctx, fall);
            if (fall.boxed) {
                FallenEvent fallen_ev{fall.side ? fall.side->Id() : SideId::None(), text};
                if (channel.OnTurnEvent(fallen_ev) == TurnControl::kAbort) return false;
            } else if (!emit_status(text, 100)) {
                return false;
            }
        }
        if (no_humans) {
            if (channel.OnTurnEvent(NoHumansEvent{}) == TurnControl::kAbort) return false;
        }
        return !channel.IsAborted();
    };

    if (!announce()) return;

    Side* side = start_side;
    while (side && side->IsComputerControlled()) {
        if (channel.IsAborted()) return;
        ai_side_ = side;
        int current_progress = 5;
        latest_text = side->Name();
        if (channel.OnTurnEvent(PhaseProgressEvent{side->Id(), 5, ""}) == TurnControl::kAbort) {
            return;
        }

        auto& side_news = game.TurnNews(side->Id());
        if (IsShownSide(ctrl, side)) {
            auto diplo_lines = side_news.diplo_news;
            for (const auto& news_line : diplo_lines) {
                if (!emit_status(news_line, 20)) return;
            }
        }
        side_news.diplo_news.clear();

        WorkerTurnEventsBridge bridge(ctrl, channel, side, ai_skip_, current_progress,
                                      pending_centre_, latest_text, pending_battle_,
                                      pending_assault_, has_pending_assault_);
        ai::PlayTurn(game, *side, &bridge);
        if (channel.IsAborted()) return;
        if (ai_status_) ai_status_->text = latest_text;

        if (channel.OnTurnEvent(PhaseProgressEvent{side->Id(), 100, "done"}) ==
            TurnControl::kAbort) {
            return;
        }
        SideId finished_id = side->Id();
        side = game::EndTurn(game);
        if (!announce()) return;
        if (channel.OnTurnEvent(TurnDoneEvent{finished_id, side ? side->Id() : SideId::None()}) ==
            TurnControl::kAbort) {
            return;
        }
    }
    next_human_side_ = side;
}

TurnControl ComputerTurnState::HandleMainEvent(MapScreenController& ctrl, const TurnEvent& event) {
    UiContext& ctx = ctrl.Ctx();
    Game& game = ctx.ActiveGame();
    if (pending_centre_) {
        ctx.CentreOn(pending_centre_->x, pending_centre_->y);
        pending_centre_.reset();
    }
    ctx.GetViewport().StratDirty();

    if (const auto* pause = std::get_if<PauseEvent>(&event)) {
        if (ai_status_ && !pause->text.empty()) ai_status_->text = pause->text;
        ai_resume_at_ = Now() + pause->ticks / 18.2;
        ai_wait_ = true;
        return TurnControl::kContinue;
    }

    if (const auto* progress = std::get_if<PhaseProgressEvent>(&event)) {
        Side* side = game.GetMap().SideById(progress->side);
        if (progress->phase.empty() && side) {
            bool has_human = std::any_of(
                    game.Sides().begin(), game.Sides().end(), [](const Side* active_side) {
                        return active_side != nullptr && active_side->IsAlive() &&
                               !active_side->IsComputerControlled();
                    });
            ctx.Audio().Music(cues::kComputer, has_human);
            ai_status_ = AiStatus{side, side->Name(), progress->percent};
        } else if (ai_status_) {
            ai_status_->progress = progress->percent;
        }
        ai_wait_ = true;
        return TurnControl::kContinue;
    }

    if (const auto* walk = std::get_if<WalkEvent>(&event)) {
        WalkSpec spec;
        spec.computer = true;
        spec.sea_before = walk->sea_before;
        for (ArmyId army_id : walk->armies) {
            if (Army* army = game.Armies().FindAlive(army_id)) spec.armies.push_back(army);
        }
        for (const TilePos& pos : walk->tiles) {
            spec.tiles.emplace_back(pos.x, pos.y);
        }
        if (!spec.tiles.empty()) {
            ctrl.EnterWalk(std::move(spec));
        } else {
            ai_wait_ = true;
        }
        return TurnControl::kContinue;
    }

    if (const auto* assault_ev = std::get_if<AssaultEvent>(&event)) {
        ctx.CentreOn(assault_ev->target.x, assault_ev->target.y);
        ctx.GetViewport().StratDirty();
        ai_wait_ = true;
        if (has_pending_assault_) {
            AssaultSpec spec = std::move(pending_assault_);
            ctrl.EnterAssault(std::move(spec));
        }
        return TurnControl::kContinue;
    }

    if (const auto* fallen = std::get_if<FallenEvent>(&event)) {
        search::Say(ctx, fallen->text, [this]() { ai_wait_ = true; });
        return TurnControl::kContinue;
    }

    if (std::holds_alternative<NoHumansEvent>(event)) {
        search::ShowMessage(ctx, kit::Text(ctx, 0xd, 0), kit::Text(ctx, 0xd, 1), [this, &ctx]() {
            search::ShowMessage(ctx, kit::Text(ctx, 0xd, 2), kit::Text(ctx, 0xd, 3),
                                [this]() { ai_wait_ = true; });
        });
        return TurnControl::kContinue;
    }

    // TurnDoneEvent: yield one rendered frame between computer factions.
    ai_wait_ = true;
    return TurnControl::kContinue;
}

bool ComputerTurnState::Resume(MapScreenController& ctrl) {
    if (!runner_) return false;
    ai_wait_ = false;
    MainThreadEventSink sink(
            [this, &ctrl](const TurnEvent& event) { return HandleMainEvent(ctrl, event); });
    bool more = runner_->Step(sink);
    if (!more) {
        if (pending_centre_) {
            ctrl.Ctx().CentreOn(pending_centre_->x, pending_centre_->y);
            pending_centre_.reset();
        }
        Side* next_side = next_human_side_;
        runner_.reset();
        ctrl.SetState(std::make_unique<HumanTurnState>());
        ctrl.FinishRound(next_side);
        return false;
    }
    return &ctrl.State() == this;
}

void ComputerTurnState::StepFrame(MapScreenController& ctrl) {
    UiContext& ctx = ctrl.Ctx();
    if (!runner_ || !ai_wait_ || !ctx.Modals().Empty() || ctrl.OpenMenu() >= 0) return;

    if (ai_resume_at_ > 0.0) {
        if (Now() < ai_resume_at_ && !ai_skip_) return;
        ai_resume_at_ = 0.0;
    }

    bool mods = ctrl.Held({"lshift", "rshift"}) || ctrl.Held({"lalt", "ralt"});
    if (!mods) ai_mods_held_ = false;
    if (mods && !ai_mods_held_) {
        ai_mods_held_ = true;
        ai_skip_ = false;
        settings::Open(ctx);
        return;
    }

    double until = Now() + 0.03;
    while (Resume(ctrl) && ai_skip_ && runner_ && ai_wait_ && ctx.Modals().Empty() &&
           ai_resume_at_ == 0.0 && Now() < until) {
    }
}

bool ComputerTurnState::HandleComputerKey(MapScreenController& ctrl, const std::string& key,
                                          StackWalkingState* active_walk) {
    UiContext& ctx = ctrl.Ctx();
    if (!ctx.Modals().Empty() || ctrl.OpenMenu() >= 0) return false;

    if (ctx.ModernUi() && key == "f10") {
        diplomacyui::OpenBrainInspector(ctx, ai_side_ ? ai_side_->Index() : kNone);
        return true;
    }
    Viewport& viewport = ctx.GetViewport();
    int ui_scale = ctx.Display().GetState().scale;
    if (key == "kp+" || key == "pageup" || key == "=") {
        ctrl.SetZoom(viewport.Zoom() + 1);
        return true;
    }
    if (key == "kp-" || key == "pagedown" || key == "-") {
        ctrl.SetZoom(viewport.Zoom() - 1);
        return true;
    }
    if (key == "up") {
        viewport.PanTiles(0.0, -1.0, ctx.GamePtr(), ui_scale);
        return true;
    }
    if (key == "down") {
        viewport.PanTiles(0.0, 1.0, ctx.GamePtr(), ui_scale);
        return true;
    }
    if (key == "left") {
        viewport.PanTiles(-1.0, 0.0, ctx.GamePtr(), ui_scale);
        return true;
    }
    if (key == "right") {
        viewport.PanTiles(1.0, 0.0, ctx.GamePtr(), ui_scale);
        return true;
    }
    if (key.ends_with("shift") || key.ends_with("alt") || key.ends_with("ctrl") ||
        key.ends_with("gui")) {
        return true;
    }
    if (!ctrl.HumansLeft()) {
        if (key == "space") {
            ai_skip_ = !ai_skip_;
            if (ai_skip_ && active_walk != nullptr) {
                active_walk->CancelComputerWalk(ctrl);
            }
            return true;
        }
        if (key == "escape") {
            settings::Open(ctx);
            return true;
        }
        if (ctrl.Held({"lctrl", "rctrl", "lgui", "rgui"}) && key == "q") return false;
        if (key == "c" || key == "." || key == "a" || key == "k" || key == "g" || key == "n" ||
            key == "w" || key == "d" || key == "h" || key == "e" || key == "j" || key == "y" ||
            key == "l" || key == "?") {
            return false;
        }
        return true;
    }
    ai_skip_ = true;
    if (active_walk != nullptr) {
        active_walk->CancelComputerWalk(ctrl);
    }
    return true;
}

bool ComputerTurnState::HandleKeyPressed(MapScreenController& ctrl, const std::string& key) {
    return HandleComputerKey(ctrl, key, nullptr);
}

bool ComputerTurnState::HandleMousePressed(MapScreenController& ctrl, int x, int y, int button) {
    UiContext& ctx = ctrl.Ctx();
    if (!ctx.Modals().Empty()) return false;
    if (!ctrl.HumansLeft()) {
        const Viewport& viewport = ctx.GetViewport();
        int hit = menu::TitleAt(viewport.MenuLayout(), x, y);
        if (hit >= 0) {
            ctrl.SetOpenMenu(ctrl.OpenMenu() == hit ? -1 : hit);
            return true;
        }
        if (ctrl.OpenMenu() >= 0) {
            const menu::Row* row = menu::RowAt(viewport.MenuLayout(), ctrl.OpenMenu(), x, y);
            ctrl.SetOpenMenu(-1);
            std::string row_key = row ? (row->key.empty() ? row->act : row->key) : "";
            if (!row_key.empty() && ctrl.MenuEnabled(row_key)) ctrl.ExecuteMenuAction(row_key);
            return true;
        }
        if (ctx.GamePtr() && ctx.Assets().MainScreenPtr()) {
            if (const auto* reg = screen::RegionAt(ctx.Assets().MainScreen(), x, y)) {
                if (reg->id == screen::kMap) {
                    if (button == 2) {
                        int ui_scale = ctx.Display().GetState().scale;
                        if (auto tile = viewport.TileAtPoint(x, y, ui_scale)) {
                            tileinfo::Open(ctx, tile->first, tile->second);
                        }
                        return true;
                    }
                    if (button == 1) {
                        ctrl.EnterDrag();
                        return true;
                    }
                } else if (reg->id == screen::kStrategic && button == 1) {
                    int tile_x = (x - reg->x) / 2;
                    int tile_y = (y - reg->y + viewport.StratScroll()) / 2;
                    ctx.CentreOn(tile_x, tile_y);
                    return true;
                }
            }
        }
        return true;
    }
    return HandleComputerKey(ctrl, "", nullptr);
}

}  // namespace w2
