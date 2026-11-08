/**
 * @file
 * @brief Battle window playback, casualty blasts, and the city-captured spoils dialog.
 */
#include <algorithm>
#include <cmath>

#include "front/internal.hpp"
#include "platform/sound.hpp"
#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/ai/groups.hpp"
#include "warlords/armytype.hpp"
#include "warlords/cues.hpp"
#include "warlords/game.hpp"
#include "warlords/quest.hpp"
#include "warlords/scn.hpp"
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

// ------------------------------------------------------------------ the assault

// A city is not walked into, it is assaulted, and a stack always fights from
// one of the eight tiles around it. What the player sees, in order: the fire
// cloud from WAR.PCK over the tile, the battle window with both lines drawn
// up, the fight played back one army at a time in the order combat_resolve
// logged, how it ended, and if a city fell, what is to be done with it. The
// fight is decided before any of it is drawn (docs/re/ui.md > The assault).
static const kit::Rect kAsWindow{160, 60, 320, 312};  // popup 8
static const int kAsCloudW = 128, kAsCloudH = 120;
static const int kAsShieldW = 32, kAsShieldH = 36;
static const int kAsShieldDef[2] = {176, 86}, kAsShieldAtk[2] = {176, 246};
static const std::vector<int> kAsRows = {86, 116, 146, 176};  // the defender's rows
static const int kAsAtkRow = 246;
static const int kAsXEven[8] = {216, 248, 280, 312, 344, 376, 408, 440};
static const int kAsXOdd[7] = {232, 264, 296, 328, 360, 392, 424};
static const int kAsSea[4] = {0, 162, 32, 18}, kAsSeaDrop = 10;  // WAR.PCK's water
static const int kAsTextY = 298, kAsTextStep = 20;
static const double kAsCloudTime = 0.7;
// One casualty: 5, 3 and 5 BIOS ticks; once hurried, 2 and 3.
static const double kAsFellTime = 13 / 18.2, kAsFellFast = 5 / 18.2;
static const double kAsBlastTime = 5 / 18.2, kAsBlastFast = 2 / 18.2;
static const int kAsFled = 141, kAsWonCityHero = 142, kAsWonCity = 143, kAsWonHero = 144,
                 kAsWon = 145, kAsLost = 146, kAsLoot = 147;
static const int kAsVText = 65, kAsVWho = 66, kAsVWhere = 67;
static const kit::Rect kAsVPopup{160, 90, 320, 200};  // popup 7, exactly VICTORY.PCK
static const int kAsVDialog = 11;
static const int kAsOccupy = 285, kAsPillage = 283, kAsSack = 286, kAsRaze = 284;
static const int kAsVTitleY = 93;
static const int kAsVLineY[4] = {140, 160, 180, 200};

/** @brief Returns the x coordinate for `slot_num` (1-based) in a battle line of `n` armies. */
static int SlotX(int n, int slot_num) {
    if (n >= 8) return kAsXEven[slot_num - 1];
    int from = 4 - (n + 1) / 2;
    return n % 2 == 1 ? kAsXOdd[from + slot_num - 1] : kAsXEven[from + slot_num - 1];
}

/** @brief Computes the (x, y) positions for a battle line of `n` armies across `rows`. */
static std::vector<std::pair<int, int>> LineSlots(int n, const std::vector<int>& rows) {
    std::vector<std::pair<int, int>> out(n);
    int start = 1;
    size_t row = 0;
    while (n - start + 1 >= 8 && row + 1 < rows.size()) {
        for (int col = 1; col <= 8; col++) out[start + col - 2] = {kAsXEven[col - 1], rows[row]};
        start += 8;
        row++;
    }
    int y = row < rows.size() ? rows[row] : rows.back();
    for (int col = 1; col <= n - start + 1; col++)
        out[start + col - 2] = {SlotX(n - start + 1, col), y};
    return out;
}

/** @brief Returns the victor's name for the spoils dialog: the leading hero, or the first army's
 * type. */
