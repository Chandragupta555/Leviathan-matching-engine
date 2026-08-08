#include "matching_engine.hpp"

#include <type_traits>
#include <utility>

// ---------------------------------------------------------------------------
// Destructor — ensures the matching thread is always joined.
// No detached threads, no leaked thread handles.
// ---------------------------------------------------------------------------
MatchingEngine::~MatchingEngine() {
    stop();
}

// ---------------------------------------------------------------------------
// start / stop — lifecycle management.
// ---------------------------------------------------------------------------

void MatchingEngine::start() {
    if (started_) return;  // idempotent
    started_ = true;
    matching_thread_ = std::thread(&MatchingEngine::matchingLoop_, this);
}

void MatchingEngine::stop() {
    if (!started_ || stopped_) return;  // idempotent / never-started case
    stopped_ = true;
    queue_.shutdown();
    if (matching_thread_.joinable()) {
        matching_thread_.join();
    }
}

// ---------------------------------------------------------------------------
// Async mutation methods — push requests onto the queue.
// All are thread-safe (the queue handles synchronization internally).
// ---------------------------------------------------------------------------

void MatchingEngine::addOrderAsync(const Order& order) {
    queue_.push(AddRequest{order});
}

std::future<bool> MatchingEngine::cancelOrderAsync(uint64_t order_id) {
    CancelRequest req;
    req.order_id = order_id;
    auto future = req.promise.get_future();
    queue_.push(std::move(req));
    return future;
}

std::future<std::vector<Trade>> MatchingEngine::submitOrderAsync(Order order) {
    std::promise<std::vector<Trade>> promise;
    auto future = promise.get_future();
    queue_.push(SubmitRequest{std::move(order), std::move(promise)});
    return future;
}

std::future<std::optional<std::vector<Trade>>> MatchingEngine::modifyOrderAsync(
    uint64_t order_id, double new_price, uint64_t new_quantity)
{
    ModifyRequest req;
    req.order_id = order_id;
    req.new_price = new_price;
    req.new_quantity = new_quantity;
    auto future = req.promise.get_future();
    queue_.push(std::move(req));
    return future;
}

// ---------------------------------------------------------------------------
// Read-only snapshot methods — routed through the queue for consistency.
// These block the caller until the matching thread processes the read request.
// ---------------------------------------------------------------------------

std::optional<double> MatchingEngine::bestBid() {
    BestBidRequest req;
    auto future = req.promise.get_future();
    queue_.push(std::move(req));
    return future.get();
}

std::optional<double> MatchingEngine::bestAsk() {
    BestAskRequest req;
    auto future = req.promise.get_future();
    queue_.push(std::move(req));
    return future.get();
}

size_t MatchingEngine::totalOrderCount() {
    TotalOrderCountRequest req;
    auto future = req.promise.get_future();
    queue_.push(std::move(req));
    return future.get();
}

// ---------------------------------------------------------------------------
// matchingLoop_ — the matching thread's main loop.
//
// Pops requests from the queue (blocking via condition_variable when idle)
// and processes them one at a time.  Exits cleanly when the queue signals
// shutdown AND all remaining items have been drained.
//
// CRITICAL: This is the ONLY function that ever calls OrderBook methods.
// No other thread touches book_ at any point.
// ---------------------------------------------------------------------------
void MatchingEngine::matchingLoop_() {
    while (auto req = queue_.pop()) {
        processRequest_(*req);
    }
    // queue_.pop() returned std::nullopt → shutdown + drained → exit.
}

// ---------------------------------------------------------------------------
// processRequest_ — dispatch a single request to the appropriate OrderBook
// method and fulfill the caller's promise.
//
// Uses std::visit with a generic lambda and if-constexpr to dispatch by
// request type without a manual switch/case or virtual dispatch.
// ---------------------------------------------------------------------------
void MatchingEngine::processRequest_(Request& req) {
    std::visit([this](auto& r) {
        using T = std::decay_t<decltype(r)>;

        if constexpr (std::is_same_v<T, AddRequest>) {
            book_.addOrder(r.order);
            // Fire-and-forget: no promise to fulfill.

        } else if constexpr (std::is_same_v<T, CancelRequest>) {
            r.promise.set_value(book_.cancelOrder(r.order_id));

        } else if constexpr (std::is_same_v<T, SubmitRequest>) {
            r.promise.set_value(book_.submitOrder(r.order));

        } else if constexpr (std::is_same_v<T, ModifyRequest>) {
            r.promise.set_value(
                book_.modifyOrder(r.order_id, r.new_price, r.new_quantity));

        } else if constexpr (std::is_same_v<T, BestBidRequest>) {
            r.promise.set_value(book_.bestBid());

        } else if constexpr (std::is_same_v<T, BestAskRequest>) {
            r.promise.set_value(book_.bestAsk());

        } else if constexpr (std::is_same_v<T, TotalOrderCountRequest>) {
            r.promise.set_value(book_.totalOrderCount());
        }
    }, req);
}
