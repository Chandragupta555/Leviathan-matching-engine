// Test framework: plain assert-style with clear PASS / FAIL printouts.
// Matches the existing project convention (no Catch2 dependency).

#include "recording_order_book.hpp"
#include "replay.hpp"

#include <cassert>
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <sstream>
#include <string>

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
// 1. Basic recording and replay — ~20 mixed operations.
//    Verifies that replaying a recorded log produces a final book state
//    identical to the live RecordingOrderBook's internal state.
// -----------------------------------------------------------------------
TEST(record_and_replay_mixed_operations) {
    std::ostringstream log_stream;
    RecordingOrderBook recorder(log_stream);

    // --- 20+ mixed operations ---

    // Ops 1-5: Add limit orders (resting, no matching through addOrder)
    recorder.addOrder(Order{1, Order::Side::BUY,  99.00, 50});  // 1
    recorder.addOrder(Order{2, Order::Side::BUY,  98.50, 30});  // 2
    recorder.addOrder(Order{3, Order::Side::BUY,  98.00, 20});  // 3
    recorder.addOrder(Order{4, Order::Side::SELL, 101.00, 40});  // 4
    recorder.addOrder(Order{5, Order::Side::SELL, 102.00, 25});  // 5

    // Op 6: Submit a sell that doesn't cross (price above best bid)
    recorder.submitOrder(Order{6, Order::Side::SELL, 100.00, 15}); // 6 — rests

    // Ops 7-8: Submit orders that DO cross and match
    // Buy at 101.00 crosses sell at 100.00 (order 6) and 101.00 (order 4)
    auto trades1 = recorder.submitOrder(
        Order{7, Order::Side::BUY, 101.00, 50}); // 7 — matches 6 (15) + 4 (35)

    // Op 9: Cancel an order
    bool cancel_ok = recorder.cancelOrder(2);  // 8 — cancel order 2
    REQUIRE(cancel_ok);

    // Op 10: Cancel non-existent order
    bool cancel_fail = recorder.cancelOrder(999);  // 9 — should fail
    REQUIRE(!cancel_fail);

    // Ops 11-14: Add more orders
    recorder.addOrder(Order{8,  Order::Side::BUY,  97.50, 100}); // 10
    recorder.addOrder(Order{9,  Order::Side::SELL, 103.00, 60});  // 11
    recorder.addOrder(Order{10, Order::Side::BUY,  99.50, 45});   // 12
    recorder.addOrder(Order{11, Order::Side::SELL, 100.50, 30});  // 13

    // Op 15: Submit a sell that partially fills
    // Best bid is 99.50 (order 10), then 99.00 (order 1).
    // Sell at 99.00 with qty 70 should match: order 10 (45) + order 1 (25 of 50)
    auto trades2 = recorder.submitOrder(
        Order{12, Order::Side::SELL, 99.00, 70}); // 14

    // Op 16: Modify order — decrease quantity (same price, preserves priority)
    auto mod1 = recorder.modifyOrder(3, 98.00, 10);   // 15 — qty 20 → 10
    REQUIRE(mod1.has_value());
    REQUIRE(mod1->empty()); // no trades from a decrease-only modify

    // Op 17: Modify order — change price (cancel + resubmit, new timestamp)
    auto mod2 = recorder.modifyOrder(8, 99.00, 100);  // 16 — price 97.50 → 99.00
    REQUIRE(mod2.has_value());

    // Op 18: Submit a market buy order that matches against sell side
    auto trades3 = recorder.submitOrder(
        Order{13, Order::Side::BUY, 0.0, 20, Order::OrderType::MARKET}); // 17

    // Op 19: Add another order
    recorder.addOrder(Order{14, Order::Side::SELL, 104.00, 80}); // 18

    // Op 20: Modify order — increase quantity (loses priority, triggers resubmit)
    auto mod3 = recorder.modifyOrder(5, 102.00, 50); // 19
    REQUIRE(mod3.has_value());

    // Op 21: Cancel order
    recorder.cancelOrder(3); // 20 — cancel order 3 (if still there)

    // --- Capture live state ---
    auto live_bid   = recorder.bestBid();
    auto live_ask   = recorder.bestAsk();
    size_t live_count = recorder.totalOrderCount();

    // --- Replay ---
    std::string log_content = log_stream.str();
    std::istringstream replay_stream(log_content);
    ReplayResult result = replayFromStream(replay_stream);

    // --- Verify: no trade mismatches ---
    REQUIRE(result.mismatches == 0);

    // --- Verify: final book state matches ---
    auto replay_bid   = result.book.bestBid();
    auto replay_ask   = result.book.bestAsk();
    size_t replay_count = result.book.totalOrderCount();

    REQUIRE(live_bid.has_value() == replay_bid.has_value());
    if (live_bid.has_value()) {
        REQUIRE(std::fabs(live_bid.value() - replay_bid.value()) < 1e-9);
    }

    REQUIRE(live_ask.has_value() == replay_ask.has_value());
    if (live_ask.has_value()) {
        REQUIRE(std::fabs(live_ask.value() - replay_ask.value()) < 1e-9);
    }

    REQUIRE(live_count == replay_count);
}

// -----------------------------------------------------------------------
// 2. Empty log replays correctly (no crashes, empty book).
// -----------------------------------------------------------------------
TEST(replay_empty_log) {
    std::istringstream empty_stream("");
    ReplayResult result = replayFromStream(empty_stream);

    REQUIRE(result.mismatches == 0);
    REQUIRE(result.replayed_trades.empty());
    REQUIRE(result.logged_trades.empty());
    REQUIRE(result.book.totalOrderCount() == 0);
    REQUIRE(!result.book.bestBid().has_value());
    REQUIRE(!result.book.bestAsk().has_value());
}

