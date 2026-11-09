/**
 * @file
 * @brief Turn banner, hero offers, combat initiation, stack movement, and new-game startup.
 */
#include <algorithm>
#include <ctime>

#include "absl/strings/str_format.h"

#include "front/internal.hpp"
#include "platform/sound.hpp"
#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/armytype.hpp"
#include "warlords/bytes.hpp"
#include "warlords/cues.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/game.hpp"
#include "warlords/randommap.hpp"
#include "warlords/uidata.hpp"

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

// ------------------------------------------------------------------ the turn

/**
 * @brief After the player's battle: the result's tutorial, and the selection
 *        re-slotted over its survivors.
 */
static void AfterBattle(const w2::Battle& result) {
    tutorial::Show("fresult");
    if (g_app.selection) {
        std::vector<Army*> alive;
        for (int i = 0; i < g_app.selection->slots.n; i++) {
            Army* army = g_app.selection->slots.army[i];
            if (!result.dead_by_army.count(army)) alive.push_back(army);
        }
        Reslot(alive);
    }
}

/// "War!" -- asked before attacking a side not at war (STRING.DAT group 140).
static const int kWarGroup = 0x8c;

/**
 * @brief A click with the sword or the dove: fights for the tile beside the
 *        stack.
 *
 * There is no walk, so the step's cost does not matter: one move left will
 * do. A side at peace -- or, for a city, one not at war -- must be declared
 * on first. The battle is decided first, shown, and only then takes effect.
 */
void AttackTile(int x, int y) {
    if (!g_app.selection || g_app.selection->stack.empty()) return;
    auto& sel = *g_app.selection;
    w2::Game& game_state = *g_app.g;
    int player_index = g_app.player->index;
    auto here = game::ArmiesAt(game_state, x, y);
    City* city = game::CityAt(game_state, x, y);
    if (city && city->razed) city = nullptr;
    int owner = !here.empty() ? here[0]->owner : (city ? city->owner_index : kNone);
    if (w2::diplomacy::MustDeclare(game_state, player_index, owner, city)) {
        input::Options options;
        options.title = kit::Text(kWarGroup, 0);
        for (int i = 1; i <= 4; i++) options.lines.push_back(kit::Text(kWarGroup, i));
        options.confirm = true;
        options.ok = [x, y, player_index, owner](const std::string&) {
            w2::diplomacy::AttackAnyway(*g_app.g, player_index, owner);
            AttackTile(x, y);
        };
        input::Open(options);
        return;
    }
    if (move::StackMoves(sel.stack) < 1) {
        Deselect();
        return;
    }
    if (owner == player_index || (!city && here.empty())) return;
    OrderTo(sel.stack, kNone, kNone);
    g_app.route.reset();
    game::ChargeAttack(game_state, sel.stack, x, y);
    // decided first, shown, and only then taken effect
    w2::Battle result = game::DecideAttack(game_state, sel.stack, x, y);
    StartAssault(x, y, result);
    g_app.assault->after = [result]() { AfterBattle(result); };
}

/**
 * @brief Walks the selection towards (x, y).
 *
 * Running into an enemy just stops the walk; attacking is the sword's
 * (AttackTile).
 */
