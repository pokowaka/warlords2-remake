// Warlords II, in the original's own interface, on SDL2.
//
// A port of the Lua remake's front end (love2d/main.lua). The screen is
// 640x480 and every rect in it comes from the game's own layout files:
// DATA/JOIN.DAT names the dialog's controls and clickable regions, AREA.DAT
// places the regions, BUTTON.DAT the controls, and FILE.DAT says which .PCK
// each control is cut from. docs/re/ui.md and docs/formats/screens.md.
//
//     warlords2 [--data DIR] [--scenario NAME] [--seed N] [--window]
//               [--size WxH] [--script FILE]
//
// --size opens a window of exactly W x H device pixels, to try a screen
// other than this one (the Steam Deck's is 1280x800).
//
// Run from the repository root, or point --data at your copy of the game.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <random>
#include <regex>
#include <set>
#include <sstream>

#include <SDL.h>

#include "front/app.hpp"
#include "front/font.hpp"
#include "front/layout.hpp"
#include "front/menu.hpp"
#include "front/pckimage.hpp"
#include "front/prefs.hpp"
#include "front/screen.hpp"
#include "platform/coroutine.hpp"
#include "platform/display.hpp"
#include "platform/gfx.hpp"
#include "platform/keys.hpp"
#include "platform/pad.hpp"
#include "platform/sound.hpp"
#include "ui/dialogs.hpp"
#include "ui/kit.hpp"
#include "util/json.hpp"
#include "util/util.hpp"
#include "warlords/ai.hpp"
#include "warlords/ai/belief.hpp"
#include "warlords/ai/core.hpp"
#include "warlords/ai/groups.hpp"
#include "warlords/ai/ml_policy.hpp"
#include "warlords/armytype.hpp"
#include "warlords/bytes.hpp"
#include "warlords/cues.hpp"
#include "warlords/diplomacy.hpp"
#include "warlords/game.hpp"
#include "warlords/hero.hpp"
#include "warlords/history.hpp"
#include "warlords/move.hpp"
#include "warlords/pal.hpp"
#include "warlords/quest.hpp"
#include "warlords/randommap.hpp"
#include "warlords/rules.hpp"
#include "warlords/save.hpp"
#include "warlords/scn.hpp"
#include "warlords/site.hpp"
#include "warlords/slots.hpp"
#include "warlords/uidata.hpp"

App G;

using w2::Army;
using w2::City;
using w2::format;
using w2::NONE;
using w2::Side;
using w2::uidata::ACTIVE;
using w2::uidata::DISABLED;
using w2::uidata::NORMAL;
namespace move = w2::move;
namespace game = w2::game;
namespace slots_mod = w2::slots;

static const int kTile = screen::TILE;
static const double kCursRate = 18.2 / 4;  // CURS.PCK frames a second (177b:011f)

static std::mt19937 loose_dice(static_cast<unsigned>(time(nullptr)));

namespace front {
int roll(int n) {
    return n <= 0 ? 1 : static_cast<int>(loose_dice() % static_cast<unsigned>(n)) + 1;
}
void say(const std::string& s) {
    G.status = s;
}
}  // namespace front

using front::say;

// ------------------------------------------------------------------ forwards

static void BeginGame();
static void ShowBanner(Side* side);
static void FinishRound(Side* side);
static void PlayComputers(Side* side);
static void RefreshControls();
static void MoveSelection(int x, int y);
static void StartAssault(int x, int y, const w2::Battle& result);
static void PresentVictory(City* city, const std::string& victor);
static void Keypressed(const std::string& k);
static void Select(int x, int y, Army* pick = nullptr);
static void AfterBanner();
static void RefreshRoute();
static void SyncSelection();
static bool MenuEnabled(const std::string& k);
static void MenuPick(const std::string& k);
static void ResumeComputer();
static std::map<std::string, std::function<void()>>& MenuDoes();

// ------------------------------------------------------------------ helpers

static gfx::Quad TileQuad(int t) {
    int i = t % 96;
    return gfx::newQuad((i % 16) * kTile, (i / 16) * kTile, kTile, kTile);
}
static gfx::Quad RoadQuad(int i) {
    return gfx::newQuad((i % 13) * 48, (i / 13) * kTile, kTile, kTile);
}
static void PalColour(int i) {
    kit::setPal(i);
}
// Only the keys the game saw go down, and has not seen come up or lost with
// the window's focus (keys::clear). Not SDL_GetModState: a modifier held as
// the window loses focus (Ctrl+arrow to another Space, Cmd+Tab) stays "down"
// there, and a phantom Ctrl or Alt turns every click into another command.
static bool Held(std::initializer_list<const char*> k) {
    return keys::isDown(k);
}
static int OwnerOr8(int o) {
    return o == NONE ? 8 : o;
}
static int opt_military_advisor = -1;

// ------------------------------------------------------------------ loading

static void LoadArt() {
    G.palette = w2::pal::load(G.dataDir + "/TERRAIN0/WAR2.PAL");
    const auto& palette = G.palette;
    auto img = [&](const std::string& path, int key = pckimage::NOKEY) {
        return pckimage::load(G.dataDir + path, palette, key);
    };
    for (int i = 0; i < 2; i++) G.sheets[i] = img("/TERRAIN0/SCENERY" + std::to_string(i) + ".PCK");
    // The army sheets stand on 10, bright green -- all but the fifth side's,
    // keyed on its own ground, the colour of its corner.
    G.roadImg = img("/TERRAIN0/ROAD.PCK", 1);
    for (int i = 0; i <= 8; i++)
        G.armyImg[i] = img("/TERRAIN0/A" + std::to_string(i) + ".PCK", pckimage::CORNER);
    // the shadow sheet keys on white: it is two colours of ghost on white
    G.shadowImg = img("/TERRAIN0/ASHADOW.PCK", 15);
    G.marble = img("/PICS/MARBLE.PCK");
    G.bigArmy = img("/PICS/BIGARMY.PCK");
    G.cityPic = img("/PICS/CITY.PCK");
    G.warPic = img("/PICS/WAR.PCK", 1);
    G.shieldImg = img("/TERRAIN0/BSHIELD.PCK", 12);
    G.victoryPic = img("/PICS/VICTORY.PCK");
    G.heroPicM = img("/PICS/MHERO.PCK");
    G.heroPicF = img("/PICS/FHERO.PCK");
    // ATRANS2.PCK: map markers on a mask colour, 1 (1997:027e, 8cc6:0952)
    G.atransShields = img("/TERRAIN0/ATRANS2.PCK", 1);
    // HIDDEN.PCK: the hidden map's edges; colour 1 is where the map shows through
    G.fogImg = img("/PICS/HIDDEN.PCK", 1);
    // CURS.PCK: the marching box round the selected stack (177b:01a1); keyed
    // on 0, its 1 shows white and its 4 black
    G.cursImg = pckimage::fromPixels(*w2::pck::decode(G.dataDir + "/PICS/CURS.PCK"), palette,
                                     {{1, 15}, {4, 0}}, {0});
    // the rings, drawn opaque as 1997:0129 blits every bitmap it is given
    G.abits = img("/PICS/ABITS.PCK");
    // STAND.PCK: the twelve mouse pointers, drawn by the game itself (22bf)
    G.pointerImg = img("/STAND.PCK", 10);
    G.screen = screen::load(G.dataDir, palette, w2::uidata::MAIN_SCREEN, 0);
    // the player's own button shortcuts (ui/shortcuts)
    try {
        std::string sc = prefs::get("shortcuts");
        if (!sc.empty()) {
            auto j = w2::Json::parse(sc);
            for (size_t i = 0; i < j.size(); i++)
                G.screen->ui.shortcuts[static_cast<int>(i)] = static_cast<int>(j[i].integer());
        }
    } catch (...) {
    }
    G.font = Font::load(G.dataDir, "TEXT", palette, 3);
    G.bigFont = Font::load(G.dataDir, "CHANCE17", palette, 3);
    G.titleFont = Font::load(G.dataDir, "CHANCE36", palette, 3);
    G.shieldsImg = img("/TERRAIN0/SHIELDS.PCK");
    G.cityBack = img("/TERRAIN0/CITYBACK.PCK");
    G.searchPic = img("/PICS/SEARCH.PCK");
    G.templePic = img("/PICS/TEMPLE.PCK");
    // a promotion's (popups 16-18) and a medal's (popups 24-27) pictures
    for (int i = 2; i <= 4; i++) G.levelPic[i] = img("/PICS/LEVEL" + std::to_string(i) + ".PCK");
    for (int i = 0; i < 4; i++) G.medalPic[i] = img("/PICS/MEDAL" + std::to_string(i) + ".PCK");
    G.scrollPic = img("/PICS/SCROLL.PCK", 10);
}

// ------------------------------------------------------------------ camera

// The view is as many tiles as the map's rect holds at the map's zoom, and
// slides smoothly: G.cx, G.cy is the tile at its top-left corner.

static std::pair<double, double> ViewSize() {
    double k = static_cast<double>(display::d.scale) / (kTile * G.zoom);
    return {G.mapRect.w * k, G.mapRect.h * k};
}

// The camera in device pixels, rounded to one.
static std::pair<int, int> CamDev() {
    double z = kTile * G.zoom;
    return {static_cast<int>(std::floor(G.cx * z + 0.5)),
            static_cast<int>(std::floor(G.cy * z + 0.5))};
}

static std::pair<double, double> UiToMap(double x, double y) {
    const auto& r = G.mapRect;
    double s = display::d.scale, z = kTile * G.zoom;
    auto [cx, cy] = CamDev();
    return {(cx + (x - r.x) * s) / z, (cy + (y - r.y) * s) / z};
}

std::pair<double, double> front::mapToUI(double tx, double ty) {
    const auto& r = G.mapRect;
    double s = display::d.scale, z = kTile * G.zoom;
    auto [cx, cy] = CamDev();
    return {r.x + (tx * z - cx) / s, r.y + (ty * z - cy) / s};
}

std::pair<int, int> front::viewCentre() {
    auto [vw, vh] = ViewSize();
    return {static_cast<int>(std::floor(G.cx + vw / 2)),
            static_cast<int>(std::floor(G.cy + vh / 2))};
}

static std::array<int, 4> ViewTiles() {
    auto [vw, vh] = ViewSize();
    int mw = G.g->map->width, mh = G.g->map->height;
    return {std::max(0, static_cast<int>(std::floor(G.cx))),
            std::max(0, static_cast<int>(std::floor(G.cy))),
            std::min(mw - 1, static_cast<int>(std::ceil(G.cx + vw)) - 1),
            std::min(mh - 1, static_cast<int>(std::ceil(G.cy + vh)) - 1)};
}

static bool Shown(int x, int y) {
    return x >= G.view[0] && y >= G.view[1] && x <= G.view[2] && y <= G.view[3];
}

static bool InView(int x, int y) {
    auto [vw, vh] = ViewSize();
    return x + 0.5 > G.cx && x + 0.5 < G.cx + vw && y + 0.5 > G.cy && y + 0.5 < G.cy + vh;
}

// Keep the view on the map; a map smaller than the view sits in its middle.
static void ClampCamera() {
    if (!G.g) return;
    auto [vw, vh] = ViewSize();
    int mw = G.g->map->width, mh = G.g->map->height;
    G.cx = vw >= mw ? (mw - vw) / 2 : std::max(0.0, std::min(G.cx, mw - vw));
    G.cy = vh >= mh ? (mh - vh) / 2 : std::max(0.0, std::min(G.cy, mh - vh));
}

void front::centreOn(int x, int y) {
    auto [vw, vh] = ViewSize();
    G.cx = x + 0.5 - vw / 2;
    G.cy = y + 0.5 - vh / 2;
    ClampCamera();
}
using front::centreOn;

// The zoom the map may go to: up to two steps past the UI's biggest.
static int MaxZoom() {
    return std::max(display::d.maxScale, display::d.scale) + 2;
}

// Zoom the map to `z`, keeping the map under the UI point (x, y) put.
static void SetZoomAt(int z, double x, double y, bool centre) {
    z = std::max(1, std::min(MaxZoom(), z));
    if (z == G.zoom) return;
    prefs::set("zoom", z);
    const auto& r = G.mapRect;
    if (centre) {
        x = r.x + r.w / 2.0;
        y = r.y + r.h / 2.0;
    }
    auto [tx, ty] = UiToMap(x, y);
    G.zoom = z;
    double k = static_cast<double>(display::d.scale) / (kTile * z);
    G.cx = tx - (x - r.x) * k;
    G.cy = ty - (y - r.y) * k;
    ClampCamera();
}
void front::setZoom(int z) {
    SetZoomAt(z, 0, 0, true);
}

static void ApplyLayout(const layout::Layout& new_layout) {
    std::optional<std::pair<double, double>> mid;
    if (G.g && G.haveLayout) {
        auto [vw, vh] = ViewSize();
        mid = std::make_pair(G.cx + vw / 2, G.cy + vh / 2);
    }
    G.layout = new_layout;
    G.haveLayout = true;
    screen::relayout(*G.screen, new_layout);
    auto reg = [](int id) {
        const auto* r = screen::region(*G.screen, id);
        return r ? layout::Rect{r->x, r->y, r->w, r->h, r->id} : layout::Rect{};
    };
    G.mapRect = reg(screen::MAP);
    G.stratRect = reg(screen::STRATEGIC);
    G.barRect = reg(screen::BOTTOMBAR);
    G.menuRect = reg(screen::MENUBAR);
    G.panelRect = reg(screen::MAPPANEL);
    G.menuLayout = menu::layout(*G.font, new_layout.w,
                                menu::withZooms(display::d.maxShort, MaxZoom(),
                                                display::d.smoothScale, display::d.shrink));
    G.zoom = std::max(1, std::min(MaxZoom(), G.zoom ? G.zoom : display::d.scale));
    if (mid) {
        auto [vw, vh] = ViewSize();
        G.cx = mid->first - vw / 2;
        G.cy = mid->second - vh / 2;
        ClampCamera();
    }
}

// The start screens are the original's 640x480, centred; the game has the
// whole screen, down to SHORT_H rows. Run each frame, and each input.
void front::syncLayout() {
    int w = layout::W, h = layout::H;
    if (!G.starting) std::tie(w, h) = display::uiSize();
    display::setFrame(std::max(w, layout::W),
                      std::max(h, G.starting ? layout::H : layout::SHORT_H));
    if (!G.haveLayout || G.layout.w != display::d.w || G.layout.h != display::d.h) {
        ApplyLayout(layout::compute(display::d.w, display::d.h));
    }
}
using front::syncLayout;

// Change how the screen is used, keeping the view centred where it was.
static void Relayout(const std::function<void()>& change) {
    std::optional<std::pair<double, double>> mid;
    if (G.g && !G.starting && G.haveLayout) {
        auto [vw, vh] = ViewSize();
        mid = std::make_pair(G.cx + vw / 2, G.cy + vh / 2);
    }
    change();
    G.haveLayout = false;
    syncLayout();
    if (mid) {
        auto [vw, vh] = ViewSize();
        G.cx = mid->first - vw / 2;
        G.cy = mid->second - vh / 2;
        ClampCamera();
    }
}

static void SetUiScale(int n) {
    Relayout([n]() { display::choose(n); });
    if (display::d.chosen == display::SMOOTH)
        prefs::set("scale", "smooth");
    else
        prefs::set("scale", display::d.chosen);
}

static void SetOriginalSize(bool on) {
    Relayout([on]() {
        if (on)
            display::d.wanted = std::make_pair(layout::W, layout::H);
        else
            display::d.wanted.reset();
    });
    prefs::set("4:3", on ? "on" : "off");
}

static void SetScreen(bool window) {
    Relayout([window]() { display::setWindowed(window); });
    prefs::set("screen", window ? "window" : "full");
}

static void ToggleCrt() {
    display::d.crt = !display::d.crt;
    prefs::set("crt", display::d.crt ? "on" : "off");
}

static bool ActiveClassicAi() {
    if (G.g && G.g->map && !G.starting) {
        for (const auto& s : G.g->map->sides) {
            if (s.inUse && s.computer) return s.classicAi;
        }
        for (const auto& s : G.g->map->sides) {
            if (s.inUse) return s.classicAi;
        }
    }
    return w2::ai::defaultClassicAi();
}

static void SetAiMode(bool classic) {
    w2::ai::setDefaultClassicAi(classic);
    prefs::set("ai", classic ? "classic" : "modern");
    if (G.g && G.g->map) {
        for (auto& s : G.g->map->sides) {
            s.classicAi = classic;
            if (!s.ai) s.ai = std::make_shared<w2::AIData>(w2::ai::core::newData());
            if (!classic) w2::ai::belief::ensure(s);
        }
    }
}

// The interface's scale in use; a short one stays ticked on the start
// screens, which are drawn as with none chosen.
static std::string UiScaleKey() {
    const auto& d = display::d;
    if (d.chosen > d.maxScale && d.chosen <= d.maxShort)
        return "ui scale " + std::to_string(d.chosen);
    return d.smooth ? "ui scale smooth" : "ui scale " + std::to_string(d.scale);
}

static bool MenuTicked(const std::string& k) {
    return k == "map zoom " + std::to_string(G.zoom) || k == UiScaleKey() ||
           k == std::string("screen ") + (display::d.windowed ? "window" : "full") ||
           (k == "screen 4:3" && display::d.wanted) || (k == "screen fit" && !display::d.wanted) ||
           (k == "screen crt" && display::d.crt) || k == "music " + sound::synth() ||
           k == (ActiveClassicAi() ? "ai classic" : "ai modern");
}

// ------------------------------------------------------------------ walking

// A stack walks one tile at a time, and the map shows where it is going. The
// destination is kept in the army record itself, so it outlives a walk that
// could not finish. 8611:2ef5 marks the route: a ring on every tile but the
// last, plain while the stack can still reach it this turn and crossed once
// it cannot, and a ghost of the leading army on the last (docs/re/ui.md).
static const int kWalkRing[2] = {496, 31}, kWalkCrossed[2] = {496, 48};
static const int kWalkRingW = 16, kWalkRingH = 14;
static const int kWalkRingAt[2] = {16, 13}, kWalkGhostAt[2] = {4, 4};
static const double kWalkStep = 0.1;  // one tile of the walk

static void RefreshRoute() {
    G.route.reset();
    if (!G.selection || G.selection->stack.empty()) return;
    auto& sel = *G.selection;
    auto target = sel.stack[0]->target;
    if (!target) return;
    if (sel.stack[0]->x == target->first && sel.stack[0]->y == target->second) {
        for (Army* a : sel.stack) a->target.reset();
        return;
    }
    G.route = move::preview(*G.g, sel.stack, target->first, target->second);
}

static void OrderTo(const std::vector<Army*>& stack, int x, int y) {
    for (Army* a : stack) {
        if (x == NONE)
            a->target.reset();
        else
            a->target = std::make_pair(x, y);
    }
}

// Alt and a click (1c8c:0007): the route is drawn but not walked.
static void PlanRoute(int x, int y) {
    if (!G.selection || G.selection->stack.empty()) return;
    auto& sel = *G.selection;
    if (!game::seen(*G.g, G.player->index, x, y)) return;
    if (sel.stack[0]->x == x && sel.stack[0]->y == y) {
        OrderTo(sel.stack, NONE, NONE);
        G.route.reset();
        return;
    }
    OrderTo(sel.stack, x, y);
    RefreshRoute();
    if (!G.route) sound::effect("chord");
}

// Play the walk back a tile at a time, centring on the stack as it goes.
// `seaBefore`: the stack was at sea as it set off, and is shown so to the end
// of the walk -- the original puts it to sea or ashore only after its last
// step (place_stack_at_step, 1a8b:04c8).
static void StartWalk(const std::vector<Army*>& armies,
                      const std::vector<std::pair<int, int>>& tiles, bool sea_before) {
    if (tiles.empty()) return;
    Walk w;
    w.seaBefore = sea_before;
    w.armies = std::set<Army*>(armies.begin(), armies.end());
    w.tiles = tiles;
    w.i = 0;
    w.at = w2::now();
    w.top = armies.empty() ? nullptr : armies[0];
    G.walk = w;
    centreOn(tiles[0].first, tiles[0].second);
}

static void MoveAllStep();
static void FlushKeys();

// A stack out of moves after its walk: done for the turn (8c07:08fb) and put
// down (1b62:08b3), with nothing selected. A goal it has reached is forgotten
// (1c8c:01fd).
static void PutDown(const std::vector<Army*>& stack) {
    Army* lead = stack.empty() ? nullptr : stack[0];
    bool arrived = lead && lead->target && lead->x == lead->target->first &&
                   lead->y == lead->target->second;
    for (Army* a : stack) {
        a->done = true;
        if (arrived) a->target.reset();
    }
    G.selection.reset();
    G.route.reset();
}

static void AdvanceWalk() {
    if (!G.walk) return;
    Walk& w = *G.walk;
    double t = w2::now();
    while (w.i < w.tiles.size() - 1 && t - w.at >= kWalkStep) {
        w.i++;
        w.at += kWalkStep;
        centreOn(w.tiles[w.i].first, w.tiles[w.i].second);
    }
    if (w.i >= w.tiles.size() - 1 && t - w.at >= kWalkStep) {
        bool computer = w.computer;
        auto down = w.putDown;
        G.walk.reset();
        if (!down.empty()) PutDown(down);
        RefreshRoute();
        if (computer && G.aiRun) ResumeComputer();
        if (G.moveAll) MoveAllStep();
        if (!G.walk && !G.moveAll) FlushKeys();
    }
}

static const std::pair<int, int>* WalkingAt() {
    return G.walk ? &G.walk->tiles[G.walk->i] : nullptr;
}

static void DrawRoute() {
    if (!G.route) return;
    const auto& route = *G.route;
    Army* ghost = G.selection && !G.selection->stack.empty() ? G.selection->stack[0] : nullptr;
    // while the walk plays out the route is drawn from where it has got to
    size_t first = G.walk ? G.walk->i + 1 : 1;
    for (size_t i = first; i <= route.path.size(); i++) {
        const auto& step = route.path[i - 1];
        if (!Shown(step.x, step.y)) continue;
        int sx = step.x * kTile, sy = step.y * kTile;
        gfx::setColor(1, 1, 1);
        if (i == route.path.size()) {
            if (ghost)
                gfx::draw(G.shadowImg, kit::armyQuad(ghost->type), sx + kWalkGhostAt[0],
                          sy + kWalkGhostAt[1]);
        } else {
            const int* src = static_cast<int>(i) <= route.reach ? kWalkRing : kWalkCrossed;
            gfx::draw(G.shadowImg, gfx::newQuad(src[0], src[1], kWalkRingW, kWalkRingH),
                      sx + kWalkRingAt[0], sy + kWalkRingAt[1]);
        }
    }
}

