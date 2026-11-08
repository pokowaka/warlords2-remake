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

constexpr int kCell = 40;  // a tile, in the sheets
constexpr int kSheetCols = 16, kSheetTiles = 96;
constexpr int kRoadCols = 13, kRoadStride = 48;  // ROAD.PCK: 40px art on a 48px stride
// colours after the game's 16
constexpr uint8_t kRoad = 16, kBack = 17;
constexpr uint8_t kBlack = 0, kYellow = 7, kWhite = 15;
// what each road piece joins: N 1, E 2, S 4, W 8 (docs/formats/map.md > .RD)
constexpr int kRoadShape[18] = {0, 10, 5, 15, 14, 13, 11, 7, 12, 9, 3, 6, 8, 4, 2, 1, 10, 5};

struct Canvas {
    int w, h;
    std::vector<uint8_t> px;
    Canvas(int width, int height)
            : w(width), h(height), px(static_cast<size_t>(width) * height, kBack) {}
    void Rect(int x, int y, int rw, int rh, uint8_t c) {
        for (int yy = std::max(0, y); yy < std::min(h, y + rh); yy++)
            for (int xx = std::max(0, x); xx < std::min(w, x + rw); xx++)
                px[static_cast<size_t>(yy) * w + xx] = c;
    }
    void Outline(int x, int y, int rw, int rh, uint8_t c) {
        Rect(x, y, rw, 1, c);
        Rect(x, y + rh - 1, rw, 1, c);
        Rect(x, y, 1, rh, c);
        Rect(x + rw - 1, y, 1, rh, c);
    }
};

uint32_t Crc32(const std::string& s) {
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

void Be32(std::string& out, uint32_t v) {
    for (int k = 3; k >= 0; k--) out += static_cast<char>((v >> (8 * k)) & 0xff);
}

void Chunk(std::string& out, const char* tag, const std::string& data) {
    Be32(out, static_cast<uint32_t>(data.size()));
    std::string body = std::string(tag, 4) + data;
    out += body;
    Be32(out, Crc32(body));
}

/** zlib data with stored (uncompressed) deflate blocks: no zlib needed. */
std::string Zstore(const std::string& raw) {
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
    Be32(out, (b << 16) | a);
    return out;
}

std::string Encode(const Canvas& cv, const std::vector<std::array<uint8_t, 3>>& palette) {
    std::string raw;
    raw.reserve(static_cast<size_t>(cv.w + 1) * cv.h);
    for (int y = 0; y < cv.h; y++) {
        raw += '\0';
        raw.append(reinterpret_cast<const char*>(&cv.px[static_cast<size_t>(y) * cv.w]), cv.w);
    }
    std::string out = "\x89PNG\r\n\x1a\n";
    std::string ihdr;
    Be32(ihdr, cv.w);
    Be32(ihdr, cv.h);
    ihdr += std::string("\x08\x03\x00\x00\x00", 5);  // 8-bit, palette
    Chunk(out, "IHDR", ihdr);
    std::string plte;
    for (auto& c : palette) plte += std::string(reinterpret_cast<const char*>(c.data()), 3);
    Chunk(out, "PLTE", plte);
    Chunk(out, "IDAT", Zstore(raw));
    Chunk(out, "IEND", "");
    return out;
}

}  // namespace

Canvas Draw(const Map& map, const std::string& data_dir, int scale);

std::string Png(const Map& map, const std::string& data_dir, int scale) {
    Canvas cv = Draw(map, data_dir, scale);
    std::vector<std::array<uint8_t, 3>> palette;
    for (auto& c : pal::Load(data_dir + "/TERRAIN0/WAR2.PAL"))
        palette.push_back({static_cast<uint8_t>(c[0] * 255 + 0.5),
                           static_cast<uint8_t>(c[1] * 255 + 0.5),
                           static_cast<uint8_t>(c[2] * 255 + 0.5)});
    palette.resize(16, {0, 0, 0});
    palette.push_back({110, 60, 20});  // ROAD
    palette.push_back({32, 32, 32});   // BACK
    return Encode(cv, palette);
}

pck::Pixels MenuPicture(const Map& map, const std::string& data_dir) {
    constexpr int kMw = 264, kMh = 225;
    constexpr uint8_t kSea = 5, kDarkRoad = 14;  // WAR2.PAL: light blue, dark brown
    Canvas big = Draw(map, data_dir, 2);
    pck::Pixels out;
    out.w = kMw;
    out.h = kMh;
    out.px.assign(static_cast<size_t>(kMw) * kMh, kSea);
    double k = static_cast<double>(big.h) / kMh;
    int w = std::min(kMw, static_cast<int>(big.w / k)), x0 = (kMw - w) / 2;
    for (int y = 0; y < kMh; y++)
        for (int x = 0; x < w; x++) {
            int sx = std::min(big.w - 1, static_cast<int>(x * k)),
                sy = std::min(big.h - 1, static_cast<int>(y * k));
            uint8_t c = big.px[static_cast<size_t>(sy) * big.w + sx];
            if (c == kRoad) c = kDarkRoad;
            if (c == kBack) c = kSea;
            out.px[static_cast<size_t>(y) * kMw + x0 + x] = c;
        }
    return out;
}

