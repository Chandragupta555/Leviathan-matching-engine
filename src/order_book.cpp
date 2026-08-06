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
