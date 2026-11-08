#include "front/screen.hpp"

#include <algorithm>
#include <cmath>

#include "front/pckimage.hpp"
#include "front/stonetile.hpp"
#include "util/util.hpp"
#include "warlords/game.hpp"
#include "warlords/scn.hpp"

namespace screen {

using w2::uidata::Control;
using w2::uidata::Region;

namespace {
// Find a .pck by name, wherever it lives: PICS/, TERRAIN<n>/ or the root.
std::string FindArt(const std::string& data_dir, const std::string& name, int terrain) {
    std::string up = w2::upper(name);
    for (const std::string& p :
         {data_dir + "/PICS/" + up, data_dir + "/TERRAIN" + std::to_string(terrain) + "/" + up,
          data_dir + "/" + up}) {
        if (w2::fileExists(p)) return p;
    }
    return "";
}

void SetPal(const Screen& s, int i) {
    const auto& c = s.palette[i];
    gfx::setColor(static_cast<float>(c[0]), static_cast<float>(c[1]), static_cast<float>(c[2]));
}

const int kLight = 2, kDark = 4;

struct Panel {
    const char* group;
    int x, y, w, h;
};
const Panel kPanels[] = {
    {"strat", 399, 29, 226, 314},
    {"panel", 399, 354, 226, 116},
    {"bar", 15, 402, 362, 68},
};

void ComposedArt(Screen& s) {
    if (s.bgImage) return;
    w2::pck::Pixels px;
    px.w = 640;
    px.h = 480;
    px.px.assign(640UZ * 480, 0);
    for (int i = 0; i < 4; i++) {
        auto& q = s.backgroundPx[i];
        int qx = (i % 2) * 320, qy = (i / 2) * 240;
        if (q) {
            for (int y = 0; y < 240 && y < q->h; y++)
                for (int x = 0; x < 320 && x < q->w; x++)
                    px.px[(qy + y) * 640 + qx + x] = q->px[y * q->w + x];
        }
    }
    s.bgImage = pckimage::fromPixels(px, s.palette);
    w2::pck::Pixels tile;
    tile.w = stonetile::W;
    tile.h = stonetile::H;
    tile.px.assign(static_cast<size_t>(stonetile::W) * stonetile::H, 0);
    for (int r = 0; r < stonetile::COUNT; r++) {
        const int* t = stonetile::RECTS[r];
        int tx = t[0], ty = t[1], w = t[2], h = t[3], sx = t[4], sy = t[5];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                tile.px[(ty + y) * stonetile::W + tx + x] = px.px[(sy + y) * 640 + sx + x];
    }
    s.stoneImg = pckimage::fromPixels(tile, s.palette);
    s.stoneImg->wrap = true;
}

void Bevel(const Screen& s, int x, int y, int w, int h) {
    SetPal(s, kDark);
    gfx::rectangle(gfx::FILL, x - 1, y - 1, w + 2, 1);
    gfx::rectangle(gfx::FILL, x - 1, y, 1, h + 1);
    SetPal(s, kLight);
    gfx::rectangle(gfx::FILL, x, y + h, w + 1, 1);
    gfx::rectangle(gfx::FILL, x + w, y, 1, h);
}

bool SameRect(const Control& a, const Control& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

// Some controls share a rect exactly -- 183/184/185, 240/241. The live one
// is what shows and what a click reaches; with none live, the first.
bool IsCovered(const Screen& s, const Control& c, size_t index) {
    bool live = s.stateOf(c.id) != w2::uidata::DISABLED;
    const auto& cs = s.dialog.controls;
    for (size_t i = 0; i < cs.size(); i++) {
        const Control& o = cs[i];
        if (i != index && SameRect(o, c)) {
            bool o_live = s.stateOf(o.id) != w2::uidata::DISABLED;
            if (o_live && (!live || i < index)) return true;
            if (!o_live && !live && i < index) return true;
        }
    }
    return false;
}

// The ring round a default button (1a0a:0005).
void DefaultRing(const Screen& s, const Control& c) {
    SetPal(s, 0);
    auto across = [](int x, int y, int n) { gfx::rectangle(gfx::FILL, x, y, n, 1); };
    auto down = [](int x, int y, int n) { gfx::rectangle(gfx::FILL, x, y, 1, n); };
    int ring_x = c.x - 2, ring_y = c.y - 2, ring_w = c.w + 3, ring_h = c.h + 3;
    for (int k = 0; k < 2; k++) {
        across(ring_x + 1, ring_y, ring_w - 2);
        down(ring_x + ring_w - 1, ring_y, 2);
        across(ring_x + ring_w - 1, ring_y + 1, 2);
        down(ring_x + ring_w, ring_y + 1, ring_h - 2);
        across(ring_x + ring_w - 1, ring_y + ring_h - 1, 2);
        down(ring_x + ring_w - 1, ring_y + ring_h - 1, 2);
        across(ring_x + 1, ring_y + ring_h, ring_w - 2);
        down(ring_x + 1, ring_y + ring_h - 1, 2);
        across(ring_x, ring_y + ring_h - 1, 2);
        down(ring_x, ring_y + 1, ring_h - 2);
        across(ring_x, ring_y + 1, 2);
        down(ring_x + 1, ring_y, 2);
        ring_x--;
        ring_y--;
        ring_w += 2;
        ring_h += 2;
    }
    gfx::setColor(1, 1, 1);
}
}  // namespace

const Art* Screen::artFor(int bitmap_id) {
    auto it = art.find(bitmap_id);
    if (it != art.end()) return it->second.image ? &it->second : nullptr;
    Art a;
    auto nm = ui.bitmaps.find(bitmap_id);
    if (nm != ui.bitmaps.end()) {
        std::string path = FindArt(dataDir, nm->second, terrain);
        if (!path.empty()) {
            auto px = w2::pck::decode(path);
            a.image = pckimage::fromPixels(*px, palette);
            a.w = px->w;
            a.h = px->h;
        }
    }
    art[bitmap_id] = a;
    return a.image ? &art[bitmap_id] : nullptr;
}

std::unique_ptr<Screen> load(const std::string& data_dir, const w2::pal::Palette& palette,
                             int dialog_id, int terrain) {
    auto s = std::make_unique<Screen>();
    s->ui = w2::uidata::load(data_dir);
    s->dialog = w2::uidata::dialog(s->ui, dialog_id);
    s->original = s->dialog;
    s->dataDir = data_dir;
    s->palette = palette;
    s->terrain = terrain;
    // The background is four 320x240 quadrants of one 640x480 image.
    for (int i = 0; i < 4; i++) {
        std::string path = FindArt(data_dir, "screen" + std::to_string(i) + ".pck", terrain);
        if (!path.empty()) {
            s->backgroundPx[i] = w2::pck::decode(path);
            s->background[i] = pckimage::fromPixels(*s->backgroundPx[i], palette);
        }
    }
    // TERRAIN<n>/MAPCOLOR.DAT, the strategic map's own colour table (834b:2a11):
    // a colour per tile id for each of a tile's four pixels, a remap, and the
    // same four pixels for a tile with a road on it.
    if (auto d = w2::readFile(data_dir + "/TERRAIN" + std::to_string(terrain) + "/MAPCOLOR.DAT")) {
        auto b = [&](size_t i) {
            return i < d->size() ? static_cast<int>(static_cast<uint8_t>((*d)[i])) : 0;
        };
        for (int q = 0; q < 4; q++) {
            for (int i = 0; i < 256; i++) s->mapColour.tile[q][i] = b(q * 256 + i);
            for (int i = 0; i < 20; i++) s->mapColour.road[q][i] = b(1120 + q * 20 + i);
        }
        for (int i = 0; i < 16; i++) s->mapColour.remap[i] = b(1024 + i * 2);
        s->mapColour.loaded = true;
    } else {
        for (int i = 0; i < 16; i++) s->mapColour.remap[i] = i;
    }
    for (auto& c : s->dialog.controls) s->state[c.id] = w2::uidata::NORMAL;
    return s;
}

void relayout(Screen& s, const layout::Layout& l) {
    s.dialog = s.original;
    for (auto& c : s.dialog.controls) {
        layout::Rect r =
                layout::move(l, layout::group(c.x, c.y), layout::Rect{c.x, c.y, c.w, c.h, c.id});
        c.x = r.x;
        c.y = r.y;
    }
    for (auto& r : s.dialog.regions) {
        layout::Rect o = layout::region(l, layout::Rect{r.x, r.y, r.w, r.h, r.id});
        r.x = o.x;
        r.y = o.y;
        r.w = o.w;
        r.h = o.h;
    }
}

void drawBackground(Screen& s, const layout::Layout& l) {
    gfx::setColor(1, 1, 1);
    if (l.classic) {
        for (int i = 0; i < 4; i++)
            if (s.background[i]) gfx::draw(s.background[i], (i % 2) * 320, (i / 2) * 240);
        return;
    }
    ComposedArt(s);
    gfx::draw(s.stoneImg, gfx::newQuad(0, 0, l.w, l.h), 0, 0);
    SetPal(s, kLight);
    gfx::rectangle(gfx::FILL, 0, 0, l.w - 1, 1);
    gfx::rectangle(gfx::FILL, 0, 1, 1, l.h - 2);
    SetPal(s, kDark);
    gfx::rectangle(gfx::FILL, 0, l.h - 1, l.w, 1);
    gfx::rectangle(gfx::FILL, l.w - 1, 0, 1, l.h - 1);
    const layout::Rect& m = l.mapRect;
    Bevel(s, m.x - 1, m.y - 1, m.w + 2, m.h + 2);
    gfx::setColor(0, 0, 0);
    gfx::rectangle(gfx::FILL, m.x - 1, m.y - 1, m.w + 2, 1);
    gfx::rectangle(gfx::FILL, m.x - 1, m.y + m.h, m.w + 2, 1);
    gfx::rectangle(gfx::FILL, m.x - 1, m.y - 1, 1, m.h + 2);
    gfx::rectangle(gfx::FILL, m.x + m.w, m.y - 1, 1, m.h + 2);
    for (const Panel& p : kPanels) {
        layout::Rect at = layout::move(l, p.group, layout::Rect{p.x, p.y, p.w, p.h});
        // on a short screen the strategic map's frame stops above the panel
        int h = std::string(p.group) == "strat" ? p.h - l.cut : p.h;
        Bevel(s, at.x, at.y, p.w, h);
        gfx::setColor(1, 1, 1);
        gfx::draw(s.bgImage, gfx::newQuad(p.x, p.y, p.w, h), at.x, at.y);
    }
}

void drawControls(Screen& s) {
    gfx::setColor(1, 1, 1);
    const auto& cs = s.dialog.controls;
    for (size_t i = 0; i < cs.size(); i++) {
        const Control& c = cs[i];
        if (c.bitmap != 0 && c.w > 0 && c.h > 0 && !IsCovered(s, c, i)) {
            int st = s.stateOf(c.id);
            const Art* a = s.artFor(c.bitmap);
            auto src = c.src[st];
            if (a && src.x + c.w <= a->w && src.y + c.h <= a->h)
                gfx::draw(a->image, gfx::newQuad(src.x, src.y, c.w, c.h), c.x, c.y);
        }
    }
}

View dialog(Screen& s, int id) {
    View v;
    v.dialog = w2::uidata::dialog(s.ui, id);
    v.id = id;
    for (auto& c : v.dialog.controls) v.state[c.id] = w2::uidata::NORMAL;
    return v;
}

const std::set<int> DEFAULT_IDS = {123, 103, 141, 174, 189, 192, 201, 223, 287, 242, 285, 251,
                                   292, 294, 330, 331, 332, 172, 356, 359, 368, 370, 396, 421,
                                   424, 425, 457, 469, 282, 485, 484, 483, 474, 489, 490, 495};

void drawDialogControls(Screen& s, const View& v, const std::set<int>* hidden) {
    gfx::setColor(1, 1, 1);
    for (const Control& c : v.dialog.controls) {
        if (hidden && hidden->count(c.id)) continue;
        if (c.bitmap != 0 && c.w > 0 && c.h > 0) {
            const Art* a = s.artFor(c.bitmap);
            auto src = c.src[v.stateOf(c.id)];
            if (a && src.x + c.w <= a->w && src.y + c.h <= a->h) {
                gfx::setColor(1, 1, 1);
                gfx::draw(a->image, gfx::newQuad(src.x, src.y, c.w, c.h), c.x, c.y);
            }
        }
        if (DEFAULT_IDS.count(c.id) && !v.screen && c.w > 0 && c.h > 0) DefaultRing(s, c);
    }
}

const Control* dialogControl(const View& v, int id) {
    for (const Control& c : v.dialog.controls)
        if (c.id == id) return &c;
    return nullptr;
}

const Control* dialogControlAt(const View& v, int x, int y) {
    for (const Control& c : v.dialog.controls) {
        if (c.w > 0 && c.h > 0 && x >= c.x && x < c.x + c.w && y >= c.y && y < c.y + c.h) return &c;
    }
    return nullptr;
}

gfx::ImageP strategicImage(Screen& s, const w2::Game& g, int side) {
    int w = g.map->width, h = g.map->height;
    const auto& mc = s.mapColour;
    long long seed = 12345;
    auto grey = [&]() {
        seed = (seed * 1103515245 + 12345) % 2147483648LL;
        return 2 + static_cast<int>(seed / 65536) % 3;
    };
    uint8_t rgb[16][3];
    for (int i = 0; i < 16; i++) {
        int k = mc.remap[i];
        w2::pal::Colour c = k >= 0 && k < static_cast<int>(s.palette.size())
                                    ? s.palette[k]
                                    : w2::pal::Colour{0, 0, 0};
        for (int j = 0; j < 3; j++) rgb[i][j] = static_cast<uint8_t>(std::floor(c[j] * 255));
    }
    std::vector<uint8_t> out(static_cast<size_t>(w) * 2 * h * 2 * 4, 0);
    auto put = [&](int x, int y, int i) {
        size_t o = (static_cast<size_t>(y) * w * 2 + x) * 4;
        out[o + 3] = 255;
        if (i < 0) return;
        i &= 15;
        out[o] = rgb[i][0];
        out[o + 1] = rgb[i][1];
        out[o + 2] = rgb[i][2];
    };
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int px[4];
            if (!w2::game::seen(g, side, x, y)) {
                px[0] = px[1] = px[2] = px[3] = -1;
            } else {
                int road = w2::scn::roadAt(*g.map, x, y) % 32;
                int id = road != 0 ? road - 1 : (w2::scn::tileAt(*g.map, x, y) % 256);
                if (road == 0 && id >= 0x50 && id <= 0x5f) {
                    for (int q = 0; q < 4; q++) px[q] = grey();
                } else {
                    for (int q = 0; q < 4; q++)
                        px[q] = road != 0 ? (id < 20 ? mc.road[q][id] : 0) : mc.tile[q][id];
                }
            }
            put(x * 2, y * 2, px[0]);
            put(x * 2 + 1, y * 2, px[1]);
            put(x * 2, y * 2 + 1, px[2]);
            put(x * 2 + 1, y * 2 + 1, px[3]);
        }
    }
    return gfx::newImageRGBA(w * 2, h * 2, out.data());
}

const Region* regionAt(const Screen& s, int x, int y) {
    const auto& rs = s.dialog.regions;
    for (int i = static_cast<int>(rs.size()) - 1; i >= 0; i--) {
        const Region& r = rs[i];
        if (r.w > 0 && r.h > 0 && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return &r;
    }
    return nullptr;
}

const Region* region(const Screen& s, int id) {
    for (const Region& r : s.dialog.regions)
        if (r.id == id) return &r;
    return nullptr;
}

const Control* control(const Screen& s, int id) {
    for (const Control& c : s.dialog.controls)
        if (c.id == id) return &c;
    return nullptr;
}

const Control* controlAt(const Screen& s, int x, int y) {
    const auto& cs = s.dialog.controls;
    for (size_t i = 0; i < cs.size(); i++) {
        const Control& c = cs[i];
        if (c.w > 0 && c.h > 0 && !IsCovered(s, c, i) && x >= c.x && x < c.x + c.w && y >= c.y &&
            y < c.y + c.h)
            return &c;
    }
    return nullptr;
}

}  // namespace screen