// Screen point to map tile, or none outside the viewport.
static std::optional<std::pair<int, int>> TileAtPoint(int sx, int sy) {
    const auto& r = G.mapRect;
    if (sx < r.x || sy < r.y || sx >= r.x + r.w || sy >= r.y + r.h) return std::nullopt;
    auto [tx, ty] = UiToMap(sx, sy);
    return std::make_pair(static_cast<int>(std::floor(tx)), static_cast<int>(std::floor(ty)));
}

// ------------------------------------------------------------------ selection

static std::vector<Army*> SelectableAt(int x, int y) {
    std::vector<Army*> out;
    for (Army* a : game::armiesAt(*G.g, x, y))
        if (a->owner == G.player->index && static_cast<int>(out.size()) < w2::rules::MAX_STACK)
            out.push_back(a);
    return out;
}

// Pick up whatever the slots now say moves, and remember the grouping.
static void SyncSelection() {
    if (!G.selection) return;
    auto& sel = *G.selection;
    slots_mod::commit(sel.slots, *G.g);
    sel.stack = slots_mod::selected(sel.slots);
    // being in the group that moves puts an army back in the cycle and marks
    // it offered for this pass (89e0:000a, 8c07:06eb)
    for (Army* a : sel.stack) {
        a->fortified = false;
        a->offered = true;
    }
}

// Put the slots back over a new list of armies, keeping the group.
static void Reslot(const std::vector<Army*>& armies) {
    if (!G.selection) return;
    auto s = armies.empty() ? std::nullopt : slots_mod::keep(G.selection->slots, *G.g, armies);
    if (!s) {
        G.selection.reset();
        return;
    }
    G.selection->slots = *s;
    G.selection->x = s->army[0]->x;
    G.selection->y = s->army[0]->y;
    SyncSelection();
}

// "Alas! %d armies have been sunk" / "due to lack of a boat!" (STRING.DAT 168)
static const int kSunkGroup = 0xa8;

// Drown the player's armies stranded on water without a boat
// (auto_ui_armies_sunk, 1b62:0ea0): on one tile, or with NONE on all of
// them. If any sank: a splash, the selection put down, the view on the tile,
// and the message (8065:1160), `after` running once it is read. Returns
// whether any did; if not, `after` runs at once.
static bool DrownStranded(int x, int y, kit::Done after = nullptr) {
    int n = game::sinkStranded(*G.g, *G.player, x, y);
    if (n == 0) {
        if (after) after();
        return false;
    }
    sound::effect("splash");
    G.selection.reset();
    G.route.reset();
    front::stratDirty();
    if (x != NONE) centreOn(x, y);
    std::string line = n == 1 ? kit::text(kSunkGroup, 2) : format(kit::text(kSunkGroup, 0), n);
    search::message(line, kit::text(kSunkGroup, 1), after);
    return true;
}

static void Select(int x, int y, Army* pick) {
    // picking a stack up on the water drowns whoever has no boat (8c07:06eb)
    if (DrownStranded(x, y)) return;
    auto stack = SelectableAt(x, y);
    if (stack.empty()) {
        G.selection.reset();
        if (City* city = game::cityAt(*G.g, x, y)) front::openCity(city);
        return;
    }
    // a click selects one army, not the stack (docs/re/ui.md > The army slots)
    auto chosen = slots_mod::clicked(*G.g, stack, G.player->index, pick);
    Selection sel;
    sel.x = x;
    sel.y = y;
    sel.slots = slots_mod::build(*G.g, stack, G.player->index, &chosen);
    G.selection = sel;
    SyncSelection();
    RefreshRoute();
    // the tutorial on picking a stack up (1b62:051d-062a)
    std::vector<std::string> moments = {"move"};
    for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
            City* c = game::cityAt(*G.g, x + dx, y + dy);
            if (c && c->ownerIndex != G.player->index && !c->razed && moments.size() == 1)
                moments.push_back("fight");
        }
    }
    Army* lead = G.selection->stack.empty() ? nullptr : G.selection->stack[0];
    if (lead && lead->hero() && !G.g->map->siteAt.empty() &&
        G.g->map->siteAt[y * G.g->map->width + x])
        moments.push_back("search");
    tutorial::chain(moments);
}
void front::selectAt(int x, int y) {
    Select(x, y);
}

// ------------------------------------------------------------- the army cycle

// The five buttons above the pad walk you through your armies one stack at
// a time (8c07:040e). The next army is the nearest of the eligible ones to
// where the cycle last stopped; one exactly there counts as far away.

// Where a side's capital now stands (67cc:10d7): its own, or the city of
// its it moved to while the side does not hold it.
static std::pair<int, int> CapOf(const Side* side) {
    if (side->capNowX != NONE) return {side->capNowX, side->capNowY};
    if (side->capital) return {side->capital->x, side->capital->y};
    return {side->capX == NONE ? 0 : side->capX, side->capY == NONE ? 0 : side->capY};
}

static void CycleReset() {
    G.cursor = CapOf(G.player);  // 8c07:0113
}

static int CycleRank(const Army* a) {
    int d = std::abs(a->x - G.cursor->first) + std::abs(a->y - G.cursor->second);
    return d == 0 ? 9000 : d;
}

static Army* NextArmy() {
    if (!G.cursor) CycleReset();
    Army* fresh = nullptr;
    Army* used = nullptr;
    for (Army* a : G.g->armies) {
        if (a->owner == G.player->index && !a->transit && !a->fortified && !a->done) {
            if (a->offered) {
                if (!used || CycleRank(a) < CycleRank(used)) used = a;
            } else if (!fresh || CycleRank(a) < CycleRank(fresh)) {
                fresh = a;
            }
        }
    }
    if (!fresh) {
        if (!used) return nullptr;
        for (Army* a : G.g->armies)
            if (a->owner == G.player->index) a->offered = false;
        fresh = used;
    }
    G.cursor = std::make_pair(fresh->x, fresh->y);
    return fresh;
}

static void SelectNext() {
    Army* a = NextArmy();
    if (!a) {
        G.selection.reset();
        G.route.reset();
        say("Every army has moved.");
        sound::effect("chord");  // 8c07:019b: nothing left
        return;
    }
    sound::effect("ding");
    Select(a->x, a->y, a);
    centreOn(a->x, a->y);
}

// Out of the cycle for the rest of this turn (control 175, 8c07:0393).
static void QuitArmy() {
    if (!G.selection) {
        SelectNext();
        return;
    }
    for (Army* a : G.selection->stack) a->done = true;
    G.route.reset();
    SelectNext();
}

// Dig in: out of the cycle until picked up again (control 176, 8c07:03a6).
static void Fortify() {
    if (!G.selection) {
        SelectNext();
        return;
    }
    for (Army* a : G.selection->stack) {
        a->fortified = true;
        a->target.reset();
    }
    G.route.reset();
    SelectNext();
}

// A second click within 40 BIOS ticks of the last counts as a double click
// (18a9:01cd).
static const double kDoubleClick = 40 / 18.2;

// A click with the select pointer (1b62:0405): on the selected stack's own
// tile, soon after the last such click, it groups the whole stack as Space
// does (89e0:0a55); otherwise it selects what is there.
static void SelectClick(int tx, int ty) {
    double t = w2::now(), last = G.lastSelectClick;
    G.lastSelectClick = t;
    if (G.selection && G.selection->x == tx && G.selection->y == ty && last >= 0 &&
        t - last < kDoubleClick) {
        slots_mod::all(G.selection->slots, *G.g);
        front::afterSlotChange();
        RefreshControls();
        return;
    }
    Select(tx, ty);
}

// Put the selection down (control 178, 8065:0f26).
static void Deselect() {
    G.selection.reset();
    G.route.reset();
    G.walk.reset();
}

// ------------------------------------------------------------------ the turn

static void AfterBattle(const w2::Battle& result) {
    tutorial::show("fresult");
    if (G.selection) {
        std::vector<Army*> alive;
        for (int i = 0; i < G.selection->slots.n; i++) {
            Army* a = G.selection->slots.army[i];
            if (!result.deadByArmy.count(a)) alive.push_back(a);
        }
        Reslot(alive);
    }
}

// "War!" -- asked before attacking a side not at war (STRING.DAT group 140)
static const int kWarGroup = 0x8c;

// A click with the sword or the dove (740d:0179): attack_tile (67cc:0000)
// fights for the tile beside the stack. There is no walk, so the step's
// cost does not matter: one move left will do. A side at peace -- or, for
// a city, one not at war -- must be declared on first.
static void AttackTile(int x, int y) {
    if (!G.selection || G.selection->stack.empty()) return;
    auto& sel = *G.selection;
    w2::Game& g = *G.g;
    int me = G.player->index;
    auto here = game::armiesAt(g, x, y);
    City* city = game::cityAt(g, x, y);
    if (city && city->razed) city = nullptr;
    int owner = !here.empty() ? here[0]->owner : (city ? city->ownerIndex : NONE);
    if (w2::diplomacy::mustDeclare(g, me, owner, city)) {
        input::Options o;
        o.title = kit::text(kWarGroup, 0);
        for (int i = 1; i <= 4; i++) o.lines.push_back(kit::text(kWarGroup, i));
        o.confirm = true;
        o.ok = [x, y, me, owner](const std::string&) {  // 67cc:20a8
            w2::diplomacy::attackAnyway(*G.g, me, owner);
            AttackTile(x, y);
        };
        input::open(o);
        return;
    }
    if (move::stackMoves(sel.stack) < 1) {
        Deselect();
        return;
    }  // 1b62:08b3
    if (owner == me || (!city && here.empty())) return;
    OrderTo(sel.stack, NONE, NONE);
    G.route.reset();
    game::chargeAttack(g, sel.stack, x, y);
    // decided first, shown, and only then taken effect (67cc:0a6b)
    w2::Battle result = game::decideAttack(g, sel.stack, x, y);
    StartAssault(x, y, result);
    G.assault->after = [result]() { AfterBattle(result); };
}

// Walk the selection towards (x, y). Running into an enemy just stops the
// walk; attacking is the sword's (attackTile).
static void MoveSelection(int x, int y) {
    if (!G.selection) return;
    auto& sel = *G.selection;
    OrderTo(sel.stack, x, y);
    G.route = move::preview(*G.g, sel.stack, x, y);
    auto r = move::moveTo(*G.g, sel.stack, x, y);
    if (r.stopped == "attack") r.stopped = "blocked";
    if (r.stopped == "no route") {
        say("There is no way there.");
        sound::effect("chord");  // 1a8b:0c4f, 1c8c:0007
    } else if (r.steps == 0) {
        say("They cannot move: " + r.stopped + ".");
    } else {
        auto walked = sel.stack;
        sel.x = sel.stack[0]->x;
        sel.y = sel.stack[0]->y;
        Reslot(SelectableAt(sel.x, sel.y));
        front::stratDirty();
        StartWalk(walked, r.walked, r.seaBefore);
    }
    // a walk that leaves the stack out of moves (walk_path's status 2) ends
    // its turn: done for the turn and put down (1c8c:01fd) -- once the walk
    // has played out, as the original draws every step first (1a8b:04c8)
    if (r.outOfMoves && G.selection) {
        if (G.walk)
            G.walk->putDown = G.selection->stack;
        else
            PutDown(G.selection->stack);
    }
    if (G.selection && !G.selection->stack.empty()) {
        G.selection->x = G.selection->stack[0]->x;
        G.selection->y = G.selection->stack[0]->y;
        if (!G.walk) centreOn(G.selection->x, G.selection->y);
    }
    if (!G.walk) RefreshRoute();
}

// ------------------------------------------------------------------ the pointer

// 18a9:0896 decides the mouse pointer from what is under it and what is
// selected, and a click on the map does whatever that pointer promises
// (740d:0037). STAND.PCK holds the twelve, with hotspots at 4125:045c.
enum Ptr {
    kArrow,
    kView,
    kBoat,
    kCityP,
    kHand,
    kSelect,
    kWalk,
    kSiteP,
    kAttack,
    kAdvise,
    kPeace,
    kAlt
};
static const int kPtrHot[12] = {0, 6, 8, 6, 8, 8, 8, 8, 0, 8, 8, 0};

static int PointerKind(int x, int y) {
    if (G.starting || G.over || G.banner || G.offer || G.assault || G.victory || G.aiRun ||
        G.openMenu >= 0 || kit::top() || !G.player || G.player->computer) {
        return kArrow;
    }
    if (G.drag) return kHand;  // 4125:1176, while it drags
    bool alt = Held({"lalt", "ralt"}), ctrl = Held({"lctrl", "rctrl"});
    if (G.stratRect.contains(x, y)) return alt ? kAlt : kView;
    auto tile = TileAtPoint(x, y);
    if (!tile) return G.panelRect.contains(x, y) ? kHand : kArrow;
    auto [tx, ty] = *tile;
    w2::Game& g = *G.g;
    int me = G.player->index;
    if (tx < 0 || ty < 0 || tx >= g.map->width || ty >= g.map->height) return kHand;
    auto here = game::armiesAt(g, tx, ty);
    City* city = game::cityAt(g, tx, ty);
    bool armies = !here.empty();
    // the tile's owner nibble: whoever stands there, else the city's, else none
    int owner = armies ? here[0]->owner : NONE;
    if (owner == NONE) owner = city ? city->ownerIndex : NONE;
    if (owner == NONE) owner = w2::rules::NEUTRAL;
    int t = city ? (city->razed ? move::SITE : move::CITY) : w2::scn::terrainAt(*g.map, tx, ty);
    const std::vector<Army*>* sel =
            G.selection && !G.selection->stack.empty() ? &G.selection->stack : nullptr;
    int dist = 0, mode = NONE, under = move::PLAIN;
    bool at_sea = false;
    if (sel) {
        int sx = (*sel)[0]->x, sy = (*sel)[0]->y;
        if (city && !city->razed) {
            int dx = std::max(0, std::max(city->x - sx, sx - (city->x + 1)));
            int dy = std::max(0, std::max(city->y - sy, sy - (city->y + 1)));
            dist = std::max(dx, dy);
        } else {
            dist = std::max(std::abs(tx - sx), std::abs(ty - sy));
        }
        mode = move::modeOf(g, *sel);
        under = w2::scn::terrainAt(*g.map, sx, sy);
        at_sea = (*sel)[0]->atSea;
    }
    // any tile with a road on it costs 1, a mountain pass too (18a9:0896)
    bool road = w2::scn::roadAt(*g.map, tx, ty) % 0x20 != 0;
    int cost = (sel && mode != move::FLYING && !road) ? move::COST[t] : 1;
    if (!game::seen(g, me, tx, ty) || cost <= 0) return kHand;
    auto walk = [&]() {
        return (mode != move::FLYING && (t == move::WATER || t == move::SHORE)) ? kBoat : kWalk;
    };
    auto look = [&]() {
        if (t == move::CITY) return kCityP;
        if (t == move::SITE) return kSiteP;
        return kHand;
    };
    static const std::set<int> kPtrOk = {move::ROAD, move::BRIDGE, move::WATER, move::SHORE,
                                         move::CITY};
    bool enemy_near = sel && dist <= 1 && owner != me && (t == move::CITY || armies);
    if (Held({"lshift", "rshift"})) {
        if (enemy_near && g.map->options.militaryAdvisor != 0) return kAdvise;
        return look();
    }
    if (armies && owner == me && !ctrl) {
        if (alt) return kAlt;
        if (!sel || dist < 1) return kSelect;
        return walk();
    }
    if (enemy_near && !(at_sea && mode == move::LAND && under == move::SHORE && !kPtrOk.count(t)) &&
        !(!at_sea && mode == move::LAND && t == move::SHORE && !kPtrOk.count(under))) {
        if (g.map->options.diplomacy == 0 || owner == w2::rules::NEUTRAL) return kAttack;
        int st = w2::diplomacy::state(g, me, owner);
        if (t == move::CITY) return st == w2::diplomacy::WAR ? kAttack : kPeace;
        return st != w2::diplomacy::PEACE ? kAttack : kPeace;
    }
    if (sel && (owner == me || (owner == w2::rules::NEUTRAL && t != move::CITY))) {
        if (alt) return kAlt;
        if (dist >= 1 && ctrl && armies) return kSelect;
        return walk();
    }
    return look();
}

// Not the original's: over the black round the frame the pointer is the
// arrow, drawn where the mouse really is rather than held to the frame's edge.
static void DrawPointer() {
    if (display::pointerAway()) return;
    int k = display::d.off ? kArrow : PointerKind(G.mouseX, G.mouseY);
    auto [x, y] = display::d.off ? *display::d.off : std::make_pair(G.mouseX, G.mouseY);
    gfx::setColor(1, 1, 1);
    gfx::draw(G.pointerImg, gfx::newQuad(k * 16, 0, 16, 16), x - kPtrHot[k], y - kPtrHot[k]);
}

// The start-of-turn banner (8cc6:0259): popup 6, (160, 60) 320x312 --
// exactly CITY.PCK -- framed in the side's own colour, with the side's name
// and "Turn %d" over it. 7ecb:0142 then blocks until any input arrives.
static const kit::Rect kBanner{160, 60, 320, 312};
static const int kBannerNameY = 85, kBannerTurnY = 130;

static void ShowBanner(Side* side) {
    G.banner = Banner{side->name, G.g->turn, side->colour, side->edge};
    sound::effect("turn");  // the fanfare, TURN.8SN
}

// The hero offer is popup 2 -- (80, 60) 480x312, MARBLE.PCK cropped -- with
// dialog 12's controls laid on it, every number from auto_ui_hero_emerges
// (6563:0d5c).
static const kit::Rect kHeroPopup{80, 60, 480, 312};
static const int kHeroDialog = 12;
static const int kHeroMapX = 80, kHeroMapY = 60;
static const int kHeroPicX = 320, kHeroPicY = 110, kHeroPicW = 224, kHeroPicH = 170;
static const int kHeroTitleY = 63, kHeroCentre = 432;
static const int kHeroLineY[4] = {190, 210, 230, 250};
static const int kMaleLabel[2] = {376, 315}, kFemaleLabel[2] = {480, 315};
static const int kHeroOk = 287, kHeroCancel = 288, kHeroField = 289, kHeroMale = 290,
                 kHeroFemale = 291;
static const int kHeroTitleGroup = 0x5f, kHeroLineGroup = 0x61, kHeroSexGroup = 0x62;
// what a hired hero's allies are announced with (STRING.DAT group 96)
static const int kAllyGroup = 0x60;
static const size_t kHeroNameMax = 19;

static bool PresentOffer(Side* side);
static void AfterOffer();
// the name field clicked: the Steam Deck's on-screen keyboard comes up for it
static bool offer_typing = false;

// Where the map is hidden and more than one human plays, the main map and the
// strategic map stay blank as a human's turn opens, until the banner is
// clicked away: each side has its own hidden map, and whoever is still at the
// screen is not to see the next one's. The original shares one seen bit among
// its humans and only holds back the re-centring (8cc6:0259).
static bool HotseatHidden() {
    if (G.g->map->options.hiddenMap == 0) return false;
    int humans = 0;
    for (auto& s : G.g->map->sides)
        if (s.inUse && s.alive && !s.computer) humans++;
    return humans > 1;
}

// Close the banner, and only then let the turn's first dialog through.
static void DismissBanner() {
    G.banner.reset();
    if (G.mapAfterBanner) centreOn(CapOf(G.player).first, CapOf(G.player).second);
    G.mapAfterBanner = false;
    // whoever is left on the water without a boat drowns (8cc6:0259), and only
    // then does the advisor have his say (6dda:026f(5)); 8cc6:04bd then puts on
    // the hero's music or the turn's
    DrownStranded(NONE, NONE, AfterBanner);
}

static void AfterBanner() {
    int says = sound::speechOn() ? w2::cues::advisor(*G.g, *G.player, front::roll) : NONE;
    // then 8cc6:04bd: the heroes promoted, each in its own popup, and the hero
    // offer with its music, or the turn's
    advisor::say(says, []() {
        auto promoted = G.player->promoted;
        G.player->promoted.clear();
        honours::promotions(promoted, []() {
            sound::music(G.player->heroOffer ? w2::cues::HERO : w2::cues::PLAY);
            std::vector<std::string> moments;
            if (G.g->turn == 2) moments.push_back("turn2");
            if (G.player->heroOffer) moments.push_back("hero");
            tutorial::chain(moments, []() {
                if (!PresentOffer(G.player)) AfterOffer();
            });
        });
    });
}

// The four lines over the picture, as 6563:0d5c assembles them: strings 0-3
// for the free hero of turn 1, 4-7 for one that asks a price, and the same
// again from 8 for a heroine.
static std::vector<std::string> HeroLines(const w2::HeroOffer& offer, bool female) {
    int first = female ? 8 : 0;
    auto s = [first](int i) { return kit::text(kHeroLineGroup, first + i); };
    if (offer.first) return {s(0), s(1), s(2), format(s(3), offer.city->name)};
    return {format(s(4), offer.city->name), format(s(5), offer.price), format(s(6), G.player->gold),
            s(7)};
}

static bool PresentOffer(Side* side) {
    if (!side->heroOffer) return false;
    if (!G.heroView) G.heroView = screen::dialog(*G.screen, kHeroDialog);
    G.offer = side->heroOffer;
    // the name and sex were rolled with the offer; the checkboxes only change
    // the wording and the picture
    G.offerFemale = G.offer->female;
    G.offerName = G.offer->name.empty() ? "Hero" : G.offer->name;
    G.heroView->state[kHeroCancel] = G.offer->first ? DISABLED : NORMAL;
    G.heroView->state[kHeroOk] = NORMAL;
    return true;
}

// Take the offer: the hero joins under the name and sex now in the dialog.
static void AcceptOffer() {
    auto offer = G.offer;
    offer->name = G.offerName;
    offer->female = G.offerFemale;
    auto hired = w2::hero::recruit(*G.g, *G.player, *offer);
    Army* h = hired.hero;
    G.offer.reset();
    G.player->heroOffer.reset();
    centreOn(h->x, h->y);
    if (hired.count > 0) {
        // "And the hero brings 1 ally!" / "... %d allies!" (hero_brings_allies,
        // 7563:01fc, through 8065:10fb): the number rolled, placed or not
        std::string line = hired.count == 1 ? kit::text(kAllyGroup, 0)
                                            : format(kit::text(kAllyGroup, 1), hired.count);
        search::say(line, []() { AfterOffer(); });
        return;
    }
    AfterOffer();
}

