// Test framework: plain assert-style with clear PASS / FAIL printouts.
// Matches the existing project convention (no Catch2 dependency).

#include "order_book.hpp"

#include <cassert>
#include <cmath>
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

// =======================================================================
//  IOC TESTS
// =======================================================================

// -----------------------------------------------------------------------
// 1. IOC: partial fill occurs, remainder is DISCARDED (not rested).
// -----------------------------------------------------------------------
TEST(ioc_partial_fill_remainder_discarded) {
    OrderBook book;

    // Resting sell: 30 shares at 100.00
    book.addOrder(Order{1, Order::Side::SELL, 100.00, 30});

    // IOC buy for 50 at 100.00 — should fill 30, discard remaining 20.
    auto trades = book.submitOrder(
        Order{2, Order::Side::BUY, 100.00, 50, Order::OrderType::IOC});

    REQUIRE(trades.size() == 1);
    REQUIRE(trades[0].buy_order_id == 2);
    REQUIRE(trades[0].sell_order_id == 1);
    REQUIRE(trades[0].quantity == 30);

    // Resting sell is fully consumed, IOC remainder does NOT rest.
    REQUIRE(book.totalOrderCount() == 0);
    REQUIRE(!book.bestBid().has_value());   // no phantom IOC resting
    REQUIRE(!book.bestAsk().has_value());   // sell fully consumed
}

// -----------------------------------------------------------------------
// 2. IOC: zero crossing liquidity → zero trades, nothing rests.
// -----------------------------------------------------------------------
TEST(ioc_no_liquidity_zero_trades) {
    OrderBook book;

    // Resting sell at 105.00 — does NOT cross a buy at 100.00.
    book.addOrder(Order{1, Order::Side::SELL, 105.00, 30});

    auto trades = book.submitOrder(
        Order{2, Order::Side::BUY, 100.00, 50, Order::OrderType::IOC});

    REQUIRE(trades.empty());

    // Book unchanged: only the original sell order rests.
    REQUIRE(book.totalOrderCount() == 1);
    REQUIRE(!book.bestBid().has_value());   // IOC did NOT rest
    REQUIRE(book.bestAsk().has_value());
    REQUIRE(book.bestAsk().value() == 105.00);
}

// -----------------------------------------------------------------------
// 3. IOC: full fill — behaves like a LIMIT that happens to fully match.
// -----------------------------------------------------------------------
TEST(ioc_full_fill) {
    OrderBook book;

    book.addOrder(Order{1, Order::Side::SELL, 100.00, 50});

    auto trades = book.submitOrder(
        Order{2, Order::Side::BUY, 100.00, 50, Order::OrderType::IOC});

    REQUIRE(trades.size() == 1);
    REQUIRE(trades[0].quantity == 50);

    // Both sides fully consumed, nothing rests.
    REQUIRE(book.totalOrderCount() == 0);
}

// =======================================================================
//  FOK TESTS
// =======================================================================

// -----------------------------------------------------------------------
// 4. FOK: sufficient liquidity → fills completely.
// -----------------------------------------------------------------------
TEST(fok_sufficient_liquidity_fills) {
    OrderBook book;

    book.addOrder(Order{1, Order::Side::SELL, 100.00, 30});
    book.addOrder(Order{2, Order::Side::SELL, 100.00, 30});

    // FOK buy for 50 at 100.00 — 60 available, so 50 can be filled.
    auto trades = book.submitOrder(
        Order{3, Order::Side::BUY, 100.00, 50, Order::OrderType::FOK});

    REQUIRE(trades.size() == 2);
    REQUIRE(trades[0].buy_order_id == 3);
    REQUIRE(trades[0].sell_order_id == 1);
    REQUIRE(trades[0].quantity == 30);   // fully fill order 1
    REQUIRE(trades[1].buy_order_id == 3);
    REQUIRE(trades[1].sell_order_id == 2);
    REQUIRE(trades[1].quantity == 20);   // partial fill order 2

    // Order 1 fully consumed, order 2 has 10 remaining.
    REQUIRE(book.totalOrderCount() == 1);
    REQUIRE(book.bestAsk().has_value());
    REQUIRE(book.bestAsk().value() == 100.00);
}