static std::string VictorName(const std::vector<Army*>& line) {
    for (Army* army : line)
        if (army->Hero() && !army->name.empty()) return army->name;
    const w2::ArmyType* rec = line.empty() ? nullptr : g_app.g->types.ById(line[0]->type);
    return rec ? rec->name : "Your armies";
}

/** @brief Builds the outcome text lines for a finished battle. */
static std::vector<std::string> OutcomeLines(const w2::Battle& result, const City* city,
                                             bool fled) {
    std::vector<std::string> out;
    if (!result.won) {
        out.push_back(kit::Text(kAsLost, 0));
        return out;
    }
    std::string hero_name;
    for (Army* army : result.lines.attackers)
        if (army->Hero() && !army->name.empty()) {
            hero_name = army->name;
            break;
        }
    if (city) {
        if (fled) out.push_back(kit::Text(kAsFled, g_app.g->rng.Dice(1, 4, -1)));
        if (!hero_name.empty())
            out.push_back(Format(kit::Text(kAsWonCityHero, 0), hero_name));
        else
            out.push_back(kit::Text(kAsWonCity, 0));
    } else if (!hero_name.empty()) {
        out.push_back(Format(kit::Text(kAsWonHero, 0), hero_name));
        out.push_back(kit::Text(kAsWonHero, 1));
    } else {
        out.push_back(kit::Text(kAsWon, 0));
    }
    if (result.loot > 0) out.push_back(Format(kit::Text(kAsLoot, 0), result.loot));
    return out;
}

/** @brief Opens the assault animation for a decided battle at (`x`, `y`). */
void StartAssault(int x, int y, const w2::Battle& result) {
    const auto& lines = result.lines;
    bool fled = lines.defenders.empty();
    Assault assault;
    assault.x = x;
    assault.y = y;
    assault.result = result;
    assault.def = lines.defenders;
    assault.atk = lines.attackers;
    assault.def_slots = LineSlots(static_cast<int>(lines.defenders.size()), kAsRows);
    assault.atk_slots = LineSlots(static_cast<int>(lines.attackers.size()), {kAsAtkRow});
    assault.def_side = lines.defenders.empty() ? 8 : OwnerOr8(lines.defenders[0]->owner);
    assault.atk_side = lines.attackers.empty() ? 8 : OwnerOr8(lines.attackers[0]->owner);
    assault.phase = "cloud";
    assault.at = w2::Now();
    // On a human's turn, hold the cloud while WAR.8SN plays.
    assault.cloud_time = (g_app.g->side && !g_app.g->side->computer) ? sound::Effect("war") : 0;
    assault.victor = VictorName(lines.attackers);
    assault.message = OutcomeLines(result, lines.city, fled);
    assault.def_fell.resize(assault.def.size());
    assault.atk_fell.resize(assault.atk.size());
    g_app.assault = assault;
}

void CloseAssault() {
    if (!g_app.assault) return;
    Assault assault = std::move(*g_app.assault);
    g_app.assault.reset();
    game::ApplyAttack(*g_app.g, assault.result);
    g_app.medal = assault.result.medal;
    StratDirty();
    if (assault.after) assault.after();
    // a computer's assault is the coroutine's: it carries on
}

