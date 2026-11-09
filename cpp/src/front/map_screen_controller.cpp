#include "front/map_screen_controller.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <utility>

#include "absl/log/absl_check.h"
#include "absl/strings/str_format.h"

#include "front/script_player.hpp"
#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/ai.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/game.hpp"
#include "warlords/rules.hpp"

namespace w2 {

namespace {

constexpr int kSunkStringGroup = 0xa8;
constexpr int kWarStringGroup = 0x8c;
constexpr double kDoubleClickSeconds = 40.0 / 18.2;

}  // namespace

MapScreenController::MapScreenController()
        : state_(std::make_unique<HumanTurnState>())
        , loose_dice_(static_cast<unsigned>(std::time(nullptr))) {}

MapScreenController::~MapScreenController() {
    ShutdownSession();
}

void MapScreenController::BindContext(UiContext& ctx, ScriptPlayer& script_player) {
    ctx_ = &ctx;
    script_player_ = &script_player;
    InitActions();
}

void MapScreenController::ShutdownSession() {
    state_ = std::make_unique<HumanTurnState>();
    move_all_.reset();
    route_.reset();
    key_buffer_.clear();
    if (ctx_ != nullptr) {
        ctx_->Modals().Clear();
        ctx_->ClearSelection();
        ctx_->SetPlayer(nullptr);
        ctx_->SetGame(nullptr);
    }
    owned_game_.reset();
}

UiContext& MapScreenController::Ctx() const {
    ABSL_DCHECK(ctx_ != nullptr);
    return *ctx_;
}

bool MapScreenController::HasScript() const {
    return script_player_ != nullptr && script_player_->IsLoaded();
}

void MapScreenController::SetState(StatePtr next_state) {
    state_ = next_state ? std::move(next_state) : std::make_unique<HumanTurnState>();
}

MapScreenController::StatePtr MapScreenController::TakeState() {
    StatePtr prev = std::move(state_);
    state_ = std::make_unique<HumanTurnState>();
    return prev;
}

void MapScreenController::AdoptGame(std::unique_ptr<Game> game) {
    state_ = std::make_unique<HumanTurnState>();
    owned_game_ = std::move(game);
    Ctx().SetGame(owned_game_.get());
}

void MapScreenController::OpenCity(City* city) {
    city::Open(Ctx(), city);
}

void MapScreenController::OpenQuest() {
    questui::Open(Ctx());
}

void MapScreenController::PresentVictory(City* city, const std::string& victor) {
    city_victory::Open(Ctx(), city, victor);
}

void MapScreenController::SelectAt(int x, int y) {
    Select(x, y);
}

void MapScreenController::AfterSlotChange() {
    SyncSelection();
}

void MapScreenController::PlayComputer() {
    PlayComputers(Ctx().ActiveGame().CurrentSide());
}

void MapScreenController::Say(const std::string& message) {
    status_ = message;
}

int MapScreenController::Roll(int n) {
    return n <= 0 ? 1 : static_cast<int>(loose_dice_() % static_cast<unsigned>(n)) + 1;
}

void MapScreenController::StartAssault(int x, int y, const Battle& result) {
    AssaultSpec spec;
    spec.x = x;
    spec.y = y;
    spec.result = result;
    EnterAssault(std::move(spec));
}

void MapScreenController::EnterWalk(WalkSpec spec) {
    if (spec.tiles.empty()) return;
    CoordPair first_tile = spec.tiles[0];
    StatePtr prev = TakeState();
    SetState(std::make_unique<StackWalkingState>(std::move(spec), std::move(prev)));
    Ctx().CentreOn(first_tile.first, first_tile.second);
}

void MapScreenController::EnterAssault(AssaultSpec spec) {
    StatePtr prev = TakeState();
    SetState(std::make_unique<AssaultAnimationState>(*this, std::move(spec), std::move(prev)));
}

void MapScreenController::EnterDrag() {
    const Viewport& viewport = Ctx().GetViewport();
    StatePtr prev = TakeState();
    SetState(std::make_unique<MapDragState>(viewport.Cx(), viewport.Cy(), std::move(prev)));
}

MapScreenController::ArmyList MapScreenController::SelectableAt(int x, int y) const {
    ArmyList out;
    const UiContext& ctx = Ctx();
    for (Army* army : game::ArmiesAt(ctx.ActiveGame(), x, y)) {
        if (army->OwnerIndex() == ctx.Player()->Index() &&
            static_cast<int>(out.size()) < rules::kMaxStack) {
            out.push_back(army);
        }
    }
    return out;
}

void MapScreenController::SyncSelection() {
    UiContext& ctx = Ctx();
    Selection* sel = ctx.ActiveSelection();
    if (!sel) return;
    slots::Commit(sel->slots, ctx.ActiveGame());
    sel->stack = slots::Selected(sel->slots);
    for (Army* army : sel->stack) {
        army->SetFortified(false);
        army->SetOffered(true);
    }
}

void MapScreenController::Reslot(const ArmyList& armies) {
    UiContext& ctx = Ctx();
    Selection* sel = ctx.ActiveSelection();
    if (!sel) return;
    auto kept = armies.empty() ? std::nullopt : slots::Keep(sel->slots, ctx.ActiveGame(), armies);
    if (!kept) {
        ctx.ClearSelection();
        return;
    }
    sel->slots = *kept;
    sel->x = kept->army[0]->X();
    sel->y = kept->army[0]->Y();
    SyncSelection();
}

bool MapScreenController::DrownStranded(int x, int y, kit::Done after) {
    UiContext& ctx = Ctx();
    int sunk = game::SinkStranded(ctx.ActiveGame(), *ctx.Player(), x, y);
    if (sunk == 0) {
        if (after) after();
        return false;
    }
    ctx.Audio().Effect("splash");
    ctx.ClearSelection();
    route_.reset();
    ctx.GetViewport().StratDirty();
    if (x != kNone) ctx.CentreOn(x, y);
    std::string line = sunk == 1 ? kit::Text(ctx, kSunkStringGroup, 2)
                                 : Format(kit::Text(ctx, kSunkStringGroup, 0), sunk);
    search::ShowMessage(ctx, line, kit::Text(ctx, kSunkStringGroup, 1), std::move(after));
    return true;
}

void MapScreenController::Select(int x, int y, Army* pick) {
    UiContext& ctx = Ctx();
    if (DrownStranded(x, y)) return;
    auto stack = SelectableAt(x, y);
    if (stack.empty()) {
        ctx.ClearSelection();
        if (City* city = game::CityAt(ctx.ActiveGame(), x, y)) OpenCity(city);
        return;
    }
    auto chosen = slots::Clicked(ctx.ActiveGame(), stack, ctx.Player()->Index(), pick);
    Selection sel;
    sel.x = x;
    sel.y = y;
    sel.slots = slots::Build(ctx.ActiveGame(), stack, ctx.Player()->Index(), &chosen);
    ctx.SetSelection(std::move(sel));
    SyncSelection();
    RefreshRoute();

    std::vector<std::string> moments = {"move"};
    for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
            City* neighbour = game::CityAt(ctx.ActiveGame(), x + dx, y + dy);
            if (neighbour && neighbour->OwnerIndex() != ctx.Player()->Index() &&
                !neighbour->IsRazed() && moments.size() == 1) {
                moments.push_back("fight");
            }
        }
    }
    Selection* active_sel = ctx.ActiveSelection();
    Army* lead = (active_sel && !active_sel->stack.empty()) ? active_sel->stack[0] : nullptr;
    if (lead && lead->Hero() && ctx.ActiveGame().GetMap().SiteAt(x, y)) {
        moments.push_back("search");
    }
    tutorial::Chain(ctx, moments);
}