// -----------------------------------------------------------------------
// 5. FOK: INSUFFICIENT liquidity → ZERO trades, book state UNCHANGED.
//    This is the most critical test — verifies "look before you leap".
// -----------------------------------------------------------------------
TEST(fok_insufficient_liquidity_no_mutation) {
    OrderBook book;

    // Set up a realistic book state.
    book.addOrder(Order{1, Order::Side::BUY,  98.00, 40});
    book.addOrder(Order{2, Order::Side::BUY,  99.00, 25});
    book.addOrder(Order{3, Order::Side::SELL, 101.00, 30});
    book.addOrder(Order{4, Order::Side::SELL, 102.00, 30});

    // Capture book state BEFORE the FOK submission.
    auto bid_before   = book.bestBid();
    auto ask_before   = book.bestAsk();
    size_t count_before = book.totalOrderCount();
    auto buy_depth_before  = book.getDepth(Order::Side::BUY, 10);
    auto sell_depth_before = book.getDepth(Order::Side::SELL, 10);

    // FOK buy for 100 at 102.00 — only 60 available (30 + 30), need 100.
    auto trades = book.submitOrder(
        Order{5, Order::Side::BUY, 102.00, 100, Order::OrderType::FOK});

    // ZERO trades — order was killed.
    REQUIRE(trades.empty());

    // Book state must be IDENTICAL to before — the critical invariant.
    REQUIRE(book.bestBid().has_value() == bid_before.has_value());
    REQUIRE(std::fabs(book.bestBid().value() - bid_before.value()) < 1e-9);
    REQUIRE(book.bestAsk().has_value() == ask_before.has_value());
    REQUIRE(std::fabs(book.bestAsk().value() - ask_before.value()) < 1e-9);
    REQUIRE(book.totalOrderCount() == count_before);

    // Verify full depth is unchanged — no partial fills, no removed levels.
    auto buy_depth_after  = book.getDepth(Order::Side::BUY, 10);
    auto sell_depth_after = book.getDepth(Order::Side::SELL, 10);

    REQUIRE(buy_depth_after.size() == buy_depth_before.size());
    for (size_t i = 0; i < buy_depth_after.size(); ++i) {
        REQUIRE(std::fabs(buy_depth_after[i].price - buy_depth_before[i].price) < 1e-9);
        REQUIRE(buy_depth_after[i].aggregate_quantity == buy_depth_before[i].aggregate_quantity);
        REQUIRE(buy_depth_after[i].order_count == buy_depth_before[i].order_count);
    }

    REQUIRE(sell_depth_after.size() == sell_depth_before.size());
    for (size_t i = 0; i < sell_depth_after.size(); ++i) {
        REQUIRE(std::fabs(sell_depth_after[i].price - sell_depth_before[i].price) < 1e-9);
        REQUIRE(sell_depth_after[i].aggregate_quantity == sell_depth_before[i].aggregate_quantity);
        REQUIRE(sell_depth_after[i].order_count == sell_depth_before[i].order_count);
    }
}

// -----------------------------------------------------------------------
// 6. FOK: liquidity across MULTIPLE price levels sums to enough → fills.
// -----------------------------------------------------------------------
TEST(fok_multi_level_fill) {
    OrderBook book;

    book.addOrder(Order{1, Order::Side::SELL, 100.00, 20});
    book.addOrder(Order{2, Order::Side::SELL, 101.00, 20});
    book.addOrder(Order{3, Order::Side::SELL, 102.00, 20});

    // FOK buy for 50 at 102.00 — needs to walk 3 levels (20+20+10).
    auto trades = book.submitOrder(
        Order{4, Order::Side::BUY, 102.00, 50, Order::OrderType::FOK});

    REQUIRE(trades.size() == 3);
    REQUIRE(trades[0].sell_order_id == 1);
    REQUIRE(trades[0].quantity == 20);
    REQUIRE(std::fabs(trades[0].price - 100.00) < 1e-9);
    REQUIRE(trades[1].sell_order_id == 2);
    REQUIRE(trades[1].quantity == 20);
    REQUIRE(std::fabs(trades[1].price - 101.00) < 1e-9);
    REQUIRE(trades[2].sell_order_id == 3);
    REQUIRE(trades[2].quantity == 10);
    REQUIRE(std::fabs(trades[2].price - 102.00) < 1e-9);

    // Order 3 has 10 remaining.
    REQUIRE(book.totalOrderCount() == 1);
    REQUIRE(book.bestAsk().has_value());
    REQUIRE(std::fabs(book.bestAsk().value() - 102.00) < 1e-9);
}