// The turn's opening, once any hero offer is settled (8cc6:04bd(1)): the
// cities build -- or on turn 1 the capital's dialog opens in Production.
static void AfterOffer() {
    if (G.player->computer) return;
    game::humanProduction(*G.g, *G.player);
    front::stratDirty();
    auto cap = CapOf(G.player);
    City* city = game::cityAt(*G.g, cap.first, cap.second);
    if (G.g->turn == 1 && city) {
        city::open(city, city::PRODUCTION);
        return;
    }
    if (G.modernUi && !G.player->missives.empty()) {
        diplomacyui::pollMissives();
    } else {
        G.player->missives.clear();
    }
}

static void RefuseOffer() {
    G.offer.reset();
    G.player->heroOffer.reset();
    AfterOffer();
}

// ------------------------------------------------------------ the computer

// The computer's turns run on a coroutine (the Lua remake's), so the screen
// keeps being drawn while they are played: it gives a frame back after every
// side's turn, and stops after each walk worth showing. A turn is shown when
// its side is observed or no human is left (8cc6:0000); a walk only if the
// player has seen any of it. Holding Shift or Alt between turns opens
// Settings (5db9:045e); any other key or a click runs the rest of the round
// through unwatched.

static Coroutine* ai_co = nullptr;  // the turn being played, from inside it
static Side* ai_result = nullptr;   // where the round ended

static bool HumansLeft() {
    return G.g && game::hasActiveHuman(*G.g);
}

static bool ShownSide(const Side* side) {
    return side && (side->observe || !HumansLeft());
}

// A line in the status bar, and the time 8065:11d1 then waits, in BIOS ticks
static void Status(const std::string& text, int ticks) {
    if (G.aiStatus) G.aiStatus->text = text;
    if (ticks > 0 && !G.aiSkip && ai_co) {
        Yield y;
        y.what = "pause";
        y.ticks = ticks;
        ai_co->yield(y);
    }
}

// ai_turn's progress bar after each phase (5db9:0000)
static int ProgressOf(const std::string& phase) {
    static const std::map<std::string, int> kP = {
        {"diplomacy", 10},  {"move hero", 15}, {"move search", 20}, {"move explore", 25},
        {"assault", 30},    {"move #1", 40},   {"rescue", 45},      {"evaluate", 50},
        {"clean city", 55}, {"neutral", 60},   {"missions", 60},    {"move #2", 65},
        {"assault XX", 70}, {"specials", 75},  {"rebuilding", 80},  {"last rescue", 85},
        {"production", 90}, {"economy", 92},   {"vectoring", 95}};
    auto it = kP.find(phase);
    return it == kP.end() ? (G.aiStatus ? G.aiStatus->progress : 0) : it->second;
}

static void InstallHooks() {
    w2::ai::hooks.onWalk = [](w2::Game& g, const std::vector<Army*>& stack,
                              const move::WalkResult& r) {
        if (!ai_co || G.aiSkip || !ShownSide(G.aiSide)) return;
        bool seen = !HumansLeft();
        for (auto& t : r.walked) {
            if (seen) break;
            if (game::seen(g, G.player->index, t.first, t.second)) seen = true;
        }
        if (seen) {
            Yield y;
            y.what = "walk";
            y.armies = stack;
            y.tiles = r.walked;
            y.seaBefore = r.seaBefore;
            ai_co->yield(y);
        }
    };
    // A computer's battle (auto_ui_being_attacked, 67cc:124a): who is attacked
    // goes up in the status bar, and then how it went, five ticks each -- when
    // the attacker's turn is watched or the defender is a human.
    w2::ai::hooks.onFight = [](w2::Game& g, const std::vector<Army*>&, int x, int y,
                               w2::Battle& result) {
        if (!ai_co || !G.aiStatus) return;
        int def = NONE;
        if (result.lines.city)
            def = result.lines.city->ownerIndex;
        else if (!result.lines.defenders.empty())
            def = result.lines.defenders[0]->owner;
        Side* def_side = def != NONE && def < 8 ? &g.map->sides[def] : nullptr;
        bool human = def_side && !def_side->computer;
        if (!(ShownSide(G.aiSide) || human)) return;
        std::vector<Side*> humans;
        for (Side* s : g.sides)
            if (s->alive && !s->computer) humans.push_back(s);
        if (g.map->options.hiddenMap != 0 && humans.size() == 1 &&
            !game::seen(g, humans[0]->index, x, y))
            return;
        auto t = [](int grp) { return kit::text(grp, 0); };
        Army* hero_a = nullptr;
        for (Army* a : result.lines.attackers)
            if (a->hero()) {
                hero_a = a;
                break;
            }
        std::string me = G.aiSide->name;
        std::string outcome;
        if (!result.won)
            outcome = format(t(0x98), me);
        else if (hero_a)
            outcome = format(t(0x8e), hero_a->name);
        else
            outcome = format(t(0x96), me);
        bool hidden_many = g.map->options.hiddenMap != 0 && humans.size() > 1;
        bool window = !hidden_many && ((ShownSide(G.aiSide) && def_side) || human);
        bool cloud = !hidden_many;
        // 8961:0227 slides the view box to the battle and centres the map on it,
        // Observe or not -- a human attacked gets to see where
        if (!hidden_many) {
            centreOn(x, y);
            front::stratDirty();
        }
        Status(def_side ? format(t(0x94), def_side->name) : t(0x95), 5);
        if (cloud && !G.aiSkip) {
            StartAssault(x, y, result);
            G.assault->computer = true;
            G.assault->window = window;
            G.assault->outcome = outcome;
            G.assault->message = {outcome};
            Yield yl;
            yl.what = "assault";
            ai_co->yield(yl);
            if (window) return;
        }
        Status(outcome, 5);
    };
    w2::ai::hooks.onSpoils = nullptr;
}

static void ComputerTurns(Coroutine& co, Side* side) {
    ai_co = &co;
    auto yield_what = [&co](const char* what) {
        Yield y;
        y.what = what;
        co.yield(y);
    };
    // What the round's end found, as the original tells it inside the
    // computer's turns: each side put out in a box, or -- with no human
    // playing -- in the status bar for 100 ticks (8065:18ab), then the last
    // human's fall in two boxes (8065:1c6f). The rest waits for the turn that
    // follows (ui/ending.cpp).
    auto announce = [&co, &yield_what]() {
        w2::Ending& e = G.g->ending;
        if (e.told) return;
        e.told = true;
        auto fallen = e.fallen;
        bool no_humans = e.noHumans;
        for (auto& f : fallen) {
            std::string text = ending::fallenText(f);
            if (f.boxed) {
                Yield y;
                y.what = "fallen";
                y.text = text;
                co.yield(y);
            } else {
                Status(text, 100);
            }
        }
        if (no_humans) yield_what("nohumans");
    };
    announce();  // a human's turn may have ended the round
    while (side && side->computer) {
        G.aiSide = side;
        // 8065:2123: each computer turn opens with a song that plays once
        sound::music(w2::cues::COMPUTER, G.g->sides);
        G.aiStatus = AiStatus{side, side->name, 5};
        yield_what("progress");
        if (ShownSide(side)) {
            auto news = side->diploNews;
            for (auto& m : news) Status(m, 20);
        }
        side->diploNews.clear();
        w2::ai::playTurn(*G.g, *side, [&](const char* phase) {
            G.aiStatus->progress = ProgressOf(phase);
            yield_what("progress");
        });
        G.aiStatus->progress = 100;
        yield_what("progress");
        side = game::endTurn(*G.g);
        announce();
        yield_what("turn");
    }
    ai_result = side;
}

static void ResumeComputer() {
    if (!G.aiRun) return;
    G.aiWait = false;
    bool more = G.aiRun->resume();
    if (!more) {
        G.aiRun.reset();
        ai_co = nullptr;
        G.aiSkip = false;
        G.aiSide = nullptr;
        FinishRound(ai_result);
        return;
    }
    front::stratDirty();
    const Yield& y = G.aiRun->value();
    if (y.what == "pause") {
        G.aiResumeAt = w2::now() + y.ticks / 18.2;
        G.aiWait = true;
    } else if (y.what == "walk") {
        StartWalk(y.armies, y.tiles, y.seaBefore);
        if (G.walk)
            G.walk->computer = true;
        else
            G.aiWait = true;
    } else if (y.what == "nohumans") {
        // 8065:1c6f: the last human is gone, and the war goes on
        auto t = [](int i) { return kit::text(0xd, i); };
        search::message(t(0), t(1),
                        [t]() { search::message(t(2), t(3), []() { G.aiWait = true; }); });
    } else if (y.what == "fallen") {
        search::say(y.text, []() { G.aiWait = true; });  // 8065:10fb
    } else {
        // a turn done, or a battle up: go on at the next frame free of it
        G.aiWait = true;
    }
}

// Called every frame: carry the computer on once nothing is in its way.
static void StepComputer() {
    if (!G.aiRun || !G.aiWait || G.walk || G.assault || kit::top() || G.openMenu >= 0) return;
    if (G.aiResumeAt > 0) {
        if (w2::now() < G.aiResumeAt && !G.aiSkip) return;
        G.aiResumeAt = 0;
    }
    // only a Shift or Alt pressed since the round began: the Alt of Alt-E
    // is still down as the computer starts
    bool mods = Held({"lshift", "rshift"}) || Held({"lalt", "ralt"});
    if (!mods) G.aiModsHeld = false;
    if (mods && !G.aiModsHeld) {
        G.aiModsHeld = true;
        G.aiSkip = false;
        settings::open();
        return;
    }
    // several steps a frame while running through unwatched
    double until = w2::now() + 0.03;
    do {
        ResumeComputer();
    } while (G.aiSkip && G.aiRun && G.aiWait && !G.walk && !G.assault && !kit::top() &&
             G.aiResumeAt == 0 && w2::now() < until);
}

// Play computer sides from `side` on, until a human's turn or the end.
static void PlayComputers(Side* side) {
    G.aiModsHeld = Held({"lshift", "rshift"}) || Held({"lalt", "ralt"});
    ai_result = nullptr;
    G.aiRun = std::make_unique<Coroutine>([side](Coroutine& co) { ComputerTurns(co, side); });
    ResumeComputer();
}
void front::playComputer() {
    PlayComputers(G.g->side);
}

static void EndTurn() {
    // End Turn (8065:2074) does nothing once the game is won: the winner stays
    // on the turn, to look the kingdom over
    if (G.g->won) return;
    // It puts the stack down, and its route with it: the next player sees no
    // other side's plans
    G.selection.reset();
    G.route.reset();
    Side* side = game::endTurn(*G.g);
    if (side && side->computer) return PlayComputers(side);
    FinishRound(side);
}

// The round is over: the next human side takes the keyboard.
static void FinishRound(Side* side) {
    if (!side) {
        G.aiStatus.reset();
        G.over = true;
        ending::over(G.g->ending);
        return;
    }
    G.player = side;
    G.aiStatus.reset();  // 8065:0aeb: the bar is the player's again
    front::stratDirty();
    CycleReset();
    // 8cc6:0259 centres on the side's capital -- but where the map is hidden
    // and more than one human plays, only once the banner is gone, and until
    // then neither map is shown
    G.mapAfterBanner = HotseatHidden();
    if (!G.mapAfterBanner) centreOn(CapOf(side).first, CapOf(side).second);
    ending::show(G.g->ending, [side]() { ShowBanner(side); });
}

void front::openCity(City* c) {
    city::open(c);
}
void front::openQuest() {
    questui::open();
}

void front::takeLoaded(std::unique_ptr<w2::Game> loaded) {
    G.g = std::move(loaded);
    if (opt_military_advisor >= 0) {
        G.g->map->options.militaryAdvisor = opt_military_advisor;
    } else if (G.g->map->options.militaryAdvisor == 0) {
        G.g->map->options.militaryAdvisor = 1;
    }
    G.selection.reset();
    G.route.reset();
    G.walk.reset();
    G.over = false;
    G.starting = false;
    G.mapAfterBanner = false;
    front::stratDirty();
    G.player = G.g->sides[G.g->current];
    centreOn(CapOf(G.player).first, CapOf(G.player).second);
    sound::music(w2::cues::PLAY);  // 7721:02d3
}

// ------------------------------------------------------------------ drawing

static Army* TopArmy(const std::vector<Army*>& stack) {
    Army* best = nullptr;
    int best_rank = NONE;
    for (Army* a : stack) {
        int rank = G.g->map->fightOrder[OwnerOr8(a->owner)][a->type];
        if (best_rank == NONE || rank > best_rank) {
            best = a;
            best_rank = rank;
        }
    }
    return best;
}

// The figure a stack shows (8611:2985): its top army's, or a boat -- the
// Navy, type 5 -- when an army in it is at sea.
static const int kNavy = 5;
static int StackFigure(const std::vector<Army*>& stack) {
    for (Army* a : stack)
        if (a->atSea) return kNavy;
    Army* a = TopArmy(stack);
    return a ? a->type : NONE;
}

// The hidden map (8611:24c2): over each unseen tile, black through its
// HIDDEN.PCK cell; cell 14, a tile with nothing seen round it, is black.
static void DrawFog() {
    int viewer = game::viewerSide(*G.g, G.player);
    if (G.g->map->options.hiddenMap == 0 || viewer == NONE) return;
    for (int my = G.view[1]; my <= G.view[3]; my++) {
        for (int mx = G.view[0]; mx <= G.view[2]; mx++) {
            int cell = game::fogCell(*G.g, viewer, mx, my);
            if (cell == NONE) continue;
            int sx = mx * kTile, sy = my * kTile;
            if (cell == game::FOG_BLACK) {
                gfx::setColor(0, 0, 0);
                gfx::rectangle(gfx::FILL, sx, sy, kTile, kTile);
            } else {
                gfx::setColor(1, 1, 1);
                gfx::draw(G.fogImg, gfx::newQuad((cell % 7) * 48, (cell / 7) * 41, 40, 40), sx, sy);
            }
        }
    }
    gfx::setColor(1, 1, 1);
}

// A stack on the map (8611:0335): the top army's figure at (8, 7) in the
// tile; the flag pole, three 40-pixel lines in colours 14, 13 and 14; and
// the flag, from the side's own sheet at (464, y) 48 x 8, bigger the more
// armies stand there (4125:2d4e). More than four fly the smallest 8 lower
// as well, and the flag for the rest at the top.
static const int kFlagY[4] = {29, 38, 47, 56};
static void DrawStack(int owner, int army_type, int count, int sx, int sy) {
    if (owner < 0 || owner > 8) owner = 8;
    gfx::setColor(1, 1, 1);
    gfx::draw(G.armyImg[owner], kit::armyQuad(army_type), sx + 8, sy + 7);
    PalColour(14);
    gfx::rectangle(gfx::FILL, sx + 2, sy, 1, kTile);
    gfx::rectangle(gfx::FILL, sx + 4, sy, 1, kTile);
    PalColour(13);
    gfx::rectangle(gfx::FILL, sx + 3, sy, 1, kTile);
    gfx::setColor(1, 1, 1);
    auto flag = [&](int k, int y) {
        gfx::draw(G.armyImg[owner], gfx::newQuad(464, kFlagY[k - 1], 48, 8), sx, y);
    };
    if (count > 4) {
        flag(1, sy + 8);
        count -= 4;
    }
    flag(std::max(1, std::min(count, 4)), sy);
}

// The map, in map pixels: the caller has the transform and the scissor up.
static void DrawMap() {
    G.view = ViewTiles();
    const auto& v = G.view;
    std::map<int, std::vector<Army*>> armies_on;
    for (Army* a : G.g->armies) {
        if (!a->transit && a->x != NONE && a->x >= v[0] && a->x <= v[2] && a->y >= v[1] &&
            a->y <= v[3])
            armies_on[a->x + a->y * 1000].push_back(a);
    }
    // one item a tile: the last in item order (8611:2d7c walks them from the end)
    std::map<int, w2::Item*> items_on_tile;
    for (int i = static_cast<int>(G.g->map->items.size()) - 1; i >= 0; i--) {
        w2::Item& it = G.g->map->items[i];
        if (it.status == 1 && it.x != NONE && !items_on_tile.count(it.x + it.y * 1000))
            items_on_tile[it.x + it.y * 1000] = &it;
    }
    struct Standing {
        std::vector<Army*> stack;
        int x, y, sx, sy;
        bool hidden = false;
    };
    std::vector<Standing> standing;
    for (int my = v[1]; my <= v[3]; my++) {
        for (int mx = v[0]; mx <= v[2]; mx++) {
            int sx = mx * kTile, sy = my * kTile;
            int t = w2::scn::tileAt(*G.g->map, mx, my);
            int sheet = std::min(1, t / 96);
            gfx::setColor(1, 1, 1);
            gfx::draw(G.sheets[sheet], TileQuad(t), sx, sy);
            int rd = w2::scn::roadAt(*G.g->map, mx, my);
            if (rd != 0) gfx::draw(G.roadImg, RoadQuad(rd - 1), sx, sy);
            // a port -- a crossing, map word bit 15 -- takes the anchor, at the
            // tile's corner after the roads (8611:1a79)
            if (w2::scn::isCrossing(*G.g->map, mx, my))
                gfx::draw(G.atransShields, gfx::newQuad(0, 0, 32, 29), sx, sy);
            // items on the ground (8611:2d7c): a planted standard as its side's
            // flag, army cell 29; anything else as the bag
            auto here = items_on_tile.find(mx + my * 1000);
            if (here != items_on_tile.end()) {
                w2::Item* it = here->second;
                if (it->planted && it->standardOf != NONE)
                    gfx::draw(G.armyImg[it->standardOf], kit::armyQuad(29), sx, sy);
                else
                    gfx::draw(G.atransShields, gfx::newQuad(64, 0, 32, 29), sx, sy);
            }
            auto on = armies_on.find(mx + my * 1000);
            if (on != armies_on.end()) {
                std::vector<Army*> stack;
                for (Army* a : on->second)
                    if (!(G.walk && G.walk->armies.count(a))) stack.push_back(a);
                if (!stack.empty()) standing.push_back(Standing{stack, mx, my, sx, sy});
            }
        }
    }
    // the ground first, then the encampments, then the stacks (8611:1a79)
    for (auto& s : standing) {
        if (game::towerAt(*G.g, s.x, s.y)) {
            int owner = OwnerOr8(s.stack[0]->owner);
            if (owner == 15) owner = 8;
            gfx::setColor(1, 1, 1);
            gfx::draw(G.roadImg, gfx::newQuad(owner * 48 + 192, 40, 48, 40), s.sx, s.sy);
            s.hidden = !(G.selection && G.selection->x == s.x && G.selection->y == s.y);
        }
    }
    // the selected stack's tile shows the lead of the group that moves
    Army* lead = G.selection && !G.selection->stack.empty() ? TopArmy(G.selection->stack) : nullptr;
    if (lead && G.walk && G.walk->armies.count(lead)) lead = nullptr;
    for (auto& s : standing) {
        if (s.hidden) continue;
        Army* a = TopArmy(s.stack);
        int figure = StackFigure(s.stack);
        if (lead && G.selection->x == s.x && G.selection->y == s.y) {
            a = lead;
            figure = lead->atSea ? kNavy : lead->type;
        }
        DrawStack(OwnerOr8(a->owner), figure, static_cast<int>(s.stack.size()), s.sx, s.sy);
    }
    DrawFog();
    DrawRoute();
    // the stack itself, wherever the walk has got to
    const auto* at = WalkingAt();
    if (at) {
        Army* a = G.walk->top;
        if (a && Shown(at->first, at->second)) {
            std::vector<Army*> walking(G.walk->armies.begin(), G.walk->armies.end());
            Army* top = TopArmy(walking);
            int figure = G.walk->seaBefore ? kNavy : (top ? top->type : NONE);
            DrawStack(OwnerOr8(a->owner), figure, static_cast<int>(walking.size()),
                      at->first * kTile, at->second * kTile);
        }
    }
    // the selection box, which follows the walk
    if (G.selection) {
        int x = at ? at->first : G.selection->x, y = at ? at->second : G.selection->y;
        if (Shown(x, y)) {
            gfx::setColor(1, 1, 1);
            int base = G.selection->stack.size() > 1 ? 4 : 0;
            int frame = base + static_cast<int>(std::floor(w2::now() * kCursRate)) % 4;
            gfx::draw(G.cursImg, gfx::newQuad((frame % 4) * 64, (frame / 4) * 40, 40, 40),
                      x * kTile, y * kTile);
        }
    }
}

static int strat_viewer = -99;

void front::drawStrategicMap(int x, int y, const City* mark, bool no_cities,
                             const std::vector<int>* owners, int scroll, int rows) {
    int viewer = game::viewerSide(*G.g, G.player);
    if (!G.stratImage || strat_viewer != viewer) {
        G.stratImage = screen::strategicImage(*G.screen, *G.g, viewer);
        strat_viewer = viewer;
    }
    gfx::setScissor(x, y, 224, rows);
    y -= scroll;
    gfx::setColor(1, 1, 1);
    gfx::draw(G.stratImage, x, y);
    if (!no_cities) {
        auto& cities = G.g->map->cities;
        for (size_t i = 0; i < cities.size(); i++) {
            const City& c = cities[i];
            int owner = owners && i < owners->size() ? (*owners)[i] : NONE;
            bool gone = owners ? owner == 0xff : c.razed;
            if (!gone && game::seen(*G.g, viewer, c.x, c.y)) {
                int side = owners ? owner : OwnerOr8(c.ownerIndex);
                gfx::draw(G.atransShields, gfx::newQuad(side * 16, 30, 8, 8), x + c.x * 2 - 1,
                          y + c.y * 2 - 1);
            }
        }
    }
    if (mark) {
        int mx = x + mark->x * 2 - 1, my = y + mark->y * 2 - 1;
        PalColour(15);
        gfx::rectangle(gfx::FILL, mx - 1, my - 1, 10, 1);
        gfx::rectangle(gfx::FILL, mx - 1, my + 9, 10, 1);
        gfx::rectangle(gfx::FILL, mx - 1, my - 1, 1, 10);
        gfx::rectangle(gfx::FILL, mx + 9, my - 1, 1, 10);
    }
    // Highlight any Allied partner's broadcasted war target city on the strategic map
    if (G.modernUi && !no_cities && G.player && viewer != NONE) {
        for (int s = 0; s < 8; s++) {
            if (s != G.player->index && w2::diplomacy::isAllied(*G.g, G.player->index, s)) {
                int tc = w2::diplomacy::alliedTargetCity(*G.g, s);
                if (const City* ac = G.g->map->city(tc)) {
                    if (!ac->razed && game::seen(*G.g, viewer, ac->x, ac->y)) {
                        int mx = x + ac->x * 2 - 1, my = y + ac->y * 2 - 1;
                        PalColour(11);
                        gfx::rectangle(gfx::FILL, mx - 2, my - 2, 12, 1);
                        gfx::rectangle(gfx::FILL, mx - 2, my + 10, 12, 1);
                        gfx::rectangle(gfx::FILL, mx - 2, my - 2, 1, 12);
                        gfx::rectangle(gfx::FILL, mx + 10, my - 2, 1, 12);
                    }
                }
            }
        }
    }
    gfx::setScissor();
}