MapScreenController::CoordPair MapScreenController::CapOf(const Side* side) const {
    if (side->CurrentCapitalPos()) return side->CurrentCapitalPos()->ToPair();
    const UiContext& ctx = Ctx();
    if (ctx.GamePtr() && ctx.GamePtr()->HasMap()) {
        if (const City* cap = side->CapitalCity(ctx.GamePtr()->GetMap())) {
            return {cap->X(), cap->Y()};
        }
    }
    TilePos orig = side->OriginCapitalPos();
    return {orig.x == kNone ? 0 : orig.x, orig.y == kNone ? 0 : orig.y};
}

void MapScreenController::CycleReset() {
    cycle_cursor_ = CapOf(Ctx().Player());
}

void MapScreenController::SelectNext() {
    UiContext& ctx = Ctx();
    if (!cycle_cursor_) CycleReset();
    auto rank_of = [this](const Army* army) {
        int dist = std::abs(army->X() - cycle_cursor_->first) +
                   std::abs(army->Y() - cycle_cursor_->second);
        return dist == 0 ? 9000 : dist;
    };

    Army* fresh = nullptr;
    Army* used = nullptr;
    for (Army* army : ctx.ActiveGame().Armies()) {
        if (army->OwnerIndex() == ctx.Player()->Index() && !army->InTransit() &&
            !army->IsFortified() && !army->IsDone()) {
            if (army->IsOffered()) {
                if (!used || rank_of(army) < rank_of(used)) used = army;
            } else if (!fresh || rank_of(army) < rank_of(fresh)) {
                fresh = army;
            }
        }
    }
    if (!fresh) {
        if (!used) {
            ctx.ClearSelection();
            route_.reset();
            Say("Every army has moved.");
            ctx.Audio().Effect("chord");
            return;
        }
        for (Army* army : ctx.ActiveGame().Armies()) {
            if (army->OwnerIndex() == ctx.Player()->Index()) army->SetOffered(false);
        }
        fresh = used;
    }
    cycle_cursor_ = std::make_pair(fresh->X(), fresh->Y());
    ctx.Audio().Effect("ding");
    Select(fresh->X(), fresh->Y(), fresh);
    ctx.CentreOn(fresh->X(), fresh->Y());
}