// -----------------------------------------------------------------------
// 7. FOK: EXACTLY enough liquidity at boundary → fills completely.
// -----------------------------------------------------------------------
TEST(fok_exact_boundary_fill) {
    OrderBook book;

    book.addOrder(Order{1, Order::Side::SELL, 100.00, 25});
    book.addOrder(Order{2, Order::Side::SELL, 100.00, 25});

    // FOK buy for exactly 50 at 100.00 — exactly 50 available.
    auto trades = book.submitOrder(
        Order{3, Order::Side::BUY, 100.00, 50, Order::OrderType::FOK});

    REQUIRE(trades.size() == 2);
    REQUIRE(trades[0].quantity == 25);
    REQUIRE(trades[1].quantity == 25);

    // Both fully consumed, no phantom resting order.
    REQUIRE(book.totalOrderCount() == 0);
    REQUIRE(!book.bestBid().has_value());
    REQUIRE(!book.bestAsk().has_value());
}

// -----------------------------------------------------------------------
// 8. FOK sell side — verify it works symmetrically for sells too.
// -----------------------------------------------------------------------
TEST(fok_sell_side) {
    OrderBook book;

    book.addOrder(Order{1, Order::Side::BUY, 100.00, 30});
    book.addOrder(Order{2, Order::Side::BUY,  99.00, 30});

    // FOK sell for 50 at 99.00 — 60 available at eligible prices.
    auto trades = book.submitOrder(
        Order{3, Order::Side::SELL, 99.00, 50, Order::OrderType::FOK});

    REQUIRE(trades.size() == 2);
    REQUIRE(trades[0].buy_order_id == 1);   // best bid first
    REQUIRE(trades[0].quantity == 30);
    REQUIRE(trades[1].buy_order_id == 2);
    REQUIRE(trades[1].quantity == 20);

    // Order 2 has 10 remaining.
    REQUIRE(book.totalOrderCount() == 1);
    REQUIRE(book.bestBid().has_value());
    REQUIRE(std::fabs(book.bestBid().value() - 99.00) < 1e-9);
}

// =======================================================================
//  REGRESSION TESTS — existing order types still work
// =======================================================================

// -----------------------------------------------------------------------
// 9. Regression: LIMIT order behavior is unaffected.
// -----------------------------------------------------------------------
TEST(regression_limit_order_unchanged) {
    OrderBook book;

    // LIMIT sell rests if no crossing bid.
    auto trades1 = book.submitOrder(
        Order{1, Order::Side::SELL, 100.00, 30, Order::OrderType::LIMIT});
    REQUIRE(trades1.empty());
    REQUIRE(book.totalOrderCount() == 1);
    REQUIRE(book.bestAsk().value() == 100.00);

    // LIMIT buy crosses the resting sell.
    auto trades2 = book.submitOrder(
        Order{2, Order::Side::BUY, 100.00, 20, Order::OrderType::LIMIT});
    REQUIRE(trades2.size() == 1);
    REQUIRE(trades2[0].quantity == 20);

    // Unfilled LIMIT buy remainder RESTS (the key behavioral difference).
    // Wait — the buy was for 20 and matched 20. Let me do a partial case.
    auto trades3 = book.submitOrder(
        Order{3, Order::Side::BUY, 100.00, 20, Order::OrderType::LIMIT});
    // Sell has 10 remaining → fills 10, buy has 10 remainder that RESTS.
    REQUIRE(trades3.size() == 1);
    REQUIRE(trades3[0].quantity == 10);
    REQUIRE(book.totalOrderCount() == 1);    // the resting buy remainder
    REQUIRE(book.bestBid().has_value());
    REQUIRE(book.bestBid().value() == 100.00);
}

// -----------------------------------------------------------------------
// 10. Regression: MARKET order behavior is unaffected.
// -----------------------------------------------------------------------
TEST(regression_market_order_unchanged) {
    OrderBook book;

    book.addOrder(Order{1, Order::Side::SELL, 100.00, 10});
    book.addOrder(Order{2, Order::Side::SELL, 200.00, 10});

    // MARKET buy: no price constraint, matches both levels.
    auto trades = book.submitOrder(
        Order{3, Order::Side::BUY, 0.0, 30, Order::OrderType::MARKET});

    REQUIRE(trades.size() == 2);
    REQUIRE(trades[0].quantity == 10);
    REQUIRE(std::fabs(trades[0].price - 100.00) < 1e-9);
    REQUIRE(trades[1].quantity == 10);
    REQUIRE(std::fabs(trades[1].price - 200.00) < 1e-9);

    // Remainder of 10 is discarded — MARKET never rests.
    REQUIRE(book.totalOrderCount() == 0);
    REQUIRE(!book.bestBid().has_value());
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