void front::drawSiteMarkers(int x, int y, const w2::Site* highlight) {
    int viewer = game::viewerSide(*G.g, G.player);
    gfx::setScissor(x, y, 224, 312);
    for (auto& s : G.g->map->sites) {
        if (!(viewer == NONE ||
              (w2::site::shownTo(s, *G.player) && game::seen(*G.g, viewer, s.x, s.y))))
            continue;
        int sx, sy;
        if (s.content == w2::site::TEMPLE) {
            sx = 112;
            sy = 20;
        } else if (s.searched) {
            sx = 112;
            sy = 10;
        } else if (s.rich) {
            sx = 128;
            sy = 0;
        } else {
            sx = 112;
            sy = 0;
        }
        int mx = (s.x * 2 - 1 + 4) / 8 * 8;
        int my = s.y * 2 - 1;
        gfx::setColor(1, 1, 1);
        gfx::draw(G.atransShields, gfx::newQuad(sx, sy, 16, 10), x + mx, y + my);
        if (&s == highlight) {
            kit::setPal(15);
            kit::outline(x + mx, y + my, 12, 10);
        }
    }
    gfx::setScissor();
}

// Not the original's: on a short screen the strategic map's frame shows
// only some of it, scrolled to keep the view's box in the middle, so that a
// click towards an edge brings more of the world in from that side.
static int strat_scroll = 0;

static void FollowView() {
    const auto& r = G.stratRect;
    int full = G.g->map->height * 2;
    auto [vw, vh] = ViewSize();
    // a whole tile at a time
    int s = static_cast<int>(std::lround(G.cy + vh / 2 - r.h / 4.0)) * 2;
    strat_scroll = std::clamp(s, 0, std::max(0, full - r.h));
}

static void DrawStrategic() {
    const auto& r = G.stratRect;
    FollowView();
    front::drawStrategicMap(r.x, r.y, nullptr, false, nullptr, strat_scroll, r.h);
    gfx::setScissor(r.x, r.y, r.w, r.h);
    // the view box (8961:0698): whatever the view covers, to the device pixel
    PalColour(15);
    double s = display::d.scale;
    auto snap = [s](double v) { return std::floor(v * s + 0.5) / s; };
    auto [vw, vh] = ViewSize();
    double bx = r.x + snap(G.cx * 2), by = r.y - strat_scroll + snap(G.cy * 2);
    double w = snap(vw * 2), h = snap(vh * 2);
    gfx::rectangle(gfx::FILL, bx, by, w, 2);
    gfx::rectangle(gfx::FILL, bx, by + h - 2, w, 2);
    gfx::rectangle(gfx::FILL, bx, by, 2, h);
    gfx::rectangle(gfx::FILL, bx + w - 2, by, 2, h);
    gfx::setScissor();
}

void front::stratDirty() {
    G.stratImage.reset();
    strat_viewer = -99;
}

// The four configurable buttons are painted a second time from MENUBUTT.PCK
// at the rect the assigned item carries in UDB.DAT (545c:030a).
static void DrawShortcutIcons() {
    auto& ui = G.screen->ui;
    auto art = G.screen->artFor(w2::uidata::SHORTCUT_BITMAP);
    for (int i = 0; i < w2::uidata::SHORTCUT_COUNT; i++) {
        auto c = screen::control(*G.screen, w2::uidata::SHORTCUT_FIRST + i);
        auto sc = ui.shortcuts.find(i);
        if (!c || sc == ui.shortcuts.end()) continue;
        auto item = ui.shortcutItems.find(sc->second);
        if (item == ui.shortcutItems.end()) continue;
        if (art) {
            auto src = item->second.src[G.screen->stateOf(c->id)];
            gfx::setColor(1, 1, 1);
            gfx::draw(art->image, gfx::newQuad(src.x, src.y, item->second.w, item->second.h), c->x,
                      c->y);
        } else {
            std::string short_name = item->second.name.substr(0, 6);
            gfx::setColor(1, 1, 1);
            G.font->draw(short_name, c->x + std::max(1, (c->w - G.font->width(short_name)) / 2),
                         c->y + (c->h - G.font->lineHeight) / 2);
        }
    }
}

// Turn and the sides still in it, at the right of the bar (8cc6:0952).
static void DrawTurnStrip() {
    PalColour(15);
    gfx::rectangle(gfx::FILL, 437, 0, 202, 17);
    std::vector<Side*> alive;
    for (Side* s : G.g->sides)
        if (s->alive) alive.push_back(s);
    int n = static_cast<int>(alive.size());
    std::string turn = w2::fmt("Turn %d", G.g->turn);
    const Font& f = G.bigFont->colours(0, 15);
    f.draw(turn, (8 - n) * 16 + 508 - f.width(turn), 0);
    const Side* active_side = (G.aiSide && !HumansLeft()) ? G.aiSide : G.player;
    for (int k = 0; k < n; k++) {
        int x = (8 - n + k) * 16 + 512;
        if (alive[k] == active_side) {
            PalColour(0);
            gfx::rectangle(gfx::FILL, x - 3, 1, 17, 15);
        }
        int s = alive[k]->index;
        gfx::setColor(1, 1, 1);
        gfx::draw(G.atransShields, gfx::newQuad(112 + (s / 4) * 16, 94 + (s % 4) * 14, 16, 14), x,
                  2);
    }
}

// The menu bar (7ae8:02d8, 2372:132f): white, the titles in TEXT, black. An
// open title and the row under the pointer are XORed with colour 7; a greyed
// item's glyph is colour 3. The dropdown (2372:0803) is white with a black
// outline and a shadow.
static void DrawMenuBar() {
    PalColour(15);
    gfx::rectangle(gfx::FILL, 0, 0, G.layout.w, menu::BAR_H);
    for (size_t i = 0; i < G.menuLayout.size(); i++) {
        const auto& m = G.menuLayout[i];
        bool lit = static_cast<int>(i) == G.openMenu;
        if (lit) {
            PalColour(8);
            gfx::rectangle(gfx::FILL, m.x, 0, m.w, menu::BAR_H);
        }
        gfx::setColor(1, 1, 1);
        G.font->colours(lit ? 7 : 0, lit ? 8 : 15).draw(m.title, m.x + 2, menu::BAR_Y);
    }
    if (!G.starting && G.g) {
        gfx::push();
        gfx::translate(G.layout.ex, 0);
        DrawTurnStrip();
        gfx::pop();
    }
    if (G.openMenu < 0 || G.openMenu >= static_cast<int>(G.menuLayout.size())) return;
    const auto& d = G.menuLayout[G.openMenu].drop;
    PalColour(15);
    gfx::rectangle(gfx::FILL, d.x, d.y, d.w - 1, d.h - 1);
    PalColour(0);
    gfx::rectangle(gfx::FILL, d.x, d.y + d.h - 2, d.w - 2, 1);
    gfx::rectangle(gfx::FILL, d.x + 2, d.y + d.h - 1, d.w - 2, 1);
    gfx::rectangle(gfx::FILL, d.x, d.y, 1, d.h - 2);
    gfx::rectangle(gfx::FILL, d.x + d.w - 2, d.y, 1, d.h - 1);
    gfx::rectangle(gfx::FILL, d.x + d.w - 1, d.y + 1, 1, d.h - 1);
    const menu::Row* hover = menu::rowAt(G.menuLayout, G.openMenu, G.mouseX, G.mouseY);
    for (const auto& r : d.rows) {
        if (r.label == "-") {
            PalColour(0);
            gfx::rectangle(gfx::FILL, r.x + 1, r.y - 1, d.w - 2, 1);
            continue;
        }
        std::string k = r.key.empty() ? r.act : r.key;
        bool grey = !MenuEnabled(k);
        bool lit = &r == hover && !grey;
        if (lit) {
            PalColour(8);
            gfx::rectangle(gfx::FILL, r.x + 1, r.y, d.w - 3, r.h);
        }
        int glyph = grey ? 3 : (lit ? 7 : 0);
        const Font& f = G.font->colours(glyph, lit ? 8 : 15);
        gfx::setColor(1, 1, 1);
        f.draw(r.label, r.x + 3, r.y);
        if (!r.key.empty()) f.draw(r.key, r.x + d.keyCol, r.y);
        // the setting in use (not the original's): a diamond in the key column
        if (MenuTicked(k)) {
            PalColour(glyph);
            int cx = r.x + d.keyCol + 2, cy = r.y + r.h / 2;
            for (int j = -2; j <= 2; j++) {
                int half = 2 - std::abs(j);
                gfx::rectangle(gfx::FILL, cx - half, cy + j, half * 2 + 1, 1);
            }
        }
    }
}

// The bottom bar's eight army slots and the mark under each are real
// controls -- ids 224-231 the slots, 232-239 the marks, 240/241 the Grp
// button -- drawn from ABITS.PCK and the army sheets at the rects 89e0:0356
// and 8611:08be use (docs/re/ui.md > The army slots).
static const int kSlotFirst = 224, kBarFirst = 232, kSlotCount = 8;
static const int kGrpAll = 240, kGrpNone = 241;
static const int kSlotX = 24, kSlotY = 405, kSlotStep = 40;
static const int kSlotMovesX = 8, kSlotMovesY = 31;
static const int kMarkY = 449, kMarkW = 32, kMarkH = 16;

static void DrawAbits(int sx, int sy, int w, int h, int x, int y) {
    gfx::setColor(1, 1, 1);
    gfx::draw(G.abits, gfx::newQuad(sx, sy, w, h), x, y);
}

// A number in ABITS's own 8x8 digits, always two of them ("%02d").
static void DrawDigits(int n, int x, int y) {
    std::string text = w2::fmt("%02d", std::max(0, std::min(99, n)));
    for (size_t i = 0; i < text.size(); i++)
        DrawAbits(64 + (text[i] - '0') * 8, 30, 8, 8, x + static_cast<int>(i) * 8, y);
}

// The ring a slot's army sits in: each group a colour of its own, counted
// round from the current player's.
static int GroupRing(int group) {
    return (G.player->index + group) % 8 + 1;
}

static void DrawArmySlots() {
    const slots_mod::Slots* s = G.selection ? &G.selection->slots : nullptr;
    for (int i = 0; i < kSlotCount; i++) {
        int x = kSlotX + i * kSlotStep;
        Army* a = s && i < s->n ? s->army[i] : nullptr;
        gfx::setColor(1, 1, 1);
        int ring = a ? GroupRing(s->group[i]) : 0;
        gfx::draw(G.abits, gfx::newQuad(ring * 32, 0, 32, 30), x, kSlotY);
        if (a) {
            // the armies not moving with the group are drawn as ghosts
            gfx::draw(s->inGroup[i] ? G.armyImg[G.player->index] : G.shadowImg,
                      kit::armyQuad(a->type), x, kSlotY);
            DrawDigits(a->moves, x + kSlotMovesX, kSlotY + kSlotMovesY);
            if (s->mark[i] != slots_mod::NOMARK)
                DrawAbits(448, s->mark[i] == slots_mod::TICK ? 16 : 0, kMarkW, kMarkH, x, kMarkY);
        }
    }
    // "Group Move", the group's movement, and the Grp button (89e0:0567)
    DrawAbits(0, 30, 32, 8, 344, 420);
    DrawAbits(32, 30, 32, 8, 344, 428);
    DrawDigits(s ? slots_mod::moves(*s) : 0, 352, 436);
    bool green = s && slots_mod::grouped(*s);
    DrawAbits(288, green ? 19 : 0, 32, 19, 344, 447);
    // the way the moving group travels (89e0:070d)
    std::vector<Army*> moving;
    if (s)
        for (int i = 0; i < s->n; i++)
            if (s->inGroup[i]) moving.push_back(s->army[i]);
    int icon = NONE;
    if (!moving.empty()) {
        bool sea = true, woods = false, hills = false;
        for (Army* a : moving) {
            const w2::ArmyType* t = G.g->types.byId(a->type);
            if (t->woodsMove) woods = true;
            if (t->hillsMove) hills = true;
            if (!a->atSea) sea = false;
        }
        if (move::modeOf(*G.g, moving) == move::FLYING)
            icon = 184;
        else if (sea)
            icon = 424;
        else if (woods && hills)
            icon = 216;
        else if (woods)
            icon = 248;
        else if (hills)
            icon = 152;
    }
    if (icon != NONE) {
        DrawAbits(icon, 30, 32, 10, 344, 407);
    } else {
        PalColour(3);
        gfx::rectangle(gfx::FILL, 344, 407, 32, 10);
    }
}

// With nothing selected the bar shows the side's standing (89e0:05a3):
// cities, treasury, income and upkeep, four 40 x 20 cells of ABITS.PCK each
// with a number beside it (4125:3072, :3092, :30a2, :3128).
struct StatusCell {
    int sx, sy, x, tx;
    const char* fmt;
};
static const StatusCell kStatusCells[4] = {{344, 0, 32, 72, "%d"},
                                           {344, 20, 120, 144, "%dgp"},
                                           {384, 0, 200, 232, "%dgp"},
                                           {384, 20, 280, 320, "%dgp"}};
static const int kStatusY = 425;

static int StatusValue(int k) {
    switch (k) {
    case 0:
        return static_cast<int>(game::sideCities(*G.g, *G.player).size());
    case 1:
        return G.player->gold;
    case 2:
        return game::income(*G.g, *G.player);
    default:
        return game::upkeep(*G.g, *G.player);
    }
}

static void DrawStatus() {
    for (int k = 0; k < 4; k++) {
        const auto& st = kStatusCells[k];
        DrawAbits(st.sx, st.sy, 40, 20, st.x, kStatusY);
        G.bigFont->draw(w2::format(st.fmt, StatusValue(k)), st.tx, kStatusY);
    }
}

// A computer's turn in the status bar (8cc6:0000, 5db9:0000): a line centred
// on (196, 415) in the side's colours, a black frame at (36, 442) 320x22, and
// the first n / 5 * 16 pixels of the side's MOVEBAR<n>.PCK at (32, 444).
static void DrawComputerStatus() {
    const AiStatus& st = *G.aiStatus;
    Side* side = st.side;
    gfx::setColor(1, 1, 1);
    kit::centred(G.bigFont->colours(side->colour, side->edge), st.text, 196, 415);
    PalColour(0);
    kit::outline(36, 442, 320, 22);
    int w = st.progress / 5 * 16;
    if (w > 0) {
        int i = side->index;
        if (!G.moveBars.count(i)) {
            try {
                G.moveBars[i] =
                        pckimage::load(G.dataDir + "/TERRAIN0/MOVEBAR" + std::to_string(i) + ".PCK",
                                       G.palette, pckimage::CORNER);
            } catch (...) {
                G.moveBars[i] = nullptr;
            }
        }
        gfx::setColor(1, 1, 1);
        if (G.moveBars[i])
            gfx::draw(G.moveBars[i], gfx::newQuad(0, 0, std::min(w, 320), 18), 32, 444);
    }
}

// The right button on the bottom bar (89e0:0ad9).
static void BarInfo(int x, int y) {
    int bx = x - G.layout.bar.x;
    if (G.aiStatus) return;
    if (G.selection) {
        if (bx < 336) {
            int n = (bx - 16) / 40;
            if (bx >= 16 && n < G.selection->slots.n)
                infobox::army(x, y, G.selection->slots.army[n]);
            else
                infobox::lines(x, y, "Select Army", "Select armies when present");  // 4125:3222
        } else {
            infobox::lines(x, y, "Group/Ungroup", "Manipulate all armies");  // 4125:3169
        }
        return;
    }
    static const char* const kHelp[4][2] = {{"Number of Cities", "You have %d cities!"},
                                            {"Your Treasury", "You have %d gold!"},
                                            {"Your Income", "You earn %d gold!"},
                                            {"Your Upkeep", "You pay %d gold!"}};
    int k = (bx - 16) / 90;
    if (bx >= 16 && k >= 0 && k < 4)
        infobox::lines(x, y, kHelp[k][0], w2::format(kHelp[k][1], StatusValue(k)));
}

static void DrawBottomBar() {
    // 8065:0aeb blits MARBLE.PCK over the bar, (16, 403) 360x66 from (0, 60).
    gfx::setColor(1, 1, 1);
    gfx::draw(G.marble, gfx::newQuad(0, 60, 360, 66), 16, 403);
    if (G.aiStatus)
        DrawComputerStatus();
    else if (G.selection)
        DrawArmySlots();
    else
        DrawStatus();
}

// The city dialog's Vector mode paints the map its own way (834b:08df, and
// 834b:1817 for See All): a marker on each of the side's cities from
// ATRANS2.PCK's row at y = 94 (4125:2c76), and lines for the vectors.
static const int kVmarkX[7] = {0, 32, 48, 16, 64, 80, 96};

