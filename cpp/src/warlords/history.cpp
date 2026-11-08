#include "warlords/history.hpp"

#include "warlords/armytype.hpp"
#include "warlords/game.hpp"
#include "warlords/report.hpp"

namespace w2::history {

void RecordDeed(Game& game, const Side* side, DeedType type, int subject, int object,
                const std::string& name) {
    if (!side) return;
    auto& kept = game.deeds[side->index];
    Deed deed{side->index, type, subject, object, name.substr(0, 15)};
    if (kept.size() < 2) {
        kept.push_back(deed);
        return;
    }
    // a lower type counts for more: replace the less important of the two
    int worse = kept[1].type >= kept[0].type ? 1 : 0;
    if (kept[worse].type > type) kept[worse] = deed;
}

/** @brief Adds `n` to one kind of loss in `side`'s Triumphs row for `opponent`. */
static void Bump(Game& game, int side, int opponent, Loss kind, int n = 1) {
    if (side < 0 || opponent < 0 || side > 7 || opponent > 7) return;
    auto& row = game.triumphs[side]
                        .try_emplace(opponent, std::array<int, 5>{0, 0, 0, 0, 0})
                        .first->second;
    row[static_cast<int>(kind)] += n;
}

void Tally(Game& game, const Army* army, int killer, bool boat) {
    int loser = army->owner;
    Loss kind;
    if (army->type == armytype::kHero)
        kind = Loss::kHeroes;
    else if (game.types.ById(army->type)->IsMagical())
        kind = Loss::kCreatures;
    else
        kind = Loss::kArmies;
    int standards = 0;
    for (Item* item : army->items)
        if (item->index < 8) standards++;
    // the loser's own row counts it lost, the killer's row for the loser killed
    int rows[2][2] = {{loser, loser}, {killer, loser}};
    for (auto& row : rows) {
        Bump(game, row[0], row[1], kind);
        if (boat) Bump(game, row[0], row[1], Loss::kNavies);
        if (standards > 0) Bump(game, row[0], row[1], Loss::kStandards, standards);
    }
}

int Triumph(const Game& game, int side, int opponent, Loss kind) {
    auto side_rows = game.triumphs.find(side);
    if (side_rows == game.triumphs.end()) return 0;
    auto row = side_rows->second.find(opponent);
    if (row == side_rows->second.end()) return 0;
    return row->second[static_cast<int>(kind)];
}

void Record(Game& game) {
    if (game.turn > kLastRecordedTurn) return;
    HistoryRecord record;
    auto win = report::ComputeFigures(game, *game.sides[0], report::kWinning);
    for (int i = 0; i < 8; i++) {
        Side* side = game.map->Side(i);
        record.gold[i] = (side && side->in_use) ? side->gold : 0;
        record.score[i] = win.value[i];
        record.cities[i] = side ? static_cast<int>(game::SideCities(game, *side).size()) : 0;
    }
    for (auto& city : game.map->cities)
        record.owners.push_back(city.razed ? 0xff
                                           : (city.owner_index == kNone ? 8 : city.owner_index));
    // every side's first deed, then second ones while there is room
    int kept = 0;
    int take[8] = {0};
    for (int i = 0; i < 8; i++) {
        auto side_deeds = game.deeds.find(i);
        if (side_deeds != game.deeds.end() && side_deeds->second.size() >= 1) {
            take[i] = 1;
            kept++;
        }
    }
    for (int i = 0; i < 8; i++) {
        auto side_deeds = game.deeds.find(i);
        if (side_deeds != game.deeds.end() && side_deeds->second.size() >= 2 &&
            kept < kMaxDeedsPerRound) {
            take[i] = 2;
            kept++;
        }
    }
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < take[i]; j++) record.events.push_back(game.deeds[i][j]);
    }
    game.deeds.clear();
    game.history.push_back(record);
}

}  // namespace w2::history
