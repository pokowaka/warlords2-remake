/**
 * @file
 * @brief Thread-safe registry for default `IComputerPlayer` factory and new-game setup hooks.
 */
#include "warlords/model/computer_player.hpp"

#include <utility>

#include "absl/base/no_destructor.h"
#include "absl/base/thread_annotations.h"
#include "absl/synchronization/mutex.h"

#include "warlords/model/game_state.hpp"

namespace w2 {
namespace {

class ComputerPlayerRegistry {
  public:
    void SetFactory(ComputerPlayerFactory factory) ABSL_LOCKS_EXCLUDED(registry_mutex_) {
        absl::MutexLock registry_lock(&registry_mutex_);
        factory_ = std::move(factory);
    }

    ComputerPlayerFactory SnapshotFactory() const ABSL_LOCKS_EXCLUDED(registry_mutex_) {
        absl::MutexLock registry_lock(&registry_mutex_);
        return factory_;
    }

    void SetSetup(GameComputerSetupFn setup_fn) ABSL_LOCKS_EXCLUDED(registry_mutex_) {
        absl::MutexLock registry_lock(&registry_mutex_);
        setup_fn_ = std::move(setup_fn);
    }

    GameComputerSetupFn SnapshotSetup() const ABSL_LOCKS_EXCLUDED(registry_mutex_) {
        absl::MutexLock registry_lock(&registry_mutex_);
        return setup_fn_;
    }

    void SetSnapshotCodec(GameEncodeFn encode_fn, GameDecodeFn decode_fn)
            ABSL_LOCKS_EXCLUDED(registry_mutex_) {
        absl::MutexLock registry_lock(&registry_mutex_);
        encode_fn_ = std::move(encode_fn);
        decode_fn_ = std::move(decode_fn);
    }

    std::pair<GameEncodeFn, GameDecodeFn> SnapshotCodec() const
            ABSL_LOCKS_EXCLUDED(registry_mutex_) {
        absl::MutexLock registry_lock(&registry_mutex_);
        return {encode_fn_, decode_fn_};
    }

  private:
    mutable absl::Mutex registry_mutex_;
    ComputerPlayerFactory factory_ ABSL_GUARDED_BY(registry_mutex_);
    GameComputerSetupFn setup_fn_ ABSL_GUARDED_BY(registry_mutex_);
    GameEncodeFn encode_fn_ ABSL_GUARDED_BY(registry_mutex_);
    GameDecodeFn decode_fn_ ABSL_GUARDED_BY(registry_mutex_);
};

ComputerPlayerRegistry& GlobalComputerPlayerRegistry() {
    static absl::NoDestructor<ComputerPlayerRegistry> registry;
    return *registry;
}

}  // namespace

void RegisterDefaultComputerPlayerFactory(ComputerPlayerFactory factory) {
    GlobalComputerPlayerRegistry().SetFactory(std::move(factory));
}

std::shared_ptr<IComputerPlayer> CreateDefaultComputerPlayer(bool classic_mode, Game& game,
                                                             Side& side) {
    ComputerPlayerFactory factory = GlobalComputerPlayerRegistry().SnapshotFactory();
    if (!factory) return nullptr;
    return factory(classic_mode, game, side);
}

void RegisterDefaultGameComputerSetup(GameComputerSetupFn setup_fn) {
    GlobalComputerPlayerRegistry().SetSetup(std::move(setup_fn));
}

void RunDefaultGameComputerSetup(Game& game) {
    GameComputerSetupFn setup_fn = GlobalComputerPlayerRegistry().SnapshotSetup();
    if (setup_fn) setup_fn(game);
}

void RegisterDefaultGameSnapshotCodec(GameEncodeFn encode_fn, GameDecodeFn decode_fn) {
    GlobalComputerPlayerRegistry().SetSnapshotCodec(std::move(encode_fn), std::move(decode_fn));
}

std::string EncodeDefaultGameSnapshot(const Game& game) {
    auto [encode_fn, decode_fn] = GlobalComputerPlayerRegistry().SnapshotCodec();
    (void)decode_fn;
    if (!encode_fn) return {};
    return encode_fn(game);
}

std::unique_ptr<Game> DecodeDefaultGameSnapshot(std::string_view text,
                                                const std::filesystem::path& data_dir) {
    auto [encode_fn, decode_fn] = GlobalComputerPlayerRegistry().SnapshotCodec();
    (void)encode_fn;
    if (!decode_fn) return nullptr;
    return decode_fn(text, data_dir);
}

}  // namespace w2
