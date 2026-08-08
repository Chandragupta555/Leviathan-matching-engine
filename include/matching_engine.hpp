#pragma once

#include "order_book.hpp"
#include "thread_safe_queue.hpp"

#include <cstdint>
#include <future>
#include <optional>
#include <thread>
#include <variant>
#include <vector>

// ---------------------------------------------------------------------------
// Request types — one struct per operation, carrying the data needed to
// execute it and (where applicable) a std::promise to deliver the result
// back to the calling thread asynchronously.
// ---------------------------------------------------------------------------

struct AddRequest {
    Order order;
    // Fire-and-forget: no promise/future needed — the caller doesn't need
    // to know when the add completes.
};

struct CancelRequest {
    uint64_t order_id;
    std::promise<bool> promise;
};

struct SubmitRequest {
    Order order;
    std::promise<std::vector<Trade>> promise;
};

struct ModifyRequest {
    uint64_t order_id;
    double new_price;
    uint64_t new_quantity;
    std::promise<std::optional<std::vector<Trade>>> promise;
};

struct BestBidRequest {
    std::promise<std::optional<double>> promise;
};

struct BestAskRequest {
    std::promise<std::optional<double>> promise;
};

struct TotalOrderCountRequest {
    std::promise<size_t> promise;
};

using Request = std::variant<
    AddRequest, CancelRequest, SubmitRequest, ModifyRequest,
    BestBidRequest, BestAskRequest, TotalOrderCountRequest
>;

// ---------------------------------------------------------------------------
// MatchingEngine — single-writer, multi-reader-producer wrapper around
//                  OrderBook.
//
// THREAD SAFETY INVARIANT:
// The internal OrderBook (book_) is ONLY EVER accessed by the matching
// thread for its entire lifetime once the engine is running.  No other
// thread may call any OrderBook method directly.  All mutations (addOrder,
// cancelOrder, submitOrder, modifyOrder) AND all reads (bestBid, bestAsk,
// totalOrderCount) are routed through a thread-safe request queue and
// processed sequentially by the single matching thread.
//
// This guarantees that OrderBook — which is intentionally single-threaded
// and NOT internally synchronized — is never subject to concurrent access
// of any kind.
//
// PRODUCER ORDERING NOTE:
// Multiple producer threads may submit requests concurrently.  The order
// in which their requests arrive at the internal queue is determined by
// the mutex acquisition order inside ThreadSafeQueue::push(), which is
// non-deterministic across runs.  This is expected and correct — a real
// exchange's sequencer similarly stamps arrival order at the gateway, not
// origination order at the client.
//
// READ-ACCESS DESIGN CHOICE (queue-routed reads):
// Read-only methods (bestBid, bestAsk, totalOrderCount) are routed through
// the SAME queue+future mechanism as mutations, rather than using a
// separate reader-writer lock.  Justification:
//
//   1. CORRECTNESS: OrderBook's internal std::map and std::unordered_map
//      are NOT safe for concurrent read+write.  A reader thread calling
//      bestBid() while the matching thread modifies bids_ is undefined
//      behavior.  Any non-queue approach requires explicit synchronization.
//
//   2. SIMPLICITY: Routing reads through the queue requires zero additional
//      synchronization primitives — the existing queue mutex is sufficient.
//      A reader-writer lock (std::shared_mutex) would require acquiring a
//      shared lock on every read AND an exclusive lock on every mutation,
//      adding complexity and potential for deadlocks.
//
//   3. INVARIANT PRESERVATION: The strict single-writer invariant is
//      maintained — OrderBook is only ever touched by one thread, period.
//      A reader-writer lock would explicitly violate this by allowing
//      concurrent reads from non-matching threads.
//
//   4. CONSISTENCY: Reads see a fully consistent snapshot — no torn reads,
//      no partially-applied modifications, no stale data from a previous
//      cache line.
//
//   TRADE-OFF: Reads have higher latency (queued behind pending mutations).
//   This is acceptable because monitoring/snapshot reads are typically
//   infrequent and do not need sub-microsecond response times.
// ---------------------------------------------------------------------------

class MatchingEngine {
public:
    MatchingEngine() = default;
    ~MatchingEngine();

    // Non-copyable, non-movable (owns a thread handle).
    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;
    MatchingEngine(MatchingEngine&&) = delete;
    MatchingEngine& operator=(MatchingEngine&&) = delete;

    // Spawn the matching thread.  Must be called before submitting requests.
    void start();

    // Signal shutdown, drain remaining queued requests, and join the matching
    // thread.  Safe to call multiple times (idempotent).  All queued requests
    // are processed (promises fulfilled) before the thread exits, so no
    // future is left forever pending.
    void stop();

    // --- Mutation methods (thread-safe, callable from any producer thread) ---

    // Fire-and-forget: pushes an add-order request onto the queue.  No result.
    void addOrderAsync(const Order& order);

    std::future<bool> cancelOrderAsync(uint64_t order_id);
    std::future<std::vector<Trade>> submitOrderAsync(Order order);
    std::future<std::optional<std::vector<Trade>>> modifyOrderAsync(
        uint64_t order_id, double new_price, uint64_t new_quantity);

    // --- Read-only snapshot methods (routed through the queue) ---
    // These block until the matching thread processes the read request,
    // ensuring a consistent snapshot.

    std::optional<double> bestBid();
    std::optional<double> bestAsk();
    size_t totalOrderCount();

private:
    OrderBook                book_;      // Only touched by matching_thread_
    ThreadSafeQueue<Request> queue_;
    std::thread              matching_thread_;
    bool                     started_{false};
    bool                     stopped_{false};

    // The matching thread's main loop: pop requests (blocking) and process
    // them one at a time until shutdown is signaled and the queue is drained.
    void matchingLoop_();

    // Dispatch a single request to the appropriate OrderBook method and
    // fulfill the caller's promise (if applicable).
    void processRequest_(Request& req);
};
