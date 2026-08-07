#include "recording_order_book.hpp"

#include <iomanip>

RecordingOrderBook::RecordingOrderBook(std::ostream& log_stream)
    : log_(log_stream)
{
    // Use fixed-precision for all double output so replayed values parse
    // back to the same floating-point representation.
    log_ << std::fixed << std::setprecision(6);
}

void RecordingOrderBook::addOrder(const Order& order) {
    log_ << "ADD," << nextSeq() << ","
         << order.id << ","
         << static_cast<int>(order.side) << ","
         << order.price << ","
         << order.quantity << ","
         << static_cast<int>(order.type) << "\n";
    log_.flush();

    book_.addOrder(order);
}

bool RecordingOrderBook::cancelOrder(uint64_t order_id) {
    // Log BEFORE delegation — matches the pattern of all other methods.
    log_ << "CANCEL," << nextSeq() << ","
         << order_id << "\n";
    log_.flush();

    return book_.cancelOrder(order_id);
}

std::vector<Trade> RecordingOrderBook::submitOrder(Order order) {
    // Log the incoming request BEFORE delegation.
    log_ << "SUBMIT," << nextSeq() << ","
         << order.id << ","
         << static_cast<int>(order.side) << ","
         << order.price << ","
         << order.quantity << ","
         << static_cast<int>(order.type) << "\n";
    log_.flush();

    // Delegate to the real OrderBook.
    auto trades = book_.submitOrder(order);

    // Log every resulting trade AFTER delegation.
    logTrades(trades);

    return trades;
}

std::optional<std::vector<Trade>> RecordingOrderBook::modifyOrder(
    uint64_t order_id, double new_price, uint64_t new_quantity)
{
    // Log the modify request BEFORE delegation.
    log_ << "MODIFY," << nextSeq() << ","
         << order_id << ","
         << new_price << ","
         << new_quantity << "\n";
    log_.flush();

    // Delegate to the real OrderBook.
    auto result = book_.modifyOrder(order_id, new_price, new_quantity);

    // Log resulting trades (if modification succeeded and produced fills).
    if (result.has_value()) {
        logTrades(result.value());
    }

    return result;
}

std::optional<double> RecordingOrderBook::bestBid() const {
    return book_.bestBid();
}

std::optional<double> RecordingOrderBook::bestAsk() const {
    return book_.bestAsk();
}

size_t RecordingOrderBook::totalOrderCount() const {
    return book_.totalOrderCount();
}

void RecordingOrderBook::logTrades(const std::vector<Trade>& trades) {
    for (const auto& t : trades) {
        log_ << "TRADE," << nextSeq() << ","
             << t.buy_order_id << ","
             << t.sell_order_id << ","
             << t.price << ","
             << t.quantity << "\n";
    }
    if (!trades.empty()) {
        log_.flush();
    }
}
