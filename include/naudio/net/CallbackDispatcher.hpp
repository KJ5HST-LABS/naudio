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
