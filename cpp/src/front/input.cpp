/**
 * @file
 * @brief Mouse, keyboard, control-bar, menu-bar, and gamepad input handling.
 */
#include <algorithm>
#include <cmath>
#include <regex>

#include "front/internal.hpp"
#include "front/prefs.hpp"
#include "platform/display.hpp"
#include "platform/pad.hpp"
#include "platform/sound.hpp"
#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/armytype.hpp"
#include "warlords/cues.hpp"
#include "warlords/game.hpp"
#include "warlords/quest.hpp"
#include "warlords/rules.hpp"
#include "warlords/save.hpp"
#include "warlords/scn.hpp"
#include "warlords/site.hpp"
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

// ------------------------------------------------------------------ input

static void MenuPick(const std::string& menu_key);
static std::map<std::string, std::function<void()>>& MenuDoes();

/**
 * @brief Returns whether a human stack walk or Move All is currently in progress.
 *
 * No mouse clicks are handled while a stack walks; keys wait in the keyboard
 * buffer (up to 15) and are replayed once the walk finishes.
 */
static bool BusyWalking() {
    return ((g_app.walk && !g_app.walk->computer) || g_app.move_all) && !kit::Top();
}

/** @brief Replays any keys buffered while a stack walk was in progress. */
void FlushKeys() {
    auto buffered = g_app.key_buffer;
    g_app.key_buffer.clear();
    for (auto& key : buffered) Keypressed(key);
}

/**
 * @brief Handles keyboard input while a computer side is playing.
 *
 * Hurries a battle, or runs the rest of the round through unwatched.
 * @return True when it consumed the input.
 */
static bool ComputerInput(const std::string& key) {
    if (!(g_app.ai_run && !kit::Top() && g_app.open_menu < 0)) return false;
    if (g_app.assault && g_app.assault->computer) {
        PressAssault();
        return true;
    }
    // Allow opening the F10 AI Brain Inspector (under --modern-ui), panning, or adjusting zoom
    // while spectating AI turns
    if (g_app.modern_ui && key == "f10") {
        diplomacyui::OpenBrainInspector(g_app.ai_side ? g_app.ai_side->Index() : kNone);
        return true;
    }
    if (key == "kp+" || key == "pageup" || key == "=") {
        SetZoom(g_app.zoom + 1);
        return true;
    }
    if (key == "kp-" || key == "pagedown" || key == "-") {
        SetZoom(g_app.zoom - 1);
        return true;
    }
    if (key == "up") {
        g_app.cy -= 1;
        ClampCamera();
        return true;
    }
    if (key == "down") {
        g_app.cy += 1;
        ClampCamera();
        return true;
    }
    if (key == "left") {
        g_app.cx -= 1;
        ClampCamera();
        return true;
    }
    if (key == "right") {
        g_app.cx += 1;
        ClampCamera();
        return true;
    }
    // Shift and Alt are held to reach Settings, not pressed to skip
    if (key.ends_with("shift") || key.ends_with("alt") || key.ends_with("ctrl") ||
        key.ends_with("gui")) {
        return true;
    }
    if (!HumansLeft()) {
        // In AI-vs-AI spectator mode, let Space toggle fast-forward and allow read-only spectator
        // dialogs/quit
        if (key == "space") {
            g_app.ai_skip = !g_app.ai_skip;
            if (g_app.ai_skip && g_app.walk && g_app.walk->computer) {
                g_app.walk.reset();
                ResumeComputer();
            }
            return true;
        }
        if (key == "escape") {
            settings::Open();
            return true;
        }
        if (Held({"lctrl", "rctrl", "lgui", "rgui"}) && key == "q") return false;
        if (key == "c" || key == "." || key == "a" || key == "k" || key == "g" || key == "n" ||
            key == "w" || key == "d" || key == "h" || key == "e" || key == "j" || key == "y" ||
            key == "l" || key == "?") {
            return false;
        }
        return true;
    }
    g_app.ai_skip = true;
    if (g_app.walk && g_app.walk->computer) {
        g_app.walk.reset();
        ResumeComputer();
    }
    return true;
}

// Each control id maps to its click action (see docs/re/ui.md > Commands).
static std::map<int, std::function<void()>>& Actions();

