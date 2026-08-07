#include "replay.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

// ---------------------------------------------------------------------------
// Internal: parse one CSV line and replay the operation on `book`, collecting
// trades as appropriate.
//
// If any extraction fails (iss.fail()), the line is malformed — we emit a
// warning to std::cerr, skip the line entirely (do NOT construct an Order
// or call any OrderBook method with corrupted data), and continue.
// ---------------------------------------------------------------------------
static void parseLine(const std::string& line, OrderBook& book,
                      std::vector<Trade>& replayed_trades,
                      std::vector<Trade>& logged_trades)
{
    if (line.empty()) return;

    std::istringstream iss(line);
    std::string tag;
    std::getline(iss, tag, ',');

    char comma;

    if (tag == "ADD") {
        uint64_t seq, order_id, quantity;
        int side_int, type_int;
        double price;

        iss >> seq >> comma >> order_id >> comma >> side_int >> comma
            >> price >> comma >> quantity >> comma >> type_int;

        if (iss.fail()) {
            std::cerr << "[WARN] Malformed ADD line, skipping: " << line << "\n";
            return;
        }

        Order order{order_id, static_cast<Order::Side>(side_int), price,
                    quantity, static_cast<Order::OrderType>(type_int)};
        book.addOrder(order);

    } else if (tag == "CANCEL") {
        uint64_t seq, order_id;

        iss >> seq >> comma >> order_id;

        if (iss.fail()) {
            std::cerr << "[WARN] Malformed CANCEL line, skipping: " << line << "\n";
            return;
        }

        // Replay the cancel.  We don't need to compare the result — the
        // important correctness check is whether the FINAL book state and
        // trade sequence match, not whether each intermediate cancel
        // returned the same bool (which it will, given identical sequence).
        book.cancelOrder(order_id);

    } else if (tag == "SUBMIT") {
        uint64_t seq, order_id, quantity;
        int side_int, type_int;
        double price;

        iss >> seq >> comma >> order_id >> comma >> side_int >> comma
            >> price >> comma >> quantity >> comma >> type_int;

        if (iss.fail()) {
            std::cerr << "[WARN] Malformed SUBMIT line, skipping: " << line << "\n";
            return;
        }

        Order order{order_id, static_cast<Order::Side>(side_int), price,
                    quantity, static_cast<Order::OrderType>(type_int)};
        auto trades = book.submitOrder(order);

        replayed_trades.insert(replayed_trades.end(),
                               trades.begin(), trades.end());

    } else if (tag == "MODIFY") {
        uint64_t seq, order_id, new_quantity;
        double new_price;

        iss >> seq >> comma >> order_id >> comma >> new_price >> comma
            >> new_quantity;

        if (iss.fail()) {
            std::cerr << "[WARN] Malformed MODIFY line, skipping: " << line << "\n";
            return;
        }

        auto result = book.modifyOrder(order_id, new_price, new_quantity);
        if (result.has_value()) {
            replayed_trades.insert(replayed_trades.end(),
                                   result->begin(), result->end());
        }

    } else if (tag == "TRADE") {
        uint64_t seq, buy_id, sell_id, quantity;
        double price;

        iss >> seq >> comma >> buy_id >> comma >> sell_id >> comma
            >> price >> comma >> quantity;

        if (iss.fail()) {
            std::cerr << "[WARN] Malformed TRADE line, skipping: " << line << "\n";
            return;
        }

        // Store with timestamp=0 since we only compare non-timestamp fields.
        logged_trades.push_back(Trade{buy_id, sell_id, price, quantity, 0});
    }
    // Unknown tags are silently ignored (forward-compatibility).
}

// ---------------------------------------------------------------------------
// Internal: compare replayed trades against logged trades.
// ---------------------------------------------------------------------------
static void compareTrades(ReplayResult& result) {
    const auto& replayed = result.replayed_trades;
    const auto& logged   = result.logged_trades;
    size_t max_count = std::max(replayed.size(), logged.size());

    std::ostringstream report;

    for (size_t i = 0; i < max_count; ++i) {
        if (i >= replayed.size()) {
            report << "Trade #" << i
                   << ": MISSING in replay (logged: buy=" << logged[i].buy_order_id
                   << " sell=" << logged[i].sell_order_id
                   << " price=" << logged[i].price
                   << " qty=" << logged[i].quantity << ")\n";
            result.mismatches++;
        } else if (i >= logged.size()) {
            report << "Trade #" << i
                   << ": EXTRA in replay (replay: buy=" << replayed[i].buy_order_id
                   << " sell=" << replayed[i].sell_order_id
                   << " price=" << replayed[i].price
                   << " qty=" << replayed[i].quantity << ")\n";
            result.mismatches++;
        } else {
            const auto& r = replayed[i];
            const auto& l = logged[i];
            // Compare everything EXCEPT timestamp — see event_log.hpp rationale.
            if (r.buy_order_id != l.buy_order_id ||
                r.sell_order_id != l.sell_order_id ||
                std::fabs(r.price - l.price) > 1e-9 ||
                r.quantity != l.quantity)
            {
                report << "Trade #" << i << ": MISMATCH\n"
                       << "  Logged:   buy=" << l.buy_order_id
                       << " sell=" << l.sell_order_id
                       << " price=" << l.price
                       << " qty=" << l.quantity << "\n"
                       << "  Replayed: buy=" << r.buy_order_id
                       << " sell=" << r.sell_order_id
                       << " price=" << r.price
                       << " qty=" << r.quantity << "\n";
                result.mismatches++;
            }
        }
    }

    result.mismatch_report = report.str();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

ReplayResult replayFromStream(std::istream& input) {
    ReplayResult result;
    std::string line;

    while (std::getline(input, line)) {
        // Strip trailing \r if present (Windows CRLF).
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        parseLine(line, result.book, result.replayed_trades, result.logged_trades);
    }

    compareTrades(result);
    return result;
}

ReplayResult replayFromFile(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open log file: " + filepath);
    }
    return replayFromStream(file);
}