void MapScreenController::QuitArmy() {
    Selection* sel = Ctx().ActiveSelection();
    if (!sel) {
        SelectNext();
        return;
    }
    for (Army* army : sel->stack) army->SetDone(true);
    route_.reset();
    SelectNext();
}

void MapScreenController::Fortify() {
    Selection* sel = Ctx().ActiveSelection();
    if (!sel) {
        SelectNext();
        return;
    }
    for (Army* army : sel->stack) {
        army->SetFortified(true);
        army->ClearTarget();
    }
    route_.reset();
    SelectNext();
}

void MapScreenController::SelectClick(int tile_x, int tile_y) {
    UiContext& ctx = Ctx();
    double now = Now();
    double last = last_select_click_;
    last_select_click_ = now;
    Selection* sel = ctx.ActiveSelection();
    if (sel && sel->x == tile_x && sel->y == tile_y && last >= 0.0 &&
        now - last < kDoubleClickSeconds) {
        slots::All(sel->slots, ctx.ActiveGame());
        AfterSlotChange();
        RefreshControls();
        return;
    }
    Select(tile_x, tile_y);
}

void MapScreenController::Deselect() {
    Ctx().ClearSelection();
    route_.reset();
    if (state_->IsWalking() && !state_->IsComputerTurn()) {
        SetState(std::make_unique<HumanTurnState>());
    }
}

void MapScreenController::RefreshRoute() {
    route_.reset();
    Selection* sel = Ctx().ActiveSelection();
    if (!sel || sel->stack.empty()) return;
    auto target = sel->stack[0]->Target();
    if (!target) return;
    if (sel->stack[0]->X() == target->x && sel->stack[0]->Y() == target->y) {
        for (Army* army : sel->stack) army->ClearTarget();
        return;
    }
    route_ = move::PreviewRoute(Ctx().ActiveGame(), sel->stack, target->x, target->y);
}