/** @brief Handles a mouse button press at UI coordinates (`x`, `y`). */
void Mousepressed(int x, int y, int button) {
    if (BusyWalking()) return;
    SyncLayout();
    int frame_x = x - g_app.layout.dialog.x, frame_y = y - g_app.layout.dialog.y;
    if (g_app.ai_run && !kit::Top()) {
        if (g_app.assault && g_app.assault->computer) {
            PressAssault();
            return;
        }
        if (!HumansLeft()) {
            // AI-vs-AI spectator mouse controls: menu bar, right-click tile/stack inspect, minimap
            // jump, and map drag
            int hit = menu::TitleAt(g_app.menu_layout, x, y);
            if (hit >= 0) {
                g_app.open_menu = g_app.open_menu == hit ? -1 : hit;
                return;
            }
            if (g_app.open_menu >= 0) {
                const menu::Row* row = menu::RowAt(g_app.menu_layout, g_app.open_menu, x, y);
                g_app.open_menu = -1;
                std::string row_key = row ? (row->key.empty() ? row->act : row->key) : "";
                if (!row_key.empty() && MenuEnabled(row_key)) MenuPick(row_key);
                return;
            }
            if (g_app.g && g_app.screen) {
                if (auto reg = screen::RegionAt(*g_app.screen, x, y)) {
                    if (reg->id == screen::kMap) {
                        if (button == 2) {
                            if (auto tile = TileAtPoint(x, y))
                                tileinfo::Open(tile->first, tile->second);
                            return;
                        }
                        if (button == 1) {
                            g_app.drag = Drag{g_app.cx, g_app.cy, 0, 0};
                            return;
                        }
                    } else if (reg->id == screen::kStrategic && button == 1) {
                        int tile_x = (x - reg->x) / 2, tile_y = (y - reg->y + strat_scroll) / 2;
                        CentreOn(tile_x, tile_y);
                        return;
                    }
                }
            }
            return;
        }
        if (ComputerInput("")) return;
    }
    // The banner eats the click that dismisses it.
    if (g_app.banner) {
        DismissBanner();
        return;
    }
    if (g_app.assault) {
        PressAssault();
        return;
    }
    // The right button is not a click on a dialog's button.
    if ((g_app.victory || g_app.offer) && button == 2) return;
    if (g_app.victory) {
        auto ctrl = screen::DialogControlAt(*g_app.victory_view, frame_x, frame_y);
        if (ctrl && g_app.victory_view->StateOf(ctrl->id) != kDisabled) TakeCity(ctrl->id);
        return;
    }
    if (g_app.offer) {
        auto ctrl = screen::DialogControlAt(*g_app.hero_view, frame_x, frame_y);
        if (!ctrl) return;
        if (ctrl->id == kHeroField)
            offer_typing = true;
        else if (ctrl->id == kHeroMale)
            g_app.offer_female = false;
        else if (ctrl->id == kHeroFemale)
            g_app.offer_female = true;
        else if (ctrl->id == kHeroOk)
            AcceptOffer();
        else if (ctrl->id == kHeroCancel && !g_app.offer->first)
            RefuseOffer();
        return;
    }
    // On the start screens the menu bar stays live over them.
    auto top = kit::TopShared();
    if (top && top->menu_bar &&
        (g_app.open_menu >= 0 || menu::TitleAt(g_app.menu_layout, x, y) >= 0))
        top = nullptr;
    if (top) {
        // the right button on a dialog: a control's help, or the dialog's regions
        if (button == 2 && top->view) {
            auto ctrl = infobox::ControlAt(*top->view, frame_x, frame_y, &top->hidden);
            if (!(ctrl && infobox::ExplainControl(x, y, ctrl->id, top.get())))
                top->Rightpressed(frame_x, frame_y, x, y);
            return;
        }
        top->Mousepressed(frame_x, frame_y, button);
        return;
    }
    if (g_app.over) return;
    // the menu bar takes precedence over everything beneath it, the start
    // screens' too, with no game yet
    int hit = menu::TitleAt(g_app.menu_layout, x, y);
    if (hit >= 0) {
        g_app.open_menu = g_app.open_menu == hit ? -1 : hit;
        return;
    }
    if (g_app.open_menu >= 0) {
        const menu::Row* row = menu::RowAt(g_app.menu_layout, g_app.open_menu, x, y);
        g_app.open_menu = -1;
        std::string row_key = row ? (row->key.empty() ? row->act : row->key) : "";
        if (!row_key.empty() && MenuEnabled(row_key)) MenuPick(row_key);
        return;
    }
    if (!g_app.g) return;
    auto ctrl = screen::ControlAt(*g_app.screen, x, y);
    if (ctrl && button == 2) {
        if (infobox::ExplainControl(x, y, ctrl->id)) return;
    } else if (ctrl) {
        if (g_app.screen->StateOf(ctrl->id) == kDisabled) return;
        g_app.pressed = ctrl->id;
        g_app.screen->state[ctrl->id] = kActive;
        return;
    }
    // The hand pointer drags the map.
    if (button == 1 && PointerKind(x, y) == kHand) {
        g_app.drag = Drag{g_app.cx, g_app.cy, 0, 0};
        return;
    }
    auto reg = screen::RegionAt(*g_app.screen, x, y);
    if (!reg) return;
    if (reg->id == screen::kMap) {
        auto tile = TileAtPoint(x, y);
        if (!tile) return;
        auto [tile_x, tile_y] = *tile;
        if (button == 2) {
            tileinfo::Open(tile_x, tile_y);
            return;
        }
        // The left button does what the pointer shows.
        int ptr_kind = PointerKind(x, y);
        if (ptr_kind == kWalk || ptr_kind == kBoat)
            MoveSelection(tile_x, tile_y);
        else if (ptr_kind == kAttack || ptr_kind == kPeace)
            AttackTile(tile_x, tile_y);
        else if (ptr_kind == kCityP || ptr_kind == kSiteP) {
            if (City* city = game::CityAt(*g_app.g, tile_x, tile_y)) front::OpenCity(city);
        } else if (ptr_kind == kAdvise) {
            miladvisor::Open(g_app.selection ? &g_app.selection->stack : nullptr, tile_x, tile_y);
        } else if (ptr_kind == kSelect) {
            SelectClick(tile_x, tile_y);
        } else if (ptr_kind == kAlt) {
            PlanRoute(tile_x, tile_y);
            RefreshControls();
        }
    } else if (reg->id == screen::kStrategic) {
        int tile_x = (x - reg->x) / 2, tile_y = (y - reg->y + strat_scroll) / 2;
        if (button != 1) return;
        if (Held({"lalt", "ralt"})) {
            PlanRoute(tile_x, tile_y);
            RefreshControls();
        } else {
            CentreOn(tile_x, tile_y);
        }
    } else if (reg->id == screen::kBottombar && button == 2) {
        BarInfo(x, y);
    } else if (reg->id == screen::kMappanel && button == 2) {
        infobox::ShowLines(x, y, "- Drag Screen -", "Move mouse to drag the screen");
    }
}

