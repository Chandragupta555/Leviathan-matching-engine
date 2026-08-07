#pragma once

#include "order_book.hpp"
#include "event_log.hpp"

#include <cstdint>
#include <ostream>
#include <vector>

// ---------------------------------------------------------------------------
// RecordingOrderBook — composition-based wrapper around OrderBook.
//
// Owns a real OrderBook internally and exposes the same public interface.
// Every mutating operation is logged to a text stream before/after delegation.
//
// Constructor takes std::ostream& rather than a file path.  Justification:
//   - Tests can pass std::ostringstream, avoiding filesystem side effects.
//   - Production code passes std::ofstream for real file logging.
//   - The caller controls stream lifetime, which is the natural C++ RAII model.
//   - RecordingOrderBook stays decoupled from filesystem concerns.
// ---------------------------------------------------------------------------

class RecordingOrderBook {
public:
    explicit RecordingOrderBook(std::ostream& log_stream);

    void                                    addOrder(const Order& order);
    bool                                    cancelOrder(uint64_t order_id);
    std::vector<Trade>                      submitOrder(Order order);
    std::optional<std::vector<Trade>>       modifyOrder(uint64_t order_id,
                                                        double new_price,
                                                        uint64_t new_quantity);

    std::optional<double>                   bestBid() const;
    std::optional<double>                   bestAsk() const;
    size_t                                  totalOrderCount() const;

    // Direct read-only access to the underlying OrderBook for verification.
    const OrderBook& book() const { return book_; }

private:
    OrderBook    book_;
    std::ostream& log_;
    uint64_t     seq_{0};

    uint64_t nextSeq() { return seq_++; }
    void logTrades(const std::vector<Trade>& trades);
};