/** @brief Advances the assault animation state machine by wall-clock time. */
static void AdvanceAssault() {
    Assault& assault = *g_app.assault;
    double now = w2::Now();
    if (assault.phase == "cloud") {
        if (now - assault.at >= std::max(kAsCloudTime, assault.cloud_time)) {
            // a computer's battle nobody is to see: the cloud was all of it
            if (assault.computer && !assault.window) {
                CloseAssault();
                return;
            }
            assault.phase = "battle";
            assault.at = now;
            // Wait five ticks on the drawn-up lines before a computer's fight.
            if (assault.computer) assault.at = now + 5 / 18.2;
        }
        return;
    }
    if (assault.phase == "over" && assault.computer) {
        if (assault.close_at == 0) {
            assault.close_at = now + 15 / 18.2;
            if (g_app.ai_status) g_app.ai_status->text = assault.outcome;
        } else if (now >= assault.close_at) {
            CloseAssault();
        }
        return;
    }
    if (assault.phase != "battle") return;
    double each = assault.fast ? kAsFellFast : kAsFellTime;
    const auto& log = assault.result.log;
    while (assault.step < log.size() && now >= assault.at) {
        assault.step++;
        Fell fell{assault.at, assault.fast ? kAsBlastFast : kAsBlastTime};
        if (log[assault.step - 1] == 1) {
            if (assault.atk_down < static_cast<int>(assault.atk_fell.size()))
                assault.atk_fell[assault.atk_down] = fell;
            assault.atk_down++;
        } else {
            if (assault.def_down < static_cast<int>(assault.def_fell.size()))
                assault.def_fell[assault.def_down] = fell;
            assault.def_down++;
        }
        // Play a blow sound for each casualty, until Space hurries the fight on.
        if (!assault.fast) sound::Effect(log[assault.step - 1] == 1 ? "army" : "army2");
        assault.at += each;
    }
    if (assault.step >= log.size() && now >= assault.at) assault.phase = "over";
}

/** @brief Handles a key or click during an assault: hurries playback or closes a finished battle.
 */
void PressAssault() {
    if (!g_app.assault) return;
    Assault& assault = *g_app.assault;
    if (assault.phase != "over") {
        assault.fast = true;
        assault.at = std::min(assault.at, w2::Now() + kAsFellFast);
        AdvanceAssault();
        return;
    }
    City* city = assault.result.captured;
    bool computer = assault.computer;
    std::string victor = assault.victor;
    // captured is set once applied: apply, then ask
    CloseAssault();
    Army* medal = g_app.medal;
    g_app.medal = nullptr;
    if (!computer) {
        // the fight took effect in CloseAssault; see whether a city fell
        if (g_app.selection && !g_app.selection->stack.empty()) {
            City* captured_city = game::CityAt(*g_app.g, g_app.selection->stack[0]->x,
                                               g_app.selection->stack[0]->y);
            if (captured_city && captured_city->owner_index == g_app.player->index)
                city = captured_city;
        }
        // A medal is told before the city's fate is asked.
        auto then = [city, victor]() {
            if (city) PresentVictory(city, victor);
        };
        if (medal && g_app.g->Alive(medal))
            honours::Medal(medal, then);
        else
            then();
    }
}

/** @brief Draws the war cloud from WAR.PCK over the assaulted tile, in map pixels. */
void DrawCloud() {
    const Assault& assault = *g_app.assault;
    int screen_x = assault.x * kTile + (kTile - kAsCloudW) / 2;
    int screen_y = assault.y * kTile + (kTile - kAsCloudH) / 2;
    gfx::SetColor(1, 1, 1);
    gfx::Draw(g_app.war_pic, gfx::NewQuad(0, 0, kAsCloudW, kAsCloudH), screen_x, screen_y);
}

/** @brief Draws one side's line of armies in the battle window, hiding casualties once their blast
 * ends. */
static void DrawBattleLine(const std::vector<Army*>& armies,
                           const std::vector<std::pair<int, int>>& slots, int side, int down,
                           const std::vector<Fell>& fell) {
    if (side < 0 || side > 8) side = 8;
    double now = w2::Now();
    for (size_t i = 0; i < armies.size() && i < slots.size(); i++) {
        bool gone =
                static_cast<int>(i) < down && i < fell.size() && now >= fell[i].at + fell[i].burn;
        if (gone) continue;
        auto pos = slots[i];
        gfx::SetColor(1, 1, 1);
        if (armies[i]->at_sea)
            gfx::Draw(g_app.war_pic, gfx::NewQuad(kAsSea[0], kAsSea[1], kAsSea[2], kAsSea[3]),
                      pos.first, pos.second + kAsSeaDrop);
        gfx::Draw(g_app.army_img[side], kit::ArmyQuad(armies[i]->type), pos.first, pos.second);
        if (static_cast<int>(i) < down)
            gfx::Draw(g_app.atrans_shields, gfx::NewQuad(32, 0, 32, 29), pos.first, pos.second);
    }
}

