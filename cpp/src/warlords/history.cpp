#include "warlords/history.hpp"

#include "warlords/armytype.hpp"
#include "warlords/game.hpp"
#include "warlords/report.hpp"

namespace w2::history {

void RecordDeed(Game& game, const Side* side, DeedType type, int subject, int object,
                const std::string& name) {
    if (!side) return;
    game.RecordDeed(side->Id(), type, subject, object, name);
}

/** @brief Adds `n` to one kind of loss in `side`'s Triumphs row for `opponent`. */
static void Bump(Game& game, int side, int opponent, Loss kind, int n = 1) {
    game.RecordTriumphLoss(side, opponent, static_cast<int>(kind), n);
}

void Tally(Game& game, const Army* army, int killer, bool boat) {
    int loser = army->OwnerIndex();
    Loss kind;
    if (army->Hero())
        kind = Loss::kHeroes;
    else if (game.ArmyTypes().ById(army->Type())->IsMagical())
        kind = Loss::kCreatures;
    else
        kind = Loss::kArmies;
    int standards = 0;
    for (Item* item : army->Items())
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
    return game.TriumphCount(side, opponent, static_cast<int>(kind));
}

void Record(Game& game) {
    if (game.Turn() > kLastRecordedTurn) return;
    const Map& map = game.GetMap();
    HistoryRecord record;
    auto win = report::ComputeFigures(game, *game.Sides()[0], report::kWinning);
    for (int i = 0; i < 8; i++) {
        const Side* side = map.SideAt(i);
        record.gold[i] = (side && side->InUse()) ? side->Gold() : 0;
        record.score[i] = win.value[i];
        record.cities[i] = side ? static_cast<int>(game::SideCities(game, *side).size()) : 0;
    }
    for (const auto& city : map.Cities())
        record.owners.push_back(city.IsRazed() ? 0xff : (city.IsNeutral() ? 8 : city.OwnerIndex()));
    // every side's first deed, then second ones while there is room
    int kept = 0;
    int take[8] = {0};
    for (int i = 0; i < 8; i++) {
        if (game.DeedsForSide(i).size() >= 1) {
            take[i] = 1;
            kept++;
        }
    }
    for (int i = 0; i < 8; i++) {
        if (game.DeedsForSide(i).size() >= 2 && kept < kMaxDeedsPerRound) {
            take[i] = 2;
            kept++;
        }
    }
    for (int i = 0; i < 8; i++) {
        const auto& side_deeds = game.DeedsForSide(i);
        for (int j = 0; j < take[i]; j++) record.events.push_back(side_deeds[j]);
    }
    game.ClearDeeds();
    game.AppendHistoryRecord(std::move(record));
}

}  // namespace w2::history