void MoveSelection(int x, int y) {
    if (!g_app.selection) return;
    auto& sel = *g_app.selection;
    OrderTo(sel.stack, x, y);
    g_app.route = move::PreviewRoute(*g_app.g, sel.stack, x, y);
    auto walk_result = move::MoveTo(*g_app.g, sel.stack, x, y);
    if (walk_result.stopped == move::WalkStop::kAttack) {
        walk_result.stopped = move::WalkStop::kBlocked;
    }
    if (walk_result.stopped == move::WalkStop::kNoRoute) {
        Say("There is no way there.");
        sound::Effect("chord");
    } else if (walk_result.steps == 0) {
        Say(absl::StrFormat("They cannot move: %s.", move::WalkStopName(walk_result.stopped)));
    } else {
        auto walked = sel.stack;
        sel.x = sel.stack[0]->x;
        sel.y = sel.stack[0]->y;
        Reslot(SelectableAt(sel.x, sel.y));
        StratDirty();
        StartWalk(walked, walk_result.walked, walk_result.sea_before);
    }
    // a walk that leaves the stack out of moves ends its turn: done for the
    // turn and put down -- once the walk has played out, as the original
    // draws every step first
    if (walk_result.out_of_moves && g_app.selection) {
        if (g_app.walk)
            g_app.walk->put_down = g_app.selection->stack;
        else
            PutDown(g_app.selection->stack);
    }
    if (g_app.selection && !g_app.selection->stack.empty()) {
        g_app.selection->x = g_app.selection->stack[0]->x;
        g_app.selection->y = g_app.selection->stack[0]->y;
        if (!g_app.walk) CentreOn(g_app.selection->x, g_app.selection->y);
    }
    if (!g_app.walk) RefreshRoute();
}

// The start-of-turn banner: popup 6, (160, 60) 320x312 -- exactly CITY.PCK
// -- framed in the side's own colour, with the side's name and "Turn %d"
// over it. The original then blocks until any input arrives.
static const kit::Rect kBanner{160, 60, 320, 312};
static const int kBannerNameY = 85, kBannerTurnY = 130;

/** @brief Raises the start-of-turn banner for the side, with its fanfare. */
void ShowBanner(Side* side) {
    g_app.banner = Banner{side->name, g_app.g->turn, side->colour, side->edge};
    sound::Effect("turn");  // the fanfare, TURN.8SN
}

// The hero offer is popup 2 -- (80, 60) 480x312, MARBLE.PCK cropped -- with
// dialog 12's controls laid on it, every number from the original's own
// hero offer.
static const kit::Rect kHeroPopup{80, 60, 480, 312};
static const int kHeroDialog = 12;
static const int kHeroMapX = 80, kHeroMapY = 60;
static const int kHeroPicX = 320, kHeroPicY = 110, kHeroPicW = 224, kHeroPicH = 170;
static const int kHeroTitleY = 63, kHeroCentre = 432;
static const int kHeroLineY[4] = {190, 210, 230, 250};
static const int kMaleLabel[2] = {376, 315}, kFemaleLabel[2] = {480, 315};
static const int kHeroTitleGroup = 0x5f, kHeroLineGroup = 0x61, kHeroSexGroup = 0x62;
/// What a hired hero's allies are announced with (STRING.DAT group 96).
static const int kAllyGroup = 0x60;

static void AfterBanner();
static bool PresentOffer(Side* side);
static void AfterOffer();
/// The name field clicked: the Steam Deck's on-screen keyboard comes up for it.
bool offer_typing = false;

/**
 * @brief Whether the maps stay blank until the banner is clicked away.
 *
 * Where the map is hidden and more than one human plays, the main map and
 * the strategic map stay blank as a human's turn opens, until the banner is
 * clicked away: each side has its own hidden map, and whoever is still at
 * the screen is not to see the next one's. The original shares one seen bit
 * among its humans and only holds back the re-centring.
 */
bool HotseatHidden() {
    if (g_app.g->map->options.hidden_map == 0) return false;
    int humans = 0;
    for (auto& side : g_app.g->map->sides)
        if (side.in_use && side.alive && !side.computer) humans++;
    return humans > 1;
}

/** @brief Closes the banner, and only then lets the turn's first dialog through. */
void DismissBanner() {
    g_app.banner.reset();
    if (g_app.map_after_banner) CentreOn(CapOf(g_app.player).first, CapOf(g_app.player).second);
    g_app.map_after_banner = false;
    // whoever is left on the water without a boat drowns, and only then does
    // the advisor have his say; the turn's opening then puts on the hero's
    // music or the turn's
    DrownStranded(kNone, kNone, AfterBanner);
}