// 2133:00b8 draws a plain Bresenham line, both ends included
static void LinePixels(int x0, int y0, int x1, int y1) {
    int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        gfx::rectangle(gfx::FILL, x0, y0, 1, 1);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void front::drawVectorMap(int x, int y, City* city, int filter, bool see_all) {
    if (!G.stratImage) G.stratImage = screen::strategicImage(*G.screen, *G.g, G.player->index);
    gfx::setScissor(x, y, 224, 312);
    gfx::setColor(1, 1, 1);
    gfx::draw(G.stratImage, x, y);
    auto marker = [&](const City* c, int k) {
        gfx::setColor(1, 1, 1);
        gfx::draw(G.atransShields, gfx::newQuad(kVmarkX[k], 94, 16, 10), x + c->x * 2 - 2,
                  y + c->y * 2 - 1);
    };
    auto line = [&](const City* a, const City* b, int colour) {
        PalColour(colour);
        LinePixels(x + a->x * 2 + 2, y + a->y * 2 + 2, x + b->x * 2 + 2, y + b->y * 2 + 2);
    };
    auto mine = game::sideCities(*G.g, *G.player);
    auto building = [](const City* c) { return game::producing(*c) ? 0 : 3; };
    auto [sx, sy] = game::standardAt(*G.g, *G.player);
    auto to_standard = [&, sx = sx, sy = sy](const City* c, int colour) {
        PalColour(colour);
        LinePixels(x + c->x * 2 + 2, y + c->y * 2 + 2, x + sx * 2, y + sy * 2);
    };
    if (see_all) {
        std::set<const City*> done;
        for (City* c : mine) {
            if (done.count(c)) continue;
            done.insert(c);
            auto incoming = game::vectoredTo(*G.g, c);
            int k = building(c);
            if (c->vectorTo != NONE) k = 2;
            if (!incoming.empty()) k = game::producing(*c) ? 1 : 5;
            marker(c, k);
            City* dest = c->vectorTo >= 0 ? G.g->map->city(c->vectorTo) : nullptr;
            if (dest) {
                done.insert(dest);
                marker(dest, game::producing(*dest) ? 1 : 5);
                line(dest, c, 7);
            } else if (c->vectorTo == game::STANDARD && sx != NONE) {
                to_standard(c, 8);
            }
            for (City* src : incoming) {
                done.insert(src);
                marker(src, 2);
                line(src, c, 8);
            }
        }
        PalColour(15);
        int bx = x + (city ? city->x : -10) * 2 - 3, by = y + (city ? city->y : -10) * 2 - 2;
        gfx::rectangle(gfx::FILL, bx, by, 11, 1);
        gfx::rectangle(gfx::FILL, bx, by + 11, 11, 1);
        gfx::rectangle(gfx::FILL, bx, by, 1, 11);
        gfx::rectangle(gfx::FILL, bx + 11, by, 1, 11);
    } else if (city) {
        for (City* c : mine) {
            int n = static_cast<int>(game::vectoredTo(*G.g, c).size());
            bool full = (filter == -1 && n + 1 > game::MAX_VECTORED_TO) ||
                        (filter >= 0 && filter != NONE && n + filter > game::MAX_VECTORED_TO);
            if (c == city || !full) {
                int k = building(c);
                if (c == city) k = game::producing(*c) ? 4 : 6;
                marker(c, k);
            }
        }
        City* dest = city->vectorTo >= 0 ? G.g->map->city(city->vectorTo) : nullptr;
        if (dest) {
            marker(dest, game::producing(*dest) ? 1 : 5);
            line(city, dest, 7);
        } else if (city->vectorTo == game::STANDARD && sx != NONE) {
            to_standard(city, 7);
        }
        for (City* src : game::vectoredTo(*G.g, city)) {
            marker(src, 2);
            line(src, city, 8);
        }
    }
    if (sx != NONE) {
        gfx::setColor(1, 1, 1);
        gfx::draw(G.atransShields, gfx::newQuad(96, 15, 16, 15),
                  x + std::max(0, (sx * 2 - 2) / 8 * 8), y + std::max(0, sy * 2 - 6));
    }
    gfx::setScissor();
}

// The figure is 834b:1f5f's third argument, a rect from 4125:2cae: 1, (96, 0)
// outlined in white, everywhere but the hero dialog, which draws the other
// heroes with 0, (96, 15), dark (6c1b:10ac).
void front::drawHeroFigure(int x, int y, int tx, int ty, bool dark) {
    gfx::setScissor(x, y, 224, 312);
    gfx::setColor(1, 1, 1);
    gfx::draw(G.atransShields, gfx::newQuad(96, dark ? 15 : 0, 16, 15),
              x + std::max(0, (tx * 2 - 2) / 8 * 8), y + std::max(0, ty * 2 - 6));
    gfx::setScissor();
}

void front::drawStrategicPanel(int x, int y, const City* city) {
    drawStrategicMap(x, y, city);
}

// The start-of-turn banner: CITY.PCK framed in the side's colour, the frame
// painted into the picture (54f6:0000).
static void DrawBanner() {
    const Banner& b = *G.banner;
    const auto& popup_rect = kBanner;
    auto fill = [](int x, int y, int w, int h) { gfx::rectangle(gfx::FILL, x, y, w, h); };
    kit::popupFrame(popup_rect);
    gfx::setColor(1, 1, 1);
    gfx::draw(G.cityPic, popup_rect.x, popup_rect.y);
    auto outline = [&](int x, int y, int w, int h) {
        fill(popup_rect.x + x, popup_rect.y + y, w, 1);
        fill(popup_rect.x + x, popup_rect.y + y + h - 1, w, 1);
        fill(popup_rect.x + x, popup_rect.y + y, 1, h);
        fill(popup_rect.x + x + w - 1, popup_rect.y + y, 1, h);
    };
    PalColour(b.edge);
    outline(0, 0, popup_rect.w, popup_rect.h);
    PalColour(b.colour);
    fill(popup_rect.x + 1, popup_rect.y + 1, popup_rect.w - 2, 9);
    fill(popup_rect.x + 1, popup_rect.y + 1, 9, popup_rect.h - 2);
    fill(popup_rect.x + 1, popup_rect.y + popup_rect.h - 10, popup_rect.w - 2, 9);
    fill(popup_rect.x + popup_rect.w - 10, popup_rect.y + 1, 9, popup_rect.h - 2);
    PalColour(b.edge);
    outline(10, 10, popup_rect.w - 20, popup_rect.h - 20);
    gfx::setColor(1, 1, 1);
    kit::centred(*G.titleFont, b.name, 320, kBannerNameY);
    kit::centred(*G.titleFont, w2::fmt("Turn %d", b.turn), 320, kBannerTurnY);
}

// The hero offer: the marble, the map with where the hero would appear,
// the portrait, four lines, the name field and the two checkboxes.
static void DrawHeroOffer() {
    const auto& popup_rect = kHeroPopup;
    const w2::HeroOffer& b = *G.offer;
    kit::popupFrame(popup_rect);
    gfx::setColor(1, 1, 1);
    gfx::setScissor(popup_rect.x, popup_rect.y, popup_rect.w, popup_rect.h);
    gfx::draw(G.marble, popup_rect.x, popup_rect.y);
    gfx::setScissor();
    front::drawStrategicMap(kHeroMapX, kHeroMapY);
    gfx::setScissor(popup_rect.x, popup_rect.y, popup_rect.w, popup_rect.h);
    gfx::setColor(1, 1, 1);
    int fx = std::max(0, (b.city->x * 2 - 2) / 8 * 8);
    int fy = std::max(0, b.city->y * 2 - 6);
    gfx::draw(G.atransShields, gfx::newQuad(96, 0, 16, 15), kHeroMapX + fx, kHeroMapY + fy);
    gfx::draw(G.offerFemale ? G.heroPicF : G.heroPicM, kHeroPicX, kHeroPicY);
    gfx::setColor(0, 0, 0);
    gfx::rectangle(gfx::LINE, kHeroPicX - 0.5, kHeroPicY - 0.5, kHeroPicW + 1, kHeroPicH + 1);
    gfx::setScissor();
    auto centred = [](const Font& f, const std::string& s, int y) {
        gfx::setColor(1, 1, 1);
        f.draw(s, kHeroCentre - f.width(s) / 2, y);
    };
    centred(*G.titleFont, kit::text(kHeroTitleGroup, 0), kHeroTitleY);
    auto lines = HeroLines(b, G.offerFemale);
    for (int i = 0; i < 4; i++) {
        // font 2 in 15 with a colour-14 outline (78a8:06ae(2, 15, 14, 3))
        if (!lines[i].empty()) centred(G.bigFont->colours(15, 14), lines[i], kHeroLineY[i]);
    }
    if (auto field = screen::dialogControl(*G.heroView, kHeroField)) {
        gfx::setColor(0, 0, 0);
        kit::outline(field->x - 2, field->y - 2, field->w + 4, field->h + 4);
        kit::field(field->x, field->y, field->w, field->h, G.offerName, G.bigFont.get());
    }
    auto label = [](const int* at, const std::string& s) {
        gfx::setColor(1, 1, 1);
        G.bigFont->draw(s, at[0] - G.bigFont->width(s), at[1]);
    };
    label(kMaleLabel, kit::text(kHeroSexGroup, 0));
    label(kFemaleLabel, kit::text(kHeroSexGroup, 1));
    auto box = [](int id, bool on) {
        auto c = screen::dialogControl(*G.heroView, id);
        if (!c) return;
        gfx::setColor(1, 1, 1);
        gfx::draw(G.abits, gfx::newQuad(320, on ? 0 : 20, 24, 20), c->x, c->y);
    };
    box(kHeroMale, !G.offerFemale);
    box(kHeroFemale, G.offerFemale);
    screen::drawDialogControls(*G.screen, *G.heroView);
}

// ------------------------------------------------------------------ the assault

// A city is not walked into, it is assaulted, and a stack always fights from
// one of the eight tiles around it. What the player sees, in order: the fire
// cloud from WAR.PCK over the tile (67cc:1836), the battle window with both
// lines drawn up (6a35:0160), the fight played back one army at a time in
// the order combat_resolve logged (6a35:0094), how it ended (6a35:04c5), and
// if a city fell, what is to be done with it (63fa:0000). The fight is
// decided before any of it is drawn (docs/re/ui.md > The assault).
static const kit::Rect kAsWindow{160, 60, 320, 312};  // popup 8
static const int kAsCloudW = 128, kAsCloudH = 120;    // 4125:0cfa
static const int kAsShieldW = 32, kAsShieldH = 36;
static const int kAsShieldDef[2] = {176, 86}, kAsShieldAtk[2] = {176, 246};
static const std::vector<int> kAsRows = {86, 116, 146, 176};  // the defender's rows
static const int kAsAtkRow = 246;
static const int kAsXEven[8] = {216, 248, 280, 312, 344, 376, 408, 440};
static const int kAsXOdd[7] = {232, 264, 296, 328, 360, 392, 424};
static const int kAsSea[4] = {0, 162, 32, 18}, kAsSeaDrop = 10;  // WAR.PCK's water
static const int kAsTextY = 298, kAsTextStep = 20;               // 4125:4366
static const double kAsCloudTime = 0.7;
// one casualty (6a35:0094): 5, 3 and 5 BIOS ticks; once hurried, 2 and 3
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

// Where slot k (from 1) of a line of n sits across the window (6a35:0160).
static int SlotX(int n, int k) {
    if (n >= 8) return kAsXEven[k - 1];
    int from = 4 - (n + 1) / 2;
    return n % 2 == 1 ? kAsXOdd[from + k - 1] : kAsXEven[from + k - 1];
}

// A whole line laid out: rows of eight from the top, the last centred.
static std::vector<std::pair<int, int>> LineSlots(int n, const std::vector<int>& rows) {
    std::vector<std::pair<int, int>> out(n);
    int i = 1;
    size_t row = 0;
    while (n - i + 1 >= 8 && row + 1 < rows.size()) {
        for (int k = 1; k <= 8; k++) out[i + k - 2] = {kAsXEven[k - 1], rows[row]};
        i += 8;
        row++;
    }
    int y = row < rows.size() ? rows[row] : rows.back();
    for (int k = 1; k <= n - i + 1; k++) out[i + k - 2] = {SlotX(n - i + 1, k), y};
    return out;
}

// The name the spoils dialog calls the victor by: the hero who led the
// assault, else the best army in the line.
static std::string VictorName(const std::vector<Army*>& line) {
    for (Army* a : line)
        if (a->hero() && !a->name.empty()) return a->name;
    const w2::ArmyType* rec = line.empty() ? nullptr : G.g->types.byId(line[0]->type);
    return rec ? rec->name : "Your armies";
}

// How the fight ended, in the original's own words.
static std::vector<std::string> OutcomeLines(const w2::Battle& result, const City* city,
                                             bool fled) {
    std::vector<std::string> out;
    if (!result.won) {
        out.push_back(kit::text(kAsLost, 0));
        return out;
    }
    std::string hero_name;
    for (Army* a : result.lines.attackers)
        if (a->hero() && !a->name.empty()) {
            hero_name = a->name;
            break;
        }
    if (city) {
        if (fled) out.push_back(kit::text(kAsFled, G.g->rng.dice(1, 4, -1)));
        if (!hero_name.empty())
            out.push_back(format(kit::text(kAsWonCityHero, 0), hero_name));
        else
            out.push_back(kit::text(kAsWonCity, 0));
    } else if (!hero_name.empty()) {
        out.push_back(format(kit::text(kAsWonHero, 0), hero_name));
        out.push_back(kit::text(kAsWonHero, 1));
    } else {
        out.push_back(kit::text(kAsWon, 0));
    }
    if (result.loot > 0) out.push_back(format(kit::text(kAsLoot, 0), result.loot));
    return out;
}

// Begin showing an assault; the fight is already decided.
static void StartAssault(int x, int y, const w2::Battle& result) {
    const auto& lines = result.lines;
    bool fled = lines.defenders.empty();
    Assault a;
    a.x = x;
    a.y = y;
    a.result = result;
    a.def = lines.defenders;
    a.atk = lines.attackers;
    a.defSlots = LineSlots(static_cast<int>(lines.defenders.size()), kAsRows);
    a.atkSlots = LineSlots(static_cast<int>(lines.attackers.size()), {kAsAtkRow});
    a.defSide = lines.defenders.empty() ? 8 : OwnerOr8(lines.defenders[0]->owner);
    a.atkSide = lines.attackers.empty() ? 8 : OwnerOr8(lines.attackers[0]->owner);
    a.phase = "cloud";
    a.at = w2::now();
    // on a human's turn 67cc:1836 holds the cloud while WAR.8SN plays
    a.cloudTime = (G.g->side && !G.g->side->computer) ? sound::effect("war") : 0;
    a.victor = VictorName(lines.attackers);
    a.message = OutcomeLines(result, lines.city, fled);
    a.defFell.resize(a.def.size());
    a.atkFell.resize(a.atk.size());
    G.assault = a;
}
void front::startAssault(int x, int y, const w2::Battle& result) {
    ::StartAssault(x, y, result);
}

// Close the battle window and let the fight take effect (67cc:0a6b).
void front::closeAssault() {
    if (!G.assault) return;
    Assault a = std::move(*G.assault);
    G.assault.reset();
    game::applyAttack(*G.g, a.result);
    G.medal = a.result.medal;
    front::stratDirty();
    if (a.after) a.after();
    // a computer's assault is the coroutine's: it carries on
}

// Carry the playback on by the clock.
static void AdvanceAssault() {
    Assault& a = *G.assault;
    double t = w2::now();
    if (a.phase == "cloud") {
        if (t - a.at >= std::max(kAsCloudTime, a.cloudTime)) {
            // a computer's battle nobody is to see: the cloud was all of it
            if (a.computer && !a.window) {
                front::closeAssault();
                return;
            }
            a.phase = "battle";
            a.at = t;
            // 67cc:124a waits five ticks on the drawn-up lines before the fight
            if (a.computer) a.at = t + 5 / 18.2;
        }
        return;
    }
    if (a.phase == "over" && a.computer) {
        if (a.closeAt == 0) {
            a.closeAt = t + 15 / 18.2;
            if (G.aiStatus) G.aiStatus->text = a.outcome;
        } else if (t >= a.closeAt) {
            front::closeAssault();
        }
        return;
    }
    if (a.phase != "battle") return;
    double each = a.fast ? kAsFellFast : kAsFellTime;
    const auto& log = a.result.log;
    while (a.step < log.size() && t >= a.at) {
        a.step++;
        Fell fell{a.at, a.fast ? kAsBlastFast : kAsBlastTime};
        if (log[a.step - 1] == 1) {
            if (a.atkDown < static_cast<int>(a.atkFell.size())) a.atkFell[a.atkDown] = fell;
            a.atkDown++;
        } else {
            if (a.defDown < static_cast<int>(a.defFell.size())) a.defFell[a.defDown] = fell;
            a.defDown++;
        }
        // 7dda:0181: a blow for each, until Space hurries the fight on
        if (!a.fast) sound::effect(log[a.step - 1] == 1 ? "army" : "army2");
        a.at += each;
    }
    if (a.step >= log.size() && t >= a.at) a.phase = "over";
}

// A key or a click: run the playback through, and when it is through, close
// the window and ask what is to be done with the city.
static void PressAssault() {
    if (!G.assault) return;
    Assault& a = *G.assault;
    if (a.phase != "over") {
        a.fast = true;
        a.at = std::min(a.at, w2::now() + kAsFellFast);
        AdvanceAssault();
        return;
    }
    City* city = a.result.captured;
    bool computer = a.computer;
    std::string victor = a.victor;
    // captured is set once applied: apply, then ask
    front::closeAssault();
    Army* medal = G.medal;
    G.medal = nullptr;
    if (!computer) {
        // the fight took effect in closeAssault; see whether a city fell
        if (G.selection && !G.selection->stack.empty()) {
            City* c = game::cityAt(*G.g, G.selection->stack[0]->x, G.selection->stack[0]->y);
            if (c && c->ownerIndex == G.player->index) city = c;
        }
        // a medal is told before the city's fate is asked (67cc:0000)
        auto then = [city, victor]() {
            if (city) PresentVictory(city, victor);
        };
        if (medal && G.g->alive(medal))
            honours::medal(medal, then);
        else
            then();
    }
}

// The cloud over the tile, drawn with the map, in map pixels.
static void DrawCloud() {
    const Assault& a = *G.assault;
    int sx = a.x * kTile + (kTile - kAsCloudW) / 2;
    int sy = a.y * kTile + (kTile - kAsCloudH) / 2;
    gfx::setColor(1, 1, 1);
    gfx::draw(G.warPic, gfx::newQuad(0, 0, kAsCloudW, kAsCloudH), sx, sy);
}

// One line of armies, struck off from the front as they fall.
static void DrawBattleLine(const std::vector<Army*>& armies,
                           const std::vector<std::pair<int, int>>& slots, int side, int down,
                           const std::vector<Fell>& fell) {
    if (side < 0 || side > 8) side = 8;
    double t = w2::now();
    for (size_t i = 0; i < armies.size() && i < slots.size(); i++) {
        bool gone = static_cast<int>(i) < down && i < fell.size() && t >= fell[i].at + fell[i].burn;
        if (gone) continue;
        auto at = slots[i];
        gfx::setColor(1, 1, 1);
        if (armies[i]->atSea)
            gfx::draw(G.warPic, gfx::newQuad(kAsSea[0], kAsSea[1], kAsSea[2], kAsSea[3]), at.first,
                      at.second + kAsSeaDrop);
        gfx::draw(G.armyImg[side], kit::armyQuad(armies[i]->type), at.first, at.second);
        if (static_cast<int>(i) < down)
            gfx::draw(G.atransShields, gfx::newQuad(32, 0, 32, 29), at.first, at.second);
    }
}

static void DrawBattle() {
    const Assault& a = *G.assault;
    const auto& popup_rect = kAsWindow;
    gfx::setColor(0, 0, 0);
    gfx::rectangle(gfx::LINE, popup_rect.x - 0.5, popup_rect.y - 0.5, popup_rect.w + 1,
                   popup_rect.h + 1);
    gfx::rectangle(gfx::FILL, popup_rect.x + 1, popup_rect.y + popup_rect.h + 1, popup_rect.w + 2,
                   2);
    gfx::rectangle(gfx::FILL, popup_rect.x + popup_rect.w + 1, popup_rect.y + 1, 2,
                   popup_rect.h + 2);
    gfx::setColor(1, 1, 1);
    gfx::draw(G.marble, gfx::newQuad(0, 0, popup_rect.w, popup_rect.h), popup_rect.x, popup_rect.y);
    auto shield = [](int side, const int* at) {
        gfx::setColor(1, 1, 1);
        gfx::draw(G.shieldImg, gfx::newQuad(side * kAsShieldW, 0, kAsShieldW, kAsShieldH), at[0],
                  at[1]);
    };
    shield(a.defSide, kAsShieldDef);
    shield(a.atkSide, kAsShieldAtk);
    DrawBattleLine(a.def, a.defSlots, a.defSide, a.defDown, a.defFell);
    DrawBattleLine(a.atk, a.atkSlots, a.atkSide, a.atkDown, a.atkFell);
    if (a.phase == "over") {
        int y = kAsTextY;
        gfx::setColor(1, 1, 1);
        for (auto& line : a.message) {
            kit::centred(*G.bigFont, line, 320, y);  // 6a35:04c5, on x = 320
            y += kAsTextStep;
        }
    }
}

static void DrawAssault() {
    AdvanceAssault();
    if (!G.assault) return;
    if (G.assault->phase != "cloud") DrawBattle();
}

// Dialog 11 behind popup 7 -- exactly VICTORY.PCK -- with four buttons.
// Pillage needs a production type to strip and sack two (63fa:0000).
static void PresentVictory(City* city, const std::string& victor) {
    if (!G.victoryView) G.victoryView = screen::dialog(*G.screen, kAsVDialog);
    Victory v;
    v.city = city;
    v.who = format(kit::text(kAsVWho, G.g->rng.dice(1, 4, -1)), victor);
    v.where = format(kit::text(kAsVWhere, G.g->rng.dice(1, 3, -1)), city->name);
    G.victory = v;
    auto& st = G.victoryView->state;
    st[kAsOccupy] = NORMAL;
    st[kAsRaze] = NORMAL;
    // Pillage and Sack by the gold they would bring (63fa:0834, 63fa:07d0)
    int pillage_gold =
            city->slots.empty() ? 0 : std::abs(G.g->types.byId(city->slots.back().type)->price) / 2;
    int sack_gold = w2::ai::groups::sackValue(*G.g, *city);
    st[kAsPillage] = pillage_gold != 0 ? NORMAL : DISABLED;
    st[kAsSack] = sack_gold != 0 ? NORMAL : DISABLED;
    // Enter is Occupy -- unless the stack's hero is on a quest this answers:
    // Raze for a raze quest on this city, Sack (or Pillage) for gold (63fa:0000)
    G.victory->enter = kAsOccupy;
    auto q = G.player->quest;
    Army* hero = nullptr;
    if (G.selection) {
        for (Army* a : G.selection->stack)
            if (a->type == w2::armytype::HERO) {
                hero = a;
                break;
            }
    }
    if (G.g->map->options.quests != 0 && q && hero && hero == q->hero) {
        if (q->type == w2::quest::RAZE && q->city == city)
            G.victory->enter = kAsRaze;
        else if (q->type == w2::quest::PILLAGE_GOLD) {
            if (sack_gold != 0)
                G.victory->enter = kAsSack;
            else if (pillage_gold != 0)
                G.victory->enter = kAsPillage;
        }
    }
}
void front::presentVictory(City* city, const std::string& victor) {
    ::PresentVictory(city, victor);
}

static void DrawVictory(const Victory& v) {
    const auto& popup_rect = kAsVPopup;
    gfx::setColor(0, 0, 0);
    gfx::rectangle(gfx::LINE, popup_rect.x - 0.5, popup_rect.y - 0.5, popup_rect.w + 1,
                   popup_rect.h + 1);
    gfx::rectangle(gfx::FILL, popup_rect.x + 1, popup_rect.y + popup_rect.h + 1, popup_rect.w + 2,
                   2);
    gfx::rectangle(gfx::FILL, popup_rect.x + popup_rect.w + 1, popup_rect.y + 1, 2,
                   popup_rect.h + 2);
    gfx::setColor(1, 1, 1);
    gfx::draw(G.victoryPic, popup_rect.x, popup_rect.y);
    kit::centred(*G.titleFont, kit::text(kAsVText, 0), 320, kAsVTitleY);
    kit::centred(*G.bigFont, v.who, 320, kAsVLineY[0]);
    kit::centred(*G.bigFont, v.where, 320, kAsVLineY[1]);
    kit::centred(*G.bigFont, kit::text(kAsVText, 1), 320, kAsVLineY[2]);
    kit::centred(*G.bigFont, kit::text(kAsVText, 2), 320, kAsVLineY[3]);
    screen::drawDialogControls(*G.screen, *G.victoryView);
}

// What the player chose to do with the city they have just taken (63fa).
static void TakeCity(int what) {
    if (!G.victory) return;
    Victory v = *G.victory;
    City* c = v.city;
    std::vector<Army*> stack = G.selection ? G.selection->stack : std::vector<Army*>{};
    G.victory.reset();
    auto done = []() {
        G.victoryUnder.reset();
        front::stratDirty();
        RefreshControls();
    };
    if (what == kAsPillage || what == kAsSack) {
        bool sacked = what == kAsSack;
        auto spoils = sacked ? game::sack(*G.g, *G.player, *c, stack)
                             : game::pillage(*G.g, *G.player, *c, stack);
        G.victoryUnder = v;
        spoils::open(sacked, c, spoils.gold, spoils.lost, static_cast<int>(c->slots.size()), done);
    } else if (what == kAsRaze) {
        G.victoryUnder = v;
        search::say(c->name + " is in ruins!", [c, stack, done]() {  // 4125:0a56
            game::raze(*G.g, *G.player, *c, stack);
            done();
        });
    } else {
        // quest_check(4): a quest done is its reward instead of the city
        auto news = game::occupy(*G.g, *G.player, *c, stack);
        auto production = [c]() { city::open(c, city::PRODUCTION); };
        if (news) {
            G.player->questNews.reset();
            if (!news->failed.empty())
                questnews::show(news, production);
            else
                questnews::show(news);
        } else {
            production();
        }
    }
}

// Draw in the popups' 640x480 frame, centred on the screen.
static void InDialogFrame(const std::function<void()>& draw) {
    gfx::push();
    gfx::translate(G.layout.dialog.x, G.layout.dialog.y);
    draw();
    gfx::pop();
}

// Draw one of the fixed groups of the original screen where it now is.
static void InGroup(const layout::Offset& o, const std::function<void()>& draw) {
    gfx::push();
    gfx::translate(o.x, o.y);
    draw();
    gfx::pop();
}

static void DrawModals() {
    // a dialog may push another while drawing is under way: a copy is drawn
    auto modals = G.modals;
    for (auto& d : modals) d->draw();
}

// The samples queue and the advisor blinks by the clock.
static void Update() {
    sound::update();
    if (auto top = kit::topShared()) top->update();
}

// One frame: the chrome in UI pixels, the map in map pixels at its own
// zoom, then the dialogs and the pointer over both.
static void Draw() {
    gfx::begin();
    gfx::clear(0, 0, 0);
    syncLayout();
    StepComputer();
    display::pushUI();
    if (G.starting || !G.g) {
        DrawModals();
        DrawMenuBar();
        DrawPointer();
        gfx::pop();
        return;
    }
    AdvanceWalk();
    // a quest that ended is told once the screen is the player's again
    if (G.player && G.player->questNews &&
        !(G.over || G.banner || G.offer || G.assault || G.victory || G.victoryUnder || G.walk ||
          G.moveAll || G.aiRun || G.openMenu >= 0 || kit::top())) {
        questnews::poll();
    }
    RefreshControls();
    screen::drawBackground(*G.screen, G.layout);
    const auto& r = G.mapRect;
    if (G.mapAfterBanner) {
        // a hotseat turn not yet taken up: both maps blank (hotseatHidden)
        const auto& s = G.stratRect;
        gfx::setColor(0, 0, 0);
        gfx::rectangle(gfx::FILL, r.x, r.y, r.w, r.h);
        gfx::rectangle(gfx::FILL, s.x, s.y, s.w, s.h);
        gfx::setColor(1, 1, 1);
    } else {
        gfx::setScissor(r.x, r.y, r.w, r.h);
        auto [cx, cy] = CamDev();
        display::pushMap(r.x, r.y, cx, cy, G.zoom);
        DrawMap();
        if (G.assault && G.assault->phase == "cloud") DrawCloud();
        gfx::pop();
        gfx::setScissor();
        DrawStrategic();
    }
    // 8065:0a9d refills the control panel with marble before the controls go on
    InGroup(G.layout.panel, []() {
        gfx::setColor(1, 1, 1);
        gfx::draw(G.marble, gfx::newQuad(0, 0, 224, 114), 400, 355);
    });
    screen::drawControls(*G.screen);
    DrawShortcutIcons();
    InGroup(G.layout.bar, DrawBottomBar);
    InDialogFrame([]() {
        if (G.victoryUnder) DrawVictory(*G.victoryUnder);
        DrawModals();
    });
    DrawMenuBar();
    // the turn opens with the banner over the offer, and is dismissed first
    InDialogFrame([]() {
        if (G.offer) DrawHeroOffer();
        if (G.assault) DrawAssault();
        if (G.victory) DrawVictory(*G.victory);
        if (G.banner) DrawBanner();
    });
    DrawPointer();
    gfx::pop();
}

// ------------------------------------------------------------------ input

// 1a8b:04c8 reads no input while a stack walks, and Move All walks one stack
// after another. A click meanwhile is lost; a key waits in the keyboard's
// buffer, fifteen of them, and is read once it is all over.
static bool BusyWalking() {
    return ((G.walk && !G.walk->computer) || G.moveAll) && !kit::top();
}

static void FlushKeys() {
    auto ks = G.keyBuffer;
    G.keyBuffer.clear();
    for (auto& k : ks) Keypressed(k);
}

// While the computer plays: a key hurries a battle, or runs the rest of the
// round through unwatched. True when it took the input.
static bool ComputerInput(const std::string& k) {
    if (!(G.aiRun && !kit::top() && G.openMenu < 0)) return false;
    if (G.assault && G.assault->computer) {
        PressAssault();
        return true;
    }
    // Allow opening the F10 AI Brain Inspector (under --modern-ui), panning, or adjusting zoom
    // while spectating AI turns
    if (G.modernUi && k == "f10") {
        diplomacyui::openBrainInspector(G.aiSide ? G.aiSide->index : NONE);
        return true;
    }
    if (k == "kp+" || k == "pageup" || k == "=") {
        front::setZoom(G.zoom + 1);
        return true;
    }
    if (k == "kp-" || k == "pagedown" || k == "-") {
        front::setZoom(G.zoom - 1);
        return true;
    }
    if (k == "up") {
        G.cy -= 1;
        ClampCamera();
        return true;
    }
    if (k == "down") {
        G.cy += 1;
        ClampCamera();
        return true;
    }
    if (k == "left") {
        G.cx -= 1;
        ClampCamera();
        return true;
    }
    if (k == "right") {
        G.cx += 1;
        ClampCamera();
        return true;
    }
    // Shift and Alt are held to reach Settings, not pressed to skip
    if (w2::endsWith(k, "shift") || w2::endsWith(k, "alt") || w2::endsWith(k, "ctrl") ||
        w2::endsWith(k, "gui")) {
        return true;
    }
    if (!HumansLeft()) {
        // In AI-vs-AI spectator mode, let Space toggle fast-forward and allow read-only spectator
        // dialogs/quit
        if (k == "space") {
            G.aiSkip = !G.aiSkip;
            if (G.aiSkip && G.walk && G.walk->computer) {
                G.walk.reset();
                ResumeComputer();
            }
            return true;
        }
        if (k == "escape") {
            settings::open();
            return true;
        }
        if (Held({"lctrl", "rctrl", "lgui", "rgui"}) && k == "q") return false;
        if (k == "c" || k == "." || k == "a" || k == "k" || k == "g" || k == "n" || k == "w" ||
            k == "d" || k == "h" || k == "e" || k == "j" || k == "y" || k == "l" || k == "?") {
            return false;
        }
        return true;
    }
    G.aiSkip = true;
    if (G.walk && G.walk->computer) {
        G.walk.reset();
        ResumeComputer();
    }
    return true;
}

// A control's id, minus 100, indexes the 397-entry jump table at 17be:0b0c:
// that is what turns a click into an action (docs/re/ui.md > Commands).
static std::map<int, std::function<void()>>& Actions();

static void Mousepressed(int x, int y, int button) {
    if (BusyWalking()) return;
    syncLayout();
    int fx = x - G.layout.dialog.x, fy = y - G.layout.dialog.y;
    if (G.aiRun && !kit::top()) {
        if (G.assault && G.assault->computer) {
            PressAssault();
            return;
        }
        if (!HumansLeft()) {
            // AI-vs-AI spectator mouse controls: menu bar, right-click tile/stack inspect, minimap
            // jump, and map drag
            int hit = menu::titleAt(G.menuLayout, x, y);
            if (hit >= 0) {
                G.openMenu = G.openMenu == hit ? -1 : hit;
                return;
            }
            if (G.openMenu >= 0) {
                const menu::Row* row = menu::rowAt(G.menuLayout, G.openMenu, x, y);
                G.openMenu = -1;
                std::string rk = row ? (row->key.empty() ? row->act : row->key) : "";
                if (!rk.empty() && MenuEnabled(rk)) MenuPick(rk);
                return;
            }
            if (G.g && G.screen) {
                if (auto r = screen::regionAt(*G.screen, x, y)) {
                    if (r->id == screen::MAP) {
                        if (button == 2) {
                            if (auto tile = TileAtPoint(x, y))
                                tileinfo::open(tile->first, tile->second);
                            return;
                        }
                        if (button == 1) {
                            G.drag = Drag{G.cx, G.cy, 0, 0};
                            return;
                        }
                    } else if (r->id == screen::STRATEGIC && button == 1) {
                        int tx = (x - r->x) / 2, ty = (y - r->y + strat_scroll) / 2;
                        centreOn(tx, ty);
                        return;
                    }
                }
            }
            return;
        }
        if (ComputerInput("")) return;
    }
    // 7ecb:0142: the banner eats the click that dismisses it
    if (G.banner) {
        DismissBanner();
        return;
    }
    if (G.assault) {
        PressAssault();
        return;
    }
    // the right button is not a click on a dialog's button (18a9:012c)
    if ((G.victory || G.offer) && button == 2) return;
    if (G.victory) {
        auto c = screen::dialogControlAt(*G.victoryView, fx, fy);
        if (c && G.victoryView->stateOf(c->id) != DISABLED) TakeCity(c->id);
        return;
    }
    if (G.offer) {
        auto c = screen::dialogControlAt(*G.heroView, fx, fy);
        if (!c) return;
        if (c->id == kHeroField)
            offer_typing = true;
        else if (c->id == kHeroMale)
            G.offerFemale = false;
        else if (c->id == kHeroFemale)
            G.offerFemale = true;
        else if (c->id == kHeroOk)
            AcceptOffer();
        else if (c->id == kHeroCancel && !G.offer->first)
            RefuseOffer();
        return;
    }
    // on the start screens the menu bar stays live over them (7f77:0200)
    auto top = kit::topShared();
    if (top && top->menuBar && (G.openMenu >= 0 || menu::titleAt(G.menuLayout, x, y) >= 0))
        top = nullptr;
    if (top) {
        // the right button on a dialog: a control's help, or the dialog's regions
        if (button == 2 && top->view) {
            auto c = infobox::controlAt(*top->view, fx, fy, &top->hidden);
            if (!(c && infobox::control(x, y, c->id, top.get()))) top->rightpressed(fx, fy, x, y);
            return;
        }
        top->mousepressed(fx, fy, button);
        return;
    }
    if (G.over) return;
    // the menu bar takes precedence over everything beneath it, the start
    // screens' too, with no game yet
    int hit = menu::titleAt(G.menuLayout, x, y);
    if (hit >= 0) {
        G.openMenu = G.openMenu == hit ? -1 : hit;
        return;
    }
    if (G.openMenu >= 0) {
        const menu::Row* row = menu::rowAt(G.menuLayout, G.openMenu, x, y);
        G.openMenu = -1;
        std::string k = row ? (row->key.empty() ? row->act : row->key) : "";
        if (!k.empty() && MenuEnabled(k)) MenuPick(k);
        return;
    }
    if (!G.g) return;
    auto c = screen::controlAt(*G.screen, x, y);
    if (c && button == 2) {
        if (infobox::control(x, y, c->id)) return;
    } else if (c) {
        if (G.screen->stateOf(c->id) == DISABLED) return;
        G.pressed = c->id;
        G.screen->state[c->id] = ACTIVE;
        return;
    }
    // the hand drags the map (740d:00e4 -> 8065:0e04)
    if (button == 1 && PointerKind(x, y) == kHand) {
        G.drag = Drag{G.cx, G.cy, 0, 0};
        return;
    }
    auto r = screen::regionAt(*G.screen, x, y);
    if (!r) return;
    if (r->id == screen::MAP) {
        auto tile = TileAtPoint(x, y);
        if (!tile) return;
        auto [tx, ty] = *tile;
        if (button == 2) {
            tileinfo::open(tx, ty);
            return;
        }
        // the left button does what the pointer shows (740d:00ce)
        int k = PointerKind(x, y);
        if (k == kWalk || k == kBoat)
            MoveSelection(tx, ty);
        else if (k == kAttack || k == kPeace)
            AttackTile(tx, ty);
        else if (k == kCityP || k == kSiteP) {
            if (City* city = game::cityAt(*G.g, tx, ty)) front::openCity(city);
        } else if (k == kAdvise) {
            miladvisor::open(G.selection ? &G.selection->stack : nullptr, tx, ty);
        } else if (k == kSelect) {
            SelectClick(tx, ty);
        } else if (k == kAlt) {
            PlanRoute(tx, ty);
            RefreshControls();
        }
    } else if (r->id == screen::STRATEGIC) {
        int tx = (x - r->x) / 2, ty = (y - r->y + strat_scroll) / 2;
        if (button != 1) return;
        if (Held({"lalt", "ralt"})) {
            PlanRoute(tx, ty);
            RefreshControls();
        } else {
            centreOn(tx, ty);
        }
    } else if (r->id == screen::BOTTOMBAR && button == 2) {
        BarInfo(x, y);
    } else if (r->id == screen::MAPPANEL && button == 2) {
        infobox::lines(x, y, "- Drag Screen -", "Move mouse to drag the screen");
    }
}

void front::afterSlotChange() {
    SyncSelection();
}

// Order > Move All (1c8c:04c4): every army of the side with a destination
// walks there in turn, each walk played out before the next.
static void MoveAllStep() {
    while (G.moveAll) {
        if (G.walk) return;
        Army* lead = nullptr;
        for (Army* a : G.g->armies) {
            if (a->owner == G.player->index && a->target && !a->transit &&
                !G.moveAll->seen.count(a)) {
                lead = a;
                break;
            }
        }
        if (!lead) {
            if (G.moveAll->moved == 0) say("Nothing is under orders.");
            G.moveAll.reset();
            RefreshControls();
            return;
        }
        G.moveAll->seen.insert(lead);
        Select(lead->x, lead->y, lead);
        if (G.selection) {
            for (Army* a : G.selection->stack) G.moveAll->seen.insert(a);
            // selecting a stack already where it was sent lifts the order
            // (refreshRoute), so there may be nothing left to walk to
            if (lead->target) {
                auto target = *lead->target;
                MoveSelection(target.first, target.second);
                if (G.moveAll) G.moveAll->moved++;
            }
        }
    }
}

static void MoveAll() {
    G.moveAll = App::MoveAll{};
    if (G.selection && !G.selection->stack.empty() && G.selection->stack[0]->target) {
        auto t = *G.selection->stack[0]->target;
        for (Army* a : G.selection->stack) G.moveAll->seen.insert(a);
        MoveSelection(t.first, t.second);
        if (G.moveAll) G.moveAll->moved = 1;
    }
    MoveAllStep();
}

// Hero > Search (6536:0000) with the selected stack.
static void DoSearch() {
    if (!G.selection) return;
    int x = G.selection->x, y = G.selection->y;
    search::open(G.selection->stack);
    Reslot(SelectableAt(x, y));
    front::stratDirty();
}

// The key of each menu item UDB.DAT lets a button carry (545c:00aa).
static std::string ShortcutKey(int n) {
    static const std::map<int, std::string> kKey = {
        {507, "m"}, {508, "q"}, {510, "a"}, {511, "k"}, {512, "g"}, {513, "n"}, {514, "w"},
        {516, "="}, {517, ","}, {518, "f"}, {521, "z"}, {524, "b"}, {525, "c"}, {526, "p"},
        {527, "v"}, {528, "."}, {529, "s"}, {530, "h"}, {531, "e"}, {534, "l"}, {535, "alt E"}};
    auto sc = G.screen->ui.shortcuts.find(n);
    if (sc == G.screen->ui.shortcuts.end()) return "";
    auto k = kKey.find(sc->second);
    return k == kKey.end() ? "" : k->second;
}

static std::map<int, std::function<void()>>& Actions() {
    static std::map<int, std::function<void()>> action_table;
    if (!action_table.empty()) return action_table;
    for (int i = 0; i < kSlotCount; i++) {
        action_table[kSlotFirst + i] = [i]() {
            if (!G.selection) return;
            slots_mod::toggle(G.selection->slots, *G.g, i);
            front::afterSlotChange();
        };
        action_table[kBarFirst + i] = [i]() {
            if (!G.selection) return;
            slots_mod::pickGroup(G.selection->slots, *G.g, i);
            front::afterSlotChange();
        };
    }
    action_table[kGrpAll] = []() {
        if (!G.selection) return;
        auto& s = G.selection->slots;
        if (slots_mod::grouped(s))
            slots_mod::single(s, *G.g);
        else
            slots_mod::all(s, *G.g);
        front::afterSlotChange();
    };
    action_table[kGrpNone] = action_table[kGrpAll];
    // The five buttons above the pad, 173-178: walk on, next, done, defend,
    // deselect (8065:0ec9 / 0ed7 / 0ef4 / 0f26).
    action_table[173] = []() {
        if (!G.selection || G.selection->stack.empty()) return;
        auto t = G.selection->stack[0]->target;
        if (!t) return;
        MoveSelection(t->first, t->second);
    };
    action_table[174] = SelectNext;
    action_table[175] = QuitArmy;
    action_table[176] = Fortify;
    action_table[178] = Deselect;
    // The 3x3 pad, 320-327 (8611:0723), clockwise from north: it moves the view.
    static const int kPad[8][2] = {{0, -1}, {1, -1}, {1, 0},  {1, 1},
                                   {0, 1},  {-1, 1}, {-1, 0}, {-1, -1}};
    for (int i = 0; i < 8; i++) {
        action_table[320 + i] = [i]() {
            G.cx += kPad[i][0];
            G.cy += kPad[i][1];
            ClampCamera();
        };
    }
    for (int i = 0; i < w2::uidata::SHORTCUT_COUNT; i++) {
        action_table[w2::uidata::SHORTCUT_FIRST + i] = [i]() {
            std::string k = ShortcutKey(i);
            if (!k.empty() && MenuEnabled(k)) MenuDoes()[k]();
        };
    }
    // the "?" (8065:104e): the mouse's help page, then the keys'
    action_table[188] = []() {
        help::open(
                "HELP\\HMOUSE.GFX", []() { help::open("HELP\\HKEYS.GFX", nullptr, &help::POPUP4); },
                &help::POPUP4);
    };
    // 186 (8065:0f3f) looks at where the stack is going, and back
    action_table[186] = []() {
        if (!G.selection || G.selection->stack.empty()) return;
        auto& sel = *G.selection;
        auto t = sel.stack[0]->target;
        double cx = G.cx, cy = G.cy;
        if (t && InView(sel.x, sel.y)) {
            centreOn(t->first, t->second);
            if (G.cx == cx && G.cy == cy) centreOn(sel.x, sel.y);
        } else {
            centreOn(sel.x, sel.y);
        }
    };
    // 187 (8065:0fe9) forgets the destination
    action_table[187] = []() {
        if (!G.selection || G.selection->stack.empty() || !G.selection->stack[0]->target) return;
        for (Army* a : G.selection->stack) a->target.reset();
        G.route.reset();
    };
    // 183-185 are one button in three faces: Diplomatic Action (484e:0346)
    for (int id = 183; id <= 185; id++) action_table[id] = diplomacyui::action;
    // the pad's centre, 177, shares its handler with Home (8065:0f02)
    action_table[177] = []() {
        if (G.selection) centreOn(G.selection->x, G.selection->y);
    };
    return action_table;
}

// Which buttons are live (8065:0174, the original's own refresh). A control
// with no handler here is greyed too.
static void RefreshControls() {
    auto& st = G.screen->state;
    // under the banner and the hero offer every control is still greyed
    if (G.banner || G.offer) {
        for (auto& [id, s] : st) s = DISABLED;
        return;
    }
    auto& action_table = Actions();
    auto set = [&](int id, bool live) {
        if (!st.count(id)) return;
        if (!(live && action_table.count(id)))
            st[id] = DISABLED;
        else if (id == G.pressed)
            st[id] = ACTIVE;
        else
            st[id] = NORMAL;
    };
    const Selection* sel = G.selection ? &*G.selection : nullptr;
    bool walk_on =
            sel && G.route &&
            static_cast<int>(G.route->path.size()) > (G.walk ? static_cast<int>(G.walk->i) + 1 : 0);
    set(173, walk_on);
    bool more = false;
    for (Army* a : G.g->armies)
        if (a->owner == G.player->index && !a->transit && !a->fortified && !a->done) {
            more = true;
            break;
        }
    set(174, more);
    set(175, more && sel);
    set(176, sel);
    set(177, sel);
    set(178, sel);
    set(186, sel);
    set(187, sel && !sel->stack.empty() && sel->stack[0]->target);
    set(188, true);
    for (int i = 0; i < kSlotCount; i++) {
        bool live = sel && i < sel->slots.n;
        set(kSlotFirst + i, live);
        set(kBarFirst + i, live);
    }
    bool grouped = sel && slots_mod::grouped(sel->slots);
    set(240, sel && !grouped);
    set(241, sel && grouped);
    for (int i = 0; i < w2::uidata::SHORTCUT_COUNT; i++) {
        std::string k = ShortcutKey(i);
        set(w2::uidata::SHORTCUT_FIRST + i, !k.empty() && MenuEnabled(k));
    }
    for (int i = 0; i < 8; i++) set(320 + i, true);
    int face = diplomacyui::buttonFor(*G.g, G.player->index);
    for (int id = 183; id <= 185; id++) set(id, id == face);
}

// The drag: the map goes the way the mouse does, by exactly as far.
static void Mousemoved(double dx, double dy) {
    if (!G.drag) return;
    G.drag->dx += dx;
    G.drag->dy += dy;
    double k = static_cast<double>(display::d.scale) / (kTile * G.zoom);
    G.cx = G.drag->cx - G.drag->dx * k;
    G.cy = G.drag->cy - G.drag->dy * k;
    ClampCamera();
}

// The wheel zooms the map about the tile under the pointer.
static void Wheelmoved(int wy) {
    syncLayout();
    if (G.starting || !G.g || G.over || kit::top() || G.banner || G.offer || G.victory || wy == 0)
        return;
    if (G.mapRect.contains(G.mouseX, G.mouseY))
        SetZoomAt(G.zoom + (wy > 0 ? 1 : -1), G.mouseX, G.mouseY, false);
    else
        SetZoomAt(G.zoom + (wy > 0 ? 1 : -1), 0, 0, true);
}

static void Mousereleased(int x, int y, int button) {
    syncLayout();
    if (G.drag) {
        G.drag.reset();
        if (G.pressed == NONE) return;
    }
    if (auto top = kit::topShared()) {
        if (top->mousereleased(x - G.layout.dialog.x, y - G.layout.dialog.y, button)) return;
    }
    int id = G.pressed;
    if (id == NONE) return;
    G.pressed = NONE;
    G.screen->state[id] = NORMAL;
    auto c = screen::controlAt(*G.screen, x, y);
    if (!c || c->id != id) return;  // released off the button
    auto& action_table = Actions();
    auto act = action_table.find(id);
    if (act != action_table.end()) act->second();
    if (G.g) RefreshControls();
}

// The city nearest the view's centre, then the city dialog in that mode.
static void ViewCity(int mode) {
    auto [cx, cy] = front::viewCentre();
    int viewer = game::viewerSide(*G.g, G.player);
    City* best = nullptr;
    int best_d = NONE;
    for (auto& c : G.g->map->cities) {
        bool ok = game::seen(*G.g, viewer, c.x, c.y) &&
                  (mode == city::INFO || c.ownerIndex == G.player->index);
        if (ok) {
            int d = std::max(std::abs(c.x - cx), std::abs(c.y - cy));
            if (best_d == NONE || d < best_d) {
                best = &c;
                best_d = d;
            }
        }
    }
    if (best) city::open(best, mode);
}

void front::quit() {
    G.modals.clear();
    G.aiRun.reset();
    ai_co = nullptr;
    G.aiStatus.reset();
    G.assault.reset();
    G.walk.reset();
    G.moveAll.reset();
    G.banner.reset();
    G.offer.reset();
    G.victory.reset();
    G.victoryUnder.reset();
    G.over = false;
    front::openStart();
}

// Each menu accelerator, as far as this engine can honour it.
static std::map<std::string, std::function<void()>>& MenuDoes() {
    static std::map<std::string, std::function<void()>> menu_actions;
    if (!menu_actions.empty()) return menu_actions;
    menu_actions["alt E"] = EndTurn;
    menu_actions["m"] = MoveAll;
    // Game > Quit (7721:0000) and New game (7721:019d) ask first
    menu_actions["^Q"] = []() {
        input::Options o;
        o.title = kit::text(0x2c, 0);
        for (int i = 1; i <= 4; i++) o.lines.push_back(kit::text(0x2c, i));
        o.confirm = true;
        o.ok = [](const std::string&) {
            // 7721:0072: "Farewell, Warlord!" -- and the game closes
            advisor::say(w2::cues::QUIT, []() { G.quitRequested = true; });
        };
        input::open(o);
    };
    menu_actions["alt N"] = []() {
        input::Options o;
        o.title = kit::text(0x2d, 0);
        for (int i = 1; i <= 4; i++) o.lines.push_back(kit::text(0x2d, i));
        o.confirm = true;
        o.ok = [](const std::string&) {
            G.selection.reset();
            G.banner.reset();
            G.offer.reset();
            front::openStart();
        };
        input::open(o);
    };
    menu_actions["alt S"] = savegame::save;
    menu_actions["alt L"] = []() {
        savegame::load([](std::unique_ptr<w2::Game> g) {
            if (G.starting) {
                G.modals.clear();
                G.starting = false;
            }
            front::takeLoaded(std::move(g));
        });
    };
    menu_actions["z"] = DoSearch;
    menu_actions["c"] = []() { ViewCity(city::INFO); };
    menu_actions["b"] = []() { ViewCity(city::CITY); };
    menu_actions["p"] = []() { ViewCity(city::PRODUCTION); };
    menu_actions["v"] = []() { ViewCity(city::VECTOR); };
    // Hero > Plant Flag (7563:09f7)
    menu_actions["f"] = []() {
        if (!G.selection) return;
        for (Army* a : G.selection->stack) {
            if (a->hero() && game::plantFlag(*G.g, *G.player, a)) {
                front::stratDirty();
                return;
            }
        }
    };
    // Order > Disband (1b62:06bf)
    menu_actions["q"] = []() {
        if (!G.selection || G.selection->stack.empty()) return;
        auto stack = G.selection->stack;
        bool heroes = false;
        for (Army* a : stack)
            if (a->hero()) heroes = true;
        input::Options o;
        o.title = "Disband";  // 4125:2bf8
        o.confirm = true;
        o.lines = {"Are you sure you", "want to disband this", "group?",
                   heroes ? "It contains heroes" : ""};
        o.ok = [stack](const std::string&) {
            game::disband(*G.g, *G.player, stack);
            G.selection.reset();
            front::stratDirty();
        };
        input::open(o);
    };
    menu_actions["i"] = fightorder::open;
    menu_actions["r"] = resign::open;
    menu_actions["."] = []() {
        auto [cx, cy] = front::viewCentre();
        ruin::open(ruin::nearest(cx, cy));
    };
    menu_actions["s"] = stackui::open;
    menu_actions["o"] = armybonus::open;
    menu_actions["t"] = items::open;
    menu_actions["x"] = []() {
        if (G.selection) signpost::open(G.selection->stack);
    };
    menu_actions["h"] = []() { historyui::open(0); };
    menu_actions["e"] = []() { historyui::open(1); };
    menu_actions["j"] = []() { historyui::open(2); };
    menu_actions["y"] = []() { historyui::open(3); };
    menu_actions["alt U"] = shortcuts::open;
    menu_actions["alt X"] = []() { settings::open(); };
    menu_actions["?"] = about::open;
    menu_actions["l"] = historyui::triumphs;
    menu_actions["d"] = diplomacyui::open;
    menu_actions["="] = questui::open;
    menu_actions["u"] = levels::open;
    menu_actions[","] = heroinfo::open;
    menu_actions["a"] = []() { reports::open(0); };
    menu_actions["k"] = []() { reports::open(1); };
    menu_actions["g"] = []() { reports::open(2); };
    menu_actions["n"] = []() { reports::open(3); };
    menu_actions["w"] = []() { reports::open(4); };
    for (int n = 1; n <= 16; n++) {
        menu_actions["map zoom " + std::to_string(n)] = [n]() { front::setZoom(n); };
        menu_actions["ui scale " + std::to_string(n)] = [n]() { SetUiScale(n); };
    }
    menu_actions["ui scale smooth"] = []() { SetUiScale(display::SMOOTH); };
    menu_actions["screen full"] = []() { SetScreen(false); };
    menu_actions["screen window"] = []() { SetScreen(true); };
    menu_actions["screen 4:3"] = []() { SetOriginalSize(true); };
    menu_actions["screen fit"] = []() { SetOriginalSize(false); };
    menu_actions["screen crt"] = ToggleCrt;
    for (const char* s : {"fm", "mt32", "sc55"}) {
        std::string synth = s;
        menu_actions["music " + synth] = [synth]() { sound::setSynth(synth); };
    }
    menu_actions["ai modern"] = []() { SetAiMode(false); };
    menu_actions["ai classic"] = []() { SetAiMode(true); };
    return menu_actions;
}

static void MenuPick(const std::string& k) {
    auto& menu_actions = MenuDoes();
    auto it = menu_actions.find(k);
    if (it != menu_actions.end()) it->second();
}

// What greys a menu item (8065:0519, the table at 4125:1798).
static bool MenuEnabled(const std::string& k) {
    auto& menu_actions = MenuDoes();
    if (k.empty() || !menu_actions.count(k)) return false;
    bool music = w2::startsWith(k, "music ");
    bool ai_opt = w2::startsWith(k, "ai ");
    if (music && !sound::synthAvailable(k.substr(6))) return false;
    // 7f77:0200: on the start screens every menu is greyed but Quit and Load
    if (G.starting || !G.g) {
        return k == "^Q" || (k == "alt L" && savegame::used() > 0) ||
               w2::startsWith(k, "ui scale ") || w2::startsWith(k, "screen ") || music || ai_opt;
    }
    // While AI-vs-AI spectating is running, only enable read-only spectator reports, display/audio
    // settings, and quit
    if (G.aiRun && !HumansLeft()) {
        if (k == "h" || k == "e" || k == "j" || k == "y") return G.g->turn != 1;
        if (k == "d") return G.g->map->options.diplomacy != 0;
        return k == "^Q" || k == "alt X" || k == "?" || k == "c" || k == "." || k == "a" ||
               k == "k" || k == "g" || k == "n" || k == "w" || k == "l" ||
               w2::startsWith(k, "map zoom ") || w2::startsWith(k, "ui scale ") ||
               w2::startsWith(k, "screen ") || music || ai_opt;
    }
    w2::Game& g = *G.g;
    auto side_has_hero = [&]() {
        for (Army* a : g.armies)
            if (a->owner == G.player->index && a->hero()) return true;
        return false;
    };
    auto selected = []() { return G.selection && !G.selection->stack.empty(); };
    auto side_has_cities = [&]() {
        for (auto& c : g.map->cities)
            if (c.ownerIndex == G.player->index && !c.razed) return true;
        return false;
    };
    auto can_search = [&]() {
        if (!selected()) return false;
        Army* lead = G.selection->stack[0];
        w2::Site* s =
                g.map->siteAt.empty() ? nullptr : g.map->siteAt[lead->y * g.map->width + lead->x];
        if (!s || s->searched) return false;
        return lead->hero() || s->type == w2::site::TEMPLE;
    };
    auto can_plant_flag = [&]() {
        if (!selected()) return false;
        Army* lead = G.selection->stack[0];
        if (!lead->hero()) return false;
        if (G.player->index >= static_cast<int>(g.map->items.size())) return false;
        w2::Item* std = &g.map->items[G.player->index];
        if (std::find(lead->items.begin(), lead->items.end(), std) == lead->items.end())
            return false;
        int t = w2::scn::terrainAt(*g.map, lead->x, lead->y);
        if (t == move::WATER || t == move::SHORE || t == move::CITY || t == move::SITE)
            return false;
        for (auto& it : g.map->items)
            if (it.planted && it.x == lead->x && it.y == lead->y) return false;
        return true;
    };
    if (k == "alt L") return savegame::used() > 0;
    if (k == "q" || k == "s") return selected();
    if (k == "x")
        return selected() && w2::scn::terrainAt(*g.map, G.selection->stack[0]->x,
                                                G.selection->stack[0]->y) == move::TOWER;
    if (k == "r" || k == "b" || k == "p" || k == "v") return side_has_cities();
    if (k == "d") return g.map->options.diplomacy != 0;
    if (k == "," || k == "u") return side_has_hero();
    if (k == "f") return can_plant_flag();
    if (k == "z") return can_search();
    if (k == "h" || k == "e" || k == "j" || k == "y") return g.turn != 1;
    if (k == "alt E") return !g.won;
    return true;
}
bool front::menuEnabled(const std::string& k) {
    return ::MenuEnabled(k);
}

static void Keypressed(const std::string& k) {
    if (BusyWalking()) {
        if (G.keyBuffer.size() < 15) G.keyBuffer.push_back(k);
        return;
    }
    syncLayout();
    if (ComputerInput(k)) return;
    if (G.banner) {
        DismissBanner();
        return;
    }
    if (G.assault) {
        PressAssault();
        return;
    }
    if (G.victory) {
        // Occupy heads both the default and the cancel lists
        if (k == "return" || k == "kpenter" || k == "escape") TakeCity(G.victory->enter);
        return;
    }
    if (G.offer) {
        if (k == "return" || k == "kpenter")
            AcceptOffer();
        else if (k == "backspace" && !G.offerName.empty())
            G.offerName.pop_back();
        else if (k == "escape" && !G.offer->first)
            RefuseOffer();
        return;
    }
    bool alt = Held({"lalt", "ralt"}), ctrl = Held({"lctrl", "rctrl", "lgui", "rgui"}),
         shift = Held({"lshift", "rshift"});
    auto top = kit::topShared();
    if (top) {
        if (top->menuBar) {
            std::string accel;
            if (ctrl && k == "q")
                accel = "^Q";
            else if (alt && k.size() == 1)
                accel = "alt " + w2::upper(k);
            if (!accel.empty()) {
                G.openMenu = -1;
                if (MenuEnabled(accel)) MenuDoes()[accel]();
                return;
            }
        }
        if (G.openMenu >= 0) {
            G.openMenu = -1;
            return;
        }
        top->keypressed(k);
        return;
    }
    if (G.openMenu >= 0) {
        G.openMenu = -1;
        return;
    }
    if (G.over || !G.g) return;
    // the main screen's keys (17be:0064, 17be:0444)
    auto press = [](int id) {
        auto& action_table = Actions();
        if (G.screen->stateOf(id) != DISABLED && action_table.count(id)) action_table[id]();
        RefreshControls();
    };
    std::string digit;
    if (k.size() == 1 && k[0] >= '0' && k[0] <= '9')
        digit = k;
    else if (k.size() == 3 && w2::startsWith(k, "kp") && k[2] >= '0' && k[2] <= '9')
        digit = k.substr(2);
    if (k == "return" || k == "kpenter")
        press(174);
    else if (k == "kp+" || k == "pageup" || k == "=")
        front::setZoom(G.zoom + 1);
    else if (k == "kp-" || k == "pagedown" || k == "-")
        front::setZoom(G.zoom - 1);
    else if (k == "escape")
        press(175);
    else if (k == "space")
        press(240);  // group the stack (89e0:0a55)
    else if (k == "tab")
        press(186);
    else if (k == "backspace")
        press(187);
    else if (k == "home")
        press(177);
    else if (k == "end")
        press(173);  // walk on along the route (17be:0318)
    else if (k == "delete")
        Deselect();  // 1b62:08b3, as 178 is (17be:02f1)
    else if (digit == "5") {
        if (G.selection) centreOn(G.selection->x, G.selection->y);
    } else if (!digit.empty()) {
        // 1-9 step the stack the way the numeric pad lies (1c8c:0197)
        static const std::map<char, std::pair<int, int>> kStep = {
            {'8', {0, -1}}, {'9', {1, -1}}, {'6', {1, 0}},  {'3', {1, 1}},
            {'2', {0, 1}},  {'1', {-1, 1}}, {'4', {-1, 0}}, {'7', {-1, -1}}};
        auto st = kStep.find(digit[0]);
        if (st != kStep.end() && G.selection) {
            for (Army* a : G.selection->stack) a->target.reset();
            MoveSelection(G.selection->x + st->second.first, G.selection->y + st->second.second);
        }
    } else if (k == "up")
        press(320);
    else if (k == "right")
        press(322);
    else if (k == "down")
        press(324);
    else if (k == "left")
        press(326);
    else if (k == "f5") {
        // not the original's: quick save and load
        if (savegame::writeSlot("quick", *G.g))
            say("Saved.");
        else
            say("Could not save.");
    } else if (k == "f9") {
        if (auto loaded = savegame::readSlot("quick")) front::takeLoaded(std::move(loaded));
    } else if (G.modernUi && k == "f10") {
        diplomacyui::openBrainInspector();
    } else {
        std::string accel;
        if (ctrl && k == "q")
            accel = "^Q";
        else if (alt && k.size() == 1)
            accel = "alt " + w2::upper(k);
        else if (shift && k == "/")
            accel = "?";
        else if (k.size() == 1)
            accel = k;
        // a key is its menu item, and does nothing while the item is greyed
        if (!accel.empty() && MenuEnabled(accel)) MenuDoes()[accel]();
    }
}

// Typing into the hero's name field, or a dialog's.
static void Textinput(const std::string& text) {
    auto top = kit::topShared();
    if (top && !G.offer) {
        top->textinput(text);
        return;
    }
    if (!G.offer) return;
    static const std::regex kOk("^[A-Za-z0-9\\s'\\-.]+$");
    if (G.offerName.size() < kHeroNameMax && std::regex_match(text, kOk)) G.offerName += text;
}

// ------------------------------------------------------------------ controller

// Not the original's: a game controller plays as the mouse and a few keys,
// so that the Steam Deck plays with Steam's gamepad layout in Gaming Mode
// (cpp/linux/README.md has the table). The right stick is the pointer, and
// the right trackpad with it, as that layout makes the trackpad the right
// stick; the left stick slides the map. The touchscreen and a trackpad set
// to the mouse go on working beside them.
static const double kPointerSpeed = 1.2;  // the frame's height a second, the stick all the way over
static const double kSlideSpeed = 600;    // UI pixels a second, likewise

static double last_x = 0, last_y = 0;           // the mouse, in window points, as last seen
static std::vector<std::pair<int, int>> warps;  // where the mouse was put, its motion not yet back
static double pad_x = 0, pad_y = 0;             // the pointer as the stick has it, in UI pixels
static int pad_left = 0, pad_right = 0;         // the controls holding each mouse button down
static int typing_now = 0;  // text input: 0 off, 1 on, 2 on with the Deck's keyboard up

// The pointer to a UI point, dragging the map if a drag is on, and the
// mouse along with it, so that a trackpad or a mouse goes on from there.
static void PointTo(double x, double y) {
    x = std::clamp(x, 0.0, display::d.w - 0.001);
    y = std::clamp(y, 0.0, display::d.h - 0.001);
    double dx = x - pad_x, dy = y - pad_y;
    pad_x = x;
    pad_y = y;
    display::d.mouseInside = true;
    display::d.off.reset();
    Mousemoved(dx, dy);
    if (static_cast<int>(x) == G.mouseX && static_cast<int>(y) == G.mouseY) return;
    G.mouseX = static_cast<int>(x);
    G.mouseY = static_cast<int>(y);
    SDL_Window* win = display::window();
    if (win && SDL_GetKeyboardFocus() == win) {
        auto [wx, wy] = display::fromUI(G.mouseX + 0.5, G.mouseY + 0.5);
        last_x = std::round(wx);
        last_y = std::round(wy);
        if (warps.size() >= 16) warps.erase(warps.begin());
        warps.push_back({static_cast<int>(last_x), static_cast<int>(last_y)});
        SDL_WarpMouseInWindow(win, static_cast<int>(last_x), static_cast<int>(last_y));
    }
}

// A mouse button, held while any of the controls for it is.
static void PadMouse(int& held, int button, bool down) {
    if (down && held++ == 0)
        Mousepressed(G.mouseX, G.mouseY, button);
    else if (!down && held > 0 && --held == 0)
        Mousereleased(G.mouseX, G.mouseY, button);
}

static void PadKey(const std::string& k, bool down) {
    if (!down) {
        keys::release(k);
        return;
    }
    keys::press(k);
    Keypressed(k);
}

static void PadButton(int b, bool down) {
    switch (b) {
    case SDL_CONTROLLER_BUTTON_A:
    case pad::RIGHT_TRIGGER:
        PadMouse(pad_left, 1, down);
        break;
    case SDL_CONTROLLER_BUTTON_X:
    case pad::LEFT_TRIGGER:
        PadMouse(pad_right, 2, down);
        break;
    case SDL_CONTROLLER_BUTTON_B:
        PadKey("escape", down);
        break;
    case SDL_CONTROLLER_BUTTON_Y:
        PadKey("return", down);
        break;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
        if (down) Wheelmoved(-1);
        break;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
        if (down) Wheelmoved(1);
        break;
    // the D-pad steps the stack, as 8 2 4 6 do
    case SDL_CONTROLLER_BUTTON_DPAD_UP:
        PadKey("8", down);
        break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        PadKey("2", down);
        break;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        PadKey("4", down);
        break;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
        PadKey("6", down);
        break;
    // held, as the keys are held for a Shift-click or an Alt-click
    case SDL_CONTROLLER_BUTTON_BACK:
        down ? keys::press("lshift") : keys::release("lshift");
        break;
    case SDL_CONTROLLER_BUTTON_START:
        down ? keys::press("lalt") : keys::release("lalt");
        break;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK:
        PadKey("space", down);
        break;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
        PadKey("home", down);
        break;
    }
}

// A pad gone, or the window left: nothing stays held.
static void PadLetGo() {
    if (pad_left) Mousereleased(G.mouseX, G.mouseY, 1);
    if (pad_right) Mousereleased(G.mouseX, G.mouseY, 2);
    pad_left = pad_right = 0;
}

// Once a frame: the triggers, the pointer and the map's slide, by the
// time gone since the last.
static void PadUpdate() {
    static Uint64 then = SDL_GetPerformanceCounter();
    Uint64 now = SDL_GetPerformanceCounter();
    double dt = std::min(0.1, static_cast<double>(now - then) / SDL_GetPerformanceFrequency());
    then = now;
    int b;
    bool down;
    while (pad::trigger(b, down)) PadButton(b, down);
    // the speed goes as the square of the push: slow for aiming, quick across
    auto s = pad::sticks();
    if (s.rx != 0 || s.ry != 0) {
        // the mouse or a touch moved it since
        if (static_cast<int>(pad_x) != G.mouseX || static_cast<int>(pad_y) != G.mouseY) {
            pad_x = G.mouseX + 0.5;
            pad_y = G.mouseY + 0.5;
        }
        double v = kPointerSpeed * display::d.h * std::hypot(s.rx, s.ry) * dt;
        PointTo(pad_x + s.rx * v, pad_y + s.ry * v);
    }
    if ((s.lx != 0 || s.ly != 0) && !(G.starting || !G.g || G.over || kit::top() || G.banner ||
                                      G.offer || G.victory || G.drag)) {
        double k = kSlideSpeed * std::hypot(s.lx, s.ly) * dt * display::d.scale / (kTile * G.zoom);
        G.cx += s.lx * k;
        G.cy += s.ly * k;
        ClampCamera();
    }
}

// Text input is on while a field is being typed into, and only then: on the
// Steam Deck, SDL brings Steam's on-screen keyboard up with it. The hero's
// name is typed with no click on its field first, so the keyboard waits for
// one.
static void SyncTyping() {
    if (!G.offer) offer_typing = false;
    auto top = kit::top();
    int want = G.offer ? (offer_typing ? 2 : 1) : top && top->typing() ? 2 : 0;
    if (want == typing_now) return;
    if (want == 0 || typing_now == 2) SDL_StopTextInput();
    typing_now = want;
    if (want == 0) return;
    // as SDL starts it at first: text, but no keyboard, unless asked for
    const char* hint = SDL_GetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD);
    if (want == 1 && !hint) SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "0");
    SDL_StartTextInput();
    if (want == 1 && !hint) SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, nullptr);
}

