#include "warlords/pck.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <stdexcept>

#include "util/util.hpp"
#include "warlords/bytes.hpp"

namespace w2::pck {

namespace {
std::mutex lock;  ///< Guards `cache`.
std::map<std::string, std::shared_ptr<const Pixels>>
        cache;  ///< Decoded sheets by upper-cased path.

/**
 * @brief Decompresses one bitplane (see the format in pck.hpp).
 * @param data The whole file.
 * @param pos Where the plane's compressed bytes start.
 * @param plane_size How many bytes the plane decompresses to.
 * @param out Receives the decompressed plane.
 * @return Where the next plane starts.
 */
size_t DecodePlane(const std::string& data, size_t pos, size_t plane_size,
                   std::vector<uint8_t>& out) {
    out.assign(plane_size, 0);
    size_t written = 0;
    while (written < plane_size) {
        if (pos >= data.size()) throw std::runtime_error("plane ran out");
        int cmd = static_cast<uint8_t>(data[pos]);
        if (cmd < 0x80) {
            size_t count = cmd + 1;
            for (size_t i = 0; i < count; i++) {
                if (written + i < plane_size)
                    out[written + i] = static_cast<uint8_t>(U8(data, pos + 1 + i));
            }
            written += count;
            pos += 1 + count;
        } else {
            long dist = 65536 - (cmd * 256 + U8(data, pos + 1));
            int count = U8(data, pos + 2) + 1;
            pos += 3;
            for (int i = 0; i < count; i++) {
                long src = static_cast<long>(written) - dist;
                uint8_t value = src >= 0 ? out[src] : 0;
                if (written < plane_size) out[written] = value;
                written++;
            }
        }
    }
    if (written != plane_size) throw std::runtime_error("plane overran");
    return pos;
}
}  // namespace

std::shared_ptr<const Pixels> Decode(const std::string& path) {
    {
        std::lock_guard<std::mutex> guard(lock);
        auto cached = cache.find(Upper(path));
        if (cached != cache.end()) return cached->second;
    }
    auto data = ReadFile(path);
    if (!data) throw std::runtime_error("cannot open image: " + path);
    const std::string& bytes = *data;
    if (U16(bytes, 0) != 1) throw std::runtime_error(path + ": unexpected version");
    auto pixels = std::make_shared<Pixels>();
    pixels->w = U16(bytes, 2);
    pixels->h = U16(bytes, 4);
    size_t rowbytes = pixels->w / 8;
    size_t plane_size = rowbytes * pixels->h;
    pixels->px.assign(static_cast<size_t>(pixels->w) * pixels->h, 0);
    size_t pos = 6;
    std::vector<uint8_t> plane;
    for (int plane_index = 0; plane_index < 4; plane_index++) {
        pos = DecodePlane(bytes, pos, plane_size, plane);
        uint8_t bit = static_cast<uint8_t>(1 << plane_index);
        for (int y = 0; y < pixels->h; y++) {
            for (size_t byte_x = 0; byte_x < rowbytes; byte_x++) {
                uint8_t plane_byte = plane[y * rowbytes + byte_x];
                if (!plane_byte) continue;
                size_t row_start = static_cast<size_t>(y) * pixels->w + byte_x * 8;
                for (int bit_index = 0; bit_index < 8; bit_index++)
                    if (plane_byte & (0x80 >> bit_index)) pixels->px[row_start + bit_index] |= bit;
            }
        }
    }
    std::lock_guard<std::mutex> guard(lock);
    cache[Upper(path)] = pixels;
    return pixels;
}

namespace {
/**
 * @brief Compresses one bitplane and appends it to `out`.
 *
 * Bytes go out as literal runs of up to 128; a run of three or more copies
 * of the byte just written goes out as a match one byte back instead.
 */
void EncodePlane(const std::vector<uint8_t>& plane, std::string& out) {
    size_t pos = 0, size = plane.size();
    std::string literals;
    auto flush = [&]() {
        for (size_t at = 0; at < literals.size(); at += 128) {
            size_t chunk = std::min<size_t>(128, literals.size() - at);
            out += static_cast<char>(chunk - 1);
            out.append(literals, at, chunk);
        }
        literals.clear();
    };
    while (pos < size) {
        // a run of the byte just written: a match one byte back
        size_t run = 0;
        if (pos > 0)
            while (pos + run < size && run < 256 && plane[pos + run] == plane[pos - 1]) run++;
        if (run >= 3) {
            flush();
            out += static_cast<char>(0xff);  // offset 0xffff: one byte back
            out += static_cast<char>(0xff);
            out += static_cast<char>(run - 1);
            pos += run;
        } else {
            literals += static_cast<char>(plane[pos++]);
        }
    }
    flush();
}
}  // namespace

std::string Encode(const Pixels& pixels) {
    if (pixels.w % 8 != 0) throw std::runtime_error("pck: width must be a multiple of 8");
    std::string out;
    auto put16 = [&](int value) {
        out += static_cast<char>(value & 0xff);
        out += static_cast<char>((value >> 8) & 0xff);
    };
    put16(1);
    put16(pixels.w);
    put16(pixels.h);
    size_t rowbytes = pixels.w / 8;
    for (int plane_index = 0; plane_index < 4; plane_index++) {
        std::vector<uint8_t> plane(rowbytes * pixels.h, 0);
        for (int y = 0; y < pixels.h; y++)
            for (int x = 0; x < pixels.w; x++)
                if (pixels.At(x, y) & (1 << plane_index))
                    plane[y * rowbytes + x / 8] |= static_cast<uint8_t>(0x80 >> (x % 8));
        EncodePlane(plane, out);
    }
    return out;
}

}  // namespace w2::pck