/** @brief Draws the battle window, both armies' lines, shields, and outcome text. */
static void DrawBattle() {
    const Assault& assault = *g_app.assault;
    const auto& popup_rect = kAsWindow;
    gfx::SetColor(0, 0, 0);
    gfx::Rectangle(gfx::kLine, popup_rect.x - 0.5, popup_rect.y - 0.5, popup_rect.w + 1,
                   popup_rect.h + 1);
    gfx::Rectangle(gfx::kFill, popup_rect.x + 1, popup_rect.y + popup_rect.h + 1, popup_rect.w + 2,
                   2);
    gfx::Rectangle(gfx::kFill, popup_rect.x + popup_rect.w + 1, popup_rect.y + 1, 2,
                   popup_rect.h + 2);
    gfx::SetColor(1, 1, 1);
    gfx::Draw(g_app.marble, gfx::NewQuad(0, 0, popup_rect.w, popup_rect.h), popup_rect.x,
              popup_rect.y);
    auto shield = [](int side, const int* pos) {
        gfx::SetColor(1, 1, 1);
        gfx::Draw(g_app.shield_img, gfx::NewQuad(side * kAsShieldW, 0, kAsShieldW, kAsShieldH),
                  pos[0], pos[1]);
    };
    shield(assault.def_side, kAsShieldDef);
    shield(assault.atk_side, kAsShieldAtk);
    DrawBattleLine(assault.def, assault.def_slots, assault.def_side, assault.def_down,
                   assault.def_fell);
    DrawBattleLine(assault.atk, assault.atk_slots, assault.atk_side, assault.atk_down,
                   assault.atk_fell);
    if (assault.phase == "over") {
        int y = kAsTextY;
        gfx::SetColor(1, 1, 1);
        for (auto& line : assault.message) {
            kit::Centred(*g_app.big_font, line, 320, y);  // on x = 320
            y += kAsTextStep;
        }
    }
}

/** @brief Advances and draws the active assault window when past the cloud phase. */
void DrawAssault() {
    AdvanceAssault();
    if (!g_app.assault) return;
    if (g_app.assault->phase != "cloud") DrawBattle();
}

/**
 * @brief Opens the city-captured dialog (Occupy, Pillage, Sack, Raze) for `city`.
 *
 * Dialog 11 behind popup 7 (VICTORY.PCK) with four buttons. Pillage requires at
 * least one production type to strip, and Sack requires at least two.
 */
void PresentVictory(City* city, const std::string& victor) {
    if (!g_app.victory_view) g_app.victory_view = screen::MakeView(*g_app.screen, kAsVDialog);
    Victory vic;
    vic.city = city;
    vic.who = Format(kit::Text(kAsVWho, g_app.g->rng.Dice(1, 4, -1)), victor);
    vic.where = Format(kit::Text(kAsVWhere, g_app.g->rng.Dice(1, 3, -1)), city->name);
    g_app.victory = vic;
    auto& states = g_app.victory_view->state;
    states[kAsOccupy] = kNormal;
    states[kAsRaze] = kNormal;
    // Enable Pillage and Sack when they would yield gold.
    int pillage_gold = city->slots.empty()
                               ? 0
                               : std::abs(g_app.g->types.ById(city->slots.back().type)->price) / 2;
    int sack_gold = w2::ai::groups::SackValue(*g_app.g, *city);
    states[kAsPillage] = pillage_gold != 0 ? kNormal : kDisabled;
    states[kAsSack] = sack_gold != 0 ? kNormal : kDisabled;
    // Enter chooses Occupy unless the stack's hero is on a quest this answers:
    // Raze for a raze quest on this city, Sack (or Pillage) for gold.
    g_app.victory->enter = kAsOccupy;
    auto quest = g_app.player->quest;
    Army* hero = nullptr;
    if (g_app.selection) {
        for (Army* army : g_app.selection->stack)
            if (army->type == w2::armytype::kHero) {
                hero = army;
                break;
            }
    }
    if (g_app.g->map->options.quests != 0 && quest && hero && hero == quest->hero) {
        if (quest->type == w2::quest::kRaze && quest->city == city)
            g_app.victory->enter = kAsRaze;
        else if (quest->type == w2::quest::kPillageGold) {
            if (sack_gold != 0)
                g_app.victory->enter = kAsSack;
            else if (pillage_gold != 0)
                g_app.victory->enter = kAsPillage;
        }
    }
}

