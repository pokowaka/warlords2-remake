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

constexpr int kCell = 40;  ///< A tile's size in pixels, in the sheets.
/// How many tiles a terrain sheet has a row, and in all.
constexpr int kSheetCols = 16, kSheetTiles = 96;
/// ROAD.PCK: 13 pieces a row, 40px art on a 48px stride.
constexpr int kRoadCols = 13, kRoadStride = 48;
/// The colours after the game's 16: the bold roads, and the background off the sheets.
constexpr uint8_t kRoad = 16, kBack = 17;
/// Colours from the game's 16 the overlay uses.
constexpr uint8_t kBlack = 0, kYellow = 7, kWhite = 15;
/// What each road piece joins: N 1, E 2, S 4, W 8 (docs/formats/map.md > .RD).
constexpr int kRoadShape[18] = {0, 10, 5, 15, 14, 13, 11, 7, 12, 9, 3, 6, 8, 4, 2, 1, 10, 5};

/** @brief An 8-bit paletted picture being drawn. */
struct Canvas {
    int w;                    ///< Width in pixels.
    int h;                    ///< Height in pixels.
    std::vector<uint8_t> px;  ///< Palette indices, row by row.
    Canvas(int width, int height)
            : w(width), h(height), px(static_cast<size_t>(width) * height, kBack) {}
    /** @brief Fills a rectangle, clipped to the canvas. */
    void Rect(int x, int y, int rect_width, int rect_height, uint8_t colour) {
        for (int row = std::max(0, y); row < std::min(h, y + rect_height); row++)
            for (int column = std::max(0, x); column < std::min(w, x + rect_width); column++)
                px[static_cast<size_t>(row) * w + column] = colour;
    }
    /** @brief Draws a rectangle's one-pixel border, clipped to the canvas. */
    void Outline(int x, int y, int rect_width, int rect_height, uint8_t colour) {
        Rect(x, y, rect_width, 1, colour);
        Rect(x, y + rect_height - 1, rect_width, 1, colour);
        Rect(x, y, 1, rect_height, colour);
        Rect(x + rect_width - 1, y, 1, rect_height, colour);
    }
};

/** @brief The CRC-32 a PNG chunk ends with. */
uint32_t Crc32(const std::string& bytes) {
    static std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> entries{};
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t crc = i;
            for (int bit = 0; bit < 8; bit++) crc = crc & 1 ? 0xedb88320U ^ (crc >> 1) : crc >> 1;
            entries[i] = crc;
        }
        return entries;
    }();
    uint32_t crc = 0xffffffffU;
    for (unsigned char byte : bytes) crc = table[(crc ^ byte) & 0xff] ^ (crc >> 8);
    return crc ^ 0xffffffffU;
}

/** @brief Appends a 32-bit number, big-endian, as PNG writes them. */
void Be32(std::string& out, uint32_t value) {
    for (int shift = 3; shift >= 0; shift--)
        out += static_cast<char>((value >> (8 * shift)) & 0xff);
}

/** @brief Appends a PNG chunk: its length, tag, data and CRC. */
void Chunk(std::string& out, const char* tag, const std::string& data) {
    Be32(out, static_cast<uint32_t>(data.size()));
    std::string body = std::string(tag, 4) + data;
    out += body;
    Be32(out, Crc32(body));
}

/** @brief zlib data with stored (uncompressed) deflate blocks: no zlib needed. */
std::string Zstore(const std::string& raw) {
    std::string out = "\x78\x01";
    // the Adler-32 checksum's two halves
    uint32_t sum_a = 1, sum_b = 0;
    for (unsigned char byte : raw) {
        sum_a = (sum_a + byte) % 65521;
        sum_b = (sum_b + sum_a) % 65521;
    }
    size_t done = 0;
    do {
        size_t block = std::min<size_t>(65535, raw.size() - done);
        bool last = done + block == raw.size();
        out += static_cast<char>(last ? 1 : 0);
        out += static_cast<char>(block & 0xff);
        out += static_cast<char>(block >> 8);
        out += static_cast<char>(~block & 0xff);
        out += static_cast<char>((~block >> 8) & 0xff);
        out.append(raw, done, block);
        done += block;
    } while (done < raw.size());
    Be32(out, (sum_b << 16) | sum_a);
    return out;
}

