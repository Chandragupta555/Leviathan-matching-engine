// Matching-engine tests — plain assert-style, same framework as test_order_book.cpp.

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
// 1. Incoming SELL fully matches a single resting BUY at the BUY's price
//    (execution price = resting/maker price, NOT the taker's price)
// -----------------------------------------------------------------------
TEST(trade_executes_at_resting_price) {
    OrderBook book;
    // Resting BUY at 100.0
    book.addOrder(Order{1, Order::Side::BUY, 100.0, 10});

    // Incoming SELL at 99.0 — crosses the bid at 100.0
    auto trades = book.submitOrder(Order{2, Order::Side::SELL, 99.0, 10});

    REQUIRE(trades.size() == 1);
    // Price must be the resting BUY's price (100.0), NOT the incoming SELL's 99.0
    REQUIRE(trades[0].price == 100.0);
    REQUIRE(trades[0].buy_order_id == 1);
    REQUIRE(trades[0].sell_order_id == 2);
    REQUIRE(trades[0].quantity == 10);
    REQUIRE(book.totalOrderCount() == 0);
}

// -----------------------------------------------------------------------
// 2. Incoming order partially fills a resting order — resting order stays
//    in the book with reduced quantity
// -----------------------------------------------------------------------
TEST(partial_fill_resting_order_remains) {
    OrderBook book;
    book.addOrder(Order{10, Order::Side::SELL, 50.0, 100});

    // Incoming BUY for only 30 — partially fills the resting SELL
    auto trades = book.submitOrder(Order{11, Order::Side::BUY, 50.0, 30});

    REQUIRE(trades.size() == 1);
    REQUIRE(trades[0].quantity == 30);
    REQUIRE(trades[0].price == 50.0);
    REQUIRE(book.totalOrderCount() == 1);      // resting order still lives
    REQUIRE(book.bestAsk().has_value());
    REQUIRE(book.bestAsk().value() == 50.0);   // still at same price

    // The resting order should have 70 remaining — verify by fully filling it
    auto trades2 = book.submitOrder(Order{12, Order::Side::BUY, 50.0, 70});
    REQUIRE(trades2.size() == 1);
    REQUIRE(trades2[0].quantity == 70);
    REQUIRE(book.totalOrderCount() == 0);
}

// -----------------------------------------------------------------------
// 3. Incoming order walks through MULTIPLE price levels and orders,
//    producing TWO trades
// -----------------------------------------------------------------------
TEST(walks_multiple_price_levels) {
    OrderBook book;
    // Resting SELLs at two different price levels
    book.addOrder(Order{20, Order::Side::SELL, 100.0, 5});   // best ask
    book.addOrder(Order{21, Order::Side::SELL, 101.0, 10});  // next level

    // Incoming BUY at 101.0, quantity 12 — fills all 5 at 100.0, then 7 at 101.0
    auto trades = book.submitOrder(Order{22, Order::Side::BUY, 101.0, 12});

    REQUIRE(trades.size() == 2);

    // First trade: against best ask at 100.0
    REQUIRE(trades[0].sell_order_id == 20);
    REQUIRE(trades[0].price == 100.0);
    REQUIRE(trades[0].quantity == 5);

    // Second trade: against next level at 101.0
    REQUIRE(trades[1].sell_order_id == 21);
    REQUIRE(trades[1].price == 101.0);
    REQUIRE(trades[1].quantity == 7);

    // Order 20 fully filled (removed), order 21 partially filled (3 remaining)
    REQUIRE(book.totalOrderCount() == 1);
    REQUIRE(book.bestAsk().value() == 101.0);
}

