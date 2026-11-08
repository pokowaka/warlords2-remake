#include "front/pckimage.hpp"

#include <cmath>
#include <vector>

namespace pckimage {

gfx::ImageP FromPixels(const w2::pck::Pixels& p, const w2::pal::Palette& palette,
                       const std::map<int, int>& map, const std::set<int>& keys) {
    std::vector<uint8_t> out(static_cast<size_t>(p.w) * p.h * 4, 0);
    uint8_t lut[16][3];
    for (int i = 0; i < 16; i++) {
        w2::pal::Colour c =
                i < static_cast<int>(palette.size()) ? palette[i] : w2::pal::Colour{0, 0, 0};
        for (int k = 0; k < 3; k++) lut[i][k] = static_cast<uint8_t>(std::floor(c[k] * 255));
    }
    for (size_t i = 0; i < p.px.size(); i++) {
        int idx = p.px[i];
        if (keys.count(idx)) continue;  // left transparent
        auto m = map.find(idx);
        if (m != map.end()) idx = m->second;
        idx &= 15;
        out[i * 4] = lut[idx][0];
        out[i * 4 + 1] = lut[idx][1];
        out[i * 4 + 2] = lut[idx][2];
        out[i * 4 + 3] = 255;
    }
    return gfx::NewImageRgba(p.w, p.h, out.data());
}

gfx::ImageP Load(const std::string& path, const w2::pal::Palette& palette, int key) {
    auto px = w2::pck::Decode(path);
    std::set<int> keys;
    if (key == kCorner && !px->px.empty())
        keys.insert(px->px[0]);
    else if (key >= 0)
        keys.insert(key);
    return FromPixels(*px, palette, {}, keys);
}

gfx::ImageP Load(const std::string& path, const w2::pal::Palette& palette,
                 const std::set<int>& keys) {
    return FromPixels(*w2::pck::Decode(path), palette, {}, keys);
}

}  // namespace pckimage
