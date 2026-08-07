#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// DESIGN QUESTION FINDING: Order Priority Within a Price Level
//
// After reading order_book.cpp (specifically addOrder at line 10-17 and the
// matching loop in submitOrder which pops from .front()), orders within a
// price-level deque are ordered purely by push_back() call sequence (FIFO).
// The Order.timestamp field is auto-assigned by Order's constructor via a
// global atomic counter (order.hpp line 43-46), but the OrderBook never sorts
// or inspects timestamp values for ordering within a level — it simply pushes
// to the back (addOrder) and pops from the front (matching in submitOrder).
//
// Implication for correct replay: We must replay operations in the exact same
// CALL SEQUENCE as they were originally recorded (which our monotonic sequence
// counter guarantees). We do NOT need to reproduce identical timestamp VALUES
// — the Order constructor will assign new timestamps during replay, and since
// price-level ordering depends only on push_back() sequence (not timestamp
// values), matching behavior will be identical.
//
// However, Trade records include a timestamp field (the taker's timestamp).
// Since replayed Orders get new timestamps from the global counter, replayed
// Trade timestamps will differ from the originals. Therefore, trade comparison
// during replay verification compares buy_order_id, sell_order_id, price,
// and quantity — but NOT timestamp.
// ---------------------------------------------------------------------------

// Event types for the log.
enum class EventType : uint8_t {
    ADD,
    CANCEL,
    MODIFY,
    SUBMIT,
    TRADE
};

// ---------------------------------------------------------------------------
// Log line format — CSV, one line per event, UTF-8 text.
// Each line begins with an event-type tag followed by a monotonic sequence
// number that establishes unambiguous global ordering across event types.
//
//   ADD,<seq>,<order_id>,<side>,<price>,<quantity>,<order_type>
//     side: 0=BUY, 1=SELL
//     order_type: 0=LIMIT, 1=MARKET
//
//   CANCEL,<seq>,<order_id>
//     Logged BEFORE the call.  The cancel result (success/failure) is NOT
//     recorded — replay re-executes the cancel and the result is implied
//     by the book state at that point in the sequence.
//
//   MODIFY,<seq>,<order_id>,<new_price>,<new_quantity>
//     Logged BEFORE the call.  Resulting trades (if any) follow as TRADE lines.
//
//   SUBMIT,<seq>,<order_id>,<side>,<price>,<quantity>,<order_type>
//     Logged BEFORE the call.  Resulting trades (if any) follow as TRADE lines.
//
//   TRADE,<seq>,<buy_order_id>,<sell_order_id>,<price>,<quantity>
//     Logged AFTER the submit/modify call that produced it.
//     Multiple TRADE lines may follow a single SUBMIT or MODIFY.
//
// The sequence counter is separate from Order's own timestamp counter, lives
// inside RecordingOrderBook, and increments for every logged line (including
// each individual TRADE line).
// ---------------------------------------------------------------------------