void MapScreenController::OrderTo(const ArmyList& stack, int x, int y) {
    for (Army* army : stack) {
        if (x == kNone) {
            army->ClearTarget();
        } else {
            army->SetTarget(TilePos(x, y));
        }
    }
}

void MapScreenController::PlanRoute(int x, int y) {
    UiContext& ctx = Ctx();
    Selection* sel = ctx.ActiveSelection();
    if (!sel || sel->stack.empty()) return;
    if (!game::Seen(ctx.ActiveGame(), ctx.Player()->Index(), x, y)) return;
    if (sel->stack[0]->X() == x && sel->stack[0]->Y() == y) {
        OrderTo(sel->stack, kNone, kNone);
        route_.reset();
        return;
    }
    OrderTo(sel->stack, x, y);
    RefreshRoute();
    if (!route_) ctx.Audio().Effect("chord");
}

void MapScreenController::PutDown(const ArmyList& stack) {
    Army* lead = stack.empty() ? nullptr : stack[0];
    bool arrived = lead && lead->HasTarget() && lead->X() == lead->Target()->x &&
                   lead->Y() == lead->Target()->y;
    for (Army* army : stack) {
        army->SetDone(true);
        if (arrived) army->ClearTarget();
    }
    Ctx().ClearSelection();
    route_.reset();
}

void MapScreenController::AfterBattle(const Battle& result) {
    UiContext& ctx = Ctx();
    tutorial::Show(ctx, "fresult");
    if (Selection* sel = ctx.ActiveSelection()) {
        ArmyList alive;
        for (int i = 0; i < sel->slots.n; i++) {
            Army* army = sel->slots.army[i];
            if (!result.dead_by_army.count(army)) alive.push_back(army);
        }
        Reslot(alive);
    }
}

void MapScreenController::AttackTile(int x, int y) {
    UiContext& ctx = Ctx();
    Selection* sel = ctx.ActiveSelection();
    if (!sel || sel->stack.empty()) return;
    Game& game_state = ctx.ActiveGame();
    int player_index = ctx.Player()->Index();
    auto here = game::ArmiesAt(game_state, x, y);
    City* city = game::CityAt(game_state, x, y);
    if (city && city->IsRazed()) city = nullptr;
    int owner = !here.empty() ? here[0]->OwnerIndex() : (city ? city->OwnerIndex() : kNone);
    if (diplomacy::MustDeclare(game_state, player_index, owner, city)) {
        input::Options options;
        options.title = kit::Text(ctx, kWarStringGroup, 0);
        for (int i = 1; i <= 4; i++) options.lines.push_back(kit::Text(ctx, kWarStringGroup, i));
        options.confirm = true;
        options.ok = [this, x, y, player_index, owner](const std::string&) {
            diplomacy::AttackAnyway(Ctx().ActiveGame(), player_index, owner);
            AttackTile(x, y);
        };
        input::Open(ctx, options);
        return;
    }
    if (move::StackMoves(sel->stack) < 1) {
        Deselect();
        return;
    }
    if (owner == player_index || (!city && here.empty())) return;
    OrderTo(sel->stack, kNone, kNone);
    route_.reset();
    game::ChargeAttack(game_state, sel->stack, x, y);
    Battle result = game::DecideAttack(game_state, sel->stack, x, y);
    AssaultSpec spec;
    spec.x = x;
    spec.y = y;
    spec.result = result;
    spec.after = [this, result]() { AfterBattle(result); };
    EnterAssault(std::move(spec));
}