/**
 * @brief The turn's opening once the banner is gone: the advisor, the
 *        promotions, the music, the tutorial, then the hero offer.
 */
static void AfterBanner() {
    int says =
            sound::SpeechOn() ? w2::cues::AdvisorCue(*g_app.g, *g_app.player, front::Roll) : kNone;
    // then the heroes promoted, each in its own popup, and the hero offer
    // with its music, or the turn's
    advisor::Say(says, []() {
        auto promoted = g_app.player->promoted;
        g_app.player->promoted.clear();
        honours::Promotions(promoted, []() {
            sound::Music(g_app.player->hero_offer ? w2::cues::kHero : w2::cues::kPlay);
            std::vector<std::string> moments;
            if (g_app.g->turn == 2) moments.push_back("turn2");
            if (g_app.player->hero_offer) moments.push_back("hero");
            tutorial::Chain(moments, []() {
                if (!PresentOffer(g_app.player)) AfterOffer();
            });
        });
    });
}

/**
 * @brief The four lines over the hero offer's picture.
 *
 * As the original assembles them: strings 0-3 for the free hero of turn 1,
 * 4-7 for one that asks a price, and the same again from 8 for a heroine.
 */
static std::vector<std::string> HeroLines(const w2::HeroOffer& offer, bool female) {
    int first = female ? 8 : 0;
    auto line = [first](int idx) { return kit::Text(kHeroLineGroup, first + idx); };
    if (offer.first) return {line(0), line(1), line(2), Format(line(3), offer.city->name)};
    return {Format(line(4), offer.city->name), Format(line(5), offer.price),
            Format(line(6), g_app.player->gold), line(7)};
}

/**
 * @brief Opens the side's hero offer, if it has one.
 * @return Whether it had one.
 */
static bool PresentOffer(Side* side) {
    if (!side->hero_offer) return false;
    if (!g_app.hero_view) g_app.hero_view = screen::MakeView(*g_app.screen, kHeroDialog);
    g_app.offer = side->hero_offer;
    // the name and sex were rolled with the offer; the checkboxes only change
    // the wording and the picture
    g_app.offer_female = g_app.offer->female;
    g_app.offer_name = g_app.offer->name.empty() ? "Hero" : g_app.offer->name;
    g_app.hero_view->state[kHeroCancel] = g_app.offer->first ? kDisabled : kNormal;
    g_app.hero_view->state[kHeroOk] = kNormal;
    return true;
}

/** @brief Takes the offer: the hero joins under the name and sex now in the dialog. */
void AcceptOffer() {
    auto offer = g_app.offer;
    offer->name = g_app.offer_name;
    offer->female = g_app.offer_female;
    auto hired = w2::hero::Recruit(*g_app.g, *g_app.player, *offer);
    Army* hero = hired.hero;
    g_app.offer.reset();
    g_app.player->hero_offer.reset();
    CentreOn(hero->x, hero->y);
    if (hired.count > 0) {
        // "And the hero brings 1 ally!" / "... %d allies!": the number rolled,
        // placed or not
        std::string line = hired.count == 1 ? kit::Text(kAllyGroup, 0)
                                            : Format(kit::Text(kAllyGroup, 1), hired.count);
        search::Say(line, []() { AfterOffer(); });
        return;
    }
    AfterOffer();
}

/**
 * @brief The turn's opening, once any hero offer is settled: the cities
 *        build -- or on turn 1 the capital's dialog opens in Production.
 */
static void AfterOffer() {
    if (g_app.player->computer) return;
    game::HumanProduction(*g_app.g, *g_app.player);
    StratDirty();
    auto cap = CapOf(g_app.player);
    City* city = game::CityAt(*g_app.g, cap.first, cap.second);
    if (g_app.g->turn == 1 && city) {
        city::Open(city, city::kProduction);
        return;
    }
    if (g_app.modern_ui && !g_app.player->missives.empty()) {
        diplomacyui::PollMissives();
    } else {
        g_app.player->missives.clear();
    }
}

