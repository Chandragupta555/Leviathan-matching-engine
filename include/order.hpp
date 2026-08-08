#pragma once

#include <cstdint>
#include <atomic>

// Design choice: public fields instead of getters.
// Order is a plain data carrier (value type) — it holds no invariants that
// private fields + getters would protect. Keeping fields public avoids
// boilerplate accessors and mirrors the "struct-of-data" idiom common in
// high-performance order books. If invariants arise later (e.g. quantity
// must never be zero), we can tighten access then.

struct Order {
    enum class Side : uint8_t { BUY, SELL };
    enum class OrderType : uint8_t { LIMIT, MARKET, IOC, FOK };

    uint64_t    id;
    Side        side;
    double      price;
    uint64_t    quantity;   // remaining quantity to be filled
    uint64_t    timestamp;  // monotonically increasing logical clock, NOT wall time
    OrderType   type;       // LIMIT (default) or MARKET

    // Construct an order and auto-assign a timestamp from a process-wide
    // monotonic counter.  Using an inline static avoids a separate .cpp
    // just for this counter; std::atomic makes it safe if we ever go
    // multi-threaded (no cost in single-threaded builds on x86).
    //
    // The `type` parameter defaults to LIMIT so that all existing 4-argument
    // call sites (e.g. Order{1, Side::BUY, 100.0, 10}) continue to compile
    // without modification.
    Order(uint64_t id, Side side, double price, uint64_t quantity,
          OrderType type = OrderType::LIMIT)
        : id{id}
        , side{side}
        , price{price}
        , quantity{quantity}
        , timestamp{nextTimestamp()}
        , type{type}
    {}

private:
    static uint64_t nextTimestamp() {
        static std::atomic<uint64_t> counter{0};
        return counter.fetch_add(1, std::memory_order_relaxed);
    }
};
