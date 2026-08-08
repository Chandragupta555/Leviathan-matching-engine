#pragma once

#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>

// ---------------------------------------------------------------------------
// ThreadSafeQueue<T> — generic, header-only, multi-producer single-consumer
// queue for cross-thread communication.
//
// WHY BLOCKING WAIT (condition_variable), NOT SPIN-WAIT:
// Spin-waiting burns CPU continuously even when idle, which is wasteful and
// actually WORSE for a shared system where other threads (including the OS
// scheduler and other producer threads) need CPU time too.  A
// condition_variable blocks the thread entirely, yielding the CPU to other
// work until a producer actually pushes real work onto the queue.  The OS
// scheduler efficiently wakes the blocked thread when notified, with minimal
// latency compared to a spin that would poll hundreds of thousands of times
// per millisecond for no benefit.  On a matching engine where the matching
// thread may be idle between bursts of orders, blocking is strictly superior.
// ---------------------------------------------------------------------------

template <typename T>
class ThreadSafeQueue {
public:
    // Push an item onto the queue.  Thread-safe, callable from any producer
    // thread concurrently.  Wakes one blocked pop() call.
    void push(T item) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(std::move(item));
        }
        cv_.notify_one();
    }

    // Blocking pop: waits (via condition_variable, NOT spin) until an item
    // is available OR shutdown has been signaled.
    //
    // Returns std::optional<T>:
    //   - An item if one was available (even after shutdown — remaining items
    //     are drained before returning nullopt).
    //   - std::nullopt once the queue is both shut down AND empty, signaling
    //     the consumer to exit its processing loop.
    //
    // This drain-before-exit design ensures that all queued requests are
    // processed and their promises fulfilled, preventing futures from hanging
    // indefinitely.
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !queue_.empty() || shutdown_; });

        if (queue_.empty()) {
            return std::nullopt;  // shutdown signaled AND queue is drained
        }

        T item = std::move(queue_.front());
        queue_.pop();
        return item;
    }

    // Signal shutdown: wakes ALL blocked pop() calls.  After remaining items
    // are drained, pop() returns std::nullopt to each waiting consumer.
    // Thread-safe, callable from any thread.
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            shutdown_ = true;
        }
        cv_.notify_all();
    }

private:
    std::queue<T>           queue_;
    mutable std::mutex      mutex_;
    std::condition_variable cv_;
    bool                    shutdown_{false};
};