// ------------------------------------------------------------------ starting

// A fresh game: its first turn, the view on the capital.
static void BeginGame() {
    Side* first = game::begin(*G.g);
    if (!first) return;
    // a computer that opens the game plays to the screen of the human who
    // follows it, on that human's hidden map
    G.player = first->computer ? game::nextHuman(*G.g) : first;
    if (!G.player) G.player = first;
    G.cursor = CapOf(G.player);
    G.selection.reset();
    centreOn(CapOf(G.player).first, CapOf(G.player).second);
    // the banner is a human's (8cc6:0259)
    if (!first->computer) {
        G.mapAfterBanner = HotseatHidden();
        ShowBanner(G.player);
    }
}

struct Script {
    std::vector<std::string> lines;
    size_t at = 0;
    int wait = 0;
};
static std::optional<Script> script;

void front::openStart() {
    G.starting = true;
    G.haveLayout = false;
    sound::music(w2::cues::TITLE);
    start::open(
            [](const std::string& dir, const w2::game::NewGameOptions& opts) {
                auto start_game = [dir, opts]() {
                    G.starting = false;
                    G.haveLayout = false;
                    G.scenario = dir;
                    w2::game::NewGameOptions o = opts;
                    o.seed = static_cast<double>(time(nullptr) % 1000000007);
                    G.seed = o.seed;
                    if (opt_military_advisor >= 0) {
                        for (auto& [k, v] : o.options)
                            if (k == "militaryAdvisor") v = opt_military_advisor;
                    }
                    G.g = game::newGame(G.dataDir, dir, o);
                    G.selection.reset();
                    G.over = false;
                    front::stratDirty();
                    syncLayout();
                    BeginGame();
                    if (G.g->side && G.g->side->computer) {
                        // a game with no human in it says so as it begins (8065:00f1)
                        bool human = false;
                        for (Side* s : G.g->sides)
                            if (!s->computer) human = true;
                        if (human)
                            PlayComputers(G.g->side);
                        else
                            search::message(kit::text(0xe, 0), kit::text(0xe, 1),
                                            []() { PlayComputers(G.g->side); });
                    }
                };
                if (!script && sound::speechOn()) {
                    sound::music(w2::cues::BEGIN);
                    advisor::say(w2::cues::BEGIN_WAR, start_game);
                } else {
                    start_game();
                }
            },
            [](std::unique_ptr<w2::Game> g) {
                G.starting = false;
                G.haveLayout = false;
                syncLayout();
                front::takeLoaded(std::move(g));
            });
}

