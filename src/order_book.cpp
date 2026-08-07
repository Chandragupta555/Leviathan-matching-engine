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
// Design: both LIMIT and MARKET orders share the same matching loop body.
// The ONLY difference is the price-crossing check:
//   - LIMIT BUY:  best ask <= incoming price   (price constraint)
//   - LIMIT SELL: best bid >= incoming price   (price constraint)
//   - MARKET:     always true — accepts whatever price is available
// This avoids duplicating the fill logic for the two order types.
//
// CRITICAL: A MARKET order (OrderType::MARKET) must NEVER rest in the book.
// If a MARKET order is not fully filled because the opposite side is
// exhausted, the unfilled remainder is simply discarded — addOrder() is
// NOT called for it.  This is enforced at the bottom of the function.
//
// Termination guarantee: the while-loop advances on every iteration because
// each iteration either:
//   (a) reduces incoming.quantity toward 0  (fill_qty >= 1), or
//   (b) breaks out because no price level crosses anymore (LIMIT only),
//       or the opposite side is empty.
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

    const bool is_market = (incoming.type == Order::OrderType::MARKET);

    std::vector<Trade> trades;

    if (incoming.side == Order::Side::BUY) {
        // Match against ASK side.
        // LIMIT: while best ask <= incoming price.
        // MARKET: while asks exist (no price constraint).
        while (incoming.quantity > 0 && !asks_.empty()) {
            auto level_it = asks_.begin();          // best (lowest) ask

            // Price-crossing check — skipped entirely for MARKET orders.
            if (!is_market && level_it->first > incoming.price) {
                break;  // no more crossable price levels (LIMIT only)
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
        // incoming is SELL — match against BID side.
        // LIMIT: while best bid >= incoming price.
        // MARKET: while bids exist (no price constraint).
        while (incoming.quantity > 0 && !bids_.empty()) {
            auto level_it = bids_.begin();          // best (highest) bid

            // Price-crossing check — skipped entirely for MARKET orders.
            if (!is_market && level_it->first < incoming.price) {
                break;  // no more crossable price levels (LIMIT only)
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

    // Rest any unfilled remainder via the existing addOrder() method —
    // but ONLY for LIMIT orders.
    //
    // CRITICAL: MARKET orders NEVER rest.  If a MARKET order has unfilled
    // quantity here, it means the opposite side ran out of liquidity.
    // The remainder is simply discarded (we do nothing with it).
    // This is the single most important behavioral difference between
    // LIMIT and MARKET orders.
    if (incoming.quantity > 0 && !is_market) {
        addOrder(incoming);
    }
    // If is_market && incoming.quantity > 0, the remainder is intentionally
    // discarded here — MARKET order leftover does NOT rest in the book.

    return trades;
}

// ---------------------------------------------------------------------------
// modifyOrder
//
// Modifies an existing resting order's price and/or quantity.
//
// Three cases:
//   CASE 1: Decrease quantity only, same price → in-place update (preserves
//           time priority).
//   CASE 2: Price change, or quantity increase → cancel + resubmit (loses
//           time priority, may trigger new matches).
//   CASE 3: new_quantity == 0 → implicit full cancellation.
//
// Returns std::nullopt if order_id doesn't exist; otherwise returns a
// (possibly empty) vector of Trade.
// ---------------------------------------------------------------------------
std::optional<std::vector<Trade>> OrderBook::modifyOrder(uint64_t order_id,
                                                          double new_price,
                                                          uint64_t new_quantity) {
    // Look up the order to find its current side and price level.
    auto idx_it = order_index_.find(order_id);
    if (idx_it == order_index_.end()) {
        return std::nullopt;  // order doesn't exist — clean failure
    }

    const auto side  = idx_it->second.side;
    const auto price = idx_it->second.price;

    // ------------------------------------------------------------------
    // CASE 3: new_quantity == 0 → implicit full cancellation.
    // Reducing quantity to zero is semantically identical to removing the
    // order entirely.  We reuse cancelOrder() and return an empty trades
    // vector (no matching can occur from a cancellation).
    // ------------------------------------------------------------------
    if (new_quantity == 0) {
        bool cancelled = cancelOrder(order_id);
        if (cancelled) {
            return std::vector<Trade>{};  // successfully removed
        }
        return std::nullopt;  // shouldn't happen since we found it above, but safe
    }

    // Find the actual order in its deque to read its current quantity.
    // We need this to distinguish CASE 1 (decrease-only) from CASE 2.
    auto find_order_in_deque = [&](auto& side_map) -> Order* {
        auto level_it = side_map.find(price);
        if (level_it == side_map.end()) return nullptr;
        auto& q = level_it->second;
        for (auto& order : q) {
            if (order.id == order_id) {
                return &order;
            }
        }
        return nullptr;
    };

    Order* order_ptr = nullptr;
    if (side == Order::Side::BUY) {
        order_ptr = find_order_in_deque(bids_);
    } else {
        order_ptr = find_order_in_deque(asks_);
    }

    if (order_ptr == nullptr) {
        return std::nullopt;  // shouldn't happen if index is consistent
    }

    const uint64_t current_quantity = order_ptr->quantity;

    // ------------------------------------------------------------------
    // CASE 1: Decrease-only, same price.
    // If new_price equals the current price AND new_quantity <= current
    // quantity, update the quantity IN PLACE (mutate directly via
    // reference).  The order does NOT change position in the deque, so
    // time priority is preserved.  No new match can occur from a quantity
    // decrease at an unchanged price since the order was already resting
    // and not crossing anything.
    // ------------------------------------------------------------------
    if (new_price == price && new_quantity <= current_quantity) {
        order_ptr->quantity = new_quantity;
        return std::vector<Trade>{};  // no matches triggered
    }

    // ------------------------------------------------------------------
    // CASE 2: Price change, OR quantity increase, OR both.
    // This loses time priority.  Cancel the existing order (reuses
    // cancelOrder() — do not duplicate its logic), then construct a fresh
    // Order with the new price/quantity and a NEW timestamp, and submit it
    // via submitOrder() (reuses matching logic — do not duplicate).
    // ------------------------------------------------------------------
    cancelOrder(order_id);

    // Construct a fresh order with a new timestamp (Order constructor
    // auto-assigns from the monotonic counter).  The order type is always
    // LIMIT for resting orders being modified (MARKET orders never rest
    // and therefore can never be modified).
    Order new_order{order_id, side, new_price, new_quantity, Order::OrderType::LIMIT};

    return submitOrder(new_order);
}