void AfterSlotChange() {
    SyncSelection();
}

/**
 * @brief Advances Order > Move All to the next army of the side with a destination.
 *
 * Every army of the side with a destination walks there in turn, each walk
 * played out before the next.
 */
void MoveAllStep() {
    while (g_app.move_all) {
        if (g_app.walk) return;
        Army* lead = nullptr;
        for (Army* army : g_app.g->Armies()) {
            if (army->OwnerIndex() == g_app.player->Index() && army->HasTarget() &&
                !army->InTransit() && !g_app.move_all->seen.count(army)) {
                lead = army;
                break;
            }
        }
        if (!lead) {
            if (g_app.move_all->moved == 0) Say("Nothing is under orders.");
            g_app.move_all.reset();
            RefreshControls();
            return;
        }
        g_app.move_all->seen.insert(lead);
        Select(lead->X(), lead->Y(), lead);
        if (g_app.selection) {
            for (Army* army : g_app.selection->stack) g_app.move_all->seen.insert(army);
            // selecting a stack already where it was sent lifts the order
            // (refreshRoute), so there may be nothing left to walk to
            if (lead->HasTarget()) {
                auto target = *lead->Target();
                MoveSelection(target.x, target.y);
                if (g_app.move_all) g_app.move_all->moved++;
            }
        }
    }
}

/** @brief Starts Order > Move All, walking the selected stack first if it has a destination. */
static void MoveAll() {
    g_app.move_all = App::MoveAll{};
    if (g_app.selection && !g_app.selection->stack.empty() &&
        g_app.selection->stack[0]->HasTarget()) {
        auto target = *g_app.selection->stack[0]->Target();
        for (Army* army : g_app.selection->stack) g_app.move_all->seen.insert(army);
        MoveSelection(target.x, target.y);
        if (g_app.move_all) g_app.move_all->moved = 1;
    }
    MoveAllStep();
}

/** @brief Runs Hero > Search with the selected stack. */
static void DoSearch() {
    if (!g_app.selection) return;
    int x = g_app.selection->x, y = g_app.selection->y;
    search::Open(g_app.selection->stack);
    Reslot(SelectableAt(x, y));
    StratDirty();
}

/** @brief Returns the menu accelerator key assigned to shortcut button `n`. */
static std::string ShortcutKey(int n) {
    static const std::map<int, std::string> kKey = {
        {507, "m"}, {508, "q"}, {510, "a"}, {511, "k"}, {512, "g"}, {513, "n"}, {514, "w"},
        {516, "="}, {517, ","}, {518, "f"}, {521, "z"}, {524, "b"}, {525, "c"}, {526, "p"},
        {527, "v"}, {528, "."}, {529, "s"}, {530, "h"}, {531, "e"}, {534, "l"}, {535, "alt E"}};
    auto shortcut = g_app.screen->ui.shortcuts.find(n);
    if (shortcut == g_app.screen->ui.shortcuts.end()) return "";
    auto key_it = kKey.find(shortcut->second);
    return key_it == kKey.end() ? "" : key_it->second;
}

