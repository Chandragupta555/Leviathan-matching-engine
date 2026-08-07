// Market order tests — plain assert-style, same framework as test_order_book.cpp.

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
// 1. Market BUY against empty ask side → zero trades, nothing rests
// -----------------------------------------------------------------------
TEST(market_buy_empty_ask_side) {
    OrderBook book;
    const size_t count_before = book.totalOrderCount();

    // Submit a MARKET BUY with no asks available — should produce zero trades
    // and the market order must NOT rest in the book.
    auto trades = book.submitOrder(
        Order{1, Order::Side::BUY, 0.0, 10, Order::OrderType::MARKET});

    REQUIRE(trades.empty());
    REQUIRE(book.totalOrderCount() == count_before);  // nothing rests
    REQUIRE(!book.bestBid().has_value());  // no phantom resting market order
}

// -----------------------------------------------------------------------
// 2. Market BUY fully consumes 2 price levels
//    (5 units at 100.0, then remainder at 101.0)
// -----------------------------------------------------------------------
TEST(market_buy_consumes_two_levels) {
    OrderBook book;

    // Set up two ask levels: 5 @ 100.0 and 10 @ 101.0
    book.addOrder(Order{10, Order::Side::SELL, 100.0, 5});
    book.addOrder(Order{11, Order::Side::SELL, 101.0, 10});

    // Submit a MARKET BUY for 12 units — fills 5 @ 100.0 + 7 @ 101.0
    auto trades = book.submitOrder(
        Order{12, Order::Side::BUY, 0.0, 12, Order::OrderType::MARKET});

    REQUIRE(trades.size() == 2);

    // First trade: fills 5 at best ask (100.0) — execution price = resting price
    REQUIRE(trades[0].buy_order_id == 12);
    REQUIRE(trades[0].sell_order_id == 10);
    REQUIRE(trades[0].price == 100.0);
    REQUIRE(trades[0].quantity == 5);

    // Second trade: fills 7 at next level (101.0) — execution price = resting price
    REQUIRE(trades[1].buy_order_id == 12);
    REQUIRE(trades[1].sell_order_id == 11);
    REQUIRE(trades[1].price == 101.0);
    REQUIRE(trades[1].quantity == 7);

    // Total filled: 5 + 7 = 12
    // Order 10 fully filled (removed), order 11 partially filled (3 remaining)
    REQUIRE(book.totalOrderCount() == 1);
    REQUIRE(book.bestAsk().value() == 101.0);
}

// -----------------------------------------------------------------------
// 3. Market BUY with quantity LARGER than total available liquidity
//    → partial fill, remainder DISCARDED (not rested)
// -----------------------------------------------------------------------
TEST(market_buy_partial_fill_remainder_discarded) {
    OrderBook book;

    // Only 15 units of total ask liquidity
    book.addOrder(Order{20, Order::Side::SELL, 100.0, 10});
    book.addOrder(Order{21, Order::Side::SELL, 105.0, 5});

    // Submit a MARKET BUY for 50 — can only fill 15, remainder must be discarded
    auto trades = book.submitOrder(
        Order{22, Order::Side::BUY, 0.0, 50, Order::OrderType::MARKET});

    REQUIRE(trades.size() == 2);
    REQUIRE(trades[0].quantity == 10);  // filled at 100.0
    REQUIRE(trades[1].quantity == 5);   // filled at 105.0

    // All ask-side orders consumed
    REQUIRE(!book.bestAsk().has_value());

    // CRITICAL: the unfilled 35 units of the MARKET order must NOT rest.
    // totalOrderCount must be 0 — no phantom resting market order.
    REQUIRE(book.totalOrderCount() == 0);
    REQUIRE(!book.bestBid().has_value());  // no resting market order on bid side
}

// -----------------------------------------------------------------------
// 4. Existing LIMIT order behavior unaffected — basic regression check
//    A LIMIT BUY that doesn't cross should still rest normally.
// -----------------------------------------------------------------------
TEST(limit_order_still_rests_normally) {
    OrderBook book;

    // Resting SELL at 105.0
    book.addOrder(Order{30, Order::Side::SELL, 105.0, 10});

    // LIMIT BUY at 100.0 — doesn't cross the ask at 105.0, should rest
    auto trades = book.submitOrder(Order{31, Order::Side::BUY, 100.0, 10});

    REQUIRE(trades.empty());
    REQUIRE(book.totalOrderCount() == 2);       // both orders rest
    REQUIRE(book.bestBid().value() == 100.0);   // LIMIT order rests as expected
    REQUIRE(book.bestAsk().value() == 105.0);   // unchanged
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
