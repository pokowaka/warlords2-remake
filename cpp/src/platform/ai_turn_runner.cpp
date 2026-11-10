#include "platform/ai_turn_runner.hpp"

#include <utility>

#include <SDL.h>

#include "absl/log/absl_check.h"

namespace w2 {

AiTurnRunner::AiTurnRunner(TurnBody body) : body_(std::move(body)) {}

AiTurnRunner::~AiTurnRunner() {
    Abort();
}

TurnControl AiTurnRunner::WorkerObserver::OnTurnEvent(const TurnEvent& event) {
    return runner_.EmitFromWorker(event);
}

bool AiTurnRunner::WorkerObserver::IsAborted() const {
    return runner_.CheckAborted();
}

int AiTurnRunner::WorkerThreadMain(void* self_ptr) {
    auto* self = static_cast<AiTurnRunner*>(self_ptr);
    self->RunWorkerBody();
    return 0;
}

void AiTurnRunner::RunWorkerBody() {
    bool should_run = false;
    {
        absl::MutexLock turn_lock(&turn_mutex_);
        turn_mutex_.Await(absl::Condition(this, &AiTurnRunner::IsWorkerTurn));
        should_run = !abort_requested_ && static_cast<bool>(body_);
    }

    // Alien turn logic executes strictly outside `turn_mutex_`.
    if (should_run) {
        WorkerObserver worker_channel(*this);
        body_(worker_channel);
    }

    {
        absl::MutexLock turn_lock(&turn_mutex_);
        pending_event_.reset();
        finished_ = true;
        owner_ = TurnOwner::kMain;
    }
}

TurnControl AiTurnRunner::EmitFromWorker(const TurnEvent& event) {
    absl::MutexLock turn_lock(&turn_mutex_);
    if (abort_requested_) {
        return TurnControl::kAbort;
    }
    pending_event_ = event;
    owner_ = TurnOwner::kMain;
    turn_mutex_.Await(absl::Condition(this, &AiTurnRunner::IsWorkerTurn));
    return abort_requested_ ? TurnControl::kAbort : TurnControl::kContinue;
}

bool AiTurnRunner::CheckAborted() const {
    absl::MutexLock turn_lock(&turn_mutex_);
    return abort_requested_;
}

OptionalTurnEvent AiTurnRunner::Resume() {
    {
        absl::MutexLock turn_lock(&turn_mutex_);
        if (finished_ || abort_requested_) {
            return std::nullopt;
        }
    }

#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
    if (!started_) {
        started_ = true;
        class SingleThreadQueueObserver final : public ITurnObserver {
          public:
            explicit SingleThreadQueueObserver(std::vector<TurnEvent>& queue,
                                               const bool& abort_flag)
                    : queue_(queue), abort_flag_(abort_flag) {}
            TurnControl OnTurnEvent(const TurnEvent& event) override {
                if (abort_flag_) return TurnControl::kAbort;
                queue_.push_back(event);
                return TurnControl::kContinue;
            }
            bool IsAborted() const override { return abort_flag_; }

          private:
            std::vector<TurnEvent>& queue_;
            const bool& abort_flag_;
        };
        SingleThreadQueueObserver observer(sync_queue_, abort_requested_);
        if (body_) {
            body_(observer);
        }
        finished_ = true;
        sync_cursor_ = 0;
    }
    if (sync_cursor_ < sync_queue_.size()) {
        return sync_queue_[sync_cursor_++];
    }
    return std::nullopt;
#else
    if (!started_) {
        started_ = true;
        worker_thread_ = SDL_CreateThreadWithStackSize(&AiTurnRunner::WorkerThreadMain,
                                                       "ai_turn_worker", kWorkerStackBytes, this);
        if (worker_thread_ == nullptr) {
            absl::MutexLock turn_lock(&turn_mutex_);
            finished_ = true;
            owner_ = TurnOwner::kMain;
            return std::nullopt;
        }
    }

    OptionalTurnEvent delivered;
    bool now_finished = false;
    {
        absl::MutexLock turn_lock(&turn_mutex_);
        pending_event_.reset();
        owner_ = TurnOwner::kWorker;
        turn_mutex_.Await(absl::Condition(this, &AiTurnRunner::IsMainTurn));
        now_finished = finished_;
        if (!now_finished) {
            ABSL_DCHECK(pending_event_.has_value());
            delivered = std::move(pending_event_);
            pending_event_.reset();
        }
    }

    if (now_finished) {
        JoinWorkerThread();
        return std::nullopt;
    }
    return delivered;
#endif
}

bool AiTurnRunner::Step(ITurnObserver& observer) {
    OptionalTurnEvent event = Resume();
    if (!event.has_value()) {
        return false;
    }
    // Observer notification runs strictly outside `turn_mutex_`.
    TurnControl control = observer.OnTurnEvent(*event);
    if (control == TurnControl::kAbort) {
        Abort();
        return false;
    }
    return true;
}

void AiTurnRunner::RunToCompletion(ITurnObserver& observer) {
    while (Step(observer)) {
    }
}

void AiTurnRunner::Abort() {
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
    abort_requested_ = true;
    finished_ = true;
    sync_queue_.clear();
#else
    {
        absl::MutexLock turn_lock(&turn_mutex_);
        abort_requested_ = true;
        pending_event_.reset();
        if (started_ && !finished_) {
            owner_ = TurnOwner::kWorker;
            turn_mutex_.Await(absl::Condition(this, &AiTurnRunner::IsMainTurn));
            ABSL_DCHECK(finished_);
        } else {
            finished_ = true;
        }
    }
    JoinWorkerThread();
#endif
}

bool AiTurnRunner::Done() const {
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
    return finished_ && sync_cursor_ >= sync_queue_.size();
#else
    absl::MutexLock turn_lock(&turn_mutex_);
    return finished_;
#endif
}

void AiTurnRunner::JoinWorkerThread() {
    if (worker_thread_ != nullptr) {
        SDL_WaitThread(worker_thread_, nullptr);
        worker_thread_ = nullptr;
    }
}

}  // namespace w2
