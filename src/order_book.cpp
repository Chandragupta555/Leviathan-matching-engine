#include "order_book.hpp"

#include <algorithm>

// ---------------------------------------------------------------------------
// addOrder
//   O(log P) for the map lookup / insert  +  O(1) amortised deque push_back
//   + O(1) amortised hash-map insert.
// ---------------------------------------------------------------------------
void OrderBook::addOrder(const Order& order) {
    if (order.side == Order::Side::BUY) {
        bids_[order.price].push_back(order);
    } else {
        asks_[order.price].push_back(order);
    }
    order_index_[order.id] = OrderLocation{order.side, order.price};
}

// ---------------------------------------------------------------------------
// cancelOrder
//   O(1) expected hash-map lookup  →  O(L) scan within the price-level
//   deque  →  O(log P) map erase if the level is now empty.
//
// Edge case: when the cancelled order was the LAST one at its price level,
// the deque becomes empty and we erase the map entry so no stale keys linger.
// ---------------------------------------------------------------------------

// Helper: erase an order from a typed side-map.  Templated so it works for
// both BidMap (greater<double>) and AskMap (less<double>).
template <typename MapType>
static bool eraseFromSide(MapType& side_map, double price, uint64_t order_id) {
    auto level_it = side_map.find(price);
    if (level_it == side_map.end()) return false;

    auto& q = level_it->second;
    auto it = std::find_if(q.begin(), q.end(),
                           [order_id](const Order& o) { return o.id == order_id; });
    if (it == q.end()) return false;

    q.erase(it);

    // Clean up the price level if it's now empty.
    if (q.empty()) {
        side_map.erase(level_it);
    }
    return true;
}

bool OrderBook::cancelOrder(uint64_t order_id) {
    auto idx_it = order_index_.find(order_id);
    if (idx_it == order_index_.end()) return false;

    const auto [side, price] = idx_it->second;
    bool removed = false;

    if (side == Order::Side::BUY) {
        removed = eraseFromSide(bids_, price, order_id);
    } else {
        removed = eraseFromSide(asks_, price, order_id);
    }

    if (removed) {
        order_index_.erase(idx_it);
    }
    return removed;
}

// ---------------------------------------------------------------------------
// bestBid / bestAsk
//   O(1) — std::map::begin() is the "best" element thanks to the comparator.
// ---------------------------------------------------------------------------
std::optional<double> OrderBook::bestBid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;   // greatest price (std::greater comparator)
}

std::optional<double> OrderBook::bestAsk() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;   // smallest price (std::less comparator)
}

// ---------------------------------------------------------------------------
// totalOrderCount — O(1): the index tracks every live order.
// ---------------------------------------------------------------------------
size_t OrderBook::totalOrderCount() const {
    return order_index_.size();
}

// ---------------------------------------------------------------------------
// removeFrontOrder_  (private helper)
//
// Pops the front order from a price-level deque and removes it from
// order_index_.  If the deque becomes empty, erases the map entry.
//
// This mirrors the empty-level cleanup in the file-scope eraseFromSide()
// used by cancelOrder(), but is specialised for the matching path where we
// always consume the front of the deque — so no linear scan is needed.
// ---------------------------------------------------------------------------
template <typename MapType>
void OrderBook::removeFrontOrder_(MapType& side_map,
                                  typename MapType::iterator level_it) {
    auto& q = level_it->second;
    // Remove from the fast-lookup index.
    order_index_.erase(q.front().id);
    q.pop_front();

    // Clean up the price level if it's now empty — same rule as cancelOrder.
    if (q.empty()) {
        side_map.erase(level_it);
    }
}

// ---------------------------------------------------------------------------
// submitOrder
//
// Entry point for incoming orders.  Attempts to match against the opposite
// side of the book, then rests any unfilled remainder via addOrder().
//
// Termination guarantee: the while-loop advances on every iteration because
// each iteration either:
//   (a) reduces incoming.quantity toward 0  (fill_qty >= 1), or
//   (b) breaks out because no price level crosses anymore.
// Since incoming.quantity is finite and strictly decreases, the loop
// terminates.
//
// Complexity: O(M * log P) where M = resting orders matched, P = price
// levels.  Each matched order costs O(1) deque pop + O(1) hash erase +
// amortised O(log P) map erase when a level empties.
// ---------------------------------------------------------------------------
std::vector<Trade> OrderBook::submitOrder(Order incoming) {
    // Reject zero-quantity orders immediately.
    if (incoming.quantity == 0) {
        return {};
    }

    std::vector<Trade> trades;

    if (incoming.side == Order::Side::BUY) {
        // Match against ASK side: while the best ask <= incoming buy price.
        while (incoming.quantity > 0 && !asks_.empty()) {
            auto level_it = asks_.begin();          // best (lowest) ask
            if (level_it->first > incoming.price) {
                break;  // no more crossable price levels
            }

            auto& resting = level_it->second.front();  // earliest order at this level

            const uint64_t fill_qty = std::min(incoming.quantity, resting.quantity);

            // Trade executes at the RESTING (maker) order's price —
            // the taker gets whatever price the maker committed to.
            trades.push_back(Trade{
                incoming.id,        // buy_order_id  (incoming is BUY)
                resting.id,         // sell_order_id  (resting is SELL)
                resting.price,      // execution price = resting order's price
                fill_qty,
                incoming.timestamp  // trade timestamp = taker's arrival time
            });

            incoming.quantity -= fill_qty;
            resting.quantity  -= fill_qty;

            if (resting.quantity == 0) {
                // Resting order fully filled — remove from book & index.
                removeFrontOrder_(asks_, level_it);
            }
        }
    } else {
        // incoming is SELL — match against BID side: while best bid >= incoming sell price.
        while (incoming.quantity > 0 && !bids_.empty()) {
            auto level_it = bids_.begin();          // best (highest) bid
            if (level_it->first < incoming.price) {
                break;  // no more crossable price levels
            }

            auto& resting = level_it->second.front();

            const uint64_t fill_qty = std::min(incoming.quantity, resting.quantity);

            // Trade executes at the RESTING (maker) order's price.
            trades.push_back(Trade{
                resting.id,         // buy_order_id  (resting is BUY)
                incoming.id,        // sell_order_id  (incoming is SELL)
                resting.price,      // execution price = resting order's price
                fill_qty,
                incoming.timestamp
            });

            incoming.quantity -= fill_qty;
            resting.quantity  -= fill_qty;

            if (resting.quantity == 0) {
                removeFrontOrder_(bids_, level_it);
            }
        }
    }

    // Rest any unfilled remainder via the existing addOrder() method.
    if (incoming.quantity > 0) {
        addOrder(incoming);
    }

    return trades;
}
