#include "warlords/pck.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <stdexcept>

#include "util/util.hpp"
#include "warlords/bytes.hpp"

namespace w2::pck {

namespace {
std::mutex lock;
std::map<std::string, std::shared_ptr<const Pixels>> cache;

size_t decodePlane(const std::string& s, size_t pos, size_t planeSize, std::vector<uint8_t>& out) {
    out.assign(planeSize, 0);
    size_t n = 0;
    while (n < planeSize) {
        if (pos >= s.size()) throw std::runtime_error("plane ran out");
        int cmd = static_cast<uint8_t>(s[pos]);
        if (cmd < 0x80) {
            size_t count = cmd + 1;
            for (size_t i = 0; i < count; i++) {
                if (n + i < planeSize) out[n + i] = static_cast<uint8_t>(u8(s, pos + 1 + i));
            }
            n += count;
            pos += 1 + count;
        } else {
            long dist = 65536 - (cmd * 256 + u8(s, pos + 1));
            int count = u8(s, pos + 2) + 1;
            pos += 3;
            for (int i = 0; i < count; i++) {
                long src = static_cast<long>(n) - dist;
                uint8_t v = src >= 0 ? out[src] : 0;
                if (n < planeSize) out[n] = v;
                n++;
            }
        }
    }
    if (n != planeSize) throw std::runtime_error("plane overran");
    return pos;
}
}  // namespace

std::shared_ptr<const Pixels> decode(const std::string& path) {
    {
        std::lock_guard<std::mutex> g(lock);
        auto it = cache.find(upper(path));
        if (it != cache.end()) return it->second;
    }
    auto data = readFile(path);
    if (!data) throw std::runtime_error("cannot open image: " + path);
    const std::string& s = *data;
    if (u16(s, 0) != 1) throw std::runtime_error(path + ": unexpected version");
    auto p = std::make_shared<Pixels>();
    p->w = u16(s, 2);
    p->h = u16(s, 4);
    size_t rowbytes = p->w / 8;
    size_t planeSize = rowbytes * p->h;
    p->px.assign(static_cast<size_t>(p->w) * p->h, 0);
    size_t pos = 6;
    std::vector<uint8_t> plane;
    for (int b = 0; b < 4; b++) {
        pos = decodePlane(s, pos, planeSize, plane);
        uint8_t bit = static_cast<uint8_t>(1 << b);
        for (int y = 0; y < p->h; y++) {
            for (size_t xb = 0; xb < rowbytes; xb++) {
                uint8_t v = plane[y * rowbytes + xb];
                if (!v) continue;
                size_t o = static_cast<size_t>(y) * p->w + xb * 8;
                for (int k = 0; k < 8; k++)
                    if (v & (0x80 >> k)) p->px[o + k] |= bit;
            }
        }
    }
    std::lock_guard<std::mutex> g(lock);
    cache[upper(path)] = p;
    return p;
}

namespace {
void encodePlane(const std::vector<uint8_t>& plane, std::string& out) {
    size_t n = 0, size = plane.size();
    std::string lit;
    auto flush = [&]() {
        for (size_t at = 0; at < lit.size(); at += 128) {
            size_t k = std::min<size_t>(128, lit.size() - at);
            out += static_cast<char>(k - 1);
            out.append(lit, at, k);
        }
        lit.clear();
    };
    while (n < size) {
        // a run of the byte just written: a match one byte back
        size_t run = 0;
        if (n > 0)
            while (n + run < size && run < 256 && plane[n + run] == plane[n - 1]) run++;
        if (run >= 3) {
            flush();
            out += static_cast<char>(0xff);  // offset 0xffff: one byte back
            out += static_cast<char>(0xff);
            out += static_cast<char>(run - 1);
            n += run;
        } else {
            lit += static_cast<char>(plane[n++]);
        }
    }
    flush();
}
}  // namespace

std::string encode(const Pixels& p) {
    if (p.w % 8 != 0) throw std::runtime_error("pck: width must be a multiple of 8");
    std::string out;
    auto put16 = [&](int v) {
        out += static_cast<char>(v & 0xff);
        out += static_cast<char>((v >> 8) & 0xff);
    };
    put16(1);
    put16(p.w);
    put16(p.h);
    size_t rowbytes = p.w / 8;
    for (int b = 0; b < 4; b++) {
        std::vector<uint8_t> plane(rowbytes * p.h, 0);
        for (int y = 0; y < p.h; y++)
            for (int x = 0; x < p.w; x++)
                if (p.at(x, y) & (1 << b))
                    plane[y * rowbytes + x / 8] |= static_cast<uint8_t>(0x80 >> (x % 8));
        encodePlane(plane, out);
    }
    return out;
}

}  // namespace w2::pck