// ------------------------------------------------------------------ scripts

// A script drives the game for testing: one command a line, the coordinates
// UI pixels.
//   wait N          let N frames go by
//   key NAME        press and release a key, by LÖVE's name for it
//   hold NAME / release NAME
//   click X Y [B]   press and release a mouse button (1 left, 2 right)
//   move X Y        move the pointer
//   text STRING     type
//   shot FILE       write the frame as a .bmp
//   glass FILE      write the window as the next frame shows it, the CRT's
//                   tube and all, as a .bmp
//   pad NAME        press and release a controller's button, by SDL's name
//                   for it (a, b, dpup, leftshoulder, lefttrigger ...)
//   padhold NAME / padrelease NAME
//   stick LX LY RX RY   hold the sticks there, -1..1; all 0 lets go
//   eval            print the game's state (turn, side, modals)
//   select TX TY    pick up the stack on a tile
//   attack TX TY    send the selection at a tile, fighting if it must
//   near            list the cities nearest the selection not the player's
//   greet           the advisor's greeting, as the start screens open
//   sdlclick X Y    a left click on UI point (X, Y) sent through SDL's event
//                   queue, as a real one arrives
//   quit
static void StepScript() {
    if (!script) return;
    if (script->wait > 0) {
        script->wait--;
        return;
    }
    while (script->at < script->lines.size()) {
        std::istringstream in(script->lines[script->at++]);
        std::string cmd;
        in >> cmd;
        if (cmd.empty() || cmd[0] == '#') continue;
        if (cmd == "wait") {
            in >> script->wait;
            return;
        } else if (cmd == "key") {
            std::string k;
            in >> k;
            keys::press(k);
            Keypressed(k);
            keys::release(k);
        } else if (cmd == "hold") {
            std::string k;
            in >> k;
            keys::press(k);
        } else if (cmd == "release") {
            std::string k;
            in >> k;
            keys::release(k);
        } else if (cmd == "click") {
            int x, y, b = 1;
            in >> x >> y;
            if (!(in >> b)) b = 1;
            G.mouseX = x;
            G.mouseY = y;
            display::d.mouseInside = true;
            display::d.off.reset();
            Mousepressed(x, y, b);
            Mousereleased(x, y, b);
        } else if (cmd == "move") {
            in >> G.mouseX >> G.mouseY;
            display::d.mouseInside = true;
            display::d.off.reset();
        } else if (cmd == "pad" || cmd == "padhold" || cmd == "padrelease") {
            std::string k;
            in >> k;
            int b = pad::fromName(k);
            if (b < 0) continue;
            if (cmd != "padrelease") PadButton(b, true);
            if (cmd != "padhold") PadButton(b, false);
        } else if (cmd == "stick") {
            pad::Sticks st;
            in >> st.lx >> st.ly >> st.rx >> st.ry;
            pad::script(st);
        } else if (cmd == "text") {
            std::string t;
            std::getline(in, t);
            Textinput(w2::trim(t));
        } else if (cmd == "shot") {
            std::string f;
            in >> f;
            gfx::saveShot(f);
        } else if (cmd == "glass") {
            in >> display::d.glassShot;
        } else if (cmd == "eval") {
            printf("turn=%d player=%s modals=%d banner=%d offer=%d ai=%d over=%d walk=%d "
                   "assault=%d status=%s\n",
                   G.g ? G.g->turn : 0, G.player ? G.player->name.c_str() : "-",
                   static_cast<int>(G.modals.size()), static_cast<bool>(G.banner),
                   static_cast<bool>(G.offer), static_cast<bool>(G.aiRun), G.over,
                   static_cast<bool>(G.walk), static_cast<bool>(G.assault),
                   G.aiStatus ? G.aiStatus->text.c_str() : "");
            if (G.g && G.g->map)
                printf("  options: milAdv=%d diplo=%d hidden=%d viewEn=%d\n",
                       G.g->map->options.militaryAdvisor, G.g->map->options.diplomacy,
                       G.g->map->options.hiddenMap, G.g->map->options.viewEnemies);
            if (G.g && G.player && G.player->capital)
                printf("  cam=%.2f,%.2f zoom=%d map=%d,%d,%d,%d capital=%d,%d pointer=%d,%d\n",
                       G.cx, G.cy, G.zoom, G.mapRect.x, G.mapRect.y, G.mapRect.w, G.mapRect.h,
                       G.player->capital->x, G.player->capital->y, G.mouseX, G.mouseY);
            fflush(stdout);
        } else if (cmd == "opt") {
            std::string o;
            int v;
            in >> o >> v;
            if (G.g && G.g->map) {
                if (int* f = G.g->map->options.field(o)) *f = v;
            }
        } else if (cmd == "load") {
            std::string s;
            in >> s;
            while (kit::top()) kit::pop(kit::top());
            if (auto loaded = savegame::readSlot(s)) {
                front::takeLoaded(std::move(loaded));
                syncLayout();
            }
        } else if (cmd == "centre") {
            int tx, ty;
            in >> tx >> ty;
            centreOn(tx, ty);
        } else if (cmd == "pointer") {
            int x, y;
            in >> x >> y;
            printf("pointer(%d,%d)=%d\n", x, y, PointerKind(x, y));
            fflush(stdout);
        } else if (cmd == "tileptr") {
            int tx, ty;
            in >> tx >> ty;
            int px = G.mapRect.x +
                     static_cast<int>((tx + 0.5 - G.cx) * kTile * G.zoom / display::d.scale);
            int py = G.mapRect.y +
                     static_cast<int>((ty + 0.5 - G.cy) * kTile * G.zoom / display::d.scale);
            printf("tileptr(%d,%d)=%d\n", tx, ty, PointerKind(px, py));
            fflush(stdout);
        } else if (cmd == "spawn" && G.g) {
            int tx, ty, owner, type;
            in >> tx >> ty >> owner >> type;
            auto a = std::make_unique<Army>();
            a->x = tx;
            a->y = ty;
            a->owner = owner;
            a->type = type;
            a->moves = 10;
            G.g->armies.push_back(a.get());
            G.g->pool.push_back(std::move(a));
        } else if (cmd == "advise") {
            int x, y;
            in >> x >> y;
            miladvisor::open(G.selection ? &G.selection->stack : nullptr, x, y);
        } else if (cmd == "select") {
            int x, y;
            in >> x >> y;
            Select(x, y);
        } else if (cmd == "attack") {
            int x, y;
            in >> x >> y;
            AttackTile(x, y);
        } else if (cmd == "near" && G.g && G.selection) {
            std::vector<std::pair<int, City*>> found;
            for (auto& c : G.g->map->cities) {
                if (c.ownerIndex != G.player->index && !c.razed)
                    found.push_back({std::max(std::abs(c.x - G.selection->x),
                                              std::abs(c.y - G.selection->y)),
                                     &c});
            }
            std::sort(found.begin(), found.end(),
                      [](auto& a, auto& b) { return a.first < b.first; });
            for (size_t i = 0; i < found.size() && i < 3; i++)
                printf("  %s at %d,%d owner %d, %d away\n", found[i].second->name.c_str(),
                       found[i].second->x, found[i].second->y, found[i].second->ownerIndex,
                       found[i].first);
            fflush(stdout);
        } else if (cmd == "greet") {
            advisor::say(w2::cues::GREET);
        } else if (cmd == "sdlclick") {
            // a click on UI point (x, y) as SDL delivers one, in window points,
            // through the event loop
            int ux, uy;
            in >> ux >> uy;
            auto [wx, wy] = display::fromUI(ux, uy);
            int x = static_cast<int>(wx), y = static_cast<int>(wy);
            for (Uint32 type : {SDL_MOUSEBUTTONDOWN, SDL_MOUSEBUTTONUP}) {
                SDL_Event ev{};
                ev.type = type;
                ev.button.button = SDL_BUTTON_LEFT;
                ev.button.state = type == SDL_MOUSEBUTTONDOWN ? SDL_PRESSED : SDL_RELEASED;
                ev.button.x = x;
                ev.button.y = y;
                SDL_PushEvent(&ev);
            }
        } else if (cmd == "quit") {
            G.quitRequested = true;
            return;
        }
    }
}

