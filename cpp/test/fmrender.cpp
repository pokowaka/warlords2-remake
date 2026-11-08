// Render a song through the FM driver and print a checksum per second, to
// compare with the Lua remake's (test/fmrender.lua).
//     w2fm original/SOUND/SINT12.XMI 10
#include <cstdio>
#include <cstdlib>

#include "util/util.hpp"
#include "warlords/ailfm.hpp"

/**
 * @brief Renders the given XMI song for the given number of seconds and
 *        prints, per second, a rolling checksum and the peak sample.
 */
int main(int argc, char** argv) {
    if (argc < 3) return 2;
    auto sequence = w2::xmi::Parse(w2::MustRead(argv[1]));
    if (!sequence) return 1;
    w2::AilFm driver(w2::MustRead("original/ADLIB.ADV"), w2::MustRead("original/MIDPAK.AD"));
    driver.Play(&*sequence, false);
    int seconds = atoi(argv[2]);
    const int opl_rate = 49716;
    std::vector<int16_t> samples(opl_rate);
    for (int second = 0; second < seconds; second++) {
        driver.Render(samples.data(), opl_rate);
        long sum = 0, peak = 0;
        for (int i = 0; i < opl_rate; i++) {
            sum = (sum * 31 + samples[i] + 65536) % 1000000007;
            peak = std::max(peak, static_cast<long>(std::abs(samples[i])));
        }
        printf("%d %ld %ld\n", second, sum, peak);
    }
}
