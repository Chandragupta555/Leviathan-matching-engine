#pragma once

#include "order.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>

// ---------------------------------------------------------------------------
// OrderBook — stores resting (unmatched) orders.
//
// Internal layout
// ---------------
//   Buy  side: std::map<double, std::deque<Order>, std::greater<double>>
//              → prices sorted DESCENDING (best/highest bid first)
//   Sell side: std::map<double, std::deque<Order>, std::less<double>>
//              → prices sorted ASCENDING  (best/lowest ask first)
//
// Within each price level, orders sit in a std::deque in arrival order
// (earliest timestamp at front).
//
// Why std::deque over std::vector?
//   Cancel can remove an element from the middle of a price level.
//   std::deque gives O(1) front pop (useful for future matching) and is
//   friendlier than std::vector for mid-container erases on moderate-size
//   queues because it doesn't relocate the entire backing array — it only
//   shifts within the affected block.  std::list would give O(1) erase via
//   iterator but has poor cache locality; deque is a good middle ground for
//   a level that typically holds tens-to-hundreds of orders.
//
// Auxiliary index
// ---------------
//   order_index_:  unordered_map<order_id → OrderLocation>
//   Lets cancelOrder() jump straight to the order in O(1) expected time
//   instead of scanning every level.
//
// Complexity (N = total orders, L = orders at a given price level)
//   addOrder   : O(log P)        — P = number of distinct price levels
//                                    (map insert / find)
//                + O(1)          — deque push_back & hash-map insert
//   cancelOrder: O(1) expected   — hash-map lookup
//                + O(L)          — linear scan within that one deque to
//                                    erase the specific order (L is
//                                    typically small)
//                + O(log P)      — map erase if level becomes empty
// ---------------------------------------------------------------------------

class OrderBook {
public:
    void                addOrder(const Order& order);
    bool                cancelOrder(uint64_t order_id);

    std::optional<double> bestBid() const;
    std::optional<double> bestAsk() const;

    size_t              totalOrderCount() const;

private:
    // Price-level maps (the comparator controls sort direction).
    using BidMap = std::map<double, std::deque<Order>, std::greater<double>>;
    using AskMap = std::map<double, std::deque<Order>, std::less<double>>;

    BidMap bids_;
    AskMap asks_;

    // Fast lookup: order_id  →  {side, price} so cancelOrder can jump
    // straight to the right map and the right deque.
    struct OrderLocation {
        Order::Side side;
        double      price;
    };
    std::unordered_map<uint64_t, OrderLocation> order_index_;
};