// ------------------------------------------------------------------ main

// Why the game cannot start, on stderr and in a box: on Windows the game has
// no console to print to.
static int Fail(const std::string& msg) {
    fprintf(stderr, "%s\n", msg.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Warlords II", msg.c_str(), nullptr);
    return 1;
}

int main(int argc, char** argv) {
    std::string scenario, script_file, weights_file;
    double seed = 0;
    int size_w = 0, size_h = 0;
    bool windowed = prefs::get("screen") == "window";
    bool use_classic_ai = prefs::get("ai") == "classic";
    bool force_diplomacy = false;
    bool force_hidden_map = false;
    bool spectate_all_ai = false;
    G.modernUi = false;
    G.dataDir = "original";
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--data")
            G.dataDir = next();
        else if (a == "--scenario")
            scenario = w2::upper(next());
        else if (a == "--seed")
            seed = atof(next().c_str());
        else if (a == "--window")
            windowed = true;
        else if (a == "--fullscreen")
            windowed = false;
        else if (a == "--script")
            script_file = next();
        else if (a == "--size")
            sscanf(next().c_str(), "%dx%d", &size_w, &size_h);
        else if (a == "--classic-ai")
            use_classic_ai = true;
        else if (a == "--modern-ai")
            use_classic_ai = false;
        else if (a == "--modern-ui")
            G.modernUi = true;
        else if (a == "--classic-ui")
            G.modernUi = false;
        else if (a == "--weights")
            weights_file = next();
        else if (a == "--diplomacy")
            force_diplomacy = true;
        else if (a == "--hidden-map")
            force_hidden_map = true;
        else if (a == "--military-advisor")
            opt_military_advisor = 1;
        else if (a == "--no-military-advisor")
            opt_military_advisor = 0;
        else if (a == "--spectate" || a == "--all-ai")
            spectate_all_ai = true;
    }
    // Modern AI by default; its trained weights come from --weights or ai_weights.json
    w2::ai::setDefaultClassicAi(use_classic_ai);
    std::string weights_path = !weights_file.empty() ? weights_file
                               : w2::fileExists("cpp/ai_weights.json")
                                       ? std::string("cpp/ai_weights.json")
                               : w2::fileExists("ai_weights.json") ? std::string("ai_weights.json")
                                                                   : std::string();
    if (weights_path.empty() || !w2::ai::ml::activePolicy().loadJson(weights_path)) {
        fprintf(stderr,
                "warning: Modern AI weights %s%s%s could not be loaded (missing, or trained for a "
                "different network "
                "size); the Modern AI will play with untrained random weights. Retrain with "
                "cpp/test/train.sh or "
                "pass --weights PATH.\n",
                weights_path.empty() ? "(none found)" : "'", weights_path.c_str(),
                weights_path.empty() ? "" : "'");
    }
    if (!w2::fileExists(G.dataDir + "/TERRAIN0/ARMYTYPE.DAT"))
        return Fail("cannot find the game's files in '" + G.dataDir +
                    "': run from the repository root, or pass --data DIR");
    pad::hints();
    // Windows: the window sized in points and drawn in pixels, as on a Mac,
    // rather than Windows stretching a blurred 100% picture
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_SCALING, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0) {
        return Fail(std::string("SDL: ") + SDL_GetError());
    }
    SDL_Window* win = display::open("Warlords II", windowed, size_w, size_h);
    if (!win) {
        return Fail(std::string("SDL window: ") + SDL_GetError());
    }
    // OpenGL, where the CRT screen's tube is a shader (platform/crt.hpp); SDL
    // goes on down its list where there is none, and SDL_RENDER_DRIVER wins
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
    SDL_Renderer* ren =
            SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    if (!ren) {
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!ren) {
        return Fail(std::string("SDL renderer: ") + SDL_GetError());
    }
    gfx::init(ren);
    SDL_ShowCursor(SDL_DISABLE);
    // SDL starts with text input on; it waits here for a field (syncTyping)
    SDL_StopTextInput();
    if (!script_file.empty()) {
        Script s;
        std::ifstream in(script_file);
        std::string line;
        while (std::getline(in, line)) s.lines.push_back(line);
        script = s;
    }
    try {
        LoadArt();
        InstallHooks();
        auto ui = w2::uidata::load(G.dataDir);
        sound::init(G.dataDir, ui.files);
        // the screen: its real pixels, the UI's scale, and the map's zoom
        if (prefs::get("4:3") == "on") display::d.wanted = std::make_pair(layout::W, layout::H);
        display::d.crt = prefs::get("crt") == "on";
        display::d.chosen =
                prefs::get("scale") == "smooth" ? display::SMOOTH : prefs::getInt("scale", 0);
        display::measure();
        G.zoom = prefs::getInt("zoom", display::d.scale);
        if (scenario.empty()) {
            front::openStart();
            // 7f77:0000 greets the player the first time round
            if (script_file.empty()) advisor::say(w2::cues::GREET);
        } else {
            G.starting = false;
            G.scenario = scenario;
            game::NewGameOptions o;
            o.seed = seed != 0 ? seed : static_cast<double>(time(nullptr) % 1000000007);
            static const char* const kOptionKeys[10] = {
                "neutralCities",  "diplomacy",     "quests",     "hiddenMap",       "viewEnemies",
                "viewProduction", "intenseCombat", "quickStart", "militaryAdvisor", "randomTurns"};
            int default_options[10] = {0, 0, 0, 0, 1, 0, 0, 0, 1, 0};
            if (auto opt_bytes = w2::readFile(G.dataDir + "/DATA/OPTIONS.DAT")) {
                for (size_t i = 0; i <= 9 && 2 * i + 2 <= opt_bytes->size(); i++) {
                    default_options[i] = w2::u16(*opt_bytes, 2 * i);
                }
            }
            for (int i = 0; i < 10; i++) {
                o.options.emplace_back(kOptionKeys[i], default_options[i]);
            }
            if (force_diplomacy) {
                for (auto& [k, v] : o.options)
                    if (k == "diplomacy") v = 1;
            }
            if (force_hidden_map) {
                for (auto& [k, v] : o.options) {
                    if (k == "hiddenMap") v = 1;
                    if (k == "viewEnemies") v = 0;
                }
            }
            if (opt_military_advisor >= 0) {
                for (auto& [k, v] : o.options)
                    if (k == "militaryAdvisor") v = opt_military_advisor;
            }
            if (scenario == w2::randommap::DIR) {
                // RANDOM: a random world with the start menu's first settings, made
                // from the seed, in place of the last one the original left on disk
                w2::Rng r(o.seed);
                w2::randommap::Options ro;
                ro.dataDir = G.dataDir;
                ro.rng = &r;
                w2::randommap::install(G.dataDir, w2::randommap::generateNow(ro));
            }
            G.g = game::newGame(G.dataDir, scenario, o);
            for (size_t i = 0; i < G.g->sides.size(); i++)
                G.g->sides[i]->computer = spectate_all_ai || (i > 0);
            auto start_war = [=]() {
                syncLayout();
                BeginGame();
                if (G.g->side && G.g->side->computer) PlayComputers(G.g->side);
            };
            if (script_file.empty() && !spectate_all_ai && sound::speechOn()) {
                sound::music(w2::cues::BEGIN);
                advisor::say(w2::cues::BEGIN_WAR, start_war);
            } else {
                start_war();
            }
        }
    } catch (const std::exception& e) {
        fprintf(stderr, "could not start: %s\n", e.what());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Warlords II", e.what(), win);
        return 1;
    }

    {
        int mx, my;
        SDL_GetMouseState(&mx, &my);
        last_x = mx;
        last_y = my;
        display::d.mouseInside = (SDL_GetWindowFlags(win) & SDL_WINDOW_MOUSE_FOCUS) != 0;
        display::d.off = display::offFrame(mx, my);
        std::tie(G.mouseX, G.mouseY) = display::toUI(mx, my);
        pad_x = G.mouseX + 0.5;
        pad_y = G.mouseY + 0.5;
    }
    while (!G.quitRequested) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            try {
                switch (e.type) {
                case SDL_QUIT:
                    G.quitRequested = true;
                    break;
                case SDL_WINDOWEVENT:
                    if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                        e.window.event == SDL_WINDOWEVENT_RESIZED) {
                        display::measure();
                        G.haveLayout = false;
                    } else if (e.window.event == SDL_WINDOWEVENT_LEAVE) {
                        display::d.mouseInside = false;
                    } else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                        keys::clear();
                        PadLetGo();
                    } else if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                        SDL_ShowCursor(SDL_DISABLE);  // the game draws its own pointer
                    }
                    break;
                case SDL_KEYDOWN: {
                    std::string k = keys::loveName(e.key.keysym.sym);
                    if (k.empty()) break;
                    // the Lua remake leaves LÖVE's key repeat off
                    if (e.key.repeat) break;
                    keys::press(k);
                    Keypressed(k);
                    break;
                }
                case SDL_KEYUP: {
                    std::string k = keys::loveName(e.key.keysym.sym);
                    if (!k.empty()) keys::release(k);
                    break;
                }
                case SDL_TEXTINPUT:
                    if (!keys::isDown({"lalt", "ralt", "lctrl", "rctrl", "lgui", "rgui"}))
                        Textinput(w2::utf8ToLatin1(e.text.text));
                    break;
                case SDL_MOUSEBUTTONDOWN:
                case SDL_MOUSEBUTTONUP: {
                    // a click on a window that has not got the focus takes it (see
                    // display::open): else the pointer moves only while dragging
                    if (e.type == SDL_MOUSEBUTTONDOWN &&
                        !(SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS)) {
                        SDL_RaiseWindow(win);
                    }
                    auto [x, y] = display::toUI(e.button.x, e.button.y);
                    G.mouseX = x;
                    G.mouseY = y;
                    int b = e.button.button == SDL_BUTTON_RIGHT    ? 2
                            : e.button.button == SDL_BUTTON_MIDDLE ? 3
                                                                   : 1;
                    if (e.type == SDL_MOUSEBUTTONDOWN)
                        Mousepressed(x, y, b);
                    else
                        Mousereleased(x, y, b);
                    break;
                }
                case SDL_MOUSEMOTION: {
                    // a warp's own motion comes back late, when the stick has gone on
                    auto w = std::find(warps.begin(), warps.end(),
                                       std::make_pair(e.motion.x, e.motion.y));
                    if (w != warps.end()) {
                        warps.erase(warps.begin(), w + 1);
                        break;
                    }
                    auto [x, y] = display::toUI(e.motion.x, e.motion.y);
                    G.mouseX = x;
                    G.mouseY = y;
                    display::d.mouseInside = true;
                    display::d.off = display::offFrame(e.motion.x, e.motion.y);
                    double k = display::d.dpi / display::d.scale;
                    Mousemoved((e.motion.x - last_x) * k, (e.motion.y - last_y) * k);
                    last_x = e.motion.x;
                    last_y = e.motion.y;
                    break;
                }
                case SDL_MOUSEWHEEL:
                    if (e.wheel.y != 0) Wheelmoved(e.wheel.y > 0 ? 1 : -1);
                    break;
                case SDL_CONTROLLERDEVICEADDED:
                case SDL_CONTROLLERDEVICEREMOVED:
                case SDL_CONTROLLERBUTTONDOWN:
                case SDL_CONTROLLERBUTTONUP: {
                    int b;
                    bool down;
                    if (pad::event(e, b, down))
                        PadButton(b, down);
                    else if (e.type == SDL_CONTROLLERDEVICEREMOVED)
                        PadLetGo();
                    break;
                }
                }
            } catch (const std::exception& ex) {
                fprintf(stderr, "error: %s\n", ex.what());
            }
        }
        // Where the mouse is, asked rather than waited for. On some Macs (SDL 3
        // under sdl2-compat) the window holds both focuses and still SDL sends
        // the mouse's moves only while a button is held -- a bare SDL program
        // shows it too -- so the pointer would follow only a drag. So while the
        // window has the focus, the pointer follows wherever the mouse is; a
        // move SDL did send has already brought lastX/lastY up to date.
        if (!script && SDL_GetKeyboardFocus() == win) {
            int gx, gy, wx, wy, ww, wh;
            SDL_GetGlobalMouseState(&gx, &gy);
            SDL_GetWindowPosition(win, &wx, &wy);
            SDL_GetWindowSize(win, &ww, &wh);
            int mx = gx - wx, my = gy - wy;
            if (mx >= 0 && my >= 0 && mx < ww && my < wh &&
                (mx != static_cast<int>(last_x) || my != static_cast<int>(last_y))) {
                auto [x, y] = display::toUI(mx, my);
                G.mouseX = x;
                G.mouseY = y;
                display::d.mouseInside = true;
                display::d.off = display::offFrame(mx, my);
                double k = display::d.dpi / display::d.scale;
                Mousemoved((mx - last_x) * k, (my - last_y) * k);
                last_x = mx;
                last_y = my;
            }
        }
        try {
            display::d.minH = G.starting || !G.g ? layout::H : layout::SHORT_H;
            display::measure();
            display::beginFrame();
            PadUpdate();
            SyncTyping();
            StepScript();
            Update();
            Draw();
        } catch (const std::exception& ex) {
            fprintf(stderr, "error: %s\n", ex.what());
        }
        display::present();
    }
    G.aiRun.reset();
    G.modals.clear();
    sound::shutdown();
    gfx::shutdown();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