/** @brief Turns the hero offer down. */
void RefuseOffer() {
    g_app.offer.reset();
    g_app.player->hero_offer.reset();
    AfterOffer();
}

void DrawBanner() {
    const Banner& banner = *g_app.banner;
    const auto& popup_rect = kBanner;
    auto fill = [](int x, int y, int width, int height) {
        gfx::Rectangle(gfx::kFill, x, y, width, height);
    };
    kit::PopupFrame(popup_rect);
    gfx::SetColor(1, 1, 1);
    gfx::Draw(g_app.city_pic, popup_rect.x, popup_rect.y);
    auto outline = [&](int x, int y, int width, int height) {
        fill(popup_rect.x + x, popup_rect.y + y, width, 1);
        fill(popup_rect.x + x, popup_rect.y + y + height - 1, width, 1);
        fill(popup_rect.x + x, popup_rect.y + y, 1, height);
        fill(popup_rect.x + x + width - 1, popup_rect.y + y, 1, height);
    };
    PalColour(banner.edge);
    outline(0, 0, popup_rect.w, popup_rect.h);
    PalColour(banner.colour);
    fill(popup_rect.x + 1, popup_rect.y + 1, popup_rect.w - 2, 9);
    fill(popup_rect.x + 1, popup_rect.y + 1, 9, popup_rect.h - 2);
    fill(popup_rect.x + 1, popup_rect.y + popup_rect.h - 10, popup_rect.w - 2, 9);
    fill(popup_rect.x + popup_rect.w - 10, popup_rect.y + 1, 9, popup_rect.h - 2);
    PalColour(banner.edge);
    outline(10, 10, popup_rect.w - 20, popup_rect.h - 20);
    gfx::SetColor(1, 1, 1);
    kit::Centred(*g_app.title_font, banner.name, 320, kBannerNameY);
    kit::Centred(*g_app.title_font, absl::StrFormat("Turn %d", banner.turn), 320, kBannerTurnY);
}

/** @brief Draws the hero-offer popup: map, portrait, text lines, name field and sex checkboxes. */
void DrawHeroOffer() {
    const auto& popup_rect = kHeroPopup;
    const w2::HeroOffer& offer = *g_app.offer;
    kit::PopupFrame(popup_rect);
    gfx::SetColor(1, 1, 1);
    gfx::SetScissor(popup_rect.x, popup_rect.y, popup_rect.w, popup_rect.h);
    gfx::Draw(g_app.marble, popup_rect.x, popup_rect.y);
    gfx::SetScissor();
    DrawStrategicMap(kHeroMapX, kHeroMapY);
    gfx::SetScissor(popup_rect.x, popup_rect.y, popup_rect.w, popup_rect.h);
    gfx::SetColor(1, 1, 1);
    int fig_x = std::max(0, (offer.city->x * 2 - 2) / 8 * 8);
    int fig_y = std::max(0, offer.city->y * 2 - 6);
    gfx::Draw(g_app.atrans_shields, gfx::NewQuad(96, 0, 16, 15), kHeroMapX + fig_x,
              kHeroMapY + fig_y);
    gfx::Draw(g_app.offer_female ? g_app.hero_pic_f : g_app.hero_pic_m, kHeroPicX, kHeroPicY);
    gfx::SetColor(0, 0, 0);
    gfx::Rectangle(gfx::kLine, kHeroPicX - 0.5, kHeroPicY - 0.5, kHeroPicW + 1, kHeroPicH + 1);
    gfx::SetScissor();
    auto centred = [](const Font& font, const std::string& str, int y) {
        gfx::SetColor(1, 1, 1);
        font.Draw(str, kHeroCentre - font.Width(str) / 2, y);
    };
    centred(*g_app.title_font, kit::Text(kHeroTitleGroup, 0), kHeroTitleY);
    auto lines = HeroLines(offer, g_app.offer_female);
    for (int i = 0; i < 4; i++) {
        // Font 2 in colour 15 with a colour-14 outline.
        if (!lines[i].empty()) centred(g_app.big_font->Colours(15, 14), lines[i], kHeroLineY[i]);
    }
    if (auto field = screen::DialogControl(*g_app.hero_view, kHeroField)) {
        gfx::SetColor(0, 0, 0);
        kit::Outline(field->x - 2, field->y - 2, field->w + 4, field->h + 4);
        kit::Field(field->x, field->y, field->w, field->h, g_app.offer_name, g_app.big_font.get());
    }
    auto label = [](const int* pos, const std::string& str) {
        gfx::SetColor(1, 1, 1);
        g_app.big_font->Draw(str, pos[0] - g_app.big_font->Width(str), pos[1]);
    };
    label(kMaleLabel, kit::Text(kHeroSexGroup, 0));
    label(kFemaleLabel, kit::Text(kHeroSexGroup, 1));
    auto box = [](int control_id, bool checked) {
        auto ctrl = screen::DialogControl(*g_app.hero_view, control_id);
        if (!ctrl) return;
        gfx::SetColor(1, 1, 1);
        gfx::Draw(g_app.abits, gfx::NewQuad(320, checked ? 0 : 20, 24, 20), ctrl->x, ctrl->y);
    };
    box(kHeroMale, !g_app.offer_female);
    box(kHeroFemale, g_app.offer_female);
    screen::DrawDialogControls(*g_app.screen, *g_app.hero_view);
}

