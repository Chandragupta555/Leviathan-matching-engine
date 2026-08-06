// Test framework: plain assert-style with clear PASS / FAIL printouts.
// Catch2 was not assumed to be available; this keeps the dependency footprint
// at zero and still gives unambiguous per-test output.

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
// 1. Buy orders at different prices → bestBid() returns highest price
// -----------------------------------------------------------------------
TEST(best_bid_returns_highest_price) {
    OrderBook book;
    book.addOrder(Order{1, Order::Side::BUY, 100.0, 10});
    book.addOrder(Order{2, Order::Side::BUY, 102.0, 10});
    book.addOrder(Order{3, Order::Side::BUY, 99.5,  10});

    auto best = book.bestBid();
    REQUIRE(best.has_value());
    REQUIRE(best.value() == 102.0);
}

// -----------------------------------------------------------------------
// 2. Sell orders at different prices → bestAsk() returns lowest price
// -----------------------------------------------------------------------
TEST(best_ask_returns_lowest_price) {
    OrderBook book;
    book.addOrder(Order{10, Order::Side::SELL, 105.0, 5});
    book.addOrder(Order{11, Order::Side::SELL, 103.0, 5});
    book.addOrder(Order{12, Order::Side::SELL, 107.0, 5});

    auto best = book.bestAsk();
    REQUIRE(best.has_value());
    REQUIRE(best.value() == 103.0);
}

// -----------------------------------------------------------------------
// 3. Two orders at the SAME price are both stored
// -----------------------------------------------------------------------
TEST(same_price_orders_both_stored) {
    OrderBook book;
    book.addOrder(Order{20, Order::Side::BUY, 100.0, 10});
    book.addOrder(Order{21, Order::Side::BUY, 100.0, 20});

    REQUIRE(book.totalOrderCount() == 2);

    // Both are at the best bid price; cancelling one should leave the other.
    REQUIRE(book.cancelOrder(20));
    REQUIRE(book.totalOrderCount() == 1);

    auto best = book.bestBid();
    REQUIRE(best.has_value());
    REQUIRE(best.value() == 100.0);  // still there via order 21
}

// -----------------------------------------------------------------------
// 4. cancelOrder removes the best bid, next-best becomes new best
// -----------------------------------------------------------------------
TEST(cancel_best_bid_updates_correctly) {
    OrderBook book;
    book.addOrder(Order{30, Order::Side::BUY, 100.0, 10});
    book.addOrder(Order{31, Order::Side::BUY, 102.0, 10});  // best
    book.addOrder(Order{32, Order::Side::BUY, 101.0, 10});

    REQUIRE(book.bestBid().value() == 102.0);

    REQUIRE(book.cancelOrder(31));
    REQUIRE(book.bestBid().value() == 101.0);  // next best

    // Same test for sell side
    OrderBook book2;
    book2.addOrder(Order{33, Order::Side::SELL, 200.0, 5});
    book2.addOrder(Order{34, Order::Side::SELL, 198.0, 5});  // best
    book2.addOrder(Order{35, Order::Side::SELL, 199.0, 5});

    REQUIRE(book2.bestAsk().value() == 198.0);

    REQUIRE(book2.cancelOrder(34));
    REQUIRE(book2.bestAsk().value() == 199.0);
}

// -----------------------------------------------------------------------
// 5. cancelOrder on non-existent ID returns false, no crash
// -----------------------------------------------------------------------
TEST(cancel_nonexistent_returns_false) {
    OrderBook book;
    book.addOrder(Order{40, Order::Side::BUY, 100.0, 10});

    REQUIRE(!book.cancelOrder(9999));
    REQUIRE(book.totalOrderCount() == 1);  // unchanged
}

// -----------------------------------------------------------------------
// 6. Empty book — bestBid/bestAsk return nullopt, no crash
// -----------------------------------------------------------------------
TEST(empty_book_returns_nullopt) {
    OrderBook book;

    REQUIRE(!book.bestBid().has_value());
    REQUIRE(!book.bestAsk().has_value());
    REQUIRE(book.totalOrderCount() == 0);
}

// -----------------------------------------------------------------------
// 7. totalOrderCount accurate after adds and cancels
// -----------------------------------------------------------------------
TEST(total_order_count_accurate) {
    OrderBook book;
    REQUIRE(book.totalOrderCount() == 0);

    book.addOrder(Order{50, Order::Side::BUY,  100.0, 10});
    book.addOrder(Order{51, Order::Side::SELL, 105.0, 10});
    book.addOrder(Order{52, Order::Side::BUY,  101.0, 10});
    REQUIRE(book.totalOrderCount() == 3);

    REQUIRE(book.cancelOrder(51));
    REQUIRE(book.totalOrderCount() == 2);

    REQUIRE(book.cancelOrder(50));
    REQUIRE(book.totalOrderCount() == 1);

    book.addOrder(Order{53, Order::Side::SELL, 104.0, 5});
    REQUIRE(book.totalOrderCount() == 2);

    REQUIRE(book.cancelOrder(52));
    REQUIRE(book.cancelOrder(53));
    REQUIRE(book.totalOrderCount() == 0);

    // Book is empty again — bestBid/bestAsk should be nullopt
    REQUIRE(!book.bestBid().has_value());
    REQUIRE(!book.bestAsk().has_value());
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
