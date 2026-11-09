/**
 * @file
 * @brief Registry for default `IComputerPlayer` factory and new-game setup hooks.
 */
#include "warlords/model/computer_player.hpp"

#include <utility>

#include "absl/base/no_destructor.h"

namespace w2 {
namespace {

ComputerPlayerFactory& DefaultFactorySlot() {
    static absl::NoDestructor<ComputerPlayerFactory> factory;
    return *factory;
}

GameComputerSetupFn& DefaultSetupSlot() {
    static absl::NoDestructor<GameComputerSetupFn> setup_fn;
    return *setup_fn;
}

}  // namespace

void RegisterDefaultComputerPlayerFactory(ComputerPlayerFactory factory) {
    DefaultFactorySlot() = std::move(factory);
}

std::shared_ptr<IComputerPlayer> CreateDefaultComputerPlayer(bool classic_mode, Game& game,
                                                             Side& side) {
    const auto& factory = DefaultFactorySlot();
    if (!factory) return nullptr;
    return factory(classic_mode, game, side);
}

void RegisterDefaultGameComputerSetup(GameComputerSetupFn setup_fn) {
    DefaultSetupSlot() = std::move(setup_fn);
}

void RunDefaultGameComputerSetup(Game& game) {
    const auto& setup_fn = DefaultSetupSlot();
    if (setup_fn) setup_fn(game);
}

}  // namespace w2
