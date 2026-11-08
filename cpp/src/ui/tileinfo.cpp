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
    void draw() override {
        auto b = infobox::board(sign ? infobox::POPUP2 : infobox::LINES);
        gfx::setColor(1, 1, 1);
        if (b) gfx::draw(b, gfx::newQuad(0, 0, kW, kH), bx, by);
        gfx::setColor(1, 1, 1);
        paint();
    }
    bool mousereleased(int, int, int) override {
        kit::pop(this);
        return true;
    }
    void mousepressed(int, int, int) override { kit::pop(this); }
    void keypressed(const std::string&) override { kit::pop(this); }
};
}  // namespace

void open(int tx, int ty) {
    w2::Game& g = *G.g;
    int viewer = w2::game::viewerSide(g, G.player);
    if (!w2::game::seen(g, viewer, tx, ty)) return;
    const auto& l = G.layout;
    auto [fcx, fcy] = front::mapToUI(tx + 0.5, ty + 0.5);
    int cx = static_cast<int>(std::floor(fcx)), cy = static_cast<int>(std::floor(fcy));
    cx = (cx + 4) / 8 * 8;
    cx = std::max(kW / 2, std::min(l.w - kW / 2, cx));
    cy = std::max(kH / 2, std::min(l.h - 2 - kH / 2, cy));
    int bx = cx - kW / 2 - l.dialog.x, by = cy - kH / 2 - l.dialog.y;
    w2::Side* me = G.player;
    auto d = std::make_shared<TileBox>();
    d->bx = bx;
    d->by = by;

    auto armies = w2::game::armiesAt(g, tx, ty);
    int owner = armies.empty() ? w2::NONE : armies[0]->owner;
    bool tower = w2::game::towerAt(g, tx, ty);
    w2::City* city = w2::game::cityAt(g, tx, ty);
    w2::Site* site = g.map->siteAt.empty() ? nullptr : g.map->siteAt[ty * g.map->width + tx];
    int terrain = w2::scn::terrainAt(*g.map, tx, ty);
    auto colours = [](const w2::Side* s) -> const Font& {
        return kit::font(2).colours(s->colour, s->edge);
    };
    auto two_lines = [bx, by](const std::string& a, const std::string& b, bool plain_first) {
        const Font& f = kit::font(2);
        kit::centred(plain_first ? f : f.colours(7, 0), a, bx + kW / 2 - 8, by + 11);
        kit::centred(f, b, bx + kW / 2 - 8, by + 35);
    };

    if (!armies.empty() &&
        (viewer == w2::NONE || g.map->options.viewEnemies != 0 || owner == me->index || tower)) {
        if (viewer != w2::NONE && owner != me->index && tower && g.map->options.viewEnemies == 0) {
            d->paint = [two_lines]() { two_lines(kit::text(0x82, 0), kit::text(0x82, 1), false); };
        } else {
            d->paint = [armies, bx, by]() {
                int n = static_cast<int>(armies.size());
                int x = (bx + kW / 2 - n * 12 - 16 + 4) / 8 * 8;
                for (int i = 0; i < n; i++)
                    kit::army(armies[i]->type, armies[i]->owner, x + 24 * i, by + 16, 0);
            };
        }
    } else if (city) {
        d->paint = [city, me, bx, by, colours]() {
            const Font& f = kit::font(2);
            const w2::Side* own =
                    city->ownerIndex != w2::NONE ? &G.g->map->sides[city->ownerIndex] : me;
            kit::centred(colours(own), city->name, bx + kW / 2 - 8, by + 11);
            if (city->razed) {
                kit::centred(f, "Razed!", bx + kW / 2 - 8, by + 35);
                return;
            }
            if (city->ownerIndex != w2::NONE && city->ownerIndex < 8) {
                for (int x : {bx + 24, bx + kW - 48}) {
                    gfx::setColor(1, 1, 1);
                    gfx::draw(G.shieldsImg, gfx::newQuad(city->ownerIndex * 40 + 24, 46, 16, 16), x,
                              by + 10);
                }
            }
            gfx::setColor(1, 1, 1);
            gfx::draw(G.abits, gfx::newQuad(424, 0, 24, 11), bx + 48, by + 36);
            f.draw(w2::fmt("%d", city->income), bx + 80, by + 33);
            gfx::setColor(1, 1, 1);
            gfx::draw(G.abits, gfx::newQuad(424, 11, 24, 11), bx + 144, by + 36);
            f.draw(w2::fmt("%d", city->defence), bx + 176, by + 33);
            auto bs = G.screen->artFor(38);
            for (int i = 0; i < 8; i++) {
                if (G.g->map->sides[i].capital == city && bs) {
                    for (int x : {bx + 16, bx + kW - 56}) {
                        gfx::setColor(1, 1, 1);
                        gfx::draw(bs->image, gfx::newQuad(i * 32, 36, 32, 23), x, by + 10);
                    }
                }
            }
        };
    } else if (site) {
        d->paint = [site, me, bx, by, colours]() {
            const Font& f = kit::font(2);
            kit::centred(colours(me), site->name, bx + kW / 2 - 8, by + 11);
            if (site->content == w2::site::TEMPLE)
                kit::centred(f, "Blessings & Quests!", bx + kW / 2 - 8, by + 35);
            else if (site->searched)
                kit::centred(f, "Explored!", bx + kW / 2 - 8, by + 33);
            else
                kit::centred(f, "Unexplored!", bx + kW / 2 - 8, by + 35);
        };
    } else if (terrain == w2::move::TOWER && w2::game::signAt(g, tx, ty)) {
        w2::Sign* sign = w2::game::signAt(g, tx, ty);
        d->sign = true;
        d->paint = [sign, two_lines]() { two_lines(sign->lines[0], sign->lines[1], true); };
    } else {
        bool road = w2::scn::roadAt(*g.map, tx, ty) % 32 != 0;
        bool port = g.map->crossing[ty * g.map->width + tx];
        if (port) {
            d->paint = [two_lines]() {
                two_lines("Port", "A way for armies to put to sea", false);
            };
        } else {
            int t = road ? 0 : terrain;
            d->paint = [t, two_lines]() {
                two_lines(kit::text(0x80, t), kit::text(0x81, t), false);
            };
        }
    }
    d->infobox = true;
    kit::push(d);
}

}  // namespace tileinfo
