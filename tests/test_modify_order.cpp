// Order modification tests — plain assert-style, same framework as test_order_book.cpp.

#include "order_book.hpp"

#include <cassert>
#include <cstdlib>
#include <iostream>
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
// 1. Quantity decrease only, same price → order stays in place,
//    time priority preserved.
//    Setup: add A, add B at same price. Decrease A's qty. Submit a
//    crossing order that only partially fills the level → A matches
//    first, proving it's still at the front.
// -----------------------------------------------------------------------
TEST(quantity_decrease_preserves_time_priority) {
    OrderBook book;

    // A arrives first, then B — both BUY at 100.0
    book.addOrder(Order{1, Order::Side::BUY, 100.0, 20});  // order A
    book.addOrder(Order{2, Order::Side::BUY, 100.0, 10});  // order B

    // Decrease A's quantity from 20 to 5 (same price — CASE 1)
    auto result = book.modifyOrder(1, 100.0, 5);
    REQUIRE(result.has_value());
    REQUIRE(result->empty());  // no trades from a decrease

    // Now submit a crossing SELL for 8 units at 100.0
    // Should match A (5 units, fully filling A) then B (3 units, partial)
    auto trades = book.submitOrder(Order{3, Order::Side::SELL, 100.0, 8});

    REQUIRE(trades.size() == 2);

    // First trade is against order A (time priority preserved!)
    REQUIRE(trades[0].buy_order_id == 1);
    REQUIRE(trades[0].quantity == 5);

    // Second trade is against order B
    REQUIRE(trades[1].buy_order_id == 2);
    REQUIRE(trades[1].quantity == 3);

    // A fully filled (removed), B partially filled (7 remaining)
    REQUIRE(book.totalOrderCount() == 1);
}

// -----------------------------------------------------------------------
// 2. Price change → order loses time priority.
//    Setup: add A and B at same price. Modify A to a DIFFERENT price
//    (still on same side). Add C back at A's original price. Verify
//    that at the original price level, B is now first (A moved away).
// -----------------------------------------------------------------------
TEST(price_change_loses_time_priority) {
    OrderBook book;

    // A and B are both BUY at 100.0
    book.addOrder(Order{10, Order::Side::BUY, 100.0, 10});  // order A
    book.addOrder(Order{11, Order::Side::BUY, 100.0, 10});  // order B

    // Modify A's price from 100.0 to 99.0 (CASE 2 — price change)
    auto result = book.modifyOrder(10, 99.0, 10);
    REQUIRE(result.has_value());
    REQUIRE(result->empty());  // 99.0 doesn't cross any ask, no trades

    // Now at price 100.0, only B remains. At 99.0, A is there.
    // Submit a SELL at 100.0 for 5 — should match B (the only one at 100.0)
    auto trades = book.submitOrder(Order{12, Order::Side::SELL, 100.0, 5});

    REQUIRE(trades.size() == 1);
    REQUIRE(trades[0].buy_order_id == 11);  // B, not A
    REQUIRE(trades[0].quantity == 5);

    // B partially filled (5 left), A at 99.0 (untouched)
    REQUIRE(book.totalOrderCount() == 2);
    REQUIRE(book.bestBid().value() == 100.0);  // B's remaining at 100.0
}

// -----------------------------------------------------------------------
// 3. Quantity increase → same time-priority-loss as price change.
//    Setup: A and B at same price. Increase A's quantity → A should
//    lose its position at the front.
// -----------------------------------------------------------------------
TEST(quantity_increase_loses_time_priority) {
    OrderBook book;

    // A and B both SELL at 100.0
    book.addOrder(Order{20, Order::Side::SELL, 100.0, 5});   // order A
    book.addOrder(Order{21, Order::Side::SELL, 100.0, 10});  // order B

    // Increase A's quantity from 5 to 15 (CASE 2 — qty increase, same price)
    auto result = book.modifyOrder(20, 100.0, 15);
    REQUIRE(result.has_value());
    REQUIRE(result->empty());  // no crossing, no trades

    // Now submit a BUY at 100.0 for 8 — should match B first (A lost priority)
    auto trades = book.submitOrder(Order{22, Order::Side::BUY, 100.0, 8});

    REQUIRE(trades.size() == 1);
    REQUIRE(trades[0].sell_order_id == 21);  // B matched first, not A
    REQUIRE(trades[0].quantity == 8);

    // B partially filled (2 left), A untouched (15)
    REQUIRE(book.totalOrderCount() == 2);
}

// -----------------------------------------------------------------------
// 4. Modify causes an immediate match — new price crosses the spread.
//    Setup: resting SELL at 100.0. Resting BUY at 95.0. Modify BUY's
//    price to 100.0 → should now cross and produce trades.
// -----------------------------------------------------------------------
TEST(modify_causes_immediate_match) {
    OrderBook book;

    book.addOrder(Order{30, Order::Side::SELL, 100.0, 10});
    book.addOrder(Order{31, Order::Side::BUY,  95.0,  15});

    REQUIRE(book.totalOrderCount() == 2);

    // Modify the BUY's price from 95.0 to 100.0 — now crosses the ask
    auto result = book.modifyOrder(31, 100.0, 15);
    REQUIRE(result.has_value());
    REQUIRE(result->size() == 1);

    // Trade: BUY 31 matches SELL 30 at resting price 100.0, qty 10
    REQUIRE((*result)[0].buy_order_id == 31);
    REQUIRE((*result)[0].sell_order_id == 30);
    REQUIRE((*result)[0].price == 100.0);
    REQUIRE((*result)[0].quantity == 10);

    // SELL 30 fully filled (removed). BUY 31 has 5 remaining, rests at 100.0.
    REQUIRE(book.totalOrderCount() == 1);
    REQUIRE(book.bestBid().value() == 100.0);
}

// -----------------------------------------------------------------------
// 5. modifyOrder on non-existent ID → returns std::nullopt, book unchanged.
// -----------------------------------------------------------------------
TEST(modify_nonexistent_returns_nullopt) {
    OrderBook book;
    book.addOrder(Order{40, Order::Side::BUY, 100.0, 10});

    const size_t count_before = book.totalOrderCount();

    auto result = book.modifyOrder(9999, 105.0, 5);

    REQUIRE(!result.has_value());  // std::nullopt
    REQUIRE(book.totalOrderCount() == count_before);  // unchanged
}

// -----------------------------------------------------------------------
// 6. Modify to new_quantity = 0 → order is removed (implicit cancel).
//    Verify via totalOrderCount decreasing and cancelOrder returning false.
// -----------------------------------------------------------------------
TEST(modify_to_zero_quantity_removes_order) {
    OrderBook book;
    book.addOrder(Order{50, Order::Side::BUY, 100.0, 10});
    book.addOrder(Order{51, Order::Side::SELL, 105.0, 5});

    REQUIRE(book.totalOrderCount() == 2);

    // Modify order 50 to quantity 0 — should act as cancellation
    auto result = book.modifyOrder(50, 100.0, 0);
    REQUIRE(result.has_value());
    REQUIRE(result->empty());  // no trades from a cancellation

    REQUIRE(book.totalOrderCount() == 1);  // order 50 removed

    // Confirm cancelOrder on the same ID now returns false (already gone)
    REQUIRE(!book.cancelOrder(50));

    // Order 51 should still be there
    REQUIRE(book.bestAsk().value() == 105.0);
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