/** @brief The canvas as PNG bytes with the given palette. */
std::string Encode(const Canvas& canvas, const std::vector<std::array<uint8_t, 3>>& palette) {
    std::string raw;
    raw.reserve(static_cast<size_t>(canvas.w + 1) * canvas.h);
    for (int y = 0; y < canvas.h; y++) {
        raw += '\0';
        raw.append(reinterpret_cast<const char*>(&canvas.px[static_cast<size_t>(y) * canvas.w]),
                   canvas.w);
    }
    std::string out = "\x89PNG\r\n\x1a\n";
    std::string ihdr;
    Be32(ihdr, canvas.w);
    Be32(ihdr, canvas.h);
    ihdr += std::string("\x08\x03\x00\x00\x00", 5);  // 8-bit, palette
    Chunk(out, "IHDR", ihdr);
    std::string plte;
    for (auto& colour : palette)
        plte += std::string(reinterpret_cast<const char*>(colour.data()), 3);
    Chunk(out, "PLTE", plte);
    Chunk(out, "IDAT", Zstore(raw));
    Chunk(out, "IEND", "");
    return out;
}

}  // namespace

/**
 * @brief Draws the map, `scale` pixels a tile, from the game's tile sheets,
 *        with the roads, sites, cities and capitals bold over it.
 * @throws std::runtime_error if the tile sheets cannot be read.
 */
Canvas Draw(const Map& map, const std::string& data_dir, int scale);

std::string Png(const Map& map, const std::string& data_dir, int scale) {
    Canvas canvas = Draw(map, data_dir, scale);
    std::vector<std::array<uint8_t, 3>> palette;
    for (auto& colour : pal::Load(data_dir + "/TERRAIN0/WAR2.PAL"))
        palette.push_back({static_cast<uint8_t>(colour[0] * 255 + 0.5),
                           static_cast<uint8_t>(colour[1] * 255 + 0.5),
                           static_cast<uint8_t>(colour[2] * 255 + 0.5)});
    palette.resize(16, {0, 0, 0});
    palette.push_back({110, 60, 20});  // ROAD
    palette.push_back({32, 32, 32});   // BACK
    return Encode(canvas, palette);
}

