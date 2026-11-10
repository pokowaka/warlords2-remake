#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>

#include "front/map_screen_controller.hpp"
#include "ui/dialogs.hpp"
#include "warlords/ai.hpp"
#include "warlords/cues.hpp"
#include "warlords/game.hpp"
#include "warlords/scengen.hpp"
#include "warlords/scn.hpp"
#include "warlords/site.hpp"
#include "warlords/uidata.hpp"

namespace w2 {

namespace {

constexpr std::array<std::array<int, 2>, 8> kPadDirs = {{
    {0, -1},
    {1, -1},
    {1, 0},
    {1, 1},
    {0, 1},
    {-1, 1},
    {-1, 0},
    {-1, -1},
}};

}  // namespace

void MapScreenController::SyncLayout() {
    UiContext& ctx = Ctx();
    ctx.GetViewport().SyncLayout(ctx.Display(), ctx.Assets().MainScreen(), ctx.Assets().TextFont(),
                                 ctx.GamePtr(), ctx.Starting());
}

void MapScreenController::SetZoomAt(int zoom, double x, double y, bool centre) {
    UiContext& ctx = Ctx();
    Viewport& viewport = ctx.GetViewport();
    if (viewport.SetZoomAt(zoom, x, y, centre, ctx.Display().GetState(), ctx.GamePtr())) {
        ctx.SavePref("zoom", viewport.Zoom());
    }
}

void MapScreenController::SetZoom(int zoom) {
    SetZoomAt(zoom, 0.0, 0.0, true);
}

void MapScreenController::SetUiScale(int scale) {
    UiContext& ctx = Ctx();
    ctx.GetViewport().RelayoutPreservingCentre(
            ctx.Display(), ctx.Assets().MainScreen(), ctx.Assets().TextFont(), ctx.GamePtr(),
            ctx.Starting(), [&ctx, scale]() { ctx.Display().Choose(scale); });
    int chosen = ctx.Display().GetState().chosen;
    if (chosen == display::kSmooth) {
        ctx.SavePref("scale", "smooth");
    } else {
        ctx.SavePref("scale", chosen);
    }
}

void MapScreenController::SetOriginalSize(bool enabled) {
    UiContext& ctx = Ctx();
    ctx.GetViewport().RelayoutPreservingCentre(
            ctx.Display(), ctx.Assets().MainScreen(), ctx.Assets().TextFont(), ctx.GamePtr(),
            ctx.Starting(), [&ctx, enabled]() { ctx.Display().SetOriginalAspect(enabled); });
    ctx.SavePref("4:3", enabled ? "on" : "off");
}

void MapScreenController::SetScreen(bool window) {
    UiContext& ctx = Ctx();
    ctx.GetViewport().RelayoutPreservingCentre(
            ctx.Display(), ctx.Assets().MainScreen(), ctx.Assets().TextFont(), ctx.GamePtr(),
            ctx.Starting(), [&ctx, window]() { ctx.Display().SetWindowed(window); });
    ctx.SavePref("screen", window ? "window" : "full");
}

void MapScreenController::ToggleCrt() {
    UiContext& ctx = Ctx();
    bool next_crt = !ctx.Display().CrtEnabled();
    ctx.Display().SetCrtEnabled(next_crt);
    ctx.SavePref("crt", next_crt ? "on" : "off");
}

bool MapScreenController::ActiveClassicAi() const {
    const UiContext& ctx = Ctx();
    if (ctx.GamePtr() && ctx.GamePtr()->HasMap() && !ctx.Starting()) {
        for (const auto& side : ctx.GamePtr()->GetMap().Sides()) {
            if (side.InUse() && side.IsComputerControlled()) return side.IsClassicAi();
        }
        for (const auto& side : ctx.GamePtr()->GetMap().Sides()) {
            if (side.InUse()) return side.IsClassicAi();
        }
    }
    return ai::DefaultClassicAi();
}

void MapScreenController::SetAiMode(bool classic) {
    UiContext& ctx = Ctx();
    ai::SetDefaultClassicAi(classic);
    ctx.SavePref("ai", classic ? "classic" : "modern");
    if (ctx.GamePtr() && ctx.GamePtr()->HasMap()) {
        for (auto& side : ctx.ActiveGame().GetMap().Sides()) {
            ai::SetSideClassicAi(ctx.ActiveGame(), side, classic);
        }
    }
}

void MapScreenController::SetMapGenMode(bool modern, int shape) {
    UiContext& ctx = Ctx();
    StartWorldSettings& world = ctx.StartWorld();
    world.modern_mapgen = modern;
    if (modern) {
        world.shape = std::clamp(shape, 0, 2);
        ctx.SavePref("mapgen", scengen::ShapeName(static_cast<scengen::Shape>(world.shape)));
    } else {
        ctx.SavePref("mapgen", "classic");
    }
}

std::string MapScreenController::MapGenKey() const {
    const StartWorldSettings& world = Ctx().StartWorld();
    if (!world.modern_mapgen) return "mapgen classic";
    int clamped = std::clamp(world.shape, 0, 2);
    return std::string("mapgen ") + scengen::ShapeName(static_cast<scengen::Shape>(clamped));
}

std::string MapScreenController::UiScaleKey() const {
    const auto& state = Ctx().Display().GetState();
    if (state.chosen > state.max_scale && state.chosen <= state.max_short) {
        return "ui scale " + std::to_string(state.chosen);
    }
    return state.smooth ? "ui scale smooth" : "ui scale " + std::to_string(state.scale);
}

bool MapScreenController::MenuTicked(const std::string& key) const {
    const UiContext& ctx = Ctx();
    const auto& disp_state = ctx.Display().GetState();
    return key == "map zoom " + std::to_string(ctx.GetViewport().Zoom()) || key == UiScaleKey() ||
           key == std::string("screen ") + (disp_state.windowed ? "window" : "full") ||
           (key == "screen 4:3" && disp_state.wanted.has_value()) ||
           (key == "screen fit" && !disp_state.wanted.has_value()) ||
           (key == "screen crt" && disp_state.crt) ||
           key == "music " + std::string(sound::SynthName(ctx.Audio().Synth())) ||
           key == (ActiveClassicAi() ? "ai classic" : "ai modern") || key == MapGenKey();
}

void MapScreenController::ViewCity(int mode) {
    UiContext& ctx = Ctx();
    auto [centre_x, centre_y] = ctx.ViewCentre();
    int viewer = game::ViewerSide(ctx.ActiveGame(), ctx.Player());
    City* best = nullptr;
    int best_dist = kNone;
    for (auto& city : ctx.ActiveGame().GetMap().Cities()) {
        bool allowed = game::Seen(ctx.ActiveGame(), viewer, city.X(), city.Y()) &&
                       (mode == city::kInfo || city.OwnerIndex() == ctx.Player()->Index());
        if (allowed) {
            int dist = std::max(std::abs(city.X() - centre_x), std::abs(city.Y() - centre_y));
            if (best_dist == kNone || dist < best_dist) {
                best = &city;
                best_dist = dist;
            }
        }
    }
    if (best) city::Open(ctx, best, mode);
}

std::string MapScreenController::ShortcutKey(int slot_index) const {
    static const absl::flat_hash_map<int, std::string> kKeyMap = {
        {507, "m"}, {508, "q"}, {510, "a"}, {511, "k"}, {512, "g"}, {513, "n"}, {514, "w"},
        {516, "="}, {517, ","}, {518, "f"}, {521, "z"}, {524, "b"}, {525, "c"}, {526, "p"},
        {527, "v"}, {528, "."}, {529, "s"}, {530, "h"}, {531, "e"}, {534, "l"}, {535, "alt E"},
    };
    const auto& shortcuts = Ctx().Assets().MainScreen().ui.shortcuts;
    auto shortcut_it = shortcuts.find(slot_index);
    if (shortcut_it == shortcuts.end()) return "";
    auto key_it = kKeyMap.find(shortcut_it->second);
    return key_it == kKeyMap.end() ? "" : key_it->second;
}

void MapScreenController::InitActions() {
    control_actions_.clear();
    menu_actions_.clear();

    for (int i = 0; i < kSlotCount; i++) {
        control_actions_[kSlotFirst + i] = [this, i]() {
            if (Selection* sel = Ctx().ActiveSelection()) {
                slots::Toggle(sel->slots, Ctx().ActiveGame(), i);
                AfterSlotChange();
            }
        };
        control_actions_[kBarFirst + i] = [this, i]() {
            if (Selection* sel = Ctx().ActiveSelection()) {
                slots::PickGroup(sel->slots, Ctx().ActiveGame(), i);
                AfterSlotChange();
            }
        };
    }
    control_actions_[kGrpAll] = [this]() {
        if (Selection* sel = Ctx().ActiveSelection()) {
            if (slots::Grouped(sel->slots)) {
                slots::Single(sel->slots, Ctx().ActiveGame());
            } else {
                slots::All(sel->slots, Ctx().ActiveGame());
            }
            AfterSlotChange();
        }
    };
    control_actions_[kGrpNone] = control_actions_[kGrpAll];
    control_actions_[173] = [this]() {
        Selection* sel = Ctx().ActiveSelection();
        if (!sel || sel->stack.empty()) return;
        if (auto target = sel->stack[0]->Target()) MoveSelection(target->x, target->y);
    };
    control_actions_[174] = [this]() { SelectNext(); };
    control_actions_[175] = [this]() { QuitArmy(); };
    control_actions_[176] = [this]() { Fortify(); };
    control_actions_[178] = [this]() { Deselect(); };

    for (int i = 0; i < 8; i++) {
        control_actions_[320 + i] = [this, i]() {
            UiContext& ctx = Ctx();
            const auto& dir = kPadDirs[static_cast<size_t>(i)];
            ctx.GetViewport().PanTiles(dir[0], dir[1], ctx.GamePtr(),
                                       ctx.Display().GetState().scale);
        };
    }
    for (int i = 0; i < uidata::kShortcutCount; i++) {
        control_actions_[uidata::kShortcutFirst + i] = [this, i]() {
            std::string key = ShortcutKey(i);
            if (!key.empty() && MenuEnabled(key)) ExecuteMenuAction(key);
        };
    }
    control_actions_[188] = [this]() {
        UiContext& ctx = Ctx();
        help::Open(
                ctx, "HELP\\HMOUSE.GFX",
                [this]() { help::Open(Ctx(), "HELP\\HKEYS.GFX", nullptr, &help::kPopuP4); },
                &help::kPopuP4);
    };
    control_actions_[186] = [this]() {
        UiContext& ctx = Ctx();
        Selection* sel = ctx.ActiveSelection();
        if (!sel || sel->stack.empty()) return;
        Viewport& viewport = ctx.GetViewport();
        int ui_scale = ctx.Display().GetState().scale;
        auto target = sel->stack[0]->Target();
        double cam_x = viewport.Cx();
        double cam_y = viewport.Cy();
        if (target && viewport.InView(sel->x, sel->y, ui_scale)) {
            ctx.CentreOn(target->x, target->y);
            if (viewport.Cx() == cam_x && viewport.Cy() == cam_y) ctx.CentreOn(sel->x, sel->y);
        } else {
            ctx.CentreOn(sel->x, sel->y);
        }
    };
    control_actions_[187] = [this]() {
        Selection* sel = Ctx().ActiveSelection();
        if (!sel || sel->stack.empty() || !sel->stack[0]->HasTarget()) return;
        for (Army* army : sel->stack) army->ClearTarget();
        route_.reset();
    };
    for (int control_id = 183; control_id <= 185; control_id++) {
        control_actions_[control_id] = [this]() { diplomacyui::Action(Ctx()); };
    }
    control_actions_[177] = [this]() {
        if (Selection* sel = Ctx().ActiveSelection()) Ctx().CentreOn(sel->x, sel->y);
    };

    menu_actions_["alt E"] = [this]() { EndTurn(); };
    menu_actions_["m"] = [this]() { MoveAll(); };
    menu_actions_["^Q"] = [this]() {
        UiContext& ctx = Ctx();
        input::Options opts;
        opts.title = kit::Text(ctx, 0x2c, 0);
        for (int i = 1; i <= 4; i++) opts.lines.push_back(kit::Text(ctx, 0x2c, i));
        opts.confirm = true;
        opts.ok = [this](const std::string&) {
            advisor::Say(Ctx(), cues::kQuit, [this]() { Ctx().RequestQuit(); });
        };
        input::Open(ctx, opts);
    };
    menu_actions_["alt N"] = [this]() {
        UiContext& ctx = Ctx();
        input::Options opts;
        opts.title = kit::Text(ctx, 0x2d, 0);
        for (int i = 1; i <= 4; i++) opts.lines.push_back(kit::Text(ctx, 0x2d, i));
        opts.confirm = true;
        opts.ok = [this](const std::string&) {
            UiContext& inner_ctx = Ctx();
            inner_ctx.ClearSelection();
            inner_ctx.Modals().Clear();
            OpenStart();
        };
        input::Open(ctx, opts);
    };
    menu_actions_["alt S"] = [this]() { savegame::Save(Ctx()); };
    menu_actions_["alt L"] = [this]() {
        savegame::Load(Ctx(), [this](std::unique_ptr<Game> loaded_game) {
            UiContext& inner_ctx = Ctx();
            if (inner_ctx.Starting()) {
                inner_ctx.Modals().Clear();
                inner_ctx.SetStarting(false);
            }
            TakeLoaded(std::move(loaded_game));
        });
    };
    menu_actions_["z"] = [this]() { DoSearch(); };
    menu_actions_["c"] = [this]() { ViewCity(city::kInfo); };
    menu_actions_["b"] = [this]() { ViewCity(city::kCity); };
    menu_actions_["p"] = [this]() { ViewCity(city::kProduction); };
    menu_actions_["v"] = [this]() { ViewCity(city::kVector); };
    menu_actions_["f"] = [this]() {
        UiContext& ctx = Ctx();
        Selection* sel = ctx.ActiveSelection();
        if (!sel) return;
        for (Army* army : sel->stack) {
            if (army->Hero() && game::PlantFlag(ctx.ActiveGame(), *ctx.Player(), army)) {
                ctx.GetViewport().StratDirty();
                return;
            }
        }
    };
    menu_actions_["q"] = [this]() {
        UiContext& ctx = Ctx();
        Selection* sel = ctx.ActiveSelection();
        if (!sel || sel->stack.empty()) return;
        ArmyList stack = sel->stack;
        bool heroes = false;
        for (Army* army : stack) {
            if (army->Hero()) heroes = true;
        }
        input::Options opts;
        opts.title = "Disband";
        opts.confirm = true;
        opts.lines = {"Are you sure you", "want to disband this", "group?",
                      heroes ? "It contains heroes" : ""};
        opts.ok = [this, stack](const std::string&) {
            UiContext& inner_ctx = Ctx();
            game::Disband(inner_ctx.ActiveGame(), *inner_ctx.Player(), stack);
            inner_ctx.ClearSelection();
            inner_ctx.GetViewport().StratDirty();
        };
        input::Open(ctx, opts);
    };
    menu_actions_["i"] = [this]() { fightorder::Open(Ctx()); };
    menu_actions_["r"] = [this]() { resign::Open(Ctx()); };
    menu_actions_["."] = [this]() {
        UiContext& ctx = Ctx();
        auto [centre_x, centre_y] = ctx.ViewCentre();
        ruin::Open(ctx, ruin::Nearest(ctx, centre_x, centre_y));
    };
    menu_actions_["s"] = [this]() { stackui::Open(Ctx()); };
    menu_actions_["o"] = [this]() { armybonus::Open(Ctx()); };
    menu_actions_["t"] = [this]() { items::Open(Ctx()); };
    menu_actions_["x"] = [this]() {
        if (Selection* sel = Ctx().ActiveSelection()) signpost::Open(Ctx(), sel->stack);
    };
    menu_actions_["h"] = [this]() { historyui::Open(Ctx(), 0); };
    menu_actions_["e"] = [this]() { historyui::Open(Ctx(), 1); };
    menu_actions_["j"] = [this]() { historyui::Open(Ctx(), 2); };
    menu_actions_["y"] = [this]() { historyui::Open(Ctx(), 3); };
    menu_actions_["alt U"] = [this]() { shortcuts::Open(Ctx()); };
    menu_actions_["alt X"] = [this]() { settings::Open(Ctx()); };
    menu_actions_["?"] = [this]() { about::Open(Ctx()); };
    menu_actions_["l"] = [this]() { historyui::Triumphs(Ctx()); };
    menu_actions_["d"] = [this]() { diplomacyui::Open(Ctx()); };
    menu_actions_["="] = [this]() { questui::Open(Ctx()); };
    menu_actions_["u"] = [this]() { levels::Open(Ctx()); };
    menu_actions_[","] = [this]() { heroinfo::Open(Ctx()); };
    menu_actions_["a"] = [this]() { reports::Open(Ctx(), 0); };
    menu_actions_["k"] = [this]() { reports::Open(Ctx(), 1); };
    menu_actions_["g"] = [this]() { reports::Open(Ctx(), 2); };
    menu_actions_["n"] = [this]() { reports::Open(Ctx(), 3); };
    menu_actions_["w"] = [this]() { reports::Open(Ctx(), 4); };
    for (int level = 1; level <= 16; level++) {
        menu_actions_["map zoom " + std::to_string(level)] = [this, level]() { SetZoom(level); };
        menu_actions_["ui scale " + std::to_string(level)] = [this, level]() { SetUiScale(level); };
    }
    menu_actions_["ui scale smooth"] = [this]() { SetUiScale(display::kSmooth); };
    menu_actions_["screen full"] = [this]() { SetScreen(false); };
    menu_actions_["screen window"] = [this]() { SetScreen(true); };
    menu_actions_["screen 4:3"] = [this]() { SetOriginalSize(true); };
    menu_actions_["screen fit"] = [this]() { SetOriginalSize(false); };
    menu_actions_["screen crt"] = [this]() { ToggleCrt(); };
    for (sound::SynthKind synth :
         {sound::SynthKind::kFm, sound::SynthKind::kMt32, sound::SynthKind::kSc55}) {
        menu_actions_["music " + std::string(sound::SynthName(synth))] = [this, synth]() {
            Ctx().Audio().SetSynth(synth);
            Ctx().SavePref("music", sound::SynthName(synth));
        };
    }
    menu_actions_["ai modern"] = [this]() { SetAiMode(false); };
    menu_actions_["ai classic"] = [this]() { SetAiMode(true); };
    menu_actions_["mapgen classic"] = [this]() { SetMapGenMode(false); };
    menu_actions_["mapgen continents"] = [this]() {
        SetMapGenMode(true, static_cast<int>(scengen::Shape::kContinents));
    };
    menu_actions_["mapgen pangaea"] = [this]() {
        SetMapGenMode(true, static_cast<int>(scengen::Shape::kPangaea));
    };
    menu_actions_["mapgen islands"] = [this]() {
        SetMapGenMode(true, static_cast<int>(scengen::Shape::kIslands));
    };
}

void MapScreenController::ExecuteMenuAction(const std::string& menu_key) {
    auto action_it = menu_actions_.find(menu_key);
    if (action_it != menu_actions_.end()) action_it->second();
}

bool MapScreenController::MenuEnabled(const std::string& menu_key) {
    if (menu_key.empty() || !menu_actions_.contains(menu_key)) return false;
    UiContext& ctx = Ctx();
    bool music = menu_key.starts_with("music ");
    bool ai_opt = menu_key.starts_with("ai ");
    bool mapgen_opt = menu_key.starts_with("mapgen ");
    if (music) {
        auto synth = sound::ParseSynthKind(menu_key.substr(6));
        if (!synth || !ctx.Audio().SynthAvailable(*synth)) return false;
    }
    if (ctx.Starting() || !ctx.GamePtr()) {
        return menu_key == "^Q" || (menu_key == "alt L" && savegame::Used() > 0) ||
               menu_key.starts_with("ui scale ") || menu_key.starts_with("screen ") || music ||
               ai_opt || mapgen_opt;
    }
    if (state_->IsComputerTurn() && !HumansLeft()) {
        if (menu_key == "h" || menu_key == "e" || menu_key == "j" || menu_key == "y") {
            return ctx.ActiveGame().Turn() != 1;
        }
        if (menu_key == "d") return ctx.ActiveGame().GetMap().GameOptions().diplomacy != 0;
        return menu_key == "^Q" || menu_key == "alt X" || menu_key == "?" || menu_key == "c" ||
               menu_key == "." || menu_key == "a" || menu_key == "k" || menu_key == "g" ||
               menu_key == "n" || menu_key == "w" || menu_key == "l" ||
               menu_key.starts_with("map zoom ") || menu_key.starts_with("ui scale ") ||
               menu_key.starts_with("screen ") || music || ai_opt || mapgen_opt;
    }
    Game& game_state = ctx.ActiveGame();
    Map& map = game_state.GetMap();
    auto side_has_hero = [&]() {
        for (Army* army : game_state.Armies()) {
            if (army->OwnerIndex() == ctx.Player()->Index() && army->Hero()) return true;
        }
        return false;
    };
    auto selected = [&]() {
        const Selection* sel = ctx.ActiveSelection();
        return sel && !sel->stack.empty();
    };
    auto side_has_cities = [&]() {
        for (const auto& city : map.Cities()) {
            if (city.OwnerIndex() == ctx.Player()->Index() && !city.IsRazed()) return true;
        }
        return false;
    };
    auto can_search = [&]() {
        if (!selected()) return false;
        Army* lead = ctx.ActiveSelection()->stack[0];
        const Site* site = map.SiteAt(lead->X(), lead->Y());
        if (!site || site->searched) return false;
        return lead->Hero() || site->type == site::kTemple;
    };
    auto can_plant_flag = [&]() {
        if (!selected()) return false;
        Army* lead = ctx.ActiveSelection()->stack[0];
        if (!lead->Hero()) return false;
        auto map_items = map.Items();
        if (ctx.Player()->Index() >= static_cast<int>(map_items.size())) return false;
        Item* std_item = &map_items[static_cast<size_t>(ctx.Player()->Index())];
        if (!lead->HasItem(std_item)) return false;
        int terrain = scn::TerrainAt(map, lead->X(), lead->Y());
        if (terrain == move::kWater || terrain == move::kShore || terrain == move::kCity ||
            terrain == move::kSite) {
            return false;
        }
        for (const auto& item : map_items) {
            if (item.planted && item.x == lead->X() && item.y == lead->Y()) return false;
        }
        return true;
    };

    if (menu_key == "alt L") return savegame::Used() > 0;
    if (menu_key == "q" || menu_key == "s") return selected();
    if (menu_key == "x") {
        return selected() && scn::TerrainAt(map, ctx.ActiveSelection()->stack[0]->X(),
                                            ctx.ActiveSelection()->stack[0]->Y()) == move::kTower;
    }
    if (menu_key == "r" || menu_key == "b" || menu_key == "p" || menu_key == "v") {
        return side_has_cities();
    }
    if (menu_key == "d") return map.GameOptions().diplomacy != 0;
    if (menu_key == "," || menu_key == "u") return side_has_hero();
    if (menu_key == "f") return can_plant_flag();
    if (menu_key == "z") return can_search();
    if (menu_key == "h" || menu_key == "e" || menu_key == "j" || menu_key == "y") {
        return game_state.Turn() != 1;
    }
    if (menu_key == "alt E") return !game_state.IsWon();
    return true;
}

}  // namespace w2