// ------------------------------------------------------------------ starting

/** @brief Starts the first turn of a newly created game and centres the view on the capital. */
void BeginGame() {
    Side* first = game::Begin(*g_app.g);
    if (!first) return;
    // a computer that opens the game plays to the screen of the human who
    // follows it, on that human's hidden map
    g_app.player = first->computer ? game::NextHuman(*g_app.g) : first;
    if (!g_app.player) g_app.player = first;
    g_app.cursor = CapOf(g_app.player);
    g_app.selection.reset();
    CentreOn(CapOf(g_app.player).first, CapOf(g_app.player).second);
    // The banner is shown for a human side.
    if (!first->computer) {
        g_app.map_after_banner = HotseatHidden();
        ShowBanner(g_app.player);
    }
}

void OpenStart() {
    g_app.starting = true;
    g_app.have_layout = false;
    sound::Music(w2::cues::kTitle);
    start::Open(
            [](const std::string& dir, const w2::game::NewGameOptions& opts) {
                std::optional<w2::ScenarioFiles> mem_files;
                if (opts.memory_files) mem_files = *opts.memory_files;
                auto start_game = [dir, opts, mem_files = std::move(mem_files)]() {
                    g_app.starting = false;
                    g_app.have_layout = false;
                    g_app.scenario = dir;
                    w2::game::NewGameOptions game_opts = opts;
                    if (mem_files) game_opts.memory_files = &*mem_files;
                    game_opts.seed = static_cast<double>(time(nullptr) % 1000000007);
                    g_app.seed = game_opts.seed;
                    if (opt_military_advisor >= 0) {
                        for (auto& [opt_name, opt_val] : game_opts.options)
                            if (opt_name == "militaryAdvisor") opt_val = opt_military_advisor;
                    }
                    auto created_game = game::NewGame(g_app.data_dir, dir, game_opts);
                    if (!created_game.ok()) {
                        Say(std::string(created_game.status().message()));
                        OpenStart();
                        return;
                    }
                    g_app.g = std::move(*created_game);
                    g_app.selection.reset();
                    g_app.over = false;
                    StratDirty();
                    SyncLayout();
                    BeginGame();
                    if (g_app.g->side && g_app.g->side->computer) {
                        // A game with no human in it says so as it begins.
                        bool human = false;
                        for (Side* side : g_app.g->sides)
                            if (!side->computer) human = true;
                        if (human)
                            PlayComputers(g_app.g->side);
                        else
                            search::ShowMessage(kit::Text(0xe, 0), kit::Text(0xe, 1),
                                                []() { PlayComputers(g_app.g->side); });
                    }
                };
                if (!HasScript() && sound::SpeechOn()) {
                    sound::Music(w2::cues::kBegin);
                    advisor::Say(w2::cues::kBeginWar, start_game);
                } else {
                    start_game();
                }
            },
            [](std::unique_ptr<w2::Game> loaded_game) {
                g_app.starting = false;
                g_app.have_layout = false;
                SyncLayout();
                TakeLoaded(std::move(loaded_game));
            });
}

