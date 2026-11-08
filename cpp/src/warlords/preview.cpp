#include "warlords/preview.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "warlords/pal.hpp"
#include "warlords/pck.hpp"
#include "warlords/scn.hpp"

namespace w2::preview {

namespace {

constexpr int CELL = 40;  // a tile, in the sheets
constexpr int SHEET_COLS = 16, SHEET_TILES = 96;
constexpr int ROAD_COLS = 13, ROAD_STRIDE = 48;  // ROAD.PCK: 40px art on a 48px stride
// colours after the game's 16
constexpr uint8_t ROAD = 16, BACK = 17;
constexpr uint8_t BLACK = 0, YELLOW = 7, WHITE = 15;
// what each road piece joins: N 1, E 2, S 4, W 8 (docs/formats/map.md > .RD)
constexpr int ROAD_SHAPE[18] = {0, 10, 5, 15, 14, 13, 11, 7, 12, 9, 3, 6, 8, 4, 2, 1, 10, 5};

struct Canvas {
    int w, h;
    std::vector<uint8_t> px;
    Canvas(int w_, int h_) : w(w_), h(h_), px(static_cast<size_t>(w_) * h_, BACK) {}
    void rect(int x, int y, int rw, int rh, uint8_t c) {
        for (int yy = std::max(0, y); yy < std::min(h, y + rh); yy++)
            for (int xx = std::max(0, x); xx < std::min(w, x + rw); xx++)
                px[static_cast<size_t>(yy) * w + xx] = c;
    }
    void outline(int x, int y, int rw, int rh, uint8_t c) {
        rect(x, y, rw, 1, c);
        rect(x, y + rh - 1, rw, 1, c);
        rect(x, y, 1, rh, c);
        rect(x + rw - 1, y, 1, rh, c);
    }
};

uint32_t crc32(const std::string& s) {
    static std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320U ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xffffffffU;
    for (unsigned char b : s) c = table[(c ^ b) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffU;
}

void be32(std::string& out, uint32_t v) {
    for (int k = 3; k >= 0; k--) out += static_cast<char>((v >> (8 * k)) & 0xff);
}

void chunk(std::string& out, const char* tag, const std::string& data) {
    be32(out, static_cast<uint32_t>(data.size()));
    std::string body = std::string(tag, 4) + data;
    out += body;
    be32(out, crc32(body));
}

/** zlib data with stored (uncompressed) deflate blocks: no zlib needed. */
std::string zstore(const std::string& raw) {
    std::string out = "\x78\x01";
    uint32_t a = 1, b = 0;
    for (unsigned char c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    size_t at = 0;
    do {
        size_t n = std::min<size_t>(65535, raw.size() - at);
        bool last = at + n == raw.size();
        out += static_cast<char>(last ? 1 : 0);
        out += static_cast<char>(n & 0xff);
        out += static_cast<char>(n >> 8);
        out += static_cast<char>(~n & 0xff);
        out += static_cast<char>((~n >> 8) & 0xff);
        out.append(raw, at, n);
        at += n;
    } while (at < raw.size());
    be32(out, (b << 16) | a);
    return out;
}

std::string encode(const Canvas& cv, const std::vector<std::array<uint8_t, 3>>& palette) {
    std::string raw;
    raw.reserve(static_cast<size_t>(cv.w + 1) * cv.h);
    for (int y = 0; y < cv.h; y++) {
        raw += '\0';
        raw.append(reinterpret_cast<const char*>(&cv.px[static_cast<size_t>(y) * cv.w]), cv.w);
    }
    std::string out = "\x89PNG\r\n\x1a\n";
    std::string ihdr;
    be32(ihdr, cv.w);
    be32(ihdr, cv.h);
    ihdr += std::string("\x08\x03\x00\x00\x00", 5);  // 8-bit, palette
    chunk(out, "IHDR", ihdr);
    std::string plte;
    for (auto& c : palette) plte += std::string(reinterpret_cast<const char*>(c.data()), 3);
    chunk(out, "PLTE", plte);
    chunk(out, "IDAT", zstore(raw));
    chunk(out, "IEND", "");
    return out;
}

}  // namespace

Canvas draw(const Map& map, const std::string& dataDir, int scale);

std::string png(const Map& map, const std::string& dataDir, int scale) {
    Canvas cv = draw(map, dataDir, scale);
    std::vector<std::array<uint8_t, 3>> palette;
    for (auto& c : pal::load(dataDir + "/TERRAIN0/WAR2.PAL"))
        palette.push_back({static_cast<uint8_t>(c[0] * 255 + 0.5),
                           static_cast<uint8_t>(c[1] * 255 + 0.5),
                           static_cast<uint8_t>(c[2] * 255 + 0.5)});
    palette.resize(16, {0, 0, 0});
    palette.push_back({110, 60, 20});  // ROAD
    palette.push_back({32, 32, 32});   // BACK
    return encode(cv, palette);
}

pck::Pixels menuPicture(const Map& map, const std::string& dataDir) {
    constexpr int MW = 264, MH = 225;
    constexpr uint8_t SEA = 5, DARK_ROAD = 14;  // WAR2.PAL: light blue, dark brown
    Canvas big = draw(map, dataDir, 2);
    pck::Pixels out;
    out.w = MW;
    out.h = MH;
    out.px.assign(static_cast<size_t>(MW) * MH, SEA);
    double k = static_cast<double>(big.h) / MH;
    int w = std::min(MW, static_cast<int>(big.w / k)), x0 = (MW - w) / 2;
    for (int y = 0; y < MH; y++)
        for (int x = 0; x < w; x++) {
            int sx = std::min(big.w - 1, static_cast<int>(x * k)),
                sy = std::min(big.h - 1, static_cast<int>(y * k));
            uint8_t c = big.px[static_cast<size_t>(sy) * big.w + sx];
            if (c == ROAD) c = DARK_ROAD;
            if (c == BACK) c = SEA;
            out.px[static_cast<size_t>(y) * MW + x0 + x] = c;
        }
    return out;
}

Canvas draw(const Map& map, const std::string& dataDir, int scale) {
    scale = std::clamp(scale, 1, CELL);
    const int step = std::max(1, CELL / scale);
    std::string terrain = dataDir + "/TERRAIN0/";
    auto sheet0 = pck::decode(terrain + "SCENERY0.PCK"),
         sheet1 = pck::decode(terrain + "SCENERY1.PCK");
    auto roads = pck::decode(terrain + "ROAD.PCK");
    if (!sheet0 || !sheet1 || !roads)
        throw std::runtime_error("cannot read the tile sheets in " + terrain);
    int W = map.width, H = map.height;
    Canvas cv(W * scale, H * scale);
    uint8_t key = roads->at(0, 0);  // ROAD.PCK is colour-keyed on its corner
    for (int ty = 0; ty < H; ty++)
        for (int tx = 0; tx < W; tx++) {
            int idx = scn::tileAt(map, tx, ty) & scn::TILE_MASK;
            const pck::Pixels& sh = idx / SHEET_TILES == 0 ? *sheet0 : *sheet1;
            int cell = idx % SHEET_TILES, sx = (cell % SHEET_COLS) * CELL,
                sy = (cell / SHEET_COLS) * CELL;
            int piece = scn::roadAt(map, tx, ty) % 32;
            int rx = (piece - 1) % ROAD_COLS * ROAD_STRIDE, ry = (piece - 1) / ROAD_COLS * CELL;
            for (int y = 0; y < scale; y++)
                for (int x = 0; x < scale; x++) {
                    int px = x * step, py = y * step;
                    uint8_t c = sx + px < sh.w && sy + py < sh.h ? sh.at(sx + px, sy + py) : BACK;
                    if (piece > 0 && piece <= 17 && rx + px < roads->w && ry + py < roads->h) {
                        uint8_t r = roads->at(rx + px, ry + py);
                        if (r != key) c = r;
                    }
                    cv.px[static_cast<size_t>(ty * scale + y) * cv.w +
                          static_cast<size_t>(tx) * scale + x] = c;
                }
        }
    // bold over the tiles: roads, sites, cities, capitals
    int s = scale, mid = scale / 2, t = std::max(1, scale / 3);
    for (int ty = 0; ty < H; ty++)
        for (int tx = 0; tx < W; tx++) {
            int piece = scn::roadAt(map, tx, ty) % 32;
            if (piece <= 0 || piece > 17) continue;
            int shape = ROAD_SHAPE[piece], cx = tx * s + mid, cy = ty * s + mid;
            cv.rect(cx - t / 2, cy - t / 2, t, t, ROAD);
            if (shape & 1) cv.rect(cx - t / 2, ty * s, t, mid + 1, ROAD);
            if (shape & 4) cv.rect(cx - t / 2, cy, t, s - mid, ROAD);
            if (shape & 8) cv.rect(tx * s, cy - t / 2, mid + 1, t, ROAD);
            if (shape & 2) cv.rect(cx, cy - t / 2, s - mid, t, ROAD);
        }
    for (const Site& site : map.sites) {
        cv.rect(site.x * s, site.y * s, s, s, YELLOW);
        cv.outline(site.x * s, site.y * s, s, s, BLACK);
    }
    for (const City& c : map.cities) {
        const Side* owner = nullptr;
        for (const Side& sd : map.sides)
            if (sd.capital == &c) owner = &sd;
        if (!owner) {
            cv.rect(c.x * s, c.y * s, 2 * s, 2 * s, WHITE);
            cv.outline(c.x * s, c.y * s, 2 * s, 2 * s, BLACK);
        } else {
            cv.rect(c.x * s - 2, c.y * s - 2, 2 * s + 4, 2 * s + 4, BLACK);
            cv.rect(c.x * s, c.y * s, 2 * s, 2 * s, static_cast<uint8_t>(owner->colour & 15));
            cv.outline(c.x * s - 2, c.y * s - 2, 2 * s + 4, 2 * s + 4, WHITE);
        }
    }
    return cv;
}

}  // namespace w2::preview
