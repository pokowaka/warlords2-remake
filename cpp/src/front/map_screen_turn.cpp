#include <array>
#include <ctime>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "front/map_screen_controller.hpp"
#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/bytes.hpp"
#include "warlords/cues.hpp"
#include "warlords/game.hpp"
#include "warlords/randommap.hpp"

namespace w2 {

bool MapScreenController::HotseatHidden() const {
    const UiContext& ctx = Ctx();
    if (ctx.ActiveGame().GetMap().GameOptions().hidden_map == 0) return false;
    int humans = 0;
    for (const auto& side : ctx.ActiveGame().GetMap().Sides()) {
        if (side.IsHuman()) ++humans;
    }
    return humans > 1;
}

bool MapScreenController::HumansLeft() const {
    const UiContext& ctx = Ctx();
    return ctx.GamePtr() && game::HasActiveHuman(ctx.ActiveGame());
}

void MapScreenController::ShowBanner(Side* side) {
    turn_banner::Open(Ctx(), *side, [this]() { DismissBanner(); });
}

void MapScreenController::DismissBanner() {
    UiContext& ctx = Ctx();
    if (map_after_banner_) {
        auto cap = CapOf(ctx.Player());
        ctx.CentreOn(cap.first, cap.second);
    }
    map_after_banner_ = false;
    DrownStranded(kNone, kNone, [this]() {
        UiContext& inner_ctx = Ctx();
        int says = inner_ctx.Audio().SpeechOn()
                           ? cues::AdvisorCue(inner_ctx.ActiveGame(), *inner_ctx.Player(),
                                              [this](int sides) { return Roll(sides); })
                           : kNone;
        advisor::Say(inner_ctx, says, [this]() {
            UiContext& promo_ctx = Ctx();
            auto& news = promo_ctx.ActiveGame().TurnNews(promo_ctx.Player()->Id());
            auto promoted = news.promoted;
            news.promoted.clear();
            honours::Promotions(promo_ctx, promoted, [this]() {
                UiContext& offer_ctx = Ctx();
                const auto& offer =
                        offer_ctx.ActiveGame().TurnNews(offer_ctx.Player()->Id()).hero_offer;
                offer_ctx.Audio().Music(offer ? cues::kHero : cues::kPlay);
                std::vector<std::string> moments;
                if (offer_ctx.ActiveGame().Turn() == 2) moments.push_back("turn2");
                if (offer) moments.push_back("hero");
                tutorial::Chain(offer_ctx, moments, [this]() {
                    UiContext& present_ctx = Ctx();
                    if (!hero_offer::Present(present_ctx, *present_ctx.Player(),
                                             [this]() { AfterOffer(); })) {
                        AfterOffer();
                    }
                });
            });
        });
    });
}

void MapScreenController::AfterOffer() {
    UiContext& ctx = Ctx();
    if (!ctx.Player() || ctx.Player()->IsComputerControlled()) return;
    game::HumanProduction(ctx.ActiveGame(), *ctx.Player());
    ctx.GetViewport().StratDirty();
    auto cap = CapOf(ctx.Player());
    City* city = game::CityAt(ctx.ActiveGame(), cap.first, cap.second);
    if (ctx.ActiveGame().Turn() == 1 && city) {
        city::Open(ctx, city, city::kProduction);
        return;
    }
    if (ctx.ModernUi() && !ctx.Player()->Missives().empty()) {
        diplomacyui::PollMissives(ctx);
    } else {
        ctx.Player()->ClearMissives();
    }
}

void MapScreenController::PlayComputers(Side* first_side) {
    auto comp_state = std::make_unique<ComputerTurnState>(*this, first_side);
    auto* raw_comp = comp_state.get();
    SetState(std::move(comp_state));
    raw_comp->Resume(*this);
}

void MapScreenController::EndTurn() {
    UiContext& ctx = Ctx();
    if (ctx.ActiveGame().IsWon()) return;
    ctx.ClearSelection();
    route_.reset();
    Side* next_side = game::EndTurn(ctx.ActiveGame());
    if (next_side && next_side->IsComputerControlled()) {
        PlayComputers(next_side);
        return;
    }
    FinishRound(next_side);
}

void MapScreenController::AutoSave() {
    UiContext& ctx = Ctx();
    if (!ctx.GamePtr() || ctx.ActiveGame().IsWon()) return;
    savegame::WriteSlot("auto", ctx.ActiveGame());
}

void MapScreenController::FinishRound(Side* next_side) {
    UiContext& ctx = Ctx();
    if (!next_side) {
        SetState(std::make_unique<GameOverState>());
        ending::Over(ctx, ctx.ActiveGame().MutableEndingState());
        return;
    }
    AutoSave();
    SetState(std::make_unique<HumanTurnState>());
    ctx.SetPlayer(next_side);
    ctx.GetViewport().StratDirty();
    CycleReset();
    map_after_banner_ = HotseatHidden();
    if (!map_after_banner_) {
        auto cap = CapOf(next_side);
        ctx.CentreOn(cap.first, cap.second);
    }
    ending::Show(ctx, ctx.ActiveGame().MutableEndingState(),
                 [this, next_side]() { ShowBanner(next_side); });
}

void MapScreenController::TakeLoaded(std::unique_ptr<Game> loaded) {
    UiContext& ctx = Ctx();
    AdoptGame(std::move(loaded));
    if (ctx.OptMilitaryAdvisor() >= 0) {
        ctx.ActiveGame().GetMap().MutableOptions().military_advisor = ctx.OptMilitaryAdvisor();
    } else if (ctx.ActiveGame().GetMap().GameOptions().military_advisor == 0) {
        ctx.ActiveGame().GetMap().MutableOptions().military_advisor = 1;
    }
    ctx.ClearSelection();
    route_.reset();
    move_all_.reset();
    ctx.SetStarting(false);
    map_after_banner_ = false;
    ctx.GetViewport().StratDirty();
    ctx.SetPlayer(ctx.ActiveGame().CurrentSide());
    auto cap = CapOf(ctx.Player());
    ctx.CentreOn(cap.first, cap.second);
    ctx.Audio().Music(cues::kPlay);
}

void MapScreenController::Quit() {
    UiContext& ctx = Ctx();
    SetState(std::make_unique<HumanTurnState>());
    ctx.Modals().Clear();
    move_all_.reset();
    route_.reset();
    OpenStart();
}

void MapScreenController::BeginGame() {
    UiContext& ctx = Ctx();
    Side* first = game::Begin(ctx.ActiveGame());
    if (!first) return;
    Side* player = first->IsComputerControlled() ? game::NextHuman(ctx.ActiveGame()) : first;
    if (!player) player = first;
    ctx.SetPlayer(player);
    cycle_cursor_ = CapOf(player);
    ctx.ClearSelection();
    auto cap = CapOf(player);
    ctx.CentreOn(cap.first, cap.second);
    if (!first->IsComputerControlled()) {
        AutoSave();
        map_after_banner_ = HotseatHidden();
        ShowBanner(player);
    }
}

void MapScreenController::OpenStart() {
    UiContext& ctx = Ctx();
    ctx.SetStarting(true);
    ctx.GetViewport().InvalidateLayout();
    ctx.Audio().Music(cues::kTitle);
    start::Open(
            ctx,
            [this](const std::string& dir, const game::NewGameOptions& opts) {
                std::optional<ScenarioFiles> mem_files;
                if (opts.memory_files) mem_files = *opts.memory_files;
                auto start_game = [this, dir, opts, mem_files = std::move(mem_files)]() {
                    UiContext& inner_ctx = Ctx();
                    inner_ctx.SetStarting(false);
                    inner_ctx.GetViewport().InvalidateLayout();
                    inner_ctx.SetScenario(dir);
                    game::NewGameOptions game_opts = opts;
                    if (mem_files) game_opts.memory_files = &*mem_files;
                    game_opts.seed = static_cast<double>(std::time(nullptr) % 1000000007);
                    seed_ = game_opts.seed;
                    if (inner_ctx.OptMilitaryAdvisor() >= 0) {
                        for (auto& [opt_name, opt_val] : game_opts.options) {
                            if (opt_name == "militaryAdvisor") {
                                opt_val = inner_ctx.OptMilitaryAdvisor();
                            }
                        }
                    }
                    auto created_game = game::NewGame(inner_ctx.Assets().DataDir(), dir, game_opts);
                    if (!created_game.ok()) {
                        Say(std::string(created_game.status().message()));
                        OpenStart();
                        return;
                    }
                    AdoptGame(std::move(*created_game));
                    inner_ctx.ClearSelection();
                    route_.reset();
                    inner_ctx.GetViewport().StratDirty();
                    SyncLayout();
                    BeginGame();
                    if (inner_ctx.ActiveGame().CurrentSide() &&
                        inner_ctx.ActiveGame().CurrentSide()->IsComputerControlled()) {
                        bool any_human = false;
                        for (Side* side : inner_ctx.ActiveGame().Sides()) {
                            if (!side->IsComputerControlled()) any_human = true;
                        }
                        if (any_human) {
                            PlayComputers(inner_ctx.ActiveGame().CurrentSide());
                        } else {
                            search::ShowMessage(inner_ctx, kit::Text(inner_ctx, 0xe, 0),
                                                kit::Text(inner_ctx, 0xe, 1), [this]() {
                                                    PlayComputers(Ctx().ActiveGame().CurrentSide());
                                                });
                        }
                    }
                };
                if (!HasScript() && Ctx().Audio().SpeechOn()) {
                    Ctx().Audio().Music(cues::kBegin);
                    advisor::Say(Ctx(), cues::kBeginWar, start_game);
                } else {
                    start_game();
                }
            },
            [this](std::unique_ptr<Game> loaded_game) {
                Ctx().SetStarting(false);
                Ctx().GetViewport().InvalidateLayout();
                SyncLayout();
                TakeLoaded(std::move(loaded_game));
            });
}

absl::Status MapScreenController::StartCliScenario(const std::string& scenario, double seed,
                                                   bool force_diplomacy, bool force_hidden_map,
                                                   bool spectate_all_ai) {
    UiContext& ctx = Ctx();
    ctx.SetStarting(false);
    ctx.SetScenario(scenario);
    game::NewGameOptions game_opts;
    game_opts.seed = seed != 0.0 ? seed : static_cast<double>(std::time(nullptr) % 1000000007);
    constexpr std::array<const char*, 10> kOptionKeys = {
        "neutralCities",  "diplomacy",     "quests",     "hiddenMap",       "viewEnemies",
        "viewProduction", "intenseCombat", "quickStart", "militaryAdvisor", "randomTurns"};
    std::array<int, 10> default_options = {0, 0, 0, 0, 1, 0, 0, 0, 1, 0};
    if (auto opt_bytes = ReadBytes(ctx.Assets().DataDir() / "DATA/OPTIONS.DAT"); opt_bytes.ok()) {
        for (size_t i = 0; i <= 9 && 2 * i + 2 <= opt_bytes->size(); i++) {
            default_options[i] = U16(*opt_bytes, 2 * i);
        }
    }
    for (size_t i = 0; i < 10; i++) {
        game_opts.options.emplace_back(kOptionKeys[i], default_options[i]);
    }
    if (force_diplomacy) {
        for (auto& [opt_name, opt_val] : game_opts.options) {
            if (opt_name == "diplomacy") opt_val = 1;
        }
    }
    if (force_hidden_map) {
        for (auto& [opt_name, opt_val] : game_opts.options) {
            if (opt_name == "hiddenMap") opt_val = 1;
            if (opt_name == "viewEnemies") opt_val = 0;
        }
    }
    if (ctx.OptMilitaryAdvisor() >= 0) {
        for (auto& [opt_name, opt_val] : game_opts.options) {
            if (opt_name == "militaryAdvisor") opt_val = ctx.OptMilitaryAdvisor();
        }
    }
    randommap::Files generated_files;
    if (scenario == randommap::kDir) {
        Rng rng(game_opts.seed);
        randommap::Options rand_opts;
        rand_opts.data_dir = ctx.Assets().DataDir();
        rand_opts.rng = &rng;
        if (ctx.StartWorld().modern_mapgen) {
            rand_opts.scenario = true;
            rand_opts.world.shape =
                    static_cast<scengen::Shape>(std::clamp(ctx.StartWorld().shape, 0, 2));
        }
        auto generated = randommap::GenerateNow(rand_opts);
        if (!generated.ok()) return generated.status();
        generated_files = std::move(*generated);
        game_opts.memory_files = &generated_files;
    }
    auto created_game = game::NewGame(ctx.Assets().DataDir(), scenario, game_opts);
    if (!created_game.ok()) return created_game.status();
    AdoptGame(std::move(*created_game));
    auto sides = ctx.ActiveGame().Sides();
    for (size_t i = 0; i < sides.size(); i++) {
        sides[i]->SetComputerControlled(spectate_all_ai || (i > 0));
    }
    auto start_war = [this]() {
        SyncLayout();
        BeginGame();
        if (Ctx().ActiveGame().CurrentSide() &&
            Ctx().ActiveGame().CurrentSide()->IsComputerControlled()) {
            PlayComputers(Ctx().ActiveGame().CurrentSide());
        }
    };
    if (!HasScript() && !spectate_all_ai && ctx.Audio().SpeechOn()) {
        ctx.Audio().Music(cues::kBegin);
        advisor::Say(ctx, cues::kBeginWar, start_war);
    } else {
        start_war();
    }
    return absl::OkStatus();
}

}  // namespace w2