absl::Status StartCliScenario(const std::string& scenario, double seed, bool force_diplomacy,
                              bool force_hidden_map, bool spectate_all_ai) {
    g_app.starting = false;
    g_app.scenario = scenario;
    game::NewGameOptions game_opts;
    game_opts.seed = seed != 0 ? seed : static_cast<double>(time(nullptr) % 1000000007);
    static const char* const kOptionKeys[10] = {
        "neutralCities",  "diplomacy",     "quests",     "hiddenMap",       "viewEnemies",
        "viewProduction", "intenseCombat", "quickStart", "militaryAdvisor", "randomTurns"};
    int default_options[10] = {0, 0, 0, 0, 1, 0, 0, 0, 1, 0};
    if (auto opt_bytes = w2::ReadBytes(g_app.data_dir / "DATA/OPTIONS.DAT"); opt_bytes.ok()) {
        for (size_t i = 0; i <= 9 && 2 * i + 2 <= opt_bytes->size(); i++) {
            default_options[i] = w2::U16(*opt_bytes, 2 * i);
        }
    }
    for (int i = 0; i < 10; i++) {
        game_opts.options.emplace_back(kOptionKeys[i], default_options[i]);
    }
    if (force_diplomacy) {
        for (auto& [opt_name, opt_val] : game_opts.options)
            if (opt_name == "diplomacy") opt_val = 1;
    }
    if (force_hidden_map) {
        for (auto& [opt_name, opt_val] : game_opts.options) {
            if (opt_name == "hiddenMap") opt_val = 1;
            if (opt_name == "viewEnemies") opt_val = 0;
        }
    }
    if (opt_military_advisor >= 0) {
        for (auto& [opt_name, opt_val] : game_opts.options)
            if (opt_name == "militaryAdvisor") opt_val = opt_military_advisor;
    }
    w2::randommap::Files generated_files;
    if (scenario == w2::randommap::kDir) {
        w2::Rng rng(game_opts.seed);
        w2::randommap::Options rand_opts;
        rand_opts.data_dir = g_app.data_dir;
        rand_opts.rng = &rng;
        auto generated = w2::randommap::GenerateNow(rand_opts);
        if (!generated.ok()) return generated.status();
        generated_files = std::move(*generated);
        game_opts.memory_files = &generated_files;
    }
    auto created_game = game::NewGame(g_app.data_dir, scenario, game_opts);
    if (!created_game.ok()) return created_game.status();
    g_app.g = std::move(*created_game);
    for (size_t i = 0; i < g_app.g->sides.size(); i++)
        g_app.g->sides[i]->computer = spectate_all_ai || (i > 0);
    auto start_war = [=]() {
        SyncLayout();
        BeginGame();
        if (g_app.g->side && g_app.g->side->computer) PlayComputers(g_app.g->side);
    };
    if (!HasScript() && !spectate_all_ai && sound::SpeechOn()) {
        sound::Music(w2::cues::kBegin);
        advisor::Say(w2::cues::kBeginWar, start_war);
    } else {
        start_war();
    }
    return absl::OkStatus();
}

}  // namespace front