/** @brief Draws the city-captured victory popup and its controls. */
void DrawVictory(const Victory& vic) {
    const auto& popup_rect = kAsVPopup;
    gfx::SetColor(0, 0, 0);
    gfx::Rectangle(gfx::kLine, popup_rect.x - 0.5, popup_rect.y - 0.5, popup_rect.w + 1,
                   popup_rect.h + 1);
    gfx::Rectangle(gfx::kFill, popup_rect.x + 1, popup_rect.y + popup_rect.h + 1, popup_rect.w + 2,
                   2);
    gfx::Rectangle(gfx::kFill, popup_rect.x + popup_rect.w + 1, popup_rect.y + 1, 2,
                   popup_rect.h + 2);
    gfx::SetColor(1, 1, 1);
    gfx::Draw(g_app.victory_pic, popup_rect.x, popup_rect.y);
    kit::Centred(*g_app.title_font, kit::Text(kAsVText, 0), 320, kAsVTitleY);
    kit::Centred(*g_app.big_font, vic.who, 320, kAsVLineY[0]);
    kit::Centred(*g_app.big_font, vic.where, 320, kAsVLineY[1]);
    kit::Centred(*g_app.big_font, kit::Text(kAsVText, 1), 320, kAsVLineY[2]);
    kit::Centred(*g_app.big_font, kit::Text(kAsVText, 2), 320, kAsVLineY[3]);
    screen::DrawDialogControls(*g_app.screen, *g_app.victory_view);
}

/** @brief Executes the player's chosen action (Occupy, Pillage, Sack, or Raze) on a captured city.
 */
void TakeCity(int what) {
    if (!g_app.victory) return;
    Victory vic = *g_app.victory;
    City* city = vic.city;
    std::vector<Army*> stack = g_app.selection ? g_app.selection->stack : std::vector<Army*>{};
    g_app.victory.reset();
    auto done = []() {
        g_app.victory_under.reset();
        StratDirty();
        RefreshControls();
    };
    if (what == kAsPillage || what == kAsSack) {
        bool sacked = what == kAsSack;
        auto spoils = sacked ? game::Sack(*g_app.g, *g_app.player, *city, stack)
                             : game::Pillage(*g_app.g, *g_app.player, *city, stack);
        g_app.victory_under = vic;
        spoils::Open(sacked, city, spoils.gold, spoils.lost, static_cast<int>(city->slots.size()),
                     done);
    } else if (what == kAsRaze) {
        g_app.victory_under = vic;
        search::Say(city->name + " is in ruins!", [city, stack, done]() {
            game::Raze(*g_app.g, *g_app.player, *city, stack);
            done();
        });
    } else {
        // quest_check(4): a quest done is its reward instead of the city
        auto news = game::Occupy(*g_app.g, *g_app.player, *city, stack);
        auto production = [city]() { city::Open(city, city::kProduction); };
        if (news) {
            g_app.player->quest_news.reset();
            if (!news->failed.empty())
                questnews::Show(news, production);
            else
                questnews::Show(news);
        } else {
            production();
        }
    }
}

}  // namespace front
