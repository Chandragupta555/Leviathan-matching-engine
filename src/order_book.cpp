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
// getDepth — EXCEPTION addition (see order_book.hpp comment for justification).
// Returns up to max_levels aggregated price levels for the requested side.
// O(L * max_levels) where L = orders per level (to sum quantities).
// ---------------------------------------------------------------------------
template <typename MapType>
static std::vector<OrderBook::PriceLevel> depthFromMap(const MapType& side_map,
                                                        size_t max_levels)
{
    std::vector<OrderBook::PriceLevel> depth;
    depth.reserve(max_levels);

    size_t count = 0;
    for (auto it = side_map.begin(); it != side_map.end() && count < max_levels;
         ++it, ++count)
    {
        uint64_t agg_qty = 0;
        for (const auto& order : it->second) {
            agg_qty += order.quantity;
        }
        depth.push_back({it->first, agg_qty, it->second.size()});
    }
    return depth;
}

std::vector<OrderBook::PriceLevel> OrderBook::getDepth(Order::Side side,
                                                        size_t max_levels) const
{
    if (side == Order::Side::BUY) {
        return depthFromMap(bids_, max_levels);
    } else {
        return depthFromMap(asks_, max_levels);
    }
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
// canFullyFillFromMap_  (file-scope helper for the FOK preview pass)
//
// PROVABLY READ-ONLY: This function accepts a const reference to the side map
// and performs ZERO mutations.  It does not call removeFrontOrder_, does not
// modify any deque, does not erase any map entry, and does not touch
// order_index_.  It only reads iterator keys (prices) and order quantities
// via const iteration.
//
// Walks the opposite side's price levels (best-first, as determined by the
// map's comparator) and sums available quantity from orders whose price
// satisfies the crossing check.  Returns true as soon as accumulated
// quantity >= needed, or false if all eligible levels are exhausted.
//
// The `crosses` callable encodes the price-crossing rule:
//   BUY incoming:  crosses(ask_price, buy_limit) iff ask_price <= buy_limit
//   SELL incoming: crosses(bid_price, sell_limit) iff bid_price >= sell_limit
// ---------------------------------------------------------------------------
template <typename MapType, typename CrossCheck>
static bool canFullyFillFromMap_(const MapType& side_map, double price,
                                  uint64_t needed, CrossCheck crosses)
{
    uint64_t available = 0;
    for (auto it = side_map.cbegin(); it != side_map.cend(); ++it) {
        if (!crosses(it->first, price)) {
            break;  // no more crossable levels (map is sorted best-first)
        }
        for (const auto& order : it->second) {
            available += order.quantity;
            if (available >= needed) {
                return true;  // early exit — enough confirmed
            }
        }
    }
    return false;
}

bool OrderBook::canFullyFill_(Order::Side incoming_side, double price,
                               uint64_t quantity) const
{
    if (incoming_side == Order::Side::BUY) {
        // Incoming buy matches against asks; ask crosses if ask_price <= buy's limit.
        return canFullyFillFromMap_(asks_, price, quantity,
            [](double ask_price, double limit) { return ask_price <= limit; });
    } else {
        // Incoming sell matches against bids; bid crosses if bid_price >= sell's limit.
        return canFullyFillFromMap_(bids_, price, quantity,
            [](double bid_price, double limit) { return bid_price >= limit; });
    }
}

// ---------------------------------------------------------------------------
// submitOrder
//
// Entry point for incoming orders.  Attempts to match against the opposite
// side of the book, then rests any unfilled remainder via addOrder().
//
// Supported order types and their behavior:
//
//   LIMIT:  Price constraint (best opposite <= incoming price for BUY,
//           >= for SELL).  Unfilled remainder RESTS in the book.
//
//   MARKET: No price constraint (accepts any available price).
//           Unfilled remainder is DISCARDED — never rests.
//
//   IOC:    Price constraint (same as LIMIT).
//           Unfilled remainder is DISCARDED — never rests.
//           Semantics: "match what you can right now, kill the rest."
//
//   FOK:    Price constraint (same as LIMIT).
//           Before ANY matching, a read-only preview confirms full
//           quantity is available at eligible prices.  If not, returns
//           immediately with ZERO trades and ZERO mutations.
//           If preview passes, executes normally (guaranteed full fill).
//           Never rests under any circumstance.
//
// RESTRUCTURING NOTE (IOC/FOK milestone):
// The original code used a single `is_market` boolean for two purposes:
//   1. Skip the price-crossing check  (MARKET only)
//   2. Prevent resting of unfilled remainder  (MARKET only)
// These concerns are now split into two orthogonal flags:
//   - skip_price_check: true for MARKET only (IOC/FOK have price constraints)
//   - never_rest:       true for MARKET, IOC, and FOK (only LIMIT rests)
//
// For existing order types, behavior is byte-for-byte UNCHANGED:
//   LIMIT:  skip_price_check=false, never_rest=false  (was: is_market=false)
//   MARKET: skip_price_check=true,  never_rest=true   (was: is_market=true)
// The matching loop body, trade construction, and fill logic are IDENTICAL.
//
// Termination guarantee: unchanged — the while-loop advances on every
// iteration because fill_qty >= 1 strictly decreases incoming.quantity.
//
// Complexity: unchanged — O(M * log P).
// FOK adds a preview pass of O(M_preview) where M_preview <= total eligible
// resting orders, but this is dominated by the subsequent matching pass.
// ---------------------------------------------------------------------------
std::vector<Trade> OrderBook::submitOrder(Order incoming) {
    // Reject zero-quantity orders immediately.
    if (incoming.quantity == 0) {
        return {};
    }

    const auto type = incoming.type;

    // Price-crossing check is skipped for MARKET orders only (they accept
    // any available price).  LIMIT, IOC, and FOK all enforce price constraints.
    const bool skip_price_check = (type == Order::OrderType::MARKET);

    // Only LIMIT orders rest their unfilled remainder in the book.
    // MARKET, IOC, and FOK all discard unfilled quantity.
    const bool never_rest = (type != Order::OrderType::LIMIT);

    // FOK: "fill or kill" — must confirm full quantity is available at
    // eligible price levels BEFORE executing any matches.  If the preview
    // finds insufficient liquidity, return immediately with zero trades
    // and zero mutations to the book.
    if (type == Order::OrderType::FOK) {
        if (!canFullyFill_(incoming.side, incoming.price, incoming.quantity)) {
            return {};
        }
        // Preview confirmed: enough liquidity exists.  The matching loop
        // below is guaranteed to fully fill this order.
    }

    std::vector<Trade> trades;

    if (incoming.side == Order::Side::BUY) {
        // Match against ASK side.
        // LIMIT/IOC/FOK: while best ask <= incoming price.
        // MARKET: while asks exist (no price constraint).
        while (incoming.quantity > 0 && !asks_.empty()) {
            auto level_it = asks_.begin();          // best (lowest) ask

            // Price-crossing check — skipped entirely for MARKET orders.
            if (!skip_price_check && level_it->first > incoming.price) {
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
        // incoming is SELL — match against BID side.
        // LIMIT/IOC/FOK: while best bid >= incoming price.
        // MARKET: while bids exist (no price constraint).
        while (incoming.quantity > 0 && !bids_.empty()) {
            auto level_it = bids_.begin();          // best (highest) bid

            // Price-crossing check — skipped entirely for MARKET orders.
            if (!skip_price_check && level_it->first < incoming.price) {
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

    // Rest any unfilled remainder — ONLY for LIMIT orders.
    //
    // MARKET, IOC, and FOK orders NEVER rest:
    //   - MARKET: opposite side exhausted, remainder discarded.
    //   - IOC: matched what was available, remainder discarded.
    //   - FOK: preview guaranteed full fill, so remainder is always 0 here.
    //          (But even if it weren't, FOK would still not rest.)
    if (incoming.quantity > 0 && !never_rest) {
        addOrder(incoming);
    }

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
