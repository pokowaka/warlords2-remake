// Runs a computer side's turn on a dedicated worker thread, handing control
// back and forth with the front end so that only one thread executes at a
// time. Mirrors the Lua remake's coroutine model, where the front end resumes
// the AI after animating each yielded action (walk, battle, pause).
#pragma once

#include <exception>
#include <functional>
#include <string>
#include <vector>

namespace w2 {
struct Army;
}
struct SDL_Thread;
struct SDL_mutex;
struct SDL_cond;

/**
 * @brief Action yielded by a computer turn to the front end.
 */
struct Yield {
    std::string
            what;   ///< Kind: "walk", "pause", "assault", "progress", "turn", "nohumans", "fallen".
    int ticks = 0;  ///< For "pause": BIOS ticks to wait.
    std::string text;                        ///< For "fallen": elimination message line.
    std::vector<w2::Army*> armies;           ///< For "walk": the moving stack.
    std::vector<std::pair<int, int>> tiles;  ///< For "walk": tile path covered.
    bool sea_before = false;                 ///< For "walk": true if stack started at sea.
};

/**
 * @brief Synchronous handoff thread emulating a Lua coroutine for AI turns.
 */
class Coroutine {
  public:
    using Body = std::function<void(Coroutine&)>;
    explicit Coroutine(Body body);
    ~Coroutine();
    Coroutine(const Coroutine&) = delete;
    Coroutine& operator=(const Coroutine&) = delete;

    /**
     * @brief Runs the coroutine body until it yields or finishes.
     * @return False once the body has finished; rethrows any uncaught exception.
     */
    bool Resume();
    /** @brief Called from inside the body: hands `yielded` to the front end and waits. */
    void YieldValue(Yield yielded);
    /** @brief True once the coroutine body has returned. */
    bool Done() const { return finished_; }
    /** @brief Returns the most recently yielded value. */
    const Yield& Value() const { return value_; }

  private:
    static int Run(void* self_ptr);
    struct Cancelled {};

    Body body_;                     ///< Function executed on the worker thread.
    SDL_Thread* thread_ = nullptr;  ///< Worker thread handle.
    SDL_mutex* lock_ = nullptr;     ///< Mutex guarding handoff state.
    SDL_cond* cond_ = nullptr;      ///< Condition variable signalling turn handoff.
    bool body_turn_ = false;        ///< True when the worker thread owns execution.
    bool started_ = false;          ///< True once the worker thread has been spawned.
    bool finished_ = false;         ///< True once `body_` has completed.
    bool cancel_ = false;           ///< True when the destructor requests unwind.
    Yield value_;                   ///< Latest value passed to `YieldValue`.
    std::exception_ptr error_;      ///< Exception thrown inside `body_`, if any.
};
