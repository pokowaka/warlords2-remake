#include "warlords/pal.hpp"

#include <cctype>
#include <stdexcept>

#include "util/util.hpp"

namespace w2::pal {

Palette Load(const std::string& path) {
    auto text = ReadFile(path);
    if (!text) throw std::runtime_error("cannot open palette: " + path);
    // every run of three numbers separated by blanks, as the Lua's pattern
    std::vector<long> nums;
    Palette out;
    const std::string& contents = *text;
    size_t pos = 0;
    while (pos < contents.size()) {
        // find three integers separated by whitespace
        if (std::isdigit(static_cast<unsigned char>(contents[pos]))) {
            size_t scan = pos;
            long rgb[3];
            bool matched = true;
            for (int component = 0; component < 3; component++) {
                if (scan >= contents.size() ||
                    !std::isdigit(static_cast<unsigned char>(contents[scan]))) {
                    matched = false;
                    break;
                }
                long number = 0;
                while (scan < contents.size() &&
                       std::isdigit(static_cast<unsigned char>(contents[scan])))
                    number = number * 10 + (contents[scan++] - '0');
                rgb[component] = number;
                if (component < 2) {
                    size_t blank_start = scan;
                    while (scan < contents.size() &&
                           std::isspace(static_cast<unsigned char>(contents[scan])))
                        scan++;
                    if (scan == blank_start) {
                        matched = false;
                        break;
                    }
                }
            }
            if (matched) {
                out.push_back({rgb[0] / 99.0, rgb[1] / 99.0, rgb[2] / 99.0});
                pos = scan;
                continue;
            }
            while (pos < contents.size() && std::isdigit(static_cast<unsigned char>(contents[pos])))
                pos++;
            continue;
        }
        pos++;
    }
    if (out.size() != 16)
        throw std::runtime_error(
                Fmt("%s: expected 16 entries, got %d", path.c_str(), static_cast<int>(out.size())));
    return out;
}

}  // namespace w2::pal