/** @brief Returns the table mapping main-screen control ids to their click handlers. */
static std::map<int, std::function<void()>>& Actions() {
    static std::map<int, std::function<void()>> action_table;
    if (!action_table.empty()) return action_table;
    for (int i = 0; i < kSlotCount; i++) {
        action_table[kSlotFirst + i] = [i]() {
            if (!g_app.selection) return;
            slots_mod::Toggle(g_app.selection->slots, *g_app.g, i);
            AfterSlotChange();
        };
        action_table[kBarFirst + i] = [i]() {
            if (!g_app.selection) return;
            slots_mod::PickGroup(g_app.selection->slots, *g_app.g, i);
            AfterSlotChange();
        };
    }
    action_table[kGrpAll] = []() {
        if (!g_app.selection) return;
        auto& slots = g_app.selection->slots;
        if (slots_mod::Grouped(slots))
            slots_mod::Single(slots, *g_app.g);
        else
            slots_mod::All(slots, *g_app.g);
        AfterSlotChange();
    };
    action_table[kGrpNone] = action_table[kGrpAll];
    // The five buttons above the pad, 173-178: walk on, next, done, defend,
    // deselect.
    action_table[173] = []() {
        if (!g_app.selection || g_app.selection->stack.empty()) return;
        auto target = g_app.selection->stack[0]->Target();
        if (!target) return;
        MoveSelection(target->x, target->y);
    };
    action_table[174] = SelectNext;
    action_table[175] = QuitArmy;
    action_table[176] = Fortify;
    action_table[178] = Deselect;
    // The 3x3 pad, 320-327, clockwise from north: it moves the view.
    static const int kPad[8][2] = {{0, -1}, {1, -1}, {1, 0},  {1, 1},
                                   {0, 1},  {-1, 1}, {-1, 0}, {-1, -1}};
    for (int i = 0; i < 8; i++) {
        action_table[320 + i] = [i]() {
            g_app.cx += kPad[i][0];
            g_app.cy += kPad[i][1];
            ClampCamera();
        };
    }
    for (int i = 0; i < w2::uidata::kShortcutCount; i++) {
        action_table[w2::uidata::kShortcutFirst + i] = [i]() {
            std::string key = ShortcutKey(i);
            if (!key.empty() && MenuEnabled(key)) MenuDoes()[key]();
        };
    }
    // The "?": the mouse's help page, then the keys'.
    action_table[188] = []() {
        help::Open(
                "HELP\\HMOUSE.GFX",
                []() { help::Open("HELP\\HKEYS.GFX", nullptr, &help::kPopuP4); }, &help::kPopuP4);
    };
    // 186 looks at where the stack is going, and back.
    action_table[186] = []() {
        if (!g_app.selection || g_app.selection->stack.empty()) return;
        auto& sel = *g_app.selection;
        auto target = sel.stack[0]->Target();
        double cam_x = g_app.cx, cam_y = g_app.cy;
        if (target && InView(sel.x, sel.y)) {
            CentreOn(target->x, target->y);
            if (g_app.cx == cam_x && g_app.cy == cam_y) CentreOn(sel.x, sel.y);
        } else {
            CentreOn(sel.x, sel.y);
        }
    };
    // 187 forgets the destination.
    action_table[187] = []() {
        if (!g_app.selection || g_app.selection->stack.empty() ||
            !g_app.selection->stack[0]->HasTarget())
            return;
        for (Army* army : g_app.selection->stack) army->ClearTarget();
        g_app.route.reset();
    };
    // 183-185 are one button in three faces: Diplomatic Action.
    for (int control_id = 183; control_id <= 185; control_id++)
        action_table[control_id] = diplomacyui::Action;
    // The pad's centre, 177, shares its handler with Home.
    action_table[177] = []() {
        if (g_app.selection) CentreOn(g_app.selection->x, g_app.selection->y);
    };
    return action_table;
}

/**
 * @brief Updates the enabled/active state of every main-screen control.
 *
 * A control with no handler in Actions() is greyed too.
 */
void RefreshControls() {
    auto& states = g_app.screen->state;
    // under the banner and the hero offer every control is still greyed
    if (g_app.banner || g_app.offer) {
        for (auto& [control_id, state] : states) state = kDisabled;
        return;
    }
    auto& action_table = Actions();
    auto set = [&](int control_id, bool live) {
        if (!states.count(control_id)) return;
        if (!(live && action_table.count(control_id)))
            states[control_id] = kDisabled;
        else if (control_id == g_app.pressed)
            states[control_id] = kActive;
        else
            states[control_id] = kNormal;
    };
    const Selection* sel = g_app.selection ? &*g_app.selection : nullptr;
    bool walk_on = sel && g_app.route &&
                   static_cast<int>(g_app.route->path.size()) >
                           (g_app.walk ? static_cast<int>(g_app.walk->i) + 1 : 0);
    set(173, walk_on);
    bool more = false;
    for (Army* army : g_app.g->Armies())
        if (army->OwnerIndex() == g_app.player->Index() && !army->InTransit() &&
            !army->IsFortified() && !army->IsDone()) {
            more = true;
            break;
        }
    set(174, more);
    set(175, more && sel);
    set(176, sel);
    set(177, sel);
    set(178, sel);
    set(186, sel);
    set(187, sel && !sel->stack.empty() && sel->stack[0]->HasTarget());
    set(188, true);
    for (int i = 0; i < kSlotCount; i++) {
        bool live = sel && i < sel->slots.n;
        set(kSlotFirst + i, live);
        set(kBarFirst + i, live);
    }
    bool grouped = sel && slots_mod::Grouped(sel->slots);
    set(240, sel && !grouped);
    set(241, sel && grouped);
    for (int i = 0; i < w2::uidata::kShortcutCount; i++) {
        std::string key = ShortcutKey(i);
        set(w2::uidata::kShortcutFirst + i, !key.empty() && MenuEnabled(key));
    }
    for (int i = 0; i < 8; i++) set(320 + i, true);
    int face = diplomacyui::ButtonFor(*g_app.g, g_app.player->Index());
    for (int control_id = 183; control_id <= 185; control_id++) set(control_id, control_id == face);
}

