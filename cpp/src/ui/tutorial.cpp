/**
 * @file
 * @brief The tutorial's help pages (the Tutorial option, .SCN 0x12e): each
 * shown once, the first time its moment comes.
 *
 * The pages are FILE.DAT group 0x19 -- TUTORIA\T*.GFX -- shown as help files.
 */
#include <array>
#include <cstddef>
#include <string>
#include <string_view>

#include "ui/dialogs.hpp"

namespace tutorial {

namespace {

/** @brief Associates a tutorial moment identifier with its `TUTORIA\` help stem. */
struct MomentPage {
    std::string_view moment;  ///< Moment key passed to `Show`.
    const char* file;         ///< Help file stem under `TUTORIA\`, without `.GFX`.
};

/// Each moment's help page stem under `TUTORIA\`.
constexpr std::array<MomentPage, 10> kPages = {{
    {"hero", "THERO"},
    {"prod", "TPROD"},
    {"select", "TSELECT"},
    {"move", "TMOVE"},
    {"fight", "TFIGHT"},
    {"search", "TSEARCH"},
    {"prod2", "TPROD2"},
    {"turn2", "TTURN2"},
    {"endturn", "TENDTURN"},
    {"fresult", "TFRESULT"},
}};

/** @brief Looks up the `TUTORIA\` file stem for `moment`, or `nullptr` if unknown. */
const char* FindTutorialFile(std::string_view moment) {
    for (const auto& entry : kPages) {
        if (entry.moment == moment) return entry.file;
    }
    return nullptr;
}

/** @brief Shows the pages for `moments` from index `next` onward, one after another. */
void ChainFrom(w2::UiContext& ctx, const MomentList& moments, size_t next, kit::Done after) {
    if (next < moments.size()) {
        Show(ctx, moments[next],
             [&ctx, moments, next, after]() { ChainFrom(ctx, moments, next + 1, after); });
    } else if (after) {
        after();
    }
}

}  // namespace

/**
 * @brief Shows the page for `moment` the first time it comes, when the
 * tutorial is on; `after` runs once it is closed, or at once when there is
 * nothing to show.
 */
void Show(w2::UiContext& ctx, const std::string& moment, kit::Done after) {
    auto done = [&after]() {
        if (after) after();
    };
    const char* file = FindTutorialFile(moment);
    w2::Game* game = ctx.GamePtr();
    if (!game || !file || game->GetMap().GameOptions().tutorial == 0) return done();
    if (game->HasSeenTutorial(moment)) return done();
    game->MarkTutorialSeen(moment);
    if (!help::Open(ctx, std::string("TUTORIA\\") + file + ".GFX", after)) done();
}

/** @brief Shows the pages for `moments` in turn, then runs `after`. */
void Chain(w2::UiContext& ctx, const MomentList& moments, kit::Done after) {
    ChainFrom(ctx, moments, 0, std::move(after));
}

}  // namespace tutorial
