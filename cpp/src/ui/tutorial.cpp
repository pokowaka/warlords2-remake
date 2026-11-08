// The tutorial's help pages (the Tutorial option, .SCN 0x12e): each shown
// once, the first time its moment comes. The pages are FILE.DAT group 0x19
// -- TUTORIA\T*.GFX -- shown by 8065:168d.
#include <map>

#include "ui/dialogs.hpp"

namespace tutorial {

namespace {
const std::map<std::string, int> kPages = {
    {"hero", 0},   {"prod", 1},  {"select", 2}, {"move", 3},    {"fight", 4},
    {"search", 5}, {"prod2", 6}, {"turn2", 7},  {"endturn", 9}, {"fresult", 10}};
const char* const kFiles[] = {"THERO",  "TPROD",  "TSELECT",  "TMOVE",    "TFIGHT",  "TSEARCH",
                              "TPROD2", "TTURN2", "TWARLORD", "TENDTURN", "TFRESULT"};
}  // namespace

void show(const std::string& moment, kit::Done after) {
    auto done = [after]() {
        if (after) after();
    };
    auto it = kPages.find(moment);
    if (!G.g || it == kPages.end() || G.g->map->options.tutorial == 0) return done();
    if (G.g->tutorialSeen.count(moment)) return done();
    G.g->tutorialSeen.insert(moment);
    if (!help::open(std::string("TUTORIA\\") + kFiles[it->second] + ".GFX", after)) done();
}

static void ChainFrom(std::vector<std::string> moments, size_t k, kit::Done after) {
    if (k < moments.size())
        show(moments[k], [moments, k, after]() { ChainFrom(moments, k + 1, after); });
    else if (after)
        after();
}

void chain(const std::vector<std::string>& moments, kit::Done after) {
    ChainFrom(moments, 0, after);
}

}  // namespace tutorial
