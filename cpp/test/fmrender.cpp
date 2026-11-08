// Render a song through the FM driver and print a checksum per second, to
// compare with the Lua remake's (test/fmrender.lua).
//     w2fm original/SOUND/SINT12.XMI 10
#include <cstdio>
#include <cstdlib>

#include "util/util.hpp"
#include "warlords/ailfm.hpp"

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    auto seq = w2::xmi::Parse(w2::MustRead(argv[1]));
    if (!seq) return 1;
    w2::AilFm drv(w2::MustRead("original/ADLIB.ADV"), w2::MustRead("original/MIDPAK.AD"));
    drv.Play(&*seq, false);
    int secs = atoi(argv[2]);
    const int opl_rate = 49716;
    std::vector<int16_t> buf(opl_rate);
    for (int s = 0; s < secs; s++) {
        drv.Render(buf.data(), opl_rate);
        long sum = 0, peak = 0;
        for (int i = 0; i < opl_rate; i++) {
            sum = (sum * 31 + buf[i] + 65536) % 1000000007;
            peak = std::max(peak, static_cast<long>(std::abs(buf[i])));
        }
        printf("%d %ld %ld\n", s, sum, peak);
    }
}