/** @brief Pans the map camera during an active mouse drag by (`dx`, `dy`) UI pixels. */
void Mousemoved(double dx, double dy) {
    if (!g_app.drag) return;
    g_app.drag->dx += dx;
    g_app.drag->dy += dy;
    double scale = static_cast<double>(display::state.scale) / (kTile * g_app.zoom);
    g_app.cx = g_app.drag->cx - g_app.drag->dx * scale;
    g_app.cy = g_app.drag->cy - g_app.drag->dy * scale;
    ClampCamera();
}

/** @brief Zooms the map around the tile under the pointer on mouse wheel scroll `wheel_y`. */
void Wheelmoved(int wheel_y) {
    SyncLayout();
    if (g_app.starting || !g_app.g || g_app.over || kit::Top() || g_app.banner || g_app.offer ||
        g_app.victory || wheel_y == 0)
        return;
    if (g_app.map_rect.Contains(g_app.mouse_x, g_app.mouse_y))
        SetZoomAt(g_app.zoom + (wheel_y > 0 ? 1 : -1), g_app.mouse_x, g_app.mouse_y, false);
    else
        SetZoomAt(g_app.zoom + (wheel_y > 0 ? 1 : -1), 0, 0, true);
}

/** @brief Handles a mouse button release at UI coordinates (`x`, `y`). */
void Mousereleased(int x, int y, int button) {
    SyncLayout();
    if (g_app.drag) {
        g_app.drag.reset();
        if (g_app.pressed == kNone) return;
    }
    if (auto top = kit::TopShared()) {
        if (top->Mousereleased(x - g_app.layout.dialog.x, y - g_app.layout.dialog.y, button))
            return;
    }
    int control_id = g_app.pressed;
    if (control_id == kNone) return;
    g_app.pressed = kNone;
    g_app.screen->state[control_id] = kNormal;
    auto ctrl = screen::ControlAt(*g_app.screen, x, y);
    if (!ctrl || ctrl->id != control_id) return;  // released off the button
    auto& action_table = Actions();
    auto act = action_table.find(control_id);
    if (act != action_table.end()) act->second();
    if (g_app.g) RefreshControls();
}

/** @brief Finds the city nearest the view's centre and opens the city dialog in `mode`. */
static void ViewCity(int mode) {
    auto [centre_x, centre_y] = front::ViewCentre();
    int viewer = game::ViewerSide(*g_app.g, g_app.player);
    City* best = nullptr;
    int best_d = kNone;
    for (auto& city : g_app.g->GetMap().Cities()) {
        bool allowed = game::Seen(*g_app.g, viewer, city.X(), city.Y()) &&
                       (mode == city::kInfo || city.OwnerIndex() == g_app.player->Index());
        if (allowed) {
            int dist = std::max(std::abs(city.X() - centre_x), std::abs(city.Y() - centre_y));
            if (best_d == kNone || dist < best_d) {
                best = &city;
                best_d = dist;
            }
        }
    }
    if (best) city::Open(best, mode);
}

void Quit() {
    g_app.modals.clear();
    g_app.ai_run.reset();
    ai_co = nullptr;
    g_app.ai_status.reset();
    g_app.assault.reset();
    g_app.walk.reset();
    g_app.move_all.reset();
    g_app.banner.reset();
    g_app.offer.reset();
    g_app.victory.reset();
    g_app.victory_under.reset();
    g_app.over = false;
    OpenStart();
}

