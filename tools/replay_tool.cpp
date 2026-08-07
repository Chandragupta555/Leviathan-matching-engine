// ---------------------------------------------------------------------------
// replay_tool — standalone CLI executable for replaying event logs.
//
// Usage:  replay_tool <log_file_path>
//
// Reads a CSV event log produced by RecordingOrderBook, replays every
// ADD/CANCEL/MODIFY/SUBMIT on a fresh OrderBook, verifies that replayed
// trades match logged TRADE lines (ignoring timestamps — see event_log.hpp),
// and prints final book state including a market depth view.
// ---------------------------------------------------------------------------

#include "replay.hpp"
#include "order_book.hpp"

#include <iomanip>
#include <iostream>
#include <string>

static void printDepthSide(const OrderBook& book, Order::Side side,
                           const char* label, size_t max_levels)
{
    auto levels = book.getDepth(side, max_levels);
    std::cout << "  " << label << " (" << levels.size() << " levels):\n";

    if (levels.empty()) {
        std::cout << "    (empty)\n";
        return;
    }

    std::cout << "    " << std::left << std::setw(16) << "Price"
              << std::setw(16) << "Agg Quantity"
              << std::setw(12) << "Orders" << "\n";
    std::cout << "    " << std::string(44, '-') << "\n";

    for (const auto& lvl : levels) {
        std::cout << "    " << std::left
                  << std::setw(16) << std::fixed << std::setprecision(6) << lvl.price
                  << std::setw(16) << lvl.aggregate_quantity
                  << std::setw(12) << lvl.order_count << "\n";
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: replay_tool <log_file_path>\n";
        return 1;
    }

    ReplayResult result;
    try {
        result = replayFromFile(argv[1]);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    std::cout << std::fixed << std::setprecision(6);

    // --- Trade verification ---
    std::cout << "=== TRADE VERIFICATION ===\n";
    std::cout << "Trades replayed: " << result.replayed_trades.size() << "\n";
    std::cout << "Trades logged:   " << result.logged_trades.size() << "\n";

    if (result.mismatches == 0) {
        std::cout << "Result: ALL " << result.replayed_trades.size()
                  << " trades MATCHED\n";
    } else {
        std::cout << "Result: " << result.mismatches << " MISMATCHES\n";
        std::cout << result.mismatch_report;
    }

    // --- Final book state ---
    std::cout << "\n=== FINAL BOOK STATE ===\n";
    auto bid = result.book.bestBid();
    auto ask = result.book.bestAsk();
    std::cout << "Best Bid:     "
              << (bid.has_value() ? std::to_string(bid.value()) : "none") << "\n";
    std::cout << "Best Ask:     "
              << (ask.has_value() ? std::to_string(ask.value()) : "none") << "\n";
    std::cout << "Total Orders: " << result.book.totalOrderCount() << "\n";

    // --- Market depth view (top 5 levels each side) ---
    std::cout << "\n=== MARKET DEPTH (top 5 levels) ===\n";
    printDepthSide(result.book, Order::Side::BUY, "BIDS (highest first)", 5);
    std::cout << "\n";
    printDepthSide(result.book, Order::Side::SELL, "ASKS (lowest first)", 5);
    std::cout << "\n";

    return (result.mismatches == 0) ? 0 : 1;
}
