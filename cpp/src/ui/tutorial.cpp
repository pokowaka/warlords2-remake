/**
 * @file
 * @brief The tutorial's help pages (the Tutorial option, .SCN 0x12e): each
 * shown once, the first time its moment comes.
 *
 * The pages are FILE.DAT group 0x19 -- TUTORIA\T*.GFX -- shown as help files.
 */
#include <map>

#include "ui/dialogs.hpp"

namespace tutorial {

namespace {
/// Each moment's page: its index in kFiles.
const std::map<std::string, int> kPages = {
    {"hero", 0},   {"prod", 1},  {"select", 2}, {"move", 3},    {"fight", 4},
    {"search", 5}, {"prod2", 6}, {"turn2", 7},  {"endturn", 9}, {"fresult", 10}};
/// The pages' help files in TUTORIA\, without the .GFX.
const char* const kFiles[] = {"THERO",  "TPROD",  "TSELECT",  "TMOVE",    "TFIGHT",  "TSEARCH",
                              "TPROD2", "TTURN2", "TWARLORD", "TENDTURN", "TFRESULT"};
}  // namespace

/**
 * @brief Shows the page for `moment` the first time it comes, when the
 * tutorial is on; `after` runs once it is closed, or at once when there is
 * nothing to show.
 */
void Show(const std::string& moment, kit::Done after) {
    auto done = [after]() {
        if (after) after();
    };
    auto page = kPages.find(moment);
    if (!g_app.g || page == kPages.end() || g_app.g->GetMap().GameOptions().tutorial == 0)
        return done();
    if (g_app.g->HasSeenTutorial(moment)) return done();
    g_app.g->MarkTutorialSeen(moment);
    if (!help::Open(std::string("TUTORIA\\") + kFiles[page->second] + ".GFX", after)) done();
}

/** @brief Shows the pages for `moments` from number `next` on, one after another. */
static void ChainFrom(std::vector<std::string> moments, size_t next, kit::Done after) {
    if (next < moments.size())
        Show(moments[next], [moments, next, after]() { ChainFrom(moments, next + 1, after); });
    else if (after)
        after();
}

/** @brief Shows the pages for `moments` in turn, then runs `after`. */
void Chain(const std::vector<std::string>& moments, kit::Done after) {
    ChainFrom(moments, 0, after);
}

}  // namespace tutorial