/** @brief Returns the table mapping menu accelerators and action keys to their handlers. */
static std::map<std::string, std::function<void()>>& MenuDoes() {
    static std::map<std::string, std::function<void()>> menu_actions;
    if (!menu_actions.empty()) return menu_actions;
    menu_actions["alt E"] = EndTurn;
    menu_actions["m"] = MoveAll;
    // Game > Quit and New game ask for confirmation first.
    menu_actions["^Q"] = []() {
        input::Options opts;
        opts.title = kit::Text(0x2c, 0);
        for (int i = 1; i <= 4; i++) opts.lines.push_back(kit::Text(0x2c, i));
        opts.confirm = true;
        opts.ok = [](const std::string&) {
            // "Farewell, Warlord!" -- and the game closes.
            advisor::Say(w2::cues::kQuit, []() { g_app.quit_requested = true; });
        };
        input::Open(opts);
    };
    menu_actions["alt N"] = []() {
        input::Options opts;
        opts.title = kit::Text(0x2d, 0);
        for (int i = 1; i <= 4; i++) opts.lines.push_back(kit::Text(0x2d, i));
        opts.confirm = true;
        opts.ok = [](const std::string&) {
            g_app.selection.reset();
            g_app.banner.reset();
            g_app.offer.reset();
            OpenStart();
        };
        input::Open(opts);
    };
    menu_actions["alt S"] = savegame::Save;
    menu_actions["alt L"] = []() {
        savegame::Load([](std::unique_ptr<w2::Game> loaded_game) {
            if (g_app.starting) {
                g_app.modals.clear();
                g_app.starting = false;
            }
            TakeLoaded(std::move(loaded_game));
        });
    };
    menu_actions["z"] = DoSearch;
    menu_actions["c"] = []() { ViewCity(city::kInfo); };
    menu_actions["b"] = []() { ViewCity(city::kCity); };
    menu_actions["p"] = []() { ViewCity(city::kProduction); };
    menu_actions["v"] = []() { ViewCity(city::kVector); };
    // Hero > Plant Flag
    menu_actions["f"] = []() {
        if (!g_app.selection) return;
        for (Army* army : g_app.selection->stack) {
            if (army->Hero() && game::PlantFlag(*g_app.g, *g_app.player, army)) {
                StratDirty();
                return;
            }
        }
    };
    // Order > Disband
    menu_actions["q"] = []() {
        if (!g_app.selection || g_app.selection->stack.empty()) return;
        auto stack = g_app.selection->stack;
        bool heroes = false;
        for (Army* army : stack)
            if (army->Hero()) heroes = true;
        input::Options opts;
        opts.title = "Disband";
        opts.confirm = true;
        opts.lines = {"Are you sure you", "want to disband this", "group?",
                      heroes ? "It contains heroes" : ""};
        opts.ok = [stack](const std::string&) {
            game::Disband(*g_app.g, *g_app.player, stack);
            g_app.selection.reset();
            StratDirty();
        };
        input::Open(opts);
    };
    menu_actions["i"] = fightorder::Open;
    menu_actions["r"] = resign::Open;
    menu_actions["."] = []() {
        auto [centre_x, centre_y] = front::ViewCentre();
        ruin::Open(ruin::Nearest(centre_x, centre_y));
    };
    menu_actions["s"] = stackui::Open;
    menu_actions["o"] = armybonus::Open;
    menu_actions["t"] = items::Open;
    menu_actions["x"] = []() {
        if (g_app.selection) signpost::Open(g_app.selection->stack);
    };
    menu_actions["h"] = []() { historyui::Open(0); };
    menu_actions["e"] = []() { historyui::Open(1); };
    menu_actions["j"] = []() { historyui::Open(2); };
    menu_actions["y"] = []() { historyui::Open(3); };
    menu_actions["alt U"] = shortcuts::Open;
    menu_actions["alt X"] = []() { settings::Open(); };
    menu_actions["?"] = about::Open;
    menu_actions["l"] = historyui::Triumphs;
    menu_actions["d"] = diplomacyui::Open;
    menu_actions["="] = questui::Open;
    menu_actions["u"] = levels::Open;
    menu_actions[","] = heroinfo::Open;
    menu_actions["a"] = []() { reports::Open(0); };
    menu_actions["k"] = []() { reports::Open(1); };
    menu_actions["g"] = []() { reports::Open(2); };
    menu_actions["n"] = []() { reports::Open(3); };
    menu_actions["w"] = []() { reports::Open(4); };
    for (int level = 1; level <= 16; level++) {
        menu_actions["map zoom " + std::to_string(level)] = [level]() { front::SetZoom(level); };
        menu_actions["ui scale " + std::to_string(level)] = [level]() { SetUiScale(level); };
    }
    menu_actions["ui scale smooth"] = []() { SetUiScale(display::kSmooth); };
    menu_actions["screen full"] = []() { SetScreen(false); };
    menu_actions["screen window"] = []() { SetScreen(true); };
    menu_actions["screen 4:3"] = []() { SetOriginalSize(true); };
    menu_actions["screen fit"] = []() { SetOriginalSize(false); };
    menu_actions["screen crt"] = ToggleCrt;
    for (sound::SynthKind synth :
         {sound::SynthKind::kFm, sound::SynthKind::kMt32, sound::SynthKind::kSc55}) {
        menu_actions["music " + std::string(sound::SynthName(synth))] = [synth]() {
            sound::SetSynth(synth);
        };
    }
    menu_actions["ai modern"] = []() { SetAiMode(false); };
    menu_actions["ai classic"] = []() { SetAiMode(true); };
    return menu_actions;
}