void MapScreenController::MoveSelection(int x, int y) {
    UiContext& ctx = Ctx();
    Selection* sel = ctx.ActiveSelection();
    if (!sel) return;
    OrderTo(sel->stack, x, y);
    route_ = move::PreviewRoute(ctx.ActiveGame(), sel->stack, x, y);
    auto walk_result = move::MoveTo(ctx.ActiveGame(), sel->stack, x, y);
    if (walk_result.stopped == move::WalkStop::kAttack) {
        walk_result.stopped = move::WalkStop::kBlocked;
    }
    bool started_walk = false;
    WalkSpec walk_spec;
    if (walk_result.stopped == move::WalkStop::kNoRoute) {
        Say("There is no way there.");
        ctx.Audio().Effect("chord");
    } else if (walk_result.steps == 0) {
        Say(absl::StrFormat("They cannot move: %s.", move::WalkStopName(walk_result.stopped)));
    } else {
        ArmyList walked = sel->stack;
        sel->x = sel->stack[0]->X();
        sel->y = sel->stack[0]->Y();
        Reslot(SelectableAt(sel->x, sel->y));
        ctx.GetViewport().StratDirty();
        walk_spec.armies = std::move(walked);
        walk_spec.tiles = std::move(walk_result.walked);
        walk_spec.sea_before = walk_result.sea_before;
        started_walk = !walk_spec.tiles.empty();
    }
    if (walk_result.out_of_moves && ctx.ActiveSelection()) {
        if (started_walk) {
            walk_spec.put_down = ctx.ActiveSelection()->stack;
        } else {
            PutDown(ctx.ActiveSelection()->stack);
        }
    }
    if (started_walk) {
        EnterWalk(std::move(walk_spec));
    }
    if (Selection* updated_sel = ctx.ActiveSelection();
        updated_sel && !updated_sel->stack.empty()) {
        updated_sel->x = updated_sel->stack[0]->X();
        updated_sel->y = updated_sel->stack[0]->Y();
        if (!state_->IsWalking()) ctx.CentreOn(updated_sel->x, updated_sel->y);
    }
    if (!state_->IsWalking()) RefreshRoute();
}

void MapScreenController::MoveAllStep() {
    UiContext& ctx = Ctx();
    while (move_all_) {
        if (state_->IsWalking()) return;
        Army* lead = nullptr;
        for (Army* army : ctx.ActiveGame().Armies()) {
            if (army->OwnerIndex() == ctx.Player()->Index() && army->HasTarget() &&
                !army->InTransit() && !move_all_->seen.count(army)) {
                lead = army;
                break;
            }
        }
        if (!lead) {
            if (move_all_->moved == 0) Say("Nothing is under orders.");
            move_all_.reset();
            RefreshControls();
            return;
        }
        move_all_->seen.insert(lead);
        Select(lead->X(), lead->Y(), lead);
        if (Selection* sel = ctx.ActiveSelection()) {
            for (Army* army : sel->stack) move_all_->seen.insert(army);
            if (lead->HasTarget()) {
                TilePos target = *lead->Target();
                MoveSelection(target.x, target.y);
                if (move_all_) ++move_all_->moved;
            }
        }
    }
}

void MapScreenController::MoveAll() {
    move_all_ = MoveAllProgress{};
    if (Selection* sel = Ctx().ActiveSelection();
        sel && !sel->stack.empty() && sel->stack[0]->HasTarget()) {
        TilePos target = *sel->stack[0]->Target();
        for (Army* army : sel->stack) move_all_->seen.insert(army);
        MoveSelection(target.x, target.y);
        if (move_all_) move_all_->moved = 1;
    }
    MoveAllStep();
}

void MapScreenController::DoSearch() {
    UiContext& ctx = Ctx();
    Selection* sel = ctx.ActiveSelection();
    if (!sel) return;
    int x = sel->x;
    int y = sel->y;
    search::Open(ctx, sel->stack);
    Reslot(SelectableAt(x, y));
    ctx.GetViewport().StratDirty();
}

}  // namespace w2
