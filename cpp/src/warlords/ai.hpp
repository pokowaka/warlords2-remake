// The computer players.
//
// A port of WARLORD2.EXE's AI (docs/re/ai.md). Every side has a block of AI
// data (ai/core) filled at game start from its level and its character card
// (docs/formats/crd.md); each computer turn runs the original's nineteen
// phases in the original's order (ai_turn, 5db9:0000).
//
//   ai/core       data, neighbours, the selection, flood, battles, walk
//   ai/cities     evaluate, garrisons, neutrals, production, vectoring
//   ai/groups     the assault groups and picking an enemy
//   ai/moves      standing orders, rescue, explorers, hero parties
//   ai/heroes     the hero phase
//   ai/diplomacy  proposals
//
// The Lua runs a computer turn as a coroutine, so the front end can show each
// walk and battle as it happens. Here the turn is plain code, and the hooks
// are where the front end shows things: it runs the turn on a thread of its
// own and has each hook wait for the showing to finish (front/aithread).
// With no hooks set the turn simply plays through.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "warlords/move.hpp"
#include "warlords/types.hpp"

namespace w2 {
struct Battle;
}

namespace w2::ai {

/** What the front end listens with. */
struct Hooks {
    std::function<void(Game&, const std::vector<Army*>&, const move::WalkResult&)> on_walk;
    std::function<void(Game&, const std::vector<Army*>&, int, int, Battle&)> on_fight;
    std::function<void(Game&, Side&, City&, const std::string&)> on_spoils;
};
extern Hooks hooks;

/** Whether newly initialized computer sides default to the 1993 Classic AI
 *  (true for legacy parity tests) or the 100% Learnable ML AI (false in app). */
bool DefaultClassicAi();
void SetDefaultClassicAi(bool classic);

/** A side's AI data at game start (ai_init_side, 59bf:084d). */
AIData& InitSide(Game& g, Side& side);
/** Set the computer players up for a new game (79fa:0000). */
void StartGame(Game& g);
/** Play one computer turn: ai_turn (5db9:0000), phase by phase. `pause`, if
 *  given, is called between phases with the phase's name. */
void PlayTurn(Game& g, Side& side, const std::function<void(const char*)>& pause = nullptr);
/** Every walk a computer stack makes is reported here once it is made. */
void Walked(Game& g, const std::vector<Army*>& stack, const move::WalkResult& r);
/** Where a hired hero appears for a computer (ai_hero_city, 5db9:0919). */
City* HeroCity(Game& g, Side& side, City* dflt);
/** A computer's quest hero has taken a city (5e97:038d). True when it razed
 *  or sacked, so the capture is not also taken for an occupation. */
bool QuestCapture(Game& g, Side& side, City& c, const std::vector<Army*>& stack);
/** A battle has been fought on a side's tile (ai_record_battle, 5db9:09d7). */
void RecordBattle(Game& g, int defender, int attacker, int x, int y, int heroes_lost,
                  int armies_lost, bool all_lost, bool city_tile);

}  // namespace w2::ai
