/**
 * @file
 * @brief Coroutine-driven AI turn execution, hooks, and round transitions.
 */
#include <algorithm>
#include <cmath>

#include "front/internal.hpp"
#include "platform/sound.hpp"
#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/ai.hpp"
#include "warlords/cues.hpp"
#include "warlords/game.hpp"
#include "warlords/history.hpp"
#include "warlords/scn.hpp"

namespace front {

using w2::Army;
using w2::City;
using w2::Format;
using w2::Side;
using w2::uidata::kActive;
using w2::uidata::kDisabled;
using w2::uidata::kNormal;
namespace move = w2::move;
namespace game = w2::game;
namespace slots_mod = w2::slots;

// ------------------------------------------------------------ the computer

// The computer's turns run on a coroutine (the Lua remake's), so the screen
// keeps being drawn while they are played: it gives a frame back after every
// side's turn, and stops after each walk worth showing. A turn is shown when
// its side is observed or no human is left; a walk only if the player has
// seen any of it. Holding Shift or Alt between turns opens Settings; any
// other key or a click runs the rest of the round through unwatched.

Coroutine* ai_co = nullptr;        ///< The turn being played, from inside it.
static Side* ai_result = nullptr;  ///< Where the round ended.
static void FinishRound(Side* side);

/** @brief Whether any human is still in the game. */
bool HumansLeft() {
    return g_app.g && game::HasActiveHuman(*g_app.g);
}

/** @brief Whether the side's turn is shown: it is observed, or no human is left. */
static bool ShownSide(const Side* side) {
    return side && (side->IsObserved() || !HumansLeft());
}

/**
 * @brief A line in the status bar during the computer's turn, and the time
 *        the original then waits.
 * @param ticks How long to wait, in BIOS ticks; none when running through.
 */
static void Status(const std::string& text, int ticks) {
    if (g_app.ai_status) g_app.ai_status->text = text;
    if (ticks > 0 && !g_app.ai_skip && ai_co) {
        Yield pause;
        pause.what = "pause";
        pause.ticks = ticks;
        ai_co->YieldValue(pause);
    }
}

/** @brief The computer's progress bar after each phase of its turn, in percent. */
static int ProgressOf(const std::string& phase) {
    static const std::map<std::string, int> kProgress = {
        {"diplomacy", 10},  {"move hero", 15}, {"move search", 20}, {"move explore", 25},
        {"assault", 30},    {"move #1", 40},   {"rescue", 45},      {"evaluate", 50},
        {"clean city", 55}, {"neutral", 60},   {"missions", 60},    {"move #2", 65},
        {"assault XX", 70}, {"specials", 75},  {"rebuilding", 80},  {"last rescue", 85},
        {"production", 90}, {"economy", 92},   {"vectoring", 95}};
    auto found = kProgress.find(phase);
    return found == kProgress.end() ? (g_app.ai_status ? g_app.ai_status->progress : 0)
                                    : found->second;
}

/**
 * @brief Hooks the computer's walks and battles, so the screen can show them
 *        as the turn plays.
 */
void InstallHooks() {
    w2::ai::hooks.on_walk = [](w2::Game& game_state, const std::vector<Army*>& stack,
                               const move::WalkResult& walk_result) {
        if (!ai_co || g_app.ai_skip || !ShownSide(g_app.ai_side)) return;
        bool seen = !HumansLeft();
        for (auto& tile : walk_result.walked) {
            if (seen) break;
            if (game::Seen(game_state, g_app.player->Index(), tile.first, tile.second)) seen = true;
        }
        if (seen) {
            Yield walk_step;
            walk_step.what = "walk";
            walk_step.armies = stack;
            walk_step.tiles = walk_result.walked;
            walk_step.sea_before = walk_result.sea_before;
            ai_co->YieldValue(walk_step);
        }
    };
    // A computer's battle: who is attacked goes up in the status bar, and
    // then how it went, five ticks each -- when the attacker's turn is
    // watched or the defender is a human.
    w2::ai::hooks.on_fight = [](w2::Game& game_state, const std::vector<Army*>&, int x, int y,
                                w2::Battle& result) {
        if (!ai_co || !g_app.ai_status) return;
        int defender = kNone;
        if (result.lines.city)
            defender = result.lines.city->OwnerIndex();
        else if (!result.lines.defenders.empty())
            defender = result.lines.defenders[0]->OwnerIndex();
        Side* def_side =
                defender != kNone && defender < 8 ? &game_state.map->sides[defender] : nullptr;
        bool human = def_side && !def_side->IsComputerControlled();
        if (!(ShownSide(g_app.ai_side) || human)) return;
        std::vector<Side*> humans;
        for (Side* side : game_state.sides)
            if (side->IsAlive() && !side->IsComputerControlled()) humans.push_back(side);
        if (game_state.map->options.hidden_map != 0 && humans.size() == 1 &&
            !game::Seen(game_state, humans[0]->Index(), x, y))
            return;
        auto text_of = [](int grp) { return kit::Text(grp, 0); };
        Army* attacking_hero = nullptr;
        for (Army* army : result.lines.attackers)
            if (army->Hero()) {
                attacking_hero = army;
                break;
            }
        std::string attacker_name = g_app.ai_side->Name();
        std::string outcome;
        if (!result.won)
            outcome = Format(text_of(0x98), attacker_name);
        else if (attacking_hero)
            outcome = Format(text_of(0x8e), attacking_hero->Name());
        else
            outcome = Format(text_of(0x96), attacker_name);
        bool hidden_many = game_state.map->options.hidden_map != 0 && humans.size() > 1;
        bool window = !hidden_many && ((ShownSide(g_app.ai_side) && def_side) || human);
        bool cloud = !hidden_many;
        // the original slides the view box to the battle and centres the map
        // on it, Observe or not -- a human attacked gets to see where
        if (!hidden_many) {
            CentreOn(x, y);
            StratDirty();
        }
        Status(def_side ? Format(text_of(0x94), def_side->Name()) : text_of(0x95), 5);
        if (cloud && !g_app.ai_skip) {
            StartAssault(x, y, result);
            g_app.assault->computer = true;
            g_app.assault->window = window;
            g_app.assault->outcome = outcome;
            g_app.assault->message = {outcome};
            Yield assault_step;
            assault_step.what = "assault";
            ai_co->YieldValue(assault_step);
            if (window) return;
        }
        Status(outcome, 5);
    };
    w2::ai::hooks.on_spoils = nullptr;
}

/**
 * @brief The coroutine's body: plays computer sides from `side` on, until a
 *        human's turn or the end, yielding to the screen as it goes.
 *
 * Leaves the side whose turn comes next, or nullptr at the end, in
 * ai_result.
 */
static void ComputerTurns(Coroutine& coroutine, Side* side) {
    ai_co = &coroutine;
    auto yield_what = [&coroutine](const char* what) {
        Yield step;
        step.what = what;
        coroutine.YieldValue(step);
    };
    // What the round's end found, as the original tells it inside the
    // computer's turns: each side put out in a box, or -- with no human
    // playing -- in the status bar for 100 ticks, then the last human's fall
    // in two boxes. The rest waits for the turn that follows (ui/ending.cpp).
    auto announce = [&coroutine, &yield_what]() {
        w2::Ending& round_end = g_app.g->ending;
        if (round_end.told) return;
        round_end.told = true;
        auto fallen = round_end.fallen;
        bool no_humans = round_end.no_humans;
        for (auto& fall : fallen) {
            std::string text = ending::FallenText(fall);
            if (fall.boxed) {
                Yield step;
                step.what = "fallen";
                step.text = text;
                coroutine.YieldValue(step);
            } else {
                Status(text, 100);
            }
        }
        if (no_humans) yield_what("nohumans");
    };
    announce();  // a human's turn may have ended the round
    while (side && side->IsComputerControlled()) {
        g_app.ai_side = side;
        // each computer turn opens with a song that plays once
        sound::Music(w2::cues::kComputer, g_app.g->sides);
        g_app.ai_status = AiStatus{side, side->Name(), 5};
        yield_what("progress");
        auto& side_news = g_app.g->TurnNews(side->Id());
        if (ShownSide(side)) {
            auto news = side_news.diplo_news;
            for (auto& news_line : news) Status(news_line, 20);
        }
        side_news.diplo_news.clear();
        w2::ai::PlayTurn(*g_app.g, *side, [&](const char* phase) {
            g_app.ai_status->progress = ProgressOf(phase);
            yield_what("progress");
        });
        g_app.ai_status->progress = 100;
        yield_what("progress");
        side = game::EndTurn(*g_app.g);
        announce();
        yield_what("turn");
    }
    ai_result = side;
}

/**
 * @brief Runs the computer's coroutine on to its next yield, and acts on
 *        what it yielded: a pause, a walk, a message, or the round's end.
 */
void ResumeComputer() {
    if (!g_app.ai_run) return;
    g_app.ai_wait = false;
    bool more = g_app.ai_run->Resume();
    if (!more) {
        g_app.ai_run.reset();
        ai_co = nullptr;
        g_app.ai_skip = false;
        g_app.ai_side = nullptr;
        FinishRound(ai_result);
        return;
    }
    StratDirty();
    const Yield& step = g_app.ai_run->Value();
    if (step.what == "pause") {
        g_app.ai_resume_at = w2::Now() + step.ticks / 18.2;
        g_app.ai_wait = true;
    } else if (step.what == "walk") {
        StartWalk(step.armies, step.tiles, step.sea_before);
        if (g_app.walk)
            g_app.walk->computer = true;
        else
            g_app.ai_wait = true;
    } else if (step.what == "nohumans") {
        // the last human is gone, and the war goes on
        auto text_of = [](int line) { return kit::Text(0xd, line); };
        search::ShowMessage(text_of(0), text_of(1), [text_of]() {
            search::ShowMessage(text_of(2), text_of(3), []() { g_app.ai_wait = true; });
        });
    } else if (step.what == "fallen") {
        search::Say(step.text, []() { g_app.ai_wait = true; });
    } else {
        // a turn done, or a battle up: go on at the next frame free of it
        g_app.ai_wait = true;
    }
}

/** @brief Called every frame: carries the computer on once nothing is in its way. */
void StepComputer() {
    if (!g_app.ai_run || !g_app.ai_wait || g_app.walk || g_app.assault || kit::Top() ||
        g_app.open_menu >= 0)
        return;
    if (g_app.ai_resume_at > 0) {
        if (w2::Now() < g_app.ai_resume_at && !g_app.ai_skip) return;
        g_app.ai_resume_at = 0;
    }
    // only a Shift or Alt pressed since the round began: the Alt of Alt-E
    // is still down as the computer starts
    bool mods = Held({"lshift", "rshift"}) || Held({"lalt", "ralt"});
    if (!mods) g_app.ai_mods_held = false;
    if (mods && !g_app.ai_mods_held) {
        g_app.ai_mods_held = true;
        g_app.ai_skip = false;
        settings::Open();
        return;
    }
    // several steps a frame while running through unwatched
    double until = w2::Now() + 0.03;
    do {
        ResumeComputer();
    } while (g_app.ai_skip && g_app.ai_run && g_app.ai_wait && !g_app.walk && !g_app.assault &&
             !kit::Top() && g_app.ai_resume_at == 0 && w2::Now() < until);
}

/** @brief Plays computer sides from `side` on, until a human's turn or the end. */
void PlayComputers(Side* side) {
    g_app.ai_mods_held = Held({"lshift", "rshift"}) || Held({"lalt", "ralt"});
    ai_result = nullptr;
    g_app.ai_run = std::make_unique<Coroutine>(
            [side](Coroutine& coroutine) { ComputerTurns(coroutine, side); });
    ResumeComputer();
}
/** @brief Plays the computer sides from the side whose turn it is. */
void PlayComputer() {
    PlayComputers(g_app.g->side);
}

/** @brief End Turn: hands over to the next side, computer or human. */
void EndTurn() {
    // End Turn does nothing once the game is won: the winner stays on the
    // turn, to look the kingdom over
    if (g_app.g->won) return;
    // It puts the stack down, and its route with it: the next player sees no
    // other side's plans
    g_app.selection.reset();
    g_app.route.reset();
    Side* side = game::EndTurn(*g_app.g);
    if (side && side->IsComputerControlled()) return PlayComputers(side);
    FinishRound(side);
}

/**
 * @brief The round is over: the next human side takes the keyboard, or with
 *        none the game is over.
 */
static void FinishRound(Side* side) {
    if (!side) {
        g_app.ai_status.reset();
        g_app.over = true;
        ending::Over(g_app.g->ending);
        return;
    }
    g_app.player = side;
    g_app.ai_status.reset();  // the bar is the player's again
    StratDirty();
    CycleReset();
    // the original centres on the side's capital -- but where the map is
    // hidden and more than one human plays, only once the banner is gone,
    // and until then neither map is shown
    g_app.map_after_banner = HotseatHidden();
    if (!g_app.map_after_banner) CentreOn(CapOf(side).first, CapOf(side).second);
    ending::Show(g_app.g->ending, [side]() { ShowBanner(side); });
}

/** @brief Opens the city's dialog. */
void OpenCity(City* city) {
    city::Open(city);
}
/** @brief Opens the quest dialog. */
void OpenQuest() {
    questui::Open();
}

/**
 * @brief Takes up a game just loaded: its military advisor setting, a clean
 *        screen, and the view on its current side's capital.
 */
void TakeLoaded(std::unique_ptr<w2::Game> loaded) {
    g_app.g = std::move(loaded);
    if (opt_military_advisor >= 0) {
        g_app.g->map->options.military_advisor = opt_military_advisor;
    } else if (g_app.g->map->options.military_advisor == 0) {
        g_app.g->map->options.military_advisor = 1;
    }
    g_app.selection.reset();
    g_app.route.reset();
    g_app.walk.reset();
    g_app.over = false;
    g_app.starting = false;
    g_app.map_after_banner = false;
    StratDirty();
    g_app.player = g_app.g->sides[g_app.g->current];
    CentreOn(CapOf(g_app.player).first, CapOf(g_app.player).second);
    sound::Music(w2::cues::kPlay);
}

}  // namespace front