/** @brief Invokes the menu action for `menu_key` if registered. */
static void MenuPick(const std::string& menu_key) {
    auto& menu_actions = MenuDoes();
    auto action_it = menu_actions.find(menu_key);
    if (action_it != menu_actions.end()) action_it->second();
}

/** @brief Returns whether the menu item with key or action `menu_key` can be chosen now. */
bool MenuEnabled(const std::string& menu_key) {
    auto& menu_actions = MenuDoes();
    if (menu_key.empty() || !menu_actions.count(menu_key)) return false;
    bool music = menu_key.starts_with("music ");
    bool ai_opt = menu_key.starts_with("ai ");
    if (music) {
        auto synth = sound::ParseSynthKind(menu_key.substr(6));
        if (!synth || !sound::SynthAvailable(*synth)) return false;
    }
    // On the start screens every menu is greyed except Quit, Load, and display/audio/AI settings.
    if (g_app.starting || !g_app.g) {
        return menu_key == "^Q" || (menu_key == "alt L" && savegame::Used() > 0) ||
               menu_key.starts_with("ui scale ") || menu_key.starts_with("screen ") || music ||
               ai_opt;
    }
    // While AI-vs-AI spectating is running, only enable read-only spectator reports, display/audio
    // settings, and quit
    if (g_app.ai_run && !HumansLeft()) {
        if (menu_key == "h" || menu_key == "e" || menu_key == "j" || menu_key == "y")
            return g_app.g->Turn() != 1;
        if (menu_key == "d") return g_app.g->GetMap().GameOptions().diplomacy != 0;
        return menu_key == "^Q" || menu_key == "alt X" || menu_key == "?" || menu_key == "c" ||
               menu_key == "." || menu_key == "a" || menu_key == "k" || menu_key == "g" ||
               menu_key == "n" || menu_key == "w" || menu_key == "l" ||
               menu_key.starts_with("map zoom ") || menu_key.starts_with("ui scale ") ||
               menu_key.starts_with("screen ") || music || ai_opt;
    }
    w2::Game& game_state = *g_app.g;
    w2::Map& map = game_state.GetMap();
    auto side_has_hero = [&]() {
        for (Army* army : game_state.Armies())
            if (army->OwnerIndex() == g_app.player->Index() && army->Hero()) return true;
        return false;
    };
    auto selected = []() { return g_app.selection && !g_app.selection->stack.empty(); };
    auto side_has_cities = [&]() {
        for (const auto& city : map.Cities())
            if (city.OwnerIndex() == g_app.player->Index() && !city.IsRazed()) return true;
        return false;
    };
    auto can_search = [&]() {
        if (!selected()) return false;
        Army* lead = g_app.selection->stack[0];
        const w2::Site* site = map.SiteAt(lead->X(), lead->Y());
        if (!site || site->searched) return false;
        return lead->Hero() || site->type == w2::site::kTemple;
    };
    auto can_plant_flag = [&]() {
        if (!selected()) return false;
        Army* lead = g_app.selection->stack[0];
        if (!lead->Hero()) return false;
        auto map_items = map.Items();
        if (g_app.player->Index() >= static_cast<int>(map_items.size())) return false;
        w2::Item* std_item = &map_items[g_app.player->Index()];
        if (!lead->HasItem(std_item)) return false;
        int terrain = w2::scn::TerrainAt(map, lead->X(), lead->Y());
        if (terrain == move::kWater || terrain == move::kShore || terrain == move::kCity ||
            terrain == move::kSite)
            return false;
        for (const auto& item : map_items)
            if (item.planted && item.x == lead->X() && item.y == lead->Y()) return false;
        return true;
    };
    if (menu_key == "alt L") return savegame::Used() > 0;
    if (menu_key == "q" || menu_key == "s") return selected();
    if (menu_key == "x")
        return selected() && w2::scn::TerrainAt(map, g_app.selection->stack[0]->X(),
                                                g_app.selection->stack[0]->Y()) == move::kTower;
    if (menu_key == "r" || menu_key == "b" || menu_key == "p" || menu_key == "v")
        return side_has_cities();
    if (menu_key == "d") return map.GameOptions().diplomacy != 0;
    if (menu_key == "," || menu_key == "u") return side_has_hero();
    if (menu_key == "f") return can_plant_flag();
    if (menu_key == "z") return can_search();
    if (menu_key == "h" || menu_key == "e" || menu_key == "j" || menu_key == "y")
        return game_state.Turn() != 1;
    if (menu_key == "alt E") return !game_state.IsWon();
    return true;
}

