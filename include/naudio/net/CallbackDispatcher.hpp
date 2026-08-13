// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — safe-by-construction callback dispatch.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

namespace naudio::net {

// A single-threaded callback dispatch queue. User callbacks posted here run on ONE
// dedicated dispatch thread — never on a caller's worker thread and never under a
// caller's lock — so a user callback may freely re-enter the object that posted it
// (call disconnect(), setters, getters) without deadlocking the worker-join path or
// any internal mutex. This is the mechanism behind the AudioStreamClient /
// AudioStreamServer threading contract: events are observed asynchronously, in order.
//
// Lifecycle: start() spawns the thread; stop() requests shutdown, drains every task
// already queued, then joins. stop() is idempotent and re-entrancy-safe — if it is
// called from WITHIN a dispatched callback (i.e. on the dispatch thread) it only
// requests stop and returns, because a thread cannot join itself; the owner's normal
// teardown performs the actual join. The owner MUST call stop() (join) before any
// state a posted closure might touch is destroyed — typically as the last act of the
// owner's destructor, after its worker threads have stopped posting.
//
// Not safe to call stop() concurrently from multiple threads; only the owner's
// single-threaded teardown calls it.
class CallbackDispatcher {
public:
    CallbackDispatcher() = default;
    ~CallbackDispatcher() { stop(); }

    CallbackDispatcher(const CallbackDispatcher&) = delete;
    CallbackDispatcher& operator=(const CallbackDispatcher&) = delete;

    // Spawns the dispatch thread (idempotent).
    void start() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (started_) return;
        started_ = true;
        stop_ = false;
        thread_ = std::thread([this] { run(); });
    }

    // Enqueues a task to run on the dispatch thread, in FIFO order. A no-op once stop()
    // has begun (teardown drains only what was queued before stop).
    void post(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) return;
            queue_.push_back(std::move(task));
        }
        cv_.notify_one();
    }

    // Blocks until every task queued BEFORE this call has finished running, by posting a marker
    // and waiting for it. Returns true if it actually waited.
    //
    // This is the primitive behind removeStreamListener(): erasing a listener from the roster
    // stops FUTURE posts from naming it, but every notify* snapshots the roster and captures the
    // POINTERS by value, so a task already queued still holds it. A caller that erases and then
    // destroys the listener races that task. fence() closes exactly that window — after it
    // returns, no previously-queued task can still be holding the pointer.
    //
    // Deliberately NOT stop(): it never sets stop_ and never joins, so the dispatcher stays live
    // and restartable. That distinction is the whole reason this exists rather than a drain —
    // see the issue #89 decision recorded on AudioStreamServer::stop().
    //
    // Returns false WITHOUT waiting in the three cases where waiting is impossible or pointless,
    // each of which is a genuine no-op rather than a silent failure:
    //   - called ON the dispatch thread (a thread cannot wait for itself). The caller is inside a
    //     callback, so the only task that could hold the pointer is the one on its own stack.
    //   - never started, so nothing was ever queued.
    //   - already stopped: post() no-ops from then on and stop() has already drained what was
    //     queued, so no task can still be pending.
    bool fence() {
        // shared_ptr because std::function requires a CopyConstructible target and std::promise
        // is move-only; the refcounted shared state also outlives whichever side finishes last,
        // which a stack-local mutex/condvar pair would not.
        auto reached = std::make_shared<std::promise<void>>();
        std::future<void> ready = reached->get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!started_ || stop_) return false;
            if (thread_.get_id() == std::this_thread::get_id()) return false;
            queue_.push_back([reached] { reached->set_value(); });
        }
        cv_.notify_one();
        // Safe against a concurrent stop(): the marker is queued under the same lock stop() takes,
        // and run() returns only once the queue is EMPTY — so a queued marker is always executed,
        // whether it is reached by the running loop or by stop()'s drain.
        ready.wait();
        return true;
    }

    // Requests stop, drains the remaining queue on the dispatch thread, then joins.
    // Safe to call repeatedly. Returns without joining if called on the dispatch thread
    // itself (re-entrant) — the owner's later call from another thread joins.
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
            // join() can throw std::system_error on a kernel-level thread anomaly. stop() is called
            // from ~CallbackDispatcher and ~AudioStreamClient/~AudioStreamServer — all implicitly
            // noexcept — so a propagating join error would std::terminate. Swallow
            // it; on failure detach so the std::thread destructor (also noexcept) can't terminate on
            // a still-joinable handle. Teardown is best-effort.
            try {
                thread_.join();
            } catch (...) {
                try { thread_.detach(); } catch (...) {}
            }
        }
    }

private:
    void run() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
                if (queue_.empty()) return;  // stop_ requested and the queue is drained
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            task();  // invoked OUTSIDE the lock — a callback may re-enter post()/the owner
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
    std::thread thread_;
    bool started_ = false;
    bool stop_ = false;
};

}  // namespace naudio::net
