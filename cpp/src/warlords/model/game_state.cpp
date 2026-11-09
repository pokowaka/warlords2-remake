/**
 * @file
 * @brief Implementation of `w2::Game` model-layer invariants and accessors.
 */
#include "warlords/model/game_state.hpp"

#include <algorithm>
#include <utility>

#include "absl/base/no_destructor.h"
#include "absl/log/absl_check.h"

namespace w2 {

void Game::SetDataDir(std::filesystem::path data_dir) {
    data_dir_ = std::move(data_dir);
}

void Game::SetMap(MapHandle map) {
    map_ = std::move(map);
    sides_.clear();
    current_side_index_ = kNone;
}

Army* Game::Make(const Army& army) {
    return armies_.Make(army);
}

Army* Game::Add(const Army& army) {
    return armies_.Add(army);
}

bool Game::Alive(const Army* army) const {
    return armies_.Alive(army);
}

void Game::Remove(const Army* army) {
    armies_.Remove(army);
}

Game::ArmyStack Game::ArmiesOnTile(int x, int y) const {
    return armies_.ArmiesOnTile(x, y);
}

void Game::SetActiveSides(SideList sides) {
    sides_.clear();
    sides_.reserve(sides.size());
    for (Side* side : sides) {
        if (side != nullptr) sides_.push_back(side);
    }
    if (current_side_index_ < 0 || current_side_index_ >= static_cast<int>(sides_.size())) {
        current_side_index_ = kNone;
    }
}

void Game::AddActiveSide(Side* side) {
    if (side != nullptr) sides_.push_back(side);
}

int Game::LivingSideCount() const {
    int count = 0;
    for (const Side* side_ptr : sides_) {
        if (side_ptr != nullptr && side_ptr->IsAlive()) ++count;
    }
    return count;
}

Side* Game::CurrentSide() {
    return (current_side_index_ >= 0 && current_side_index_ < static_cast<int>(sides_.size()))
                   ? sides_[static_cast<size_t>(current_side_index_)]
                   : nullptr;
}

const Side* Game::CurrentSide() const {
    return (current_side_index_ >= 0 && current_side_index_ < static_cast<int>(sides_.size()))
                   ? sides_[static_cast<size_t>(current_side_index_)]
                   : nullptr;
}

void Game::SetCurrentSideIndex(int index) {
    current_side_index_ = (index >= 0 && index < static_cast<int>(sides_.size())) ? index : kNone;
}

void Game::SetCurrentSide(Side* side) {
    if (side == nullptr) {
        current_side_index_ = kNone;
        return;
    }
    auto found = std::find(sides_.begin(), sides_.end(), side);
    ABSL_DCHECK(found != sides_.end());
    current_side_index_ = found != sides_.end() ? static_cast<int>(found - sides_.begin()) : kNone;
}

TurnReport& Game::TurnNews(SideId side_id) {
    ABSL_DCHECK(side_id.IsValid() && side_id.Value() < 8);
    int idx = std::clamp(side_id.Value(), 0, 7);
    return turn_news_[static_cast<size_t>(idx)];
}

const TurnReport& Game::TurnNews(SideId side_id) const {
    ABSL_DCHECK(side_id.IsValid() && side_id.Value() < 8);
    int idx = std::clamp(side_id.Value(), 0, 7);
    return turn_news_[static_cast<size_t>(idx)];
}

bool Game::HasTower(int x, int y) const {
    return map_ != nullptr && map_->InBounds(x, y) && towers_.count(y * map_->Width() + x) != 0;
}

void Game::PlaceTower(TilePos pos) {
    if (map_ != nullptr && map_->InBounds(pos.x, pos.y)) {
        towers_.insert(pos.y * map_->Width() + pos.x);
    }
}

void Game::PlaceTowerKey(int tile_key) {
    if (map_ == nullptr) return;
    int max_keys = map_->Width() * map_->Height();
    if (tile_key >= 0 && tile_key < max_keys) {
        towers_.insert(tile_key);
    }
}

void Game::RemoveTower(TilePos pos) {
    if (map_ != nullptr && map_->InBounds(pos.x, pos.y)) {
        towers_.erase(pos.y * map_->Width() + pos.x);
    }
}

void Game::ClearAbandonedTowers() {
    if (towers_.empty() || map_ == nullptr) return;
    const int map_width = map_->Width();
    TowerSet held;
    for (const Army* army : armies_) {
        if (army->OnMap()) held.insert(army->Y() * map_width + army->X());
    }
    for (auto iter = towers_.begin(); iter != towers_.end();) {
        if (held.count(*iter) == 0) {
            iter = towers_.erase(iter);
        } else {
            ++iter;
        }
    }
}

void Game::SetEndingState(Ending ending) {
    ending_ = std::move(ending);
}

void Game::AppendLog(std::string entry) {
    log_.push_back(std::move(entry));
}

void Game::AppendHistoryRecord(HistoryRecord record) {
    history_.push_back(std::move(record));
}

const Game::DeedList& Game::DeedsForSide(int side_index) const {
    static const absl::NoDestructor<DeedList> kEmptyDeeds;
    auto found = deeds_.find(side_index);
    return found != deeds_.end() ? found->second : *kEmptyDeeds;
}

void Game::RecordDeed(SideId side_id, DeedType type, int subject, int object,
                      std::string_view actor_name) {
    if (!side_id.IsValid() || side_id.Value() >= 8) return;
    int side_idx = side_id.Value();
    DeedList& kept = deeds_[side_idx];
    Deed deed{side_idx, type, subject, object, std::string(actor_name.substr(0, 15))};
    if (kept.size() < 2) {
        kept.push_back(std::move(deed));
        return;
    }
    int worse = kept[1].type >= kept[0].type ? 1 : 0;
    if (kept[static_cast<size_t>(worse)].type > type) {
        kept[static_cast<size_t>(worse)] = std::move(deed);
    }
}

void Game::SetSideDeeds(int side_index, DeedList deeds) {
    if (deeds.empty()) {
        deeds_.erase(side_index);
    } else {
        deeds_[side_index] = std::move(deeds);
    }
}

void Game::RecordTriumphLoss(int side_index, int opponent_index, int kind_index, int count) {
    if (side_index < 0 || side_index > 7 || opponent_index < 0 || opponent_index > 7 ||
        kind_index < 0 || kind_index >= 5) {
        return;
    }
    TriumphCounts& row = triumphs_[side_index]
                                 .try_emplace(opponent_index, TriumphCounts{0, 0, 0, 0, 0})
                                 .first->second;
    row[static_cast<size_t>(kind_index)] += count;
}

int Game::TriumphCount(int side_index, int opponent_index, int kind_index) const {
    if (kind_index < 0 || kind_index >= 5) return 0;
    auto side_rows = triumphs_.find(side_index);
    if (side_rows == triumphs_.end()) return 0;
    auto row = side_rows->second.find(opponent_index);
    if (row == side_rows->second.end()) return 0;
    return row->second[static_cast<size_t>(kind_index)];
}

void Game::SetTriumphRow(int side_index, int opponent_index, const TriumphCounts& counts) {
    if (side_index >= 0 && side_index <= 7 && opponent_index >= 0 && opponent_index <= 7) {
        triumphs_[side_index][opponent_index] = counts;
    }
}

bool Game::MarkTutorialSeen(std::string moment) {
    return tutorial_seen_.insert(std::move(moment)).second;
}

const Game::HeroNameList* Game::CachedHeroNames(int side_index) const {
    auto found = hero_names_.find(side_index);
    return found != hero_names_.end() ? &found->second : nullptr;
}

const Game::HeroNameList& Game::CacheHeroNames(int side_index, HeroNameList names) {
    return hero_names_[side_index] = std::move(names);
}

ScopedAiTurnEvents::ScopedAiTurnEvents(Game& game, IAiTurnEvents* events)
        : game_(game), previous_(game.active_ai_events_) {
    game_.active_ai_events_ = events;
}

ScopedAiTurnEvents::~ScopedAiTurnEvents() {
    game_.active_ai_events_ = previous_;
}

}  // namespace w2