// -----------------------------------------------------------------------
// 3. Recording produces correct trade count for matching scenario.
// -----------------------------------------------------------------------
TEST(recording_captures_trades) {
    std::ostringstream log_stream;
    RecordingOrderBook recorder(log_stream);

    recorder.addOrder(Order{1, Order::Side::SELL, 100.00, 10});
    recorder.addOrder(Order{2, Order::Side::SELL, 100.00, 20});

    auto trades = recorder.submitOrder(
        Order{3, Order::Side::BUY, 100.00, 25});

    // Should produce 2 trades: fully fill order 1 (10), partially fill order 2 (15)
    REQUIRE(trades.size() == 2);
    REQUIRE(trades[0].quantity == 10);
    REQUIRE(trades[1].quantity == 15);

    // Replay should match
    std::istringstream replay_stream(log_stream.str());
    ReplayResult result = replayFromStream(replay_stream);
    REQUIRE(result.mismatches == 0);
    REQUIRE(result.replayed_trades.size() == 2);
}

// -----------------------------------------------------------------------
// 4. Modify that triggers matching is correctly logged and replayed.
// -----------------------------------------------------------------------
TEST(modify_with_matching_replays_correctly) {
    std::ostringstream log_stream;
    RecordingOrderBook recorder(log_stream);

    // Resting sell at 100.00
    recorder.addOrder(Order{1, Order::Side::SELL, 100.00, 30});

    // Resting buy at 98.00
    recorder.addOrder(Order{2, Order::Side::BUY, 98.00, 20});

    // Modify buy's price to 100.00 → should match against sell
    auto trades = recorder.modifyOrder(2, 100.00, 20);
    REQUIRE(trades.has_value());
    REQUIRE(trades->size() == 1);
    REQUIRE((*trades)[0].quantity == 20);

    // Replay
    std::istringstream replay_stream(log_stream.str());
    ReplayResult result = replayFromStream(replay_stream);
    REQUIRE(result.mismatches == 0);
    REQUIRE(result.replayed_trades.size() == 1);

    // Final state: sell order 1 should have 10 remaining
    REQUIRE(result.book.totalOrderCount() == 1);
    REQUIRE(result.book.bestAsk().has_value());
    REQUIRE(std::fabs(result.book.bestAsk().value() - 100.00) < 1e-9);
}
// -----------------------------------------------------------------------
// 5. Malformed/corrupt log lines are skipped gracefully.
//    A log with one corrupt line mixed among valid lines should:
//      - NOT crash
//      - Skip the bad line (warning goes to stderr)
//      - Replay all valid surrounding operations correctly
// -----------------------------------------------------------------------
TEST(malformed_line_skipped_gracefully) {
    // Hand-craft a log with a corrupt CANCEL line (missing order_id field).
    // Valid lines surround it to ensure they still replay correctly.
    //
    // Sequence:
    //   ADD order 1 (BUY 99.00 qty 50)   — valid
    //   ADD order 2 (SELL 101.00 qty 30)  — valid
    //   CANCEL,??                         — MALFORMED (missing order_id)
    //   SUBMIT order 3 (BUY 101.00 qty 10) — valid, should match order 2
    //   TRADE from that match             — valid
    //   ADD,garbage,not,a,number          — MALFORMED (unparsable fields)
    //   ADD order 4 (SELL 102.00 qty 20)  — valid
    std::string log =
        "ADD,0,1,0,99.000000,50,0\n"
        "ADD,1,2,1,101.000000,30,0\n"
        "CANCEL,2\n"                        // malformed: missing order_id after seq
        "SUBMIT,3,3,0,101.000000,10,0\n"
        "TRADE,4,3,2,101.000000,10\n"
        "ADD,garbage,not,a,number,x,y\n"    // malformed: non-numeric fields
        "ADD,5,4,1,102.000000,20,0\n";

    // Redirect stderr to capture warnings
    std::ostringstream captured_stderr;
    std::streambuf* original_stderr = std::cerr.rdbuf(captured_stderr.rdbuf());

    std::istringstream input(log);
    ReplayResult result = replayFromStream(input);

    // Restore stderr
    std::cerr.rdbuf(original_stderr);

    // Warnings should have been emitted for the 2 malformed lines
    std::string warnings = captured_stderr.str();
    REQUIRE(warnings.find("[WARN]") != std::string::npos);

    // Trade verification: 1 trade from the valid SUBMIT
    REQUIRE(result.mismatches == 0);
    REQUIRE(result.replayed_trades.size() == 1);
    REQUIRE(result.replayed_trades[0].buy_order_id == 3);
    REQUIRE(result.replayed_trades[0].sell_order_id == 2);
    REQUIRE(result.replayed_trades[0].quantity == 10);

    // Final book state:
    //   order 1 (BUY 99.00 qty 50) — still resting
    //   order 2 (SELL 101.00 qty 20 remaining after partial fill of 10)
    //   order 4 (SELL 102.00 qty 20) — added after the malformed line
    //   The malformed CANCEL and ADD were skipped, so 3 orders remain.
    REQUIRE(result.book.totalOrderCount() == 3);
    REQUIRE(result.book.bestBid().has_value());
    REQUIRE(std::fabs(result.book.bestBid().value() - 99.0) < 1e-9);
    REQUIRE(result.book.bestAsk().has_value());
    REQUIRE(std::fabs(result.book.bestAsk().value() - 101.0) < 1e-9);
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
