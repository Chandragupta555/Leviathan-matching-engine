// Test framework: plain assert-style with clear PASS / FAIL printouts.
// Matches the existing project convention (no Catch2 dependency).

#include "matching_engine.hpp"

#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

static int g_pass = 0;
static int g_fail = 0;

#define TEST(name) \
    static void test_##name(); \
    struct Register_##name { \
        Register_##name() { \
            try { \
                test_##name(); \
                std::cout << "[PASS] " #name "\n"; \
                ++g_pass; \
            } catch (const std::exception& e) { \
                std::cout << "[FAIL] " #name " — " << e.what() << "\n"; \
                ++g_fail; \
            } catch (...) { \
                std::cout << "[FAIL] " #name " — unknown exception\n"; \
                ++g_fail; \
            } \
        } \
    } g_register_##name; \
    static void test_##name()

#define REQUIRE(expr) \
    do { \
        if (!(expr)) { \
            throw std::runtime_error( \
                std::string("Assertion failed: ") + #expr + \
                " (" + __FILE__ + ":" + std::to_string(__LINE__) + ")"); \
        } \
    } while (false)

// -----------------------------------------------------------------------
// 1. Single-threaded sanity check — validates plumbing before concurrency.
//    Submit orders via the async API, .get() futures, confirm results match
//    what calling OrderBook directly would produce.
// -----------------------------------------------------------------------
TEST(single_threaded_sanity) {
    MatchingEngine engine;
    engine.start();

    // Submit a sell (rests — no crossing buy exists yet).
    auto f1 = engine.submitOrderAsync(
        Order{1, Order::Side::SELL, 100.0, 30});
    auto trades1 = f1.get();
    REQUIRE(trades1.empty());

    // Verify book state via queue-routed reads.
    REQUIRE(engine.totalOrderCount() == 1);
    REQUIRE(engine.bestAsk().has_value());
    REQUIRE(std::fabs(engine.bestAsk().value() - 100.0) < 1e-9);
    REQUIRE(!engine.bestBid().has_value());

    // Submit a buy that crosses the resting sell — partial fill.
    auto f2 = engine.submitOrderAsync(
        Order{2, Order::Side::BUY, 100.0, 20});
    auto trades2 = f2.get();
    REQUIRE(trades2.size() == 1);
    REQUIRE(trades2[0].buy_order_id == 2);
    REQUIRE(trades2[0].sell_order_id == 1);
    REQUIRE(trades2[0].quantity == 20);
    REQUIRE(std::fabs(trades2[0].price - 100.0) < 1e-9);

    // Sell has 10 remaining.
    REQUIRE(engine.totalOrderCount() == 1);
    REQUIRE(engine.bestAsk().has_value());
    REQUIRE(std::fabs(engine.bestAsk().value() - 100.0) < 1e-9);

    // Cancel the remaining sell.
    auto f3 = engine.cancelOrderAsync(1);
    REQUIRE(f3.get() == true);
    REQUIRE(engine.totalOrderCount() == 0);

    // Cancel a non-existent order.
    auto f4 = engine.cancelOrderAsync(999);
    REQUIRE(f4.get() == false);

    engine.stop();
}

