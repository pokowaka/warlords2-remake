// Render a song through the FM driver and print a checksum per second, to
// compare with the Lua remake's (test/fmrender.lua).
//     w2fm original/SOUND/SINT12.XMI 10
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "absl/flags/flag.h"

#include "util/util.hpp"
#include "warlords/ailfm.hpp"

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
ABSL_FLAG(std::string, data, "original", "Path to the Warlords II game data directory.");

/**
 * @brief Renders the given XMI song for the given number of seconds and
 *        prints, per second, a rolling checksum and the peak sample.
 */
int main(int argc, char** argv) {
    auto args = w2::ParseCliFlags(argc, argv, "w2fm <XMI_FILE> <SECONDS> [--data DIR]");
    if (args.size() < 2) return 2;
    auto xmi_bytes = w2::ReadBytes(args[0]);
    if (!xmi_bytes.ok()) {
        fprintf(stderr, "w2fm: %s\n", std::string(xmi_bytes.status().message()).c_str());
        return 1;
    }
    auto sequence = w2::xmi::Parse(*xmi_bytes);
    if (!sequence) return 1;
    std::filesystem::path data_dir = absl::GetFlag(FLAGS_data);
    auto adv_bytes = w2::ReadBytes(data_dir / "ADLIB.ADV");
    auto ad_bytes = w2::ReadBytes(data_dir / "MIDPAK.AD");
    if (!adv_bytes.ok() || !ad_bytes.ok()) {
        fprintf(stderr, "w2fm: failed to read ADLIB.ADV / MIDPAK.AD from '%s'\n",
                data_dir.string().c_str());
        return 1;
    }
    w2::AilFm driver(*adv_bytes, *ad_bytes);
    driver.Play(&*sequence, false);
    int seconds = static_cast<int>(w2::ToInt(args[1]).value_or(0));
    constexpr int kOplRate = 49716;
    std::vector<int16_t> samples(kOplRate);
    for (int second = 0; second < seconds; second++) {
        driver.Render(samples.data(), kOplRate);
        long sum = 0;
        long peak = 0;
        for (int i = 0; i < kOplRate; i++) {
            sum = (sum * 31 + samples[static_cast<size_t>(i)] + 65536) % 1000000007;
            peak = std::max(peak, static_cast<long>(std::abs(samples[static_cast<size_t>(i)])));
        }
        printf("%d %ld %ld\n", second, sum, peak);
    }
    return 0;
}
