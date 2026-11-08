#include "platform/coroutine.hpp"

#include <SDL.h>

Coroutine::Coroutine(Body body) : body_(std::move(body)) {
    lock_ = SDL_CreateMutex();
    cond_ = SDL_CreateCond();
}

Coroutine::~Coroutine() {
    if (started_ && !finished_) {
        // Unwind the worker body: its next yield throws Cancelled and exits cleanly.
        SDL_LockMutex(lock_);
        cancel_ = true;
        body_turn_ = true;
        SDL_CondBroadcast(cond_);
        while (body_turn_) SDL_CondWait(cond_, lock_);
        SDL_UnlockMutex(lock_);
    }
    if (thread_) SDL_WaitThread(thread_, nullptr);
    SDL_DestroyCond(cond_);
    SDL_DestroyMutex(lock_);
}

int Coroutine::Run(void* self_ptr) {
    Coroutine* self = static_cast<Coroutine*>(self_ptr);
    SDL_LockMutex(self->lock_);
    while (!self->body_turn_) SDL_CondWait(self->cond_, self->lock_);
    SDL_UnlockMutex(self->lock_);
    if (!self->cancel_) {
        try {
            self->body_(*self);
        } catch (const Cancelled&) {
        } catch (...) {
            self->error_ = std::current_exception();
        }
    }
    SDL_LockMutex(self->lock_);
    self->finished_ = true;
    self->body_turn_ = false;
    SDL_CondBroadcast(self->cond_);
    SDL_UnlockMutex(self->lock_);
    return 0;
}

bool Coroutine::Resume() {
    if (finished_) return false;
    if (!started_) {
        started_ = true;
        // Allocate a 16 MB stack so recursive AI search has ample headroom.
        thread_ = SDL_CreateThreadWithStackSize(Run, "computer", 16UZ * 1024 * 1024, this);
    }
    SDL_LockMutex(lock_);
    body_turn_ = true;
    SDL_CondBroadcast(cond_);
    while (body_turn_) SDL_CondWait(cond_, lock_);
    SDL_UnlockMutex(lock_);
    if (error_) {
        auto err = error_;
        error_ = nullptr;
        std::rethrow_exception(err);
    }
    return !finished_;
}

void Coroutine::YieldValue(Yield yielded) {
    SDL_LockMutex(lock_);
    value_ = std::move(yielded);
    body_turn_ = false;
    SDL_CondBroadcast(cond_);
    while (!body_turn_) SDL_CondWait(cond_, lock_);
    bool cancelled = cancel_;
    SDL_UnlockMutex(lock_);
    if (cancelled) throw Cancelled{};
}