pck::Pixels MenuPicture(const Map& map, const std::string& data_dir) {
    constexpr int kMenuWidth = 264, kMenuHeight = 225;
    constexpr uint8_t kSea = 5, kDarkRoad = 14;  // WAR2.PAL: light blue, dark brown
    Canvas big = Draw(map, data_dir, 2);
    pck::Pixels out;
    out.w = kMenuWidth;
    out.h = kMenuHeight;
    out.px.assign(static_cast<size_t>(kMenuWidth) * kMenuHeight, kSea);
    double shrink = static_cast<double>(big.h) / kMenuHeight;
    int width = std::min(kMenuWidth, static_cast<int>(big.w / shrink)),
        left = (kMenuWidth - width) / 2;
    for (int y = 0; y < kMenuHeight; y++)
        for (int x = 0; x < width; x++) {
            int source_x = std::min(big.w - 1, static_cast<int>(x * shrink)),
                source_y = std::min(big.h - 1, static_cast<int>(y * shrink));
            uint8_t colour = big.px[static_cast<size_t>(source_y) * big.w + source_x];
            if (colour == kRoad) colour = kDarkRoad;
            if (colour == kBack) colour = kSea;
            out.px[static_cast<size_t>(y) * kMenuWidth + left + x] = colour;
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
    Canvas canvas(map_width * scale, map_height * scale);
    uint8_t key = roads->At(0, 0);  // ROAD.PCK is colour-keyed on its corner
    for (int tile_y = 0; tile_y < map_height; tile_y++)
        for (int tile_x = 0; tile_x < map_width; tile_x++) {
            int tile = scn::TileAt(map, tile_x, tile_y) & scn::kTileMask;
            const pck::Pixels& sheet = tile / kSheetTiles == 0 ? *sheet0 : *sheet1;
            int cell = tile % kSheetTiles, sheet_x = (cell % kSheetCols) * kCell,
                sheet_y = (cell / kSheetCols) * kCell;
            int piece = scn::RoadAt(map, tile_x, tile_y) % 32;
            int road_x = (piece - 1) % kRoadCols * kRoadStride,
                road_y = (piece - 1) / kRoadCols * kCell;
            for (int y = 0; y < scale; y++)
                for (int x = 0; x < scale; x++) {
                    int pixel_x = x * step, pixel_y = y * step;
                    uint8_t colour = sheet_x + pixel_x < sheet.w && sheet_y + pixel_y < sheet.h
                                             ? sheet.At(sheet_x + pixel_x, sheet_y + pixel_y)
                                             : kBack;
                    if (piece > 0 && piece <= 17 && road_x + pixel_x < roads->w &&
                        road_y + pixel_y < roads->h) {
                        uint8_t road_colour = roads->At(road_x + pixel_x, road_y + pixel_y);
                        if (road_colour != key) colour = road_colour;
                    }
                    canvas.px[static_cast<size_t>(tile_y * scale + y) * canvas.w +
                              static_cast<size_t>(tile_x) * scale + x] = colour;
                }
        }
    // bold over the tiles: roads, sites, cities, capitals
    int size = scale, mid = scale / 2, thick = std::max(1, scale / 3);
    for (int tile_y = 0; tile_y < map_height; tile_y++)
        for (int tile_x = 0; tile_x < map_width; tile_x++) {
            int piece = scn::RoadAt(map, tile_x, tile_y) % 32;
            if (piece <= 0 || piece > 17) continue;
            int shape = kRoadShape[piece], centre_x = tile_x * size + mid,
                centre_y = tile_y * size + mid;
            canvas.Rect(centre_x - thick / 2, centre_y - thick / 2, thick, thick, kRoad);
            if (shape & 1) canvas.Rect(centre_x - thick / 2, tile_y * size, thick, mid + 1, kRoad);
            if (shape & 4) canvas.Rect(centre_x - thick / 2, centre_y, thick, size - mid, kRoad);
            if (shape & 8) canvas.Rect(tile_x * size, centre_y - thick / 2, mid + 1, thick, kRoad);
            if (shape & 2) canvas.Rect(centre_x, centre_y - thick / 2, size - mid, thick, kRoad);
        }
    for (const Site& site : map.sites) {
        canvas.Rect(site.x * size, site.y * size, size, size, kYellow);
        canvas.Outline(site.x * size, site.y * size, size, size, kBlack);
    }
    for (const City& city : map.cities) {
        const Side* owner = nullptr;
        for (const Side& side : map.sides)
            if (side.capital == &city) owner = &side;
        if (!owner) {
            canvas.Rect(city.x * size, city.y * size, 2 * size, 2 * size, kWhite);
            canvas.Outline(city.x * size, city.y * size, 2 * size, 2 * size, kBlack);
        } else {
            canvas.Rect(city.x * size - 2, city.y * size - 2, 2 * size + 4, 2 * size + 4, kBlack);
            canvas.Rect(city.x * size, city.y * size, 2 * size, 2 * size,
                        static_cast<uint8_t>(owner->colour & 15));
            canvas.Outline(city.x * size - 2, city.y * size - 2, 2 * size + 4, 2 * size + 4,
                           kWhite);
        }
    }
    return canvas;
}

}  // namespace w2::preview