/** @brief Handles a key press identified by LÖVE key name `key`. */
void Keypressed(const std::string& key) {
    if (BusyWalking()) {
        if (g_app.key_buffer.size() < 15) g_app.key_buffer.push_back(key);
        return;
    }
    SyncLayout();
    if (ComputerInput(key)) return;
    if (g_app.banner) {
        DismissBanner();
        return;
    }
    if (g_app.assault) {
        PressAssault();
        return;
    }
    if (g_app.victory) {
        // Occupy heads both the default and the cancel lists
        if (key == "return" || key == "kpenter" || key == "escape") TakeCity(g_app.victory->enter);
        return;
    }
    if (g_app.offer) {
        if (key == "return" || key == "kpenter")
            AcceptOffer();
        else if (key == "backspace" && !g_app.offer_name.empty())
            g_app.offer_name.pop_back();
        else if (key == "escape" && !g_app.offer->first)
            RefuseOffer();
        return;
    }
    bool alt = Held({"lalt", "ralt"}), ctrl = Held({"lctrl", "rctrl", "lgui", "rgui"}),
         shift = Held({"lshift", "rshift"});
    auto top = kit::TopShared();
    if (top) {
        if (top->menu_bar) {
            std::string accel;
            if (ctrl && key == "q")
                accel = "^Q";
            else if (alt && key.size() == 1)
                accel = "alt " + w2::Upper(key);
            if (!accel.empty()) {
                g_app.open_menu = -1;
                if (MenuEnabled(accel)) MenuDoes()[accel]();
                return;
            }
        }
        if (g_app.open_menu >= 0) {
            g_app.open_menu = -1;
            return;
        }
        top->Keypressed(key);
        return;
    }
    if (g_app.open_menu >= 0) {
        g_app.open_menu = -1;
        return;
    }
    if (g_app.over || !g_app.g) return;
    // The main screen's keys.
    auto press = [](int control_id) {
        auto& action_table = Actions();
        if (g_app.screen->StateOf(control_id) != kDisabled && action_table.count(control_id))
            action_table[control_id]();
        RefreshControls();
    };
    std::string digit;
    if (key.size() == 1 && key[0] >= '0' && key[0] <= '9')
        digit = key;
    else if (key.size() == 3 && key.starts_with("kp") && key[2] >= '0' && key[2] <= '9')
        digit = key.substr(2);
    if (key == "return" || key == "kpenter")
        press(174);
    else if (key == "kp+" || key == "pageup" || key == "=")
        SetZoom(g_app.zoom + 1);
    else if (key == "kp-" || key == "pagedown" || key == "-")
        SetZoom(g_app.zoom - 1);
    else if (key == "escape")
        press(175);
    else if (key == "space")
        press(240);  // group the stack
    else if (key == "tab")
        press(186);
    else if (key == "backspace")
        press(187);
    else if (key == "home")
        press(177);
    else if (key == "end")
        press(173);  // walk on along the route
    else if (key == "delete")
        Deselect();  // as control 178 is
    else if (digit == "5") {
        if (g_app.selection) CentreOn(g_app.selection->x, g_app.selection->y);
    } else if (!digit.empty()) {
        // 1-9 step the stack the way the numeric pad lies.
        static const std::map<char, std::pair<int, int>> kStep = {
            {'8', {0, -1}}, {'9', {1, -1}}, {'6', {1, 0}},  {'3', {1, 1}},
            {'2', {0, 1}},  {'1', {-1, 1}}, {'4', {-1, 0}}, {'7', {-1, -1}}};
        auto step_it = kStep.find(digit[0]);
        if (step_it != kStep.end() && g_app.selection) {
            for (Army* army : g_app.selection->stack) army->ClearTarget();
            MoveSelection(g_app.selection->x + step_it->second.first,
                          g_app.selection->y + step_it->second.second);
        }
    } else if (key == "up")
        press(320);
    else if (key == "right")
        press(322);
    else if (key == "down")
        press(324);
    else if (key == "left")
        press(326);
    else if (key == "f5") {
        // not the original's: quick save and load
        if (savegame::WriteSlot("quick", *g_app.g))
            Say("Saved.");
        else
            Say("Could not save.");
    } else if (key == "f9") {
        if (auto loaded = savegame::ReadSlot("quick")) front::TakeLoaded(std::move(loaded));
    } else if (g_app.modern_ui && key == "f10") {
        diplomacyui::OpenBrainInspector();
    } else {
        std::string accel;
        if (ctrl && key == "q")
            accel = "^Q";
        else if (alt && key.size() == 1)
            accel = "alt " + w2::Upper(key);
        else if (shift && key == "/")
            accel = "?";
        else if (key.size() == 1)
            accel = key;
        // a key is its menu item, and does nothing while the item is greyed
        if (!accel.empty() && MenuEnabled(accel)) MenuDoes()[accel]();
    }
}

/** @brief Appends typed text to the hero offer's name field or the top modal dialog. */
void Textinput(const std::string& text) {
    auto top = kit::TopShared();
    if (top && !g_app.offer) {
        top->Textinput(text);
        return;
    }
    if (!g_app.offer) return;
    static const std::regex kOk("^[A-Za-z0-9\\s'\\-.]+$");
    if (g_app.offer_name.size() < kHeroNameMax && std::regex_match(text, kOk))
        g_app.offer_name += text;
}

}  // namespace front