Canvas Draw(const Map& map, const std::string& data_dir, int scale) {
    scale = std::clamp(scale, 1, kCell);
    const int step = std::max(1, kCell / scale);
    std::string terrain = data_dir + "/TERRAIN0/";
    auto sheet0 = pck::Decode(terrain + "SCENERY0.PCK"),
         sheet1 = pck::Decode(terrain + "SCENERY1.PCK");
    auto roads = pck::Decode(terrain + "ROAD.PCK");
    if (!sheet0 || !sheet1 || !roads)
        throw std::runtime_error("cannot read the tile sheets in " + terrain);
    int map_width = map.width, map_height = map.height;
    Canvas cv(map_width * scale, map_height * scale);
    uint8_t key = roads->At(0, 0);  // ROAD.PCK is colour-keyed on its corner
    for (int ty = 0; ty < map_height; ty++)
        for (int tx = 0; tx < map_width; tx++) {
            int idx = scn::TileAt(map, tx, ty) & scn::kTileMask;
            const pck::Pixels& sh = idx / kSheetTiles == 0 ? *sheet0 : *sheet1;
            int cell = idx % kSheetTiles, sx = (cell % kSheetCols) * kCell,
                sy = (cell / kSheetCols) * kCell;
            int piece = scn::RoadAt(map, tx, ty) % 32;
            int rx = (piece - 1) % kRoadCols * kRoadStride, ry = (piece - 1) / kRoadCols * kCell;
            for (int y = 0; y < scale; y++)
                for (int x = 0; x < scale; x++) {
                    int px = x * step, py = y * step;
                    uint8_t c = sx + px < sh.w && sy + py < sh.h ? sh.At(sx + px, sy + py) : kBack;
                    if (piece > 0 && piece <= 17 && rx + px < roads->w && ry + py < roads->h) {
                        uint8_t r = roads->At(rx + px, ry + py);
                        if (r != key) c = r;
                    }
                    cv.px[static_cast<size_t>(ty * scale + y) * cv.w +
                          static_cast<size_t>(tx) * scale + x] = c;
                }
        }
    // bold over the tiles: roads, sites, cities, capitals
    int s = scale, mid = scale / 2, t = std::max(1, scale / 3);
    for (int ty = 0; ty < map_height; ty++)
        for (int tx = 0; tx < map_width; tx++) {
            int piece = scn::RoadAt(map, tx, ty) % 32;
            if (piece <= 0 || piece > 17) continue;
            int shape = kRoadShape[piece], cx = tx * s + mid, cy = ty * s + mid;
            cv.Rect(cx - t / 2, cy - t / 2, t, t, kRoad);
            if (shape & 1) cv.Rect(cx - t / 2, ty * s, t, mid + 1, kRoad);
            if (shape & 4) cv.Rect(cx - t / 2, cy, t, s - mid, kRoad);
            if (shape & 8) cv.Rect(tx * s, cy - t / 2, mid + 1, t, kRoad);
            if (shape & 2) cv.Rect(cx, cy - t / 2, s - mid, t, kRoad);
        }
    for (const Site& site : map.sites) {
        cv.Rect(site.x * s, site.y * s, s, s, kYellow);
        cv.Outline(site.x * s, site.y * s, s, s, kBlack);
    }
    for (const City& c : map.cities) {
        const Side* owner = nullptr;
        for (const Side& sd : map.sides)
            if (sd.capital == &c) owner = &sd;
        if (!owner) {
            cv.Rect(c.x * s, c.y * s, 2 * s, 2 * s, kWhite);
            cv.Outline(c.x * s, c.y * s, 2 * s, 2 * s, kBlack);
        } else {
            cv.Rect(c.x * s - 2, c.y * s - 2, 2 * s + 4, 2 * s + 4, kBlack);
            cv.Rect(c.x * s, c.y * s, 2 * s, 2 * s, static_cast<uint8_t>(owner->colour & 15));
            cv.Outline(c.x * s - 2, c.y * s - 2, 2 * s + 4, 2 * s + 4, kWhite);
        }
    }
    return cv;
}

}  // namespace w2::preview
