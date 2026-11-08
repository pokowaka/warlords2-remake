// The right button on the map: what is on a tile (740d:0037, button 2).
//
// Only on a tile the side has seen, and gone when the button comes up
// (740d:11cf). The box is 256x75, centred on the tile (740d:131a), with
// POPUP.PCK behind it through its mask. What it shows, first that applies:
// a stack the side may see, side by side (740d:0bad) -- an enemy in a tower
// only "Tower / Very hard to conquer!"; a city (740d:0c73) with its shields,
// income and defence; a site (740d:0fd7); a signpost over POPUP2.PCK
// (740d:0a1c); else the terrain (740d:10e1).
#include <cmath>

#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/move.hpp"
#include "warlords/scn.hpp"
#include "warlords/site.hpp"

namespace tileinfo {

namespace {
const int kW = 256, kH = 75;

struct TileBox : kit::Modal {
    int bx = 0, by = 0;
    bool sign = false;
    std::function<void()> paint;
    void Draw() override {
        auto b = infobox::BoardImage(sign ? infobox::kPopuP2 : infobox::kLines);
        gfx::SetColor(1, 1, 1);
        if (b) gfx::Draw(b, gfx::NewQuad(0, 0, kW, kH), bx, by);
        gfx::SetColor(1, 1, 1);
        paint();
    }
    bool Mousereleased(int, int, int) override {
        kit::Pop(this);
        return true;
    }
    void Mousepressed(int, int, int) override { kit::Pop(this); }
    void Keypressed(const std::string&) override { kit::Pop(this); }
};
}  // namespace

void Open(int tx, int ty) {
    w2::Game& g = *G.g;
    int viewer = w2::game::ViewerSide(g, G.player);
    if (!w2::game::Seen(g, viewer, tx, ty)) return;
    const auto& l = G.layout;
    auto [fcx, fcy] = front::MapToUi(tx + 0.5, ty + 0.5);
    int cx = static_cast<int>(std::floor(fcx)), cy = static_cast<int>(std::floor(fcy));
    cx = (cx + 4) / 8 * 8;
    cx = std::max(kW / 2, std::min(l.w - kW / 2, cx));
    cy = std::max(kH / 2, std::min(l.h - 2 - kH / 2, cy));
    int bx = cx - kW / 2 - l.dialog.x, by = cy - kH / 2 - l.dialog.y;
    w2::Side* me = G.player;
    auto d = std::make_shared<TileBox>();
    d->bx = bx;
    d->by = by;

    auto armies = w2::game::ArmiesAt(g, tx, ty);
    int owner = armies.empty() ? w2::kNone : armies[0]->owner;
    bool tower = w2::game::TowerAt(g, tx, ty);
    w2::City* city = w2::game::CityAt(g, tx, ty);
    w2::Site* site = g.map->site_at.empty() ? nullptr : g.map->site_at[ty * g.map->width + tx];
    int terrain = w2::scn::TerrainAt(*g.map, tx, ty);
    auto colours = [](const w2::Side* s) -> const Font& {
        return kit::GetFont(2).Colours(s->colour, s->edge);
    };
    auto two_lines = [bx, by](const std::string& a, const std::string& b, bool plain_first) {
        const Font& f = kit::GetFont(2);
        kit::Centred(plain_first ? f : f.Colours(7, 0), a, bx + kW / 2 - 8, by + 11);
        kit::Centred(f, b, bx + kW / 2 - 8, by + 35);
    };

    if (!armies.empty() &&
        (viewer == w2::kNone || g.map->options.view_enemies != 0 || owner == me->index || tower)) {
        if (viewer != w2::kNone && owner != me->index && tower &&
            g.map->options.view_enemies == 0) {
            d->paint = [two_lines]() { two_lines(kit::Text(0x82, 0), kit::Text(0x82, 1), false); };
        } else {
            d->paint = [armies, bx, by]() {
                int n = static_cast<int>(armies.size());
                int x = (bx + kW / 2 - n * 12 - 16 + 4) / 8 * 8;
                for (int i = 0; i < n; i++)
                    kit::Army(armies[i]->type, armies[i]->owner, x + 24 * i, by + 16, 0);
            };
        }
    } else if (city) {
        d->paint = [city, me, bx, by, colours]() {
            const Font& f = kit::GetFont(2);
            const w2::Side* own =
                    city->owner_index != w2::kNone ? &G.g->map->sides[city->owner_index] : me;
            kit::Centred(colours(own), city->name, bx + kW / 2 - 8, by + 11);
            if (city->razed) {
                kit::Centred(f, "Razed!", bx + kW / 2 - 8, by + 35);
                return;
            }
            if (city->owner_index != w2::kNone && city->owner_index < 8) {
                for (int x : {bx + 24, bx + kW - 48}) {
                    gfx::SetColor(1, 1, 1);
                    gfx::Draw(G.shields_img, gfx::NewQuad(city->owner_index * 40 + 24, 46, 16, 16),
                              x, by + 10);
                }
            }
            gfx::SetColor(1, 1, 1);
            gfx::Draw(G.abits, gfx::NewQuad(424, 0, 24, 11), bx + 48, by + 36);
            f.Draw(w2::Fmt("%d", city->income), bx + 80, by + 33);
            gfx::SetColor(1, 1, 1);
            gfx::Draw(G.abits, gfx::NewQuad(424, 11, 24, 11), bx + 144, by + 36);
            f.Draw(w2::Fmt("%d", city->defence), bx + 176, by + 33);
            auto bs = G.screen->ArtFor(38);
            for (int i = 0; i < 8; i++) {
                if (G.g->map->sides[i].capital == city && bs) {
                    for (int x : {bx + 16, bx + kW - 56}) {
                        gfx::SetColor(1, 1, 1);
                        gfx::Draw(bs->image, gfx::NewQuad(i * 32, 36, 32, 23), x, by + 10);
                    }
                }
            }
        };
    } else if (site) {
        d->paint = [site, me, bx, by, colours]() {
            const Font& f = kit::GetFont(2);
            kit::Centred(colours(me), site->name, bx + kW / 2 - 8, by + 11);
            if (site->content == w2::site::kTemple)
                kit::Centred(f, "Blessings & Quests!", bx + kW / 2 - 8, by + 35);
            else if (site->searched)
                kit::Centred(f, "Explored!", bx + kW / 2 - 8, by + 33);
            else
                kit::Centred(f, "Unexplored!", bx + kW / 2 - 8, by + 35);
        };
    } else if (terrain == w2::move::kTower && w2::game::SignAt(g, tx, ty)) {
        w2::Sign* sign = w2::game::SignAt(g, tx, ty);
        d->sign = true;
        d->paint = [sign, two_lines]() { two_lines(sign->lines[0], sign->lines[1], true); };
    } else {
        bool road = w2::scn::RoadAt(*g.map, tx, ty) % 32 != 0;
        bool port = g.map->crossing[ty * g.map->width + tx];
        if (port) {
            d->paint = [two_lines]() {
                two_lines("Port", "A way for armies to put to sea", false);
            };
        } else {
            int t = road ? 0 : terrain;
            d->paint = [t, two_lines]() {
                two_lines(kit::Text(0x80, t), kit::Text(0x81, t), false);
            };
        }
    }
    d->infobox = true;
    kit::Push(d);
}

}  // namespace tileinfo
