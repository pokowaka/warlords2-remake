/**
 * @file
 * @brief Context-sensitive STAND.PCK mouse cursor selection and rendering.
 */
#include <algorithm>
#include <cmath>

#include "front/internal.hpp"
#include "platform/display.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/game.hpp"
#include "warlords/rules.hpp"
#include "warlords/scn.hpp"
#include "warlords/site.hpp"

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

/// Each pointer's hotspot, the same distance in from its left and top.
static const int kPtrHot[12] = {0, 6, 8, 6, 8, 8, 8, 8, 0, 8, 8, 0};

/**
 * @brief The pointer (Ptr) for the UI point (x, y): what a click there would do.
 */
int PointerKind(int x, int y) {
    if (g_app.starting || g_app.over || g_app.banner || g_app.offer || g_app.assault ||
        g_app.victory || g_app.ai_run || g_app.open_menu >= 0 || kit::Top() || !g_app.player ||
        g_app.player->computer) {
        return kArrow;
    }
    if (g_app.drag) return kHand;  // while it drags
    bool alt = Held({"lalt", "ralt"}), ctrl = Held({"lctrl", "rctrl"});
    if (g_app.strat_rect.Contains(x, y)) return alt ? kAlt : kView;
    auto tile = TileAtPoint(x, y);
    if (!tile) return g_app.panel_rect.Contains(x, y) ? kHand : kArrow;
    auto [tile_x, tile_y] = *tile;
    w2::Game& game_state = *g_app.g;
    int player_index = g_app.player->index;
    if (tile_x < 0 || tile_y < 0 || tile_x >= game_state.map->width ||
        tile_y >= game_state.map->height)
        return kHand;
    auto here = game::ArmiesAt(game_state, tile_x, tile_y);
    City* city = game::CityAt(game_state, tile_x, tile_y);
    bool armies = !here.empty();
    // the tile's owner nibble: whoever stands there, else the city's, else none
    int owner = armies ? here[0]->owner : kNone;
    if (owner == kNone) owner = city ? city->owner_index : kNone;
    if (owner == kNone) owner = w2::rules::kNeutral;
    int terrain = city ? (city->razed ? move::kSite : move::kCity)
                       : w2::scn::TerrainAt(*game_state.map, tile_x, tile_y);
    const std::vector<Army*>* sel =
            g_app.selection && !g_app.selection->stack.empty() ? &g_app.selection->stack : nullptr;
    int dist = 0, mode = kNone, under = move::kPlain;
    bool at_sea = false;
    if (sel) {
        int stack_x = (*sel)[0]->x, stack_y = (*sel)[0]->y;
        if (city && !city->razed) {
            int dx = std::max(0, std::max(city->x - stack_x, stack_x - (city->x + 1)));
            int dy = std::max(0, std::max(city->y - stack_y, stack_y - (city->y + 1)));
            dist = std::max(dx, dy);
        } else {
            dist = std::max(std::abs(tile_x - stack_x), std::abs(tile_y - stack_y));
        }
        mode = move::ModeOf(game_state, *sel);
        under = w2::scn::TerrainAt(*game_state.map, stack_x, stack_y);
        at_sea = (*sel)[0]->at_sea;
    }
    // any tile with a road on it costs 1, a mountain pass too
    bool road = w2::scn::RoadAt(*game_state.map, tile_x, tile_y) % 0x20 != 0;
    int cost = (sel && mode != move::kFlying && !road) ? move::kCost[terrain] : 1;
    if (!game::Seen(game_state, player_index, tile_x, tile_y) || cost <= 0) return kHand;
    auto walk = [&]() {
        return (mode != move::kFlying && (terrain == move::kWater || terrain == move::kShore))
                       ? kBoat
                       : kWalk;
    };
    auto look = [&]() {
        if (terrain == move::kCity) return kCityP;
        if (terrain == move::kSite) return kSiteP;
        return kHand;
    };
    static const std::set<int> kPtrOk = {move::kRoad, move::kBridge, move::kWater, move::kShore,
                                         move::kCity};
    bool enemy_near =
            sel && dist <= 1 && owner != player_index && (terrain == move::kCity || armies);
    if (Held({"lshift", "rshift"})) {
        if (enemy_near && game_state.map->options.military_advisor != 0) return kAdvise;
        return look();
    }
    if (armies && owner == player_index && !ctrl) {
        if (alt) return kAlt;
        if (!sel || dist < 1) return kSelect;
        return walk();
    }
    if (enemy_near &&
        !(at_sea && mode == move::kLand && under == move::kShore && !kPtrOk.count(terrain)) &&
        !(!at_sea && mode == move::kLand && terrain == move::kShore && !kPtrOk.count(under))) {
        if (game_state.map->options.diplomacy == 0 || owner == w2::rules::kNeutral) return kAttack;
        int relation = w2::diplomacy::State(game_state, player_index, owner);
        if (terrain == move::kCity) return relation == w2::diplomacy::kWar ? kAttack : kPeace;
        return relation != w2::diplomacy::kPeace ? kAttack : kPeace;
    }
    if (sel &&
        (owner == player_index || (owner == w2::rules::kNeutral && terrain != move::kCity))) {
        if (alt) return kAlt;
        if (dist >= 1 && ctrl && armies) return kSelect;
        return walk();
    }
    return look();
}

/**
 * @brief Draws the mouse pointer.
 *
 * Not the original's: over the black round the frame the pointer is the
 * arrow, drawn where the mouse really is rather than held to the frame's edge.
 */
void DrawPointer() {
    if (display::PointerAway()) return;
    int kind = display::state.off ? kArrow : PointerKind(g_app.mouse_x, g_app.mouse_y);
    auto [x, y] =
            display::state.off ? *display::state.off : std::make_pair(g_app.mouse_x, g_app.mouse_y);
    gfx::SetColor(1, 1, 1);
    gfx::Draw(g_app.pointer_img, gfx::NewQuad(kind * 16, 0, 16, 16), x - kPtrHot[kind],
              y - kPtrHot[kind]);
}

}  // namespace front
