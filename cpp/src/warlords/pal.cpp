#include "warlords/pal.hpp"

#include <cctype>
#include <string>

#include "absl/status/status.h"
#include "absl/strings/str_format.h"

#include "util/util.hpp"

namespace w2::pal {

absl::StatusOr<Palette> Load(const std::filesystem::path& path) {
    auto text = ReadFile(path);
    if (!text.ok()) return text.status();
    // every run of three numbers separated by blanks, as the Lua's pattern
    Palette out;
    const std::string& contents = *text;
    size_t pos = 0;
    while (pos < contents.size()) {
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
                       std::isdigit(static_cast<unsigned char>(contents[scan]))) {
                    number = number * 10 + (contents[scan++] - '0');
                }
                rgb[component] = number;
                if (component < 2) {
                    size_t blank_start = scan;
                    while (scan < contents.size() &&
                           std::isspace(static_cast<unsigned char>(contents[scan]))) {
                        scan++;
                    }
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
            while (pos < contents.size() &&
                   std::isdigit(static_cast<unsigned char>(contents[pos]))) {
                pos++;
            }
            continue;
        }
        pos++;
    }
    if (out.size() != 16) {
        return absl::InvalidArgumentError(absl::StrFormat(
                "Failed to parse palette '%s': expected 16 RGB triplets, got %zu; verify the .PAL "
                "file contains 16 'RR GG BB' entries (0..99).",
                path.string(), out.size()));
    }
    return out;
}

}  // namespace w2::pal