// -----------------------------------------------------------------------
// 4. Incoming order doesn't cross the spread — zero trades, order rests
// -----------------------------------------------------------------------
TEST(no_crossing_order_rests) {
    OrderBook book;
    book.addOrder(Order{30, Order::Side::SELL, 105.0, 10});

    // Incoming BUY at 100.0 — below the best ask of 105.0, no match
    auto trades = book.submitOrder(Order{31, Order::Side::BUY, 100.0, 10});

    REQUIRE(trades.empty());
    REQUIRE(book.totalOrderCount() == 2);       // both orders rest
    REQUIRE(book.bestBid().value() == 100.0);   // new resting bid
    REQUIRE(book.bestAsk().value() == 105.0);   // unchanged
}

// -----------------------------------------------------------------------
// 5. Exact quantity match — no zero-quantity remainder gets rested
// -----------------------------------------------------------------------
TEST(exact_fill_no_phantom_remainder) {
    OrderBook book;
    book.addOrder(Order{40, Order::Side::BUY, 100.0, 25});

    // Incoming SELL matches exactly 25
    auto trades = book.submitOrder(Order{41, Order::Side::SELL, 100.0, 25});

    REQUIRE(trades.size() == 1);
    REQUIRE(trades[0].quantity == 25);
    // No phantom zero-quantity order should be left
    REQUIRE(book.totalOrderCount() == 0);
    REQUIRE(!book.bestBid().has_value());
    REQUIRE(!book.bestAsk().has_value());
}

// -----------------------------------------------------------------------
// 6. Price-time priority — two resting BUYs at the same price, incoming
//    SELL should match the EARLIER-arrived one first
// -----------------------------------------------------------------------
TEST(price_time_priority) {
    OrderBook book;
    // Order 50 arrives first, then order 51 — both at 100.0
    book.addOrder(Order{50, Order::Side::BUY, 100.0, 10});
    book.addOrder(Order{51, Order::Side::BUY, 100.0, 10});

    // Incoming SELL for only 5 — should match order 50 (earlier) first
    auto trades = book.submitOrder(Order{52, Order::Side::SELL, 100.0, 5});

    REQUIRE(trades.size() == 1);
    REQUIRE(trades[0].buy_order_id == 50);   // earlier order matched first
    REQUIRE(trades[0].quantity == 5);

    // Order 51 should be untouched — verify by cancelling it successfully
    REQUIRE(book.totalOrderCount() == 2);  // order 50 (partial, 5 left) + order 51

    // Now match order 50's remainder
    auto trades2 = book.submitOrder(Order{53, Order::Side::SELL, 100.0, 5});
    REQUIRE(trades2.size() == 1);
    REQUIRE(trades2[0].buy_order_id == 50);  // still order 50 first

    // Now only order 51 remains
    REQUIRE(book.totalOrderCount() == 1);
    // Cancel it to prove it's order 51
    REQUIRE(book.cancelOrder(51));
    REQUIRE(book.totalOrderCount() == 0);
}

// -----------------------------------------------------------------------
// 7. Incoming order with quantity 0 is rejected — book unchanged
// -----------------------------------------------------------------------
TEST(zero_quantity_rejected) {
    OrderBook book;
    book.addOrder(Order{60, Order::Side::BUY, 100.0, 10});
    const size_t count_before = book.totalOrderCount();

    auto trades = book.submitOrder(Order{61, Order::Side::SELL, 100.0, 0});

    REQUIRE(trades.empty());
    REQUIRE(book.totalOrderCount() == count_before);
}

// -----------------------------------------------------------------------
// 8. After full match removes a resting order, cancelOrder() on that
//    order's ID returns false (order_index_ was cleaned up during matching)
// -----------------------------------------------------------------------
TEST(cancel_after_full_match_returns_false) {
    OrderBook book;
    book.addOrder(Order{70, Order::Side::SELL, 100.0, 10});

    // Fully match it
    auto trades = book.submitOrder(Order{71, Order::Side::BUY, 100.0, 10});
    REQUIRE(trades.size() == 1);
    REQUIRE(book.totalOrderCount() == 0);

    // Now try to cancel the fully-matched order — should return false, not crash
    REQUIRE(!book.cancelOrder(70));
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
