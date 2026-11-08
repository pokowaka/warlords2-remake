#include "warlords/cues.hpp"

#include <algorithm>
#include <map>

#include "warlords/armytype.hpp"

namespace w2::cues {

namespace {
/// FILE.DAT group and index of each cue's song; index -1 is a random entry.
const std::map<int, std::pair<int, int>> kSong = {
    {0, {8, 0}},  {1, {10, -1}}, {3, {9, -1}}, {4, {12, 0}},  {5, {13, -1}}, {6, {14, -1}},
    {7, {15, 0}}, {8, {16, 0}},  {9, {17, 0}}, {10, {18, 0}}, {11, {19, 0}},
};
/// The advisor's group for a fall to fewer cities, by the mark it falls below.
const std::map<int, int> kLosing = {{5, 40}, {10, 41}, {15, 42}, {20, 43}, {25, 44}, {30, 45}};
/// The advisor's group for a rise in cities, by the new mark reached.
const std::map<int, int> kWinning = {{10, 48}, {15, 49}, {20, 50}, {25, 51}, {30, 52}, {35, 53}};

/**
 * @brief An entry of a FILE.DAT group: `index` from 0, or a random one for -1.
 *
 * An index past the end takes the last entry; "" for a missing group.
 */
std::string Entry(const uidata::Strings& files, int group, int index, const Roll& roll) {
    static const std::vector<std::string> kNone;
    const auto& entries =
            group >= 0 && group < static_cast<int>(files.size()) ? files[group] : kNone;
    if (index < 0) index = roll(static_cast<int>(entries.size())) - 1;
    if (index > static_cast<int>(entries.size()) - 1) index = static_cast<int>(entries.size()) - 1;
    if (index < 0 || index >= static_cast<int>(entries.size())) return "";
    return entries[index];
}
}  // namespace

std::pair<std::string, bool> Song(const uidata::Strings& files, int cue,
                                  const std::vector<Side*>& sides, const Roll& roll) {
    if (cue == kComputer) {
        // with a human still in play, group 11; in a game of computers alone, 5%
        // group 12, 47% group 11 and 48% group 10
        bool human = false;
        for (Side* side : sides) {
            if (side->alive && !side->computer) {
                human = true;
                break;
            }
        }
        int group = 11;
        if (!human) {
            int percent = roll(100);
            if (percent < 6)
                group = 12;
            else if (percent >= 53)
                group = 10;
        }
        return {Entry(files, group, -1, roll), false};
    }
    auto song = kSong.find(cue);
    if (song == kSong.end()) return {"", false};
    return {Entry(files, song->second.first, song->second.second, roll), true};
}

int AdvisorCue(const Game& game, Side& side, const Roll& roll) {
    int cities = 0, heroes = 0;
    for (auto& city : game.map->cities)
        if (city.owner_index == side.index && !city.razed) cities++;
    if (cities > 39) return kNone;
    for (Army* army : game.armies)
        if (army->owner == side.index && army->type == armytype::kHero) heroes++;
    Advisor& advisor = side.advisor;
    int group;
    if (cities < advisor.mark) {
        int mark = (cities / 5) * 5;
        advisor.dir = 2;
        advisor.mark = mark;
        auto line = kLosing.find(mark + 5);
        group = line != kLosing.end() ? line->second : 46;
    } else if (advisor.mark + 5 <= cities) {
        int mark = (cities / 5) * 5;
        advisor.dir = 1;
        advisor.mark = mark;
        auto line = kWinning.find(mark);
        group = line != kWinning.end() ? line->second : 47;
    } else {
        if (game.turn % 7 != 0) return kNone;
        if (side.gold < 100)
            group = 54;
        else if (side.gold > 2800)
            group = 55;
        else if (heroes < 1)
            group = 56;
        else if (heroes > 4)
            group = 57;
        else if (roll(5) == 1)
            group = 58;
        else
            return kNone;
    }
    if (game.turn == 1) return kNone;
    return group;
}

std::pair<std::string, std::string> Clip(const uidata::Strings& files, int group,
                                         const Roll& roll) {
    if (group >= kGreet) return {Entry(files, group, 0, roll), Entry(files, group, 1, roll)};
    static const std::vector<std::string> kNone;
    const auto& samples =
            group >= 0 && group < static_cast<int>(files.size()) ? files[group] : kNone;
    int pick = roll(static_cast<int>(samples.size()));
    const auto& subtitles = group + 30 < static_cast<int>(files.size()) ? files[group + 30] : kNone;
    std::string sample =
            pick >= 1 && pick <= static_cast<int>(samples.size()) ? samples[pick - 1] : "";
    // the subtitle at the same place, or the last one when there are fewer
    int subtitle_index = std::min(pick, static_cast<int>(subtitles.size())) - 1;
    std::string text = subtitle_index >= 0 && subtitle_index < static_cast<int>(subtitles.size())
                               ? subtitles[subtitle_index]
                               : "";
    return {sample, text};
}

}  // namespace w2::cues
