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
}

bool AiTurnRunner::Done() const {
    absl::MutexLock turn_lock(&turn_mutex_);
    return finished_;
}

void AiTurnRunner::JoinWorkerThread() {
    if (worker_thread_ != nullptr) {
        SDL_WaitThread(worker_thread_, nullptr);
        worker_thread_ = nullptr;
    }
}

}  // namespace w2