// -----------------------------------------------------------------------
// 2. Multi-threaded stress test — real concurrent access.
//
//    Design: 8 producer threads, each submitting 1,000 LIMIT orders via
//    submitOrderAsync.  All BUY orders are priced at 10–49, all SELL
//    orders at 200–249.  Since best ask (200) > best bid (49), NO matching
//    ever occurs regardless of arrival order at the queue.  This makes the
//    expected final state fully deterministic:
//      - totalOrderCount == 8,000  (all orders rest)
//      - every future returns an empty trades vector
//      - every future is fulfilled (no hanging/deadlock)
//
//    The non-deterministic arrival order at the queue is the expected
//    consequence of concurrent push(), as documented in MatchingEngine's
//    class comment.
// -----------------------------------------------------------------------
TEST(multi_threaded_stress) {
    MatchingEngine engine;
    engine.start();

    constexpr int NUM_THREADS = 8;
    constexpr int ORDERS_PER_THREAD = 1000;

    // Each producer thread stores its own futures — no cross-thread access.
    std::vector<std::vector<std::future<std::vector<Trade>>>> all_futures(NUM_THREADS);

    std::vector<std::thread> producers;
    for (int t = 0; t < NUM_THREADS; ++t) {
        producers.emplace_back([&engine, &all_futures, t]() {
            all_futures[t].reserve(ORDERS_PER_THREAD);
            for (int i = 0; i < ORDERS_PER_THREAD; ++i) {
                uint64_t id = static_cast<uint64_t>(t) * 10000 + i;
                // Threads 0-3: BUY at 10–49.  Threads 4-7: SELL at 200–249.
                // These price ranges never cross, so no matching occurs.
                Order::Side side = (t < NUM_THREADS / 2)
                    ? Order::Side::BUY : Order::Side::SELL;
                double price = (side == Order::Side::BUY)
                    ? 10.0 + (i % 40) : 200.0 + (i % 50);
                auto fut = engine.submitOrderAsync(
                    Order{id, side, price, 1});
                all_futures[t].push_back(std::move(fut));
            }
        });
    }

    // Join all producer threads (all pushes complete).
    for (auto& t : producers) {
        t.join();
    }

    // Verify: every single future was fulfilled — no future left pending.
    // If any future hangs, this loop would hang → test failure by timeout.
    int total_trades = 0;
    int futures_fulfilled = 0;
    for (int t = 0; t < NUM_THREADS; ++t) {
        for (auto& f : all_futures[t]) {
            auto trades = f.get();
            total_trades += static_cast<int>(trades.size());
            ++futures_fulfilled;
        }
    }

    // (a) No crash or hang — we reached here.
    // (b) No matching should have occurred.
    REQUIRE(total_trades == 0);
    // (c) All futures fulfilled.
    REQUIRE(futures_fulfilled == NUM_THREADS * ORDERS_PER_THREAD);

    // All orders should be resting in the book.
    size_t count = engine.totalOrderCount();
    REQUIRE(count == static_cast<size_t>(NUM_THREADS * ORDERS_PER_THREAD));

    engine.stop();
}

// -----------------------------------------------------------------------
// 3. Clean shutdown — start engine, submit work, stop, verify no hang.
// -----------------------------------------------------------------------
TEST(clean_shutdown) {
    MatchingEngine engine;
    engine.start();

    // Submit a few orders (fire-and-forget + future-based).
    engine.addOrderAsync(Order{1, Order::Side::BUY, 50.0, 10});
    auto f = engine.submitOrderAsync(Order{2, Order::Side::SELL, 100.0, 20});

    engine.stop();
    // If we reach here, stop() returned cleanly (matching thread was joined,
    // all queued requests were processed).

    // The future should also be fulfilled (the request was in the queue
    // when stop() was called, so it was processed during draining).
    auto trades = f.get();
    REQUIRE(trades.empty());  // prices don't cross
}

// -----------------------------------------------------------------------
// 4. Construct and destroy without start/stop — destructor handles the
//    "never started" case gracefully (no crash, no hang).
// -----------------------------------------------------------------------
TEST(construct_destroy_no_start) {
    MatchingEngine engine;
    // Immediately goes out of scope — destructor calls stop(), which
    // detects that start() was never called and returns immediately.
}

// -----------------------------------------------------------------------
// 5. Multiple stop() calls are idempotent — no crash on double stop.
// -----------------------------------------------------------------------
TEST(double_stop_is_safe) {
    MatchingEngine engine;
    engine.start();
    engine.stop();
    engine.stop();  // should not crash or hang
}

// -----------------------------------------------------------------------
// main — prints summary
// -----------------------------------------------------------------------
int main() {
    std::cout << "\n========================================\n";
    std::cout << "  Results:  " << g_pass << " passed,  "
              << g_fail << " failed\n";
    std::cout << "========================================\n";
    return g_fail == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
