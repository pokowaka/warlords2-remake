#include "front/pckimage.hpp"

#include <cmath>
#include <vector>

namespace pckimage {

gfx::ImageP FromPixels(const w2::pck::Pixels& pixels, const w2::pal::Palette& palette,
                       const std::map<int, int>& map, const std::set<int>& keys) {
    std::vector<uint8_t> out(static_cast<size_t>(pixels.w) * pixels.h * 4, 0);
    uint8_t rgb_by_index[16][3];
    for (int i = 0; i < 16; i++) {
        w2::pal::Colour colour =
                i < static_cast<int>(palette.size()) ? palette[i] : w2::pal::Colour{0, 0, 0};
        for (int channel = 0; channel < 3; channel++)
            rgb_by_index[i][channel] = static_cast<uint8_t>(std::floor(colour[channel] * 255));
    }
    for (size_t i = 0; i < pixels.px.size(); i++) {
        int colour_index = pixels.px[i];
        if (keys.count(colour_index)) continue;  // left transparent
        auto remapped = map.find(colour_index);
        if (remapped != map.end()) colour_index = remapped->second;
        colour_index &= 15;
        out[i * 4] = rgb_by_index[colour_index][0];
        out[i * 4 + 1] = rgb_by_index[colour_index][1];
        out[i * 4 + 2] = rgb_by_index[colour_index][2];
        out[i * 4 + 3] = 255;
    }
    return gfx::NewImageRgba(pixels.w, pixels.h, out.data());
}

gfx::ImageP Load(const std::string& path, const w2::pal::Palette& palette, int key) {
    auto pixels = w2::pck::Decode(path);
    std::set<int> keys;
    if (key == kCorner && !pixels->px.empty())
        keys.insert(pixels->px[0]);
    else if (key >= 0)
        keys.insert(key);
    return FromPixels(*pixels, palette, {}, keys);
}

gfx::ImageP Load(const std::string& path, const w2::pal::Palette& palette,
                 const std::set<int>& keys) {
    return FromPixels(*w2::pck::Decode(path), palette, {}, keys);
}

}  // namespace pckimage
